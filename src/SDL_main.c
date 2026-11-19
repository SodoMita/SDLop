/*
  SDLop - SDL_main.h support: SDL_RunApp(), SDL_SetMainReady() and the
  SDL_MAIN_USE_CALLBACKS entry point SDL_EnterAppMainCallbacks().

  Port of SDL3's src/main/SDL_main_callbacks.c and the generic
  SDL_sysmain_callbacks.c (release-3.4.20): events are delivered to
  SDL_AppEvent() from the main loop (lifecycle events immediately, through
  an event watch), SDL_AppIterate() runs as fast as possible or at
  SDL_HINT_MAIN_CALLBACK_RATE ("waitevent" = once per event), and
  SDL_AppQuit() is followed by SDL_Quit().

  Copyright (C) 1997-2026 Sam Lantinga (SDL3 parts), zlib license.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#define SDL_MAIN_NOIMPL /* types and declarations only, no main() */
#include <SDL3/SDL_main.h>

static bool main_ready;

void SDL_SetMainReady(void)
{
    main_ready = true;
    SDLOP_InitMainThread();
}

static void check_default_argv(int *argc, char ***argv)
{
    if (!*argv) {
        static char dummyargv0[] = "SDL_app";
        static char *argvdummy[2] = { dummyargv0, NULL };
        *argc = 1;
        *argv = argvdummy;
    }
}

int SDL_RunApp(int argc, char *argv[], SDL_main_func mainFunction, void *reserved)
{
    (void)reserved;
    check_default_argv(&argc, &argv);
    SDL_SetMainReady();
    return mainFunction(argc, argv);
}

/* ------------------------------------------------------------------ */
/* Main callbacks                                                      */
/* ------------------------------------------------------------------ */

static SDL_AppEvent_func main_event_callback;
static SDL_AppIterate_func main_iteration_callback;
static SDL_AppQuit_func main_quit_callback;
static SDL_AtomicInt apprc; /* moves away from CONTINUE exactly once */
static void *main_appstate;

static bool should_dispatch_immediately(const SDL_Event *event)
{
    switch (event->type) {
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_LOW_MEMORY:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        return true;
    default:
        return false;
    }
}

static void dispatch_event(SDL_Event *event)
{
    if (SDL_GetAtomicInt(&apprc) == SDL_APP_CONTINUE) {
        SDL_CompareAndSwapAtomicInt(&apprc, SDL_APP_CONTINUE, main_event_callback(main_appstate, event));
    }
}

static void dispatch_queued_events(void)
{
    SDL_Event events[16];
    for (;;) {
        int count = SDL_PeepEvents(events, (int)SDL_arraysize(events), SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST);
        if (count <= 0) {
            break;
        }
        for (int i = 0; i < count; ++i) {
            if (!should_dispatch_immediately(&events[i])) {
                dispatch_event(&events[i]);
            }
        }
    }
}

static bool SDLCALL main_callback_event_watcher(void *userdata, SDL_Event *event)
{
    (void)userdata;
    if (should_dispatch_immediately(event)) {
        dispatch_queued_events();
        dispatch_event(event);
        if (event->type == SDL_EVENT_TERMINATING) {
            SDL_CompareAndSwapAtomicInt(&apprc, SDL_APP_CONTINUE, SDL_APP_SUCCESS);
        }
    }
    return true;
}

static SDL_AppResult init_main_callbacks(int argc, char *argv[], SDL_AppInit_func appinit, SDL_AppIterate_func appiter,
                                         SDL_AppEvent_func appevent, SDL_AppQuit_func appquit)
{
    main_iteration_callback = appiter;
    main_event_callback = appevent;
    main_quit_callback = appquit;
    SDL_SetAtomicInt(&apprc, SDL_APP_CONTINUE);

    const SDL_AppResult rc = appinit(&main_appstate, argc, argv);
    if (SDL_CompareAndSwapAtomicInt(&apprc, SDL_APP_CONTINUE, rc) && rc == SDL_APP_CONTINUE) {
        /* events must work even if the app didn't initialize them */
        if (!SDL_InitSubSystem(SDL_INIT_EVENTS) || !SDL_AddEventWatch(main_callback_event_watcher, NULL)) {
            SDL_SetAtomicInt(&apprc, SDL_APP_FAILURE);
            return SDL_APP_FAILURE;
        }
    }
    return (SDL_AppResult)SDL_GetAtomicInt(&apprc);
}

static SDL_AppResult iterate_main_callbacks(bool pump_events)
{
    if (pump_events) {
        SDL_PumpEvents();
    }
    dispatch_queued_events();
    SDL_AppResult rc = (SDL_AppResult)SDL_GetAtomicInt(&apprc);
    if (rc == SDL_APP_CONTINUE) {
        rc = main_iteration_callback(main_appstate);
        if (!SDL_CompareAndSwapAtomicInt(&apprc, SDL_APP_CONTINUE, rc)) {
            rc = (SDL_AppResult)SDL_GetAtomicInt(&apprc); /* keep an earlier quit result */
        }
    }
    return rc;
}

static void quit_main_callbacks(SDL_AppResult result)
{
    SDL_RemoveEventWatch(main_callback_event_watcher, NULL);
    main_quit_callback(main_appstate, result);
    main_appstate = NULL;
    SDL_Quit();
}

static Uint64 callback_rate_increment;
static bool iterate_after_waitevent;

static void SDLCALL main_callback_rate_changed(void *userdata, const char *name, const char *oldValue, const char *newValue)
{
    (void)userdata;
    (void)name;
    (void)oldValue;
    iterate_after_waitevent = newValue && SDL_strcmp(newValue, "waitevent") == 0;
    if (iterate_after_waitevent) {
        callback_rate_increment = 0;
    } else {
        const double rate = newValue ? SDL_atof(newValue) : 0.0;
        callback_rate_increment = (rate > 0.0) ? (Uint64)((double)SDL_NS_PER_SECOND / rate) : 0;
    }
}

int SDL_EnterAppMainCallbacks(int argc, char *argv[], SDL_AppInit_func appinit, SDL_AppIterate_func appiter,
                              SDL_AppEvent_func appevent, SDL_AppQuit_func appquit)
{
    check_default_argv(&argc, &argv);
    SDL_SetMainReady();
    SDL_AppResult rc = init_main_callbacks(argc, argv, appinit, appiter, appevent, appquit);
    if (rc == SDL_APP_CONTINUE) {
        SDL_AddHintCallback(SDL_HINT_MAIN_CALLBACK_RATE, main_callback_rate_changed, NULL);
        Uint64 next_iteration = callback_rate_increment ? (SDL_GetTicksNS() + callback_rate_increment) : 0;
        for (;;) {
            if (iterate_after_waitevent) {
                SDL_WaitEvent(NULL);
            }
            rc = iterate_main_callbacks(!iterate_after_waitevent);
            if (rc != SDL_APP_CONTINUE) {
                break;
            }
            if (callback_rate_increment == 0) {
                next_iteration = 0;
            } else {
                const Uint64 now = SDL_GetTicksNS();
                if (next_iteration > now) {
                    SDL_DelayPrecise(next_iteration - now);
                } else {
                    next_iteration = now;
                }
                next_iteration += callback_rate_increment;
            }
        }
        SDL_RemoveHintCallback(SDL_HINT_MAIN_CALLBACK_RATE, main_callback_rate_changed, NULL);
    }
    quit_main_callbacks(rc);
    return (rc == SDL_APP_FAILURE) ? 1 : 0;
}
