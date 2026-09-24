/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  GENERATED FILE -- do not edit.  Produced by tools/sdlop.py from the upstream SDL3
  header SDL_error.h (3.2.10), which is zlib licensed, Copyright (C) 1997-2025 Sam Lantinga
  and SDL contributors.  Declarations are copied verbatim so that source and binary
  compatibility with SDL3 are exact; the items SDLop does not implement were
  removed (the full list is in tools/dropped.txt).

  In this header: Error reporting.
*/

#ifndef SDL_error_h_
#define SDL_error_h_

#include <SDL3/SDL_begin_code.h>

#ifdef __cplusplus
extern "C" {
#endif

extern SDL_DECLSPEC bool SDLCALL SDL_SetError(SDL_PRINTF_FORMAT_STRING const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);

extern SDL_DECLSPEC bool SDLCALL SDL_OutOfMemory(void);

extern SDL_DECLSPEC const char * SDLCALL SDL_GetError(void);

extern SDL_DECLSPEC bool SDLCALL SDL_ClearError(void);

#define SDL_Unsupported() SDL_SetError("That operation is not supported")

#define SDL_InvalidParamError(param) SDL_SetError("Parameter '%s' is invalid", (param))

#include <SDL3/SDL_close_code.h>

#ifdef __cplusplus
}
#endif

#endif /* SDL_error_h_ */
