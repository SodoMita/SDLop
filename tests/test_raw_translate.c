/*
  SDLop test: the raw evdev path's translation of absolute pointers (VM /
  USB tablets, pen tablets, single-touch screens), without devices: fake
  device entries + events fed straight into the main-thread translation
  (SDLOP_RawInputTestAddDevice / SDLOP_RawInputTestFeed), dummy driver.

  - console policy: the device range maps onto the (fullscreen) window,
    like stock's mapping onto the display mode; BTN_TOUCH is the left
    button on devices without BTN_LEFT; a click lands where the pointer is
    (motion of the frame first)
  - window-system policy: the device's motion is scaled to the desktop and
    anchored at the window system's enter position; nothing while the
    pointer is elsewhere (but the position keeps being tracked)
  - BTN_STYLUS / BTN_STYLUS2 = right / middle

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include "internal/sdlop_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

enum
{
    EV_SYN = 0,
    EV_KEY = 1,
    EV_ABS = 3,
    ABS_X = 0,
    ABS_Y = 1,
    BTN_LEFT = 0x110,
    BTN_TOUCH = 0x14a,
    BTN_STYLUS = 0x14b,
    BTN_STYLUS2 = 0x14c
};

static int dev_tablet, dev_vm;
static bool winsys;
static SDL_Event evs[64];
static int nev;

static void feed(int dev, int type, int code, int value)
{
    SDLop_RawEvent e;
    memset(&e, 0, sizeof(e));
    e.device = (Uint32)dev;
    e.type = (Uint16)type;
    e.code = (Uint16)code;
    e.value = value;
    e.timestamp_ns = SDLOP_MonotonicNS();
    SDLOP_RawInputTestFeed(&e, winsys);
}

static void move_to(int dev, int x, int y)
{
    feed(dev, EV_ABS, ABS_X, x);
    feed(dev, EV_ABS, ABS_Y, y);
    feed(dev, EV_SYN, 0, 0);
}

static void collect(void)
{
    nev = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if ((e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
             e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
            nev < (int)SDL_arraysize(evs)) {
            evs[nev++] = e;
        }
    }
}

static const SDL_Event *first(Uint32 type)
{
    for (int i = 0; i < nev; i++) {
        if (evs[i].type == type) {
            return &evs[i];
        }
    }
    return NULL;
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.6f;
}

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("raw", 200, 100, 0);
    CHECK(w != NULL, "window");
    SDL_PumpEvents();
    collect();

    /* a pen tablet (BTN_TOUCH, no BTN_LEFT) and a VM tablet (BTN_LEFT) */
    dev_tablet = SDLOP_RawInputTestAddDevice(false, false, true, 0, 32767, 0, 32767, true);
    dev_vm = SDLOP_RawInputTestAddDevice(false, false, true, -100, 100, -100, 100, false);
    CHECK(dev_tablet >= 0 && dev_vm >= 0, "fake devices");

    /* --- console: absolute mapping onto the window --------------------- */
    winsys = false;
    sdlop.mouse_focus = w;
    move_to(dev_tablet, 16384, 8192);
    collect();
    const SDL_Event *e = first(SDL_EVENT_MOUSE_MOTION);
    CHECK(e && near(e->motion.x, 100.0f) && near(e->motion.y, 25.0f), "console: tablet at (100,25), got (%g,%g)",
          e ? e->motion.x : -1.0, e ? e->motion.y : -1.0);
    /* pen down somewhere else: the motion comes first, then the press */
    feed(dev_tablet, EV_ABS, ABS_X, 32767);
    feed(dev_tablet, EV_KEY, BTN_TOUCH, 1);
    feed(dev_tablet, EV_SYN, 0, 0);
    collect();
    CHECK(nev >= 2 && evs[0].type == SDL_EVENT_MOUSE_MOTION && evs[1].type == SDL_EVENT_MOUSE_BUTTON_DOWN,
          "console: motion, then BTN_TOUCH press (%d events)", nev);
    e = first(SDL_EVENT_MOUSE_BUTTON_DOWN);
    CHECK(e && e->button.button == SDL_BUTTON_LEFT && near(e->button.x, 199.0f), "console: left press at the right edge");
    feed(dev_tablet, EV_KEY, BTN_TOUCH, 0);
    feed(dev_tablet, EV_SYN, 0, 0);
    feed(dev_tablet, EV_KEY, BTN_STYLUS, 1);
    feed(dev_tablet, EV_SYN, 0, 0);
    feed(dev_tablet, EV_KEY, BTN_STYLUS, 0);
    feed(dev_tablet, EV_KEY, BTN_STYLUS2, 1);
    feed(dev_tablet, EV_SYN, 0, 0);
    feed(dev_tablet, EV_KEY, BTN_STYLUS2, 0);
    feed(dev_tablet, EV_SYN, 0, 0);
    collect();
    int left_up = 0, right = 0, middle = 0;
    for (int i = 0; i < nev; i++) {
        left_up += evs[i].type == SDL_EVENT_MOUSE_BUTTON_UP && evs[i].button.button == SDL_BUTTON_LEFT;
        right += evs[i].type == SDL_EVENT_MOUSE_BUTTON_DOWN && evs[i].button.button == SDL_BUTTON_RIGHT;
        middle += evs[i].type == SDL_EVENT_MOUSE_BUTTON_DOWN && evs[i].button.button == SDL_BUTTON_MIDDLE;
    }
    CHECK(left_up == 1 && right == 1 && middle == 1, "console: tip up, stylus buttons = right, middle (%d %d %d)",
          left_up, right, middle);
    /* a VM tablet with a signed range and its own BTN_LEFT */
    move_to(dev_vm, 0, 100);
    feed(dev_vm, EV_KEY, BTN_LEFT, 1);
    feed(dev_vm, EV_SYN, 0, 0);
    feed(dev_vm, EV_KEY, BTN_LEFT, 0);
    feed(dev_vm, EV_SYN, 0, 0);
    collect();
    e = first(SDL_EVENT_MOUSE_MOTION);
    CHECK(e && near(e->motion.x, 100.0f) && near(e->motion.y, 99.0f), "console: VM tablet centre-bottom, got (%g,%g)",
          e ? e->motion.x : -1.0, e ? e->motion.y : -1.0);
    CHECK(first(SDL_EVENT_MOUSE_BUTTON_DOWN) && first(SDL_EVENT_MOUSE_BUTTON_UP), "console: VM tablet BTN_LEFT click");

    /* --- window system: desktop-scaled motion from the enter anchor ---- */
    winsys = true;
    SDL_Rect desk = { 0, 0, 0, 0 };
    SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &desk);
    CHECK(desk.w > 0 && desk.h > 0, "display bounds");
    move_to(dev_tablet, 0, 0); /* the device's position before we look */
    collect();
    sdlop.mouse_x = 50.0f; /* where the window system said we entered */
    sdlop.mouse_y = 50.0f;
    sdlop.mouse_has_position = true;
    move_to(dev_tablet, 0, 0);
    collect();
    CHECK(first(SDL_EVENT_MOUSE_MOTION) == NULL, "winsys: no motion without movement (%d events)", nev);
    /* 1/100 of the device's width = 1/100 of the desktop */
    move_to(dev_tablet, 32767 / 100, 0);
    collect();
    e = first(SDL_EVENT_MOUSE_MOTION);
    const float want_dx = (float)(32767 / 100) * (float)desk.w / 32767.0f;
    CHECK(e && near(e->motion.xrel, want_dx) && near(e->motion.x, 50.0f + want_dx) && near(e->motion.y, 50.0f),
          "winsys: moved by %g from the anchor, got x=%g xrel=%g y=%g", want_dx, e ? e->motion.x : -1.0,
          e ? e->motion.xrel : -1.0, e ? e->motion.y : -1.0);
    /* pointer elsewhere: nothing, but the position is tracked */
    sdlop.mouse_focus = NULL;
    move_to(dev_tablet, 20000, 20000);
    collect();
    CHECK(nev == 0, "winsys: no events outside our window (%d)", nev);
    sdlop.mouse_focus = w;
    sdlop.mouse_x = 10.0f;
    sdlop.mouse_y = 10.0f;
    move_to(dev_tablet, 20000, 20000 + 32767 / 100);
    collect();
    e = first(SDL_EVENT_MOUSE_MOTION);
    const float want_dy = (float)(32767 / 100) * (float)desk.h / 32767.0f;
    CHECK(e && near(e->motion.xrel, 0.0f) && near(e->motion.yrel, want_dy) && near(e->motion.y, 10.0f + want_dy),
          "winsys: no jump after re-entering (xrel=%g yrel=%g)", e ? e->motion.xrel : -1.0, e ? e->motion.yrel : -1.0);

    SDL_DestroyWindow(w);
    SDL_Quit();
    if (failures) {
        printf("test_raw_translate: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_raw_translate: PASS\n");
    return 0;
}
