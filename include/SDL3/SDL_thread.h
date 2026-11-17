/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Thread management (API compatible with <SDL3/SDL_thread.h>).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_thread_h_
#define SDL_thread_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_atomic.h>

#include <SDL3/SDL_begin_code.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Thread opaque object, ID and TLS ID types (identical to SDL3) */
typedef struct SDL_Thread SDL_Thread;
typedef Uint64 SDL_ThreadID;
typedef SDL_AtomicInt SDL_TLSID;

typedef enum SDL_ThreadPriority {
    SDL_THREAD_PRIORITY_LOW,
    SDL_THREAD_PRIORITY_NORMAL,
    SDL_THREAD_PRIORITY_HIGH,
    SDL_THREAD_PRIORITY_TIME_CRITICAL
} SDL_ThreadPriority;

typedef enum SDL_ThreadState
{
    SDL_THREAD_UNKNOWN,     /**< The thread is not valid */
    SDL_THREAD_ALIVE,       /**< The thread is currently running */
    SDL_THREAD_DETACHED,    /**< The thread is detached and can't be waited on */
    SDL_THREAD_COMPLETE     /**< The thread has finished and should be cleaned up with SDL_WaitThread() */
} SDL_ThreadState;

/**
 * The function passed to SDL_CreateThread().
 *
 * \param data what was passed as `data` to SDL_CreateThread().
 * \returns a value that can be reported through SDL_WaitThread().
 */
typedef int (SDLCALL *SDL_ThreadFunction) (void *data);

/*
 * SDL3 passes the C runtime's _beginthreadex/_endthreadex through on
 * Windows; everywhere else they are NULL (and must stay NULL - SDLop,
 * like SDL3, rejects non-NULL values on non-Windows platforms).
 */
#ifndef SDL_BeginThreadFunction
#define SDL_BeginThreadFunction NULL
#endif
#ifndef SDL_EndThreadFunction
#define SDL_EndThreadFunction NULL
#endif

extern SDL_Thread * SDLCALL SDL_CreateThreadRuntime(SDL_ThreadFunction fn, const char *name, void *data, SDL_FunctionPointer pfnBeginThread, SDL_FunctionPointer pfnEndThread);
extern SDL_Thread * SDLCALL SDL_CreateThreadWithPropertiesRuntime(SDL_PropertiesID props, SDL_FunctionPointer pfnBeginThread, SDL_FunctionPointer pfnEndThread);

#define SDL_CreateThread(fn, name, data) SDL_CreateThreadRuntime((fn), (name), (data), (SDL_FunctionPointer) (SDL_BeginThreadFunction), (SDL_FunctionPointer) (SDL_EndThreadFunction))
#define SDL_CreateThreadWithProperties(props) SDL_CreateThreadWithPropertiesRuntime((props), (SDL_FunctionPointer) (SDL_BeginThreadFunction), (SDL_FunctionPointer) (SDL_EndThreadFunction))

#define SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER                  "SDL.thread.create.entry_function"
#define SDL_PROP_THREAD_CREATE_NAME_STRING                             "SDL.thread.create.name"
#define SDL_PROP_THREAD_CREATE_USERDATA_POINTER                        "SDL.thread.create.userdata"
#define SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER                        "SDL.thread.create.stacksize"

/**
 * Get the thread name as it was specified in SDL_CreateThread().
 *
 * Returns a pointer to a UTF-8 string that follows the SDL_GetStringRule
 * (persists past the thread's cleanup by SDL_WaitThread), or NULL if the
 * thread doesn't have a name.
 */
extern const char * SDLCALL SDL_GetThreadName(SDL_Thread *thread);

/**
 * Get the thread identifier for the current thread.
 */
extern SDL_ThreadID SDLCALL SDL_GetCurrentThreadID(void);

/**
 * Get the thread identifier for the specified thread (0 if the pointer is
 * NULL or no longer valid; equivalent to SDL_GetCurrentThreadID() when
 * `thread` is NULL).
 */
extern SDL_ThreadID SDLCALL SDL_GetThreadID(SDL_Thread *thread);

/**
 * Set the priority for the current thread.
 *
 * On Linux this maps to setpriority() nice values (LOW 19, NORMAL 0,
 * HIGH -10, TIME_CRITICAL -20), exactly like SDL3 without an rtkit/dbus
 * fallback: negative nice values need privileges and the call fails with
 * SDL_GetError() otherwise. The SDL_THREAD_PRIORITY_POLICY and
 * SDL_THREAD_FORCE_REALTIME_TIME_CRITICAL environment variables are
 * honored like SDL3's hints.
 */
extern bool SDLCALL SDL_SetCurrentThreadPriority(SDL_ThreadPriority priority);

/**
 * Wait for a thread to finish.
 *
 * Threads that haven't been detached will remain (as a "zombie") until
 * this function cleans them up.
 */
extern void SDLCALL SDL_WaitThread(SDL_Thread *thread, int *status);

/**
 * Get the current state of a thread.
 */
extern SDL_ThreadState SDLCALL SDL_GetThreadState(SDL_Thread *thread);

/**
 * Let a thread clean up on exit without waiting for it.
 */
extern void SDLCALL SDL_DetachThread(SDL_Thread *thread);

/**
 * Get the current thread's value associated with a thread local storage ID.
 *
 * \param id a pointer to the thread local storage ID, may not be NULL.
 * \returns the value associated with the ID for the current thread, or NULL
 *          if no value has been set; call SDL_GetError() for more information.
 */
extern void * SDLCALL SDL_GetTLS(SDL_TLSID *id);

/**
 * The callback used to cleanup data passed to SDL_SetTLS().
 */
typedef void (SDLCALL *SDL_TLSDestructorCallback)(void *value);

/**
 * Set the current thread's value associated with a thread local storage ID.
 *
 * \param id a pointer to the thread local storage ID, may not be NULL.
 * \param value the value to associate with the ID for the current thread.
 * \param destructor a function called when the thread exits, to free the
 *                   value; may be NULL.
 * \returns true on success or false on failure; call SDL_GetError().
 */
extern bool SDLCALL SDL_SetTLS(SDL_TLSID *id, const void *value, SDL_TLSDestructorCallback destructor);

/**
 * Cleanup all TLS data for this thread.
 */
extern void SDLCALL SDL_CleanupTLS(void);

#ifdef __cplusplus
}
#endif

#include <SDL3/SDL_close_code.h>

#endif /* SDL_thread_h_ */
