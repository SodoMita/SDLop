/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.

  Drop-in aggregate header: `#include <SDL3/SDL.h>` works exactly like
  SDL3 for the windowing + input subset implemented by SDLop.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_h_
#define SDL_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_events.h>

/** SDLop version information */
#define SDLOP_MAJOR_VERSION 0
#define SDLOP_MINOR_VERSION 1
#define SDLOP_PATCHLEVEL    0

/** SDL3-compatible version numbers (API level SDLop targets) */
#define SDL_MAJOR_VERSION 3
#define SDL_MINOR_VERSION 2
#define SDL_MICRO_VERSION 10

#endif /* SDL_h_ */
