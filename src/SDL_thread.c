/*
  SDLop - atomics, mutexes, threads and thread-local storage
  (SDL_atomic.h, SDL_mutex.h, SDL_thread.h).

  Thin POSIX/C11 implementations with SDL 3.4.20 semantics (ported from
  SDL3's src/thread/pthread and src/atomic):
    - SDL_Mutex is recursive; every lock/unlock accepts NULL as a no-op;
    - timeouts are in milliseconds, < 0 waits forever, 0 polls;
    - SDL_SetTLS() IDs are allocated lazily from a global counter and
      destructors run when the thread exits (any thread, not only SDL ones)
      or on SDL_CleanupTLS();
    - SDL_SetCurrentThreadPriority() maps to nice values (19/0/-10/-20) or
      SCHED_RR when SDL_HINT_THREAD_FORCE_REALTIME_TIME_CRITICAL is set.
      There is no RealtimeKit fallback; the SDL_system.h Linux priority
      entry points use setpriority() / sched_setscheduler() directly.

  Copyright (C) 1997-2026 Sam Lantinga (SDL3 parts), zlib license.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "internal/sdlop_internal.h"
#include <errno.h>
#include <stdatomic.h>
#include <time.h>

#ifndef _WIN32
#include <pthread.h>
#if defined(SDLOP_HAVE_PTHREAD_NP_H) && SDLOP_HAVE_PTHREAD_NP_H
#include <pthread_np.h>
#endif
#if defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)
#include <dlfcn.h>
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT NULL
#endif
#endif
#ifdef __HAIKU__
#include <kernel/OS.h>
#endif
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Atomics                                                             */
/* ------------------------------------------------------------------ */

/* SDL_AtomicInt/U32 are { int value; } / { Uint32 value; }: operate on
 * the member through the GCC/Clang __atomic builtins (seq_cst, like SDL3) */

bool SDL_CompareAndSwapAtomicInt(SDL_AtomicInt *a, int oldval, int newval)
{
    return __atomic_compare_exchange_n(&a->value, &oldval, newval, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

int SDL_SetAtomicInt(SDL_AtomicInt *a, int v)
{
    return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST);
}

int SDL_GetAtomicInt(SDL_AtomicInt *a)
{
    return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST);
}

int SDL_AddAtomicInt(SDL_AtomicInt *a, int v)
{
    return __atomic_fetch_add(&a->value, v, __ATOMIC_SEQ_CST);
}

bool SDL_CompareAndSwapAtomicU32(SDL_AtomicU32 *a, Uint32 oldval, Uint32 newval)
{
    return __atomic_compare_exchange_n(&a->value, &oldval, newval, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

Uint32 SDL_SetAtomicU32(SDL_AtomicU32 *a, Uint32 v)
{
    return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST);
}

Uint32 SDL_GetAtomicU32(SDL_AtomicU32 *a)
{
    return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST);
}

Uint32 SDL_AddAtomicU32(SDL_AtomicU32 *a, int v)
{
    return __atomic_fetch_add(&a->value, (Uint32)v, __ATOMIC_SEQ_CST);
}

bool SDL_CompareAndSwapAtomicPointer(void **a, void *oldval, void *newval)
{
    return __atomic_compare_exchange_n(a, &oldval, newval, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

void *SDL_SetAtomicPointer(void **a, void *v)
{
    return __atomic_exchange_n(a, v, __ATOMIC_SEQ_CST);
}

void *SDL_GetAtomicPointer(void **a)
{
    return __atomic_load_n(a, __ATOMIC_SEQ_CST);
}

void SDL_MemoryBarrierReleaseFunction(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

void SDL_MemoryBarrierAcquireFunction(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

bool SDL_TryLockSpinlock(SDL_SpinLock *lock)
{
    return __atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) == 0;
}

void SDL_LockSpinlock(SDL_SpinLock *lock)
{
    int iterations = 0;
    while (!SDL_TryLockSpinlock(lock)) {
        if (iterations < 32) {
            iterations++;
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        } else {
            SDLOP_YIELD(); /* give the owner a chance to run */
        }
    }
}

void SDL_UnlockSpinlock(SDL_SpinLock *lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ */
/* Mutex / RW lock / semaphore / condition                             */
/* ------------------------------------------------------------------ */

struct SDL_Mutex
{
    SDLOP_MUTEX_T id;
    bool initialized;
};

struct SDL_RWLock
{
    SDLOP_RWLOCK_T id;
    bool initialized;
#ifdef _WIN32
    /* SDL_UnlockRWLock() has no mode argument and SRW locks do: only the
     * owner can unlock, so the owner's mode is tracked here. Recursive
     * read-locking from one thread is not supported by SRWLOCK anyway
     * (same restriction as pthread_rwlock under POSIX). */
    volatile LONG rwmode; /* 0 = free, 1 = shared, 2 = exclusive */
#endif
};

struct SDL_Semaphore
{
    SDLOP_SEM_T sem;
};

struct SDL_Condition
{
    SDLOP_COND_T cond;
};

SDL_Mutex *SDL_CreateMutex(void)
{
    SDL_Mutex *mutex = (SDL_Mutex *)SDL_calloc(1, sizeof(*mutex));
    if (!mutex) {
        return NULL;
    }
    SDLOP_MUTEX_INIT_ONCE(&mutex->id);
    mutex->initialized = true;
    return mutex;
}

void SDL_DestroyMutex(SDL_Mutex *mutex)
{
    if (mutex) {
        if (mutex->initialized) {
            SDLOP_MUTEX_DESTROY(&mutex->id);
        }
        SDL_free(mutex);
    }
}

void SDL_LockMutex(SDL_Mutex *mutex)
{
    if (mutex) {
        SDLOP_MUTEX_LOCK(&mutex->id);
    }
}

bool SDL_TryLockMutex(SDL_Mutex *mutex)
{
    if (!mutex) {
        return true;
    }
#ifdef _WIN32
    return TryEnterCriticalSection(&mutex->id) != 0;
#else
    return pthread_mutex_trylock(&mutex->id) == 0;
#endif
}

void SDL_UnlockMutex(SDL_Mutex *mutex)
{
    if (mutex) {
        SDLOP_MUTEX_UNLOCK(&mutex->id);
    }
}

SDL_RWLock *SDL_CreateRWLock(void)
{
    SDL_RWLock *rw = (SDL_RWLock *)SDL_calloc(1, sizeof(*rw));
    if (!rw) {
        return NULL;
    }
    SDLOP_RWLOCK_INIT_ONCE(&rw->id);
    rw->initialized = true;
    return rw;
}

void SDL_DestroyRWLock(SDL_RWLock *rwlock)
{
    if (rwlock) {
        if (rwlock->initialized) {
            SDLOP_RWLOCK_DESTROY(&rwlock->id);
        }
        SDL_free(rwlock);
    }
}

void SDL_LockRWLockForReading(SDL_RWLock *rwlock)
{
    if (rwlock) {
        SDLOP_RWLOCK_RDLOCK(&rwlock->id);
#ifdef _WIN32
        rwlock->rwmode = 1;
#endif
    }
}

void SDL_LockRWLockForWriting(SDL_RWLock *rwlock)
{
    if (rwlock) {
        SDLOP_RWLOCK_WRLOCK(&rwlock->id);
#ifdef _WIN32
        rwlock->rwmode = 2;
#endif
    }
}

bool SDL_TryLockRWLockForReading(SDL_RWLock *rwlock)
{
    if (!rwlock) {
        return true;
    }
    if (!SDLOP_RWLOCK_RDLOCK_TRY(&rwlock->id)) {
        return false;
    }
#ifdef _WIN32
    rwlock->rwmode = 1;
#endif
    return true;
}

bool SDL_TryLockRWLockForWriting(SDL_RWLock *rwlock)
{
    if (!rwlock) {
        return true;
    }
    if (!SDLOP_RWLOCK_WRLOCK_TRY(&rwlock->id)) {
        return false;
    }
#ifdef _WIN32
    rwlock->rwmode = 2;
#endif
    return true;
}

void SDL_UnlockRWLock(SDL_RWLock *rwlock)
{
    if (rwlock) {
#ifdef _WIN32
        if (rwlock->rwmode == 2) {
            SDLOP_RWLOCK_WUNLOCK(&rwlock->id);
        } else {
            SDLOP_RWLOCK_RUNLOCK(&rwlock->id);
        }
        rwlock->rwmode = 0;
#else
        SDLOP_RWLOCK_UNLOCK(&rwlock->id);
#endif
    }
}

SDL_Semaphore *SDL_CreateSemaphore(Uint32 initial_value)
{
    SDL_Semaphore *sem = (SDL_Semaphore *)SDL_malloc(sizeof(*sem));
    if (!sem) {
        return NULL;
    }
    if (!SDLOP_SEM_INIT(&sem->sem, initial_value)) {
        SDL_SetError("semaphore creation failed");
        SDL_free(sem);
        return NULL;
    }
    return sem;
}

void SDL_DestroySemaphore(SDL_Semaphore *sem)
{
    if (sem) {
        SDLOP_SEM_DESTROY(&sem->sem);
        SDL_free(sem);
    }
}

/* Absolute deadline `timeout_ns` from now, expressed on the platform's
 * monotonic clock (clock_gettime on POSIX, QueryPerformanceCounter on
 * Windows, which is why it goes through SDLOP_MonotonicNowNS()). */
static void abs_timeout_clock(Sint64 timeout_ns, struct timespec *ts, int clk)
{
#ifdef _WIN32
    (void)clk;
    SDLOP_DeadlineFromNS(ts, SDLOP_MonotonicNowNS() + timeout_ns);
#else
    if (clk == CLOCK_MONOTONIC) {
        SDLOP_DeadlineFromNS(ts, SDLOP_MonotonicNowNS() + timeout_ns);
        return;
    }
    struct timespec now;
    clock_gettime(clk, &now);
    Sint64 nsec = (Sint64)now.tv_nsec + (timeout_ns % 1000000000ll);
    ts->tv_sec = now.tv_sec + (time_t)(timeout_ns / 1000000000ll) + (time_t)(nsec / 1000000000ll);
    ts->tv_nsec = (long)(nsec % 1000000000ll);
#endif
}

bool SDL_WaitSemaphoreTimeout(SDL_Semaphore *sem, Sint32 timeoutMS)
{
    if (!sem) {
        return true;
    }
    if (timeoutMS == 0) {
        return SDLOP_SEM_TRYWAIT(&sem->sem);
    }
    if (timeoutMS < 0) {
        return SDLOP_SEM_WAIT(&sem->sem);
    }
    struct timespec ts;
    abs_timeout_clock((Sint64)timeoutMS * 1000000ll, &ts, SDLOP_SEM_CLOCK);
    return SDLOP_SemWaitUntil(&sem->sem, &ts);
}

void SDL_WaitSemaphore(SDL_Semaphore *sem)
{
    SDL_WaitSemaphoreTimeout(sem, -1);
}

bool SDL_TryWaitSemaphore(SDL_Semaphore *sem)
{
    return SDL_WaitSemaphoreTimeout(sem, 0);
}

void SDL_SignalSemaphore(SDL_Semaphore *sem)
{
    if (sem) {
        SDLOP_SEM_POST(&sem->sem);
    }
}

Uint32 SDL_GetSemaphoreValue(SDL_Semaphore *sem)
{
    return sem ? SDLOP_SemValue(&sem->sem) : 0u;
}

SDL_Condition *SDL_CreateCondition(void)
{
    SDL_Condition *cond = (SDL_Condition *)SDL_calloc(1, sizeof(*cond));
    if (!cond) {
        return NULL;
    }
    SDLOP_COND_INIT(&cond->cond);
    return cond;
}

void SDL_DestroyCondition(SDL_Condition *cond)
{
    if (cond) {
        SDLOP_COND_DESTROY(&cond->cond);
        SDL_free(cond);
    }
}

void SDL_SignalCondition(SDL_Condition *cond)
{
    if (cond) {
        SDLOP_COND_SIGNAL(&cond->cond);
    }
}

void SDL_BroadcastCondition(SDL_Condition *cond)
{
    if (cond) {
        SDLOP_COND_BROADCAST(&cond->cond);
    }
}

bool SDL_WaitConditionTimeout(SDL_Condition *cond, SDL_Mutex *mutex, Sint32 timeoutMS)
{
    if (!cond || !mutex) {
        return true;
    }
    if (timeoutMS < 0) {
        return SDLOP_COND_WAIT(&cond->cond, &mutex->id) == 0;
    }
    struct timespec ts;
    abs_timeout_clock((Sint64)timeoutMS * 1000000ll, &ts, SDLOP_COND_CLOCK);
    int rc;
    do {
        rc = SDLOP_COND_WAIT_UNTIL(&cond->cond, &mutex->id, &ts);
    } while (rc == EINTR);
    return rc != SDLOP_COND_TIMEDOUT;
}

void SDL_WaitCondition(SDL_Condition *cond, SDL_Mutex *mutex)
{
    SDL_WaitConditionTimeout(cond, mutex, -1);
}

/* ------------------------------------------------------------------ */
/* SDL_InitState                                                       */
/* ------------------------------------------------------------------ */

bool SDL_ShouldInit(SDL_InitState *state)
{
    while (SDL_GetAtomicInt(&state->status) != SDL_INIT_STATUS_INITIALIZED) {
        if (SDL_CompareAndSwapAtomicInt(&state->status, SDL_INIT_STATUS_UNINITIALIZED, SDL_INIT_STATUS_INITIALIZING)) {
            state->thread = SDL_GetCurrentThreadID();
            return true;
        }
        SDL_Delay(1); /* another thread is mid-transition */
    }
    return false;
}

bool SDL_ShouldQuit(SDL_InitState *state)
{
    while (SDL_GetAtomicInt(&state->status) != SDL_INIT_STATUS_UNINITIALIZED) {
        if (SDL_CompareAndSwapAtomicInt(&state->status, SDL_INIT_STATUS_INITIALIZED, SDL_INIT_STATUS_UNINITIALIZING)) {
            state->thread = SDL_GetCurrentThreadID();
            return true;
        }
        SDL_Delay(1);
    }
    return false;
}

void SDL_SetInitialized(SDL_InitState *state, bool initialized)
{
    SDL_SetAtomicInt(&state->status, initialized ? SDL_INIT_STATUS_INITIALIZED : SDL_INIT_STATUS_UNINITIALIZED);
}

/* ------------------------------------------------------------------ */
/* Thread-local storage                                                */
/* ------------------------------------------------------------------ */

typedef struct TLSEntry
{
    void *data;
    SDL_TLSDestructorCallback destructor;
} TLSEntry;

typedef struct TLSData
{
    int limit;
    TLSEntry array[];
} TLSData;

#define TLS_ALLOC_CHUNKSIZE 4

static SDL_AtomicInt tls_next_id;

/* Thread-local storage for SDL_SetTLS(). POSIX uses a pthread key with a
 * destructor; Windows uses Fiber Local Storage, whose callback has the same
 * shape (FlsAlloc with a destructor) — Get/SetTlsValue do not. */
#ifdef _WIN32
typedef DWORD sdlop_tls_key;
static sdlop_tls_key tls_key;
static INIT_ONCE tls_once = INIT_ONCE_STATIC_INIT;
static void tls_thread_exit(void *ptr);
static void tls_init(void)
{
    tls_key = FlsAlloc(tls_thread_exit);
}
#else
typedef pthread_key_t sdlop_tls_key;
static sdlop_tls_key tls_key;
static pthread_once_t tls_once = PTHREAD_ONCE_INIT;
static void tls_thread_exit(void *ptr);
static void tls_init(void)
{
    pthread_key_create(&tls_key, tls_thread_exit);
}
#endif

static void tls_ensure_init(void)
{
#ifdef _WIN32
    InitOnceExecuteOnce(&tls_once, SDLOP_OnceTrampoline, (PVOID)tls_init, NULL);
#else
    pthread_once(&tls_once, tls_init);
#endif
}

static void *tls_get(void)
{
#ifdef _WIN32
    return FlsGetValue(tls_key);
#else
    return pthread_getspecific(tls_key);
#endif
}

static int tls_set(void *value)
{
#ifdef _WIN32
    return FlsSetValue(tls_key, value) ? 0 : (int)GetLastError();
#else
    return pthread_setspecific(tls_key, value);
#endif
}

static void tls_run_destructors(TLSData *storage)
{
    /* destructors may set TLS again: loop like pthreads does */
    for (int round = 0; storage && round < 4; round++) {
        bool any = false;
        for (int i = 0; i < storage->limit; ++i) {
            TLSEntry *e = &storage->array[i];
            if (e->destructor && e->data) {
                void *data = e->data;
                SDL_TLSDestructorCallback dtor = e->destructor;
                e->data = NULL;
                e->destructor = NULL;
                dtor(data);
                any = true;
            }
        }
        if (!any) {
            break;
        }
        storage = (TLSData *)tls_get();
    }
}

static void tls_thread_exit(void *ptr)
{
    TLSData *storage = (TLSData *)ptr;
    if (!storage) {
        return;
    }
    tls_set(storage); /* destructors may call SDL_GetTLS */
    tls_run_destructors(storage);
    storage = (TLSData *)tls_get();
    tls_set(NULL);
    SDL_free(storage);
}

void *SDL_GetTLS(SDL_TLSID *id)
{
    if (!id) {
        SDL_InvalidParamError("id");
        return NULL;
    }
    tls_ensure_init();
    int index = SDL_GetAtomicInt(id) - 1;
    TLSData *storage = (TLSData *)tls_get();
    if (!storage || index < 0 || index >= storage->limit) {
        return NULL;
    }
    return storage->array[index].data;
}

bool SDL_SetTLS(SDL_TLSID *id, const void *value, SDL_TLSDestructorCallback destructor)
{
    if (!id) {
        return SDL_InvalidParamError("id");
    }
    tls_ensure_init();

    int index = SDL_GetAtomicInt(id) - 1;
    if (index < 0) {
        int new_id = SDL_AddAtomicInt(&tls_next_id, 1) + 1;
        SDL_CompareAndSwapAtomicInt(id, 0, new_id); /* a lost race wastes an ID */
        index = SDL_GetAtomicInt(id) - 1;
    } else {
        int next = SDL_GetAtomicInt(&tls_next_id);
        while (index >= next) {
            if (SDL_CompareAndSwapAtomicInt(&tls_next_id, next, index + 1)) {
                break;
            }
            next = SDL_GetAtomicInt(&tls_next_id);
        }
    }

    TLSData *storage = (TLSData *)tls_get();
    if (!storage || index >= storage->limit) {
        int oldlimit = storage ? storage->limit : 0;
        int newlimit = index + TLS_ALLOC_CHUNKSIZE;
        TLSData *grown = (TLSData *)SDL_realloc(storage, sizeof(*storage) + (size_t)newlimit * sizeof(TLSEntry));
        if (!grown) {
            return false;
        }
        storage = grown;
        storage->limit = newlimit;
        for (int i = oldlimit; i < newlimit; ++i) {
            storage->array[i].data = NULL;
            storage->array[i].destructor = NULL;
        }
        if (tls_set(storage) != 0) {
            SDL_free(storage);
            return SDL_SetError("thread-local storage allocation failed");
        }
    }
    storage->array[index].data = (void *)value;
    storage->array[index].destructor = destructor;
    return true;
}

void SDL_CleanupTLS(void)
{
    tls_ensure_init();
    TLSData *storage = (TLSData *)tls_get();
    if (storage) {
        tls_run_destructors(storage);
        storage = (TLSData *)tls_get();
        tls_set(NULL);
        SDL_free(storage);
    }
}

/* SDL3's SDL_GetPersistentString(): return a per-thread stable copy, so
 * strings like device names stay valid after the owning object is freed.
 * Uses the TLS machinery above; SDL_CleanupTLS() frees the pool. */
typedef struct PersistentStrings
{
    char **items;
    int count;
    int capacity;
} PersistentStrings;

static SDL_TLSID persistent_strings_id;

static void sdloP_free_persistent_strings(void *value)
{
    PersistentStrings *strings = (PersistentStrings *)value;
    if (strings) {
        for (int i = 0; i < strings->count; ++i) {
            SDL_free(strings->items[i]);
        }
        SDL_free(strings->items);
        SDL_free(strings);
    }
}

const char *SDLOP_GetPersistentString(const char *string)
{
    if (!string) {
        return NULL;
    }
    if (!*string) {
        return "";
    }

    PersistentStrings *strings = (PersistentStrings *)SDL_GetTLS(&persistent_strings_id);
    if (!strings) {
        strings = (PersistentStrings *)SDL_calloc(1, sizeof(*strings));
        if (!strings) {
            return NULL;
        }
        if (!SDL_SetTLS(&persistent_strings_id, strings, sdloP_free_persistent_strings)) {
            SDL_free(strings);
            return NULL;
        }
    }

    for (int i = 0; i < strings->count; ++i) {
        if (strcmp(strings->items[i], string) == 0) {
            return strings->items[i];
        }
    }
    if (strings->count == strings->capacity) {
        int capacity = strings->capacity ? strings->capacity * 2 : 8;
        char **items = (char **)SDL_realloc(strings->items, (size_t)capacity * sizeof(*items));
        if (!items) {
            return NULL;
        }
        strings->items = items;
        strings->capacity = capacity;
    }
    char *copy = SDL_strdup(string);
    if (!copy) {
        return NULL;
    }
    strings->items[strings->count++] = copy;
    return copy;
}

/* ------------------------------------------------------------------ */
/* Threads                                                             */
/* ------------------------------------------------------------------ */

#define SDLOP_THREAD_MAGIC 0x54485244u /* 'THRD' */

/* Live-thread registry: `SDL_Thread *` handles are opaque to callers and a
 * joined/detached object is freed, so a magic field alone would be a
 * use-after-free read (the memory can be handed out again). Validity is
 * therefore "is this pointer still registered". (utils b0e7d28) */
static SDLOP_STATIC_MUTEX(thread_registry_lock);
static SDL_Thread *thread_registry;

struct SDL_Thread
{
    Uint32 magic;
    SDL_ThreadID threadid;
#ifdef _WIN32
    HANDLE handle;
    unsigned handle_id;
#else
    pthread_t handle;
#endif
    int status;
    SDL_AtomicInt state; /* SDL_ThreadState */
    char *name;
    size_t stacksize;
    SDL_ThreadFunction userfunc;
    void *userdata;
    SDL_Semaphore *started; /* creator waits until the name/id are set */
    SDL_FunctionPointer pfnEndThread; /* Windows: the CRT's _endthreadex */
    SDL_FunctionPointer pfnBeginThread; /* Windows: the app's _beginthreadex */
    struct SDL_Thread *registry_next;
};

static void thread_register(SDL_Thread *thread)
{
    SDLOP_MUTEX_LOCK(SDLOP_SM(&thread_registry_lock));
    thread->registry_next = thread_registry;
    thread_registry = thread;
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&thread_registry_lock));
}

static void thread_unregister(SDL_Thread *thread)
{
    SDLOP_MUTEX_LOCK(SDLOP_SM(&thread_registry_lock));
    SDL_Thread **link = &thread_registry;
    while (*link) {
        if (*link == thread) {
            *link = thread->registry_next;
            break;
        }
        link = &(*link)->registry_next;
    }
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&thread_registry_lock));
}

static bool thread_valid(SDL_Thread *thread)
{
    if (!thread) {
        SDL_InvalidParamError("thread");
        return false;
    }

    bool found = false;
    SDLOP_MUTEX_LOCK(SDLOP_SM(&thread_registry_lock));
    for (SDL_Thread *t = thread_registry; t; t = t->registry_next) {
        if (t == thread) {
            found = true;
            break;
        }
    }
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&thread_registry_lock));

    if (!found) {
        SDL_InvalidParamError("thread");
    }
    return found;
}

/* Queries report "unknown" without touching the error string, and treat NULL
 * as "the calling thread" where stock SDL3 does. */
static bool thread_queryable(SDL_Thread *thread)
{
    return !thread || thread_valid(thread);
}

/* SDL_Quit() hook: drop this thread's TLS storage so a later SDL_Init()
 * starts from a clean state (utils b0e7d28) */
void SDLOP_ThreadsQuit(void)
{
    SDL_CleanupTLS();
}

SDL_ThreadID SDL_GetCurrentThreadID(void)
{
#ifdef _WIN32
    return (SDL_ThreadID)GetCurrentThreadId();
#else
    return (SDL_ThreadID)pthread_self();
#endif
}

static void free_thread(SDL_Thread *thread)
{
    thread->magic = 0;
    SDL_free(thread->name);
    SDL_free(thread);
}

#ifndef _WIN32
static void set_current_thread_name(const char *name)
{
    if (!name) {
        return;
    }
#if defined(__APPLE__)
    typedef int (*pfn_setname_apple)(const char *);
    pfn_setname_apple fn = (pfn_setname_apple)dlsym(RTLD_DEFAULT, "pthread_setname_np");
    if (fn) {
        fn(name);
    }
#elif defined(__linux__) || defined(__ANDROID__)
    typedef int (*pfn_setname_linux)(pthread_t, const char *);
    pfn_setname_linux fn = (pfn_setname_linux)dlsym(RTLD_DEFAULT, "pthread_setname_np");
    if (fn) {
        if (fn(pthread_self(), name) == ERANGE) {
            char namebuf[16];
            SDL_strlcpy(namebuf, name, sizeof(namebuf));
            fn(pthread_self(), namebuf);
        }
    }
#elif defined(SDLOP_HAVE_PTHREAD_SETNAME_NP) && SDLOP_HAVE_PTHREAD_SETNAME_NP
#if defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void *)name);
#else
    if (pthread_setname_np(pthread_self(), name) == ERANGE) {
        char namebuf[16];
        SDL_strlcpy(namebuf, name, sizeof(namebuf));
        pthread_setname_np(pthread_self(), namebuf);
    }
#endif
#elif defined(SDLOP_HAVE_PTHREAD_SET_NAME_NP) && SDLOP_HAVE_PTHREAD_SET_NAME_NP
    pthread_set_name_np(pthread_self(), name);
#elif defined(__HAIKU__)
    char namebuf[B_OS_NAME_LENGTH];
    SDL_strlcpy(namebuf, name, sizeof(namebuf));
    rename_thread(find_thread(NULL), namebuf);
#else
    (void)name;
#endif
}
#else
/* The CRT's _beginthreadex, as passed in by <SDL3/SDL_thread.h>. SDL3 keeps
 * this typedef private to its Windows thread code; so does this file. */
typedef uintptr_t (__cdecl * SDLop_BeginThreadEx)(void *security, unsigned stacksize,
                                                  unsigned (__stdcall *startaddr)(void *),
                                                  void *arglist, unsigned initflag, unsigned *threadaddr);
#endif

static
#ifdef _WIN32
unsigned __stdcall
#else
void *
#endif
run_thread(void *arg)
{
    SDL_Thread *thread = (SDL_Thread *)arg;

#ifndef _WIN32
    set_current_thread_name(thread->name);
    /* SDL3: SDL threads don't receive asynchronous signals */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGQUIT);
    sigaddset(&mask, SIGPIPE);
    sigaddset(&mask, SIGALRM);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGWINCH);
    sigaddset(&mask, SIGVTALRM);
    sigaddset(&mask, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
#else
    /* Windows has no signal mask to inherit; SDL3 sets the thread
     * description so debuggers and the debugger's thread list show it. */
    if (thread->name) {
        wchar_t wname[64];
        if (MultiByteToWideChar(CP_UTF8, 0, thread->name, -1, wname, (int)SDL_arraysize(wname)) > 0) {
            SetThreadDescription(GetCurrentThread(), wname);
        }
    }
#endif

    thread->threadid = SDL_GetCurrentThreadID();
    SDL_SignalSemaphore(thread->started);

    int status = thread->userfunc(thread->userdata);
    thread->status = status;

    SDL_CleanupTLS();

#ifdef _WIN32
    /* Read this before the detach check below: a detached thread is freed
     * there, and touching `thread` afterwards reads freed memory. */
    SDL_FunctionPointer endfn = thread->pfnEndThread;
#endif
    if (!SDL_CompareAndSwapAtomicInt(&thread->state, SDL_THREAD_ALIVE, SDL_THREAD_COMPLETE)) {
        if (SDL_GetAtomicInt(&thread->state) == SDL_THREAD_DETACHED) {
            free_thread(thread);
            thread = NULL;
        }
    }
#ifdef _WIN32
    /* _beginthreadex wants an unsigned return; _endthreadex finishes the CRT's
     * per-thread bookkeeping the same way a normal return does. It must be the
     * CRT that *created* the thread, which is why the app's pfnEndThread is
     * only ever paired with the app's pfnBeginThread (see the create path). */
    if (endfn) {
        ((void (__stdcall *)(unsigned))endfn)((unsigned)status);
    }
    return (unsigned)status;
#else
    return NULL;
#endif
}

SDL_Thread *SDL_CreateThreadWithPropertiesRuntime(SDL_PropertiesID props, SDL_FunctionPointer pfnBeginThread,
                                                  SDL_FunctionPointer pfnEndThread)
{
#ifndef _WIN32
    /* The POSIX side creates threads with pthread_create, so the C runtime
     * hooks SDL3 passes on Windows have nothing to do. */
    if (pfnBeginThread || pfnEndThread) {
        SDL_SetError("_beginthreadex/_endthreadex not supported on this platform");
        return NULL;
    }
#endif
    SDL_ThreadFunction fn = (SDL_ThreadFunction)SDL_GetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, NULL);
    const char *name = SDL_GetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, NULL);
    const size_t stacksize = (size_t)SDL_GetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 0);
    void *userdata = SDL_GetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, NULL);
    if (!fn) {
        SDL_SetError("Thread entry function is NULL");
        return NULL;
    }

    SDL_Thread *thread = (SDL_Thread *)SDL_calloc(1, sizeof(*thread));
    if (!thread) {
        return NULL;
    }
    thread->magic = SDLOP_THREAD_MAGIC;
    thread->status = -1;
    thread_register(thread);
    SDL_SetAtomicInt(&thread->state, SDL_THREAD_ALIVE);
    if (name) {
        thread->name = SDL_strdup(name);
        if (!thread->name) {
            SDL_free(thread);
            return NULL;
        }
    }
    thread->userfunc = fn;
    thread->userdata = userdata;
    thread->stacksize = stacksize;
#ifdef _WIN32
    /* The begin and end handlers must come from the same C runtime. SDL3 passes
     * the caller's _beginthreadex/_endthreadex so that CRT bookkeeping happens
     * in the CRT the application links; pairing SDLop's _beginthreadex with the
     * caller's _endthreadex (or the reverse) corrupts per-thread CRT state. So
     * only remember pfnEndThread when pfnBeginThread will do the creating;
     * with no pfnBeginThread the thread is made with CreateThread and there is
     * no per-thread CRT to wind down. */
    if (pfnBeginThread) {
        thread->pfnEndThread = pfnEndThread;
        thread->pfnBeginThread = pfnBeginThread;
    } else {
        thread->pfnEndThread = NULL;
        thread->pfnBeginThread = NULL;
    }
#else
    (void)pfnEndThread;
#endif
    thread->started = SDL_CreateSemaphore(0);
    if (!thread->started) {
        free_thread(thread);
        return NULL;
    }

    SDL_Semaphore *started = thread->started;
#ifdef _WIN32
    if (thread->pfnBeginThread) {
        /* Run in the caller's CRT, which also ends it (see run_thread). */
        unsigned threadid = 0;
        thread->handle = (HANDLE)((size_t)((SDLop_BeginThreadEx)thread->pfnBeginThread)(
            NULL, (unsigned int)stacksize, run_thread, thread, CREATE_SUSPENDED, &threadid));
        thread->handle_id = threadid;
    } else {
        thread->handle = (HANDLE)_beginthreadex(NULL, (unsigned)stacksize, run_thread, thread,
                                                CREATE_SUSPENDED, &thread->handle_id);
    }
    if (!thread->handle) {
        SDL_DestroySemaphore(started);
        free_thread(thread);
        SDL_SetError("Not enough resources to create thread");
        return NULL;
    }
    ResumeThread(thread->handle);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if (stacksize) {
        pthread_attr_setstacksize(&attr, stacksize);
    }
    if (pthread_create(&thread->handle, &attr, run_thread, thread) != 0) {
        pthread_attr_destroy(&attr);
        SDL_DestroySemaphore(started);
        free_thread(thread);
        SDL_SetError("Not enough resources to create thread");
        return NULL;
    }
    pthread_attr_destroy(&attr);
#endif
    SDL_WaitSemaphore(started); /* threadid valid from here on */
    thread->started = NULL;
    SDL_DestroySemaphore(started);
    return thread;
}

SDL_Thread *SDL_CreateThreadRuntime(SDL_ThreadFunction fn, const char *name, void *data,
                                    SDL_FunctionPointer pfnBeginThread, SDL_FunctionPointer pfnEndThread)
{
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)fn);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, name);
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, data);
    SDL_Thread *thread = SDL_CreateThreadWithPropertiesRuntime(props, pfnBeginThread, pfnEndThread);
    SDL_DestroyProperties(props);
    return thread;
}

const char *SDL_GetThreadName(SDL_Thread *thread)
{
    return thread_queryable(thread) && thread ? thread->name : NULL;
}

SDL_ThreadID SDL_GetThreadID(SDL_Thread *thread)
{
    /* NULL means "the calling thread" (stock SDL3); a joined/detached thread
       object yields 0 instead of reading freed memory */
    if (!thread) {
        return SDL_GetCurrentThreadID();
    }
    return thread_queryable(thread) ? thread->threadid : 0;
}

SDL_ThreadState SDL_GetThreadState(SDL_Thread *thread)
{
    if (!thread || !thread_queryable(thread)) {
        return SDL_THREAD_UNKNOWN;
    }
    return (SDL_ThreadState)SDL_GetAtomicInt(&thread->state);
}

void SDL_WaitThread(SDL_Thread *thread, int *status)
{
    if (!thread || !thread_valid(thread)) {
        if (status) {
            *status = -1;
        }
        return;
    }
#ifdef _WIN32
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
#else
    pthread_join(thread->handle, NULL);
#endif
    if (status) {
        *status = thread->status;
    }
    thread_unregister(thread); /* the handle stops being valid now */
    free_thread(thread);
}

void SDL_DetachThread(SDL_Thread *thread)
{
    if (!thread || !thread_valid(thread)) {
        return;
    }

    /* Capture the handle now: the object can only be freed by the thread
       itself once it observes DETACHED, and only we can set DETACHED here,
       so reading it before the CAS is race-free. (utils b0e7d28) */
#ifdef _WIN32
    const HANDLE handle = thread->handle;
#else
    const pthread_t handle = thread->handle;
#endif

    /* The thread may vanish at any time once detached: the handle stops being
       valid for the caller from here on. */
    thread_unregister(thread);

    /* Ownership passes to the child in the ALIVE case: it frees the object
       once it observes DETACHED. */
    if (SDL_CompareAndSwapAtomicInt(&thread->state, SDL_THREAD_ALIVE, SDL_THREAD_DETACHED)) {
        /* After this point the thread may self-free; don't touch `thread`. */
#ifdef _WIN32
        CloseHandle(handle); /* Win32 has no detach: closing the handle is it */
#else
        pthread_detach(handle);
#endif
    } else if (SDL_GetAtomicInt(&thread->state) == SDL_THREAD_DETACHED) {
        /* already detached (calling twice is a no-op) */
        return;
    } else if (SDL_GetAtomicInt(&thread->state) == SDL_THREAD_COMPLETE) {
        /* the child has already exited, so it is not going to free anything:
           reap the thread and drop the object here */
#ifdef _WIN32
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
#else
        pthread_join(handle, NULL);
#endif
        free_thread(thread);
    }
}

bool SDL_SetCurrentThreadPriority(SDL_ThreadPriority priority)
{
#ifdef _WIN32
    /* SDL3's Windows mapping: the hint asks for REALTIME_PRIORITY_CLASS-like
     * behaviour, which needs a privilege SDL3 does not take either; it maps
     * the four levels onto the Win32 thread priorities directly. */
    int winprio;
    switch (priority) {
    case SDL_THREAD_PRIORITY_LOW:
        winprio = THREAD_PRIORITY_LOWEST;
        break;
    case SDL_THREAD_PRIORITY_HIGH:
        winprio = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case SDL_THREAD_PRIORITY_TIME_CRITICAL:
        winprio = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    default:
        winprio = THREAD_PRIORITY_NORMAL;
        break;
    }
    if (SetThreadPriority(GetCurrentThread(), winprio)) {
        return true;
    }
    return SDL_SetError("SetThreadPriority() failed: %lu", (unsigned long)GetLastError());
#elif defined(__linux__)
    const bool realtime = SDL_GetHintBoolean(SDL_HINT_THREAD_FORCE_REALTIME_TIME_CRITICAL, false);
    if (realtime && priority == SDL_THREAD_PRIORITY_TIME_CRITICAL) {
        struct sched_param sp;
        SDL_zero(sp);
        sp.sched_priority = sched_get_priority_max(SCHED_RR);
        if (pthread_setschedparam(pthread_self(), SCHED_RR, &sp) == 0) {
            return true;
        }
        return SDL_SetError("pthread_setschedparam() failed");
    }
    int nice_value;
    switch (priority) {
    case SDL_THREAD_PRIORITY_LOW:
        nice_value = 19;
        break;
    case SDL_THREAD_PRIORITY_HIGH:
        nice_value = -10;
        break;
    case SDL_THREAD_PRIORITY_TIME_CRITICAL:
        nice_value = -20;
        break;
    default:
        nice_value = 0;
        break;
    }
    pid_t tid = (pid_t)syscall(SYS_gettid);
    if (setpriority(PRIO_PROCESS, (id_t)tid, nice_value) == 0) {
        return true;
    }
    return SDL_SetError("setpriority() failed");
#else
    /* Non-Linux POSIX (BSDs / Solaris / Haiku / QNX / macOS): match SDL3's
     * SDL_systhread.c using pthread_setschedparam(). */
    int policy = SCHED_OTHER;
    int pri_policy;
    struct sched_param sched;
    SDL_zero(sched);
    if (pthread_getschedparam(pthread_self(), &policy, &sched) != 0) {
        return SDL_SetError("pthread_getschedparam() failed");
    }
    switch (priority) {
    case SDL_THREAD_PRIORITY_LOW:
    case SDL_THREAD_PRIORITY_NORMAL:
        pri_policy = SCHED_OTHER;
        break;
    case SDL_THREAD_PRIORITY_HIGH:
    case SDL_THREAD_PRIORITY_TIME_CRITICAL:
#if defined(SDL_PLATFORM_MACOS) || defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_TVOS)
        pri_policy = SCHED_RR;
        break;
#else
        pri_policy = SCHED_OTHER;
        break;
#endif
    default:
        pri_policy = policy;
        break;
    }
    const bool realtime = SDL_GetHintBoolean(SDL_HINT_THREAD_FORCE_REALTIME_TIME_CRITICAL, false);
    if (realtime && priority == SDL_THREAD_PRIORITY_TIME_CRITICAL) {
        pri_policy = SCHED_RR;
    }
    const char *policyhint = SDL_GetHint(SDL_HINT_THREAD_PRIORITY_POLICY);
    if (policyhint) {
        if (SDL_strcmp(policyhint, "current") == 0) {
            /* keep current policy */
        } else if (SDL_strcmp(policyhint, "other") == 0) {
            policy = SCHED_OTHER;
        } else if (SDL_strcmp(policyhint, "rr") == 0) {
            policy = SCHED_RR;
        } else if (SDL_strcmp(policyhint, "fifo") == 0) {
            policy = SCHED_FIFO;
        } else {
            policy = pri_policy;
        }
    } else {
        policy = pri_policy;
    }
    const int min_priority = sched_get_priority_min(policy);
    const int max_priority = sched_get_priority_max(policy);
    if (min_priority < 0 || max_priority < 0) {
        return (priority == SDL_THREAD_PRIORITY_NORMAL) ? true : SDL_SetError("sched_get_priority_min/max() failed");
    }
    if (priority == SDL_THREAD_PRIORITY_LOW) {
        sched.sched_priority = min_priority;
    } else if (priority == SDL_THREAD_PRIORITY_TIME_CRITICAL) {
        sched.sched_priority = max_priority;
#if defined(SDL_PLATFORM_MACOS) || defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_TVOS)
    } else if (min_priority == 15 && max_priority == 47) {
        sched.sched_priority = (priority == SDL_THREAD_PRIORITY_HIGH) ? 45 : 37;
#endif
    } else {
        sched.sched_priority = min_priority + (max_priority - min_priority) / 2;
        if (priority == SDL_THREAD_PRIORITY_HIGH) {
            sched.sched_priority += (max_priority - min_priority) / 4;
        }
    }
    if (pthread_setschedparam(pthread_self(), policy, &sched) == 0) {
        return true;
    }
    return SDL_SetError("pthread_setschedparam() failed");
#endif
}

/* ------------------------------------------------------------------ */
/* Platform-specific thread priority (SDL_system.h, Linux)             */
/* ------------------------------------------------------------------ */

/* SDL3 maps SDL priorities onto nice values for SCHED_OTHER and onto
 * 1 / 74 / 99 / 49 for SCHED_RR and SCHED_FIFO. Stock SDL escalates the
 * latter through RealtimeKit over D-Bus; SDLop has no D-Bus dependency,
 * so apply the policy directly with sched_setscheduler() (a Linux thread
 * ID is also a schedulable task ID). */
#ifndef __linux__
/* Stock SDL3 declares these only under SDL_PLATFORM_LINUX. SDLop's export
 * gate scans the headers textually and cannot see that #ifdef, so the symbols
 * must exist on non-Linux platforms as well - as the honest "not this
 * platform" answer rather than a setpriority() call that cannot work there. */
bool SDL_SetLinuxThreadPriority(Sint64 threadID, int priority)
{
    (void)threadID;
    (void)priority;
    return SDL_SetError("SDL_SetLinuxThreadPriority is only available on Linux");
}

bool SDL_SetLinuxThreadPriorityAndPolicy(Sint64 threadID, int sdlPriority, int schedPolicy)
{
    (void)threadID;
    (void)sdlPriority;
    (void)schedPolicy;
    return SDL_SetError("SDL_SetLinuxThreadPriorityAndPolicy is only available on Linux");
}
#else
bool SDL_SetLinuxThreadPriority(Sint64 threadID, int priority)
{
    if (setpriority(PRIO_PROCESS, (id_t)threadID, priority) == 0) {
        return true;
    }
    return SDL_SetError("setpriority() failed");
}

bool SDL_SetLinuxThreadPriorityAndPolicy(Sint64 threadID, int sdlPriority, int schedPolicy)
{
    int osPriority;

    if (schedPolicy == SCHED_RR || schedPolicy == SCHED_FIFO) {
        /* Same realtime range SDL3 uses for its RealtimeKit requests. */
        enum { SDL_MAX_REALTIME_PRIORITY = 99 };

        switch (sdlPriority) {
        case SDL_THREAD_PRIORITY_LOW:
            osPriority = 1;
            break;
        case SDL_THREAD_PRIORITY_HIGH:
            osPriority = SDL_MAX_REALTIME_PRIORITY * 3 / 4;
            break;
        case SDL_THREAD_PRIORITY_TIME_CRITICAL:
            osPriority = SDL_MAX_REALTIME_PRIORITY;
            break;
        default:
            osPriority = SDL_MAX_REALTIME_PRIORITY / 2;
            break;
        }

        struct sched_param param;
        SDL_zero(param);
        param.sched_priority = osPriority;
        if (sched_setscheduler((pid_t)threadID, schedPolicy, &param) == 0) {
            return true;
        }
        return SDL_SetError("sched_setscheduler() failed");
    }

    switch (sdlPriority) {
    case SDL_THREAD_PRIORITY_LOW:
        osPriority = 19;
        break;
    case SDL_THREAD_PRIORITY_HIGH:
        osPriority = -10;
        break;
    case SDL_THREAD_PRIORITY_TIME_CRITICAL:
        osPriority = -20;
        break;
    default:
        osPriority = 0;
        break;
    }
    if (setpriority(PRIO_PROCESS, (id_t)threadID, osPriority) == 0) {
        return true;
    }
    return SDL_SetError("setpriority() failed");
}
#endif /* __linux__ */
