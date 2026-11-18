/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.

  Aggregate header: `#include <SDL3/SDL.h>` like SDL3. Generated from
  SDL 3.4.20 by tools/gen_headers.py; the subsystems SDLop does not
  implement (audio, joystick, threads, ...) are present as types only.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_h_
#define SDL_h_

#include <SDL3/SDL_platform.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_endian.h>
#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_version.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_blendmode.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_touch.h>
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_camera.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_guid.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_pen.h>
#include <SDL3/SDL_power.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_thread.h>

/* SDLop's own version (the SDL API level is SDL_VERSION, SDL_version.h) */
#define SDLOP_MAJOR_VERSION 0
#define SDLOP_MINOR_VERSION 2
#define SDLOP_PATCHLEVEL    0

#endif /* SDL_h_ */
