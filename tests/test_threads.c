/*
  SDLop test: threads - create/wait, detach, TLS, names, priorities,
  atomics. Headless; no video needed.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifdef SDL_PLATFORM_LINUX
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

static SDL_AtomicInt counter;
static SDL_AtomicInt detached_finished;

static int SDLCALL simple_thread(void *data)
{
    int *out = (int *)data;
    *out = 42;
    return 7;
}

static int SDLCALL counting_thread(void *data)
{
    int n = *(int *)data;
    for (int i = 0; i < n; i++) {
        SDL_AtomicIncRef(&counter);
    }
    return 0;
}

static int SDLCALL detached_thread(void *data)
{
    (void)data;
    SDL_Delay(10);
    SDL_AtomicIncRef(&detached_finished);
    return 123; /* discarded: nobody waits */
}

static SDL_TLSID tls_id;
static int destructor_calls;

static void SDLCALL tls_destructor(void *value)
{
    (void)value;
    destructor_calls++;
}

static int SDLCALL tls_thread(void *data)
{
    (void)data;
    /* TLS storage is per-thread: main's value must not be visible here */
    assert(SDL_GetTLS(&tls_id) == NULL);
    assert(SDL_SetTLS(&tls_id, (void *)"thread-value", tls_destructor));
    assert(strcmp((const char *)SDL_GetTLS(&tls_id), "thread-value") == 0);
    return 0;
}

static int SDLCALL props_worker(void *data)
{
    SDL_PropertiesID shared = (SDL_PropertiesID)(uintptr_t)data;
    int id = (int)(uintptr_t)SDL_GetCurrentThreadID();
    for (int i = 0; i < 3000; i++) {
        char key[40], val[32];
        SDL_snprintf(key, sizeof key, "t%x-k%d", id, i & 31); /* per-thread key: race-free */
        SDL_snprintf(val, sizeof val, "v%d", i);
        if (!SDL_SetStringProperty(shared, key, val)) {
            return 1;
        }
        const char *got = SDL_GetStringProperty(shared, key, NULL);
        if (!got || SDL_strcmp(got, val) != 0) {
            return 2;
        }
        SDL_PropertiesID own = SDL_CreateProperties();
        if (!own) {
            return 3;
        }
        SDL_SetNumberProperty(own, "n", i);
        SDL_DestroyProperties(own);
    }
    return 0;
}

static int SDLCALL name_thread(void *data)
{
    (void)data;
    /* SDL_GetThreadName(NULL-ish) is not allowed, but the thread can check
       its own identity indirectly through SDL_GetCurrentThreadID */
    assert(SDL_GetCurrentThreadID() != 0);
    return 0;
}

int main(void)
{
    /* --- atomics --- */
    SDL_AtomicInt a = { 0 };
    assert(SDL_GetAtomicInt(&a) == 0);
    assert(SDL_SetAtomicInt(&a, 5) == 0);       /* returns previous */
    assert(SDL_GetAtomicInt(&a) == 5);
    assert(SDL_AddAtomicInt(&a, 3) == 5);       /* returns previous */
    assert(SDL_GetAtomicInt(&a) == 8);
    assert(!SDL_CompareAndSwapAtomicInt(&a, 1, 9));  /* expected mismatch */
    assert(SDL_GetAtomicInt(&a) == 8);
    assert(SDL_CompareAndSwapAtomicInt(&a, 8, 9));
    assert(SDL_GetAtomicInt(&a) == 9);
    assert(SDL_AtomicIncRef(&a) == 9);
    assert(SDL_AtomicDecRef(&a) == false);      /* 10 -> 9, not reaching zero */
    SDL_SetAtomicInt(&a, 1);
    assert(SDL_AtomicDecRef(&a) == true);       /* reached zero */

    SDL_AtomicU32 u = { 0 };
    assert(SDL_SetAtomicU32(&u, 42) == 0);
    assert(SDL_GetAtomicU32(&u) == 42);
    assert(SDL_CompareAndSwapAtomicU32(&u, 42, 43));

    void *ptr = NULL;
    int x;
    assert(SDL_GetAtomicPointer(&ptr) == NULL);
    assert(SDL_SetAtomicPointer(&ptr, &x) == NULL);
    assert(SDL_GetAtomicPointer(&ptr) == &x);
    assert(SDL_CompareAndSwapAtomicPointer(&ptr, &x, NULL));
    assert(SDL_GetAtomicPointer(&ptr) == NULL);

    /* spinlocks */
    SDL_SpinLock lock = 0;
    assert(SDL_TryLockSpinlock(&lock));
    assert(!SDL_TryLockSpinlock(&lock));
    SDL_UnlockSpinlock(&lock);
    SDL_LockSpinlock(&lock);
    SDL_UnlockSpinlock(&lock);

    /* --- create + wait --- */
    int out = 0;
    SDL_Thread *t = SDL_CreateThread(simple_thread, "simple", &out);
    assert(t);
    assert(SDL_GetThreadState(t) == SDL_THREAD_ALIVE ||
           SDL_GetThreadState(t) == SDL_THREAD_COMPLETE);
    assert(SDL_GetThreadName(t) && strcmp(SDL_GetThreadName(t), "simple") == 0);
    int status = -1;
    SDL_WaitThread(t, &status);
    assert(status == 7);
    assert(out == 42);
    /* the object is gone now */
    assert(SDL_GetThreadState(t) == SDL_THREAD_UNKNOWN);
    assert(SDL_GetThreadID(t) == 0);
    SDL_WaitThread(t, NULL); /* double-wait is a no-op, status untouched semantics */
    /* name survives cleanup (persistent string) */
    /* (SDL_GetThreadName on an invalid thread returns NULL) */
    assert(SDL_GetThreadName(t) == NULL);

    /* NULL thread */
    SDL_WaitThread(NULL, &status);
    assert(status == -1);
    SDL_DetachThread(NULL);           /* no-op */
    assert(SDL_GetThreadState(NULL) == SDL_THREAD_UNKNOWN);
    assert(SDL_GetThreadID(NULL) == SDL_GetCurrentThreadID());

    /* unnamed thread */
    t = SDL_CreateThread(simple_thread, NULL, &out);
    assert(t);
    assert(SDL_GetThreadName(t) == NULL);
    SDL_WaitThread(t, &status);
    assert(status == 7);

    /* --- CreateThreadWithProperties --- */
    {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)simple_thread);
        SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "propped");
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, &out);
        SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 64 * 1024);
        t = SDL_CreateThreadWithProperties(props);
        assert(t);
        SDL_WaitThread(t, &status);
        assert(status == 7);
        SDL_DestroyProperties(props);

        /* missing entry function -> error */
        props = SDL_CreateProperties();
        assert(SDL_CreateThreadWithProperties(props) == NULL);
        SDL_DestroyProperties(props);
    }

    /* --- detached threads clean themselves up --- */
    SDL_SetAtomicInt(&detached_finished, 0);
    for (int i = 0; i < 8; i++) {
        t = SDL_CreateThread(detached_thread, "detached", NULL);
        assert(t);
        SDL_DetachThread(t);
        assert(SDL_GetThreadState(t) == SDL_THREAD_UNKNOWN); /* invalid after detach */
    }
    /* wait for all of them to actually finish (they self-free) */
    {
        Uint64 deadline = SDL_GetTicks() + 5000;
        while (SDL_GetAtomicInt(&detached_finished) < 8 && SDL_GetTicks() < deadline) {
            SDL_Delay(1);
        }
        assert(SDL_GetAtomicInt(&detached_finished) == 8);
    }

    /* --- many threads hammering one atomic --- */
    SDL_SetAtomicInt(&counter, 0);
    {
        enum { N = 8, PER = 10000 };
        SDL_Thread *threads[N];
        int per = PER;
        for (int i = 0; i < N; i++) {
            threads[i] = SDL_CreateThread(counting_thread, "counter", &per);
            assert(threads[i]);
        }
        for (int i = 0; i < N; i++) {
            SDL_WaitThread(threads[i], NULL);
        }
        assert(SDL_GetAtomicInt(&counter) == N * PER);
    }

    /* --- TLS --- */
    destructor_calls = 0;
    assert(SDL_SetTLS(&tls_id, (void *)"main-value", tls_destructor));
    assert(strcmp((const char *)SDL_GetTLS(&tls_id), "main-value") == 0);
    /* child sees its own storage, and its destructor runs at thread exit */
    t = SDL_CreateThread(tls_thread, "tls", NULL);
    assert(t);
    SDL_WaitThread(t, NULL);
    assert(destructor_calls == 1); /* child's destructor fired */
    assert(strcmp((const char *)SDL_GetTLS(&tls_id), "main-value") == 0); /* ours intact */
    /* NULL id rejected */
    assert(SDL_GetTLS(NULL) == NULL);
    assert(!SDL_SetTLS(NULL, (void *)"x", NULL));
    SDL_CleanupTLS();
    assert(destructor_calls == 2);
    assert(SDL_GetTLS(&tls_id) == NULL);

    /* --- priorities: NORMAL must work everywhere; others may fail
           without privileges (same as SDL3 without rtkit) --- */
    assert(SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_NORMAL));
    (void)SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    (void)SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    (void)SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);

#ifdef SDL_PLATFORM_LINUX
    /* SDL_system.h Linux entry points (used by sdl2-compat). Unprivileged
       threads may only lower their priority (raise the nice value). */
    {
        Sint64 tid = (Sint64)syscall(SYS_gettid);

        /* 19 (lowest priority) is always allowed without CAP_SYS_NICE */
        assert(SDL_SetLinuxThreadPriority(tid, 19));
        assert(getpriority(PRIO_PROCESS, 0) == 19);
        assert(SDL_SetLinuxThreadPriorityAndPolicy(tid, SDL_THREAD_PRIORITY_LOW, SCHED_OTHER));
        assert(getpriority(PRIO_PROCESS, 0) == 19);

        SDL_ClearError();
        assert(!SDL_SetLinuxThreadPriority(0x7fffffff, 0));
        assert(strstr(SDL_GetError(), "setpriority") != NULL);
    }
#endif

    /* --- threads from a thread (name thread runs asserts) --- */
    t = SDL_CreateThread(name_thread, "named", NULL);
    assert(t);
    SDL_WaitThread(t, NULL);

    /* --- properties are thread-safe (global table + per-set rwlock) --- */
    {
        SDL_PropertiesID shared = SDL_CreateProperties();
        assert(shared);
        SDL_Thread *pt[4];
        for (int i = 0; i < 4; i++) {
            pt[i] = SDL_CreateThread(props_worker, NULL, (void *)(uintptr_t)shared);
            assert(pt[i]);
        }
        for (int i = 0; i < 4; i++) {
            int st = -1;
            SDL_WaitThread(pt[i], &st);
            assert(st == 0);
        }
        assert(SDL_LockProperties(shared));
        /* the lock is not recursive (same as SDL3): a second lock would block */
        SDL_UnlockProperties(shared);
        SDL_DestroyProperties(shared);
    }

    printf("test_threads: OK\n");
    return 0;
}
