/*
  SDLop - atomic operations (see <SDL3/SDL_atomic.h>).

  GNU __atomic builtins: lock-free and sequentially consistent, matching
  SDL3's semantics (CAS/Set/Add return the previous value).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL_atomic.h>

bool SDL_TryLockSpinlock(SDL_SpinLock *lock)
{
    return __atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) == 0;
}

void SDL_LockSpinlock(SDL_SpinLock *lock)
{
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) != 0) {
        /* spin without hammering the cache line */
        while (__atomic_load_n(lock, __ATOMIC_RELAXED) != 0) {
            SDL_CPUPauseInstruction();
        }
    }
}

void SDL_UnlockSpinlock(SDL_SpinLock *lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

void SDL_MemoryBarrierReleaseFunction(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

void SDL_MemoryBarrierAcquireFunction(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

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
