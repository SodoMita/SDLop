/*
  SDLop -- SDL_main.h.

  On Linux there is nothing to do: the platform entry point is main(), so
  SDL_RunApp() just calls the application's main(). SDL3 needs a whole file of
  platform-specific scaffolding here (WinMain, SDLMain.m, Android JNI hooks);
  the SDLop build defines only the Linux rules and returns "unsupported"
  elsewhere, which is honest and keeps the file at 40 lines.

  SDL_main.h's #define main SDL_main trick means an application that includes
  <SDL3/SDL_main.h> and defines main() actually defines SDL_main(), and the
  wrapper below is what the C runtime calls.
*/

#include "../sdlop_internal.h"
#include <SDL3/SDL_main.h>

int SDL_RunApp(int argc, char *argv[], SDL_main_func mainFunction, void *reserved)
{
    (void)reserved;
    return mainFunction(argc, argv);
}

int SDL_EnterAppMainCallbacks(int argc, char *argv[], SDL_AppInit_func appinit,
                              SDL_AppIterate_func appiter, SDL_AppEvent_func appevent,
                              SDL_AppQuit_func appquit)
{
    SDL_AppResult result;
    void *appstate = NULL;
    SDL_Event event;

    result = appinit(&appstate, argc, argv);
    if (result == SDL_APP_FAILURE) {
        if (appquit) {
            appquit(appstate, result);
        }
        return 1;
    }
    if (result == SDL_APP_SUCCESS) {
        if (appquit) {
            appquit(appstate, result);
        }
        return 0;
    }

    for (;;) {
        if (!SDL_PollEvent(&event)) {
            result = appiter(&appstate);
        } else {
            result = appevent(&appstate, &event);
        }
        if (result == SDL_APP_SUCCESS || result == SDL_APP_FAILURE) {
            break;
        }
    }
    if (appquit) {
        appquit(appstate, result);
    }
    return result == SDL_APP_SUCCESS ? 0 : 1;
}
