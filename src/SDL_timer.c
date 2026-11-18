/*
  SDLop - time and timers (SDL_timer.h).

  Clocks are CLOCK_MONOTONIC. SDL_AddTimer()/SDL_AddTimerNS() callbacks run
  on a dedicated timer thread, like SDL3: the thread is started by the
  first timer and stopped by SDL_Quit(). A callback returns the next
  interval (0 cancels the timer); SDL_PushEvent() is safe to call from it.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <time.h>

/* CLOCK_MONOTONIC origin captured at first use (~library load). */
static Uint64 timer_origin_ns(void)
{
    static Uint64 origin;
    static int inited;
    if (!inited) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        origin = (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
        inited = 1;
    }
    return origin;
}

Uint64 SDL_GetPerformanceCounter(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return 1000000000ull;
}

Uint64 SDL_GetTicksNS(void)
{
    return SDL_GetPerformanceCounter() - timer_origin_ns();
}

Uint64 SDL_GetTicks(void)
{
    return SDL_GetTicksNS() / 1000000ull;
}

void SDL_Delay(Uint32 ms)
{
    SDL_DelayNS((Uint64)ms * 1000000ull);
}

void SDL_DelayNS(Uint64 ns)
{
    struct timespec req, rem;
    req.tv_sec = (time_t)(ns / 1000000000ull);
    req.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) {
        req = rem;
    }
}

static void ns_to_timespec(Uint64 ns, struct timespec *ts)
{
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
}

void SDL_DelayPrecise(Uint64 ns)
{
    /* absolute sleep to just short of the deadline, then spin: immune to
     * EINTR drift and accurate to a few microseconds */
    const Uint64 SPIN_NS = 200000; /* 0.2 ms */
    const Uint64 target = SDL_GetPerformanceCounter() + ns;
    if (ns > SPIN_NS) {
        struct timespec ts;
        ns_to_timespec(target - SPIN_NS, &ts);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
        }
    }
    while (SDL_GetPerformanceCounter() < target) {
        sched_yield();
    }
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

typedef struct SDLOP_Timer
{
    SDL_TimerID id;
    SDL_TimerCallback callback_ms;
    SDL_NSTimerCallback callback_ns;
    void *userdata;
    Uint64 interval_ns;
    Uint64 deadline_ns;     /* absolute SDL_GetPerformanceCounter() time */
    bool canceled;
    struct SDLOP_Timer *next;
} SDLOP_Timer;

static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cond;
static bool timer_cond_inited;
static pthread_t timer_thread;
static bool timer_thread_running;
static bool timer_quit;
static SDLOP_Timer *timers;   /* sorted by deadline */
static SDLOP_Timer *running;  /* timer whose callback is executing (unlocked) */
static SDL_TimerID next_timer_id;

static void insert_sorted(SDLOP_Timer *t)
{
    SDLOP_Timer **link = &timers;
    while (*link && (*link)->deadline_ns <= t->deadline_ns) {
        link = &(*link)->next;
    }
    t->next = *link;
    *link = t;
}

static void *timer_thread_main(void *arg)
{
    (void)arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL); /* signals belong to the app */

    pthread_mutex_lock(&timer_lock);
    while (!timer_quit) {
        if (!timers) {
            pthread_cond_wait(&timer_cond, &timer_lock);
            continue;
        }
        Uint64 now = SDL_GetPerformanceCounter();
        SDLOP_Timer *t = timers;
        if (t->deadline_ns > now) {
            struct timespec ts;
            ns_to_timespec(t->deadline_ns, &ts);
            pthread_cond_timedwait(&timer_cond, &timer_lock, &ts);
            continue;
        }
        timers = t->next;
        if (t->canceled) {
            SDL_free(t);
            continue;
        }
        running = t;
        pthread_mutex_unlock(&timer_lock);

        Uint64 next_ns;
        if (t->callback_ns) {
            next_ns = t->callback_ns(t->userdata, t->id, t->interval_ns);
        } else {
            next_ns = (Uint64)t->callback_ms(t->userdata, t->id, (Uint32)(t->interval_ns / 1000000ull)) * 1000000ull;
        }

        pthread_mutex_lock(&timer_lock);
        running = NULL;
        if (next_ns && !t->canceled && !timer_quit) {
            t->interval_ns = next_ns;
            /* SDL3: the next deadline counts from when the callback was due */
            t->deadline_ns += next_ns;
            now = SDL_GetPerformanceCounter();
            if (t->deadline_ns < now) {
                t->deadline_ns = now;
            }
            insert_sorted(t);
        } else {
            SDL_free(t);
        }
    }
    pthread_mutex_unlock(&timer_lock);
    return NULL;
}

static SDL_TimerID add_timer(Uint64 interval_ns, SDL_TimerCallback cb_ms, SDL_NSTimerCallback cb_ns, void *userdata)
{
    if (!cb_ms && !cb_ns) {
        SDL_InvalidParamError("callback");
        return 0;
    }
    SDLOP_Timer *t = (SDLOP_Timer *)SDL_calloc(1, sizeof(*t));
    if (!t) {
        return 0;
    }
    t->callback_ms = cb_ms;
    t->callback_ns = cb_ns;
    t->userdata = userdata;
    t->interval_ns = interval_ns;
    t->deadline_ns = SDL_GetPerformanceCounter() + interval_ns;

    pthread_mutex_lock(&timer_lock);
    if (!timer_cond_inited) {
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&timer_cond, &attr);
        pthread_condattr_destroy(&attr);
        timer_cond_inited = true;
    }
    if (!timer_thread_running) {
        timer_quit = false;
        if (pthread_create(&timer_thread, NULL, timer_thread_main, NULL) != 0) {
            pthread_mutex_unlock(&timer_lock);
            SDL_free(t);
            SDL_SetError("Couldn't create timer thread");
            return 0;
        }
        timer_thread_running = true;
    }
    t->id = ++next_timer_id;
    if (t->id == 0) {
        t->id = ++next_timer_id;
    }
    SDL_TimerID id = t->id;
    insert_sorted(t);
    pthread_cond_signal(&timer_cond);
    pthread_mutex_unlock(&timer_lock);
    return id;
}

SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *userdata)
{
    return add_timer((Uint64)interval * 1000000ull, callback, NULL, userdata);
}

SDL_TimerID SDL_AddTimerNS(Uint64 interval, SDL_NSTimerCallback callback, void *userdata)
{
    return add_timer(interval, NULL, callback, userdata);
}

bool SDL_RemoveTimer(SDL_TimerID id)
{
    if (!id) {
        return SDL_InvalidParamError("id");
    }
    bool found = false;
    pthread_mutex_lock(&timer_lock);
    for (SDLOP_Timer *t = timers; t; t = t->next) {
        if (t->id == id && !t->canceled) {
            t->canceled = true;
            found = true;
            break;
        }
    }
    if (!found && running && running->id == id && !running->canceled) {
        running->canceled = true;
        found = true;
    }
    pthread_mutex_unlock(&timer_lock);
    if (!found) {
        return SDL_SetError("Timer not found");
    }
    return true;
}

/* SDL_Quit(): stop the thread, drop all timers */
void SDLOP_QuitTimers(void)
{
    pthread_mutex_lock(&timer_lock);
    if (!timer_thread_running) {
        pthread_mutex_unlock(&timer_lock);
        return;
    }
    timer_quit = true;
    pthread_cond_signal(&timer_cond);
    pthread_mutex_unlock(&timer_lock);
    if (!pthread_equal(pthread_self(), timer_thread)) {
        pthread_join(timer_thread, NULL);
    } else {
        pthread_detach(timer_thread); /* SDL_Quit() from a timer callback */
    }
    pthread_mutex_lock(&timer_lock);
    timer_thread_running = false;
    while (timers) {
        SDLOP_Timer *t = timers;
        timers = t->next;
        SDL_free(t);
    }
    pthread_mutex_unlock(&timer_lock);
}
