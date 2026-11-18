/*
  SDLop - error handling.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL_error.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SDLOP_ERROR_SIZE 512

static _Thread_local char sdlop_error[SDLOP_ERROR_SIZE] = "";

bool SDL_SetError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sdlop_error, sizeof(sdlop_error), fmt, ap);
    va_end(ap);
    return false;
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

bool SDL_OutOfMemory(void)
{
    return SDL_SetError("Out of memory");
}
