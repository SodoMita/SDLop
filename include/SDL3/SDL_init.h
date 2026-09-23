/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Initialization and shutdown (API compatible with <SDL3/SDL_init.h>).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_init_h_
#define SDL_init_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>

/* SDL3 subsystem flags (identical values). SDLop implements TIMER, EVENTS
 * and VIDEO; the others are accepted but ignored (lean build). */
#define SDL_INIT_TIMER      0x00000001u
#define SDL_INIT_AUDIO      0x00000010u /**< not implemented by SDLop */
#define SDL_INIT_VIDEO      0x00000020u /**< implies SDL_INIT_EVENTS */
#define SDL_INIT_JOYSTICK   0x00000200u /**< not implemented by SDLop */
#define SDL_INIT_HAPTIC     0x00001000u /**< not implemented by SDLop */
#define SDL_INIT_GAMEPAD    0x00002000u /**< not implemented by SDLop */
#define SDL_INIT_EVENTS     0x00004000u
#define SDL_INIT_SENSOR     0x00008000u /**< not implemented by SDLop */
#define SDL_INIT_CAMERA     0x00010000u /**< not implemented by SDLop */

/**
 * Initialize the SDLop library (SDL3-compatible).
 *
 * \param flags subsystem initialization flags.
 * \returns true on success or false on failure; call SDL_GetError().
 */
extern bool SDL_Init(SDL_InitFlags flags);

/**
 * Compatibility for specific subsystem initialization.
 */
extern bool SDL_InitSubSystem(SDL_InitFlags flags);

/**
 * Shut down specific SDLop subsystems.
 */
extern void SDL_QuitSubSystem(SDL_InitFlags flags);

/**
 * Get a mask of the specified subsystems which have been initialized.
 */
extern SDL_InitFlags SDL_WasInit(SDL_InitFlags flags);

/**
 * Clean up all initialized subsystems.
 */
extern void SDL_Quit(void);

#endif /* SDL_init_h_ */
