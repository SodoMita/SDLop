/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Application entry points (API compatible with <SDL3/SDL_main.h>).

  SDLop targets platforms where the process provides a normal C main()
  (Linux, Emscripten), so unlike SDL3 it never needs to redefine main for
  platform reasons: SDL_MAIN_NEEDED / SDL_MAIN_AVAILABLE stay undefined.
  The callback-style entry (SDL_MAIN_USE_CALLBACKS) works exactly like
  SDL3: define it, implement SDL_AppInit/Iterate/Event/Quit, and a main()
  that runs SDL_EnterAppMainCallbacks() is provided by SDL_main_impl.h.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_main_h_
#define SDL_main_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>

/* SDLop: no platform needs main() redefinition, but callback apps do
   (their "main" is supplied by SDL_main_impl.h below). */
#if defined(SDL_MAIN_USE_CALLBACKS)
#define main SDL_main
#endif

#include <SDL3/SDL_init.h>

#include <SDL3/SDL_begin_code.h>
#ifdef __cplusplus
extern "C" {
#endif

#ifdef SDL_MAIN_USE_CALLBACKS
extern SDL_AppResult SDLCALL SDL_AppInit(void **appstate, int argc, char *argv[]);
extern SDL_AppResult SDLCALL SDL_AppIterate(void *appstate);
extern SDL_AppResult SDLCALL SDL_AppEvent(void *appstate, SDL_Event *event);
extern void SDLCALL SDL_AppQuit(void *appstate, SDL_AppResult result);
#endif  /* SDL_MAIN_USE_CALLBACKS */

typedef int (SDLCALL *SDL_main_func)(int argc, char *argv[]);

/**
 * The prototype for the application's main() function (when renamed).
 */
extern int SDLCALL SDL_main(int argc, char *argv[]);

/**
 * Circumvent failure of SDL_Init() when not using SDL_main() as an entry
 * point. (No-op on SDLop's platforms, like SDL3 on desktop platforms.)
 */
extern void SDLCALL SDL_SetMainReady(void);

/**
 * Initializes and launches an SDL application, by calling platform-specific
 * init code then the provided main function. On SDLop's platforms this is a
 * direct call, exactly like SDL3's generic SDL_RunApp().
 *
 * \param argc the argc parameter from the process entry point, or 0.
 * \param argv the argv parameter from the process entry point, or NULL.
 * \param mainFunction the SDL app's C-style main(), or NULL to fail.
 * \param reserved should be NULL (reserved for future use).
 * \returns the return value from mainFunction: 0 on success, otherwise
 *          failure; SDL_GetError() might have more information.
 */
extern int SDLCALL SDL_RunApp(int argc, char *argv[], SDL_main_func mainFunction, void *reserved);

/**
 * An entry point for SDL's use in SDL_MAIN_USE_CALLBACKS apps.
 *
 * Rarely needed directly (SDL_main_impl.h wires it up); call it from your
 * own entry point if you manage the process entry yourself. Runs the
 * callback loop on the calling thread and returns when the app quits.
 *
 * \returns 0 on success (or SDL_APP_SUCCESS), 1 on failure.
 */
extern int SDLCALL SDL_EnterAppMainCallbacks(int argc, char *argv[], SDL_AppInit_func appinit, SDL_AppIterate_func appiter, SDL_AppEvent_func appevent, SDL_AppQuit_func appquit);

#ifdef __cplusplus
}
#endif

#include <SDL3/SDL_close_code.h>

#if !defined(SDL_MAIN_HANDLED) && !defined(SDL_MAIN_NOIMPL)
    #if defined(SDL_MAIN_USE_CALLBACKS)
        /* header-only main() implementation driving the callbacks */
        #include <SDL3/SDL_main_impl.h>
    #endif
#endif

#endif /* SDL_main_h_ */
