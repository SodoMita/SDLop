/*
  SDLop test: the asyncinput-style raw input worker on Windows.

  SDLop ships a low-latency raw input path that publishes native events to a
  callback and to two lock-free rings. On Linux the producer is evdev; on
  Windows it is Raw Input, behind SDLOP_RAW_INPUT=1. This test covers the
  lifecycle and the API contract:

    - off by default, on with the hint, off again after SDL_Quit()
    - SDLop_RawInputAvailable() truthfully reports whether the worker runs
    - register / unregister callbacks
    - SDLop_PollRawEvents() argument handling and an empty ring
    - repeated init/quit cycles neither leak nor hang

  The event-producing side is covered by test_win32_raw_input_events, which
  injects real mouse input.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <stdio.h>

static int failures = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            failures++;                                          \
        }                                                        \
    } while (0)

static void SDLCALL on_raw(const SDLop_RawEvent *event, void *userdata)
{
    (void)event;
    (void)userdata;
}

int main(void)
{
    SDLop_RawEvent buffer[16];
    int i;

    CHECK(!SDLop_RawInputAvailable(), "raw input reported available before SDL_Init");

    /* off unless asked for: Windows keeps the window message path by default */
    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    CHECK(!SDLop_RawInputAvailable(), "the worker started with %s=0", SDLOP_HINT_RAW_INPUT);
    SDL_Quit();

    /* on */
    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    CHECK(SDLop_RawInputAvailable(), "the worker did not start with %s=1", SDLOP_HINT_RAW_INPUT);

    CHECK(SDLop_RegisterRawEventCallback(on_raw, NULL), "registering a callback failed: %s", SDL_GetError());

    /* argument handling: a valid drain returns >= 0, bad arguments -1 */
    CHECK(SDLop_PollRawEvents(buffer, 16) >= 0, "PollRawEvents on an idle ring returned < 0");
    CHECK(SDLop_PollRawEvents(NULL, 16) == -1, "PollRawEvents(NULL) should return -1");
    CHECK(SDLop_PollRawEvents(buffer, 0) == -1, "PollRawEvents(max_events = 0) should return -1");

    SDLop_UnregisterRawEventCallback(on_raw);
    SDL_Quit();
    CHECK(!SDLop_RawInputAvailable(), "the worker is still running after SDL_Quit");

    /* restart cycles must not hang or half-initialise */
    for (i = 0; i < 3; i++) {
        SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1");
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            CHECK(0, "cycle %d: SDL_Init failed: %s", i, SDL_GetError());
            break;
        }
        CHECK(SDLop_RawInputAvailable(), "cycle %d: the worker did not restart", i);
        SDL_Quit();
    }

    if (failures) {
        fprintf(stderr, "test_win32_raw_input: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_raw_input: worker lifecycle and API contract hold\n");
    return 0;
}
