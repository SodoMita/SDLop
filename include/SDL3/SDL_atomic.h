/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Atomic operations (API compatible with <SDL3/SDL_atomic.h>).

  Implemented with C11/GNU __atomic builtins (lock-free on every
  platform SDLop targets).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_atomic_h_
#define SDL_atomic_h_

#include <SDL3/SDL_stdinc.h>

#include <SDL3/SDL_begin_code.h>
#ifdef __cplusplus
extern "C" {
#endif

/**
 * SDL spin lock: 0 when unlocked, 1 when locked.
 */
typedef int SDL_SpinLock;

extern bool SDLCALL SDL_TryLockSpinlock(SDL_SpinLock *lock);
extern void SDLCALL SDL_LockSpinlock(SDL_SpinLock *lock);
extern void SDLCALL SDL_UnlockSpinlock(SDL_SpinLock *lock);

/**
 * Compiler barrier (prevents compiler reordering across it).
 */
#if defined(__GNUC__) || defined(__clang__)
#define SDL_CompilerBarrier()   __asm__ __volatile__ ("" : : : "memory")
#else
#define SDL_CompilerBarrier()   \
{ SDL_SpinLock _tmp = 0; SDL_LockSpinlock(&_tmp); SDL_UnlockSpinlock(&_tmp); }
#endif

extern void SDLCALL SDL_MemoryBarrierReleaseFunction(void);
extern void SDLCALL SDL_MemoryBarrierAcquireFunction(void);

#if defined(__GNUC__) && (defined(__powerpc__) || defined(__ppc__))
#define SDL_MemoryBarrierRelease()   __asm__ __volatile__ ("lwsync" : : : "memory")
#define SDL_MemoryBarrierAcquire()   __asm__ __volatile__ ("lwsync" : : : "memory")
#elif defined(__GNUC__) && defined(__aarch64__)
#define SDL_MemoryBarrierRelease()   __asm__ __volatile__ ("dmb ish" : : : "memory")
#define SDL_MemoryBarrierAcquire()   __asm__ __volatile__ ("dmb ish" : : : "memory")
#elif defined(__GNUC__) && defined(__arm__) && \
      (defined(__ARM_ARCH_7__) || defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7EM__) || \
       defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7S__) || \
       defined(__ARM_ARCH_8A__))
#define SDL_MemoryBarrierRelease()   __asm__ __volatile__ ("dmb ish" : : : "memory")
#define SDL_MemoryBarrierAcquire()   __asm__ __volatile__ ("dmb ish" : : : "memory")
#else
#define SDL_MemoryBarrierRelease()  SDL_CompilerBarrier()
#define SDL_MemoryBarrierAcquire()  SDL_CompilerBarrier()
#endif

/**
 * Pause instruction for spin loops (x86 PAUSE, ARM YIELD, ...).
 */
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))
    #define SDL_CPUPauseInstruction() __asm__ __volatile__("pause\n")
#elif (defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7) || defined(__aarch64__)
    #define SDL_CPUPauseInstruction() __asm__ __volatile__("yield" ::: "memory")
#elif defined(__powerpc__) || defined(__powerpc64__)
    #define SDL_CPUPauseInstruction() __asm__ __volatile__("or 27,27,27");
#elif (defined(__riscv) && __riscv_xlen == 64)
    #define SDL_CPUPauseInstruction() __asm__ __volatile__(".insn i 0x0F, 0, x0, x0, 0x010");
#else
    #define SDL_CPUPauseInstruction()
#endif

typedef struct SDL_AtomicInt { int value; } SDL_AtomicInt;

extern bool SDLCALL SDL_CompareAndSwapAtomicInt(SDL_AtomicInt *a, int oldval, int newval);
extern int SDLCALL SDL_SetAtomicInt(SDL_AtomicInt *a, int v);
extern int SDLCALL SDL_GetAtomicInt(SDL_AtomicInt *a);
extern int SDLCALL SDL_AddAtomicInt(SDL_AtomicInt *a, int v);

#ifndef SDL_AtomicIncRef
#define SDL_AtomicIncRef(a)    SDL_AddAtomicInt(a, 1)
#endif
#ifndef SDL_AtomicDecRef
#define SDL_AtomicDecRef(a)    (SDL_AddAtomicInt(a, -1) == 1)
#endif

typedef struct SDL_AtomicU32 { Uint32 value; } SDL_AtomicU32;

extern bool SDLCALL SDL_CompareAndSwapAtomicU32(SDL_AtomicU32 *a, Uint32 oldval, Uint32 newval);
extern Uint32 SDLCALL SDL_SetAtomicU32(SDL_AtomicU32 *a, Uint32 v);
extern Uint32 SDLCALL SDL_GetAtomicU32(SDL_AtomicU32 *a);

extern bool SDLCALL SDL_CompareAndSwapAtomicPointer(void **a, void *oldval, void *newval);
extern void * SDLCALL SDL_SetAtomicPointer(void **a, void *v);
extern void * SDLCALL SDL_GetAtomicPointer(void **a);

#ifdef __cplusplus
}
#endif

#include <SDL3/SDL_close_code.h>

#endif /* SDL_atomic_h_ */
