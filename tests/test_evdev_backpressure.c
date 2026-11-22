/*
  Deterministic evdev -> SDL queue-pressure regression, without input devices.
  Link with bench/raw_pipe.c and its --wrap options to use the real worker.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include "uinput_synth.h"

#include <linux/input-event-codes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPORTS 600
#define NATIVE_RECORDS (REPORTS * 3 + 4)

static atomic_int callbacks;
static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

static void on_raw(const SDLop_RawEvent *event, void *userdata)
{
    (void)event;
    (void)userdata;
    atomic_fetch_add_explicit(&callbacks, 1, memory_order_release);
}

static bool wait_for_callbacks(int expected)
{
    Uint64 deadline = SDL_GetTicksNS() + 2000000000ull;
    while (atomic_load_explicit(&callbacks, memory_order_acquire) < expected) {
        if (SDL_GetTicksNS() >= deadline) {
            return false;
        }
        /* Deliberately DO NOT pump SDL or drain either raw ring here. */
        SDL_Delay(1);
    }
    return true;
}

static void burst(UInputDevice *device)
{
    for (int i = 1; i <= REPORTS; i++) {
        uinput_synth_rel(device, REL_X, i);
        uinput_synth_rel(device, REL_Y, -1);
        uinput_synth_syn(device);
        if (i == REPORTS / 2) {
            uinput_synth_key(device, BTN_LEFT, 1);
            uinput_synth_syn(device);
        }
    }
    uinput_synth_key(device, BTN_LEFT, 0);
    uinput_synth_syn(device);
}

static void drain_and_check(void)
{
    SDLop_RawEvent raw[127];
    int total = 0, nx = 0, ny = 0, syn = 0, down = 0, up = 0;
    int n;
    bool raw_order = true;
    while ((n = SDLop_PollRawEvents(raw, 127)) > 0) {
        total += n;
        for (int i = 0; i < n; i++) {
            if (raw[i].type == SDLop_EV_REL && raw[i].code == REL_X) {
                if (raw[i].value != ++nx) { raw_order = false; }
            } else if (raw[i].type == SDLop_EV_REL && raw[i].code == REL_Y) {
                ny++;
            } else if (raw[i].type == SDLop_EV_SYN) {
                syn++;
            } else if (raw[i].type == SDLop_EV_KEY && raw[i].code == BTN_LEFT) {
                if (raw[i].value) { down++; } else { up++; }
            }
        }
    }
    CHECK(total == NATIVE_RECORDS, "raw ring retains complete burst");
    CHECK(raw_order && nx == REPORTS && ny == REPORTS && syn == REPORTS + 2 && down == 1 && up == 1,
          "raw burst contents and ordering");

    SDL_Event event;
    nx = ny = down = up = 0;
    bool sdl_order = true, button_down = false;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            if (event.motion.xrel != 0) {
                if (event.motion.xrel != (float)++nx || ny != nx - 1) { sdl_order = false; }
            } else if (event.motion.yrel == -1) {
                ny++;
                if (ny != nx) { sdl_order = false; }
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            down++;
            button_down = true;
            if (nx != REPORTS / 2 || ny != nx) { sdl_order = false; }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            up++;
            if (!button_down || nx != REPORTS || ny != REPORTS) { sdl_order = false; }
            button_down = false;
        }
    }
    printf("burst: raw=%d/%d SDL motions=%d/%d buttons=%d/%d\n",
           total, NATIVE_RECORDS, nx + ny, REPORTS * 2, down + up, 2);
    CHECK(nx == REPORTS && ny == REPORTS, "SDL burst has every motion");
    CHECK(down == 1 && up == 1 && !button_down, "SDL burst retains button down/up");
    CHECK(sdl_order, "SDL motion/button FIFO order");
    CHECK(!(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK), "no stuck mouse button");
}

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_RAW_INPUT", "1", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "0", 1);
    UInputDevice *device = uinput_synth_create("backpressure-test");
    if (!device || !SDL_Init(SDL_INIT_VIDEO) || !SDLop_RawInputAvailable()) {
        fprintf(stderr, "setup failed: %s\n", SDL_GetError());
        SDL_Quit();
        uinput_synth_destroy(device);
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("pressure", 320, 200, 0);
    if (!window || !SDLop_RegisterRawEventCallback(on_raw, NULL)) {
        fprintf(stderr, "window/callback setup failed: %s\n", SDL_GetError());
        SDL_Quit();
        uinput_synth_destroy(device);
        return 1;
    }
    SDL_FlushEvents(0, 0xFFFFFFFFu);

    /* Fits the old SDL raw ring (2048 records), but produces more SDL events
     * than the old SDL queue (512). One unbounded pump silently lost motions. */
    burst(device);
    CHECK(wait_for_callbacks(NATIVE_RECORDS), "worker processes first complete burst");
    SDL_PumpEvents();
    drain_and_check();
    float dx, dy;
    SDL_GetRelativeMouseState(&dx, &dy); /* clear both accumulated axes */

    /* Fill the public queue, then pump repeatedly: input must remain pending,
     * not disappear while state changes without matching deliverable events. */
    Uint32 user_type = SDL_RegisterEvents(1);
    CHECK(user_type != 0, "register sentinel event type");
    SDL_Event sentinel;
    memset(&sentinel, 0, sizeof(sentinel));
    sentinel.type = user_type;
    int sentinels = 0;
    while (sentinels < 100000 && SDL_PushEvent(&sentinel)) { sentinels++; }
    CHECK(sentinels > 0 && sentinels < 100000, "bounded SDL queue filled");
    SDL_ClearError();
    burst(device);
    CHECK(wait_for_callbacks(NATIVE_RECORDS * 2), "callbacks continue while SDL queue is full");
    for (int i = 0; i < 4; i++) { SDL_PumpEvents(); }
    SDL_GetRelativeMouseState(&dx, &dy);
    CHECK(dx == 0 && dy == 0, "full queue defers raw translation and SDL state changes");
    int removed = 0;
    SDL_Event events[113];
    int n;
    while ((n = SDL_PeepEvents(events, 113, SDL_GETEVENT, user_type, user_type)) > 0) { removed += n; }
    CHECK(removed == sentinels, "existing SDL events survive pressure");
    drain_and_check();

    SDL_Quit(); /* join before reading final worker-owned callback state */
    CHECK(atomic_load(&callbacks) == NATIVE_RECORDS * 2, "callbacks see every native record");
    uinput_synth_destroy(device);
    printf("test_evdev_backpressure: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
