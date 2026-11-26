/*
  SDLop test: evdev report-frame aggregation and wheel capability selection.

  The fake-device test API feeds raw records straight into the main-thread
  translator (no uinput needed). SDL3 emits one motion per SYN_REPORT with both
  axes together, and ignores legacy wheel records from devices that also
  report high-resolution wheel events; this pins both behaviours.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include "internal/sdlop_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
            failures++;                                         \
        }                                                       \
    } while (0)

static void feed(int dev, Uint16 type, Uint16 code, Sint32 value)
{
    SDLop_RawEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.device = (Uint32)dev;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ev.timestamp_ns = 1;
    SDLOP_RawInputTestFeed(&ev, false);
}

static void drain(void)
{
    SDL_Event ev;
    SDL_PumpEvents();
    while (SDL_PollEvent(&ev)) {
    }
}

int main(void)
{
    SDL_Event ev;
    int motions, wheels;
    float xrel, yrel, wheel_x, wheel_y;

    /* The fake-device API works only while the raw worker is stopped. */
    setenv("SDLOP_RAW_INPUT", "0", 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    drain();

    /* --- one motion per report, both axes together -------------------- */
    {
        const int dev = SDLOP_RawInputTestAddDevice(false, true, false, 0, 0, 0, 0, false);
        if (dev < 0) {
            fprintf(stderr, "test_evdev_frames: cannot add a fake device\n");
            SDL_Quit();
            return 2;
        }
        feed(dev, SDLop_EV_REL, SDLop_REL_X, 10);
        feed(dev, SDLop_EV_REL, SDLop_REL_Y, -5);
        SDL_PumpEvents();
        motions = 0;
        xrel = yrel = 0.0f;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                motions++;
                xrel += ev.motion.xrel;
                yrel += ev.motion.yrel;
            }
        }
        CHECK(motions == 0, "no motion before SYN_REPORT (got %d)", motions);
        feed(dev, SDLop_EV_SYN, SDLop_SYN_REPORT, 0);
        SDL_PumpEvents();
        motions = 0;
        xrel = yrel = 0.0f;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                motions++;
                xrel += ev.motion.xrel;
                yrel += ev.motion.yrel;
            }
        }
        CHECK(motions == 1, "one motion for a two-axis frame, got %d", motions);
        CHECK(xrel == 10.0f && yrel == -5.0f,
              "frame carried (%g, %g), want (10, -5)", xrel, yrel);
        drain();

        /* --- legacy wheel: no high-res capability, so it is delivered --- */
        feed(dev, SDLop_EV_REL, SDLop_REL_WHEEL, 1);
        feed(dev, SDLop_EV_SYN, SDLop_SYN_REPORT, 0);
        SDL_PumpEvents();
        wheels = 0;
        wheel_y = 0.0f;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                wheels++;
                wheel_y += ev.wheel.y;
            }
        }
        CHECK(wheels == 1 && wheel_y == 1.0f,
              "legacy wheel: %d events, y=%g, want 1 event with y=1", wheels, wheel_y);
        drain();

        /* --- a device with both capabilities must not double-count ------ */
        SDLOP_RawInputTestSetWheelHighRes(dev, true, true);
        feed(dev, SDLop_EV_REL, SDLop_REL_WHEEL, 1);
        feed(dev, SDLop_EV_REL, SDLop_REL_WHEEL_HI_RES, 120);
        feed(dev, SDLop_EV_SYN, SDLop_SYN_REPORT, 0);
        SDL_PumpEvents();
        wheels = 0;
        wheel_x = wheel_y = 0.0f;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                wheels++;
                wheel_x += ev.wheel.x;
                wheel_y += ev.wheel.y;
            }
        }
        CHECK(wheels == 1 && wheel_y == 1.0f && wheel_x == 0.0f,
              "mixed wheel records: %d events, (%g, %g), want (0, 1)",
              wheels, wheel_x, wheel_y);
        drain();
    }

    SDL_Quit();
    if (failures) {
        printf("test_evdev_frames: FAIL (%d)\n", failures);
        return 1;
    }
    puts("test_evdev_frames: PASS");
    return 0;
}
