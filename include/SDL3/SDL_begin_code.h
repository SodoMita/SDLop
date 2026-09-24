/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  SDL_begin_code.h / SDL_close_code.h exist so that headers written against
  upstream SDL3 (which brackets every public header with these two files) keep
  compiling.  Upstream uses them for compiler-specific pragmas and export
  decoration; SDLop needs neither, so SDL_begin_code.h is where the standard
  types come from and SDL_close_code.h is deliberately empty.
*/

#ifndef SDL_begin_code_h_
#define SDL_begin_code_h_

#include <SDL3/SDL_stdinc.h>

#endif /* SDL_begin_code_h_ */
