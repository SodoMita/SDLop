/*
 * Reference compositor-side driver for behaviour_probe_wayland.
 *
 * The probe prints READY-INPUT once its window is configured and then just
 * pumps events; this program supplies the "compositor-side key/pointer
 * script" the README asks for, so a trace run is reproducible without
 * hand-rolled tooling: run it after READY-INPUT appears and the scripted
 * sequence (enter, relative+absolute motion, click, keys, wheel) lands in
 * the probe's normalized trace.
 *
 * Protocols: wl_seat + zwlr_virtual_pointer_manager_v1 (sway/wlroots) +
 * zwp_virtual_keyboard_manager_v1. Compositors lacking either manager are
 * reported by name so a partial-capability run is explicit, not silent.
 *
 * Build: cmake --build build --target behaviour_probe_inject
 * Usage: ./build/behaviour_probe_inject          (after READY-INPUT)
 *        ./build/behaviour_probe_inject --hold   (keep devices 2s longer)
 */
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-client-protocol.h"
#include "wlr-virtual-pointer-client-protocol.h"

struct ctrl {
    struct wl_display *dpy;
    struct wl_seat *seat;
    struct zwlr_virtual_pointer_manager_v1 *vpm;
    struct zwp_virtual_keyboard_manager_v1 *vkm;
    struct zwlr_virtual_pointer_v1 *vptr;
    struct zwp_virtual_keyboard_v1 *vkb;
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version)
{
    struct ctrl *c = data;
    if (strcmp(iface, "wl_seat") == 0 && !c->seat) {
        c->seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                   version < 5 ? version : 5);
    } else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        c->vpm = wl_registry_bind(reg, name,
                                  &zwlr_virtual_pointer_manager_v1_interface,
                                  version < 2 ? version : 2);
    } else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        c->vkm = wl_registry_bind(reg, name,
                                  &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg,
                                   uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    registry_global, registry_global_remove
};

static void msleep(unsigned int ms)
{
    struct timespec ts = { ms / 1000u, (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* A virtual keyboard must upload a keymap before key events, or the
 * compositor rejects them (no_keymap error). Mirror of the pattern in
 * tests/test_virtual_input.c, using libxkbcommon directly. */
static bool upload_default_keymap(struct zwp_virtual_keyboard_v1 *vkb)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km;
    char *str;
    size_t len;
    int fd;
    bool ok = false;

    if (!ctx) {
        return false;
    }
    km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) {
        xkb_context_unref(ctx);
        return false;
    }
    str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (str) {
        len = strlen(str) + 1;
        fd = memfd_create("keymap", MFD_CLOEXEC);
        if (fd >= 0 && write(fd, str, len) == (ssize_t)len) {
            zwp_virtual_keyboard_v1_keymap(vkb,
                                           WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                           fd, (uint32_t)len);
            ok = true;
        }
        if (fd >= 0) {
            close(fd);
        }
        free(str);
    }
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    return ok;
}

static void tap_key(struct ctrl *c, uint32_t code)
{
    zwp_virtual_keyboard_v1_key(c->vkb, now_ms(), code,
                                WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_display_flush(c->dpy);
    msleep(50);
    zwp_virtual_keyboard_v1_key(c->vkb, now_ms(), code,
                                WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_display_flush(c->dpy);
    msleep(50);
}

int main(int argc, char **argv)
{
    struct ctrl c;
    struct wl_registry *reg;
    bool hold = argc > 1 && strcmp(argv[1], "--hold") == 0;
    int missing = 0;

    memset(&c, 0, sizeof(c));
    c.dpy = wl_display_connect(NULL);
    if (!c.dpy) {
        fprintf(stderr, "inject: wl_display_connect failed "
                "(WAYLAND_DISPLAY set? compositor running?)\n");
        return 1;
    }
    reg = wl_display_get_registry(c.dpy);
    wl_registry_add_listener(reg, &registry_listener, &c);
    wl_display_roundtrip(c.dpy);
    wl_registry_destroy(reg);

    if (!c.vpm) {
        fprintf(stderr, "inject: compositor lacks zwlr_virtual_pointer_manager_v1\n");
        missing = 1;
    }
    if (!c.vkm) {
        fprintf(stderr, "inject: compositor lacks zwp_virtual_keyboard_manager_v1\n");
        missing = 1;
    }
    if (!c.seat) {
        fprintf(stderr, "inject: compositor lacks wl_seat\n");
        missing = 1;
    }
    if (missing) {
        wl_display_disconnect(c.dpy);
        return 2;
    }

    /* seat=NULL for the pointer (mirrors tests/test_virtual_input.c: the
     * compositor adds the device to the default seat); keyboard on seat */
    c.vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(c.vpm, NULL);
    c.vkb = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(c.vkm, c.seat);
    if (!upload_default_keymap(c.vkb)) {
        fprintf(stderr, "inject: keymap upload failed: %s\n", strerror(errno));
        return 1;
    }
    wl_display_flush(c.dpy);
    wl_display_roundtrip(c.dpy);
    fprintf(stderr, "inject: devices ready, replaying script\n");

    /* --- scripted sequence (headless outputs are 1280x720, scale 1) --- */

    /* 1. park the pointer over the window -> enter + keyboard focus */
    zwlr_virtual_pointer_v1_motion_absolute(c.vptr, now_ms(),
                                            64, 64, 1280, 720);
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);
    msleep(150);

    /* 2. relative nudges */
    zwlr_virtual_pointer_v1_motion(c.vptr, now_ms(),
                                   wl_fixed_from_int(12), wl_fixed_from_int(9));
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);
    msleep(80);
    zwlr_virtual_pointer_v1_motion(c.vptr, now_ms(),
                                   wl_fixed_from_int(-5), wl_fixed_from_int(4));
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);
    msleep(120);

    /* 3. click */
    zwlr_virtual_pointer_v1_button(c.vptr, now_ms(), 272 /* BTN_LEFT */,
                                   WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);
    msleep(80);
    zwlr_virtual_pointer_v1_button(c.vptr, now_ms(), 272,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);
    msleep(120);

    /* 4. keys: plain letters, space, enter */
    tap_key(&c, KEY_A);
    tap_key(&c, KEY_B);
    tap_key(&c, KEY_SPACE);
    tap_key(&c, KEY_ENTER);

    /* 5. wheel */
    zwlr_virtual_pointer_v1_axis(c.vptr, now_ms(),
                                 WL_POINTER_AXIS_VERTICAL_SCROLL,
                                 wl_fixed_from_int(10));
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);
    msleep(120);

    /* 6. absolute reposition */
    zwlr_virtual_pointer_v1_motion_absolute(c.vptr, now_ms(),
                                            400, 300, 1280, 720);
    zwlr_virtual_pointer_v1_frame(c.vptr);
    wl_display_flush(c.dpy);

    if (hold) {
        msleep(2000);
    }
    msleep(200);
    zwp_virtual_keyboard_v1_destroy(c.vkb);
    zwlr_virtual_pointer_v1_destroy(c.vptr);
    zwp_virtual_keyboard_manager_v1_destroy(c.vkm);
    zwlr_virtual_pointer_manager_v1_destroy(c.vpm);
    wl_display_flush(c.dpy);
    wl_display_disconnect(c.dpy);
    fprintf(stderr, "inject: done\n");
    return 0;
}
