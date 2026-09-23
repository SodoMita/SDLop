/*
  SDLop test: full end-to-end input through a real compositor using the
  wlr virtual-input protocols (tested against sway). The test opens a
  second Wayland connection, creates a virtual pointer + virtual keyboard
  through the compositor, and injects events that the compositor routes
  back to SDLop's window like real hardware:

    - absolute/relative pointer motion -> SDL_EVENT_MOUSE_MOTION
    - pointer enter / focus            -> SDL_EVENT_WINDOW_MOUSE_ENTER
    - button press/release             -> SDL_EVENT_MOUSE_BUTTON_*
    - wheel axis                       -> SDL_EVENT_MOUSE_WHEEL
    - keymap + key press/release       -> SDL_EVENT_KEY_* (xkbcommon)
    - relative motion while pointer is LOCKED (zwp_pointer_constraints +
      zwp_relative_pointer round-trip through the compositor)

  Skips cleanly when the compositor lacks the virtual-input protocols
  (e.g. weston).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "wlr-virtual-pointer-client-protocol.h"
#include "virtual-keyboard-client-protocol.h"

static int failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                    \
            fprintf(stderr, "\n");                           \
            failures++;                                      \
        }                                                    \
    } while (0)

/* ---------------- controller connection ---------------- */

struct ctrl
{
    struct wl_display *dpy;
    struct wl_seat *seat;
    struct zwlr_virtual_pointer_manager_v1 *vpm;
    struct zwp_virtual_keyboard_manager_v1 *vkm;
    struct zwlr_virtual_pointer_v1 *vptr;
    struct zwp_virtual_keyboard_v1 *vkb;
};

static void ctrl_global(void *data, struct wl_registry *reg, uint32_t name,
                        const char *iface, uint32_t version)
{
    struct ctrl *c = data;
    if (strcmp(iface, "wl_seat") == 0 && !c->seat) {
        c->seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
    } else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        c->vpm = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface,
                                  version < 2 ? version : 2);
    } else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        c->vkm = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}

static void ctrl_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener ctrl_registry_listener = {
    ctrl_global, ctrl_global_remove
};

static bool ctrl_init(struct ctrl *c)
{
    memset(c, 0, sizeof(*c));
    c->dpy = wl_display_connect(NULL);
    if (!c->dpy) {
        return false;
    }
    struct wl_registry *reg = wl_display_get_registry(c->dpy);
    wl_registry_add_listener(reg, &ctrl_registry_listener, c);
    wl_display_roundtrip(c->dpy);
    wl_registry_destroy(reg);
    return c->vpm != NULL && c->vkm != NULL && c->seat != NULL;
}

static uint32_t now_ms(void)
{
    return (uint32_t)SDL_GetTicks();
}

/* ---------------- SDL-side helpers ---------------- */

typedef bool (*event_pred)(const SDL_Event *e, void *user);

static bool pump_until(event_pred pred, void *user, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (pred(&e, user)) {
                return true;
            }
        }
        SDL_Delay(2);
    }
    return false;
}

typedef struct
{
    Uint32 type;
    int field_i;        /* scancode/keycode/button */
    float x, y;         /* motion/wheel */
    bool check_pos;
    bool check_rel;
    float rx, ry;
    Uint32 got_type;
    int got_i;
    float got_x, got_y, got_rx, got_ry;
} expect;

static bool pred_generic(const SDL_Event *e, void *user)
{
    expect *x = user;
    if (e->type != x->type) {
        return false;
    }
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        x->got_x = e->motion.x;
        x->got_y = e->motion.y;
        x->got_rx = e->motion.xrel;
        x->got_ry = e->motion.yrel;
        if (x->check_pos && (fabsf(e->motion.x - x->x) > 0.01f || fabsf(e->motion.y - x->y) > 0.01f)) {
            return false;
        }
        if (x->check_rel && (fabsf(e->motion.xrel - x->rx) > 0.001f || fabsf(e->motion.yrel - x->ry) > 0.001f)) {
            return false;
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        x->got_i = e->button.button;
        return !x->field_i || e->button.button == x->field_i;
    case SDL_EVENT_MOUSE_WHEEL:
        x->got_x = e->wheel.x;
        x->got_y = e->wheel.y;
        /* direction must match, magnitude is compositor-dependent */
        return (e->wheel.y < 0) == (x->y < 0) && e->wheel.y != 0.0f;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        x->got_i = (int)e->key.scancode;
        x->got_type = e->key.key;
        return !x->field_i || e->key.scancode == (SDL_Scancode)x->field_i;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        return true;
    }
    return false;
}

#define KEY_W 17 /* evdev KEY_W */

int main(void)
{
    if (!getenv("WAYLAND_DISPLAY")) {
        printf("test_virtual_input: SKIP (WAYLAND_DISPLAY not set)\n");
        return 0;
    }
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    struct ctrl c;
    if (!ctrl_init(&c)) {
        printf("test_virtual_input: SKIP (compositor lacks wlr virtual-input protocols)\n");
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("virtual input", 320, 240, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (!w) {
        SDL_Quit();
        return 1;
    }
    SDL_Surface *s = SDL_GetWindowSurface(w);
    CHECK(s != NULL, "window surface: %s", SDL_GetError());
    if (s) {
        SDL_UpdateWindowSurface(w);
    }

    /* create the virtual devices; the compositor adds them to the seat,
     * which makes SDLop bind wl_pointer/wl_keyboard on its connection */
    c.vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(c.vpm, NULL);
    c.vkb = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(c.vkm, c.seat);

    /* a virtual keyboard MUST provide a keymap before key events, or the
     * compositor rejects them (no_keymap error) - build the default layout */
    struct xkb_context *xctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *xkm = xkb_keymap_new_from_names(xctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *km_str = xkb_keymap_get_as_string(xkm, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t km_len = strlen(km_str) + 1;
    int km_fd = memfd_create("keymap", MFD_CLOEXEC);
    if (km_fd < 0 || write(km_fd, km_str, km_len) != (ssize_t)km_len) {
        fprintf(stderr, "keymap memfd failed\n");
        return 1;
    }
    zwp_virtual_keyboard_v1_keymap(c.vkb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, km_fd, (uint32_t)km_len);
    close(km_fd);
    free(km_str);
    xkb_keymap_unref(xkm);
    xkb_context_unref(xctx);

    wl_display_flush(c.dpy);
    wl_display_roundtrip(c.dpy);

    /* absolute move: brings the cursor over our (tiled) window -> enter */
    zwlr_virtual_pointer_v1_motion_absolute(c.vptr, now_ms(), 64, 64, 1280, 720);
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);

    expect enter = { .type = SDL_EVENT_WINDOW_MOUSE_ENTER };
    CHECK(pump_until(pred_generic, &enter, 3000), "no WINDOW_MOUSE_ENTER from virtual pointer");
    /* motion_absolute maps to output space; window coords are relative to
     * the window origin (bars/decorations may offset it on real compositors) */
    int win_x = 0, win_y = 0;
    SDL_GetWindowPosition(w, &win_x, &win_y);
    float mx = -1, my = -1;
    SDL_GetMouseState(&mx, &my);
    printf("enter at (%.1f, %.1f) [window at (%d,%d)]\n", mx, my, win_x, win_y);
    CHECK(fabsf(mx - (64.0f - win_x)) < 1.0f && fabsf(my - (64.0f - win_y)) < 1.0f,
          "enter position (%.1f,%.1f) != (64,64) offset by (%d,%d)", mx, my, win_x, win_y);

    /* keyboard: sway sends its keymap when SDLop binds wl_keyboard;
     * press W */
    zwp_virtual_keyboard_v1_key(c.vkb, now_ms(), KEY_W, WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_display_flush(c.dpy);
    expect kd = { .type = SDL_EVENT_KEY_DOWN, .field_i = SDL_SCANCODE_W };
    CHECK(pump_until(pred_generic, &kd, 3000), "no KEY_DOWN for W (scancode W)");
    CHECK(kd.got_type == SDLK_w, "keycode 0x%X != SDLK_w", kd.got_type);

    zwp_virtual_keyboard_v1_key(c.vkb, now_ms(), KEY_W, WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_display_flush(c.dpy);
    expect ku = { .type = SDL_EVENT_KEY_UP, .field_i = SDL_SCANCODE_W };
    CHECK(pump_until(pred_generic, &ku, 3000), "no KEY_UP for W");

    /* button */
    zwlr_virtual_pointer_v1_button(c.vptr, now_ms(), 272 /* BTN_LEFT */, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);
    expect bd = { .type = SDL_EVENT_MOUSE_BUTTON_DOWN, .field_i = SDL_BUTTON_LEFT };
    CHECK(pump_until(pred_generic, &bd, 3000), "no BUTTON_DOWN left");
    zwlr_virtual_pointer_v1_button(c.vptr, now_ms(), 272, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);
    expect bu = { .type = SDL_EVENT_MOUSE_BUTTON_UP, .field_i = SDL_BUTTON_LEFT };
    CHECK(pump_until(pred_generic, &bu, 3000), "no BUTTON_UP left");

    /* wheel: positive wayland axis value = scroll down = negative SDL y */
    zwlr_virtual_pointer_v1_axis(c.vptr, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL,
                                 wl_fixed_from_double(15.0));
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);
    expect wh = { .type = SDL_EVENT_MOUSE_WHEEL, .y = -1.0f };
    CHECK(pump_until(pred_generic, &wh, 3000), "no WHEEL (down)");
    printf("wheel y=%f\n", wh.got_y);

    /* pointer lock: arm via SDL, then inject relative motion; the
     * compositor must route it through zwp_relative_pointer */
    CHECK(SDL_SetWindowRelativeMouseMode(w, true), "relative mode arm: %s", SDL_GetError());
    zwlr_virtual_pointer_v1_motion(c.vptr, now_ms(),
                                   wl_fixed_from_double(5.5), wl_fixed_from_double(-2.25));
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);
    expect rel = { .type = SDL_EVENT_MOUSE_MOTION, .check_rel = true, .rx = 5.5f, .ry = -2.25f };
    CHECK(pump_until(pred_generic, &rel, 3000),
          "no relative MOTION (5.5,-2.25) via locked pointer");
    SDL_GetMouseState(&mx, &my);
    CHECK(fabsf(mx - (64.0f - win_x)) < 1.0f && fabsf(my - (64.0f - win_y)) < 1.0f,
          "locked pointer drifted to (%.1f,%.1f)", mx, my);

    /* unlock; absolute motion must work again. The unlock travels on
     * SDLop's connection while the motion is injected on the controller
     * connection - pump first so the compositor processes the unlock
     * before the motion (different clients have no ordering guarantee) */
    CHECK(SDL_SetWindowRelativeMouseMode(w, false), "relative mode disarm: %s", SDL_GetError());
    for (int i = 0; i < 15; i++) {
        SDL_Delay(10);
        SDL_PumpEvents();
    }
    zwlr_virtual_pointer_v1_motion_absolute(c.vptr, now_ms(), 600, 400, 1280, 720);
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);
    expect abs_ = { .type = SDL_EVENT_MOUSE_MOTION, .check_pos = true,
                    .x = 600.0f - win_x, .y = 400.0f - win_y };
    CHECK(pump_until(pred_generic, &abs_, 3000), "no absolute MOTION (600,400) after unlock");

    /* cleanup */
    zwp_virtual_keyboard_v1_destroy(c.vkb);
    zwlr_virtual_pointer_v1_destroy(c.vptr);
    zwlr_virtual_pointer_manager_v1_destroy(c.vpm);
    wl_seat_destroy(c.seat);
    wl_display_flush(c.dpy);
    wl_display_disconnect(c.dpy);

    SDL_DestroyWindow(w);
    SDL_Quit();

    if (failures) {
        printf("test_virtual_input: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_virtual_input: PASS\n");
    return 0;
}
