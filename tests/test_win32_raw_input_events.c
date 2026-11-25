/*
  SDLop test: real events through the Windows raw input worker.

  Two things are checked that the API test cannot see:

    1. Injected mouse input reaches the raw rings and the callbacks, with
       relative motion and the wheel in the units SDLop uses elsewhere
       (evdev's: dx/dy in device counts, wheel in notches, 1 per detent).
    2. With the worker running, the SDL event queue gets exactly one motion
       event per packet. The window message path must stand down, or every
       movement arrives twice.

  Injection uses mouse_event(), which the OS turns into genuine Raw Input, so
  this exercises the real producer rather than a stub.

  Note on timestamps: the worker uses QueryPerformanceCounter and offsets it
  onto the SDLOP_MonotonicNS() origin, so an event's timestamp_ns is
  comparable with SDL_GetTicksNS().

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include "internal/sdlop_internal.h"
#include <windows.h>
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

static volatile LONG motion_events;
static volatile LONG wheel_events;

static void SDLCALL on_raw(const SDLop_RawEvent *event, void *userdata)
{
    (void)userdata;
    if (event->type == SDLop_EV_REL) {
        if (event->code == SDLop_REL_X || event->code == SDLop_REL_Y) {
            InterlockedIncrement(&motion_events);
        } else if (event->code == SDLop_REL_WHEEL) {
            InterlockedIncrement(&wheel_events);
        }
    }
}

int main(void)
{
    SDL_Window *window;
    LONG before_motion, before_wheel;
    int queue_motion = 0;
    float queue_wheel = 0.0f;
    int i;

    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 raw input events", 400, 300, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_ShowWindow(window);
    SDL_SetWindowPosition(window, 100, 100);
    for (i = 0; i < 20; i++) {
        SDL_PumpEvents();
        Sleep(10);
    }
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

    if (!SDLop_RegisterRawEventCallback(on_raw, NULL)) {
        fprintf(stderr, "SDLop_RegisterRawEventCallback failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* the pump only forwards pointer input while the window owns it */
    sdlop.mouse_focus = window;
    SDL_PumpEvents();

    before_motion = motion_events;
    before_wheel = wheel_events;

    SetCursorPos(300, 300);
    for (i = 0; i < 10; i++) {
        mouse_event(MOUSEEVENTF_MOVE, 5, 0, 0, 0);
    }
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, WHEEL_DELTA, 0);

    for (i = 0; i < 80; i++) {
        SDL_Event e;
        SDL_PumpEvents();
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_MOUSE_MOTION &&
                (e.motion.xrel != 0.0f || e.motion.yrel != 0.0f)) {
                queue_motion++;
            }
            if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                queue_wheel += e.wheel.y;
            }
        }
        Sleep(10);
    }

    CHECK(motion_events > before_motion,
          "no relative motion reached the raw callbacks (%ld)", (long)(motion_events - before_motion));
    CHECK(wheel_events > before_wheel,
          "no wheel event reached the raw callbacks");

    /* exactly one SDL motion event per motion packet, not two: the window
     * message path has to stand down while raw input is the source */
    CHECK(queue_motion > 0, "no relative motion reached the SDL event queue");
    CHECK(queue_motion <= (int)(motion_events - before_motion) + 2,
          "%d SDL motion events for %ld raw packets -- input is being "
          "delivered twice", queue_motion, (long)(motion_events - before_motion));

    /* one detent is 1.0 in SDL units */
    CHECK((int)queue_wheel == 1, "wheel totalled %.0f, want 1 notch", queue_wheel);

    SDLop_UnregisterRawEventCallback(on_raw);
    sdlop.mouse_focus = NULL;
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_raw_input_events: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_raw_input_events: motion and wheel arrive once, in evdev units\n");
    return 0;
}
