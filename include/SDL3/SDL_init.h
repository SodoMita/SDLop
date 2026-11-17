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
#include <SDL3/SDL_events.h>

#include <SDL3/SDL_begin_code.h>

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

/* ------------------------------------------------------------------ */
/* Callback-based application entry (SDL3-identical; see SDL_main.h)   */
/* ------------------------------------------------------------------ */

/**
 * Return values for optional main callbacks.
 *
 * Returning SDL_APP_SUCCESS or SDL_APP_FAILURE from SDL_AppInit(),
 * SDL_AppIterate() or SDL_AppEvent() terminates the app and makes
 * SDL_EnterAppMainCallbacks() return 0 or 1 respectively.
 */
typedef enum SDL_AppResult
{
    SDL_APP_CONTINUE,   /**< Value that requests that the app continue from the main callbacks. */
    SDL_APP_SUCCESS,    /**< Value that requests termination with success from the main callbacks. */
    SDL_APP_FAILURE     /**< Value that requests termination with error from the main callbacks. */
} SDL_AppResult;

/**
 * Function pointer typedef for SDL_AppInit.
 */
typedef SDL_AppResult (SDLCALL *SDL_AppInit_func)(void **appstate, int argc, char *argv[]);

/**
 * Function pointer typedef for SDL_AppIterate.
 */
typedef SDL_AppResult (SDLCALL *SDL_AppIterate_func)(void *appstate);

/**
 * Function pointer typedef for SDL_AppEvent.
 */
typedef SDL_AppResult (SDLCALL *SDL_AppEvent_func)(void *appstate, SDL_Event *event);

/**
 * Function pointer typedef for SDL_AppQuit.
 */
typedef void (SDLCALL *SDL_AppQuit_func)(void *appstate, SDL_AppResult result);

#include <SDL3/SDL_close_code.h>

#endif /* SDL_init_h_ */
