/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Error handling (API compatible with <SDL3/SDL_error.h>).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_error_h_
#define SDL_error_h_

#include <SDL3/SDL_stdinc.h>

/**
 * Set the SDL error message for the current thread.
 *
 * Calling this function will replace any previous error message that was set.
 * It always returns false, since SDL frequently uses it on failure paths:
 * `return SDL_SetError("...");`
 *
 * \param fmt a printf()-style message format string.
 * \returns false.
 */
extern bool SDL_SetError(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/**
 * Retrieve the last error message set by SDL for the current thread.
 *
 * The returned string is valid until the next SDL_SetError() call on this
 * thread (never NULL).
 */
extern const char *SDL_GetError(void);

/**
 * Clear any previous error message for this thread.
 *
 * \returns true.
 */
extern bool SDL_ClearError(void);

/**
 * Set an error indicating that memory allocation failed.
 *
 * \returns false.
 */
extern bool SDL_OutOfMemory(void);

#endif /* SDL_error_h_ */
