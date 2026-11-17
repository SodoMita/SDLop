/*
  SDLop - application entry points (see <SDL3/SDL_main.h>).

  Ported from SDL3 3.2.10: src/main/SDL_runapp.c (generic variant),
  src/main/SDL_main_callbacks.c and src/main/generic/SDL_sysmain_callbacks.c.

  Differences from SDL3, all of them consequences of SDLop's lean event
  queue: SDL3 dispatches lifecycle events (TERMINATING, LOW_MEMORY, ...)
  immediately from an internal event watcher; SDLop has no event-watch API,
  so every event is dispatched in queue order from the main loop instead.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_hints.h>

/* ------------------------------------------------------------------ */
/* SDL_RunApp (generic desktop variant)                                */
/* ------------------------------------------------------------------ */

int SDL_RunApp(int argc, char *argv[], SDL_main_func mainFunction, void *reserved)
{
    (void)reserved;

    if (!argv) {
        /* make sure argv isn't NULL, in case some user code doesn't like that */
        static char dummyargv0[] = { 'S', 'D', 'L', '_', 'a', 'p', 'p', '\0' };
        static char *argvdummy[2] = { dummyargv0, NULL };
        argc = 1;
        argv = argvdummy;
    }

    return mainFunction(argc, argv);
}

void SDL_SetMainReady(void)
{
    /* nothing to do on SDLop's platforms (like SDL3 on desktop Linux) */
}

/* ------------------------------------------------------------------ */
/* Callback-driven main loop (SDL_MAIN_USE_CALLBACKS)                  */
/* ------------------------------------------------------------------ */

static SDL_AppEvent_func SDL_main_event_callback;
static SDL_AppIterate_func SDL_main_iteration_callback;
static SDL_AppQuit_func SDL_main_quit_callback;
static SDL_AtomicInt apprc; /* use an atomic, since events might land from any thread */
static void *SDL_main_appstate;

static void SDL_DispatchMainCallbackEvent(SDL_Event *event)
{
    if (SDL_GetAtomicInt(&apprc) == SDL_APP_CONTINUE) { /* if already quitting, don't send the event to the app. */
        SDL_CompareAndSwapAtomicInt(&apprc, SDL_APP_CONTINUE, (int)SDL_main_event_callback(SDL_main_appstate, event));
    }
}

static void SDL_DispatchMainCallbackEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        SDL_DispatchMainCallbackEvent(&event);
    }
}

static SDL_AppResult SDLOP_InitMainCallbacks(int argc, char *argv[], SDL_AppInit_func appinit, SDL_AppIterate_func appiter, SDL_AppEvent_func appevent, SDL_AppQuit_func appquit)
{
    SDL_main_iteration_callback = appiter;
    SDL_main_event_callback = appevent;
    SDL_main_quit_callback = appquit;
    SDL_SetAtomicInt(&apprc, SDL_APP_CONTINUE);

    const SDL_AppResult rc = appinit(&SDL_main_appstate, argc, argv);
    if (SDL_CompareAndSwapAtomicInt(&apprc, SDL_APP_CONTINUE, (int)rc) && (rc == SDL_APP_CONTINUE)) { /* bounce if SDL_AppInit already said abort, otherwise... */
        /* make sure we definitely have events initialized, even if the app didn't do it. */
        if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
            SDL_SetAtomicInt(&apprc, SDL_APP_FAILURE);
            return SDL_APP_FAILURE;
        }
    }

    return (SDL_AppResult)SDL_GetAtomicInt(&apprc);
}

static SDL_AppResult SDLOP_IterateMainCallbacks(bool pump_events)
{
    if (pump_events) {
        SDL_PumpEvents();
    }
    SDL_DispatchMainCallbackEvents();

    SDL_AppResult rc = (SDL_AppResult)SDL_GetAtomicInt(&apprc);
    if (rc == SDL_APP_CONTINUE) {
        rc = SDL_main_iteration_callback(SDL_main_appstate);
        if (!SDL_CompareAndSwapAtomicInt(&apprc, SDL_APP_CONTINUE, (int)rc)) {
            rc = (SDL_AppResult)SDL_GetAtomicInt(&apprc); /* something else already set a quit result, keep that. */
        }
    }
    return rc;
}

static void SDLOP_QuitMainCallbacks(SDL_AppResult result)
{
    SDL_main_quit_callback(SDL_main_appstate, result);
    SDL_main_appstate = NULL; /* just in case. */

    SDL_Quit();
}

/* SDLop reads the hint's backing environment variable directly (there is
   no hint database); the value is fixed for the loop's lifetime. */
static void read_callback_rate_hint(int *callback_rate_increment, bool *iterate_after_waitevent)
{
    *iterate_after_waitevent = false;
    *callback_rate_increment = 0;

    const char *value = SDL_getenv_unsafe(SDL_HINT_MAIN_CALLBACK_RATE);
    if (value) {
        if (SDL_strcmp(value, "waitevent") == 0) {
            *iterate_after_waitevent = true;
        } else {
            const int callback_rate = SDL_atoi(value);
            if (callback_rate > 0) {
                *callback_rate_increment = (int)(1000000000ull / (Uint64)callback_rate);
            }
        }
    }
}

int SDL_EnterAppMainCallbacks(int argc, char *argv[], SDL_AppInit_func appinit, SDL_AppIterate_func appiter, SDL_AppEvent_func appevent, SDL_AppQuit_func appquit)
{
    int callback_rate_increment;
    bool iterate_after_waitevent;
    read_callback_rate_hint(&callback_rate_increment, &iterate_after_waitevent);

    SDL_AppResult rc = SDLOP_InitMainCallbacks(argc, argv, appinit, appiter, appevent, appquit);
    if (rc == SDL_APP_CONTINUE) {
        Uint64 next_iteration = callback_rate_increment ? (SDL_GetTicksNS() + (Uint64)callback_rate_increment) : 0;

        for (;;) {
            if (iterate_after_waitevent) {
                SDL_WaitEvent(NULL);
            }
            rc = SDLOP_IterateMainCallbacks(!iterate_after_waitevent);
            if (rc != SDL_APP_CONTINUE) {
                break;
            }

            /* Try to run at whatever rate the hint requested. This makes this
               not eat all the CPU in simple things like loopwave. By
               default, we run as fast as possible, which means we'll clamp to
               vsync in common cases, and won't be restrained to vsync if the
               app is doing a benchmark or doesn't want to be, based on how
               they've set up that window. */
            if (callback_rate_increment == 0) {
                next_iteration = 0; /* just clear the timer and run at the pace the video subsystem allows. */
            } else {
                const Uint64 now = SDL_GetTicksNS();
                if (next_iteration > now) { /* Running faster than the limit, sleep a little. */
                    SDL_DelayNS(next_iteration - now);
                } else {
                    next_iteration = now; /* if running behind, reset the timer. */
                }
                next_iteration += (Uint64)callback_rate_increment;
            }
        }
    }
    SDLOP_QuitMainCallbacks(rc);

    return (rc == SDL_APP_FAILURE) ? 1 : 0;
}
