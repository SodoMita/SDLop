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

/* Sleeps.
 *
 * A waitable timer has to belong to the thread that waits on it: with one
 * shared handle, concurrent sleepers steal each other's signalled state -- one
 * thread arms it, another consumes it, and the arming thread blocks forever.
 * (Measured: eight threads doing SDL_Delay(20) under Wine hung outright.)
 *
 * So one timer per thread, owned by fiber-local storage rather than a bare
 * _Thread_local. A _Thread_local HANDLE has no destructor, so every thread
 * that ever slept leaked its timer for the lifetime of the process -- and a
 * process that makes a thread per task leaks one handle per task. FlsAlloc's
 * callback runs at thread exit and closes it (verified: 20 of 20 threads).
 * The handle is set through the slot immediately after creation, which is what
 * arms that callback. Falls back to Sleep() if the slot cannot be allocated. */
static DWORD sdloP_sleep_slot = FLS_OUT_OF_INDEXES;
static INIT_ONCE sdloP_sleep_slot_once = INIT_ONCE_STATIC_INIT;

static VOID WINAPI sdloP_sleep_slot_free(PVOID param)
{
    if (param) {
        CloseHandle((HANDLE)param);
    }
}

static BOOL CALLBACK sdloP_sleep_slot_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)param;
    (void)ctx;
    sdloP_sleep_slot = FlsAlloc(sdloP_sleep_slot_free);
    return TRUE;
}

static HANDLE sdloP_sleep_timer_get(void)
{
    HANDLE h;
    InitOnceExecuteOnce(&sdloP_sleep_slot_once, sdloP_sleep_slot_init, NULL, NULL);
    if (sdloP_sleep_slot == FLS_OUT_OF_INDEXES) {
        return NULL;
    }
    h = (HANDLE)FlsGetValue(sdloP_sleep_slot);
    if (h) {
        return h;
    }
    /* AUTO-RESET: the wait must return the timer to non-signalled by itself,
     * or every later wait returns immediately and a caller looping on "sleep
     * until deadline" spins at 100% CPU. */
    h = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!h) {
        h = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    if (h && !FlsSetValue(sdloP_sleep_slot, h)) {
        /* no slot to own it, so no destructor would ever close it: report no
         * timer and let the caller Sleep(), rather than leaking one per call */
        CloseHandle(h);
        h = NULL;
    }
    return h;
}

/* Nothing to do: the timers belong to their threads and are closed by
 * sdloP_sleep_slot_free() as those threads exit. Kept so SDL_Quit has an
 * unconditional call site, symmetric with the POSIX side. */
void SDLOP_ReleaseTimerHandles(void)
{
    if (sdloP_sleep_slot != FLS_OUT_OF_INDEXES) {
        HANDLE h = (HANDLE)FlsGetValue(sdloP_sleep_slot);
        if (h) {
            FlsSetValue(sdloP_sleep_slot, NULL);
            CloseHandle(h);
        }
    }
}

static void sdloP_sleep_ns(Sint64 ns)
{
    if (ns <= 0) {
        return;
    }
    {
        HANDLE timer = sdloP_sleep_timer_get();
        if (timer) {
            /* The timer takes 100 ns units, negative = relative. */
            LARGE_INTEGER due;
            due.QuadPart = -(ns / 100);
            if (due.QuadPart == 0) {
                due.QuadPart = -1;
            }
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                return;
            }
        }
    }
    {
        const Uint64 ms = ((Uint64)ns + 999999ull) / 1000000ull;
        Sleep((DWORD)(ms > 0x7fffffffull ? 0x7fffffffull : ms));
    }
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
