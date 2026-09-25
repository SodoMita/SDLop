/*
  SDLop test: end-to-end raw evdev path.

  Creates a synthetic keyboard+mouse device via /dev/uinput, injects
  events, and verifies they arrive through the asyncinput-style worker:
  raw ring, callbacks, and the SDL event queue (with sane kernel
  timestamps).

  Skips cleanly when /dev/uinput or /dev/input are not accessible.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <linux/input-event-codes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "uinput_synth.h"

static atomic_int cb_count;
static atomic_ullong cb_last_ts;

static void on_raw(const SDLop_RawEvent *ev, void *userdata)
{
    (void)userdata;
    atomic_fetch_add(&cb_count, 1);
    atomic_store(&cb_last_ts, ev->timestamp_ns);
}

static Uint64 now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
}

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);         \
            fprintf(stderr, "\n");                \
            failures++;                           \
        }                                         \
    } while (0)

int main(void)
{
    int failures = 0;

    setenv("SDL_VIDEODRIVER", "dummy", 1);

    UInputDevice *synth = uinput_synth_create("sdlop-test-device");
    if (!synth) {
        printf("test_evdev_uinput: SKIP (/dev/uinput not accessible)\n");
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        uinput_synth_destroy(synth);
        return 1;
    }
    if (!SDLop_RawInputAvailable()) {
        printf("test_evdev_uinput: SKIP (raw input unavailable: %s)\n", SDL_GetError());
        SDL_ClearError();
        SDL_Quit();
        uinput_synth_destroy(synth);
        return 0;
    }

    SDL_Window *w = SDL_CreateWindow("evdev test", 320, 200, 0);
    CHECK(w != NULL, "create window");

    CHECK(SDLop_RegisterRawEventCallback(on_raw, NULL), "register callback");

    /* --- inject a key press/release (KEY_W) --- */
    Uint64 t_before = now_ns();
    uinput_synth_key(synth, KEY_W, 1);
    uinput_synth_syn(synth);
    uinput_synth_key(synth, KEY_W, 0);
    uinput_synth_syn(synth);
    /* --- inject mouse motion + button --- */
    uinput_synth_rel(synth, REL_X, 10);
    uinput_synth_rel(synth, REL_Y, -5);
    uinput_synth_key(synth, BTN_LEFT, 1);
    uinput_synth_syn(synth);
    uinput_synth_key(synth, BTN_LEFT, 0);
    uinput_synth_syn(synth);

    /* give the worker thread a moment */
    SDL_Delay(100);

    /* callbacks fired on worker thread */
    CHECK(atomic_load(&cb_count) >= 8, "callback count %d >= 8", atomic_load(&cb_count));

    /* kernel timestamps are sane (recent monotonic time) */
    Uint64 cbts = atomic_load(&cb_last_ts);
    CHECK(cbts > 0 && cbts <= now_ns() + 1000000000ull, "callback timestamp sane");
    CHECK(cbts >= t_before - 2000000000ull, "callback timestamp not ancient");

    /* raw poll ring */
    SDLop_RawEvent raws[64];
    int nraw = SDLop_PollRawEvents(raws, 64);
    CHECK(nraw >= 8, "raw poll count %d >= 8", nraw);
    bool saw_w_down = false, saw_btn_left = false, saw_rel_x = false;
    for (int i = 0; i < nraw; i++) {
        if (raws[i].type == SDLop_EV_KEY && raws[i].code == SDLop_KEY_W && raws[i].value == 1) {
            saw_w_down = true;
            CHECK(raws[i].timestamp_ns > 0, "raw event has kernel timestamp");
        }
        if (raws[i].type == SDLop_EV_KEY && raws[i].code == SDLop_BTN_LEFT) {
            saw_btn_left = true;
        }
        if (raws[i].type == SDLop_EV_REL && raws[i].code == SDLop_REL_X && raws[i].value == 10) {
            saw_rel_x = true;
        }
    }
    CHECK(saw_w_down, "saw KEY_W down in raw ring");
    CHECK(saw_btn_left, "saw BTN_LEFT in raw ring");
    CHECK(saw_rel_x, "saw REL_X=10 in raw ring");

    /* SDL event queue translation */
    SDL_PumpEvents();
    bool saw_keydown = false, saw_keyup = false, saw_motion = false;
    bool saw_button_down = false, saw_button_up = false;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_KEY_DOWN:
            if (ev.key.scancode == SDL_SCANCODE_W) {
                saw_keydown = true;
                CHECK(ev.key.raw == KEY_W, "raw evdev code in key event");
                CHECK(ev.key.timestamp > 0, "key event timestamp");
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (ev.key.scancode == SDL_SCANCODE_W) {
                saw_keyup = true;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            saw_motion = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                saw_button_down = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                saw_button_up = true;
            }
            break;
        default:
            break;
        }
    }
    CHECK(saw_keydown, "SDL_EVENT_KEY_DOWN for W");
    CHECK(saw_keyup, "SDL_EVENT_KEY_UP for W");
    CHECK(saw_motion, "SDL_EVENT_MOUSE_MOTION from REL");
    CHECK(saw_button_down, "SDL_EVENT_MOUSE_BUTTON_DOWN from BTN_LEFT");
    CHECK(saw_button_up, "SDL_EVENT_MOUSE_BUTTON_UP from BTN_LEFT");

    /* W should be released now */
    CHECK(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_W], "W released in keystate");

    /* press again and check state sticks until release */
    uinput_synth_key(synth, KEY_D, 1);
    uinput_synth_syn(synth);
    SDL_Delay(50);
    SDL_PumpEvents();
    SDL_FlushEvents(0, 0xFFFFFFFFu);
    CHECK(SDL_GetKeyboardState(NULL)[SDL_SCANCODE_D], "D held in keystate");
    uinput_synth_key(synth, KEY_D, 0);
    uinput_synth_syn(synth);
    SDL_Delay(50);
    SDL_PumpEvents();
    SDL_FlushEvents(0, 0xFFFFFFFFu);
    CHECK(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_D], "D released in keystate");

    SDL_DestroyWindow(w);
    SDL_Quit();
    uinput_synth_destroy(synth);

    if (failures) {
        printf("test_evdev_uinput: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_evdev_uinput: PASS\n");
    return 0;
}
