/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
 *
 * SDLop: generated from SDL 3.4.20 by tools/gen_headers.py - declarations of
 * functions SDLop does not implement are removed; everything else is
 * upstream's text. Do not edit by hand.
 */

#ifndef SDL_power_h_
#define SDL_power_h_

/**
 * # CategoryPower
 *
 * SDL power management routines.
 *
 * There is a single function in this category: SDL_GetPowerInfo().
 *
 * This function is useful for games on the go. This allows an app to know if
 * it's running on a draining battery, which can be useful if the app wants to
 * reduce processing, or perhaps framerate, to extend the duration of the
 * battery's charge. Perhaps the app just wants to show a battery meter when
 * fullscreen, or alert the user when the power is getting extremely low, so
 * they can save their game.
 */

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>

#include <SDL3/SDL_begin_code.h>
/* Set up for C function definitions, even when using C++ */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * The basic state for the system's power supply.
 *
 * These are results returned by SDL_GetPowerInfo().
 *
 * \since This enum is available since SDL 3.2.0.
 */
typedef enum SDL_PowerState
{
    SDL_POWERSTATE_ERROR = -1,   /**< error determining power status */
    SDL_POWERSTATE_UNKNOWN,      /**< cannot determine power status */
    SDL_POWERSTATE_ON_BATTERY,   /**< Not plugged in, running on the battery */
    SDL_POWERSTATE_NO_BATTERY,   /**< Plugged in, no battery available */
    SDL_POWERSTATE_CHARGING,     /**< Plugged in, charging battery */
    SDL_POWERSTATE_CHARGED       /**< Plugged in, battery charged */
} SDL_PowerState;


/* Ends C function definitions when using C++ */
#ifdef __cplusplus
}
#endif
#include <SDL3/SDL_close_code.h>

#endif /* SDL_power_h_ */
