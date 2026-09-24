/*
  SDLop -- SDL_error.h.

  The error string is per-thread, like SDL3's, so a worker thread can set an error
  without clobbering the main thread's.
*/

#include "../sdlop_internal.h"

#include <stdio.h>
#include <string.h>

#define SDLOP_ERROR_LEN 1024

static _Thread_local char sdlop_error[SDLOP_ERROR_LEN];

bool SDL_SetError(SDL_PRINTF_FORMAT_STRING const char *fmt, ...)
{
    va_list ap;
    if (!fmt) {
        sdlop_error[0] = '\0';
        return false;
    }
    va_start(ap, fmt);
    vsnprintf(sdlop_error, sizeof(sdlop_error), fmt, ap);
    va_end(ap);
    return false;
}

bool SDLOP_SetError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sdlop_error, sizeof(sdlop_error), fmt, ap);
    va_end(ap);
    return false;
}

bool SDL_OutOfMemory(void)
{
    return SDL_SetError("Out of memory");
}

const char *SDL_GetError(void)
{
    return sdlop_error;
}

bool SDL_ClearError(void)
{
    sdlop_error[0] = '\0';
    return true;
}
