/*
  SDLop - Win32 side of sdlop_platform.h.

  The POSIX half of that header is macros over pthreads; this file supplies the
  Windows equivalents and the small platform-neutral helpers both platforms
  share. Semaphores live here rather than in the header because SDLop's
  SDL_Semaphore counts *up* while the Win32 semaphore counts down.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#ifdef _WIN32

#include "internal/sdlop_platform.h"

static BOOL CALLBACK SDLOP_OnceTrampoline(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)ctx;
    ((void (*)(void))param)();
    return TRUE;
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

Sint64 SDLOP_MonotonicNowNS(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static LONGLONG start;
    LARGE_INTEGER now;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter((LARGE_INTEGER *)&start);
    }
    QueryPerformanceCounter(&now);
    const LONGLONG ticks = now.QuadPart - start;
    return (Sint64)(ticks / freq.QuadPart) * 1000000000ll +
           (Sint64)((ticks % freq.QuadPart) * 1000000000ll / freq.QuadPart);
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

Uint32 SDLOP_SemValue(SDLOP_SEM_T *sem)
{
#ifdef _WIN32
    /* Win32 has no read-only accessor; SDLop only reports this for
     * SDL_GetSemaphoreValue(), where a transient 0 is acceptable. */
    (void)sem;
    return 0;
#else
    int v = 0;
    sem_getvalue(sem, &v);
    return v < 0 ? 0u : (Uint32)v;
#endif
}

void SDLOP_SemWaitUntil(SDLOP_SEM_T *sem, const struct timespec *deadline)
{
#ifdef _WIN32
    const Sint64 want = (Sint64)deadline->tv_sec * 1000000000ll + deadline->tv_nsec;
    for (;;) {
        const Sint64 left = want - SDLOP_MonotonicNowNS();
        if (left <= 0) {
            return;
        }
        const uint64_t ms = ((uint64_t)left + 999999ull) / 1000000ull;
        if (WaitForSingleObject(sem->handle, (DWORD)ms) == WAIT_OBJECT_0) {
            return;
        }
    }
#else
    while (sem_clockwait(sem, CLOCK_MONOTONIC, deadline) != 0 && errno == EINTR) {
    }
#endif
}

bool SDLOP_SemTimedWait(SDLOP_SEM_T *sem, int timeout_ms)
{
    if (timeout_ms == 0) {
#ifdef _WIN32
        return WaitForSingleObject(sem->handle, 0) == WAIT_OBJECT_0;
#else
        return sem_trywait(sem) == 0;
#endif
    }
    if (timeout_ms < 0) {
#ifdef _WIN32
        return WaitForSingleObject(sem->handle, INFINITE) == WAIT_OBJECT_0;
#else
        int rc;
        do {
            rc = sem_wait(sem);
        } while (rc < 0 && errno == EINTR);
        return rc == 0;
#endif
    }
    const Sint64 deadline = SDLOP_MonotonicNowNS() + (Sint64)timeout_ms * 1000000ll;
#ifdef _WIN32
    for (;;) {
        const Sint64 left = deadline - SDLOP_MonotonicNowNS();
        if (left <= 0) {
            return false;
        }
        const DWORD ms = (DWORD)((left + 999999ll) / 1000000ll);
        if (WaitForSingleObject(sem->handle, ms) == WAIT_OBJECT_0) {
            return true;
        }
        if (SDLOP_MonotonicNowNS() >= deadline) {
            return false;
        }
    }
#else
    struct timespec ts;
    SDLOP_DeadlineFromNS(&ts, deadline);
    int rc;
    do {
        rc = sem_clockwait(sem, CLOCK_MONOTONIC, &ts);
    } while (rc < 0 && errno == EINTR);
    return rc == 0;
#endif
}

#ifdef _WIN32

int SDLOP_CondWaitUntilImpl(CONDITION_VARIABLE *cond, CRITICAL_SECTION *mutex,
                            const struct timespec *deadline)
{
    const Sint64 want = (Sint64)deadline->tv_sec * 1000000000ll + deadline->tv_nsec;
    for (;;) {
        const Sint64 left = want - SDLOP_MonotonicNowNS();
        if (left <= 0) {
            return SDLOP_COND_TIMEDOUT;
        }
        const DWORD ms = (DWORD)((left + 999999ll) / 1000000ll);
        if (!SleepConditionVariableCS(cond, mutex, ms)) {
            const DWORD err = GetLastError();
            if (err == ERROR_TIMEOUT) {
                if (want - SDLOP_MonotonicNowNS() <= 0) {
                    return SDLOP_COND_TIMEDOUT;
                }
                continue;
            }
            return (int)err;
        }
        return 0;
    }
}

#endif /* _WIN32 */
