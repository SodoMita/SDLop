/*
  SDLop - the threading and clock primitives the core needs, in one place.

  SDLop is a POSIX program: mutexes are recursive, condition variables and
  semaphores wait on CLOCK_MONOTONIC deadlines, timestamps are monotonic
  nanoseconds. Windows supplies none of that, but it does supply close
  equivalents:

      recursive mutex      -> CRITICAL_SECTION   (always recursive)
      read/write lock      -> SRWLOCK
      condition variable   -> CONDITION_VARIABLE (+ SleepConditionVariableCS)
      counting semaphore   -> Win32 semaphore used inverted (see below)
      once                 -> INIT_ONCE
      thread-local with
      destructors          -> Fiber Local Storage
      monotonic clock      -> QueryPerformanceCounter

  Everything below is a macro or a small function so the call sites read the
  same on both platforms and there is exactly one place where the semantics
  are pinned down.

  Semaphores: SDL3's SDL_Semaphore counts *up* (SDL_SignalSemaphore increments
  SDL_GetSemaphoreValue). A Win32 semaphore counts *down* from its maximum, so
  SDLOP_SEM_T stores both: the Win32 handle it waits on and the count it
  reports. Keeping the count here rather than reading it from the handle
  avoids racing the waiters.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_platform_h_
#define sdlop_platform_h_

#include <SDL3/SDL.h>
#include "internal/sdlop_portable.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifndef _WIN32

/* ------------------------------------------------------------------ */
/* POSIX                                                               */
/* ------------------------------------------------------------------ */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#ifndef __APPLE__
#include <semaphore.h>
#endif

/* CLOCK_MONOTONIC is POSIX.1-2008 and present on every target SDLop
 * supports now; fall back to CLOCK_REALTIME where a libc is too old
 * to define it (the deadline arithmetic still works, it just moves
 * with the wall clock). */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC CLOCK_REALTIME
#endif
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

/* --- mutex ------------------------------------------------------- */

typedef pthread_mutex_t SDLOP_MUTEX_T;

#define SDLOP_MUTEX_INIT_ONCE(m) do {                                    \
        pthread_mutexattr_t _a;                                          \
        pthread_mutexattr_init(&_a);                                     \
        pthread_mutexattr_settype(&_a, PTHREAD_MUTEX_RECURSIVE);         \
        pthread_mutex_init((m), &_a);                                    \
        pthread_mutexattr_destroy(&_a);                                  \
    } while (0)
#define SDLOP_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#define SDLOP_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#define SDLOP_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)

/* Non-recursive is available too, for the few locks that never nest. */
#define SDLOP_MUTEX_INIT_PLAIN(m) pthread_mutex_init((m), NULL)
#define SDLOP_MUTEX_INIT_RECURSIVE(m) SDLOP_MUTEX_INIT_ONCE(m)

/* A mutex with static storage duration. Both platforms spell this the same
 * way at the call site; only the expansion differs.
 *
 * PTHREAD_RECURSIVE_MUTEX_INITIALIZER(_NP) is non-standard, and the plain
 * PTHREAD_MUTEX_INITIALIZER it would replace is NOT recursive. When the
 * platform header exposes a static recursive initializer, expand directly to
 * it with zero runtime cost; otherwise (BSDs/Solaris/Haiku/QNX/musl/strict
 * POSIX) an atomic once-state initializes PTHREAD_MUTEX_RECURSIVE on first
 * lock/unlock. */
#if defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP)
typedef struct SDLOP_StaticMutex
{
    pthread_mutex_t id;
} SDLOP_StaticMutex;
#define SDLOP_STATIC_MUTEX(name) SDLOP_StaticMutex name = { PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP }
#define SDLOP_SM(m)              (&(m)->id)
#elif defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER)
typedef struct SDLOP_StaticMutex
{
    pthread_mutex_t id;
} SDLOP_StaticMutex;
#define SDLOP_STATIC_MUTEX(name) SDLOP_StaticMutex name = { PTHREAD_RECURSIVE_MUTEX_INITIALIZER }
#define SDLOP_SM(m)              (&(m)->id)
#else
typedef struct SDLOP_StaticMutex
{
    pthread_mutex_t id;
    int state;
} SDLOP_StaticMutex;
static inline pthread_mutex_t *SDLOP_StaticMutexPtr(SDLOP_StaticMutex *m)
{
    if (__builtin_expect(__atomic_load_n(&m->state, __ATOMIC_ACQUIRE) != 2, 0)) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&m->state, &expected, 1, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            SDLOP_MUTEX_INIT_RECURSIVE(&m->id);
            __atomic_store_n(&m->state, 2, __ATOMIC_RELEASE);
        } else {
            while (__atomic_load_n(&m->state, __ATOMIC_ACQUIRE) != 2) {
                sched_yield();
            }
        }
    }
    return &m->id;
}
#define SDLOP_STATIC_MUTEX(name) SDLOP_StaticMutex name = { PTHREAD_MUTEX_INITIALIZER, 0 }
#define SDLOP_SM(m)              SDLOP_StaticMutexPtr(m)
#endif

/* --- once -------------------------------------------------------- */

typedef pthread_once_t SDLOP_ONCE_T;
#define SDLOP_ONCE_INIT        PTHREAD_ONCE_INIT
#define SDLOP_ONCE(once, fn)   pthread_once((once), (fn))

/* --- read/write lock --------------------------------------------- */

typedef pthread_rwlock_t SDLOP_RWLOCK_T;
#define SDLOP_RWLOCK_INIT_ONCE(r) pthread_rwlock_init((r), NULL)
#define SDLOP_RWLOCK_DESTROY(r)   pthread_rwlock_destroy(r)
#define SDLOP_RWLOCK_RDLOCK(r)    pthread_rwlock_rdlock(r)
#define SDLOP_RWLOCK_WRLOCK(r)    pthread_rwlock_wrlock(r)
#define SDLOP_RWLOCK_RDLOCK_TRY(r) (pthread_rwlock_tryrdlock(r) == 0)
#define SDLOP_RWLOCK_WRLOCK_TRY(r) (pthread_rwlock_trywrlock(r) == 0)
#define SDLOP_RWLOCK_UNLOCK(r)    pthread_rwlock_unlock(r)

/* --- condition variable ------------------------------------------ */

typedef pthread_cond_t SDLOP_COND_T;

/* Match SDL3 (SDL_syscond.c): use CLOCK_MONOTONIC when
 * pthread_condattr_setclock() is available, else CLOCK_REALTIME. */
#if defined(SDLOP_HAVE_PTHREAD_CONDATTR_SETCLOCK) && SDLOP_HAVE_PTHREAD_CONDATTR_SETCLOCK
#define SDLOP_COND_CLOCK CLOCK_MONOTONIC
#define SDLOP_COND_INIT(c) do {                                          \
        pthread_condattr_t _a;                                           \
        pthread_condattr_init(&_a);                                      \
        pthread_condattr_setclock(&_a, CLOCK_MONOTONIC);                 \
        pthread_cond_init((c), &_a);                                     \
        pthread_condattr_destroy(&_a);                                   \
    } while (0)
#else
#define SDLOP_COND_CLOCK CLOCK_REALTIME
#define SDLOP_COND_INIT(c) pthread_cond_init((c), NULL)
#endif
#define SDLOP_COND_DESTROY(c)          pthread_cond_destroy(c)
#define SDLOP_COND_SIGNAL(c)           pthread_cond_signal(c)
#define SDLOP_COND_BROADCAST(c)        pthread_cond_broadcast(c)
#define SDLOP_COND_WAIT(c, m)          pthread_cond_wait((c), (m))
#define SDLOP_COND_WAIT_UNTIL(c, m, ts) pthread_cond_timedwait((c), (m), (ts))
#define SDLOP_COND_TIMEDOUT            ETIMEDOUT

/* --- semaphore --------------------------------------------------- */

#ifdef __APPLE__
/* macOS deprecates sem_init()/sem_destroy() and returns ENOSYS for
 * sem_getvalue(). Match SDL3 (src/thread/pthread/SDL_syssem.c ->
 * generic/SDL_syssem.c) with a mutex + condvar semaphore. */
typedef struct SDLOP_SEM_T
{
    pthread_mutex_t count_lock;
    pthread_cond_t count_nonzero;
    Uint32 count;
    Uint32 waiters_count;
} SDLOP_SEM_T;

#define SDLOP_SEM_CLOCK SDLOP_COND_CLOCK

static inline bool SDLOP_SemInit(SDLOP_SEM_T *sem, Uint32 initial_value)
{
    if (pthread_mutex_init(&sem->count_lock, NULL) != 0) {
        return false;
    }
    SDLOP_COND_INIT(&sem->count_nonzero);
    sem->count = initial_value;
    sem->waiters_count = 0;
    return true;
}

static inline void SDLOP_SemDestroy(SDLOP_SEM_T *sem)
{
    sem->count = 0xFFFFFFFFu;
    while (sem->waiters_count > 0) {
        pthread_cond_signal(&sem->count_nonzero);
        sched_yield();
    }
    pthread_cond_destroy(&sem->count_nonzero);
    pthread_mutex_destroy(&sem->count_lock);
}

static inline void SDLOP_SemPost(SDLOP_SEM_T *sem)
{
    pthread_mutex_lock(&sem->count_lock);
    if (sem->waiters_count > 0) {
        pthread_cond_signal(&sem->count_nonzero);
    }
    ++sem->count;
    pthread_mutex_unlock(&sem->count_lock);
}

static inline bool SDLOP_SemTryWait(SDLOP_SEM_T *sem)
{
    bool ok = false;
    pthread_mutex_lock(&sem->count_lock);
    if (sem->count > 0) {
        --sem->count;
        ok = true;
    }
    pthread_mutex_unlock(&sem->count_lock);
    return ok;
}

static inline bool SDLOP_SemWait(SDLOP_SEM_T *sem)
{
    pthread_mutex_lock(&sem->count_lock);
    ++sem->waiters_count;
    while (sem->count == 0) {
        pthread_cond_wait(&sem->count_nonzero, &sem->count_lock);
    }
    --sem->waiters_count;
    --sem->count;
    pthread_mutex_unlock(&sem->count_lock);
    return true;
}

static inline bool SDLOP_SemWaitUntil(SDLOP_SEM_T *sem, const struct timespec *deadline)
{
    bool ok = false;
    pthread_mutex_lock(&sem->count_lock);
    ++sem->waiters_count;
    while (sem->count == 0) {
        int rc = pthread_cond_timedwait(&sem->count_nonzero, &sem->count_lock, deadline);
        if (rc == EINTR) {
            continue;
        }
        if (rc != 0) {
            break;
        }
    }
    --sem->waiters_count;
    if (sem->count > 0) {
        --sem->count;
        ok = true;
    }
    pthread_mutex_unlock(&sem->count_lock);
    return ok;
}

static inline Uint32 SDLOP_SemValue(SDLOP_SEM_T *sem)
{
    pthread_mutex_lock(&sem->count_lock);
    const Uint32 v = sem->count;
    pthread_mutex_unlock(&sem->count_lock);
    return v;
}

#define SDLOP_SEM_INIT(s, v)  SDLOP_SemInit((s), (Uint32)(v))
#define SDLOP_SEM_DESTROY(s)  SDLOP_SemDestroy(s)
#define SDLOP_SEM_POST(s)     SDLOP_SemPost(s)
#define SDLOP_SEM_TRYWAIT(s)  SDLOP_SemTryWait(s)
#define SDLOP_SEM_WAIT(s)     SDLOP_SemWait(s)

#else /* !__APPLE__ */

typedef sem_t SDLOP_SEM_T;
#define SDLOP_SEM_INIT(s, v)  (sem_init((s), 0, (unsigned)(v)) == 0)
#define SDLOP_SEM_DESTROY(s)  sem_destroy(s)
#define SDLOP_SEM_POST(s)     sem_post(s)
#define SDLOP_SEM_TRYWAIT(s)  (sem_trywait(s) == 0)

static inline bool SDLOP_SemWait(sem_t *sem)
{
    int rc;
    do {
        rc = sem_wait(sem);
    } while (rc != 0 && errno == EINTR);
    return rc == 0;
}
#define SDLOP_SEM_WAIT(s) SDLOP_SemWait(s)

/* Match SDL3 (SDL_syssem.c): CLOCK_MONOTONIC with sem_clockwait /
 * sem_clockwait_np, else CLOCK_REALTIME with sem_timedwait. */
#if defined(SDLOP_HAVE_SEM_CLOCKWAIT) && SDLOP_HAVE_SEM_CLOCKWAIT
#define SDLOP_SEM_CLOCK CLOCK_MONOTONIC
static inline bool SDLOP_SemWaitUntil(sem_t *sem, const struct timespec *deadline)
{
    int rc;
    do {
        rc = sem_clockwait(sem, CLOCK_MONOTONIC, deadline);
    } while (rc != 0 && errno == EINTR);
    return rc == 0;
}
#elif defined(SDLOP_HAVE_SEM_CLOCKWAIT_NP) && SDLOP_HAVE_SEM_CLOCKWAIT_NP
#define SDLOP_SEM_CLOCK CLOCK_MONOTONIC
static inline bool SDLOP_SemWaitUntil(sem_t *sem, const struct timespec *deadline)
{
    int rc;
    do {
        rc = sem_clockwait_np(sem, CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
    } while (rc != 0 && errno == EINTR);
    return rc == 0;
}
#elif defined(SDLOP_HAVE_SEM_TIMEDWAIT) && SDLOP_HAVE_SEM_TIMEDWAIT
#define SDLOP_SEM_CLOCK CLOCK_REALTIME
static inline bool SDLOP_SemWaitUntil(sem_t *sem, const struct timespec *deadline)
{
    int rc;
    do {
        rc = sem_timedwait(sem, deadline);
    } while (rc != 0 && errno == EINTR);
    return rc == 0;
}
#else
#define SDLOP_SEM_CLOCK CLOCK_MONOTONIC
static inline bool SDLOP_SemWaitUntil(sem_t *sem, const struct timespec *deadline)
{
    const Sint64 stop_ns = (Sint64)deadline->tv_sec * 1000000000ll + (Sint64)deadline->tv_nsec;
    struct timespec now;
    while (sem_trywait(sem) != 0) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((Sint64)now.tv_sec * 1000000000ll + (Sint64)now.tv_nsec >= stop_ns) {
            return false;
        }
        struct timespec req = { 0, 100000 };
        nanosleep(&req, NULL);
    }
    return true;
}
#endif

static inline Uint32 SDLOP_SemValue(sem_t *sem)
{
    int v = 0;
    sem_getvalue(sem, &v);
    return v < 0 ? 0u : (Uint32)v;
}

#endif /* __APPLE__ */

/* --- sleep / yield ----------------------------------------------- */

/* Relative sleep, restarted after a signal. */
#define SDLOP_SLEEP_NS(ns) do {                                          \
        struct timespec _r, _rem;                                        \
        _r.tv_sec = (time_t)((ns) / 1000000000ull);                      \
        _r.tv_nsec = (long)((ns) % 1000000000ull);                       \
        while (nanosleep(&_r, &_rem) != 0 && errno == EINTR) {           \
            _r = _rem;                                                   \
        }                                                                \
    } while (0)

/* Sleep until an absolute CLOCK_MONOTONIC deadline. */
#if defined(SDLOP_HAVE_CLOCK_NANOSLEEP) && SDLOP_HAVE_CLOCK_NANOSLEEP
#define SDLOP_SLEEP_UNTIL_NS(abs_ns) do {                                \
        struct timespec _ts;                                             \
        _ts.tv_sec = (time_t)((abs_ns) / 1000000000ull);                 \
        _ts.tv_nsec = (long)((abs_ns) % 1000000000ull);                  \
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &_ts,     \
                               NULL) == EINTR) {                         \
        }                                                                \
    } while (0)
#else
#define SDLOP_SLEEP_UNTIL_NS(abs_ns) do {                                \
        struct timespec _now_ts;                                         \
        clock_gettime(CLOCK_MONOTONIC, &_now_ts);                        \
        const Uint64 _now_ns = (Uint64)_now_ts.tv_sec * 1000000000ull +  \
                               (Uint64)_now_ts.tv_nsec;                  \
        if ((Uint64)(abs_ns) > _now_ns) {                                \
            const Uint64 _delta = (Uint64)(abs_ns) - _now_ns;            \
            SDLOP_SLEEP_NS(_delta);                                      \
        }                                                                \
    } while (0)
#endif

#define SDLOP_YIELD() sched_yield()

/* --- thread id from a handle ------------------------------------- */

/* Only used to notice "the caller IS the timer thread" before joining it, and
 * pthread_t compares equal to itself, so this identity mapping is enough. */
#define SDLOP_ThreadIDOfHandle(h) ((SDL_ThreadID)(h))

#else /* _WIN32 */

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* Windows 7: SRW locks, condition variables */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

/* --- mutex ------------------------------------------------------- */

typedef CRITICAL_SECTION SDLOP_MUTEX_T;

/* CRITICAL_SECTION is recursive, which is what SDL_LockMutex needs. */
#define SDLOP_MUTEX_INIT_ONCE(m) do { (void)0; InitializeCriticalSection(m); } while (0)
#define SDLOP_MUTEX_DESTROY(m)   ((void)0, DeleteCriticalSection(m))
#define SDLOP_MUTEX_LOCK(m)      EnterCriticalSection(m)
#define SDLOP_MUTEX_UNLOCK(m)    LeaveCriticalSection(m)

#define SDLOP_MUTEX_INIT_PLAIN(m)     SDLOP_MUTEX_INIT_ONCE(m)
#define SDLOP_MUTEX_INIT_RECURSIVE(m) SDLOP_MUTEX_INIT_ONCE(m)

BOOL CALLBACK SDLOP_OnceTrampoline(PINIT_ONCE once, PVOID param, PVOID *ctx);

typedef struct SDLOP_StaticMutex
{
    CRITICAL_SECTION id;
    INIT_ONCE once;
} SDLOP_StaticMutex;

BOOL CALLBACK SDLOP_StaticMutexInitTrampoline(PINIT_ONCE once, PVOID param, PVOID *ctx);

#define SDLOP_STATIC_MUTEX(name) SDLOP_StaticMutex name = { { 0 }, INIT_ONCE_STATIC_INIT }

static inline SDLOP_MUTEX_T *SDLOP_StaticMutexGet(SDLOP_StaticMutex *m)
{
    InitOnceExecuteOnce(&m->once, SDLOP_StaticMutexInitTrampoline, m, NULL);
    return &m->id;
}
#define SDLOP_SM(m) SDLOP_StaticMutexGet(m)

/* --- once -------------------------------------------------------- */

typedef INIT_ONCE SDLOP_ONCE_T;
#define SDLOP_ONCE_INIT        INIT_ONCE_STATIC_INIT
#define SDLOP_ONCE(once, fn)   do {                                      \
        InitOnceExecuteOnce((once), SDLOP_OnceTrampoline, (PVOID)(fn), NULL); \
    } while (0)

/* --- read/write lock --------------------------------------------- */

/* SRWLOCK releases shared and exclusive locks through different calls, while
 * SDL exposes one SDL_UnlockRWLock(); SDL_RWLock remembers which it took. */
typedef SRWLOCK SDLOP_RWLOCK_T;
#define SDLOP_RWLOCK_INIT_ONCE(r)  ((void)InitializeSRWLock(r))
#define SDLOP_RWLOCK_DESTROY(r)    ((void)(r))
#define SDLOP_RWLOCK_RDLOCK(r)     AcquireSRWLockShared(r)
#define SDLOP_RWLOCK_WRLOCK(r)     AcquireSRWLockExclusive(r)
#define SDLOP_RWLOCK_RDLOCK_TRY(r) (TryAcquireSRWLockShared(r) != 0)
#define SDLOP_RWLOCK_WRLOCK_TRY(r) (TryAcquireSRWLockExclusive(r) != 0)
#define SDLOP_RWLOCK_RUNLOCK(r)    ReleaseSRWLockShared(r)
#define SDLOP_RWLOCK_WUNLOCK(r)    ReleaseSRWLockExclusive(r)

/* --- condition variable ------------------------------------------ */

typedef CONDITION_VARIABLE SDLOP_COND_T;

#define SDLOP_COND_INIT(c)      ((void)InitializeConditionVariable(c))
#define SDLOP_COND_DESTROY(c)   ((void)(c))
#define SDLOP_COND_SIGNAL(c)    WakeConditionVariable(c)
#define SDLOP_COND_BROADCAST(c) WakeAllConditionVariable(c)
#define SDLOP_COND_WAIT(c, m)   (SleepConditionVariableCS((c), (m), INFINITE) ? 0 : (int)GetLastError())

/* Absolute CLOCK_MONOTONIC deadline, converted to a relative wait; see
 * SDLOP_CondWaitUntilImpl. */
int SDLOP_CondWaitUntilImpl(CONDITION_VARIABLE *cond, CRITICAL_SECTION *mutex,
                            const struct timespec *deadline);
#define SDLOP_COND_WAIT_UNTIL(c, m, ts) SDLOP_CondWaitUntilImpl((c), (m), (ts))
#define SDLOP_COND_TIMEDOUT    ((int)ERROR_TIMEOUT)

/* --- semaphore --------------------------------------------------- */

/* The Win32 handle's count is the SDL-side count (see SDLOP_SemInit);
 * `count` mirrors it for SDL_GetSemaphoreValue() so polling never has to
 * disturb a waiter. */
typedef struct SDLOP_SEM_T
{
    HANDLE handle;
    volatile LONG count;
} SDLOP_SEM_T;

#define SDLOP_SEM_MAX 0x7fffffff

bool SDLOP_SemInit(SDLOP_SEM_T *sem, Uint32 initial_value);
void SDLOP_SemDestroy(SDLOP_SEM_T *sem);
void SDLOP_SemPost(SDLOP_SEM_T *sem);
bool SDLOP_SemTryWait(SDLOP_SEM_T *sem);
bool SDLOP_SemWait(SDLOP_SEM_T *sem);
Uint32 SDLOP_SemValue(SDLOP_SEM_T *sem);
bool SDLOP_SemWaitUntil(SDLOP_SEM_T *sem, const struct timespec *deadline);

#define SDLOP_SEM_INIT(s, v)     SDLOP_SemInit((s), (Uint32)(v))
#define SDLOP_SEM_DESTROY(s)     SDLOP_SemDestroy(s)
#define SDLOP_SEM_POST(s)        SDLOP_SemPost(s)
#define SDLOP_SEM_TRYWAIT(s)     SDLOP_SemTryWait(s)
#define SDLOP_SEM_WAIT(s)        SDLOP_SemWait(s)
#define SDLOP_SEM_CLOCK          0
#define SDLOP_COND_CLOCK         0

/* --- sleep / yield ----------------------------------------------- */

/* Sleep() is millisecond-granular and, at the default timer resolution,
 * rounds up to the 15.6 ms tick: SDL_Delay(1) sleeps 15.6 ms and any frame
 * limiter built on it runs at ~64 Hz. Stock SDL3 sleeps on a high-resolution
 * waitable timer, so do the same, with Sleep() as the fallback for systems
 * without CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+ has it).
 * SDLOP_SleepNS() owns the timer; see sdlop_platform.c. */
#define SDLOP_SLEEP_NS(ns) SDLOP_SleepNS((Sint64)(ns))

/* Absolute CLOCK_MONOTONIC deadline: sleep the difference, leave the last
 * 200 us to the caller's spin loop. */
#define SDLOP_SLEEP_UNTIL_NS(abs_ns) SDLOP_SleepUntilNS((Sint64)(abs_ns))

void SDLOP_SleepUntilNS(Sint64 abs_ns);
void SDLOP_SleepNS(Sint64 ns);

#define SDLOP_YIELD() SwitchToThread()

/* --- thread id from a handle ------------------------------------- */

Uint32 SDLOP_ThreadIDOfHandle(HANDLE h);

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* Shared                                                              */
/* ------------------------------------------------------------------ */

/* Monotonic nanoseconds; the same clock SDL_GetPerformanceCounter() reports.
 * Implemented in sdlop_platform.c on both platforms. */
Sint64 SDLOP_MonotonicNowNS(void);

/* Split a monotonic deadline into a struct timespec (the absolute-deadline
 * form the condition/semaphore waits take). */
void SDLOP_DeadlineFromNS(struct timespec *ts, Sint64 deadline_ns);

#endif /* sdlop_platform_h_ */
