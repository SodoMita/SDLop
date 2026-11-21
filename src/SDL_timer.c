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
#include <time.h>
#ifndef _WIN32
#include <sched.h>
#include <signal.h>
#endif

/* CLOCK_MONOTONIC origin captured at first use (~library load). */
static Uint64 timer_origin_ns(void)
{
    static Uint64 origin;
    static int inited;
    if (!inited) {
        origin = SDLOP_MonotonicNowNS();
        inited = 1;
    }
    return origin;
}

Uint64 SDL_GetPerformanceCounter(void)
{
    return (Uint64)SDLOP_MonotonicNowNS();
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
    SDLOP_SLEEP_NS(ns);
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
        SDLOP_SLEEP_UNTIL_NS(target - SPIN_NS);
    }
    while (SDL_GetPerformanceCounter() < target) {
        SDLOP_YIELD();
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

static SDLOP_STATIC_MUTEX(timer_lock);
static SDLOP_COND_T timer_cond;
static bool timer_cond_inited;
#ifdef _WIN32
static HANDLE timer_thread;
#else
static pthread_t timer_thread;
static SDL_ThreadID timer_thread_id; /* 0 = none; used to avoid self-join */
#endif
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

static
#ifdef _WIN32
unsigned __stdcall
#else
void *
#endif
timer_thread_main(void *arg)
{
    (void)arg;
#ifndef _WIN32
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL); /* signals belong to the app */
#endif

    SDLOP_MUTEX_LOCK(&timer_lock.id);
    while (!timer_quit) {
        if (!timers) {
            SDLOP_COND_WAIT(&timer_cond, SDLOP_SM(&timer_lock));
            continue;
        }
        Uint64 now = SDL_GetPerformanceCounter();
        SDLOP_Timer *t = timers;
        if (t->deadline_ns > now) {
            struct timespec ts;
            ns_to_timespec(t->deadline_ns, &ts);
            SDLOP_COND_WAIT_UNTIL(&timer_cond, SDLOP_SM(&timer_lock), &ts);
            continue;
        }
        timers = t->next;
        if (t->canceled) {
            SDL_free(t);
            continue;
        }
        running = t;
        SDLOP_MUTEX_UNLOCK(&timer_lock.id);

        Uint64 next_ns;
        if (t->callback_ns) {
            next_ns = t->callback_ns(t->userdata, t->id, t->interval_ns);
        } else {
            next_ns = (Uint64)t->callback_ms(t->userdata, t->id, (Uint32)(t->interval_ns / 1000000ull)) * 1000000ull;
        }

        SDLOP_MUTEX_LOCK(&timer_lock.id);
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
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&timer_lock));
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
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

    SDLOP_MUTEX_LOCK(&timer_lock.id);
    if (!timer_cond_inited) {
        SDLOP_COND_INIT(&timer_cond);
        timer_cond_inited = true;
    }
    if (!timer_thread_running) {
        timer_quit = false;
        bool created;
#ifdef _WIN32
        timer_thread = (HANDLE)_beginthreadex(NULL, 0, timer_thread_main, NULL, 0, NULL);
        created = (timer_thread != NULL);
#else
        created = (pthread_create(&timer_thread, NULL, timer_thread_main, NULL) == 0);
        timer_thread_id = timer_thread;
#endif
        if (!created) {
            SDLOP_MUTEX_UNLOCK(&timer_lock.id);
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
    SDLOP_COND_SIGNAL(&timer_cond);
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&timer_lock));
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
    SDLOP_MUTEX_LOCK(&timer_lock.id);
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
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&timer_lock));
    if (!found) {
        return SDL_SetError("Timer not found");
    }
    return true;
}

/* SDL_Quit(): stop the thread, drop all timers */
void SDLOP_QuitTimers(void)
{
    SDLOP_MUTEX_LOCK(&timer_lock.id);
    if (!timer_thread_running) {
        SDLOP_MUTEX_UNLOCK(&timer_lock.id);
        return;
    }
    timer_quit = true;
    SDLOP_COND_SIGNAL(&timer_cond);
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&timer_lock));
#ifdef _WIN32
    if (SDL_GetCurrentThreadID() != SDLOP_ThreadIDOfHandle(timer_thread)) {
        WaitForSingleObject(timer_thread, INFINITE);
        CloseHandle(timer_thread);
    } else {
        CloseHandle(timer_thread); /* SDL_Quit() from a timer callback */
    }
#else
    if (SDL_GetCurrentThreadID() != timer_thread_id) {
        pthread_join(timer_thread, NULL);
    } else {
        pthread_detach(timer_thread); /* SDL_Quit() from a timer callback */
    }
#endif
    SDLOP_MUTEX_LOCK(&timer_lock.id);
    timer_thread_running = false;
    while (timers) {
        SDLOP_Timer *t = timers;
        timers = t->next;
        SDL_free(t);
    }
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&timer_lock));
}
