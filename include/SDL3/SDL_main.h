/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  SDL_main.h exists to make main() portable, exactly like upstream:

    * By default, including this header (after SDL.h) renames your main() to
      SDL_main() and puts a platform entry point in this translation unit, which
      calls SDL_RunApp() -> your main().
    * Define SDL_MAIN_USE_CALLBACKS to write SDL_AppInit/SDL_AppIterate/
      SDL_AppEvent/SDL_AppQuit callbacks instead of a main() function.
    * Define SDL_MAIN_HANDLED to keep control of main() yourself; the library
      then does no entry-point juggling at all.

  Unlike upstream, SDLop's version is small: the interesting cases are Linux,
  and on Linux both paths reduce to "call the app from main()".

  Macro/function names match SDL3 3.2.10 (zlib licence, Copyright (C) 1997-2025
  Sam Lantinga and SDL contributors).
*/

#ifndef SDL_main_h_
#define SDL_main_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_events.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The entry point of a SDLop app on a platform that needs one. */
typedef int (SDLCALL *SDL_main_func)(int argc, char *argv[]);

/**
 * Run an application from a platform entry point.
 *
 * This is what the generated main() in this header calls; on Linux it simply
 * calls `mainFunction`. It is declared here because it is part of SDL3's API.
 */
extern SDL_DECLSPEC int SDLCALL SDL_RunApp(int argc, char *argv[], SDL_main_func mainFunction, void *reserved);

/**
 * App-implementation entry point, for the SDL_MAIN_USE_CALLBACKS style.
 */
extern SDL_DECLSPEC int SDLCALL SDL_EnterAppMainCallbacks(int argc, char *argv[],
                                                          SDL_AppInit_func appinit,
                                                          SDL_AppIterate_func appiter,
                                                          SDL_AppEvent_func appevent,
                                                          SDL_AppQuit_func appquit);

#ifdef __cplusplus
}
#endif

/* If the platform isn't using SDL_MAIN_HANDLED, hook things up. */
#ifndef SDL_MAIN_HANDLED

#ifdef SDL_MAIN_USE_CALLBACKS
/* The app provides SDL_AppInit/SDL_AppIterate/SDL_AppEvent/SDL_AppQuit. */
#else
/* The app provides main(), which we rename and call from the real entry point. */
#ifdef main
#undef main
#endif
#define main SDL_main
extern int SDL_main(int argc, char *argv[]);
#endif

/* The entry point itself. SDLop's platforms all use the plain C entry point,
   so this is emitted into whichever translation unit includes SDL_main.h. */
#if defined(__cplusplus)
extern "C"
#endif
int main(int argc, char *argv[])
{
#ifdef SDL_MAIN_USE_CALLBACKS
    return SDL_EnterAppMainCallbacks(argc, argv, SDL_AppInit, SDL_AppIterate, SDL_AppEvent, SDL_AppQuit);
#else
    return SDL_RunApp(argc, argv, SDL_main, (void *)0);
#endif
}

#endif /* !SDL_MAIN_HANDLED */

#endif /* SDL_main_h_ */
