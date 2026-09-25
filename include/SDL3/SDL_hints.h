/*
  SDLop - hint name strings (SDL3 3.2.10 values). The lean core reads a
  subset of these; the strings are provided so applications compiled
  against SDL3 keep compiling.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_hints_h_
#define SDL_hints_h_

#include <SDL3/SDL_stdinc.h>

#include <SDL3/SDL_begin_code.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SDL_HINT_VIDEO_DRIVER "SDL_VIDEO_DRIVER"
#define SDL_HINT_KEYCODE_OPTIONS "SDL_KEYCODE_OPTIONS"
#define SDL_HINT_MOUSE_RELATIVE_MODE_CENTER "SDL_MOUSE_RELATIVE_MODE_CENTER"
#define SDL_HINT_NO_SIGNAL_HANDLERS "SDL_NO_SIGNAL_HANDLERS"
#define SDL_HINT_RETURN_KEY_HIDES_IME "SDL_RETURN_KEY_HIDES_IME"

#ifdef __cplusplus
}
#endif
#include <SDL3/SDL_close_code.h>

#endif /* SDL_hints_h_ */
