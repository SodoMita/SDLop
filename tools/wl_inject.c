/*
  wl_inject -- drive a Wayland seat from a script.

  A headless compositor has no input devices, which makes it useless for testing
  pointer and keyboard handling: sway's headless backend creates outputs but no
  seat capabilities, and weston's headless backend does not even create a seat.
  wlroots exposes two protocols that solve this (see tools/reference/):

    * zwlr_virtual_pointer_v1  -- a client-created pointer, with relative motion,
      absolute motion (v2), buttons, axes and frames;
    * zwp_virtual_keyboard_v1  -- a client-created keyboard that uploads its own
      keymap (not used here yet; key handling is covered through weston+xdotool).

  This tool is the "finger" for such a compositor: it reads a small script and
  injects the events, so a headless sway behaves like a desktop with a mouse.
  It is a development tool and is not part of the library.

  Usage:
      wl_inject [-v] [script|-]

  Script commands (one per line, '#' starts a comment):
      sleep <ms>                     pause (lets the client run and present)
      move <dx> <dy>                 relative pointer motion
      cursor <x> <y>                 absolute pointer motion to compositor coords
      button <left|right|middle|back|forward> <down|up>
      click [left|right|middle]      press and release
      wheel <amount>                 vertical scroll (positive = down)
      hwheel <amount>                horizontal scroll
      flush                          force a roundtrip
*/

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-client-protocol.h"

struct inject_state
{
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct zwlr_virtual_pointer_manager_v1 *manager;
    struct zwlr_virtual_pointer_v1 *pointer;
    int manager_version;
    bool verbose;
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    struct inject_state *state = (struct inject_state *)data;

    if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (!state->seat) {
            state->seat = (struct wl_seat *)wl_registry_bind(registry, name, &wl_seat_interface,
                                                             version < 5 ? version : 5);
        }
    } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        uint32_t bind_version = version < 2 ? version : 2;
        state->manager = (struct zwlr_virtual_pointer_manager_v1 *)wl_registry_bind(
            registry, name, &zwlr_virtual_pointer_manager_v1_interface, bind_version);
        state->manager_version = (int)bind_version;
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};

/* The pointer protocol wants a millisecond timestamp; any monotonic source is
   accepted by wlroots, it only orders events. */
static uint32_t
inject_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t button_code(const char *name)
{
    if (strcmp(name, "left") == 0)     return BTN_LEFT;
    if (strcmp(name, "right") == 0)    return BTN_RIGHT;
    if (strcmp(name, "middle") == 0)   return BTN_MIDDLE;
    if (strcmp(name, "back") == 0)     return BTN_SIDE;
    if (strcmp(name, "forward") == 0)  return BTN_EXTRA;
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: wl_inject [-v] [--check] [script-file|-]\n");
}

int main(int argc, char **argv)
{
    struct inject_state state;
    FILE *script = stdin;
    const char *path = NULL;
    char line[256];
    int lineno = 0;
    bool check_only = false;

    memset(&state, 0, sizeof(state));
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            state.verbose = true;
        } else if (strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else if (!path) {
            path = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (path && strcmp(path, "-") != 0) {
        script = fopen(path, "r");
        if (!script) {
            fprintf(stderr, "wl_inject: cannot open %s\n", path);
            return 1;
        }
    }

    state.display = wl_display_connect(NULL);
    if (!state.display) {
        fprintf(stderr, "wl_inject: cannot connect to the Wayland display\n");
        return 1;
    }
    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);
    if (!state.manager) {
        fprintf(stderr, "wl_inject: the compositor has no zwlr_virtual_pointer_manager_v1 "
                        "(wlroots-specific: sway and other wlroots compositors have it)\n");
        return 1;
    }
    /* The seat argument is a suggestion, but wlroots needs it: with a NULL seat
       the pointer is created with no seat attached, so it never gets pointer
       focus and the seat's capabilities never grow a pointer. */
    if (!state.seat) {
        fprintf(stderr, "wl_inject: the compositor has no wl_seat\n");
        return 1;
    }
    if (check_only) {
        /* Used by the test harnesses to find out whether this compositor can be
           driven at all, before they start a client and expect events. */
        printf("virtual-pointer: manager v%d, seat %s\n", state.manager_version,
               state.seat ? "found" : "missing");
        zwlr_virtual_pointer_manager_v1_destroy(state.manager);
        wl_display_disconnect(state.display);
        return state.seat ? 0 : 1;
    }
    state.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(state.manager, state.seat);
    if (!state.pointer) {
        fprintf(stderr, "wl_inject: could not create a virtual pointer\n");
        return 1;
    }
    wl_display_roundtrip(state.display);
    if (state.verbose) {
        fprintf(stderr, "wl_inject: virtual pointer created (manager v%d, seat %s)\n",
                state.manager_version, state.seat ? "found" : "not seen");
    }

    while (fgets(line, sizeof(line), script)) {
        char cmd[32] = "", a1[64] = "", a2[64] = "";
        char *hash;
        double n1 = 0.0, n2 = 0.0;

        lineno++;
        hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        if (sscanf(line, "%31s %63s %63s", cmd, a1, a2) < 1) {
            continue;
        }
        n1 = atof(a1);
        n2 = atof(a2);

        if (strcmp(cmd, "sleep") == 0) {
            if (n1 > 0) {
                usleep((useconds_t)(n1 * 1000));
            }
        } else if (strcmp(cmd, "move") == 0) {
            zwlr_virtual_pointer_v1_motion(state.pointer, inject_time_ms(),
                                           wl_fixed_from_double(n1), wl_fixed_from_double(n2));
            zwlr_virtual_pointer_v1_frame(state.pointer);
        } else if (strcmp(cmd, "cursor") == 0) {
            if (state.manager_version < 2) {
                fprintf(stderr, "wl_inject: line %d: absolute motion needs manager v2\n", lineno);
                continue;
            }
            zwlr_virtual_pointer_v1_motion_absolute(state.pointer, inject_time_ms(),
                                                    wl_fixed_from_double(n1), wl_fixed_from_double(n2),
                                                    0, 0);
            zwlr_virtual_pointer_v1_frame(state.pointer);
        } else if (strcmp(cmd, "button") == 0 || strcmp(cmd, "click") == 0) {
            uint32_t code = button_code(a1[0] ? a1 : "left");
            const bool release_only = (strcmp(cmd, "button") == 0 && strcmp(a2, "up") == 0);
            const bool press_only = (strcmp(cmd, "button") == 0 && strcmp(a2, "down") == 0);
            if (!code) {
                fprintf(stderr, "wl_inject: line %d: unknown button '%s'\n", lineno, a1);
                continue;
            }
            if (!release_only) {
                zwlr_virtual_pointer_v1_button(state.pointer, inject_time_ms(), code,
                                               WL_POINTER_BUTTON_STATE_PRESSED);
            }
            if (!press_only) {
                zwlr_virtual_pointer_v1_button(state.pointer, inject_time_ms(), code,
                                               WL_POINTER_BUTTON_STATE_RELEASED);
            }
            zwlr_virtual_pointer_v1_frame(state.pointer);
        } else if (strcmp(cmd, "wheel") == 0 || strcmp(cmd, "hwheel") == 0) {
            uint32_t axis = strcmp(cmd, "wheel") == 0 ? WL_POINTER_AXIS_VERTICAL_SCROLL
                                                      : WL_POINTER_AXIS_HORIZONTAL_SCROLL;
            const int steps = (int)n1;
            for (int i = 0; i < (steps < 0 ? -steps : steps); i++) {
                zwlr_virtual_pointer_v1_axis(state.pointer, inject_time_ms(), axis,
                                             wl_fixed_from_double(steps < 0 ? 15.0 : -15.0));
                zwlr_virtual_pointer_v1_frame(state.pointer);
            }
        } else if (strcmp(cmd, "flush") == 0) {
            wl_display_roundtrip(state.display);
        } else {
            fprintf(stderr, "wl_inject: line %d: unknown command '%s'\n", lineno, cmd);
            continue;
        }
        wl_display_flush(state.display);
        if (state.verbose) {
            fprintf(stderr, "wl_inject: %s %s %s\n", cmd, a1, a2);
        }
    }

    /* Let the compositor process everything before the connection goes away. */
    wl_display_roundtrip(state.display);
    zwlr_virtual_pointer_v1_destroy(state.pointer);
    zwlr_virtual_pointer_manager_v1_destroy(state.manager);
    wl_display_flush(state.display);
    wl_display_disconnect(state.display);
    if (script != stdin) {
        fclose(script);
    }
    return 0;
}
