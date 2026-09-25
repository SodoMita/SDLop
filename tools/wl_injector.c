/* Combined virtual pointer + keyboard injector for Wayland behaviour probes.
 * Script commands (one per line, '#' comment):
 *   sleep <ms>
 *   cursor <x> <y>          absolute pointer motion (1280x720 space)
 *   move <dx> <dy>          relative pointer motion
 *   button <name> <down|up>
 *   click <name>
 *   wheel <amount>          vertical axis, positive = down
 *   key <name> <down|up>    virtual keyboard, evdev code
 *   tap <name>
 *   flush                   roundtrip
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-client-protocol.h"
#include "wlr-virtual-pointer-client-protocol.h"

static struct wl_display *dpy;
static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *vpm;
static struct zwp_virtual_keyboard_manager_v1 *vkm;
static struct zwlr_virtual_pointer_v1 *vptr;
static struct zwp_virtual_keyboard_v1 *vkb;

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)data;
    if (strcmp(iface, "wl_seat") == 0 && !seat) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
    } else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        vpm = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface,
                               version < 2 ? version : 2);
    } else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        vkm = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t button_code(const char *n)
{
    if (!strcmp(n, "left")) return BTN_LEFT;
    if (!strcmp(n, "right")) return BTN_RIGHT;
    if (!strcmp(n, "middle")) return BTN_MIDDLE;
    if (!strcmp(n, "back")) return BTN_BACK;
    if (!strcmp(n, "forward")) return BTN_FORWARD;
    return 0;
}

static uint32_t key_code(const char *n)
{
    static const struct { const char *n; uint32_t c; } map[] = {
        { "esc", KEY_ESC }, { "1", KEY_1 }, { "2", KEY_2 },
        { "q", KEY_Q }, { "w", KEY_W }, { "e", KEY_E }, { "a", KEY_A },
        { "enter", KEY_ENTER }, { "space", KEY_SPACE },
        { "lshift", KEY_LEFTSHIFT }, { "rshift", KEY_RIGHTSHIFT },
        { "lctrl", KEY_LEFTCTRL }, { "capslock", KEY_CAPSLOCK },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (!strcmp(n, map[i].n)) return map[i].c;
    }
    fprintf(stderr, "unknown key '%s'\n", n);
    exit(1);
}

int main(int argc, char **argv)
{
    FILE *f = stdin;
    if (argc > 1 && strcmp(argv[1], "-") != 0) {
        f = fopen(argv[1], "r");
        if (!f) { perror("script"); return 1; }
    }

    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "connect failed\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    wl_registry_destroy(reg);
    if (!seat || !vpm || !vkm) { fprintf(stderr, "missing globals (seat=%p vpm=%p vkm=%p)\n", (void*)seat, (void*)vpm, (void*)vkm); return 1; }

    vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vpm, NULL);
    vkb = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkm, seat);

    /* virtual keyboards must upload a keymap before any key event */
    struct xkb_context *xctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *xkm = xkb_keymap_new_from_names(xctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *kms = xkb_keymap_get_as_string(xkm, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t klen = strlen(kms) + 1;
    int fd = memfd_create("keymap", MFD_CLOEXEC);
    if (fd < 0 || write(fd, kms, klen) != (ssize_t)klen) { fprintf(stderr, "keymap fd\n"); return 1; }
    zwp_virtual_keyboard_v1_keymap(vkb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)klen);
    close(fd);
    free(kms);
    xkb_keymap_unref(xkm);
    xkb_context_unref(xctx);
    wl_display_roundtrip(dpy);

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, '#');
        if (p) *p = 0;
        char cmd[32] = "", a1[32] = "", a2[32] = "";
        if (sscanf(line, "%31s %31s %31s", cmd, a1, a2) < 1) continue;

        if (!strcmp(cmd, "sleep")) {
            struct timespec ts = { atoi(a1) / 1000, (atoi(a1) % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        } else if (!strcmp(cmd, "cursor")) {
            /* wlroots 0.18 reads motion_absolute x/y as raw uint32 pixels in
             * the extent space (not wl_fixed, despite the protocol XML) */
            zwlr_virtual_pointer_v1_motion_absolute(vptr, now_ms(),
                (uint32_t)atoi(a1), (uint32_t)atoi(a2), 1280, 720);
            zwlr_virtual_pointer_v1_frame(vptr);
        } else if (!strcmp(cmd, "move")) {
            zwlr_virtual_pointer_v1_motion(vptr, now_ms(),
                wl_fixed_from_double(atof(a1)), wl_fixed_from_double(atof(a2)));
            zwlr_virtual_pointer_v1_frame(vptr);
        } else if (!strcmp(cmd, "button")) {
            zwlr_virtual_pointer_v1_button(vptr, now_ms(), button_code(a1),
                !strcmp(a2, "down") ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
            zwlr_virtual_pointer_v1_frame(vptr);
        } else if (!strcmp(cmd, "click")) {
            zwlr_virtual_pointer_v1_button(vptr, now_ms(), button_code(*a1 ? a1 : "left"), WL_POINTER_BUTTON_STATE_PRESSED);
            zwlr_virtual_pointer_v1_frame(vptr);
            struct timespec ts = { 0, 60000000 };
            nanosleep(&ts, NULL);
            zwlr_virtual_pointer_v1_button(vptr, now_ms(), button_code(*a1 ? a1 : "left"), WL_POINTER_BUTTON_STATE_RELEASED);
            zwlr_virtual_pointer_v1_frame(vptr);
        } else if (!strcmp(cmd, "wheel")) {
            zwlr_virtual_pointer_v1_axis(vptr, now_ms(),
                WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(atof(a1)));
            zwlr_virtual_pointer_v1_axis_source(vptr, WL_POINTER_AXIS_SOURCE_WHEEL);
            zwlr_virtual_pointer_v1_frame(vptr);
        } else if (!strcmp(cmd, "key")) {
            zwp_virtual_keyboard_v1_key(vkb, now_ms(), key_code(a1),
                !strcmp(a2, "down") ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        } else if (!strcmp(cmd, "tap")) {
            zwp_virtual_keyboard_v1_key(vkb, now_ms(), key_code(a1), WL_KEYBOARD_KEY_STATE_PRESSED);
            struct timespec ts = { 0, 40000000 };
            nanosleep(&ts, NULL);
            zwp_virtual_keyboard_v1_key(vkb, now_ms(), key_code(a1), WL_KEYBOARD_KEY_STATE_RELEASED);
        } else if (!strcmp(cmd, "flush")) {
            wl_display_roundtrip(dpy);
        }
        wl_display_flush(dpy);
    }
    wl_display_roundtrip(dpy);
    return 0;
}
