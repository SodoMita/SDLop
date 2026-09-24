/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  SDL.h: the umbrella header. Include this and you get everything SDLop
  implements. Include <SDL3/SDL_vulkan.h> or <SDL3/SDL_opengl.h> separately if
  you need them, exactly like upstream SDL3.

  Function signatures here match SDL3 3.2.10 (zlib licence, Copyright (C)
  1997-2025 Sam Lantinga and SDL contributors).  Anything SDL3 has that the lean
  build leaves out is listed in tools/dropped.txt.
*/

#ifndef SDL_h_
#define SDL_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_version.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_misc.h>

#endif /* SDL_h_ */
