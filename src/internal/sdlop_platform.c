/*
  SDLop - sdlop_platform.h: the clock, and the Windows half of the
  semaphore, condition-variable and static-mutex primitives.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

/* ------------------------------------------------------------------ */
/* Clock (both platforms)                                              */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
/* QueryPerformanceCounter is the highest-resolution monotonic source Windows
 * offers and, like CLOCK_MONOTONIC, is unaffected by wall-clock changes. QPF is
 * constant, but the origin must be published with it: a lazy
 * `if (!freq.QuadPart)` init lets a second thread observe freq set and start
 * still 0, which yields a timestamp near the epoch. One once block fixes it. */
static INIT_ONCE sdloP_clock_once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER sdloP_clock_freq;
static LARGE_INTEGER sdloP_clock_start;

static BOOL CALLBACK sdloP_clock_init(PINIT_ONCE once, PVOID param, PVOID *context)
{
    (void)once;
    (void)param;
    (void)context;
    QueryPerformanceFrequency(&sdloP_clock_freq);
    QueryPerformanceCounter(&sdloP_clock_start);
    return TRUE;
}
#endif

Sint64 SDLOP_MonotonicNowNS(void)
{
#ifdef _WIN32
    LARGE_INTEGER now;
    InitOnceExecuteOnce(&sdloP_clock_once, sdloP_clock_init, NULL, NULL);
    QueryPerformanceCounter(&now);
    const LONGLONG ticks = now.QuadPart - sdloP_clock_start.QuadPart;
    return (Sint64)(ticks / sdloP_clock_freq.QuadPart) * 1000000000ll +
           (Sint64)((ticks % sdloP_clock_freq.QuadPart) * 1000000000ll / sdloP_clock_freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Sint64)ts.tv_sec * 1000000000ll + (Sint64)ts.tv_nsec;
#endif
}

void SDLOP_DeadlineFromNS(struct timespec *ts, Sint64 deadline_ns)
{
    ts->tv_sec = (time_t)(deadline_ns / 1000000000ll);
    ts->tv_nsec = (long)(deadline_ns % 1000000000ll);
}

#ifdef _WIN32

/* ------------------------------------------------------------------ */
/* Once helpers                                                        */
/* ------------------------------------------------------------------ */

BOOL CALLBACK SDLOP_OnceTrampoline(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)ctx;
    ((void (*)(void))param)();
    return TRUE;
}

BOOL CALLBACK SDLOP_StaticMutexInitTrampoline(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)ctx;
    InitializeCriticalSection(&((SDLOP_StaticMutex *)param)->id);
    return TRUE;
}

Uint32 SDLOP_ThreadIDOfHandle(HANDLE h)
{
    return GetThreadId(h);
}

/* ------------------------------------------------------------------ */
/* Semaphore                                                           */
/* ------------------------------------------------------------------ */

/* A Win32 semaphore with count n lets n waits through and blocks the next
 * one, which is exactly SDL3's "count up from 0" SDL_Semaphore: the handle's
 * count *is* the value SDL reports. `count` mirrors it so
 * SDL_GetSemaphoreValue() is a plain read that does not disturb waiters. */
bool SDLOP_SemInit(SDLOP_SEM_T *sem, Uint32 initial_value)
{
    const LONG initial = initial_value > (Uint32)SDLOP_SEM_MAX ? SDLOP_SEM_MAX : (LONG)initial_value;
    sem->handle = CreateSemaphoreA(NULL, initial, SDLOP_SEM_MAX, NULL);
    sem->count = initial;
    return sem->handle != NULL;
}

void SDLOP_SemDestroy(SDLOP_SEM_T *sem)
{
    if (sem->handle) {
        CloseHandle(sem->handle);
        sem->handle = NULL;
    }
}

void SDLOP_SemPost(SDLOP_SEM_T *sem)
{
    if (!sem->handle) {
        return;
    }
    InterlockedIncrement(&sem->count);
    ReleaseSemaphore(sem->handle, 1, NULL);
}

bool SDLOP_SemTryWait(SDLOP_SEM_T *sem)
{
    if (!sem->handle) {
        return false;
    }
    if (WaitForSingleObject(sem->handle, 0) != WAIT_OBJECT_0) {
        return false;
    }
    InterlockedDecrement(&sem->count);
    return true;
}

bool SDLOP_SemWait(SDLOP_SEM_T *sem)
{
    if (!sem || !sem->handle) {
        return false;
    }
    if (WaitForSingleObject(sem->handle, INFINITE) != WAIT_OBJECT_0) {
        return false;
    }
    InterlockedDecrement(&sem->count);
    return true;
}

Uint32 SDLOP_SemValue(SDLOP_SEM_T *sem)
{
    if (!sem) {
        return 0;
    }
    const LONG v = InterlockedCompareExchange(&sem->count, 0, 0);
    return v < 0 ? 0u : (Uint32)v;
}

/* Takes `count` down by one when the semaphore is signalled, but only once
 * the deadline has actually passed: a spurious WAIT_TIMEOUT is retried. */
static bool sem_wait_until_deadline(SDLOP_SEM_T *sem, Sint64 deadline_ns)
{
    for (;;) {
        const Sint64 left = deadline_ns - SDLOP_MonotonicNowNS();
        if (left <= 0) {
            return false;
        }
        const Uint64 ms = ((Uint64)left + 999999ull) / 1000000ull;
        const DWORD r = WaitForSingleObject(sem->handle, (DWORD)(ms > 0x7fffffffull ? 0x7fffffffull : ms));
        if (r == WAIT_OBJECT_0) {
            InterlockedDecrement(&sem->count);
            return true;
        }
        if (r != WAIT_TIMEOUT) {
            return false; /* WAIT_ABANDONED/WAIT_FAILED: treat as unsignalled */
        }
    }
}

bool SDLOP_SemWaitUntil(SDLOP_SEM_T *sem, const struct timespec *deadline)
{
    if (!sem->handle) {
        return false;
    }
    const Sint64 ns = (Sint64)deadline->tv_sec * 1000000000ll + (Sint64)deadline->tv_nsec;
    return sem_wait_until_deadline(sem, ns);
}

/* ------------------------------------------------------------------ */
/* Condition variable                                                  */
/* ------------------------------------------------------------------ */

int SDLOP_CondWaitUntilImpl(CONDITION_VARIABLE *cond, CRITICAL_SECTION *mutex,
                            const struct timespec *deadline)
{
    const Sint64 want = (Sint64)deadline->tv_sec * 1000000000ll + (Sint64)deadline->tv_nsec;
    for (;;) {
        const Sint64 left = want - SDLOP_MonotonicNowNS();
        if (left <= 0) {
            return SDLOP_COND_TIMEDOUT;
        }
        const Uint64 ms = ((Uint64)left + 999999ull) / 1000000ull;
        if (SleepConditionVariableCS(cond, mutex, (DWORD)(ms > 0x7fffffffull ? 0x7fffffffull : ms))) {
            return 0;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_TIMEOUT) {
            return (int)err;
        }
        /* SleepConditionVariableCS truncates to milliseconds, so a wait that
         * ends just short of the deadline comes back as a timeout; only
         * report one once the deadline is really past. */
        if (SDLOP_MonotonicNowNS() >= want) {
            return SDLOP_COND_TIMEDOUT;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Sleep                                                               */
/* ------------------------------------------------------------------ */

/* Sleeps. A waitable timer must be owned by the thread that waits on it: one
 * shared handle makes concurrent sleepers steal each other's signalled state
 * (one thread arms it, another consumes it, and the arming thread blocks
 * forever). So keep one per thread on Windows, and fall back to Sleep() when
 * the thread-local cannot be allocated. */
static _Thread_local HANDLE sdloP_timer_handle;
static _Thread_local int sdloP_timer_tried; /* 0 = unknown, 1 = have it, -1 = none */

static void sdloP_sleep_ns(Sint64 ns)
{
    if (ns <= 0) {
        return;
    }
    if (!sdloP_timer_tried) {
        sdloP_timer_tried = -1;
        /* AUTO-RESET: the wait must return the timer to non-signalled by
         * itself, or every later wait returns immediately and a caller
         * looping on "sleep until deadline" spins at 100% CPU. */
        sdloP_timer_handle = CreateWaitableTimerExW(NULL, NULL,
                                                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                    TIMER_ALL_ACCESS);
        if (sdloP_timer_handle) {
            sdloP_timer_tried = 1;
        } else {
            sdloP_timer_handle = CreateWaitableTimerW(NULL, FALSE, NULL);
            if (sdloP_timer_handle) {
                sdloP_timer_tried = 1;
            }
        }
    }
    if (sdloP_timer_handle) {
        /* The timer takes 100 ns units, negative = relative. */
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100);
        if (due.QuadPart == 0) {
            due.QuadPart = -1;
        }
        if (SetWaitableTimer(sdloP_timer_handle, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(sdloP_timer_handle, INFINITE);
            return;
        }
    }
    const Uint64 ms = ((Uint64)ns + 999999ull) / 1000000ull;
    Sleep((DWORD)(ms > 0x7fffffffull ? 0x7fffffffull : ms));
}

void SDLOP_SleepNS(Sint64 ns)
{
    sdloP_sleep_ns(ns);
}

void SDLOP_SleepUntilNS(Sint64 abs_ns)
{
    for (;;) {
        const Sint64 left = abs_ns - SDLOP_MonotonicNowNS();
        if (left <= 200000) { /* leave the last 200 us to the caller's spin */
            return;
        }
        sdloP_sleep_ns(left - 200000);
    }
}

#endif /* _WIN32 */
