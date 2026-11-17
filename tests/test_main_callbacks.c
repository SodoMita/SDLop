/*
  SDLop test: callback-based app entry (SDL_EnterAppMainCallbacks,
  SDL_RunApp, SDL_SetMainReady). Headless; no video needed.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int inits, iterates, events_seen, quits;
static SDL_AppResult last_quit_result = SDL_APP_CONTINUE;
static int iterate_limit;
static SDL_AppResult iterate_result;
static Uint32 test_event_type;

static SDL_AppResult SDLCALL test_init(void **appstate, int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    inits++;
    *appstate = (void *)"the-state";

    /* push an event the event callback should see before iterate #1 ends */
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = test_event_type;
    SDL_PushEvent(&ev);
    return SDL_APP_CONTINUE;
}

static SDL_AppResult SDLCALL test_iterate(void *appstate)
{
    assert(appstate == (void *)"the-state");
    iterates++;
    if (iterates >= iterate_limit) {
        return iterate_result;
    }
    return SDL_APP_CONTINUE;
}

static SDL_AppResult SDLCALL test_event(void *appstate, SDL_Event *event)
{
    assert(appstate == (void *)"the-state");
    if (event->type == test_event_type) {
        events_seen++;
    }
    return SDL_APP_CONTINUE;
}

static void SDLCALL test_quit(void *appstate, SDL_AppResult result)
{
    assert(appstate == (void *)"the-state");
    (void)appstate;
    quits++;
    last_quit_result = result;
}

static void reset_callbacks(void)
{
    inits = iterates = events_seen = quits = 0;
    last_quit_result = SDL_APP_CONTINUE;
}

static int SDLCALL fake_main_check_argv(int argc, char *argv[])
{
    /* SDL_RunApp(0, NULL, ...) must pass a valid dummy argv */
    assert(argc == 1);
    assert(argv && argv[0] && argv[1] == NULL);
    return 23;
}

static SDL_AppResult SDLCALL failing_init(void **appstate, int argc, char *argv[])
{
    (void)appstate;
    (void)argc;
    (void)argv;
    inits++;
    return SDL_APP_FAILURE;
}

static SDL_AppResult SDLCALL failing_iterate(void *appstate)
{
    (void)appstate;
    iterates++;
    return SDL_APP_FAILURE;
}

static void SDLCALL silent_quit(void *appstate, SDL_AppResult result)
{
    (void)appstate;
    quits++;
    last_quit_result = result;
}

static int SDLCALL fake_main_passthrough(int argc, char *argv[])
{
    assert(argc == 2);
    assert(strcmp(argv[0], "prog") == 0 && strcmp(argv[1], "arg") == 0);
    return 0;
}

int main(void)
{
    test_event_type = SDL_RegisterEvents(1);
    assert(test_event_type >= SDL_EVENT_USER);

    SDL_SetMainReady(); /* no-op, must not crash */

    /* --- SDL_RunApp --- */
    assert(SDL_RunApp(0, NULL, fake_main_check_argv, NULL) == 23);
    {
        char *args[] = { (char *)"prog", (char *)"arg", NULL };
        assert(SDL_RunApp(2, args, fake_main_passthrough, NULL) == 0);
    }

    /* --- success run: 5 iterations, one event, SUCCESS exit code 0 --- */
    reset_callbacks();
    iterate_limit = 5;
    iterate_result = SDL_APP_SUCCESS;
    int rc = SDL_EnterAppMainCallbacks(0, NULL, test_init, test_iterate, test_event, test_quit);
    assert(rc == 0);
    assert(inits == 1);
    assert(iterates == 5);
    assert(events_seen == 1);
    assert(quits == 1);
    assert(last_quit_result == SDL_APP_SUCCESS);
    assert(SDL_WasInit(0) == 0); /* EnterAppMainCallbacks ran SDL_Quit() */

    /* --- failure from iterate: exit code 1, quit sees FAILURE --- */
    reset_callbacks();
    iterate_limit = 2;
    iterate_result = SDL_APP_FAILURE;
    rc = SDL_EnterAppMainCallbacks(0, NULL, test_init, test_iterate, test_event, test_quit);
    assert(rc == 1);
    assert(iterates == 2);
    assert(quits == 1);
    assert(last_quit_result == SDL_APP_FAILURE);

    /* --- failure straight from init: loop never runs --- */
    reset_callbacks();
    rc = SDL_EnterAppMainCallbacks(0, NULL, failing_init, failing_iterate, test_event, silent_quit);
    assert(rc == 1);
    assert(inits == 1);
    assert(iterates == 0);          /* loop was never entered */
    assert(quits == 1);
    assert(last_quit_result == SDL_APP_FAILURE);

    /* --- SDL_MAIN_CALLBACK_RATE paces the loop (wall-clock sanity) --- */
    reset_callbacks();
    iterate_limit = 3;
    iterate_result = SDL_APP_SUCCESS;
    SDL_setenv_unsafe(SDL_HINT_MAIN_CALLBACK_RATE, "500", 1); /* 2 ms/frame */
    {
        Uint64 start = SDL_GetTicks();
        rc = SDL_EnterAppMainCallbacks(0, NULL, test_init, test_iterate, test_event, test_quit);
        Uint64 elapsed = SDL_GetTicks() - start;
        assert(rc == 0);
        assert(iterates == 3);
        assert(elapsed >= 3); /* pacing actually slept between iterations */
    }
    SDL_unsetenv_unsafe(SDL_HINT_MAIN_CALLBACK_RATE);

    printf("test_main_callbacks: OK\n");
    return 0;
}
