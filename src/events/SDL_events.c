/*
  SDLop -- SDL_events.h: the event queue.

  Design notes (this is where the SDLop build differs most from SDL3):

    * the queue is a fixed ring of 512 SDL_Event values preallocated in BSS: no
      malloc, no per-event copy into a queue node, no growth heuristics;
    * the push path takes one mutex and never allocates or makes a syscall -
      the condition variable is only signalled when a thread is actually waiting
      in SDL_WaitEvent();
    * SDL_WaitEvent() blocks in poll() on the platform event fd and on the
      async-input wakeup fd, so raw evdev input wakes it immediately instead of
      after a timeout;
    * event filters and watches are run at push time (SDL3 runs them in Peep
      Events), which keeps the pop path branch-free.
*/

#include "../sdlop_internal.h"

#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <signal.h>

#define SDLOP_MAX_WATCHES 16
#define SDLOP_EVENT_RANGE_BITS 0x10000
#define SDLOP_EVENT_RANGE_BYTES (SDLOP_EVENT_RANGE_BITS / 8)

typedef struct SDLOP_EventWatch
{
    SDL_EventFilter filter;
    void *userdata;
    bool disabled;
} SDLOP_EventWatch;

static SDL_Event sdlop_queue[SDLOP_EVENT_QUEUE_SIZE];
static int sdlop_queue_head;
static int sdlop_queue_tail;
static int sdlop_queue_count;
static pthread_mutex_t sdlop_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sdlop_queue_cond = PTHREAD_COND_INITIALIZER;
static int sdlop_queue_waiters;

static Uint8 sdlop_event_enabled[SDLOP_EVENT_RANGE_BYTES];
static SDL_EventFilter sdlop_event_filter;
static void *sdlop_event_filter_userdata;
static SDLOP_EventWatch sdlop_watches[SDLOP_MAX_WATCHES];
static int sdlop_num_watches;

static Uint32 sdlop_next_user_event = SDL_EVENT_USER;
static Uint32 sdlop_max_user_event = SDL_EVENT_USER;

static int sdlop_wakeup_fd = -1;

/* SDL3 turns SIGINT and SIGTERM into SDL_EVENT_QUIT instead of letting the
   process die on the spot, so an application gets to shut down cleanly - Ctrl+C
   in a windowed program is a request, not a kill. A signal handler may only
   touch async-signal-safe things, hence the flag plus the wakeup eventfd: a
   thread blocked in SDL_WaitEvent() is woken by it and finds the event. */
static volatile sig_atomic_t sdlop_quit_signalled;

static void sdlop_signal_handler(int sig)
{
    (void)sig;
    sdlop_quit_signalled = 1;
    SDLOP_SignalWakeup();          /* async-signal-safe: a write to the wakeup fd */
}

/* Only a signal that nobody is handling is taken over: an application that
   installed its own handler keeps it (and gets the signal), which is what SDL3
   does and what an application embedding the library would expect. */
static void sdlop_install_signal(int sig)
{
    struct sigaction action;

    if (sigaction(sig, NULL, &action) != 0 || action.sa_handler != SIG_DFL) {
        return;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = sdlop_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;           /* poll() must be interrupted, not restarted */
    sigaction(sig, &action, NULL);
}

static void sdlop_uninstall_signal(int sig)
{
    struct sigaction action;

    if (sigaction(sig, NULL, &action) != 0 || action.sa_handler != sdlop_signal_handler) {
        return;                    /* somebody replaced it: leave theirs alone */
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, NULL);
}

void SDLOP_InstallSignalHandlers(void)
{
    if (SDL_GetHintBoolean(SDL_HINT_NO_SIGNAL_HANDLERS, false)) {
        return;                    /* the application wants the default behaviour */
    }
    sdlop_install_signal(SIGINT);
    sdlop_install_signal(SIGTERM);
}

void SDLOP_QuitSignalHandlers(void)
{
    sdlop_uninstall_signal(SIGINT);
    sdlop_uninstall_signal(SIGTERM);
}

void SDLOP_QueueSignalQuit(void)
{
    SDL_Event event;

    if (!sdlop_quit_signalled) {
        return;
    }
    sdlop_quit_signalled = 0;
    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_QUIT;
    event.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&event);
}

/* ------------------------------------------------------------------------- */
/* Wakeup descriptor                                                         */
/* ------------------------------------------------------------------------- */

bool SDLOP_InitWakeup(void)
{
    if (sdlop_wakeup_fd < 0) {
        sdlop_wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (sdlop_wakeup_fd < 0) {
            return false;
        }
    }
    return true;
}

void SDLOP_QuitWakeup(void)
{
    if (sdlop_wakeup_fd >= 0) {
        close(sdlop_wakeup_fd);
        sdlop_wakeup_fd = -1;
    }
}

int SDLOP_GetWakeupFD(void)
{
    return sdlop_wakeup_fd;
}

void SDLOP_SignalWakeup(void)
{
    Uint64 one = 1;
    if (sdlop_wakeup_fd >= 0) {
        ssize_t rc = write(sdlop_wakeup_fd, &one, sizeof(one));
        (void)rc;
    }
}

static void sdlop_drain_wakeup(void)
{
    Uint64 value;
    if (sdlop_wakeup_fd >= 0) {
        while (read(sdlop_wakeup_fd, &value, sizeof(value)) == sizeof(value)) {
            /* drain */
        }
    }
}

/* Block until the platform, the async input thread or a timer has something for
   us. This is what makes SDL_WaitEvent() cheap *and* low latency: we are woken
   by the kernel the moment input arrives. */
bool SDLOP_WaitPlatformEvents(Sint64 timeoutNS)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();
    struct pollfd fds[2];
    int nfds = 0;
    int platform_fd = (driver && driver->get_event_fd) ? driver->get_event_fd() : -1;
    int wakeup_fd = sdlop_wakeup_fd;
    Sint64 timeout_ms;
    bool wait_needed;

    if (platform_fd >= 0) {
        fds[nfds].fd = platform_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }
    if (wakeup_fd >= 0) {
        fds[nfds].fd = wakeup_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }

    /* Announce that a thread is about to sleep. Producers only pay for the
       wakeup write() when somebody is actually parked here, which is what makes
       SDL_PushEvent()/SDL_PollEvent() a pure user-space round trip in the
       (overwhelmingly common) case of an application that polls. */
    pthread_mutex_lock(&sdlop_queue_lock);
    sdlop_queue_waiters++;
    wait_needed = (sdlop_queue_count == 0);
    pthread_mutex_unlock(&sdlop_queue_lock);

    if (wait_needed) {
        if (nfds == 0) {
            /* No descriptors to wait on: sleep on the queue condition variable. */
            struct timespec ts;
            struct timespec *tsp = NULL;
            pthread_mutex_lock(&sdlop_queue_lock);
            if (timeoutNS >= 0) {
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += (time_t)(timeoutNS / 1000000000LL);
                ts.tv_nsec += (long)(timeoutNS % 1000000000LL);
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }
                tsp = &ts;
            }
            /* an event may have arrived between the count above and here */
            if (sdlop_queue_count == 0) {
                pthread_cond_timedwait(&sdlop_queue_cond, &sdlop_queue_lock, tsp);
            }
            pthread_mutex_unlock(&sdlop_queue_lock);
        } else {
            if (timeoutNS < 0) {
                timeout_ms = -1;
            } else {
                timeout_ms = (timeoutNS + 999999) / 1000000;   /* round up to ms */
                if (timeout_ms == 0) {
                    timeout_ms = 0;
                }
            }

            if (driver && driver->prepare_read) {
                driver->prepare_read();
            }
            poll(fds, (nfds_t)nfds, (int)timeout_ms);
            if (wakeup_fd >= 0) {
                sdlop_drain_wakeup();
            }
        }
    }

    pthread_mutex_lock(&sdlop_queue_lock);
    sdlop_queue_waiters--;
    pthread_mutex_unlock(&sdlop_queue_lock);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Queue plumbing                                                            */
/* ------------------------------------------------------------------------- */

void SDLOP_InitEvents(void)
{
    memset(sdlop_event_enabled, 0xFF, sizeof(sdlop_event_enabled));   /* all on */
    sdlop_queue_head = sdlop_queue_tail = sdlop_queue_count = 0;
    sdlop_num_watches = 0;
    sdlop_event_filter = NULL;
    sdlop_event_filter_userdata = NULL;
    sdlop_next_user_event = SDL_EVENT_USER;
    sdlop_max_user_event = SDL_EVENT_USER;
    SDLOP_InitWakeup();
    /* Ctrl+C becomes an event, so an application can shut down in an orderly
       way (SDL_HINT_NO_SIGNAL_HANDLERS restores the default death). */
    SDLOP_InstallSignalHandlers();
}

void SDLOP_QuitEvents(void)
{
    SDLOP_QuitSignalHandlers();
    SDLOP_FlushEventsInternal(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    SDLOP_QuitWakeup();
}

void SDLOP_SignalEvents(void)
{
    pthread_mutex_lock(&sdlop_queue_lock);
    if (sdlop_queue_waiters > 0) {
        pthread_cond_broadcast(&sdlop_queue_cond);
    }
    pthread_mutex_unlock(&sdlop_queue_lock);
    SDLOP_SignalWakeup();
}

int SDLOP_PeekQueuedEvents(void)
{
    int count;
    pthread_mutex_lock(&sdlop_queue_lock);
    count = sdlop_queue_count;
    pthread_mutex_unlock(&sdlop_queue_lock);
    return count;
}

bool SDL_EventEnabled(Uint32 type)
{
    if (type >= SDLOP_EVENT_RANGE_BITS) {
        return false;
    }
    return (sdlop_event_enabled[type >> 3] & (Uint8)(1u << (type & 7))) != 0;
}

void SDL_SetEventEnabled(Uint32 type, bool enabled)
{
    if (type >= SDLOP_EVENT_RANGE_BITS) {
        return;
    }
    if (enabled) {
        sdlop_event_enabled[type >> 3] |= (Uint8)(1u << (type & 7));
    } else {
        sdlop_event_enabled[type >> 3] &= (Uint8)~(1u << (type & 7));
    }
}

Uint32 SDL_RegisterEvents(int numevents)
{
    Uint32 base;
    if (numevents <= 0) {
        return 0;
    }
    if ((Uint32)numevents > (SDL_EVENT_LAST + 1 - sdlop_next_user_event)) {
        SDL_SetError("Too many user events registered");
        return 0;
    }
    base = sdlop_next_user_event;
    sdlop_next_user_event += (Uint32)numevents;
    sdlop_max_user_event = sdlop_next_user_event;
    return base;
}

static void sdlop_queue_pop_front(SDL_Event *event)
{
    *event = sdlop_queue[sdlop_queue_head];
    sdlop_queue_head = (sdlop_queue_head + 1) % SDLOP_EVENT_QUEUE_SIZE;
    sdlop_queue_count--;
}

static bool sdlop_run_event_filter(SDL_Event *event)
{
    SDL_EventFilter filter;
    void *userdata;
    int i;

    /* Watches first: SDL3 calls them even for events the filter drops. */
    for (i = 0; i < sdlop_num_watches; i++) {
        if (!sdlop_watches[i].disabled) {
            sdlop_watches[i].filter(sdlop_watches[i].userdata, event);
        }
    }

    filter = sdlop_event_filter;
    userdata = sdlop_event_filter_userdata;
    if (filter) {
        if (!filter(userdata, event)) {
            return false;
        }
    }
    return true;
}

bool SDL_PushEvent(SDL_Event *event)
{
    return SDLOP_PushEvent(event);
}

bool SDLOP_PushEvent(const SDL_Event *event)
{
    bool pushed = false;
    bool wake = false;

    if (!event) {
        return SDL_InvalidParamError("event");
    }
    if (!SDL_EventEnabled(event->type)) {
        return false;
    }

    if (sdlop_event_filter || sdlop_num_watches) {
        SDL_Event copy = *event;
        if (!sdlop_run_event_filter(&copy)) {
            return false;
        }
        event = &copy;
    }

    pthread_mutex_lock(&sdlop_queue_lock);
    if (sdlop_queue_count < SDLOP_EVENT_QUEUE_SIZE) {
        sdlop_queue[sdlop_queue_tail] = *event;
        sdlop_queue_tail = (sdlop_queue_tail + 1) % SDLOP_EVENT_QUEUE_SIZE;
        sdlop_queue_count++;
        pushed = true;
    } else {
        SDL_SetError("Event queue is full (%d events)", SDLOP_EVENT_QUEUE_SIZE);
    }
    if (pushed && sdlop_queue_waiters > 0) {
        wake = true;
        pthread_cond_broadcast(&sdlop_queue_cond);
    }
    pthread_mutex_unlock(&sdlop_queue_lock);

    if (wake) {
        SDLOP_SignalWakeup();
    }
    return pushed;
}

int SDLOP_PeepEvents(SDL_Event *events, int numevents, SDL_EventAction action,
                     Uint32 minType, Uint32 maxType, bool is_internal)
{
    int found = 0;
    (void)is_internal;

    if (numevents <= 0 || !events) {
        return 0;
    }

    switch (action) {
        case SDL_ADDEVENT:
            for (int i = 0; i < numevents; i++) {
                if (!SDLOP_PushEvent(&events[i])) {
                    return i;
                }
            }
            return numevents;

        case SDL_PEEKEVENT:
        case SDL_GETEVENT:
            pthread_mutex_lock(&sdlop_queue_lock);
            {
                int index = sdlop_queue_head;
                for (int i = 0; i < sdlop_queue_count && found < numevents; i++, index = (index + 1) % SDLOP_EVENT_QUEUE_SIZE) {
                    Uint32 type = sdlop_queue[index].type;
                    if (type < minType || type > maxType) {
                        continue;
                    }
                    if (action == SDL_GETEVENT) {
                        /* compact the ring on the fly: index is where this event must
                           move to (head + found) */
                        int dest = (sdlop_queue_head + found) % SDLOP_EVENT_QUEUE_SIZE;
                        if (dest != index) {
                            sdlop_queue[dest] = sdlop_queue[index];
                        }
                        events[found++] = sdlop_queue[index];
                    } else {
                        events[found++] = sdlop_queue[index];
                    }
                }
                if (action == SDL_GETEVENT && found > 0) {
                    sdlop_queue_head = (sdlop_queue_head + found) % SDLOP_EVENT_QUEUE_SIZE;
                    sdlop_queue_count -= found;
                }
            }
            pthread_mutex_unlock(&sdlop_queue_lock);
            return found;
    }
    return 0;
}

int SDL_PeepEvents(SDL_Event *events, int numevents, SDL_EventAction action,
                   Uint32 minType, Uint32 maxType)
{
    return SDLOP_PeepEvents(events, numevents, action, minType, maxType, false);
}

bool SDL_HasEvent(Uint32 type)
{
    return SDL_HasEvents(type, type);
}

bool SDL_HasEvents(Uint32 minType, Uint32 maxType)
{
    SDL_Event event;
    return SDL_PeepEvents(&event, 1, SDL_PEEKEVENT, minType, maxType) > 0;
}

void SDL_FlushEvent(Uint32 type)
{
    SDL_FlushEvents(type, type);
}

void SDL_FlushEvents(Uint32 minType, Uint32 maxType)
{
    SDLOP_FlushEventsInternal(minType, maxType);
}

void SDLOP_FlushEventsInternal(Uint32 minType, Uint32 maxType)
{
    pthread_mutex_lock(&sdlop_queue_lock);
    for (int i = 0; i < sdlop_queue_count; ) {
        int index = (sdlop_queue_head + i) % SDLOP_EVENT_QUEUE_SIZE;
        Uint32 type = sdlop_queue[index].type;
        if (type >= minType && type <= maxType) {
            /* shift the tail down by one */
            for (int j = i; j < sdlop_queue_count - 1; j++) {
                sdlop_queue[(sdlop_queue_head + j) % SDLOP_EVENT_QUEUE_SIZE] =
                    sdlop_queue[(sdlop_queue_head + j + 1) % SDLOP_EVENT_QUEUE_SIZE];
            }
            sdlop_queue_tail = (sdlop_queue_tail + SDLOP_EVENT_QUEUE_SIZE - 1) % SDLOP_EVENT_QUEUE_SIZE;
            sdlop_queue_count--;
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&sdlop_queue_lock);
}

bool SDL_PollEvent(SDL_Event *event)
{
    bool ret = false;

    /* An event that is already queued is handed out without pumping first: the
       pump is the expensive part of the call (poll(2) on the backends'
       descriptors, the input ring, the due timers), and a queue that has an
       event in it has just been pumped. SDL_PollEvent is called once per event,
       so this is the difference between a tight loop that costs a pump per
       event and one that does not. */
    pthread_mutex_lock(&sdlop_queue_lock);
    if (event && sdlop_queue_count > 0) {
        sdlop_queue_pop_front(event);
        pthread_mutex_unlock(&sdlop_queue_lock);
        return true;
    }
    pthread_mutex_unlock(&sdlop_queue_lock);

    SDL_PumpEvents();

    pthread_mutex_lock(&sdlop_queue_lock);
    if (!event) {
        /* SDL3 semantics: a NULL event drains the queue. */
        sdlop_queue_count = 0;
        sdlop_queue_head = sdlop_queue_tail = 0;
    } else if (sdlop_queue_count > 0) {
        sdlop_queue_pop_front(event);
        ret = true;
    }
    pthread_mutex_unlock(&sdlop_queue_lock);
    return ret;
}

bool SDLOP_WaitEventTimeoutNS(SDL_Event *event, Sint64 timeoutNS)
{
    const Sint64 start = (Sint64)SDL_GetTicksNS();

    for (;;) {
        Sint64 next_timer, remaining, wait_ns;

        SDL_PumpEvents();
        if (SDL_PollEvent(event)) {
            return true;
        }

        remaining = (timeoutNS < 0) ? -1 : (timeoutNS - ((Sint64)SDL_GetTicksNS() - start));
        if (remaining == 0 || (timeoutNS >= 0 && remaining < 0)) {
            return false;
        }

        next_timer = SDLOP_NextTimerTimeoutNS();
        {
            const SDLOP_VideoDriver *vdriver = SDLOP_GetVideoDriver();
            if (vdriver && vdriver->get_event_timeout_ns) {
                Sint64 backend_timer = vdriver->get_event_timeout_ns();
                if (backend_timer >= 0 && (next_timer < 0 || backend_timer < next_timer)) {
                    next_timer = backend_timer;
                }
            }
        }
        wait_ns = remaining;
        if (next_timer >= 0 && (wait_ns < 0 || next_timer < wait_ns)) {
            wait_ns = next_timer;
        }
        SDLOP_WaitPlatformEvents(wait_ns);
    }
}

bool SDL_WaitEvent(SDL_Event *event)
{
    return SDLOP_WaitEventTimeoutNS(event, -1);
}

bool SDL_WaitEventTimeout(SDL_Event *event, Sint32 timeoutMS)
{
    return SDLOP_WaitEventTimeoutNS(event, timeoutMS < 0 ? -1 : (Sint64)timeoutMS * 1000000LL);
}

bool SDL_WaitEventTimeoutNS(SDL_Event *event, Sint64 timeoutNS)
{
    return SDLOP_WaitEventTimeoutNS(event, timeoutNS);
}

void SDL_SetEventFilter(SDL_EventFilter filter, void *userdata)
{
    sdlop_event_filter = filter;
    sdlop_event_filter_userdata = userdata;
}

bool SDL_GetEventFilter(SDL_EventFilter *filter, void **userdata)
{
    if (filter) {
        *filter = sdlop_event_filter;
    }
    if (userdata) {
        *userdata = sdlop_event_filter_userdata;
    }
    return sdlop_event_filter != NULL;
}

bool SDL_AddEventWatch(SDL_EventFilter filter, void *userdata)
{
    if (sdlop_num_watches == SDLOP_MAX_WATCHES) {
        return SDL_SetError("Too many event watches");
    }
    sdlop_watches[sdlop_num_watches].filter = filter;
    sdlop_watches[sdlop_num_watches].userdata = userdata;
    sdlop_watches[sdlop_num_watches].disabled = false;
    sdlop_num_watches++;
    return true;
}

void SDL_RemoveEventWatch(SDL_EventFilter filter, void *userdata)
{
    for (int i = 0; i < sdlop_num_watches; i++) {
        if (sdlop_watches[i].filter == filter && sdlop_watches[i].userdata == userdata) {
            memmove(&sdlop_watches[i], &sdlop_watches[i + 1],
                    (size_t)(sdlop_num_watches - i - 1) * sizeof(sdlop_watches[0]));
            sdlop_num_watches--;
            return;
        }
    }
}

void SDL_FilterEvents(SDL_EventFilter filter, void *userdata)
{
    SDL_Event events[SDLOP_EVENT_QUEUE_SIZE];
    int numevents, i;

    numevents = SDLOP_PeepEvents(events, SDLOP_EVENT_QUEUE_SIZE, SDL_GETEVENT,
                                 SDL_EVENT_FIRST, SDL_EVENT_LAST, false);
    for (i = 0; i < numevents; i++) {
        if (filter(userdata, &events[i])) {
            SDLOP_PushEvent(&events[i]);
        }
    }
}

void SDL_PumpEvents(void)
{
    /* A signal that arrived since the last pump becomes SDL_EVENT_QUIT (see the
       signal handling above). */
    SDLOP_QueueSignalQuit();
    /* Platform events first: they carry window state that input events refer to. */
    SDLOP_VideoPumpEvents();
    /* Then the async raw-input records handed over by the input thread. */
    SDLOP_PumpRawInput();
    /* Then everything that is due on this thread. */
    SDLOP_RunMainThreadCallbacks();
    SDLOP_RunTimerCallbacks();
}

void SDLOP_PumpEventsInternal(void)
{
    SDL_PumpEvents();
}
