/*
  SDLop test: SDL_MAIN_USE_CALLBACKS through the real SDL_main.h entry point
  (main() comes from SDL_main_impl.h and calls SDL_EnterAppMainCallbacks).
  Checks call order, event delivery to SDL_AppEvent, SDL_HINT_MAIN_CALLBACK_RATE
  pacing and that SDL_AppQuit receives the result.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct
{
    int iterations;
    int user_events;
    Uint64 start_ns;
} AppState;

static AppState state;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    if (argc < 1 || !argv[0]) {
        return SDL_APP_FAILURE;
    }
    *appstate = &state;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "200"); /* 5 ms per iteration */
    if (!SDL_IsMainThread()) {
        return SDL_APP_FAILURE;
    }
    state.start_ns = SDL_GetTicksNS();
    return SDL_Init(SDL_INIT_VIDEO) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    AppState *s = (AppState *)appstate;
    s->iterations++;
    if (s->iterations <= 3) {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_USER;
        SDL_PushEvent(&e);
    } else if (s->iterations == 10) {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&e);
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    AppState *s = (AppState *)appstate;
    if (event->type == SDL_EVENT_USER) {
        s->user_events++;
    }
    return event->type == SDL_EVENT_QUIT ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    AppState *s = (AppState *)appstate;
    const double ms = (double)(SDL_GetTicksNS() - s->start_ns) / 1e6;
    int ok = result == SDL_APP_SUCCESS && s->iterations == 10 && s->user_events == 3 && ms >= 40.0;
    printf("test_main_callbacks: %s (result=%d iterations=%d user_events=%d elapsed=%.1fms)\n",
           ok ? "PASS" : "FAIL", (int)result, s->iterations, s->user_events, ms);
    if (!ok) {
        exit(1); /* SDL_EnterAppMainCallbacks would return 0 for SUCCESS */
    }
}
