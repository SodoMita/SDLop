/*
  SDLop -- SDL_timer.h.

  Clocks come from clock_gettime(CLOCK_MONOTONIC); SDL_GetPerformanceCounter() is
  the same monotonic clock in nanoseconds so it never goes backwards. Timer
  callbacks run on the thread that calls SDL_PumpEvents()/SDL_WaitEvent() (SDL3
  runs them on a private thread; the SDLop build folds them into the event pump,
  which removes a thread and a lock from the common case).
*/

#include "../sdlop_internal.h"

#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>

static Uint64 sdlop_start_ns;
static bool sdlop_ticks_initialized;

static Uint64 sdlop_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Uint64)ts.tv_sec * 1000000000ULL + (Uint64)ts.tv_nsec;
}

static void sdlop_ensure_ticks(void)
{
    if (!sdlop_ticks_initialized) {
        sdlop_start_ns = sdlop_now_ns();
        sdlop_ticks_initialized = true;
    }
}

Uint64 SDL_GetTicksNS(void)
{
    sdlop_ensure_ticks();
    return sdlop_now_ns() - sdlop_start_ns;
}

Uint64 SDL_GetTicks(void)
{
    return SDL_GetTicksNS() / 1000000ULL;
}

Uint64 SDL_GetPerformanceCounter(void)
{
    sdlop_ensure_ticks();
    return sdlop_now_ns();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return 1000000000ULL;
}

void SDL_DelayNS(Uint64 ns)
{
    struct timespec ts;
    if (ns == 0) {
        return;
    }
    ts.tv_sec = (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
        /* keep sleeping for the remaining time */
    }
}

void SDL_Delay(Uint32 ms)
{
    SDL_DelayNS((Uint64)ms * 1000000ULL);
}

void SDL_DelayPrecise(Uint64 ns)
{
    /* SDL3 spins the last stretch for sub-millisecond accuracy; SDLop sleeps
       until the deadline, which is what a lean event loop wants. */
    SDL_DelayNS(ns);
}

/* ------------------------------------------------------------------------- */
/* Timer callbacks                                                           */
/* ------------------------------------------------------------------------- */

typedef struct SDLOP_Timer
{
    SDL_TimerID id;
    Uint64 deadline_ns;
    Uint64 interval_ns;         /* 0: one-shot */
    union
    {
        SDL_TimerCallback ms;
        SDL_NSTimerCallback ns;
    } callback;
    void *userdata;
    bool uses_ns;
    bool cancelled;
} SDLOP_Timer;

static pthread_mutex_t sdlop_timers_lock = PTHREAD_MUTEX_INITIALIZER;
static SDLOP_Timer *sdlop_timers;
static int sdlop_num_timers;
/* Set while the table is non-empty, so SDLOP_RunTimerCallbacks() can return
   without reading the clock when the application has no timers at all. */
static int sdlop_any_timers;
static int sdlop_timers_capacity;
static SDL_TimerID sdlop_next_timer_id = 1;

void SDLOP_InitTimers(void)
{
    sdlop_ensure_ticks();
    sdlop_num_timers = 0;
    __atomic_store_n(&sdlop_any_timers, 0, __ATOMIC_RELEASE);
}

void SDLOP_QuitTimers(void)
{
    pthread_mutex_lock(&sdlop_timers_lock);
    sdlop_num_timers = 0;
    __atomic_store_n(&sdlop_any_timers, 0, __ATOMIC_RELEASE);
    SDLOP_Free(sdlop_timers);
    sdlop_timers = NULL;
    sdlop_timers_capacity = 0;
    pthread_mutex_unlock(&sdlop_timers_lock);
}

static SDL_TimerID sdlop_add_timer(Uint64 interval_ns, void *callback, void *userdata, bool uses_ns)
{
    SDLOP_Timer *timer;

    if (interval_ns == 0) {
        interval_ns = 1;
    }
    pthread_mutex_lock(&sdlop_timers_lock);
    if (sdlop_num_timers == sdlop_timers_capacity) {
        int newcap = sdlop_timers_capacity ? sdlop_timers_capacity * 2 : 8;
        SDLOP_Timer *newtimers = (SDLOP_Timer *)SDLOP_Realloc(sdlop_timers, (size_t)newcap * sizeof(*newtimers));
        if (!newtimers) {
            pthread_mutex_unlock(&sdlop_timers_lock);
            SDLOP_OutOfMemory();
            return 0;
        }
        sdlop_timers = newtimers;
        sdlop_timers_capacity = newcap;
    }
    timer = &sdlop_timers[sdlop_num_timers++];
    __atomic_store_n(&sdlop_any_timers, 1, __ATOMIC_RELEASE);
    memset(timer, 0, sizeof(*timer));
    timer->id = sdlop_next_timer_id++;
    if (timer->id == 0) {
        timer->id = sdlop_next_timer_id++;
    }
    timer->interval_ns = interval_ns;
    timer->uses_ns = uses_ns;
    if (uses_ns) {
        timer->callback.ns = (SDL_NSTimerCallback)callback;
    } else {
        timer->callback.ms = (SDL_TimerCallback)callback;
    }
    timer->userdata = userdata;
    timer->deadline_ns = SDL_GetTicksNS() + interval_ns;
    pthread_mutex_unlock(&sdlop_timers_lock);
    return timer->id;
}

SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *userdata)
{
    if (!callback) {
        SDL_InvalidParamError("callback");
        return 0;
    }
    return sdlop_add_timer((Uint64)interval * 1000000ULL, (void *)callback, userdata, false);
}

SDL_TimerID SDL_AddTimerNS(Uint64 interval, SDL_NSTimerCallback callback, void *userdata)
{
    if (!callback) {
        SDL_InvalidParamError("callback");
        return 0;
    }
    return sdlop_add_timer(interval, (void *)callback, userdata, true);
}

bool SDL_RemoveTimer(SDL_TimerID id)
{
    int i;
    bool found = false;

    pthread_mutex_lock(&sdlop_timers_lock);
    for (i = 0; i < sdlop_num_timers; i++) {
        if (sdlop_timers[i].id == id && !sdlop_timers[i].cancelled) {
            sdlop_timers[i].cancelled = true;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&sdlop_timers_lock);
    if (!found) {
        SDL_SetError("Invalid timer ID");
    }
    return found;
}

/* How long until the next timer fires? -1 when there is nothing pending. */
Sint64 SDLOP_NextTimerTimeoutNS(void)
{
    Uint64 now = SDL_GetTicksNS();
    Sint64 best = -1;
    int i;

    pthread_mutex_lock(&sdlop_timers_lock);
    for (i = 0; i < sdlop_num_timers; i++) {
        const SDLOP_Timer *timer = &sdlop_timers[i];
        if (timer->cancelled) {
            continue;
        }
        if (timer->deadline_ns <= now) {
            best = 0;
            break;
        }
        if (best < 0 || (Sint64)(timer->deadline_ns - now) < best) {
            best = (Sint64)(timer->deadline_ns - now);
        }
    }
    pthread_mutex_unlock(&sdlop_timers_lock);
    return best;
}

#define SDLOP_MAX_TIMERS_PER_PUMP 16

void SDLOP_RunTimerCallbacks(void)
{
    Uint64 now;
    SDLOP_Timer due[SDLOP_MAX_TIMERS_PER_PUMP];
    int num_due = 0;
    int i, write_index = 0;

    if (__atomic_load_n(&sdlop_any_timers, __ATOMIC_ACQUIRE) == 0) {
        return;                        /* no timers: skip clock + lock entirely */
    }
    now = SDL_GetTicksNS();
    pthread_mutex_lock(&sdlop_timers_lock);
    for (i = 0; i < sdlop_num_timers; i++) {
        SDLOP_Timer *timer = &sdlop_timers[i];
        if (timer->cancelled) {
            continue;
        }
        if (timer->deadline_ns <= now && num_due < SDLOP_MAX_TIMERS_PER_PUMP) {
            due[num_due++] = *timer;
            if (timer->interval_ns > 0) {
                /* reschedule, skipping missed periods instead of firing a burst */
                do {
                    timer->deadline_ns += timer->interval_ns;
                } while (timer->deadline_ns <= now);
                sdlop_timers[write_index++] = *timer;
            }
            continue;
        }
        sdlop_timers[write_index++] = *timer;
    }
    for (i = write_index; i < sdlop_num_timers; i++) {
        memset(&sdlop_timers[i], 0, sizeof(sdlop_timers[i]));
    }
    sdlop_num_timers = write_index;
    __atomic_store_n(&sdlop_any_timers, write_index > 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&sdlop_timers_lock);

    /* Callbacks run without the lock held so they may add/remove timers. */
    for (i = 0; i < num_due; i++) {
        if (due[i].uses_ns) {
            Uint64 next = due[i].callback.ns(due[i].userdata, due[i].id, due[i].interval_ns);
            if (next != due[i].interval_ns && next > 0) {
                int j;
                pthread_mutex_lock(&sdlop_timers_lock);
                for (j = 0; j < sdlop_num_timers; j++) {
                    if (sdlop_timers[j].id == due[i].id) {
                        sdlop_timers[j].interval_ns = next;
                        break;
                    }
                }
                pthread_mutex_unlock(&sdlop_timers_lock);
            }
        } else {
            Uint32 next = due[i].callback.ms(due[i].userdata, due[i].id,
                                            (Uint32)(due[i].interval_ns / 1000000ULL));
            if (next != 0) {
                int j;
                pthread_mutex_lock(&sdlop_timers_lock);
                for (j = 0; j < sdlop_num_timers; j++) {
                    if (sdlop_timers[j].id == due[i].id) {
                        sdlop_timers[j].interval_ns = (Uint64)next * 1000000ULL;
                        break;
                    }
                }
                pthread_mutex_unlock(&sdlop_timers_lock);
            } else {
                SDL_RemoveTimer(due[i].id);
            }
        }
    }
}
