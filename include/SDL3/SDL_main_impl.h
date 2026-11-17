/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Header-only main() implementation (API compatible with
  <SDL3/SDL_main_impl.h>, generic-platform variant).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

/* WIKI CATEGORY: Main */

#ifndef SDL_main_impl_h_
#define SDL_main_impl_h_

#ifndef SDL_main_h_
#error "This header should not be included directly, but only via SDL_main.h!"
#endif

/* if someone wants to include SDL_main.h but doesn't want the main handing magic,
   they can #define SDL_MAIN_HANDLED first. SDL_MAIN_NOIMPL is for SDL-internal
   usage. */
#if !defined(SDL_MAIN_HANDLED) && !defined(SDL_MAIN_NOIMPL)

    /* the implementation below must be able to use the real main(), nothing
       renamed (the user's main() will be renamed to SDL_main so it can be
       called from here) */
    #ifdef main
        #undef main
    #endif

    #ifdef SDL_MAIN_USE_CALLBACKS

        /* SDLop only targets platforms where a plain main() entry exists, so
           this is always the "standard" callback entry point. The app defines
           SDL_AppInit & friends and SHOULD NOT also supply SDL_main/main. */
        #define SDL_MAIN_CALLBACK_STANDARD 1

        int SDL_main(int argc, char **argv)
        {
            return SDL_EnterAppMainCallbacks(argc, argv, SDL_AppInit, SDL_AppIterate, SDL_AppEvent, SDL_AppQuit);
        }

    #endif  /* SDL_MAIN_USE_CALLBACKS */

    #if (!defined(SDL_MAIN_USE_CALLBACKS) || defined(SDL_MAIN_CALLBACK_STANDARD)) && !defined(SDL_MAIN_EXPORTED)

        /* standard-conforming platform: real main() just forwards to SDL_RunApp */
        int main(int argc, char *argv[])
        {
            return SDL_RunApp(argc, argv, SDL_main, NULL);
        }

    #endif /* !defined(SDL_MAIN_USE_CALLBACKS) || defined(SDL_MAIN_CALLBACK_STANDARD) */

    /* rename users main() function to SDL_main() so it can be called from the wrapper above */
    #define main    SDL_main

#endif /* SDL_MAIN_HANDLED */

#endif /* SDL_main_impl_h_ */
