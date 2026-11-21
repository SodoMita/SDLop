/*
  SDLop - Win32 primitive layer.

  SDLop's own subsystems (logging, hints, properties, the environment, the
  video core) need a small set of synchronisation and timing primitives.
  Rather than assume pthreads, they call through these wrappers; on POSIX the
  header includes <pthread.h> and the wrappers are macros over the pthread
  functions, so no call site changes on Linux and no behaviour changes either.
  On Windows the inline functions below use the Win32 primitives directly.

  Threads are deliberately *not* wrapped here: SDL_thread.c implements
  SDL_CreateThread(WithProperties) with platform-specific code in each branch,
  because Windows threads differ in join/detach/name/signal semantics.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_platform_h_
#define sdlop_platform_h_

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* POSIX: thin macros over pthreads                                    */
/* ------------------------------------------------------------------ */
#ifndef _WIN32

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

#define SDLOP_MUTEX_T          pthread_mutex_t
#define SDLOP_MUTEX_INIT       PTHREAD_MUTEX_INITIALIZER
#define SDLOP_MUTEX_RECURSIVE  PTHREAD_MUTEX_RECURSIVE
/* SDLop mutexes are RECURSIVE everywhere (SDL_LockMutex nests; see
 * test_core's worker). Windows gets that for free from CRITICAL_SECTION;
 * POSIX has to ask for it. */
#define SDLOP_MUTEX_INIT_ONCE(m) do { \
        pthread_mutexattr_t _a; \
        pthread_mutexattr_init(&_a); \
        pthread_mutexattr_settype(&_a, PTHREAD_MUTEX_RECURSIVE); \
        pthread_mutex_init((m), &_a); \
        pthread_mutexattr_destroy(&_a); \
    } while (0)
#define SDLOP_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#define SDLOP_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#define SDLOP_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)

#define SDLOP_ONCE_T           pthread_once_t
#define SDLOP_ONCE_INIT        PTHREAD_ONCE_INIT
#define SDLOP_ONCE(once, fn)   pthread_once((once), (fn))

#define SDLOP_RWLOCK_T         pthread_rwlock_t
#define SDLOP_RWLOCK_INIT      PTHREAD_RWLOCK_INITIALIZER
#define SDLOP_RWLOCK_INIT_ONCE(r) pthread_rwlock_init((r), NULL)
#define SDLOP_RWLOCK_DESTROY(r)   pthread_rwlock_destroy(r)
#define SDLOP_RWLOCK_RDLOCK(r)    pthread_rwlock_rdlock(r)
#define SDLOP_RWLOCK_WRLOCK(r)    pthread_rwlock_wrlock(r)
#define SDLOP_RWLOCK_RDLOCK_TRY(r) (pthread_rwlock_tryrdlock(r) == 0)
#define SDLOP_RWLOCK_WRLOCK_TRY(r) (pthread_rwlock_trywrlock(r) == 0)
#define SDLOP_RWLOCK_UNLOCK(r)    pthread_rwlock_unlock(r)

#define SDLOP_COND_T           pthread_cond_t
#define SDLOP_COND_INIT(c)     do { \
        pthread_condattr_t _a; \
        pthread_condattr_init(&_a); \
        pthread_condattr_setclock(&_a, CLOCK_MONOTONIC); \
        pthread_cond_init((c), &_a); \
        pthread_condattr_destroy(&_a); \
    } while (0)
#define SDLOP_COND_DESTROY(c)   pthread_cond_destroy(c)
#define SDLOP_COND_SIGNAL(c)    pthread_cond_signal(c)
#define SDLOP_COND_BROADCAST(c) pthread_cond_broadcast(c)
/* absolute CLOCK_MONOTONIC deadline */
#define SDLOP_COND_WAIT_UNTIL(c, m, ts) pthread_cond_timedwait((c), (m), (ts))
#define SDLOP_COND_WAIT(c, m)           pthread_cond_wait((c), (m))
#define SDLOP_COND_TIMEDOUT             ETIMEDOUT

#define SDLOP_SEM_T            sem_t
#define SDLOP_SEM_INIT(s, v)   (sem_init((s), 0, (v)) == 0)
#define SDLOP_SEM_DESTROY(s)   sem_destroy(s)
#define SDLOP_SEM_POST(s)      sem_post(s)
#define SDLOP_SEM_TRYWAIT(s)   (sem_trywait(s) == 0)
#define SDLOP_SEM_WAIT(s)      do { int _r; do { _r = sem_wait(s); } while (_r < 0 && errno == EINTR); } while (0)
#define SDLOP_SEM_WAIT_UNTIL(s, ts) (sem_clockwait((s), CLOCK_MONOTONIC, (ts)) == 0)
#define SDLOP_SEM_VALUE(s)     ({ int _v = 0; sem_getvalue((s), &_v); _v < 0 ? 0u : (Uint32)_v; })

#define SDLOP_YIELD()          sched_yield()
#define SDLOP_SLEEP_NS(ns)     do { \
        struct timespec _r; \
        _r.tv_sec = (time_t)((ns) / 1000000000ll); \
        _r.tv_nsec = (long)((ns) % 1000000000ll); \
        while (nanosleep(&_r, &_r) != 0 && errno == EINTR) { } \
    } while (0)

/* SDLOP_MonotonicNS() lives in SDLop_core.c on every platform */

#else /* _WIN32 */

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

#define SDLOP_MUTEX_T          CRITICAL_SECTION
#define SDLOP_MUTEX_RECURSIVE  1 /* CRITICAL_SECTION is always recursive */
#define SDLOP_MUTEX_INIT_ONCE(m) do { (void)0; InitializeCriticalSection(m); } while (0)
#define SDLOP_MUTEX_DESTROY(m)   DeleteCriticalSection(m)
#define SDLOP_MUTEX_LOCK(m)      EnterCriticalSection(m)
#define SDLOP_MUTEX_UNLOCK(m)    LeaveCriticalSection(m)

typedef INIT_ONCE SDLOP_ONCE_T;
#define SDLOP_ONCE_INIT        INIT_ONCE_STATIC_INIT
#define SDLOP_ONCE(once, fn)   do { \
        BOOL _b; \
        InitOnceExecuteOnce((once), SDLOP_OnceTrampoline, (PVOID)(fn), &_b); \
    } while (0)

#define SDLOP_RWLOCK_T         SRWLOCK
#define SDLOP_RWLOCK_INIT      SRWLOCK_INIT
#define SDLOP_RWLOCK_INIT_ONCE(r) do { (void)0; InitializeSRWLock(r); } while (0)
#define SDLOP_RWLOCK_DESTROY(r)   ((void)(r)) /* nothing to release */
#define SDLOP_RWLOCK_RDLOCK(r)    AcquireSRWLockShared(r)
#define SDLOP_RWLOCK_WRLOCK(r)    AcquireSRWLockExclusive(r)
#define SDLOP_RWLOCK_RDLOCK_TRY(r) (TryAcquireSRWLockShared(r) != 0)
#define SDLOP_RWLOCK_WRLOCK_TRY(r) (TryAcquireSRWLockExclusive(r) != 0)
/* SRW locks have no "unlock" that knows the mode; SDL's Windows backend
 * tracks it in its own wrapper. SDLop's readers release with _SHARED. */
/* SRW locks release shared and exclusive differently, but SDL exposes a
 * single SDL_UnlockRWLock(). The owner's mode is tracked in SDL_RWLock
 * (`rwmode`) so UNLOCK can dispatch; see SDL_thread.c. */
#define SDLOP_RWLOCK_RUNLOCK(r)   ReleaseSRWLockShared(r)
#define SDLOP_RWLOCK_WUNLOCK(r)   ReleaseSRWLockExclusive(r)

#define SDLOP_COND_T           CONDITION_VARIABLE
#define SDLOP_COND_INIT(c)     InitializeConditionVariable(c)
#define SDLOP_COND_DESTROY(c)  ((void)(c))
#define SDLOP_COND_SIGNAL(c)   WakeConditionVariable(c)
#define SDLOP_COND_BROADCAST(c) WakeAllConditionVariable(c)
#define SDLOP_COND_TIMEDOUT    ((int)ERROR_TIMEOUT)

/* absolute CLOCK_MONOTONIC deadline; see SDLOP_CondWaitUntil */
int SDLOP_CondWaitUntilImpl(CONDITION_VARIABLE *c, CRITICAL_SECTION *m, const struct timespec *ts);
#define SDLOP_COND_WAIT_UNTIL(c, m, ts) SDLOP_CondWaitUntilImpl((c), (m), (ts))
#define SDLOP_COND_WAIT(c, m) \
    (SleepConditionVariableCS((c), (m), INFINITE) ? 0 : (int)GetLastError())

/* counting semaphore: Win32 semaphores count down, SDLop's API counts up */
typedef struct SDLOP_Sem
{
    HANDLE handle;
} SDLOP_Sem;
#define SDLOP_SEM_T            SDLOP_Sem
#define SDLOP_SEM_INIT(s, v)   (((s)->handle = CreateSemaphoreA(NULL, (LONG)(v), 0x7fffffff, NULL)) != NULL)
#define SDLOP_SEM_DESTROY(s)   CloseHandle((s)->handle)
#define SDLOP_SEM_POST(s)      ReleaseSemaphore((s)->handle, 1, NULL)
#define SDLOP_SEM_TRYWAIT(s)   (WaitForSingleObject((s)->handle, 0) == WAIT_OBJECT_0)
#define SDLOP_SEM_WAIT(s)      WaitForSingleObject((s)->handle, INFINITE)

#define SDLOP_YIELD()          SwitchToThread()

int SDLOP_CondWaitUntilImpl(CONDITION_VARIABLE *cond, CRITICAL_SECTION *mutex,
                            const struct timespec *deadline);
BOOL CALLBACK SDLOP_OnceTrampoline(PINIT_ONCE once, PVOID param, PVOID *ctx);

#endif /* _WIN32 */

/* Platform-neutral helpers implemented in sdlop_platform.c */
unsigned int SDLOP_SemValue(SDLOP_SEM_T *sem);
void SDLOP_SemWaitUntil(SDLOP_SEM_T *sem, const struct timespec *deadline);
bool SDLOP_SemTimedWait(SDLOP_SEM_T *sem, int timeout_ms);
Sint64 SDLOP_MonotonicNowNS(void);
void SDLOP_DeadlineFromNS(struct timespec *ts, Sint64 deadline_ns);

#endif /* sdlop_platform_h_ */
