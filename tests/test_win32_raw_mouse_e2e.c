/*
  SDLop test: raw mouse input end to end, through the worker.

  The keyboard counterpart (test_win32_raw_keyboard_e2e) found a real defect the
  moment it existed, so the mouse path gets the same treatment: inject actual
  Win32 mouse input with SendInput and check what the worker publishes on the
  public rings.

  Values matter as much as delivery here. Two of them have been wrong before and
  are easy to get subtly wrong again:
    - wheel deltas are published in 120-unit notches, the same unit evdev uses
      for a detent, not as the raw 120-per-notch Windows sends;
    - REL_X and REL_Y are published separately and per-axis, never batched into
      one event, or an X-only stream never flushes.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "win32_testenv.h"

/* evdev mouse button codes, as published in SDLop.h */
#define BTN_LEFT   272
#define BTN_RIGHT  273
#define BTN_MIDDLE 274

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

/* Drain the whole public ring, summing what arrived. */
struct tally {
    int x_events;
    int y_events;
    int dx;
    int dy;
    int wheel_events;
    int wheel_value;
    int button_events;
    Uint16 last_button;
    int last_button_down;
};

static void drain(struct tally *t)
{
    SDLop_RawEvent buffer[128];
    int count;

    SDL_PumpEvents();
    while ((count = SDLop_PollRawEvents(buffer, 128)) > 0) {
        int i;
        for (i = 0; i < count; i++) {
            const SDLop_RawEvent *ev = &buffer[i];
            if (ev->type == SDLop_EV_REL) {
                if (ev->code == SDLop_REL_X) {
                    t->x_events++;
                    t->dx += (int)ev->value;
                } else if (ev->code == SDLop_REL_Y) {
                    t->y_events++;
                    t->dy += (int)ev->value;
                } else if (ev->code == SDLop_REL_WHEEL || ev->code == SDLop_REL_HWHEEL) {
                    t->wheel_events++;
                    t->wheel_value += (int)ev->value;
                }
            } else if (ev->type == SDLop_EV_KEY) {
                t->button_events++;
                t->last_button = ev->code;
                t->last_button_down = ev->value ? 1 : 0;
            }
        }
        if (count < 128) {
            break;
        }
    }
}

static void send_mouse(DWORD flags, LONG dx, LONG dy, DWORD data)
{
    INPUT input;

    SDL_zero(input);
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.mouseData = data;
    SendInput(1, &input, sizeof(INPUT));
    Sleep(30);
}

static void click(DWORD down, DWORD up, Uint16 want_code, const char *name)
{
    struct tally t;

    SDL_zero(t);
    drain(&t);
    send_mouse(down, 0, 0, 0);
    SDL_zero(t);
    drain(&t);
    CHECK(t.button_events == 1, "%s press: %d events, want 1", name, t.button_events);
    CHECK(t.last_button == want_code, "%s press published evdev %u, want %u",
          name, t.last_button, want_code);
    CHECK(t.last_button_down == 1, "%s press was not a press (value %d)", name, t.last_button_down);

    send_mouse(up, 0, 0, 0);
    SDL_zero(t);
    drain(&t);
    CHECK(t.button_events == 1, "%s release: %d events, want 1", name, t.button_events);
    CHECK(t.last_button == want_code, "%s release published evdev %u, want %u",
          name, t.last_button, want_code);
    CHECK(t.last_button_down == 0, "%s release was not a release (value %d)", name, t.last_button_down);
}

int main(void)
{
    SDL_Window *window;
    if (!win32_test_interactive_desktop()) {
        printf("skipped: no interactive desktop for input injection\n");
        return 77;
    }
    struct tally t;

    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 raw mouse e2e", 400, 300, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_ShowWindow(window);
    if (!SDLop_RawInputAvailable()) {
        CHECK(0, "the raw input worker is not running (SDLOP_RAW_INPUT=1), so "
                 "nothing can be read from it");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    {
        int i;
        for (i = 0; i < 20; i++) {
            SDL_PumpEvents();
            SDL_Delay(5);
        }
    }
    SDL_zero(t);
    drain(&t); /* discard anything from setup */

    /* --- relative motion, per axis ------------------------------------
     *
     * Deliberately not asserting the exact magnitude. Under Wine the raw
     * packet for an injected dx is doubled for larger deltas and not for small
     * ones -- measured 1:1 at dx<=5 and exactly 2:1 at dx>=10, with the raw
     * packet itself carrying the doubled value, so the doubling is upstream of
     * SDLop (Wine applying the warp twice) and not reproducible on a real box.
     * What is portable, and what has actually broken before, is: the correct
     * axis and sign come out, and the axes do not get batched together. */
    SDL_zero(t);
    send_mouse(MOUSEEVENTF_MOVE, 25, 10, 0);
    drain(&t);
    CHECK(t.x_events >= 1, "an X movement published no SDLop_REL_X event");
    CHECK(t.y_events >= 1, "a Y movement published no SDLop_REL_Y event");
    CHECK(t.dx > 0, "SDLop_REL_X summed to %d for a rightward movement, want positive", t.dx);
    CHECK(t.dy > 0, "SDLop_REL_Y summed to %d for a downward movement, want positive", t.dy);

    /* Leftward and upward must come out negative, which catches a sign error
     * the positive-only case above cannot. */
    SDL_zero(t);
    send_mouse(MOUSEEVENTF_MOVE, -25, -10, 0);
    drain(&t);
    CHECK(t.dx < 0, "SDLop_REL_X summed to %d for a leftward movement, want negative", t.dx);
    CHECK(t.dy < 0, "SDLop_REL_Y summed to %d for an upward movement, want negative", t.dy);

    /* An X-only movement must not manufacture a Y event: the axes are
     * published separately and per-axis, never as one batched motion. */
    SDL_zero(t);
    send_mouse(MOUSEEVENTF_MOVE, 15, 0, 0);
    drain(&t);
    CHECK(t.x_events >= 1, "an X-only movement published no SDLop_REL_X event");
    CHECK(t.y_events == 0, "an X-only movement published %d SDLop_REL_Y events; "
                           "a zero delta must not be published", t.y_events);

    /* All three buttons, checking the evdev code each time: LEFT is 272,
     * RIGHT 273, MIDDLE 274 -- note the order, right precedes middle. */
    click(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, BTN_LEFT, "left");
    click(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, BTN_RIGHT, "right");
    click(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, BTN_MIDDLE, "middle");

    /* --- wheel: one notch is published as one unit, not as 120 -------- */
    SDL_zero(t);
    send_mouse(MOUSEEVENTF_WHEEL, 0, 0, 120);
    drain(&t);
    CHECK(t.wheel_events == 1, "one wheel notch produced %d events, want 1", t.wheel_events);
    CHECK(t.wheel_value == 1, "one wheel notch published %d, want 1 -- wheel deltas must "
                              "be in 120-unit notches, not raw Windows units", t.wheel_value);

    /* Two notches in one injection must still sum to two. */
    SDL_zero(t);
    send_mouse(MOUSEEVENTF_WHEEL, 0, 0, 240);
    drain(&t);
    CHECK(t.wheel_value == 2, "two notches published %d, want 2", t.wheel_value);

    /* --- horizontal wheel is a distinct axis -------------------------- */
    SDL_zero(t);
    send_mouse(MOUSEEVENTF_HWHEEL, 0, 0, 120);
    drain(&t);
    CHECK(t.wheel_events >= 1, "a horizontal wheel notch produced no event");

    /* --- no buttons left held ----------------------------------------- */
    {
        int mask = 0;
        /* SDL's own idea of the mouse state must agree that everything is up. */
        mask = SDL_GetMouseState(NULL, NULL);
        CHECK(mask == 0, "mouse state still reports buttons 0x%x held after release", (unsigned)mask);
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_raw_mouse_e2e: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_raw_mouse_e2e: motion, three buttons and both wheels published\n");
    return 0;
}
