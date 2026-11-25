/*
  SDLop - X11 video driver (xcb, fully dlopen-ed).

  Ported from nyorain/swa's X11 backend (xcb event handling, EWMH window
  states, WM_DELETE_WINDOW/_NET_WM_PING, MIT-SHM buffer surfaces, XKB key
  handling with xkbcommon-x11), with behavioural details taken from SDL3's
  X11 driver: the "legacy" fullscreen path for sessions without a window
  manager, _NET_WM_STATE -> window flag mapping, focus/crossing filters,
  StaticGravity positioning and Xft.dpi content scale.

  Differences to both: everything goes through xcb (no Xlib event queue, so
  the connection fd is an exact readiness signal for SDL_WaitEvent), and
  every library - libxcb included - is dlopen-ed, see sdlop_x11_dyn.h.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"
#include "internal/scancode_evdev.h"
#include "internal/sdlop_xkb_dyn.h"
#include "../sdlop_xcursor.h"

#include <errno.h>
#include <poll.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* protocol constants not exported by the core xcb headers             */
/* ------------------------------------------------------------------ */

/* ICCCM WM_SIZE_HINTS flags */
#define X11_HINT_US_POSITION  (1u << 0)
#define X11_HINT_P_POSITION   (1u << 2)
#define X11_HINT_P_SIZE       (1u << 3)
#define X11_HINT_P_MIN_SIZE   (1u << 4)
#define X11_HINT_P_MAX_SIZE   (1u << 5)
#define X11_HINT_P_ASPECT     (1u << 7)
#define X11_HINT_P_WIN_GRAVITY (1u << 9)

/* ICCCM WM_HINTS flags / states */
#define X11_WMHINT_INPUT 1u
#define X11_WMHINT_STATE 2u
#define X11_WMHINT_URGENCY (1u << 8)
#define X11_STATE_WITHDRAWN 0
#define X11_STATE_NORMAL 1
#define X11_STATE_ICONIC 3

/* EWMH _NET_WM_STATE actions */
#define NET_WM_STATE_REMOVE 0
#define NET_WM_STATE_ADD 1

/* XInput2 */
#define X11_XI_ALL_MASTER_DEVICES 1

/* xcb_send_event always transmits 32 bytes */
typedef union X11Event32
{
    char bytes[32];
    xcb_client_message_event_t client;
    xcb_unmap_notify_event_t unmap;
} X11Event32;

/* ------------------------------------------------------------------ */
/* atoms                                                               */
/* ------------------------------------------------------------------ */

#define X11_ATOM_LIST(X)                  \
    X(WM_PROTOCOLS)                       \
    X(WM_DELETE_WINDOW)                   \
    X(WM_CHANGE_STATE)                    \
    X(WM_STATE)                           \
    X(WM_CLIENT_MACHINE)                  \
    X(UTF8_STRING)                        \
    X(_NET_WM_PING)                       \
    X(_NET_WM_PID)                        \
    X(_NET_WM_NAME)                       \
    X(_NET_WM_ICON_NAME)                  \
    X(_NET_WM_STATE)                      \
    X(_NET_WM_STATE_FULLSCREEN)           \
    X(_NET_WM_STATE_MAXIMIZED_VERT)       \
    X(_NET_WM_STATE_MAXIMIZED_HORZ)       \
    X(_NET_WM_STATE_HIDDEN)               \
    X(_NET_WM_STATE_FOCUSED)              \
    X(_NET_WM_STATE_ABOVE)                \
    X(_NET_WM_STATE_SKIP_TASKBAR)         \
    X(_NET_WM_STATE_SKIP_PAGER)           \
    X(_NET_WM_STATE_MODAL)                \
    X(_NET_WM_MOVERESIZE)                 \
    X(_GTK_SHOW_WINDOW_MENU)              \
    X(_NET_WM_SYNC_REQUEST)               \
    X(_NET_WM_SYNC_REQUEST_COUNTER)       \
    X(_NET_WM_STATE_DEMANDS_ATTENTION)    \
    X(_NET_WM_WINDOW_TYPE)                \
    X(_NET_WM_WINDOW_TYPE_NORMAL)         \
    X(_NET_WM_WINDOW_TYPE_UTILITY)        \
    X(_NET_WM_WINDOW_TYPE_TOOLTIP)        \
    X(_NET_WM_WINDOW_TYPE_POPUP_MENU)     \
    X(_NET_WM_WINDOW_TYPE_DIALOG)         \
    X(_NET_WM_WINDOW_OPACITY)             \
    X(_NET_WM_ICON)                       \
    X(_NET_FRAME_EXTENTS)                 \
    X(_NET_WM_BYPASS_COMPOSITOR)          \
    X(_NET_SUPPORTED)                     \
    X(_NET_SUPPORTING_WM_CHECK)           \
    X(_NET_ACTIVE_WINDOW)                 \
    X(_MOTIF_WM_HINTS)                    \
    X(_XKB_RULES_NAMES)                   \
    X(RESOURCE_MANAGER)

typedef struct X11Atoms
{
#define X11_ATOM_FIELD(name) xcb_atom_t name;
    X11_ATOM_LIST(X11_ATOM_FIELD)
#undef X11_ATOM_FIELD
} X11Atoms;

/* ------------------------------------------------------------------ */
/* driver state                                                        */
/* ------------------------------------------------------------------ */

typedef struct X11WindowData
{
    xcb_window_t xid;
    xcb_window_t parent;          /* root, or the WM frame after reparenting */
    xcb_gcontext_t gc;
    bool mapped;                  /* MapNotify seen, no UnmapNotify since */

    /* last geometry the server reported (window->w/h hold the request) */
    int cfg_x, cfg_y, cfg_w, cfg_h;
    bool cfg_known;

    /* software framebuffer: XRGB8888, zero copy for SDL_GetWindowSurface */
    uint32_t *pixels;
    int fb_w, fb_h;
    size_t fb_cap; /* bytes behind pixels (headroom for resizes) */
    bool fb_shm;
    xcb_shm_seg_t shm_seg;

    /* GL */
    bool gl_active;
    void *egl_surface;
    xcb_visualid_t visual;        /* x11.visual, the GLX config's visual or x11.argb_visual */
    uint8_t depth;                /* depth of that visual: x11.depth, or 32 for ARGB */
    bool clear_explicit;          /* SDLop_SetWindowClearColor called (opaque even when ARGB) */
    xcb_colormap_t colormap;      /* own colormap for a non-default visual */

    /* state the app has been told about (window->flags may be preset by
     * the core before the request is confirmed, e.g. FULLSCREEN) */
    bool rep_fullscreen, rep_maximized, rep_minimized;
    /* last observed WM state (event on transitions only) */
    bool obs_fullscreen, obs_maximized, obs_iconic;

    /* no-WM emulation (SDL3 "legacy" fullscreen) */
    bool legacy_fullscreen, legacy_maximized;
    SDL_Rect saved_rect;

    bool want_grab;               /* relative mode: pointer grab pending */
    bool mouse_grab;              /* SDL_SetWindowMouseGrab: confine wanted */
    bool keyboard_grab;           /* SDL_SetWindowKeyboardGrab: grab wanted */

    int hit_test_result;          /* SDL_HitTestResult under the pointer */
    bool pending_move;            /* hit-test drag started while unfocused (stock) */
    int16_t pending_move_x, pending_move_y; /* root coordinates of that press */

    /* _NET_WM_SYNC_REQUEST (SDL_HINT_VIDEO_X11_ENABLE_XSYNC_EXT) */
    uint32_t sync_counter;        /* XSync counter, 0 = not used */
    xcb_sync_int64_t sync_value;  /* value of the last request */
    int sync_state;               /* SYNC_IDLE / SYNC_REQUESTED / SYNC_CONFIGURED */
} X11WindowData;

enum
{
    SYNC_IDLE,
    SYNC_REQUESTED,  /* request seen, waiting for the ConfigureNotify */
    SYNC_CONFIGURED  /* configured: the next present updates the counter */
};

typedef struct X11Monitor
{
    char name[64];
    SDL_Rect rect;
    int refresh_mhz;
    bool primary;
    int num_modes;
    SDL_DisplayMode modes[48];
    xcb_randr_mode_t mode_ids[48]; /* RandR mode behind modes[i] */
    xcb_randr_output_t output;     /* the output modes came from (0: no RandR) */
    SDL_DisplayID display_id;
} X11Monitor;

/* a display switched away from its desktop mode (exclusive fullscreen) */
typedef struct X11ModeSave
{
    SDL_DisplayID display_id;
    xcb_randr_mode_t desktop_mode;
} X11ModeSave;

typedef struct X11DeviceData
{
    xcb_connection_t *conn;
    int screen_num;
    xcb_screen_t *screen;
    xcb_window_t root;
    xcb_visualid_t visual;
    uint8_t depth;
    bool fb_format_ok;            /* root visual is 24/32-bit TrueColor XRGB */
    xcb_visualid_t argb_visual;   /* depth-32 TrueColor ARGB8888 visual (SDL_WINDOW_TRANSPARENT), 0 = none */
    uint32_t max_request_bytes;
    X11Atoms atom;
    bool dead;                    /* connection error seen */

    bool has_shm;
    bool has_xkb;
    uint8_t xkb_event_base;
    int32_t xkb_device_id;
    bool detectable_repeat;
    bool has_xi2;
    bool has_xi24; /* XI 2.4: touchpad gesture events */
    bool has_sync;                /* SYNC >= 3.0 (resize sync counters) */
    bool has_shape;               /* SHAPE >= 1.1 (input shapes) */
    xcb_atom_t icc_atom;          /* _ICC_PROFILE, or _ICC_PROFILE_<n> on screen n > 0 */
    uint8_t xi_opcode;
    /* XI2.2 touch sequences: per-finger last position for dx/dy, and
     * the window the sequence started on (SDL3 semantics) */
    struct
    {
        bool active;
        uint16_t source; /* slave device = SDL_TouchID */
        uint32_t detail; /* touch id = SDL_FingerID */
        SDL_Window *window;
        float nx, ny;
    } fingers[16];
    bool has_randr;
    uint8_t randr_event_base;
    X11ModeSave mode_saves[SDLOP_MAX_DISPLAYS];
    int num_mode_saves;
    bool randr_monitors;          /* RandR >= 1.5 */
    bool displays_dirty;

    X11Monitor monitors[SDLOP_MAX_DISPLAYS];
    int num_monitors;
    float content_scale;

    xcb_cursor_t blank_cursor;
    xcb_cursor_t current_cursor;  /* what SDL wants shown (blank when hidden) */
    xcb_cursor_t hit_test_cursors[SDL_HITTEST_RESIZE_LEFT + 1]; /* resize areas, created on demand */
    struct sdlop_xcursor_theme *cursor_theme;
    bool cursor_theme_tried;
    bool render_checked;
    xcb_render_pictformat_t argb_format; /* 0 = no ARGB cursors */
    SDL_Window *confine_window;   /* pointer grabbed by SDL_SetWindowMouseGrab */
    bool has_barriers;            /* XFixes >= 5 pointer barriers */
    SDL_Window *barrier_window;   /* window whose SDL_SetWindowMouseRect is fenced in */
    SDL_Rect barrier_rect;        /* ...in root coordinates */
    xcb_xfixes_barrier_t barriers[4];
    SDL_Window *capture_window;   /* pointer grabbed by SDL_CaptureMouse */
    SDL_Window *keyboard_grab_window;
    Uint64 screensaver_reset_ns;
    SDL_Window *pointer_focus;
    SDL_Window *relative_window;
    bool pointer_grabbed;
    bool warp_relative;           /* no XInput2: warp-to-center fallback */
    int warp_cx, warp_cy;

    xcb_generic_event_t *peeked;  /* event read ahead (autorepeat detection) */
    bool keymap_dirty;
    uint64_t keymap_hash;         /* skip no-op reloads (XTest device switches) */

    SDL_KeyboardID keyboard_id;
    SDL_MouseID mouse_id;
} X11DeviceData;

static X11DeviceData x11;

static void x11_dispatch(xcb_generic_event_t *ev);
static void x11_PumpEvents(SDLop_VideoDevice *device, int timeout_ms);
static void x11_update_displays(bool send_events);

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static X11WindowData *wdata(SDL_Window *window)
{
    return window ? (X11WindowData *)window->driverdata : NULL;
}

static SDL_Window *window_from_xid(xcb_window_t xid)
{
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        X11WindowData *wd = wdata(w);
        if (wd && wd->xid == xid) {
            return w;
        }
    }
    return NULL;
}

static void x11_flush(void)
{
    if (x11.conn) {
        X11_xcb_flush(x11.conn);
    }
}

/* cheap roundtrip: every request issued before has been processed */
static void x11_sync(void)
{
    free(X11_xcb_get_input_focus_reply(x11.conn, X11_xcb_get_input_focus(x11.conn), NULL));
}

static void set_property(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, uint8_t format,
                         uint32_t count, const void *data)
{
    X11_xcb_change_property(x11.conn, XCB_PROP_MODE_REPLACE, w, prop, type, format, count, data);
}

/* reads up to max_items 32-bit items; returns count, caller frees *out */
static int get_property32(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, uint32_t max_items, uint32_t **out)
{
    *out = NULL;
    xcb_get_property_reply_t *r = X11_xcb_get_property_reply(
        x11.conn, X11_xcb_get_property(x11.conn, 0, w, prop, type, 0, max_items), NULL);
    if (!r) {
        return 0;
    }
    int n = 0;
    if (r->format == 32 && r->type != XCB_NONE) {
        n = X11_xcb_get_property_value_length(r) / 4;
        if (n > 0) {
            *out = (uint32_t *)malloc((size_t)n * 4);
            if (*out) {
                memcpy(*out, X11_xcb_get_property_value(r), (size_t)n * 4);
            } else {
                n = 0;
            }
        }
    }
    free(r);
    return n;
}

/* string-ish property, NUL terminated copy; returns length or -1 */
static int get_property_bytes(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, char **out)
{
    *out = NULL;
    xcb_get_property_reply_t *r = X11_xcb_get_property_reply(
        x11.conn, X11_xcb_get_property(x11.conn, 0, w, prop, type, 0, 1u << 20), NULL);
    if (!r) {
        return -1;
    }
    int len = -1;
    if (r->format == 8 && r->type != XCB_NONE) {
        len = X11_xcb_get_property_value_length(r);
        *out = (char *)malloc((size_t)len + 1);
        if (*out) {
            memcpy(*out, X11_xcb_get_property_value(r), (size_t)len);
            (*out)[len] = '\0';
        } else {
            len = -1;
        }
    }
    free(r);
    return len;
}

static void send_to_root(X11Event32 *ev)
{
    X11_xcb_send_event(x11.conn, 0, x11.root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                       ev->bytes);
}

static void send_client_message(xcb_window_t w, xcb_atom_t type, uint32_t d0, uint32_t d1,
                                uint32_t d2, uint32_t d3, uint32_t d4)
{
    X11Event32 ev;
    memset(&ev, 0, sizeof(ev));
    ev.client.response_type = XCB_CLIENT_MESSAGE;
    ev.client.format = 32;
    ev.client.window = w;
    ev.client.type = type;
    ev.client.data.data32[0] = d0;
    ev.client.data.data32[1] = d1;
    ev.client.data.data32[2] = d2;
    ev.client.data.data32[3] = d3;
    ev.client.data.data32[4] = d4;
    send_to_root(&ev);
}

/* EWMH: a compliant WM is running when _NET_SUPPORTING_WM_CHECK on the
 * root names a window that carries the same property pointing at itself.
 * Checked on demand: a WM can start or die at any time. */
static bool wm_supports(xcb_atom_t feature)
{
    uint32_t *check = NULL;
    bool ok = false;
    if (get_property32(x11.root, x11.atom._NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 1, &check) == 1) {
        uint32_t *self = NULL;
        if (get_property32(check[0], x11.atom._NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 1, &self) == 1 &&
            self[0] == check[0]) {
            ok = true;
        }
        free(self);
    }
    free(check);
    if (!ok || feature == XCB_NONE) {
        return ok;
    }
    uint32_t *atoms = NULL;
    int n = get_property32(x11.root, x11.atom._NET_SUPPORTED, XCB_ATOM_ATOM, 4096, &atoms);
    ok = false;
    for (int i = 0; i < n; i++) {
        if (atoms[i] == feature) {
            ok = true;
            break;
        }
    }
    free(atoms);
    return ok;
}

static bool wm_running(void)
{
    return wm_supports(XCB_NONE);
}

/* ------------------------------------------------------------------ */
/* ICCCM / EWMH window properties                                      */
/* ------------------------------------------------------------------ */

static void set_size_hints(SDL_Window *window, bool lock_size, bool position)
{
    X11WindowData *wd = wdata(window);
    uint32_t h[18];
    memset(h, 0, sizeof(h));
    /* StaticGravity: positions refer to the client area, not the frame
     * (what SDL_SetWindowPosition means) */
    h[0] = X11_HINT_P_SIZE | X11_HINT_P_WIN_GRAVITY;
    h[3] = (uint32_t)window->w;
    h[4] = (uint32_t)window->h;
    if (position) {
        h[0] |= X11_HINT_US_POSITION | X11_HINT_P_POSITION;
        h[1] = (uint32_t)window->x;
        h[2] = (uint32_t)window->y;
    }
    if (lock_size) {
        h[0] |= X11_HINT_P_MIN_SIZE | X11_HINT_P_MAX_SIZE;
        h[5] = h[7] = (uint32_t)window->w;
        h[6] = h[8] = (uint32_t)window->h;
    } else if (!(window->flags & SDL_WINDOW_FULLSCREEN)) {
        /* SDL_SetWindowMinimumSize / MaximumSize / AspectRatio */
        if (window->min_w || window->min_h) {
            h[0] |= X11_HINT_P_MIN_SIZE;
            h[5] = (uint32_t)SDL_max(window->min_w, 1);
            h[6] = (uint32_t)SDL_max(window->min_h, 1);
        }
        if (window->max_w || window->max_h) {
            h[0] |= X11_HINT_P_MAX_SIZE;
            h[7] = window->max_w ? (uint32_t)window->max_w : 0x7fffu;
            h[8] = window->max_h ? (uint32_t)window->max_h : 0x7fffu;
        }
        if (window->min_aspect > 0.0f || window->max_aspect > 0.0f) {
            const float lo = window->min_aspect > 0.0f ? window->min_aspect : 1.0f / 10000.0f;
            const float hi = window->max_aspect > 0.0f ? window->max_aspect : 10000.0f;
            h[0] |= X11_HINT_P_ASPECT;
            h[11] = (uint32_t)SDL_lroundf(lo * 10000.0f);
            h[12] = 10000;
            h[13] = (uint32_t)SDL_lroundf(SDL_min(hi, 100000.0f) * 10000.0f);
            h[14] = 10000;
        }
    }
    h[17] = XCB_GRAVITY_STATIC;
    set_property(wd->xid, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, h);
}

static bool size_locked(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    /* non-resizable windows are fixed-size, except while the WM or the
     * legacy path is making them fill a display */
    return !(window->flags & SDL_WINDOW_RESIZABLE) && !(window->flags & SDL_WINDOW_FULLSCREEN) &&
           !(wd && (wd->legacy_maximized || wd->legacy_fullscreen));
}

static void set_title(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    const char *title = window->title ? window->title : "";
    uint32_t len = (uint32_t)strlen(title);
    bool ascii = true;
    for (uint32_t i = 0; i < len; i++) {
        if ((unsigned char)title[i] >= 0x80) {
            ascii = false;
            break;
        }
    }
    set_property(wd->xid, x11.atom._NET_WM_NAME, x11.atom.UTF8_STRING, 8, len, title);
    set_property(wd->xid, x11.atom._NET_WM_ICON_NAME, x11.atom.UTF8_STRING, 8, len, title);
    /* WM_NAME is Latin-1 by spec; UTF8_STRING is the de-facto extension */
    set_property(wd->xid, XCB_ATOM_WM_NAME, ascii ? XCB_ATOM_STRING : x11.atom.UTF8_STRING, 8, len, title);
    set_property(wd->xid, XCB_ATOM_WM_ICON_NAME, ascii ? XCB_ATOM_STRING : x11.atom.UTF8_STRING, 8, len, title);
}

/* ICCCM WM_HINTS: input model (SDL_WINDOW_NOT_FOCUSABLE) and urgency
 * (SDL_FlashWindow) */
static void set_wm_hints_urgent(SDL_Window *window, bool urgent)
{
    X11WindowData *wd = wdata(window);
    const bool focusable = !(window->flags & (SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_TOOLTIP));
    uint32_t wm_hints[9] = { X11_WMHINT_INPUT | X11_WMHINT_STATE, focusable ? 1u : 0u, X11_STATE_NORMAL, 0, 0, 0, 0, 0, 0 };
    if (urgent) {
        wm_hints[0] |= X11_WMHINT_URGENCY;
    }
    set_property(wd->xid, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, 9, wm_hints);
}

static void set_wm_hints(SDL_Window *window)
{
    set_wm_hints_urgent(window, false);
}

static void set_motif_decorations(SDL_Window *window, bool decorated)
{
    X11WindowData *wd = wdata(window);
    uint32_t hints[5] = { 2 /* MWM_HINTS_DECORATIONS */, 0, decorated ? 1u : 0u, 0, 0 };
    set_property(wd->xid, x11.atom._MOTIF_WM_HINTS, x11.atom._MOTIF_WM_HINTS, 32, 5, hints);
}

/* _NET_WM_STATE as it should be on an unmapped window (the WM reads it at
 * map time; once mapped, changes go through client messages) */
static void write_initial_net_wm_state(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    uint32_t atoms[8];
    uint32_t n = 0;
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        atoms[n++] = x11.atom._NET_WM_STATE_FULLSCREEN;
    }
    if (window->flags & SDL_WINDOW_ALWAYS_ON_TOP) {
        atoms[n++] = x11.atom._NET_WM_STATE_ABOVE;
    }
    if (window->flags & (SDL_WINDOW_UTILITY | SDL_WINDOW_TOOLTIP | SDL_WINDOW_POPUP_MENU)) {
        atoms[n++] = x11.atom._NET_WM_STATE_SKIP_TASKBAR;
        atoms[n++] = x11.atom._NET_WM_STATE_SKIP_PAGER;
    }
    if ((window->flags & SDL_WINDOW_MODAL) && window->parent) {
        atoms[n++] = x11.atom._NET_WM_STATE_MODAL;
    }
    if (window->flags & SDL_WINDOW_MAXIMIZED) {
        atoms[n++] = x11.atom._NET_WM_STATE_MAXIMIZED_VERT;
        atoms[n++] = x11.atom._NET_WM_STATE_MAXIMIZED_HORZ;
    }
    if (n) {
        set_property(wd->xid, x11.atom._NET_WM_STATE, XCB_ATOM_ATOM, 32, n, atoms);
    } else {
        X11_xcb_delete_property(x11.conn, wd->xid, x11.atom._NET_WM_STATE);
    }
}

static void request_net_wm_state(SDL_Window *window, bool add, xcb_atom_t a, xcb_atom_t b)
{
    X11WindowData *wd = wdata(window);
    if (!wd->mapped) {
        write_initial_net_wm_state(window);
        return;
    }
    send_client_message(wd->xid, x11.atom._NET_WM_STATE, add ? NET_WM_STATE_ADD : NET_WM_STATE_REMOVE,
                        a, b, 1 /* source: application */, 0);
}

static void activate_window(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    if (!wd || !wd->mapped) {
        return;
    }
    if (wm_supports(x11.atom._NET_ACTIVE_WINDOW)) {
        send_client_message(wd->xid, x11.atom._NET_ACTIVE_WINDOW, 1 /* application */,
                            XCB_CURRENT_TIME, 0, 0, 0);
    } else {
        /* no WM: nobody else hands out focus */
        X11_xcb_set_input_focus(x11.conn, XCB_INPUT_FOCUS_PARENT, wd->xid, XCB_CURRENT_TIME);
    }
}

/* ------------------------------------------------------------------ */
/* reported window state                                               */
/* ------------------------------------------------------------------ */

static void report_fullscreen(SDL_Window *window, bool on)
{
    X11WindowData *wd = wdata(window);
    if (on) {
        window->flags |= SDL_WINDOW_FULLSCREEN;
    } else {
        window->flags &= ~SDL_WINDOW_FULLSCREEN;
    }
    if (wd->rep_fullscreen != on) {
        wd->rep_fullscreen = on;
        SDLOP_SendWindowEvent(window, on ? SDL_EVENT_WINDOW_ENTER_FULLSCREEN : SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 0, 0);
    }
}

static void report_maximized(SDL_Window *window, bool on)
{
    X11WindowData *wd = wdata(window);
    if (on) {
        window->flags |= SDL_WINDOW_MAXIMIZED;
    } else {
        window->flags &= ~SDL_WINDOW_MAXIMIZED;
    }
    if (wd->rep_maximized != on) {
        wd->rep_maximized = on;
        SDLOP_SendWindowEvent(window, on ? SDL_EVENT_WINDOW_MAXIMIZED : SDL_EVENT_WINDOW_RESTORED, 0, 0);
    }
}

static void report_minimized(SDL_Window *window, bool on)
{
    X11WindowData *wd = wdata(window);
    if (on) {
        window->flags |= SDL_WINDOW_MINIMIZED;
    } else {
        window->flags &= ~SDL_WINDOW_MINIMIZED;
    }
    if (wd->rep_minimized != on) {
        wd->rep_minimized = on;
        SDLOP_SendWindowEvent(window, on ? SDL_EVENT_WINDOW_MINIMIZED : SDL_EVENT_WINDOW_RESTORED, 0, 0);
    }
}

/* _NET_WM_STATE / WM_STATE changed: act on observed transitions only, so a
 * request whose confirmation is still in flight is never "undone" by an
 * unrelated property update (SDL3 X11_GetNetWMState semantics) */
static void refresh_wm_state(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    bool fullscreen = false, max_v = false, max_h = false, hidden = false;
    uint32_t *atoms = NULL;
    int n = get_property32(wd->xid, x11.atom._NET_WM_STATE, XCB_ATOM_ATOM, 64, &atoms);
    for (int i = 0; i < n; i++) {
        if (atoms[i] == x11.atom._NET_WM_STATE_FULLSCREEN) {
            fullscreen = true;
        } else if (atoms[i] == x11.atom._NET_WM_STATE_MAXIMIZED_VERT) {
            max_v = true;
        } else if (atoms[i] == x11.atom._NET_WM_STATE_MAXIMIZED_HORZ) {
            max_h = true;
        } else if (atoms[i] == x11.atom._NET_WM_STATE_HIDDEN) {
            hidden = true;
        }
    }
    free(atoms);
    uint32_t *state = NULL;
    if (get_property32(wd->xid, x11.atom.WM_STATE, x11.atom.WM_STATE, 2, &state) >= 1 &&
        state[0] == X11_STATE_ICONIC) {
        hidden = true;
    }
    free(state);

    if (wd->legacy_fullscreen || wd->legacy_maximized) {
        return; /* emulated state, no WM opinion to follow */
    }
    bool maximized = max_v && max_h;
    if (fullscreen != wd->obs_fullscreen) {
        wd->obs_fullscreen = fullscreen;
        report_fullscreen(window, fullscreen);
    }
    /* while fullscreen, keep the pre-fullscreen maximized state (SDL3) */
    if (maximized != wd->obs_maximized && !fullscreen) {
        wd->obs_maximized = maximized;
        report_maximized(window, maximized);
    }
    if (hidden != wd->obs_iconic) {
        wd->obs_iconic = hidden;
        report_minimized(window, hidden);
    }
}

/* ------------------------------------------------------------------ */
/* software framebuffer (MIT-SHM, xcb_put_image fallback)              */
/* ------------------------------------------------------------------ */

static void fb_free(X11WindowData *wd)
{
    if (!wd->pixels) {
        return;
    }
    if (wd->fb_shm) {
        X11_xcb_shm_detach(x11.conn, wd->shm_seg);
        x11_sync(); /* the server must let go before we unmap */
        shmdt(wd->pixels);
    } else {
        free(wd->pixels);
    }
    wd->pixels = NULL;
    wd->fb_shm = false;
    wd->fb_w = wd->fb_h = 0;
    wd->fb_cap = 0;
}

static void fb_fill_clear(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    uint32_t color = ((uint32_t)window->clear_r << 16) | ((uint32_t)window->clear_g << 8) | window->clear_b;
    if (wd->depth == 32) {
        /* SDL_WINDOW_TRANSPARENT: fully transparent unless a colour was set */
        color = wd->clear_explicit ? (color | 0xff000000u) : 0;
    }
    size_t n = (size_t)wd->fb_w * (size_t)wd->fb_h;
    for (size_t i = 0; i < n; i++) {
        wd->pixels[i] = color;
    }
}

/* the software framebuffer at w x h, cleared. Memory is kept when the new
 * size fits (new memory, with 25% headroom, only for growth beyond it or
 * after shrinking to less than a quarter), so resizing costs no SHM
 * attach/detach round trips most of the time. An existing window surface
 * is pointed at the result in place. */
static bool fb_alloc(SDL_Window *window, int w, int h)
{
    X11WindowData *wd = wdata(window);
    if (wd->pixels && wd->fb_w == w && wd->fb_h == h) {
        return true;
    }
    const size_t need = (size_t)w * (size_t)h * 4;
    if (wd->pixels && wd->fb_cap >= need && wd->fb_cap / 4 <= need) {
        wd->fb_w = w;
        wd->fb_h = h;
        fb_fill_clear(window);
        if (window->surface) {
            SDLOP_SetSurfacePixels(window->surface, wd->pixels, w, h, w * 4);
        }
        return true;
    }
    fb_free(wd);
    const size_t size = need + need / 4;

    if (x11.has_shm) {
        int shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
        if (shmid >= 0) {
            void *addr = shmat(shmid, NULL, 0);
            if (addr != (void *)-1) {
                xcb_shm_seg_t seg = X11_xcb_generate_id(x11.conn);
                xcb_generic_error_t *err =
                    X11_xcb_request_check(x11.conn, X11_xcb_shm_attach_checked(x11.conn, seg, (uint32_t)shmid, 0));
                if (!err) {
                    wd->pixels = (uint32_t *)addr;
                    wd->fb_shm = true;
                    wd->shm_seg = seg;
                } else {
                    /* e.g. remote display: SHM can never work, stop trying */
                    free(err);
                    shmdt(addr);
                    x11.has_shm = false;
                    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop x11: MIT-SHM attach failed, using xcb_put_image");
                }
            }
            /* segment lives on until the last detach */
            shmctl(shmid, IPC_RMID, NULL);
        } else {
            x11.has_shm = false;
        }
    }
    if (!wd->pixels) {
        wd->pixels = (uint32_t *)malloc(size);
        if (!wd->pixels) {
            return SDL_OutOfMemory();
        }
    }
    wd->fb_w = w;
    wd->fb_h = h;
    wd->fb_cap = size;
    fb_fill_clear(window);

    /* SDLop: an existing window surface stays valid across resizes */
    if (window->surface) {
        SDLOP_SetSurfacePixels(window->surface, wd->pixels, w, h, w * 4);
    }
    return true;
}

static void fb_put(X11WindowData *wd, int x, int y, int w, int h)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > wd->fb_w) {
        w = wd->fb_w - x;
    }
    if (y + h > wd->fb_h) {
        h = wd->fb_h - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    if (wd->fb_shm) {
        X11_xcb_shm_put_image(x11.conn, wd->xid, wd->gc, (uint16_t)wd->fb_w, (uint16_t)wd->fb_h,
                              (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, (int16_t)x, (int16_t)y,
                              wd->depth, XCB_IMAGE_FORMAT_Z_PIXMAP, 0, wd->shm_seg, 0);
        return;
    }
    /* full-width row bands that fit the maximum request size */
    size_t stride = (size_t)wd->fb_w * 4;
    int rows = (int)((x11.max_request_bytes - 64) / stride);
    if (rows < 1) {
        rows = 1;
    }
    for (int row = y; row < y + h; row += rows) {
        int n = SDL_min(rows, y + h - row);
        X11_xcb_put_image(x11.conn, XCB_IMAGE_FORMAT_Z_PIXMAP, wd->xid, wd->gc, (uint16_t)wd->fb_w,
                          (uint16_t)n, 0, (int16_t)row, 0, wd->depth, (uint32_t)(stride * (size_t)n),
                          (const uint8_t *)(wd->pixels + (size_t)row * (size_t)wd->fb_w));
    }
}

/* _NET_WM_SYNC_REQUEST: after the first frame drawn for the configure
 * that followed the request, set the counter to the requested value so
 * the WM knows the client caught up (stock X11_HandlePresent) */
void SDLOP_X11_HandlePresent(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    if (!wd || !wd->sync_counter || wd->sync_state != SYNC_CONFIGURED) {
        return;
    }
    X11_xcb_sync_set_counter(x11.conn, wd->sync_counter, wd->sync_value);
    wd->sync_state = SYNC_IDLE;
    x11_flush();
}

static void fb_present(SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    X11WindowData *wd = wdata(window);
    if (!wd->pixels || wd->gl_active) {
        return;
    }
    if (rects && numrects > 0) {
        for (int i = 0; i < numrects; i++) {
            fb_put(wd, rects[i].x, rects[i].y, rects[i].w, rects[i].h);
        }
    } else {
        fb_put(wd, 0, 0, wd->fb_w, wd->fb_h);
    }
    SDLOP_X11_HandlePresent(window);
    if (wd->fb_shm) {
        /* the server reads SHM asynchronously; don't let the app scribble
         * over pixels that are still being copied (SDL3 does XSync too) */
        x11_sync();
    } else {
        x11_flush();
    }
}

static bool x11_CreateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface **surface)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    if (!x11.fb_format_ok) {
        return SDL_SetError("X11 visual is not 24-bit TrueColor XRGB; no window surface");
    }
    if (!fb_alloc(window, window->w, window->h)) {
        return false;
    }
    /* ARGB visual: the compositor takes the alpha channel (premultiplied) */
    const SDL_PixelFormat fmt = wd->depth == 32 ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_XRGB8888;
    SDL_Surface *s = SDL_CreateSurfaceFrom(wd->fb_w, wd->fb_h, fmt, wd->pixels, wd->fb_w * 4);
    if (!s) {
        return false;
    }
    *surface = s;
    return true;
}

static bool x11_ResizeWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface *surface)
{
    (void)device;
    (void)surface; /* == window->surface, updated by fb_alloc */
    X11WindowData *wd = wdata(window);
    if (!wd || !wd->pixels || window->w <= 0 || window->h <= 0 ||
        (wd->fb_w == window->w && wd->fb_h == window->h)) {
        return true;
    }
    return fb_alloc(window, window->w, window->h);
}

static bool x11_UpdateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd || !wd->pixels || wd->gl_active) {
        return SDL_SetError("Window surface is not presentable");
    }
    fb_present(window, rects, numrects);
    return true;
}

static void x11_DestroyWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    /* the surface wraps wd->pixels; those stay until resize/destroy */
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* accessors for the GL / Vulkan modules                               */
/* ------------------------------------------------------------------ */

xcb_connection_t *SDLOP_X11_GetConnection(void)
{
    return x11.conn;
}

xcb_window_t SDLOP_X11_GetRoot(void)
{
    return x11.root;
}

int SDLOP_X11_GetScreenNumber(void)
{
    return x11.screen_num;
}

xcb_visualid_t SDLOP_X11_GetVisual(void)
{
    return x11.visual;
}

SDL_Window *SDLOP_X11_WindowFromXID(xcb_window_t xid)
{
    return window_from_xid(xid);
}

xcb_window_t SDLOP_X11_GetWindowXID(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    return wd ? wd->xid : XCB_WINDOW_NONE;
}

xcb_visualid_t SDLOP_X11_GetWindowVisual(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    return (wd && wd->visual) ? wd->visual : x11.visual;
}

bool SDLOP_X11_IsCompatibleVisual(xcb_visualid_t visual)
{
    if (visual == x11.visual) {
        return true;
    }
    if (!x11.screen || visual == 0) {
        return false;
    }
    xcb_depth_iterator_t di = X11_xcb_screen_allowed_depths_iterator(x11.screen);
    for (; di.rem; X11_xcb_depth_next(&di)) {
        if (di.data->depth != x11.depth) {
            continue;
        }
        xcb_visualtype_iterator_t vi = X11_xcb_depth_visuals_iterator(di.data);
        for (; vi.rem; X11_xcb_visualtype_next(&vi)) {
            if (vi.data->visual_id == visual) {
                return vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR;
            }
        }
    }
    return false;
}

bool SDLOP_X11_IsARGBVisual(xcb_visualid_t visual)
{
    if (!x11.screen || visual == 0 || !x11.argb_visual) {
        return false;
    }
    if (visual == x11.argb_visual) {
        return true;
    }
    xcb_depth_iterator_t di = X11_xcb_screen_allowed_depths_iterator(x11.screen);
    for (; di.rem; X11_xcb_depth_next(&di)) {
        if (di.data->depth != 32) {
            continue;
        }
        xcb_visualtype_iterator_t vi = X11_xcb_depth_visuals_iterator(di.data);
        for (; vi.rem; X11_xcb_visualtype_next(&vi)) {
            if (vi.data->visual_id == visual) {
                return vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR && vi.data->red_mask == 0xff0000 &&
                       vi.data->green_mask == 0x00ff00 && vi.data->blue_mask == 0x0000ff;
            }
        }
    }
    return false;
}

void SDLOP_X11_SetGLActive(SDL_Window *window, bool active)
{
    X11WindowData *wd = wdata(window);
    if (wd) {
        wd->gl_active = active;
    }
}

void **SDLOP_X11_GetGLSurfaceSlot(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    return wd ? &wd->egl_surface : NULL;
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */
/* ------------------------------------------------------------------ */

static void x11_sync_xkb_state(void)
{
    if (!x11.has_xkb) {
        return;
    }
    xcb_xkb_get_state_reply_t *st = X11_xcb_xkb_get_state_reply(
        x11.conn, X11_xcb_xkb_get_state(x11.conn, (xcb_xkb_device_spec_t)x11.xkb_device_id), NULL);
    if (st) {
        SDLOP_KeyboardUpdateXkbState(st->baseMods, st->latchedMods, st->lockedMods,
                                     (Uint32)st->baseGroup, (Uint32)st->latchedGroup, st->lockedGroup);
        free(st);
    }
}

/* XKB re-announces the keymap whenever the master keyboard switches slave
 * devices (e.g. physical keyboard <-> XTest). Only a real change may
 * reach the core, which emits SDL_EVENT_KEYMAP_CHANGED. */
static bool set_keymap_string(const char *str)
{
    uint64_t h = 1469598103934665603ull; /* FNV-1a */
    for (const char *p = str; *p; p++) {
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    }
    if (h == x11.keymap_hash) {
        return true;
    }
    if (!SDLOP_KeyboardSetKeymapString(str, strlen(str))) {
        return false;
    }
    x11.keymap_hash = h;
    return true;
}

static bool keymap_from_xkbcommon_x11(void)
{
    if (!x11.has_xkb || !SDLOP_X11_HAVE(XKBX11) || !SDLOP_Xkb_LoadSymbols()) {
        return false;
    }
    bool ok = false;
    struct xkb_context *ctx = SDLOP_XKB_xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (ctx) {
        struct xkb_keymap *km = X11_xkb_x11_keymap_new_from_device(ctx, x11.conn, x11.xkb_device_id,
                                                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (km) {
            char *str = SDLOP_XKB_xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
            if (str) {
                ok = set_keymap_string(str);
                free(str);
            }
            SDLOP_XKB_xkb_keymap_unref(km);
        }
        SDLOP_XKB_xkb_context_unref(ctx);
    }
    return ok;
}

/* without libxkbcommon-x11: compile the server's RMLVO names locally */
static bool keymap_from_rules_names(void)
{
    if (!SDLOP_Xkb_LoadSymbols()) {
        return false;
    }
    char *names = NULL;
    int len = get_property_bytes(x11.root, x11.atom._XKB_RULES_NAMES, XCB_ATOM_STRING, &names);
    if (len <= 0) {
        free(names);
        return false;
    }
    const char *part[5] = { NULL, NULL, NULL, NULL, NULL };
    int p = 0;
    for (int i = 0; i < len && p < 5;) {
        part[p++] = names + i;
        i += (int)strlen(names + i) + 1;
    }
    struct xkb_rule_names rmlvo = { part[0], part[1], part[2], part[3], part[4] };
    bool ok = false;
    struct xkb_context *ctx = SDLOP_XKB_xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (ctx) {
        struct xkb_keymap *km = SDLOP_XKB_xkb_keymap_new_from_names(ctx, &rmlvo, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (km) {
            char *str = SDLOP_XKB_xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
            if (str) {
                ok = set_keymap_string(str);
                free(str);
            }
            SDLOP_XKB_xkb_keymap_unref(km);
        }
        SDLOP_XKB_xkb_context_unref(ctx);
    }
    free(names);
    return ok;
}

static void x11_load_keymap(void)
{
    if (!keymap_from_xkbcommon_x11() && !keymap_from_rules_names()) {
        SDL_ClearError(); /* the core falls back to the default keymap */
        return;
    }
    x11_sync_xkb_state();
}

static void x11_init_xkb(void)
{
    if (!SDLOP_X11_HAVE(XKB)) {
        return;
    }
    const xcb_query_extension_reply_t *ext = X11_xcb_get_extension_data(x11.conn, X11_xcb_xkb_id);
    if (!ext || !ext->present) {
        return;
    }
    if (SDLOP_X11_HAVE(XKBX11)) {
        uint16_t major = 0, minor = 0;
        uint8_t base_event = 0, base_error = 0;
        if (!X11_xkb_x11_setup_xkb_extension(x11.conn, XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
                                             XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, &major, &minor,
                                             &base_event, &base_error)) {
            return;
        }
        x11.xkb_device_id = X11_xkb_x11_get_core_keyboard_device_id(x11.conn);
        if (x11.xkb_device_id < 0) {
            return;
        }
    } else {
        xcb_xkb_use_extension_reply_t *r =
            X11_xcb_xkb_use_extension_reply(x11.conn, X11_xcb_xkb_use_extension(x11.conn, 1, 0), NULL);
        bool supported = r && r->supported;
        free(r);
        if (!supported) {
            return;
        }
        x11.xkb_device_id = XCB_XKB_ID_USE_CORE_KBD;
    }
    x11.has_xkb = true;
    x11.xkb_event_base = ext->first_event;

    /* keymap, state and repeat-rate notifications (libxkbcommon's
     * interactive-x11 recipe, as used by swa) */
    const uint16_t events = XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY |
                            XCB_XKB_EVENT_TYPE_STATE_NOTIFY | XCB_XKB_EVENT_TYPE_CONTROLS_NOTIFY;
    const uint16_t map_parts = XCB_XKB_MAP_PART_KEY_TYPES | XCB_XKB_MAP_PART_KEY_SYMS |
                               XCB_XKB_MAP_PART_MODIFIER_MAP | XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS |
                               XCB_XKB_MAP_PART_KEY_ACTIONS | XCB_XKB_MAP_PART_VIRTUAL_MODS |
                               XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP;
    xcb_xkb_select_events_details_t details;
    memset(&details, 0, sizeof(details));
    details.affectNewKeyboard = XCB_XKB_NKN_DETAIL_KEYCODES;
    details.newKeyboardDetails = XCB_XKB_NKN_DETAIL_KEYCODES;
    details.affectState = XCB_XKB_STATE_PART_MODIFIER_BASE | XCB_XKB_STATE_PART_MODIFIER_LATCH |
                          XCB_XKB_STATE_PART_MODIFIER_LOCK | XCB_XKB_STATE_PART_GROUP_BASE |
                          XCB_XKB_STATE_PART_GROUP_LATCH | XCB_XKB_STATE_PART_GROUP_LOCK;
    details.stateDetails = details.affectState;
    details.affectCtrls = XCB_XKB_BOOL_CTRL_REPEAT_KEYS;
    details.ctrlDetails = XCB_XKB_BOOL_CTRL_REPEAT_KEYS;
    free(X11_xcb_request_check(x11.conn, X11_xcb_xkb_select_events_aux_checked(
                                              x11.conn, (xcb_xkb_device_spec_t)x11.xkb_device_id, events, 0, 0,
                                              map_parts, map_parts, &details)));

    /* "detectable autorepeat": held keys produce press,press,..,release
     * instead of release/press pairs, so repeats are trivially dropped
     * (SDLop generates its own repeats, like on Wayland) */
    xcb_xkb_per_client_flags_reply_t *pcf = X11_xcb_xkb_per_client_flags_reply(
        x11.conn,
        X11_xcb_xkb_per_client_flags(x11.conn, (xcb_xkb_device_spec_t)x11.xkb_device_id,
                                     XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
                                     XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT, 0, 0, 0),
        NULL);
    if (pcf) {
        x11.detectable_repeat = (pcf->value & XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT) != 0;
        free(pcf);
    }
}

static void x11_sync_repeat_info(void)
{
    if (!x11.has_xkb) {
        return;
    }
    xcb_xkb_get_controls_reply_t *c = X11_xcb_xkb_get_controls_reply(
        x11.conn, X11_xcb_xkb_get_controls(x11.conn, (xcb_xkb_device_spec_t)x11.xkb_device_id), NULL);
    if (c) {
        sdlop.repeat_disabled = !(c->enabledControls & XCB_XKB_BOOL_CTRL_REPEAT_KEYS); /* xset r off */
        if (c->repeatDelay > 0 && c->repeatInterval > 0) {
            SDLOP_RawInputSetRepeatInfo(c->repeatDelay, c->repeatInterval);
        }
        free(c);
    }
}

static void handle_xkb_event(xcb_generic_event_t *ev)
{
    /* every XKB event shares this header */
    struct {
        uint8_t response_type;
        uint8_t xkbType;
        uint16_t sequence;
        xcb_timestamp_t time;
        uint8_t deviceID;
    } *any = (void *)ev;
    if (any->deviceID != (uint8_t)x11.xkb_device_id && x11.xkb_device_id != XCB_XKB_ID_USE_CORE_KBD) {
        return;
    }
    switch (any->xkbType) {
    case XCB_XKB_STATE_NOTIFY: {
        xcb_xkb_state_notify_event_t *s = (xcb_xkb_state_notify_event_t *)ev;
        SDLOP_KeyboardUpdateXkbState(s->baseMods, s->latchedMods, s->lockedMods,
                                     (Uint32)s->baseGroup, (Uint32)s->latchedGroup, s->lockedGroup);
        break;
    }
    case XCB_XKB_NEW_KEYBOARD_NOTIFY: {
        xcb_xkb_new_keyboard_notify_event_t *nk = (xcb_xkb_new_keyboard_notify_event_t *)ev;
        if (nk->changed & XCB_XKB_NKN_DETAIL_KEYCODES) {
            x11.keymap_dirty = true;
        }
        break;
    }
    case XCB_XKB_MAP_NOTIFY:
        x11.keymap_dirty = true; /* coalesce: setxkbmap sends bursts */
        break;
    case XCB_XKB_CONTROLS_NOTIFY:
        x11_sync_repeat_info();
        break;
    default:
        break;
    }
}

static void handle_key(const xcb_key_press_event_t *kev, bool down, bool from_im)
{
    SDL_Window *window = window_from_xid(kev->event);
    (void)window;
    if (SDLOP_RawInputIsSource()) {
        return; /* SDLOP_RAW_INPUT: evdev delivers every key; never mix the two */
    }
    if (kev->detail < 8) {
        return;
    }
    if (!from_im && SDLOP_X11_XIM_FilterKey(kev)) {
        return; /* taken by the input method: no key event (stock) */
    }
    Uint16 evdev = (Uint16)(kev->detail - 8);
    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    if (evdev <= SDLOP_EVDEV_KEY_MAX) {
        sc = (SDL_Scancode)sdlop_evdev_to_scancode[evdev];
    }
    if (sc == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    if (down && sdlop.keystate[sc]) {
        return; /* server autorepeat (detectable mode); repeats are client side */
    }
    if (!down && !x11.detectable_repeat) {
        /* classic autorepeat: Release+Press with the same timestamp */
        xcb_generic_event_t *next = x11.peeked ? x11.peeked : X11_xcb_poll_for_event(x11.conn);
        x11.peeked = next;
        if (next && (next->response_type & 0x7f) == XCB_KEY_PRESS) {
            xcb_key_press_event_t *np = (xcb_key_press_event_t *)next;
            if (np->detail == kev->detail && np->time == kev->time && np->event == kev->event) {
                free(next);
                x11.peeked = NULL;
                return; /* swallow both halves */
            }
        }
    }
    SDLOP_SendKeyboardKey(down, false, sc, evdev, SDLOP_MonotonicNS());
}

void SDLOP_X11_HandleKeyFromIM(const xcb_key_press_event_t *kev)
{
    handle_key(kev, (kev->response_type & 0x7f) == XCB_KEY_PRESS, true);
}

/* ------------------------------------------------------------------ */
/* focus / pointer                                                     */
/* ------------------------------------------------------------------ */

static void set_keyboard_focus(SDL_Window *window)
{
    if (sdlop.keyboard_focus == window) {
        return;
    }
    if (sdlop.keyboard_focus) {
        sdlop.keyboard_focus->flags &= ~SDL_WINDOW_INPUT_FOCUS;
        SDLOP_SendWindowEvent(sdlop.keyboard_focus, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
        sdlop.keyboard_focus = NULL;
        SDL_ResetKeyboard();
    }
    if (window) {
        sdlop.keyboard_focus = window;
        window->flags |= SDL_WINDOW_INPUT_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_FOCUS_GAINED, 0, 0);
    }
    SDLOP_X11_XIM_Update();
}

static bool grab_pointer(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    if (!wd->mapped) {
        return false;
    }
    xcb_grab_pointer_reply_t *r = X11_xcb_grab_pointer_reply(
        x11.conn,
        /* Grab on the root (confined to our window), not on the window
         * itself: the X server delivers XI 2.1+ raw events twice to a
         * client that holds a core grab on a non-root window (once through
         * the root selection, once through the grab). owner_events=1 keeps
         * normal delivery to our own windows. */
        X11_xcb_grab_pointer(x11.conn, 1, x11.root,
                             XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                 XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
                                 XCB_EVENT_MASK_LEAVE_WINDOW,
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, wd->xid /* confine_to */, x11.blank_cursor,
                             XCB_CURRENT_TIME),
        NULL);
    bool ok = r && r->status == XCB_GRAB_STATUS_SUCCESS;
    free(r);
    x11.pointer_grabbed = ok;
    wd->want_grab = !ok;
    return ok;
}

static void select_raw_motion(bool on)
{
    struct {
        xcb_input_event_mask_t head;
        uint32_t mask;
    } em;
    em.head.deviceid = X11_XI_ALL_MASTER_DEVICES;
    em.head.mask_len = 1;
    em.mask = on ? XCB_INPUT_XI_EVENT_MASK_RAW_MOTION : 0;
    free(X11_xcb_request_check(x11.conn, X11_xcb_input_xi_select_events_checked(x11.conn, x11.root, 1, &em.head)));
}

/* XI 2.4 gesture events (wire numbers: older xcb-proto lacks them) */
#define X11_XI_GESTURE_PINCH_BEGIN 27
#define X11_XI_GESTURE_PINCH_UPDATE 28
#define X11_XI_GESTURE_PINCH_END 29
#define X11_XI_MASK_GESTURE_PINCH ((1u << 27) | (1u << 28) | (1u << 29))

static bool select_master_events(xcb_window_t xid, uint32_t mask)
{
    struct {
        xcb_input_event_mask_t head;
        uint32_t mask;
    } em;
    em.head.deviceid = X11_XI_ALL_MASTER_DEVICES;
    em.head.mask_len = 1;
    em.mask = mask;
    xcb_generic_error_t *err =
        X11_xcb_request_check(x11.conn, X11_xcb_input_xi_select_events_checked(x11.conn, xid, 1, &em.head));
    free(err);
    return err == NULL;
}

/* XI2.2 multitouch on our windows (SDL3 xinput2_select_touch). With a
 * touch selection the server stops emulating the pointer for these
 * sequences; the core synthesizes the SDL_TOUCH_MOUSEID mouse instead.
 * XI 2.4 touchpad pinches go in the same per-window master mask. */
static void select_touch(xcb_window_t xid)
{
    if (!x11.has_xi2) {
        return;
    }
    const uint32_t touch = SDLOP_HintFlag("SDLOP_X11_NO_TOUCH", false)
                               ? 0
                               : (XCB_INPUT_XI_EVENT_MASK_TOUCH_BEGIN | XCB_INPUT_XI_EVENT_MASK_TOUCH_UPDATE |
                                  XCB_INPUT_XI_EVENT_MASK_TOUCH_END);
    const uint32_t gesture =
        x11.has_xi24 && !SDLOP_HintFlag("SDLOP_X11_NO_GESTURES", false) ? X11_XI_MASK_GESTURE_PINCH : 0;
    if (!(touch | gesture)) {
        return;
    }
    /* BadAccess if another client holds the touch (or gesture) selection:
     * then try them one by one; pointer emulation keeps working anyway */
    if (!select_master_events(xid, touch | gesture) && touch && gesture) {
        if (!select_master_events(xid, gesture)) {
            select_master_events(xid, touch);
        }
    }
}

/* XI 2.4 pinch (stock: scale passed through, it's relative to the start) */
static void handle_pinch(const xcb_ge_generic_event_t *ge)
{
    const uint8_t *b = (const uint8_t *)ge;
    const size_t size = 36 + (size_t)ge->length * 4; /* + xcb's full_sequence */
    if (size < 72) {
        return;
    }
    xcb_window_t event_win;
    int32_t scale_fp;
    memcpy(&event_win, b + 24, 4);
    memcpy(&scale_fp, b + 68, 4); /* wire offset 64, after full_sequence */
    SDL_Window *window = window_from_xid(event_win);
    if (!window) {
        return;
    }
    if (ge->event_type == X11_XI_GESTURE_PINCH_BEGIN) {
        SDLOP_SendPinch(SDL_EVENT_PINCH_BEGIN, 0, window, 0.0f);
    } else if (ge->event_type == X11_XI_GESTURE_PINCH_UPDATE) {
        SDLOP_SendPinch(SDL_EVENT_PINCH_UPDATE, 0, window, (float)scale_fp / 65536.0f);
    } else {
        SDLOP_SendPinch(SDL_EVENT_PINCH_END, 0, window, 0.0f);
    }
}

static void warp_to_center(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    x11.warp_cx = window->w / 2;
    x11.warp_cy = window->h / 2;
    X11_xcb_warp_pointer(x11.conn, XCB_WINDOW_NONE, wd->xid, 0, 0, 0, 0, (int16_t)x11.warp_cx, (int16_t)x11.warp_cy);
}

static void update_hit_test(SDL_Window *window, int x, int y, bool force);
static bool trigger_hit_test(SDL_Window *window, int16_t root_x, int16_t root_y);
static void send_moveresize(SDL_Window *window, int16_t root_x, int16_t root_y, uint32_t direction);

static void handle_motion(xcb_motion_notify_event_t *mev)
{
    SDL_Window *window = window_from_xid(mev->event);
    if (!window || SDLOP_X11_Pen_FilterCore(mev->time)) {
        return;
    }
    Uint64 ts = SDLOP_MonotonicNS();
    if (x11.relative_window == window) {
        if (x11.warp_relative) {
            int dx = mev->event_x - x11.warp_cx;
            int dy = mev->event_y - x11.warp_cy;
            if (dx || dy) {
                if (!SDLOP_RawInputIsSource()) { /* else evdev sends the deltas */
                    SDLOP_SendMouseMotion(SDLOP_NO_POS, SDLOP_NO_POS, (float)dx, (float)dy, ts);
                }
                warp_to_center(window); /* still keep the hidden pointer inside */
            }
        }
        return; /* XInput2 raw motion carries relative input */
    }
    if (sdlop.mouse_focus != window) {
        /* motion without a preceding EnterNotify (e.g. after an ungrab) */
        sdlop.mouse_focus = window;
        window->flags |= SDL_WINDOW_MOUSE_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_ENTER, 0, 0);
    }
    update_hit_test(window, mev->event_x, mev->event_y, false);
    if (SDLOP_RawInputIsSource()) {
        /* evdev sends the motion; the server's pointer only re-anchors the
           absolute position evdev's relative deltas are added to */
        sdlop.mouse_x = mev->event_x;
        sdlop.mouse_y = mev->event_y;
        sdlop.mouse_has_position = true;
        return;
    }
    if ((window->flags & SDL_WINDOW_INPUT_FOCUS) && !SDL_RectEmpty(&window->mouse_rect) &&
        !(window->flags & SDL_WINDOW_MOUSE_CAPTURE)) {
        /* SDL_SetWindowMouseRect: XFixes barriers fence the pointer in;
         * without them (or before they are up) warp it back */
        const SDL_Rect *r = &window->mouse_rect;
        int cx = SDL_clamp((int)mev->event_x, r->x, r->x + r->w - 1);
        int cy = SDL_clamp((int)mev->event_y, r->y, r->y + r->h - 1);
        if (cx != mev->event_x || cy != mev->event_y) {
            X11_xcb_warp_pointer(x11.conn, XCB_WINDOW_NONE, mev->event, 0, 0, 0, 0, (int16_t)cx, (int16_t)cy);
        }
    }
    SDLOP_SendMouseMotion((float)mev->event_x, (float)mev->event_y, 0.0f, 0.0f, ts);
}

static void handle_button(xcb_button_press_event_t *bev, bool down)
{
    SDL_Window *window = window_from_xid(bev->event);
    if (!window || SDLOP_X11_Pen_FilterCore(bev->time)) {
        return;
    }
    Uint64 ts = SDLOP_MonotonicNS();
    const bool raw_source = SDLOP_RawInputIsSource(); /* evdev sends buttons and wheel */
    /* core protocol wheel: 4/5 vertical, 6/7 horizontal (press only) */
    if (bev->detail >= 4 && bev->detail <= 7) {
        if (down && !raw_source) {
            float x = 0.0f, y = 0.0f;
            switch (bev->detail) {
            case 4: y = 1.0f; break;
            case 5: y = -1.0f; break;
            case 6: x = -1.0f; break;
            default: x = 1.0f; break;
            }
            SDLOP_SendMouseWheel(x, y, ts);
        }
        return;
    }
    Uint8 button;
    switch (bev->detail) {
    case 1: button = SDL_BUTTON_LEFT; break;
    case 2: button = SDL_BUTTON_MIDDLE; break;
    case 3: button = SDL_BUTTON_RIGHT; break;
    default: button = (Uint8)(bev->detail - 8 + SDL_BUTTON_X1); break; /* 8, 9 -> X1, X2 */
    }
    if (down && x11.relative_window == window && x11.pointer_grabbed == false && wdata(window)->want_grab) {
        grab_pointer(window);
    }
    if (down && button == SDL_BUTTON_LEFT && x11.relative_window != window &&
        trigger_hit_test(window, bev->root_x, bev->root_y)) {
        return; /* not passed on to the app (stock) */
    }
    if (raw_source) {
        return;
    }
    SDLOP_SendMouseButton(down, button, SDLOP_NO_POS, SDLOP_NO_POS, ts);
}

static double fp3232(xcb_input_fp3232_t v)
{
    return (double)v.integral + (double)v.frac / 4294967296.0;
}

static float fp1616(xcb_input_fp1616_t v)
{
    return (float)v / 65536.0f;
}

static int touch_slot(uint16_t source, uint32_t detail)
{
    for (int i = 0; i < (int)SDL_arraysize(x11.fingers); i++) {
        if (x11.fingers[i].active && x11.fingers[i].source == source && x11.fingers[i].detail == detail) {
            return i;
        }
    }
    return -1;
}

static void handle_touch(xcb_input_touch_begin_event_t *te)
{
    int slot = touch_slot(te->sourceid, te->detail);
    SDL_Window *window;
    if (te->event_type == XCB_INPUT_TOUCH_BEGIN) {
        window = window_from_xid(te->event);
        if (!window || slot >= 0) {
            return;
        }
        for (int i = 0; i < (int)SDL_arraysize(x11.fingers); i++) {
            if (!x11.fingers[i].active) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            return; /* more simultaneous touches than we track */
        }
    } else {
        if (slot < 0) {
            return; /* began before the window existed / slot overflow */
        }
        window = x11.fingers[slot].window;
    }
    if (window->w <= 0 || window->h <= 0) {
        return;
    }
    /* SDL3 X11: event-window pixel coordinates normalized by the window
     * size, clamped to 0..1 */
    float nx = SDL_max(0.0f, SDL_min(fp1616(te->event_x) / (float)window->w, 1.0f));
    float ny = SDL_max(0.0f, SDL_min(fp1616(te->event_y) / (float)window->h, 1.0f));
    SDL_TouchID touch = (SDL_TouchID)te->sourceid;
    SDL_FingerID finger = (SDL_FingerID)te->detail;
    Uint64 now = SDLOP_MonotonicNS();
    switch (te->event_type) {
    case XCB_INPUT_TOUCH_BEGIN:
        x11.fingers[slot].active = true;
        x11.fingers[slot].source = te->sourceid;
        x11.fingers[slot].detail = te->detail;
        x11.fingers[slot].window = window;
        x11.fingers[slot].nx = nx;
        x11.fingers[slot].ny = ny;
        SDLOP_SendTouch(SDL_EVENT_FINGER_DOWN, touch, finger, nx, ny, 0.0f, 0.0f, 1.0f, window->id, now);
        break;
    case XCB_INPUT_TOUCH_UPDATE: {
        float dx = nx - x11.fingers[slot].nx;
        float dy = ny - x11.fingers[slot].ny;
        if (dx == 0.0f && dy == 0.0f) {
            break;
        }
        x11.fingers[slot].nx = nx;
        x11.fingers[slot].ny = ny;
        SDLOP_SendTouch(SDL_EVENT_FINGER_MOTION, touch, finger, nx, ny, dx, dy, 1.0f, window->id, now);
        break;
    }
    case XCB_INPUT_TOUCH_END:
        x11.fingers[slot].active = false;
        SDLOP_SendTouch(SDL_EVENT_FINGER_UP, touch, finger, nx, ny, 0.0f, 0.0f, 0.0f, window->id, now);
        break;
    default:
        break;
    }
}

/* window going away mid-sequence: its fingers are canceled */
static void cancel_touches(SDL_Window *window)
{
    for (int i = 0; i < (int)SDL_arraysize(x11.fingers); i++) {
        if (x11.fingers[i].active && (!window || x11.fingers[i].window == window)) {
            x11.fingers[i].active = false;
            SDLOP_SendTouch(SDL_EVENT_FINGER_CANCELED, x11.fingers[i].source, x11.fingers[i].detail,
                            x11.fingers[i].nx, x11.fingers[i].ny, 0.0f, 0.0f, 0.0f, x11.fingers[i].window->id,
                            SDLOP_MonotonicNS());
        }
    }
}

static void handle_xi_event(xcb_ge_generic_event_t *ge)
{
    if (ge->event_type == XCB_INPUT_TOUCH_BEGIN || ge->event_type == XCB_INPUT_TOUCH_UPDATE ||
        ge->event_type == XCB_INPUT_TOUCH_END) {
        handle_touch((xcb_input_touch_begin_event_t *)ge);
        return;
    }
    if (SDLOP_X11_Pen_HandleEvent(ge)) {
        return;
    }
    if (ge->event_type >= X11_XI_GESTURE_PINCH_BEGIN && ge->event_type <= X11_XI_GESTURE_PINCH_END) {
        handle_pinch(ge);
        return;
    }
    if (ge->event_type != XCB_INPUT_RAW_MOTION || !x11.relative_window) {
        return;
    }
    /* only while the relative window has the input */
    if (sdlop.keyboard_focus != x11.relative_window && !x11.pointer_grabbed) {
        return;
    }
    xcb_input_raw_button_press_event_t *raw = (xcb_input_raw_button_press_event_t *)ge;
    if (SDLOP_X11_Pen_IsPenDevice(raw->sourceid)) {
        return; /* pens are absolute: no relative motion (stock) */
    }
    const uint32_t *mask = X11_xcb_input_raw_button_press_valuator_mask(raw);
    const xcb_input_fp3232_t *values = X11_xcb_input_raw_button_press_axisvalues_raw(raw);
    double dx = 0.0, dy = 0.0;
    int vi = 0;
    for (int axis = 0; axis < raw->valuators_len * 32 && axis < 16; axis++) {
        if (mask[axis / 32] & (1u << (axis % 32))) {
            if (axis == 0) {
                dx = fp3232(values[vi]);
            } else if (axis == 1) {
                dy = fp3232(values[vi]);
            }
            vi++;
        }
    }
    if ((dx != 0.0 || dy != 0.0) && !SDLOP_RawInputIsSource()) {
        SDLOP_SendMouseMotion(SDLOP_NO_POS, SDLOP_NO_POS, (float)dx, (float)dy, SDLOP_MonotonicNS());
    }
}

/* ------------------------------------------------------------------ */
/* displays (RandR >= 1.5 monitors, else the root window)              */
/* ------------------------------------------------------------------ */

static float x11_global_content_scale(void)
{
    /* SDL3 order: explicit override, Xft.dpi, XSETTINGS, GDK_SCALE */
    const char *env = SDL_GetHint(SDL_HINT_VIDEO_X11_SCALING_FACTOR);
    if (env && *env) {
        float f = strtof(env, NULL);
        if (f >= 1.0f && f <= 10.0f) {
            return f;
        }
    }
    char *res = NULL;
    float scale = 0.0f;
    if (get_property_bytes(x11.root, x11.atom.RESOURCE_MANAGER, XCB_ATOM_STRING, &res) > 0) {
        for (char *line = res; line && *line;) {
            char *nl = strchr(line, '\n');
            if (nl) {
                *nl = '\0';
            }
            if (strncmp(line, "Xft.dpi:", 8) == 0) {
                float dpi = strtof(line + 8, NULL);
                if (dpi > 0.0f) {
                    scale = dpi / 96.0f;
                }
            }
            line = nl ? nl + 1 : NULL;
        }
    }
    free(res);
    /* then XSETTINGS: the GDK window scale, Xft/DPI in 1024ths */
    int value;
    if (scale <= 0.0f && SDLOP_X11_XSettings_GetInt("Gdk/WindowScalingFactor", &value) && value > 0) {
        scale = (float)value;
    }
    if (scale <= 0.0f && SDLOP_X11_XSettings_GetInt("Xft/DPI", &value) && value > 0) {
        scale = (float)value / 1024.0f / 96.0f;
    }
    if (scale <= 0.0f) {
        env = getenv("GDK_SCALE");
        if (env && *env) {
            scale = (float)atoi(env);
        }
    }
    return scale >= 1.0f ? scale : 1.0f;
}

static int mode_refresh_mhz(const xcb_randr_mode_info_t *m)
{
    uint64_t vtotal = m->vtotal;
    if (m->mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN) {
        vtotal *= 2;
    }
    if (m->mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE) {
        vtotal /= 2;
    }
    if (!m->htotal || !vtotal) {
        return 0;
    }
    return (int)(((uint64_t)m->dot_clock * 1000u) / ((uint64_t)m->htotal * vtotal));
}

static const xcb_randr_mode_info_t *find_mode(xcb_randr_get_screen_resources_current_reply_t *res,
                                               xcb_randr_mode_t id)
{
    xcb_randr_mode_info_t *modes = X11_xcb_randr_get_screen_resources_current_modes(res);
    int n = X11_xcb_randr_get_screen_resources_current_modes_length(res);
    for (int i = 0; i < n; i++) {
        if (modes[i].id == id) {
            return &modes[i];
        }
    }
    return NULL;
}

/* modes + current refresh of the first output driving a monitor */
static void fill_output_modes(X11Monitor *mon, xcb_randr_get_screen_resources_current_reply_t *res,
                              xcb_randr_output_t output)
{
    xcb_randr_get_output_info_reply_t *oi = X11_xcb_randr_get_output_info_reply(
        x11.conn, X11_xcb_randr_get_output_info(x11.conn, output, res->config_timestamp), NULL);
    if (!oi) {
        return;
    }
    if (oi->crtc != XCB_NONE) {
        xcb_randr_get_crtc_info_reply_t *ci = X11_xcb_randr_get_crtc_info_reply(
            x11.conn, X11_xcb_randr_get_crtc_info(x11.conn, oi->crtc, res->config_timestamp), NULL);
        if (ci) {
            const xcb_randr_mode_info_t *cur = find_mode(res, ci->mode);
            if (cur) {
                mon->refresh_mhz = mode_refresh_mhz(cur);
            }
            free(ci);
        }
    }
    xcb_randr_mode_t *ids = X11_xcb_randr_get_output_info_modes(oi);
    int n = X11_xcb_randr_get_output_info_modes_length(oi);
    for (int i = 0; i < n && mon->num_modes < (int)SDL_arraysize(mon->modes); i++) {
        const xcb_randr_mode_info_t *m = find_mode(res, ids[i]);
        if (!m || (m->mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE)) {
            continue;
        }
        mon->mode_ids[mon->num_modes] = m->id;
        SDL_DisplayMode *dm = &mon->modes[mon->num_modes++];
        memset(dm, 0, sizeof(*dm));
        dm->format = SDL_PIXELFORMAT_ARGB8888;
        dm->w = m->width;
        dm->h = m->height;
        dm->pixel_density = 1.0f;
        int mhz = mode_refresh_mhz(m);
        if (mhz > 0) {
            dm->refresh_rate = mhz / 1000.0f;
            dm->refresh_rate_numerator = mhz;
            dm->refresh_rate_denominator = 1000;
        }
    }
    free(oi);
}

static int enumerate_monitors(X11Monitor *out, int max)
{
    int count = 0;
    if (x11.randr_monitors) {
        xcb_randr_get_monitors_reply_t *mr = X11_xcb_randr_get_monitors_reply(
            x11.conn, X11_xcb_randr_get_monitors(x11.conn, x11.root, 1), NULL);
        xcb_randr_get_screen_resources_current_reply_t *res = X11_xcb_randr_get_screen_resources_current_reply(
            x11.conn, X11_xcb_randr_get_screen_resources_current(x11.conn, x11.root), NULL);
        if (mr) {
            xcb_randr_monitor_info_iterator_t it = X11_xcb_randr_get_monitors_monitors_iterator(mr);
            for (; it.rem && count < max; X11_xcb_randr_monitor_info_next(&it)) {
                xcb_randr_monitor_info_t *mi = it.data;
                if (mi->width == 0 || mi->height == 0) {
                    continue;
                }
                X11Monitor *mon = &out[count];
                memset(mon, 0, sizeof(*mon));
                mon->rect.x = mi->x;
                mon->rect.y = mi->y;
                mon->rect.w = mi->width;
                mon->rect.h = mi->height;
                mon->primary = mi->primary != 0;
                xcb_get_atom_name_reply_t *an = X11_xcb_get_atom_name_reply(
                    x11.conn, X11_xcb_get_atom_name(x11.conn, mi->name), NULL);
                if (an) {
                    int len = X11_xcb_get_atom_name_name_length(an);
                    if (len > (int)sizeof(mon->name) - 1) {
                        len = (int)sizeof(mon->name) - 1;
                    }
                    memcpy(mon->name, X11_xcb_get_atom_name_name(an), (size_t)len);
                    mon->name[len] = '\0';
                    free(an);
                }
                if (!mon->name[0]) {
                    snprintf(mon->name, sizeof(mon->name), "x11-%d", count + 1);
                }
                if (res && X11_xcb_randr_monitor_info_outputs_length(mi) > 0) {
                    mon->output = X11_xcb_randr_monitor_info_outputs(mi)[0];
                    fill_output_modes(mon, res, mon->output);
                }
                count++;
            }
            free(mr);
        }
        free(res);
    }
    if (count == 0 && max > 0) {
        X11Monitor *mon = &out[0];
        memset(mon, 0, sizeof(*mon));
        snprintf(mon->name, sizeof(mon->name), "x11-screen-%d", x11.screen_num);
        mon->rect.w = x11.screen->width_in_pixels;
        mon->rect.h = x11.screen->height_in_pixels;
        mon->primary = true;
        count = 1;
    }
    /* SDL's primary display is the first one */
    for (int i = 1; i < count; i++) {
        if (out[i].primary) {
            X11Monitor tmp = out[0];
            out[0] = out[i];
            out[i] = tmp;
            break;
        }
    }
    return count;
}

static void apply_monitor_to_display(const X11Monitor *mon)
{
    int idx = SDLOP_GetDisplayIndexById(mon->display_id);
    if (idx < 0) {
        return;
    }
    sdlop.displays[idx].bounds = mon->rect;
    sdlop.displays[idx].content_scale = x11.content_scale;
}

static void destroy_barriers(void);

static X11ModeSave *x11_mode_save(SDL_DisplayID id)
{
    for (int i = 0; i < x11.num_mode_saves; i++) {
        if (x11.mode_saves[i].display_id == id) {
            return &x11.mode_saves[i];
        }
    }
    return NULL;
}

static void x11_update_displays(bool send_events)
{
    X11Monitor fresh[SDLOP_MAX_DISPLAYS];
    int n = enumerate_monitors(fresh, SDLOP_MAX_DISPLAYS);
    float scale = x11_global_content_scale();
    bool scale_changed = scale != x11.content_scale;
    x11.content_scale = scale;

    /* keep displays whose name survived, drop the rest */
    for (int i = 0; i < x11.num_monitors; i++) {
        X11Monitor *old = &x11.monitors[i];
        int match = -1;
        for (int j = 0; j < n; j++) {
            if (strcmp(fresh[j].name, old->name) == 0 && fresh[j].display_id == 0) {
                match = j;
                break;
            }
        }
        if (match < 0) {
            SDLOP_DelDisplay(old->display_id, send_events);
            continue;
        }
        X11Monitor *nm = &fresh[match];
        nm->display_id = old->display_id;
        int idx = SDLOP_GetDisplayIndexById(nm->display_id);
        if (idx < 0) {
            continue;
        }
        SDLop_Display *d = &sdlop.displays[idx];
        if (nm->rect.x != old->rect.x || nm->rect.y != old->rect.y) {
            if (send_events) {
                SDLOP_SendDisplayEvent(nm->display_id, SDL_EVENT_DISPLAY_MOVED, 0, 0);
            }
        }
        /* while switched (exclusive fullscreen) the monitor shows the
         * fullscreen mode, not the desktop one */
        const int desk_mhz = d->desktop_mode.refresh_rate_denominator
                                 ? (int)((Sint64)d->desktop_mode.refresh_rate_numerator * 1000 /
                                         d->desktop_mode.refresh_rate_denominator)
                                 : 0;
        if (!x11_mode_save(nm->display_id) &&
            (nm->rect.w != d->desktop_mode.w || nm->rect.h != d->desktop_mode.h || nm->refresh_mhz != desk_mhz)) {
            d->desktop_mode.w = nm->rect.w;
            d->desktop_mode.h = nm->rect.h;
            d->desktop_mode.refresh_rate = nm->refresh_mhz / 1000.0f;
            d->desktop_mode.refresh_rate_numerator = nm->refresh_mhz;
            d->desktop_mode.refresh_rate_denominator = nm->refresh_mhz ? 1000 : 0;
            SDLOP_AddFullscreenDisplayMode(nm->display_id, &d->desktop_mode);
            if (send_events) {
                SDLOP_SendDisplayEvent(nm->display_id, SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED, 0, 0);
                SDLOP_SendDisplayEvent(nm->display_id, SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED, 0, 0);
            }
        }
        for (int m = 0; m < nm->num_modes; m++) {
            SDLOP_AddFullscreenDisplayMode(nm->display_id, &nm->modes[m]);
        }
        if (scale_changed && send_events) {
            SDLOP_SendDisplayEvent(nm->display_id, SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED, 0, 0);
        }
    }
    for (int j = 0; j < n; j++) {
        X11Monitor *nm = &fresh[j];
        if (nm->display_id) {
            continue;
        }
        nm->display_id = SDLOP_AddDisplay(nm->name, nm->rect.w, nm->rect.h, nm->refresh_mhz,
                                          nm->rect.w >= nm->rect.h ? SDL_ORIENTATION_LANDSCAPE
                                                                   : SDL_ORIENTATION_PORTRAIT,
                                          NULL, send_events);
        for (int m = 0; nm->display_id && m < nm->num_modes; m++) {
            SDLOP_AddFullscreenDisplayMode(nm->display_id, &nm->modes[m]);
        }
    }
    /* SDLOP_AddDisplay lays displays out left to right; X knows better */
    for (int j = 0; j < n; j++) {
        apply_monitor_to_display(&fresh[j]);
    }
    memcpy(x11.monitors, fresh, sizeof(X11Monitor) * (size_t)n);
    x11.num_monitors = n;

    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        if (w->display_scale != scale) {
            w->display_scale = scale;
            SDLOP_SendWindowEvent(w, SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED, 0, 0);
        }
        SDLOP_UpdateWindowDisplay(w);
    }
}

/* stock X11_SetDisplayMode, minus its single-monitor screen size: the
 * screen becomes the bounding box of every lit crtc with this one resized */
static bool x11_SetDisplayMode(SDLop_VideoDevice *device, SDLop_Display *display, const SDL_DisplayMode *mode)
{
    (void)device;
    const SDL_DisplayID id = display->id;
    X11Monitor *mon = NULL;
    for (int i = 0; i < x11.num_monitors; i++) {
        if (x11.monitors[i].display_id == id) {
            mon = &x11.monitors[i];
        }
    }
    if (!x11.has_randr || !mon || !mon->output) {
        return SDL_SetError("XRandR mode switching unavailable on this display");
    }
    X11ModeSave *save = x11_mode_save(id);
    const bool to_desktop = mode->w == display->desktop_mode.w && mode->h == display->desktop_mode.h &&
                            fabsf(mode->refresh_rate - display->desktop_mode.refresh_rate) < 0.01f;
    xcb_randr_mode_t want = XCB_NONE;
    if (to_desktop) {
        if (!save) {
            return true; /* never left it */
        }
        want = save->desktop_mode;
    } else {
        for (int i = 0; i < mon->num_modes && want == XCB_NONE; i++) {
            const SDL_DisplayMode *m = &mon->modes[i];
            if (m->w == mode->w && m->h == mode->h && fabsf(m->refresh_rate - mode->refresh_rate) < 0.01f) {
                want = mon->mode_ids[i];
            }
        }
        if (want == XCB_NONE) {
            return SDL_SetError("Display mode %dx%d@%g not offered by XRandR", mode->w, mode->h,
                                (double)mode->refresh_rate);
        }
    }

    xcb_randr_get_screen_resources_current_reply_t *res = X11_xcb_randr_get_screen_resources_current_reply(
        x11.conn, X11_xcb_randr_get_screen_resources_current(x11.conn, x11.root), NULL);
    if (!res) {
        return SDL_SetError("Couldn't get XRandR screen resources");
    }
    bool ok = false;
    xcb_randr_get_output_info_reply_t *oi = X11_xcb_randr_get_output_info_reply(
        x11.conn, X11_xcb_randr_get_output_info(x11.conn, mon->output, res->config_timestamp), NULL);
    xcb_randr_get_crtc_info_reply_t *ci = NULL;
    if (oi && oi->crtc != XCB_NONE) {
        ci = X11_xcb_randr_get_crtc_info_reply(
            x11.conn, X11_xcb_randr_get_crtc_info(x11.conn, oi->crtc, res->config_timestamp), NULL);
    }
    const xcb_randr_mode_info_t *mi = find_mode(res, want);
    if (!ci || !mi) {
        SDL_SetError("Couldn't get XRandR output/crtc info");
        goto done;
    }
    if (ci->mode == want) {
        ok = true;
        goto done;
    }
    const xcb_randr_crtc_t crtc = oi->crtc;
    const bool sideways = (ci->rotation & (XCB_RANDR_ROTATION_ROTATE_90 | XCB_RANDR_ROTATION_ROTATE_270)) != 0;
    int screen_w = ci->x + (sideways ? mi->height : mi->width);
    int screen_h = ci->y + (sideways ? mi->width : mi->height);
    xcb_randr_crtc_t *crtcs = X11_xcb_randr_get_screen_resources_current_crtcs(res);
    for (int i = 0; i < X11_xcb_randr_get_screen_resources_current_crtcs_length(res); i++) {
        if (crtcs[i] == crtc) {
            continue;
        }
        xcb_randr_get_crtc_info_reply_t *o = X11_xcb_randr_get_crtc_info_reply(
            x11.conn, X11_xcb_randr_get_crtc_info(x11.conn, crtcs[i], res->config_timestamp), NULL);
        if (o) {
            if (o->mode != XCB_NONE) {
                screen_w = SDL_max(screen_w, o->x + o->width);
                screen_h = SDL_max(screen_h, o->y + o->height);
            }
            free(o);
        }
    }
    /* keep the physical size ratio the server started with */
    const int mm_w = x11.screen->width_in_pixels
                         ? (int)((Sint64)screen_w * x11.screen->width_in_millimeters / x11.screen->width_in_pixels)
                         : 0;
    const int mm_h = x11.screen->height_in_pixels
                         ? (int)((Sint64)screen_h * x11.screen->height_in_millimeters / x11.screen->height_in_pixels)
                         : 0;
    const xcb_randr_mode_t was = ci->mode;

    X11_xcb_grab_server(x11.conn);
    xcb_randr_set_crtc_config_reply_t *r = X11_xcb_randr_set_crtc_config_reply(
        x11.conn,
        X11_xcb_randr_set_crtc_config(x11.conn, crtc, XCB_CURRENT_TIME, res->config_timestamp, 0, 0, XCB_NONE,
                                      ci->rotation, 0, NULL),
        NULL);
    const bool disabled = r && r->status == XCB_RANDR_SET_CONFIG_SUCCESS;
    xcb_timestamp_t cfg = res->config_timestamp;
    free(r);
    if (disabled) {
        /* BadMatch/BadValue here are known and harmless (stock SDL #4561, #4840) */
        free(X11_xcb_request_check(x11.conn, X11_xcb_randr_set_screen_size_checked(
                                                 x11.conn, x11.root, (uint16_t)screen_w, (uint16_t)screen_h,
                                                 (uint32_t)SDL_max(mm_w, 1), (uint32_t)SDL_max(mm_h, 1))));
        xcb_randr_output_t out = mon->output;
        r = X11_xcb_randr_set_crtc_config_reply(
            x11.conn,
            X11_xcb_randr_set_crtc_config(x11.conn, crtc, XCB_CURRENT_TIME, cfg, ci->x, ci->y, want, ci->rotation,
                                          1, &out),
            NULL);
        ok = r && r->status == XCB_RANDR_SET_CONFIG_SUCCESS;
        if (!ok) {
            /* light the crtc up again the way it was */
            free(r);
            r = X11_xcb_randr_set_crtc_config_reply(
                x11.conn,
                X11_xcb_randr_set_crtc_config(x11.conn, crtc, XCB_CURRENT_TIME, cfg, ci->x, ci->y, was,
                                              ci->rotation, 1, &out),
                NULL);
        }
        free(r);
    }
    X11_xcb_ungrab_server(x11.conn);
    x11_flush();
    if (!ok) {
        SDL_SetError("XRandR refused the mode switch");
        goto done;
    }
    if (to_desktop) {
        *save = x11.mode_saves[--x11.num_mode_saves];
    } else if (!save && x11.num_mode_saves < (int)SDL_arraysize(x11.mode_saves)) {
        x11.mode_saves[x11.num_mode_saves].display_id = id;
        x11.mode_saves[x11.num_mode_saves].desktop_mode = was;
        x11.num_mode_saves++;
    }

done:
    free(ci);
    free(oi);
    free(res);
    if (ok) {
        /* bounds follow now (fullscreen windows are fitted to them), not
         * when the RandR notify comes round */
        x11_update_displays(true);
        x11.displays_dirty = false;
    }
    return ok;
}

static void x11_init_randr(void)
{
    if (!SDLOP_X11_HAVE(RANDR)) {
        return;
    }
    const xcb_query_extension_reply_t *ext = X11_xcb_get_extension_data(x11.conn, X11_xcb_randr_id);
    if (!ext || !ext->present) {
        return;
    }
    xcb_randr_query_version_reply_t *v =
        X11_xcb_randr_query_version_reply(x11.conn, X11_xcb_randr_query_version(x11.conn, 1, 5), NULL);
    if (!v) {
        return;
    }
    x11.has_randr = true;
    x11.randr_event_base = ext->first_event;
    x11.randr_monitors = v->major_version > 1 || (v->major_version == 1 && v->minor_version >= 5);
    free(v);
    X11_xcb_randr_select_input(x11.conn, x11.root,
                               XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
                                   XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE);
}

/* ------------------------------------------------------------------ */
/* init / quit                                                         */
/* ------------------------------------------------------------------ */

static Uint32 x11_device_next_id(void)
{
    static Uint32 next_id = 0;
    return ++next_id;
}

/* the visual compositors treat as translucent: depth 32, TrueColor, the
 * remaining 8 bits alpha; needs a 32 bpp pixmap format for depth 32 */
static void find_argb_visual(bool order_ok)
{
    x11.argb_visual = 0;
    const xcb_setup_t *setup = X11_xcb_get_setup(x11.conn);
    const xcb_format_t *fmts = X11_xcb_setup_pixmap_formats(setup);
    bool bpp32 = false;
    for (int i = 0; i < X11_xcb_setup_pixmap_formats_length(setup); i++) {
        bpp32 = bpp32 || (fmts[i].depth == 32 && fmts[i].bits_per_pixel == 32);
    }
    if (!bpp32 || !order_ok) {
        return;
    }
    xcb_depth_iterator_t di = X11_xcb_screen_allowed_depths_iterator(x11.screen);
    for (; di.rem && !x11.argb_visual; X11_xcb_depth_next(&di)) {
        if (di.data->depth != 32) {
            continue;
        }
        xcb_visualtype_iterator_t vi = X11_xcb_depth_visuals_iterator(di.data);
        for (; vi.rem; X11_xcb_visualtype_next(&vi)) {
            if (vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR && vi.data->red_mask == 0xff0000 &&
                vi.data->green_mask == 0x00ff00 && vi.data->blue_mask == 0x0000ff) {
                x11.argb_visual = vi.data->visual_id;
                break;
            }
        }
    }
}

static bool find_visual_format(void)
{
    x11.visual = x11.screen->root_visual;
    x11.depth = x11.screen->root_depth;
    xcb_depth_iterator_t di = X11_xcb_screen_allowed_depths_iterator(x11.screen);
    for (; di.rem; X11_xcb_depth_next(&di)) {
        xcb_visualtype_iterator_t vi = X11_xcb_depth_visuals_iterator(di.data);
        for (; vi.rem; X11_xcb_visualtype_next(&vi)) {
            if (vi.data->visual_id != x11.visual) {
                continue;
            }
            bool truecolor = vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR;
            bool xrgb = vi.data->red_mask == 0xff0000 && vi.data->green_mask == 0x00ff00 &&
                        vi.data->blue_mask == 0x0000ff;
            bool bpp32 = false;
            const xcb_setup_t *setup = X11_xcb_get_setup(x11.conn);
            xcb_format_t *fmts = X11_xcb_setup_pixmap_formats(setup);
            int nf = X11_xcb_setup_pixmap_formats_length(setup);
            for (int i = 0; i < nf; i++) {
                if (fmts[i].depth == x11.depth && fmts[i].bits_per_pixel == 32) {
                    bpp32 = true;
                }
            }
            const uint16_t one = 1;
            bool little = *(const uint8_t *)&one == 1;
            bool order_ok = (setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST) == little;
            x11.fb_format_ok = truecolor && xrgb && bpp32 && order_ok && (x11.depth == 24 || x11.depth == 32);
            find_argb_visual(order_ok);
            return true;
        }
    }
    return false;
}

static bool x11_intern_atoms(void)
{
    static const char *const names[] = {
#define X11_ATOM_NAME(name) #name,
        X11_ATOM_LIST(X11_ATOM_NAME)
#undef X11_ATOM_NAME
    };
    enum { N = (int)SDL_arraysize(names) };
    xcb_intern_atom_cookie_t cookies[N];
    for (int i = 0; i < N; i++) {
        cookies[i] = X11_xcb_intern_atom(x11.conn, 0, (uint16_t)strlen(names[i]), names[i]);
    }
    xcb_atom_t *dst = (xcb_atom_t *)&x11.atom;
    bool ok = true;
    for (int i = 0; i < N; i++) {
        xcb_intern_atom_reply_t *r = X11_xcb_intern_atom_reply(x11.conn, cookies[i], NULL);
        if (r) {
            dst[i] = r->atom;
            free(r);
        } else {
            ok = false;
        }
    }
    return ok;
}

static void x11_Quit(SDLop_VideoDevice *device);

static bool x11_Init(SDLop_VideoDevice *device)
{
    memset(&x11, 0, sizeof(x11));
    const char *display_name = getenv("DISPLAY");
    if ((!display_name || !*display_name) && !SDL_GetHint(SDL_HINT_VIDEO_DRIVER)) {
        /* probing: no X session */
        return SDL_SetError("DISPLAY not set");
    }
    if (!SDLOP_X11_LoadSymbols()) {
        return false;
    }
    x11.conn = X11_xcb_connect(NULL, &x11.screen_num);
    if (!x11.conn || X11_xcb_connection_has_error(x11.conn)) {
        if (x11.conn) {
            X11_xcb_disconnect(x11.conn);
            x11.conn = NULL;
        }
        SDLOP_X11_UnloadSymbols();
        return SDL_SetError("Could not connect to X display %s", display_name ? display_name : "(null)");
    }
    const xcb_setup_t *setup = X11_xcb_get_setup(x11.conn);
    xcb_screen_iterator_t si = X11_xcb_setup_roots_iterator(setup);
    for (int i = 0; si.rem && i < x11.screen_num; i++) {
        X11_xcb_screen_next(&si);
    }
    if (!si.rem) {
        x11_Quit(device);
        return SDL_SetError("X screen %d does not exist", x11.screen_num);
    }
    x11.screen = si.data;
    x11.root = x11.screen->root;
    find_visual_format();
    x11.max_request_bytes = X11_xcb_get_maximum_request_length(x11.conn) * 4u;
    if (x11.max_request_bytes < 4096) {
        x11.max_request_bytes = 4096;
    }
    if (!x11_intern_atoms()) {
        x11_Quit(device);
        return SDL_SetError("Could not intern X11 atoms");
    }
    /* ICC Profiles in X Specification: the root's _ICC_PROFILE (screen 0)
     * or _ICC_PROFILE_<n> */
    {
        char icc[32];
        if (x11.screen_num > 0) {
            snprintf(icc, sizeof(icc), "_ICC_PROFILE_%d", x11.screen_num);
        } else {
            snprintf(icc, sizeof(icc), "_ICC_PROFILE");
        }
        xcb_intern_atom_reply_t *r = X11_xcb_intern_atom_reply(
            x11.conn, X11_xcb_intern_atom(x11.conn, 0, (uint16_t)strlen(icc), icc), NULL);
        x11.icc_atom = r ? r->atom : XCB_NONE;
        free(r);
    }

    /* MIT-SHM (env SDLOP_X11_NO_SHM=1 or SDLOP_LIB_XCB_SHM=none disables) */
    if (SDLOP_X11_HAVE(SHM) && !SDLOP_HintFlag("SDLOP_X11_NO_SHM", false)) {
        const xcb_query_extension_reply_t *ext = X11_xcb_get_extension_data(x11.conn, X11_xcb_shm_id);
        if (ext && ext->present) {
            xcb_shm_query_version_reply_t *v =
                X11_xcb_shm_query_version_reply(x11.conn, X11_xcb_shm_query_version(x11.conn), NULL);
            x11.has_shm = v != NULL;
            free(v);
        }
    }

    /* SYNC >= 3.0 for _NET_WM_SYNC_REQUEST counters */
    if (SDLOP_X11_HAVE(SYNC)) {
        const xcb_query_extension_reply_t *ext = X11_xcb_get_extension_data(x11.conn, X11_xcb_sync_id);
        if (ext && ext->present) {
            xcb_sync_initialize_reply_t *v =
                X11_xcb_sync_initialize_reply(x11.conn, X11_xcb_sync_initialize(x11.conn, 3, 1), NULL);
            x11.has_sync = v && v->major_version >= 3;
            free(v);
        }
    }

    /* SHAPE >= 1.1 for input shapes (SDL_SetWindowShape, tooltips) */
    if (SDLOP_X11_HAVE(SHAPE)) {
        const xcb_query_extension_reply_t *ext = X11_xcb_get_extension_data(x11.conn, X11_xcb_shape_id);
        if (ext && ext->present) {
            xcb_shape_query_version_reply_t *v =
                X11_xcb_shape_query_version_reply(x11.conn, X11_xcb_shape_query_version(x11.conn), NULL);
            x11.has_shape = v && (v->major_version > 1 || (v->major_version == 1 && v->minor_version >= 1));
            free(v);
        }
    }

    /* XInput2 >= 2.2 for raw motion (relative mouse), touch and pens;
     * 2.4 adds touchpad gestures (the server answers with what it has) */
    if (SDLOP_X11_HAVE(XINPUT)) {
        const xcb_query_extension_reply_t *ext = X11_xcb_get_extension_data(x11.conn, X11_xcb_input_id);
        if (ext && ext->present) {
            xcb_input_xi_query_version_reply_t *v = X11_xcb_input_xi_query_version_reply(
                x11.conn, X11_xcb_input_xi_query_version(x11.conn, 2, 4), NULL);
            if (v && (v->major_version > 2 || (v->major_version == 2 && v->minor_version >= 2))) {
                x11.has_xi2 = true;
                x11.has_xi24 = v->major_version > 2 || v->minor_version >= 4;
                x11.xi_opcode = ext->major_opcode;
            }
            free(v);
        }
    }

    if (x11.has_xi2) {
        SDLOP_X11_Pen_Init(x11.xi_opcode);
    }
    x11_init_xkb();
    x11_load_keymap();
    x11_sync_repeat_info();
    x11_init_randr();
    /* root: RESOURCE_MANAGER changes (Xft.dpi) and XSETTINGS MANAGER
     * announcements (sent with StructureNotify) */
    uint32_t root_mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    X11_xcb_change_window_attributes(x11.conn, x11.root, XCB_CW_EVENT_MASK, &root_mask);
    SDLOP_X11_XSettings_Init();
    x11_update_displays(false);

    /* invisible cursor for relative mode: 1x1 empty bitmap */
    xcb_pixmap_t pm = X11_xcb_generate_id(x11.conn);
    X11_xcb_create_pixmap(x11.conn, 1, pm, x11.root, 1, 1);
    x11.blank_cursor = X11_xcb_generate_id(x11.conn);
    X11_xcb_create_cursor(x11.conn, x11.blank_cursor, pm, pm, 0, 0, 0, 0, 0, 0, 0, 0);
    X11_xcb_free_pixmap(x11.conn, pm);

    x11.keyboard_id = x11_device_next_id();
    SDLOP_AddKeyboard(x11.keyboard_id, NULL);
    x11.mouse_id = x11_device_next_id();
    SDLOP_AddMouse(x11.mouse_id, NULL);

    SDLOP_X11_Clipboard_Init();
    SDLOP_X11_DnD_Init();
    /* pointer barriers for SDL_SetWindowMouseRect: XFixes 5 */
    x11.has_barriers = false;
    if (SDLOP_X11_HAVE(XFIXES)) {
        const xcb_query_extension_reply_t *ext = X11_xcb_get_extension_data(x11.conn, X11_xcb_xfixes_id);
        if (ext && ext->present) {
            xcb_xfixes_query_version_reply_t *v = X11_xcb_xfixes_query_version_reply(
                x11.conn, X11_xcb_xfixes_query_version(x11.conn, 5, 0), NULL);
            x11.has_barriers = v && v->major_version >= 5;
            free(v);
        }
    }

    x11_flush();
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "SDLop x11: %s, %d display(s), shm=%d xkb=%d%s xi2=%d randr=%d%s scale=%g",
                display_name ? display_name : "(default)", x11.num_monitors, x11.has_shm, x11.has_xkb,
                x11.detectable_repeat ? "(detectable-repeat)" : "", x11.has_xi2, x11.has_randr,
                x11.randr_monitors ? "(1.5)" : "", x11.content_scale);
    return true;
}

static void x11_Quit(SDLop_VideoDevice *device)
{
    (void)device;
    destroy_barriers();
    SDLOP_X11_GL_Quit(); /* also releases libEGL */
    for (int i = 0; i < x11.num_monitors; i++) {
        SDLOP_DelDisplay(x11.monitors[i].display_id, false);
    }
    x11.num_monitors = 0;
    SDLOP_X11_Clipboard_Quit();
    SDLOP_X11_DnD_Quit();
    SDLOP_X11_XSettings_Quit();
    SDLOP_X11_XIM_Quit();
    SDLOP_X11_Pen_Quit();
    free(x11.peeked);
    x11.peeked = NULL;
    if (x11.conn) {
        if (x11.blank_cursor) {
            X11_xcb_free_cursor(x11.conn, x11.blank_cursor);
        }
        for (size_t i = 0; i < SDL_arraysize(x11.hit_test_cursors); i++) {
            if (x11.hit_test_cursors[i]) {
                X11_xcb_free_cursor(x11.conn, x11.hit_test_cursors[i]);
            }
        }
        if (x11.cursor_theme) {
            sdlop_xcursor_theme_destroy(x11.cursor_theme);
        }
        X11_xcb_flush(x11.conn);
        X11_xcb_disconnect(x11.conn);
    }
    memset(&x11, 0, sizeof(x11));
    SDLOP_X11_UnloadSymbols();
}

/* ------------------------------------------------------------------ */
/* windows                                                             */
/* ------------------------------------------------------------------ */

static bool x11_CreateWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)calloc(1, sizeof(*wd));
    if (!wd) {
        return SDL_OutOfMemory();
    }
    window->driverdata = wd;
    window->display_scale = x11.content_scale;

    wd->xid = X11_xcb_generate_id(x11.conn);
    wd->parent = x11.root;
    uint32_t clear = ((uint32_t)window->clear_r << 16) | ((uint32_t)window->clear_g << 8) | window->clear_b;
    bool gpu = (window->flags & (SDL_WINDOW_OPENGL | SDL_WINDOW_VULKAN)) != 0;
    /* OpenGL windows take the X visual of the GLX framebuffer config that
     * matches the SDL_GL_* attributes, as in stock SDL3 (X11_GL_GetVisual):
     * the root visual is bound to one fixed config (typically RGBA8888 with
     * depth 24 / stencil 8), so keeping it would override the request. */
    wd->visual = x11.visual;
    wd->depth = x11.depth;
    const bool transparent = (window->flags & SDL_WINDOW_TRANSPARENT) && x11.argb_visual;
    if (window->flags & SDL_WINDOW_OPENGL) {
        xcb_visualid_t gl_visual = SDLOP_X11_GL_ChooseWindowVisual(transparent);
        if (gl_visual && (SDLOP_X11_IsCompatibleVisual(gl_visual) ||
                          (transparent && SDLOP_X11_IsARGBVisual(gl_visual)))) {
            wd->visual = gl_visual;
        }
    }
    /* SDL_WINDOW_TRANSPARENT: an ARGB visual (software surfaces too, and
     * EGL, whose config follows the window's visual) */
    if (transparent && (wd->visual == x11.visual || SDLOP_X11_IsARGBVisual(wd->visual))) {
        if (wd->visual == x11.visual) {
            wd->visual = x11.argb_visual;
        }
        wd->depth = 32;
    }
    /* a visual other than the parent's needs its own colormap (BadMatch) */
    if (wd->visual != x11.visual) {
        wd->colormap = X11_xcb_generate_id(x11.conn);
        X11_xcb_create_colormap(x11.conn, XCB_COLORMAP_ALLOC_NONE, wd->colormap, x11.root, wd->visual);
    }
    /* GPU windows get no background so resizes don't flash */
    uint32_t mask = (gpu ? XCB_CW_BACK_PIXMAP : XCB_CW_BACK_PIXEL) | XCB_CW_BORDER_PIXEL |
                    XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | (wd->colormap ? XCB_CW_COLORMAP : 0);
    uint32_t values[5] = {
        gpu ? XCB_BACK_PIXMAP_NONE : (wd->depth == 32 ? 0 : clear),
        0,
        XCB_GRAVITY_NORTH_WEST,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS |
            XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE,
        wd->colormap, /* value order follows the mask bits: COLORMAP is last */
    };
    xcb_generic_error_t *err = X11_xcb_request_check(
        x11.conn, X11_xcb_create_window_checked(x11.conn, wd->depth, wd->xid, x11.root,
                                                (int16_t)window->x, (int16_t)window->y, (uint16_t)window->w,
                                                (uint16_t)window->h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                wd->visual, mask, values));
    if (err) {
        int code = err->error_code;
        free(err);
        if (wd->colormap) {
            X11_xcb_free_colormap(x11.conn, wd->colormap);
        }
        free(wd);
        window->driverdata = NULL;
        return SDL_SetError("xcb_create_window failed (X error %d)", code);
    }

    /* ICCCM / EWMH bookkeeping (swa: WM_PROTOCOLS + _NET_WM_PID) */
    xcb_atom_t protocols[3] = { x11.atom.WM_DELETE_WINDOW, x11.atom._NET_WM_PING, XCB_NONE };
    uint32_t nprotocols = 2;
    /* resize sync (opt-in as in stock; Vulkan presents bypass SDL) */
    if (x11.has_sync && !(window->flags & SDL_WINDOW_VULKAN) &&
        SDL_GetHintBoolean(SDL_HINT_VIDEO_X11_ENABLE_XSYNC_EXT, false)) {
        xcb_sync_int64_t zero = { 0, 0 };
        wd->sync_counter = X11_xcb_generate_id(x11.conn);
        X11_xcb_sync_create_counter(x11.conn, wd->sync_counter, zero);
        set_property(wd->xid, x11.atom._NET_WM_SYNC_REQUEST_COUNTER, XCB_ATOM_CARDINAL, 32, 1, &wd->sync_counter);
        protocols[nprotocols++] = x11.atom._NET_WM_SYNC_REQUEST;
    }
    set_property(wd->xid, x11.atom.WM_PROTOCOLS, XCB_ATOM_ATOM, 32, nprotocols, protocols);
    uint32_t pid = (uint32_t)getpid();
    set_property(wd->xid, x11.atom._NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);
    char host[256];
    if (gethostname(host, sizeof(host)) == 0) {
        host[sizeof(host) - 1] = '\0';
        set_property(wd->xid, x11.atom.WM_CLIENT_MACHINE, XCB_ATOM_STRING, 8, (uint32_t)strlen(host), host);
    }
    /* WM_CLASS = instance\0class\0 (SDL3: exe name, app id) */
    const char *app = SDLOP_GetAppID();
    const char *inst = SDLOP_GetExeName();
    if (!inst) {
        inst = app;
    }
    char wm_class[256];
    int cl = snprintf(wm_class, sizeof(wm_class) - 1, "%s%c%s", inst, '\0', app);
    if (cl > 0 && cl < (int)sizeof(wm_class) - 1) {
        set_property(wd->xid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, (uint32_t)cl + 1, wm_class);
    }
    set_wm_hints(window);
    xcb_atom_t type = x11.atom._NET_WM_WINDOW_TYPE_NORMAL;
    if (window->flags & SDL_WINDOW_TOOLTIP) {
        type = x11.atom._NET_WM_WINDOW_TYPE_TOOLTIP;
    } else if (window->flags & SDL_WINDOW_POPUP_MENU) {
        type = x11.atom._NET_WM_WINDOW_TYPE_POPUP_MENU;
    } else if (window->flags & SDL_WINDOW_UTILITY) {
        type = x11.atom._NET_WM_WINDOW_TYPE_UTILITY;
    } else if (window->flags & SDL_WINDOW_MODAL) {
        type = x11.atom._NET_WM_WINDOW_TYPE_DIALOG;
    }
    set_property(wd->xid, x11.atom._NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 32, 1, &type);
    if ((window->flags & SDL_WINDOW_TOOLTIP) && x11.has_shape) {
        /* tooltips do not receive input (stock): empty input shape */
        X11_xcb_shape_rectangles(x11.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED, wd->xid,
                                 0, 0, 0, NULL);
    }
    if (SDLOP_WINDOW_IS_POPUP(window)) {
        /* SDL3 X11: popups bypass the WM so they land exactly at the
         * requested offset and never steal the focus */
        uint32_t on = 1;
        X11_xcb_change_window_attributes(x11.conn, wd->xid, XCB_CW_OVERRIDE_REDIRECT, &on);
    }
    if (window->parent && wdata(window->parent)) {
        set_property(wd->xid, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &wdata(window->parent)->xid);
    }
    if (x11.current_cursor) {
        X11_xcb_change_window_attributes(x11.conn, wd->xid, XCB_CW_CURSOR, &x11.current_cursor);
    }
    /* SDL3 window properties (no Xlib Display here: xcb connection only) */
    if (!window->props) {
        window->props = SDL_CreateProperties();
    }
    SDL_SetNumberProperty(window->props, SDL_PROP_WINDOW_X11_SCREEN_NUMBER, x11.screen_num);
    SDL_SetNumberProperty(window->props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, (Sint64)wd->xid);
    set_size_hints(window, size_locked(window), false);
    set_title(window);
    if (window->flags & SDL_WINDOW_BORDERLESS) {
        set_motif_decorations(window, false);
    }
    write_initial_net_wm_state(window);

    wd->gc = X11_xcb_generate_id(x11.conn);
    uint32_t gcv = 0;
    X11_xcb_create_gc(x11.conn, wd->gc, wd->xid, XCB_GC_GRAPHICS_EXPOSURES, &gcv);
    select_touch(wd->xid);
    if (x11.has_xi2) {
        SDLOP_X11_Pen_WindowCreated(wd->xid);
    }

    wd->cfg_x = window->x;
    wd->cfg_y = window->y;
    wd->cfg_w = window->w;
    wd->cfg_h = window->h;
    x11_flush();
    return true;
}

/* run the event loop until pred() holds or timeout_ms passes */
void SDLOP_X11_PumpUntil(bool (*pred)(void *), void *userdata, int timeout_ms)
{
    Uint64 deadline = SDLOP_MonotonicNS() + (Uint64)timeout_ms * 1000000ull;
    x11_flush();
    while (!pred(userdata) && !x11.dead) {
        Uint64 now = SDLOP_MonotonicNS();
        if (now >= deadline) {
            break;
        }
        xcb_generic_event_t *ev = x11.peeked;
        x11.peeked = NULL;
        if (!ev) {
            ev = X11_xcb_poll_for_event(x11.conn);
        }
        if (ev) {
            x11_dispatch(ev);
            free(ev);
            continue;
        }
        if (X11_xcb_connection_has_error(x11.conn)) {
            break;
        }
        struct pollfd pfd = { X11_xcb_get_file_descriptor(x11.conn), POLLIN, 0 };
        int ms = (int)((deadline - now) / 1000000ull) + 1;
        poll(&pfd, 1, ms);
    }
}

typedef struct WindowPred
{
    SDL_Window *window;
    bool (*pred)(SDL_Window *);
} WindowPred;

static bool window_pred(void *userdata)
{
    WindowPred *wp = (WindowPred *)userdata;
    return wp->pred(wp->window);
}

static void pump_until(SDL_Window *window, bool (*pred)(SDL_Window *), int timeout_ms)
{
    WindowPred wp = { window, pred };
    SDLOP_X11_PumpUntil(window_pred, &wp, timeout_ms);
}

static bool is_mapped(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    return !wd || wd->mapped;
}

static bool is_unmapped(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    return !wd || !wd->mapped;
}

static void legacy_set_fullscreen(SDL_Window *window, bool on);
static void legacy_set_maximized(SDL_Window *window, bool on);

static void x11_ShowWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return;
    }
    set_size_hints(window, size_locked(window), true);
    write_initial_net_wm_state(window);
    X11_xcb_map_window(x11.conn, wd->xid);
    /* SDL3 waits for MapNotify so focus/grab requests that follow work */
    pump_until(window, is_mapped, 1000);
    if (!wm_running()) {
        if ((window->flags & SDL_WINDOW_MAXIMIZED) && !wd->legacy_maximized) {
            legacy_set_maximized(window, true);
        }
        if ((window->flags & SDL_WINDOW_FULLSCREEN) && !wd->legacy_fullscreen) {
            legacy_set_fullscreen(window, true);
        }
    }
    if (!SDLOP_WINDOW_IS_POPUP(window) && !(window->flags & SDL_WINDOW_NOT_FOCUSABLE)) {
        activate_window(window);
    }
    if (window->surface && wd->pixels) {
        fb_present(window, NULL, 0);
    }
    x11_flush();
}

static void x11_HideWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return;
    }
    if (x11.relative_window == window && x11.pointer_grabbed) {
        X11_xcb_ungrab_pointer(x11.conn, XCB_CURRENT_TIME);
        x11.pointer_grabbed = false;
        wd->want_grab = true;
    }
    X11_xcb_unmap_window(x11.conn, wd->xid);
    /* ICCCM 4.1.4: withdraw with a synthetic UnmapNotify to the root, so
     * the WM also forgets iconified windows */
    X11Event32 ev;
    memset(&ev, 0, sizeof(ev));
    ev.unmap.response_type = XCB_UNMAP_NOTIFY;
    ev.unmap.event = x11.root;
    ev.unmap.window = wd->xid;
    send_to_root(&ev);
    if (wd->mapped) {
        pump_until(window, is_unmapped, 1000);
    }
    x11_flush();
}

static void x11_DestroyWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return;
    }
    SDLOP_X11_DnD_WindowDestroyed(window);
    SDLOP_X11_XIM_WindowDestroyed(window);
    if (x11.barrier_window == window) {
        destroy_barriers(); /* they are tied to the window anyway */
    }
    if (x11.relative_window == window) {
        if (x11.pointer_grabbed) {
            X11_xcb_ungrab_pointer(x11.conn, XCB_CURRENT_TIME);
            x11.pointer_grabbed = false;
        }
        if (x11.has_xi2 && !x11.warp_relative) {
            select_raw_motion(false);
        }
        x11.relative_window = NULL;
    }
    if (x11.pointer_focus == window) {
        x11.pointer_focus = NULL;
    }
    if (x11.confine_window == window || x11.capture_window == window) {
        X11_xcb_ungrab_pointer(x11.conn, XCB_CURRENT_TIME);
        x11.confine_window = x11.capture_window = NULL;
    }
    if (x11.keyboard_grab_window == window) {
        X11_xcb_ungrab_keyboard(x11.conn, XCB_CURRENT_TIME);
        x11.keyboard_grab_window = NULL;
    }
    cancel_touches(window);
    if (sdlop.mouse_focus == window) {
        sdlop.mouse_focus = NULL;
    }
    if (sdlop.keyboard_focus == window) {
        sdlop.keyboard_focus = NULL;
    }
    SDLOP_X11_GL_WindowDestroyed(window);
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
    fb_free(wd);
    X11_xcb_free_gc(x11.conn, wd->gc);
    if (wd->sync_counter) {
        X11_xcb_sync_destroy_counter(x11.conn, wd->sync_counter);
    }
    X11_xcb_destroy_window(x11.conn, wd->xid);
    if (wd->colormap) {
        X11_xcb_free_colormap(x11.conn, wd->colormap);
    }
    x11_flush();
    free(wd);
    window->driverdata = NULL;
}

static bool x11_SetWindowTitle(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (wdata(window)) {
        set_title(window);
        x11_flush();
    }
    return true;
}

static bool x11_SetWindowSize(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    set_size_hints(window, size_locked(window), false);
    uint32_t v[2] = { (uint32_t)window->w, (uint32_t)window->h };
    X11_xcb_configure_window(x11.conn, wd->xid, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
    x11_flush();
    /* RESIZED follows from the ConfigureNotify (the WM may adjust) */
    return true;
}

static bool x11_SetWindowPosition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    set_size_hints(window, size_locked(window), true);
    int32_t v[2] = { window->x, window->y };
    X11_xcb_configure_window(x11.conn, wd->xid, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
    x11_flush();
    return true;
}

static void move_resize(SDL_Window *window, const SDL_Rect *r, bool raise)
{
    X11WindowData *wd = wdata(window);
    uint32_t v[5] = { (uint32_t)r->x, (uint32_t)r->y, (uint32_t)r->w, (uint32_t)r->h, XCB_STACK_MODE_ABOVE };
    uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    if (raise) {
        mask |= XCB_CONFIG_WINDOW_STACK_MODE;
    }
    X11_xcb_configure_window(x11.conn, wd->xid, mask, v);
}

static SDL_Rect current_rect(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    SDL_Rect r = { wd->cfg_x, wd->cfg_y, wd->cfg_w, wd->cfg_h };
    return r;
}

static bool display_rect(SDL_DisplayID id, SDL_Rect *out)
{
    if (id && SDLOP_GetDisplayBoundsById(id, out)) {
        return true;
    }
    return SDLOP_GetDisplayBoundsById(SDL_GetPrimaryDisplay(), out);
}

static SDL_DisplayID fullscreen_target(SDL_Window *window)
{
    if (window->has_fullscreen_mode && window->fullscreen_mode.displayID) {
        return window->fullscreen_mode.displayID;
    }
    SDL_Rect r = current_rect(window);
    SDL_DisplayID id = SDL_GetDisplayForRect(&r);
    return id ? id : SDL_GetPrimaryDisplay();
}

/* SDL3 X11_SetWindowFullscreenLegacy: no WM, so do the WM's job */
static void legacy_set_fullscreen(SDL_Window *window, bool on)
{
    X11WindowData *wd = wdata(window);
    if (on) {
        SDL_Rect target;
        if (!display_rect(fullscreen_target(window), &target)) {
            return;
        }
        if (!wd->legacy_fullscreen && !wd->legacy_maximized) {
            wd->saved_rect = current_rect(window);
        }
        wd->legacy_fullscreen = true;
        set_size_hints(window, false, true);
        move_resize(window, &target, true);
        report_fullscreen(window, true);
    } else {
        if (!wd->legacy_fullscreen) {
            return;
        }
        wd->legacy_fullscreen = false;
        if (wd->legacy_maximized) {
            SDL_Rect target;
            if (display_rect(fullscreen_target(window), &target)) {
                move_resize(window, &target, false);
            }
        } else {
            window->w = wd->saved_rect.w;
            window->h = wd->saved_rect.h;
            set_size_hints(window, size_locked(window), true);
            move_resize(window, &wd->saved_rect, false);
        }
        report_fullscreen(window, false);
    }
    x11_flush();
}

static void x11_MinimizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return;
    }
    /* ICCCM 4.1.4: XIconifyWindow == WM_CHANGE_STATE(IconicState) */
    send_client_message(wd->xid, x11.atom.WM_CHANGE_STATE, X11_STATE_ICONIC, 0, 0, 0, 0);
    x11_flush();
    /* flagged right away like the Wayland driver; the WM confirms via
     * _NET_WM_STATE_HIDDEN / WM_STATE (deduplicated) */
    report_minimized(window, true);
}

/* no WM: fill the display like a WM would (SDL3 has no such emulation;
 * without it SDL_MaximizeWindow would be a silent no-op on bare X) */
static void legacy_set_maximized(SDL_Window *window, bool on)
{
    X11WindowData *wd = wdata(window);
    if (on == wd->legacy_maximized) {
        return;
    }
    if (on) {
        SDL_Rect target;
        if (!display_rect(fullscreen_target(window), &target)) {
            return;
        }
        if (!wd->legacy_fullscreen) {
            wd->saved_rect = current_rect(window);
            wd->legacy_maximized = true;
            set_size_hints(window, false, true);
            move_resize(window, &target, false);
        }
        wd->legacy_maximized = true;
    } else {
        wd->legacy_maximized = false;
        if (!wd->legacy_fullscreen) {
            window->w = wd->saved_rect.w;
            window->h = wd->saved_rect.h;
            set_size_hints(window, size_locked(window), true);
            move_resize(window, &wd->saved_rect, false);
        }
    }
    report_maximized(window, on);
    x11_flush();
}

static void x11_MaximizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return;
    }
    if (!wd->mapped) {
        /* applied when the window is shown */
        window->flags |= SDL_WINDOW_MAXIMIZED;
        write_initial_net_wm_state(window);
    } else if (!wd->legacy_maximized && wm_supports(x11.atom._NET_WM_STATE_MAXIMIZED_VERT)) {
        /* flags follow the WM's _NET_WM_STATE update */
        request_net_wm_state(window, true, x11.atom._NET_WM_STATE_MAXIMIZED_VERT,
                             x11.atom._NET_WM_STATE_MAXIMIZED_HORZ);
    } else {
        legacy_set_maximized(window, true);
    }
    x11_flush();
}

static void x11_RestoreWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return;
    }
    if (window->flags & SDL_WINDOW_MINIMIZED) {
        /* ICCCM: mapping an iconic window de-iconifies it */
        if (!(window->flags & SDL_WINDOW_HIDDEN)) {
            X11_xcb_map_window(x11.conn, wd->xid);
            activate_window(window);
        }
        report_minimized(window, false);
        x11_flush();
        return;
    }
    if (wd->legacy_maximized) {
        legacy_set_maximized(window, false);
    } else if (!wd->mapped) {
        window->flags &= ~SDL_WINDOW_MAXIMIZED;
        write_initial_net_wm_state(window);
    } else if (wd->rep_maximized || (window->flags & SDL_WINDOW_MAXIMIZED)) {
        request_net_wm_state(window, false, x11.atom._NET_WM_STATE_MAXIMIZED_VERT,
                             x11.atom._NET_WM_STATE_MAXIMIZED_HORZ);
    }
    x11_flush();
}

static bool x11_SetWindowFullscreen(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    bool on = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
    if (!wd->mapped) {
        /* applied at map time (property for the WM, legacy otherwise) */
        write_initial_net_wm_state(window);
        if (!on && wd->legacy_fullscreen) {
            legacy_set_fullscreen(window, false);
        }
        x11_flush();
        return true;
    }
    if (wd->legacy_fullscreen || !wm_supports(x11.atom._NET_WM_STATE_FULLSCREEN)) {
        legacy_set_fullscreen(window, on);
        return true;
    }
    /* Compiz & co refuse to fullscreen fixed-size windows (SDL3 note):
     * lift the min/max lock while fullscreen */
    set_size_hints(window, size_locked(window), false);
    if (on) {
        /* the WM fullscreens on the monitor the window is on: move first */
        SDL_Rect target, cur = current_rect(window);
        SDL_DisplayID want = fullscreen_target(window);
        if (display_rect(want, &target) && SDL_GetDisplayForRect(&cur) != want) {
            int32_t v[2] = { target.x, target.y };
            X11_xcb_configure_window(x11.conn, wd->xid, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
        }
    }
    request_net_wm_state(window, on, x11.atom._NET_WM_STATE_FULLSCREEN, XCB_NONE);
    x11_flush();
    return true;
}

static void x11_RaiseWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return;
    }
    uint32_t v = XCB_STACK_MODE_ABOVE;
    X11_xcb_configure_window(x11.conn, wd->xid, XCB_CONFIG_WINDOW_STACK_MODE, &v);
    activate_window(window);
    x11_flush();
}

static void x11_SetWindowClearColor(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return;
    }
    uint32_t color = ((uint32_t)window->clear_r << 16) | ((uint32_t)window->clear_g << 8) | window->clear_b;
    wd->clear_explicit = true;
    if (wd->depth == 32) {
        color |= 0xff000000u; /* ARGB visual: an explicit clear colour is opaque */
    }
    if (!(window->flags & (SDL_WINDOW_OPENGL | SDL_WINDOW_VULKAN))) {
        X11_xcb_change_window_attributes(x11.conn, wd->xid, XCB_CW_BACK_PIXEL, &color);
    }
    if (wd->pixels) {
        fb_fill_clear(window);
        fb_present(window, NULL, 0);
    } else if (!wd->gl_active) {
        X11_xcb_clear_area(x11.conn, 0, wd->xid, 0, 0, 0, 0);
    }
    x11_flush();
}

static bool x11_SetWindowRelativeMouseMode(SDLop_VideoDevice *device, SDL_Window *window, bool enabled)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    if (enabled) {
        if (x11.relative_window && x11.relative_window != window) {
            x11_SetWindowRelativeMouseMode(device, x11.relative_window, false);
        }
        x11.relative_window = window;
        x11.warp_relative = !x11.has_xi2 || SDLOP_HintFlag("SDLOP_X11_RELATIVE_WARP", false);
        if (!x11.warp_relative) {
            select_raw_motion(true);
        }
        X11_xcb_change_window_attributes(x11.conn, wd->xid, XCB_CW_CURSOR, &x11.blank_cursor);
        x11.confine_window = x11.capture_window = NULL; /* replaced by this grab */
        grab_pointer(window); /* retried on focus/click if the WM holds a grab */
        if (x11.warp_relative) {
            warp_to_center(window);
        }
        x11_flush();
        return true;
    }
    if (x11.relative_window != window) {
        return true;
    }
    if (x11.pointer_grabbed) {
        X11_xcb_ungrab_pointer(x11.conn, XCB_CURRENT_TIME);
        x11.pointer_grabbed = false;
    }
    if (!x11.warp_relative) {
        select_raw_motion(false);
    }
    wd->want_grab = false;
    x11.relative_window = NULL;
    x11.warp_relative = false;
    uint32_t cur = x11.current_cursor == x11.blank_cursor ? XCB_CURSOR_NONE : x11.current_cursor;
    X11_xcb_change_window_attributes(x11.conn, wd->xid, XCB_CW_CURSOR, &cur);
    /* SDL3: put the visible cursor back where the app thinks it is */
    if (sdlop.mouse_has_position && wd->mapped) {
        X11_xcb_warp_pointer(x11.conn, XCB_WINDOW_NONE, wd->xid, 0, 0, 0, 0, (int16_t)sdlop.mouse_x,
                             (int16_t)sdlop.mouse_y);
    }
    x11_flush();
    return true;
}

/* ------------------------------------------------------------------ */
/* SDL 3.4 window management                                           */
/* ------------------------------------------------------------------ */

static void x11_UpdateSizeHints(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (!wdata(window)) {
        return;
    }
    set_size_hints(window, size_locked(window), false);
    x11_flush();
}

static void x11_SetWindowResizable(SDLop_VideoDevice *device, SDL_Window *window, bool resizable)
{
    (void)resizable; /* window->flags already updated */
    x11_UpdateSizeHints(device, window);
}

static bool x11_GetWindowBordersSize(SDLop_VideoDevice *device, SDL_Window *window, int *top, int *left, int *bottom, int *right)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    uint32_t *ext = NULL;
    /* _NET_FRAME_EXTENTS: left, right, top, bottom */
    if (get_property32(wd->xid, x11.atom._NET_FRAME_EXTENTS, XCB_ATOM_CARDINAL, 4, &ext) == 4) {
        *left = (int)ext[0];
        *right = (int)ext[1];
        *top = (int)ext[2];
        *bottom = (int)ext[3];
    }
    free(ext);
    return true;
}

static bool x11_SetWindowOpacity(SDLop_VideoDevice *device, SDL_Window *window, float opacity)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (opacity >= 1.0f) {
        X11_xcb_delete_property(x11.conn, wd->xid, x11.atom._NET_WM_WINDOW_OPACITY);
    } else {
        uint32_t alpha = (uint32_t)((double)opacity * 0xFFFFFFFFu);
        set_property(wd->xid, x11.atom._NET_WM_WINDOW_OPACITY, XCB_ATOM_CARDINAL, 32, 1, &alpha);
    }
    x11_flush();
    return true;
}

static bool x11_SetWindowParent(SDLop_VideoDevice *device, SDL_Window *window, SDL_Window *parent)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    X11WindowData *pd = wdata(parent);
    if (pd) {
        set_property(wd->xid, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &pd->xid);
    } else {
        X11_xcb_delete_property(x11.conn, wd->xid, XCB_ATOM_WM_TRANSIENT_FOR);
    }
    x11_flush();
    return true;
}

static bool x11_SetWindowModal(SDLop_VideoDevice *device, SDL_Window *window, bool modal)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return true;
    }
    X11WindowData *pd = modal ? wdata(window->parent) : NULL;
    if (pd) {
        set_property(wd->xid, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &pd->xid);
    }
    request_net_wm_state(window, modal, x11.atom._NET_WM_STATE_MODAL, 0);
    x11_flush();
    return true;
}

static void x11_SetWindowBordered(SDLop_VideoDevice *device, SDL_Window *window, bool bordered)
{
    (void)device;
    set_motif_decorations(window, bordered);
    x11_flush();
}

static void x11_SetWindowAlwaysOnTop(SDLop_VideoDevice *device, SDL_Window *window, bool on_top)
{
    (void)device;
    request_net_wm_state(window, on_top, x11.atom._NET_WM_STATE_ABOVE, 0);
    x11_flush();
}

static bool x11_SetWindowIcon(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface *icon)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    /* _NET_WM_ICON: width, height, then non-premultiplied ARGB rows */
    const size_t count = 2 + (size_t)icon->w * (size_t)icon->h;
    if (count * 4 + 64 > x11.max_request_bytes) {
        return SDL_SetError("Icon too large for the X server request size");
    }
    uint32_t *data = (uint32_t *)malloc(count * 4);
    if (!data) {
        return SDL_OutOfMemory();
    }
    data[0] = (uint32_t)icon->w;
    data[1] = (uint32_t)icon->h;
    for (int y = 0; y < icon->h; ++y) {
        memcpy(&data[2 + (size_t)y * (size_t)icon->w], (const Uint8 *)icon->pixels + (size_t)y * (size_t)icon->pitch,
               (size_t)icon->w * 4);
    }
    set_property(wd->xid, x11.atom._NET_WM_ICON, XCB_ATOM_CARDINAL, 32, (uint32_t)count, data);
    free(data);
    x11_flush();
    return true;
}

static bool confine_pointer(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    if (!wd->mapped) {
        return false;
    }
    xcb_grab_pointer_reply_t *r = X11_xcb_grab_pointer_reply(
        x11.conn,
        X11_xcb_grab_pointer(x11.conn, 1, wd->xid,
                             XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                 XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
                                 XCB_EVENT_MASK_LEAVE_WINDOW,
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, wd->xid, XCB_CURSOR_NONE, XCB_CURRENT_TIME),
        NULL);
    bool ok = r && r->status == XCB_GRAB_STATUS_SUCCESS;
    free(r);
    if (ok) {
        x11.confine_window = window;
        x11.capture_window = NULL;
    }
    return ok;
}

static bool x11_SetWindowMouseGrab(SDLop_VideoDevice *device, SDL_Window *window, bool grabbed)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return true;
    }
    wd->mouse_grab = grabbed && (window->flags & SDL_WINDOW_MOUSE_GRABBED);
    if (x11.relative_window == window) {
        return true; /* the relative-mode grab already confines */
    }
    if (wd->mouse_grab) {
        if (x11.confine_window != window) {
            /* SDL3 retries for a moment: the WM may still hold its grab
             * right after mapping/focusing */
            for (int attempt = 0; attempt < 10 && !confine_pointer(window); ++attempt) {
                SDL_Delay(10);
            }
        }
    } else if (x11.confine_window == window) {
        X11_xcb_ungrab_pointer(x11.conn, XCB_CURRENT_TIME);
        x11.confine_window = NULL;
    }
    x11_flush();
    return true;
}

static bool grab_keyboard(SDL_Window *window)
{
    X11WindowData *wd = wdata(window);
    if (!wd->mapped) {
        return false;
    }
    xcb_grab_keyboard_reply_t *r = X11_xcb_grab_keyboard_reply(
        x11.conn,
        X11_xcb_grab_keyboard(x11.conn, 1, wd->xid, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
        NULL);
    bool ok = r && r->status == XCB_GRAB_STATUS_SUCCESS;
    free(r);
    if (ok) {
        x11.keyboard_grab_window = window;
    }
    return ok;
}

static bool x11_SetWindowKeyboardGrab(SDLop_VideoDevice *device, SDL_Window *window, bool grabbed)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return true;
    }
    wd->keyboard_grab = grabbed;
    if (grabbed) {
        if (x11.keyboard_grab_window != window) {
            grab_keyboard(window);
        }
    } else if (x11.keyboard_grab_window == window) {
        X11_xcb_ungrab_keyboard(x11.conn, XCB_CURRENT_TIME);
        x11.keyboard_grab_window = NULL;
    }
    x11_flush();
    return true;
}

static void destroy_barriers(void)
{
    for (int i = 0; i < 4; i++) {
        if (x11.barriers[i]) {
            X11_xcb_xfixes_delete_pointer_barrier(x11.conn, x11.barriers[i]);
            x11.barriers[i] = 0;
        }
    }
    x11.barrier_window = NULL;
    memset(&x11.barrier_rect, 0, sizeof(x11.barrier_rect));
}

/* stock X11_ConfineCursorWithFlags, kept in step with focus, mapping,
 * geometry and capture: four barriers along the mouse rect's edges, each
 * spanning all displays so the pointer can't slip out at a corner. Called
 * after every event batch and on SDL_SetWindowMouseRect; idempotent. */
static void x11_update_barriers(void)
{
    if (!x11.has_barriers) {
        return;
    }
    SDL_Window *want = NULL;
    SDL_Rect r = { 0, 0, 0, 0 };
    SDL_Window *w = sdlop.keyboard_focus;
    X11WindowData *wd = w ? wdata(w) : NULL;
    if (wd && wd->mapped && !SDL_RectEmpty(&w->mouse_rect) &&
        !(w->flags & (SDL_WINDOW_MOUSE_CAPTURE | SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))) {
        const SDL_Rect wr = wd->cfg_known ? current_rect(w) : (SDL_Rect){ w->x, w->y, w->w, w->h };
        const SDL_Rect *mr = &w->mouse_rect;
        const int x1 = wr.x + SDL_max(mr->x, 0);
        const int y1 = wr.y + SDL_max(mr->y, 0);
        const int x2 = SDL_min(wr.x + mr->x + mr->w, wr.x + wr.w);
        const int y2 = SDL_min(wr.y + mr->y + mr->h, wr.y + wr.h);
        if (x2 > x1 && y2 > y1) {
            want = w;
            r = (SDL_Rect){ x1, y1, x2 - x1, y2 - y1 };
        }
    }
    if (want == x11.barrier_window &&
        (!want || (r.x == x11.barrier_rect.x && r.y == x11.barrier_rect.y && r.w == x11.barrier_rect.w &&
                   r.h == x11.barrier_rect.h))) {
        return;
    }
    destroy_barriers();
    if (want) {
        SDL_Rect span = { 0, 0, x11.screen->width_in_pixels, x11.screen->height_in_pixels };
        for (int i = 0; i < sdlop.num_displays; i++) {
            SDL_Rect u;
            SDL_GetRectUnion(&span, &sdlop.displays[i].bounds, &u);
            span = u;
        }
        const int sx1 = span.x, sy1 = span.y, sx2 = span.x + span.w, sy2 = span.y + span.h;
        const int x2 = r.x + r.w, y2 = r.y + r.h;
        const struct
        {
            int x1, y1, x2, y2;
            uint32_t dir;
        } b[4] = {
            /* the pointer may cross each line inwards only */
            { r.x, sy1, r.x, sy2, XCB_XFIXES_BARRIER_DIRECTIONS_POSITIVE_X },
            { x2, sy1, x2, sy2, XCB_XFIXES_BARRIER_DIRECTIONS_NEGATIVE_X },
            { sx1, r.y, sx2, r.y, XCB_XFIXES_BARRIER_DIRECTIONS_POSITIVE_Y },
            { sx1, y2, sx2, y2, XCB_XFIXES_BARRIER_DIRECTIONS_NEGATIVE_Y },
        };
        for (int i = 0; i < 4; i++) {
            x11.barriers[i] = X11_xcb_generate_id(x11.conn);
            X11_xcb_xfixes_create_pointer_barrier(x11.conn, x11.barriers[i], wdata(want)->xid, (uint16_t)b[i].x1,
                                                  (uint16_t)b[i].y1, (uint16_t)b[i].x2, (uint16_t)b[i].y2,
                                                  b[i].dir, 0, NULL);
        }
        x11.barrier_window = want;
        x11.barrier_rect = r;
    }
    x11_flush();
}

static bool x11_SetWindowMouseRect(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    /* handle_motion warps back where barriers aren't available */
    x11_update_barriers();
    return true;
}

static bool x11_FlashWindow(SDLop_VideoDevice *device, SDL_Window *window, SDL_FlashOperation operation)
{
    (void)device;
    const bool on = operation != SDL_FLASH_CANCEL;
    set_wm_hints_urgent(window, on);
    request_net_wm_state(window, on, x11.atom._NET_WM_STATE_DEMANDS_ATTENTION, 0);
    x11_flush();
    return true;
}

static bool x11_SetWindowFocusable(SDLop_VideoDevice *device, SDL_Window *window, bool focusable)
{
    (void)device;
    (void)focusable; /* window->flags already updated */
    set_wm_hints(window);
    x11_flush();
    return true;
}

static bool x11_SyncWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)window;
    x11_flush();
    x11_sync();
    x11_PumpEvents(device, -1);
    return true;
}

static bool x11_SuspendScreenSaver(SDLop_VideoDevice *device)
{
    (void)device;
    if (sdlop.suspend_screensaver && x11.conn) {
        x11.screensaver_reset_ns = SDLOP_MonotonicNS();
        X11_xcb_force_screen_saver(x11.conn, XCB_SCREEN_SAVER_RESET);
        x11_flush();
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* cursors and pointer                                                 */
/* ------------------------------------------------------------------ */

static void x11_find_argb_format(void)
{
    if (x11.render_checked) {
        return;
    }
    x11.render_checked = true;
    if (!SDLOP_X11_HaveModule(SDLOP_X11_MOD_RENDER) || SDLOP_HintFlag("SDLOP_X11_NO_RENDER", false)) {
        return;
    }
    const xcb_query_extension_reply_t *ext = X11_xcb_get_extension_data(x11.conn, X11_xcb_render_id);
    if (!ext || !ext->present) {
        return;
    }
    xcb_render_query_pict_formats_reply_t *r = X11_xcb_render_query_pict_formats_reply(
        x11.conn, X11_xcb_render_query_pict_formats(x11.conn), NULL);
    if (!r) {
        return;
    }
    for (xcb_render_pictforminfo_iterator_t it = X11_xcb_render_query_pict_formats_formats_iterator(r); it.rem;
         X11_xcb_render_pictforminfo_next(&it)) {
        const xcb_render_pictforminfo_t *f = it.data;
        if (f->type == XCB_RENDER_PICT_TYPE_DIRECT && f->depth == 32 && f->direct.alpha_mask == 0xff &&
            f->direct.alpha_shift == 24 && f->direct.red_mask == 0xff && f->direct.red_shift == 16 &&
            f->direct.green_mask == 0xff && f->direct.green_shift == 8 && f->direct.blue_mask == 0xff &&
            f->direct.blue_shift == 0) {
            x11.argb_format = f->id;
            break;
        }
    }
    free(r);
}

/* ARGB cursor through RENDER (premultiplied, as the server expects) */
static xcb_cursor_t create_argb_cursor(const Uint32 *argb, int w, int h, int pitch_px, int hot_x, int hot_y)
{
    x11_find_argb_format();
    if (!x11.argb_format || (size_t)w * (size_t)h * 4 + 64 > x11.max_request_bytes) {
        return XCB_CURSOR_NONE;
    }
    uint32_t *pm_data = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!pm_data) {
        return XCB_CURSOR_NONE;
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Uint32 p = argb[(size_t)y * (size_t)pitch_px + (size_t)x];
            const Uint32 a = p >> 24;
            const Uint32 r = ((p >> 16) & 0xff) * a / 255;
            const Uint32 g = ((p >> 8) & 0xff) * a / 255;
            const Uint32 b = (p & 0xff) * a / 255;
            pm_data[(size_t)y * (size_t)w + (size_t)x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    xcb_pixmap_t pm = X11_xcb_generate_id(x11.conn);
    X11_xcb_create_pixmap(x11.conn, 32, pm, x11.root, (uint16_t)w, (uint16_t)h);
    xcb_gcontext_t gc = X11_xcb_generate_id(x11.conn);
    X11_xcb_create_gc(x11.conn, gc, pm, 0, NULL);
    X11_xcb_put_image(x11.conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pm, gc, (uint16_t)w, (uint16_t)h, 0, 0, 0, 32,
                      (uint32_t)w * (uint32_t)h * 4, (const uint8_t *)pm_data);
    X11_xcb_free_gc(x11.conn, gc);
    free(pm_data);
    xcb_render_picture_t pic = X11_xcb_generate_id(x11.conn);
    X11_xcb_render_create_picture(x11.conn, pic, pm, x11.argb_format, 0, NULL);
    xcb_cursor_t cursor = X11_xcb_generate_id(x11.conn);
    X11_xcb_render_create_cursor(x11.conn, cursor, pic, (uint16_t)hot_x, (uint16_t)hot_y);
    X11_xcb_render_free_picture(x11.conn, pic);
    X11_xcb_free_pixmap(x11.conn, pm);
    return cursor;
}

/* two-colour fallback (no RENDER): alpha >= 50% is opaque, light pixels
 * become white, the rest black (SDL3 X11_CreatePixmapCursor) */
static xcb_cursor_t create_bitmap_cursor(const Uint32 *argb, int w, int h, int pitch_px, int hot_x, int hot_y)
{
    const xcb_setup_t *setup = X11_xcb_get_setup(x11.conn);
    const int pad = setup->bitmap_format_scanline_pad ? setup->bitmap_format_scanline_pad : 32;
    const bool msb = setup->bitmap_format_bit_order == XCB_IMAGE_ORDER_MSB_FIRST;
    const size_t stride = (size_t)(((w + pad - 1) / pad) * pad / 8);
    uint8_t *src = (uint8_t *)calloc(stride * (size_t)h, 1);
    uint8_t *mask = (uint8_t *)calloc(stride * (size_t)h, 1);
    if (!src || !mask) {
        free(src);
        free(mask);
        return XCB_CURSOR_NONE;
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Uint32 p = argb[(size_t)y * (size_t)pitch_px + (size_t)x];
            const uint8_t bit = msb ? (uint8_t)(0x80u >> (x & 7)) : (uint8_t)(1u << (x & 7));
            const size_t i = (size_t)y * stride + (size_t)(x >> 3);
            if ((p >> 24) >= 0x80) {
                mask[i] |= bit;
                const unsigned lum = ((p >> 16) & 0xff) * 30 + ((p >> 8) & 0xff) * 59 + (p & 0xff) * 11;
                if (lum >= 128 * 100) {
                    src[i] |= bit;
                }
            }
        }
    }
    xcb_pixmap_t pms = X11_xcb_generate_id(x11.conn);
    xcb_pixmap_t pmm = X11_xcb_generate_id(x11.conn);
    X11_xcb_create_pixmap(x11.conn, 1, pms, x11.root, (uint16_t)w, (uint16_t)h);
    X11_xcb_create_pixmap(x11.conn, 1, pmm, x11.root, (uint16_t)w, (uint16_t)h);
    xcb_gcontext_t gc = X11_xcb_generate_id(x11.conn);
    X11_xcb_create_gc(x11.conn, gc, pms, 0, NULL);
    X11_xcb_put_image(x11.conn, XCB_IMAGE_FORMAT_XY_BITMAP, pms, gc, (uint16_t)w, (uint16_t)h, 0, 0, 0, 1,
                      (uint32_t)(stride * (size_t)h), src);
    X11_xcb_put_image(x11.conn, XCB_IMAGE_FORMAT_XY_BITMAP, pmm, gc, (uint16_t)w, (uint16_t)h, 0, 0, 0, 1,
                      (uint32_t)(stride * (size_t)h), mask);
    X11_xcb_free_gc(x11.conn, gc);
    free(src);
    free(mask);
    xcb_cursor_t cursor = X11_xcb_generate_id(x11.conn);
    /* foreground (source bit 1) white, background black */
    X11_xcb_create_cursor(x11.conn, cursor, pms, pmm, 0xffff, 0xffff, 0xffff, 0, 0, 0, (uint16_t)hot_x, (uint16_t)hot_y);
    X11_xcb_free_pixmap(x11.conn, pms);
    X11_xcb_free_pixmap(x11.conn, pmm);
    return cursor;
}

static xcb_cursor_t create_image_cursor(const Uint32 *argb, int w, int h, int pitch_px, int hot_x, int hot_y)
{
    xcb_cursor_t c = create_argb_cursor(argb, w, h, pitch_px, hot_x, hot_y);
    if (!c) {
        c = create_bitmap_cursor(argb, w, h, pitch_px, hot_x, hot_y);
    }
    return c;
}

static struct sdlop_xcursor_theme *x11_cursor_theme(void)
{
    if (!x11.cursor_theme_tried) {
        x11.cursor_theme_tried = true;
        int size = (int)(24.0f * (x11.content_scale > 0.0f ? x11.content_scale : 1.0f) + 0.5f);
        const char *ss = getenv("XCURSOR_SIZE");
        if (ss) {
            long v = strtol(ss, NULL, 10);
            if (v >= 8 && v <= 256) {
                size = (int)v;
            }
        }
        x11.cursor_theme = sdlop_xcursor_theme_load(getenv("XCURSOR_THEME"), size);
    }
    return x11.cursor_theme;
}

/* glyph from the core "cursor" font (X11/cursorfont.h numbering) */
static unsigned x11_legacy_cursor_shape(SDL_SystemCursor id)
{
    switch (id) {
    case SDL_SYSTEM_CURSOR_TEXT: return 152;        /* XC_xterm */
    case SDL_SYSTEM_CURSOR_WAIT: return 150;        /* XC_watch */
    case SDL_SYSTEM_CURSOR_CROSSHAIR: return 130;   /* XC_tcross */
    case SDL_SYSTEM_CURSOR_PROGRESS: return 150;    /* XC_watch */
    case SDL_SYSTEM_CURSOR_NWSE_RESIZE: return 134; /* XC_top_left_corner */
    case SDL_SYSTEM_CURSOR_NESW_RESIZE: return 136; /* XC_top_right_corner */
    case SDL_SYSTEM_CURSOR_EW_RESIZE: return 108;   /* XC_sb_h_double_arrow */
    case SDL_SYSTEM_CURSOR_NS_RESIZE: return 116;   /* XC_sb_v_double_arrow */
    case SDL_SYSTEM_CURSOR_MOVE: return 52;         /* XC_fleur */
    case SDL_SYSTEM_CURSOR_NOT_ALLOWED: return 88;  /* XC_pirate */
    case SDL_SYSTEM_CURSOR_POINTER: return 60;      /* XC_hand2 */
    case SDL_SYSTEM_CURSOR_NW_RESIZE: return 134;   /* XC_top_left_corner */
    case SDL_SYSTEM_CURSOR_N_RESIZE: return 138;    /* XC_top_side */
    case SDL_SYSTEM_CURSOR_NE_RESIZE: return 136;   /* XC_top_right_corner */
    case SDL_SYSTEM_CURSOR_E_RESIZE: return 96;     /* XC_right_side */
    case SDL_SYSTEM_CURSOR_SE_RESIZE: return 14;    /* XC_bottom_right_corner */
    case SDL_SYSTEM_CURSOR_S_RESIZE: return 16;     /* XC_bottom_side */
    case SDL_SYSTEM_CURSOR_SW_RESIZE: return 12;    /* XC_bottom_left_corner */
    case SDL_SYSTEM_CURSOR_W_RESIZE: return 70;     /* XC_left_side */
    default: return 68;                             /* XC_left_ptr */
    }
}

static const char *x11_legacy_cursor_name(SDL_SystemCursor id)
{
    switch (id) {
    case SDL_SYSTEM_CURSOR_TEXT: return "xterm";
    case SDL_SYSTEM_CURSOR_WAIT: return "watch";
    case SDL_SYSTEM_CURSOR_CROSSHAIR: return "tcross";
    case SDL_SYSTEM_CURSOR_PROGRESS: return "left_ptr_watch";
    case SDL_SYSTEM_CURSOR_NWSE_RESIZE: return "top_left_corner";
    case SDL_SYSTEM_CURSOR_NESW_RESIZE: return "top_right_corner";
    case SDL_SYSTEM_CURSOR_EW_RESIZE: return "sb_h_double_arrow";
    case SDL_SYSTEM_CURSOR_NS_RESIZE: return "sb_v_double_arrow";
    case SDL_SYSTEM_CURSOR_MOVE: return "fleur";
    case SDL_SYSTEM_CURSOR_NOT_ALLOWED: return "crossed_circle";
    case SDL_SYSTEM_CURSOR_POINTER: return "hand2";
    case SDL_SYSTEM_CURSOR_NW_RESIZE: return "top_left_corner";
    case SDL_SYSTEM_CURSOR_N_RESIZE: return "top_side";
    case SDL_SYSTEM_CURSOR_NE_RESIZE: return "top_right_corner";
    case SDL_SYSTEM_CURSOR_E_RESIZE: return "right_side";
    case SDL_SYSTEM_CURSOR_SE_RESIZE: return "bottom_right_corner";
    case SDL_SYSTEM_CURSOR_S_RESIZE: return "bottom_side";
    case SDL_SYSTEM_CURSOR_SW_RESIZE: return "bottom_left_corner";
    case SDL_SYSTEM_CURSOR_W_RESIZE: return "left_side";
    default: return "left_ptr";
    }
}

static xcb_cursor_t create_system_cursor(SDL_SystemCursor id)
{
    /* themed first (SDL3: XcursorLibraryLoadCursor with the CSS name,
     * its fallback, then the legacy X name), core font glyph last */
    struct sdlop_xcursor_theme *theme = x11_cursor_theme();
    if (theme) {
        const char *fallback = NULL;
        const char *names[3];
        names[0] = SDLOP_GetCSSCursorName(id, &fallback);
        names[1] = fallback;
        names[2] = x11_legacy_cursor_name(id);
        for (int i = 0; i < 3; ++i) {
            struct sdlop_xcursor *xc = names[i] ? sdlop_xcursor_theme_get_cursor(theme, names[i]) : NULL;
            if (xc && xc->image_count > 0) {
                const struct sdlop_xcursor_image *img = xc->images[0];
                xcb_cursor_t c = create_argb_cursor((const Uint32 *)img->buffer, (int)img->width, (int)img->height,
                                                    (int)img->width, (int)SDL_min(img->hotspot_x, img->width - 1),
                                                    (int)SDL_min(img->hotspot_y, img->height - 1));
                if (c) {
                    return c;
                }
                break;
            }
        }
    }
    const unsigned shape = x11_legacy_cursor_shape(id);
    xcb_font_t font = X11_xcb_generate_id(x11.conn);
    X11_xcb_open_font(x11.conn, font, 6, "cursor");
    xcb_cursor_t c = X11_xcb_generate_id(x11.conn);
    X11_xcb_create_glyph_cursor(x11.conn, c, font, font, (uint16_t)shape, (uint16_t)(shape + 1), 0, 0, 0, 0xffff,
                                0xffff, 0xffff);
    X11_xcb_close_font(x11.conn, font);
    return c;
}

static bool x11_CreateCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    xcb_cursor_t c;
    if (cursor->surface) {
        SDL_Surface *s = cursor->surface; /* ARGB8888 */
        c = create_image_cursor((const Uint32 *)s->pixels, s->w, s->h, s->pitch / 4, cursor->hot_x, cursor->hot_y);
    } else {
        c = create_system_cursor(cursor->system_id);
    }
    if (!c) {
        return SDL_SetError("Couldn't create X11 cursor");
    }
    cursor->internal = (void *)(uintptr_t)c;
    x11_flush();
    return true;
}

static void x11_FreeCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    xcb_cursor_t c = (xcb_cursor_t)(uintptr_t)cursor->internal;
    if (c && x11.conn) {
        if (x11.current_cursor == c) {
            x11.current_cursor = XCB_CURSOR_NONE;
        }
        X11_xcb_free_cursor(x11.conn, c);
        x11_flush();
    }
    cursor->internal = NULL;
}

/* the cursor a window shows: blank in relative mode, a resize cursor over
 * SDL_HITTEST_RESIZE_* areas (stock), else SDL's current cursor */
static uint32_t window_cursor(SDL_Window *window)
{
    static const SDL_SystemCursor ids[] = {
        SDL_SYSTEM_CURSOR_NW_RESIZE, SDL_SYSTEM_CURSOR_N_RESIZE, SDL_SYSTEM_CURSOR_NE_RESIZE,
        SDL_SYSTEM_CURSOR_E_RESIZE,  SDL_SYSTEM_CURSOR_SE_RESIZE, SDL_SYSTEM_CURSOR_S_RESIZE,
        SDL_SYSTEM_CURSOR_SW_RESIZE, SDL_SYSTEM_CURSOR_W_RESIZE
    };
    X11WindowData *wd = wdata(window);
    if (x11.relative_window == window) {
        return x11.blank_cursor;
    }
    const int rc = wd ? wd->hit_test_result : SDL_HITTEST_NORMAL;
    if (window->hit_test && x11.current_cursor != x11.blank_cursor && rc >= SDL_HITTEST_RESIZE_TOPLEFT &&
        rc <= SDL_HITTEST_RESIZE_LEFT) {
        if (!x11.hit_test_cursors[rc]) {
            x11.hit_test_cursors[rc] = create_system_cursor(ids[rc - SDL_HITTEST_RESIZE_TOPLEFT]);
        }
        if (x11.hit_test_cursors[rc]) {
            return x11.hit_test_cursors[rc];
        }
    }
    return x11.current_cursor;
}

static void update_hit_test(SDL_Window *window, int x, int y, bool force)
{
    X11WindowData *wd = wdata(window);
    if (!wd || !window->hit_test || (!force && SDL_GetMouseState(NULL, NULL) != 0)) {
        return;
    }
    const SDL_Point p = { x, y };
    const int rc = (int)window->hit_test(window, &p, window->hit_test_data);
    if (rc != wd->hit_test_result || force) {
        wd->hit_test_result = rc;
        uint32_t v = window_cursor(window);
        X11_xcb_change_window_attributes(x11.conn, wd->xid, XCB_CW_CURSOR, &v);
    }
}

/* EWMH interactive move / resize by the WM, which needs the pointer
 * (implicit grab of the press included) released first (stock) */
static void send_moveresize(SDL_Window *window, int16_t root_x, int16_t root_y, uint32_t direction)
{
    X11WindowData *wd = wdata(window);
    X11_xcb_ungrab_pointer(x11.conn, XCB_CURRENT_TIME);
    x11.pointer_grabbed = false;
    x11.confine_window = x11.capture_window = NULL; /* re-confined on the next focus */
    send_client_message(wd->xid, x11.atom._NET_WM_MOVERESIZE, (uint32_t)root_x, (uint32_t)root_y, direction,
                        1 /* button */, 1 /* source: application */);
    x11_flush();
}

#define NET_WM_MOVERESIZE_MOVE 8

/* a left press on a draggable / resize area: the WM takes over */
static bool trigger_hit_test(SDL_Window *window, int16_t root_x, int16_t root_y)
{
    X11WindowData *wd = wdata(window);
    if (!wd || !window->hit_test) {
        return false;
    }
    if (wd->hit_test_result == SDL_HITTEST_DRAGGABLE) {
        /* some WMs get confused by a move racing the click-to-focus of the
         * same press: wait for the focus (stock) */
        if (window->flags & SDL_WINDOW_INPUT_FOCUS) {
            send_moveresize(window, root_x, root_y, NET_WM_MOVERESIZE_MOVE);
        } else {
            wd->pending_move = true;
            wd->pending_move_x = root_x;
            wd->pending_move_y = root_y;
        }
        return true;
    }
    if (wd->hit_test_result >= SDL_HITTEST_RESIZE_TOPLEFT && wd->hit_test_result <= SDL_HITTEST_RESIZE_LEFT) {
        /* _NET_WM_MOVERESIZE_SIZE_TOPLEFT (0) .. _SIZE_LEFT (7): SDL's order */
        send_moveresize(window, root_x, root_y, (uint32_t)(wd->hit_test_result - SDL_HITTEST_RESIZE_TOPLEFT));
        return true;
    }
    return false;
}

static bool x11_SetWindowHitTest(SDLop_VideoDevice *device, SDL_Window *window, bool enabled)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (wd) {
        wd->hit_test_result = SDL_HITTEST_NORMAL;
        wd->pending_move = false;
        if (enabled && sdlop.mouse_focus == window && sdlop.mouse_has_position) {
            update_hit_test(window, (int)sdlop.mouse_x, (int)sdlop.mouse_y, true);
        }
        uint32_t v = window_cursor(window);
        X11_xcb_change_window_attributes(x11.conn, wd->xid, XCB_CW_CURSOR, &v);
        x11_flush();
    }
    return true;
}

/* the WM's window menu (GTK protocol, understood by mutter, KWin, xfwm4,
 * openbox ...; stock) at window coordinates x, y */
/* SDL_SetWindowShape: as stock, the alpha channel (stretched to the window
 * size) becomes the input shape - pixels with alpha 0 let clicks through;
 * what is visible is up to the ARGB contents. NULL restores the default. */
static bool x11_UpdateWindowShape(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface *shape)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    if (!x11.has_shape) {
        return SDL_Unsupported();
    }
    if (!shape) {
        X11_xcb_shape_mask(x11.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, wd->xid, 0, 0, XCB_NONE);
        x11_flush();
        return true;
    }
    const int w = window->w, h = window->h;
    if (w <= 0 || h <= 0) {
        return true;
    }
    SDL_Surface *stretched = NULL;
    if (shape->w != w || shape->h != h) {
        stretched = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB32);
        if (!stretched) {
            return false;
        }
        if (!SDL_StretchSurface(shape, NULL, stretched, NULL, SDL_SCALEMODE_LINEAR)) {
            SDL_DestroySurface(stretched);
            return false;
        }
        shape = stretched;
    }
    /* 1-bit image in the server's bitmap format (scanline pad, bit order) */
    const xcb_setup_t *setup = X11_xcb_get_setup(x11.conn);
    const int pad = setup->bitmap_format_scanline_pad ? setup->bitmap_format_scanline_pad : 32;
    const size_t stride = (size_t)(((w + pad - 1) / pad) * pad) / 8;
    const bool msb = setup->bitmap_format_bit_order == XCB_IMAGE_ORDER_MSB_FIRST;
    uint8_t *bits = (uint8_t *)calloc((size_t)h, stride);
    if (!bits) {
        if (stretched) {
            SDL_DestroySurface(stretched);
        }
        return SDL_OutOfMemory();
    }
    for (int y = 0; y < h; y++) {
        /* ARGB32 is A,R,G,B in memory: alpha is byte 0 */
        const Uint8 *a = (const Uint8 *)shape->pixels + (size_t)y * (size_t)shape->pitch;
        uint8_t *row = bits + (size_t)y * stride;
        for (int x = 0; x < w; x++, a += 4) {
            if (*a != SDL_ALPHA_TRANSPARENT) {
                row[x / 8] |= (uint8_t)(msb ? 0x80 >> (x % 8) : 1 << (x % 8));
            }
        }
    }
    if (stretched) {
        SDL_DestroySurface(stretched);
    }
    xcb_pixmap_t pm = X11_xcb_generate_id(x11.conn);
    X11_xcb_create_pixmap(x11.conn, 1, pm, wd->xid, (uint16_t)w, (uint16_t)h);
    xcb_gcontext_t gc = X11_xcb_generate_id(x11.conn);
    X11_xcb_create_gc(x11.conn, gc, pm, 0, NULL);
    int rows = (int)((x11.max_request_bytes - 64) / stride);
    if (rows < 1) {
        rows = 1;
    }
    for (int y = 0; y < h; y += rows) {
        const int n = SDL_min(rows, h - y);
        X11_xcb_put_image(x11.conn, XCB_IMAGE_FORMAT_XY_PIXMAP, pm, gc, (uint16_t)w, (uint16_t)n, 0, (int16_t)y, 0, 1,
                          (uint32_t)(stride * (size_t)n), bits + (size_t)y * stride);
    }
    free(bits);
    X11_xcb_shape_mask(x11.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, wd->xid, 0, 0, pm);
    X11_xcb_free_gc(x11.conn, gc);
    X11_xcb_free_pixmap(x11.conn, pm);
    x11_flush();
    return true;
}

/* SDL_GetWindowICCProfile: the screen's profile from the root window */
static void *x11_GetWindowICCProfile(SDLop_VideoDevice *device, SDL_Window *window, size_t *size)
{
    (void)device;
    (void)window;
    xcb_get_property_reply_t *r =
        x11.icc_atom ? X11_xcb_get_property_reply(x11.conn,
                                                  X11_xcb_get_property(x11.conn, 0, x11.root, x11.icc_atom,
                                                                       XCB_GET_PROPERTY_TYPE_ANY, 0, 1u << 24),
                                                  NULL)
                     : NULL;
    const int len = (r && r->format == 8 && r->type != XCB_NONE) ? X11_xcb_get_property_value_length(r) : 0;
    if (len <= 0) {
        free(r);
        SDL_SetError("Screen is not calibrated.");
        return NULL;
    }
    void *data = SDL_malloc((size_t)len);
    if (data) {
        memcpy(data, X11_xcb_get_property_value(r), (size_t)len);
        *size = (size_t)len;
    }
    free(r);
    return data;
}

static void x11_ShowWindowSystemMenu(SDLop_VideoDevice *device, SDL_Window *window, int x, int y)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return;
    }
    int rx = x, ry = y;
    xcb_translate_coordinates_reply_t *tr = X11_xcb_translate_coordinates_reply(
        x11.conn, X11_xcb_translate_coordinates(x11.conn, wd->xid, x11.root, (int16_t)x, (int16_t)y), NULL);
    if (tr) {
        rx = tr->dst_x;
        ry = tr->dst_y;
        free(tr);
    }
    send_client_message(wd->xid, x11.atom._GTK_SHOW_WINDOW_MENU, 0 /* device */, (uint32_t)rx, (uint32_t)ry, 0, 0);
    x11_flush();
}

static bool x11_ShowCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    xcb_cursor_t c = cursor ? (xcb_cursor_t)(uintptr_t)cursor->internal : x11.blank_cursor;
    x11.current_cursor = c;
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        X11WindowData *wd = wdata(w);
        if (!wd) {
            continue;
        }
        uint32_t v = window_cursor(w);
        X11_xcb_change_window_attributes(x11.conn, wd->xid, XCB_CW_CURSOR, &v);
    }
    x11_flush();
    return true;
}

static bool x11_WarpMouse(SDLop_VideoDevice *device, SDL_Window *window, float x, float y)
{
    (void)device;
    X11WindowData *wd = wdata(window);
    if (!wd) {
        return false;
    }
    X11_xcb_warp_pointer(x11.conn, XCB_WINDOW_NONE, wd->xid, 0, 0, 0, 0, (int16_t)SDL_lroundf(x),
                         (int16_t)SDL_lroundf(y));
    x11_flush();
    return true;
}

static bool x11_WarpMouseGlobal(SDLop_VideoDevice *device, float x, float y)
{
    (void)device;
    X11_xcb_warp_pointer(x11.conn, XCB_WINDOW_NONE, x11.root, 0, 0, 0, 0, (int16_t)SDL_lroundf(x),
                         (int16_t)SDL_lroundf(y));
    x11_flush();
    return true;
}

static bool x11_CaptureMouse(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (window) {
        X11WindowData *wd = wdata(window);
        if (!wd || x11.pointer_grabbed || x11.confine_window) {
            return true; /* an existing grab already delivers everything */
        }
        if (x11.capture_window == window) {
            return true;
        }
        xcb_grab_pointer_reply_t *r = X11_xcb_grab_pointer_reply(
            x11.conn,
            X11_xcb_grab_pointer(x11.conn, 1, wd->xid,
                                 XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                     XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
                                     XCB_EVENT_MASK_LEAVE_WINDOW,
                                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE, XCB_CURSOR_NONE,
                                 XCB_CURRENT_TIME),
            NULL);
        bool ok = r && r->status == XCB_GRAB_STATUS_SUCCESS;
        free(r);
        if (!ok) {
            return SDL_SetError("Failed to capture the mouse");
        }
        x11.capture_window = window;
    } else if (x11.capture_window) {
        X11_xcb_ungrab_pointer(x11.conn, XCB_CURRENT_TIME);
        x11.capture_window = NULL;
    }
    x11_flush();
    return true;
}

static SDL_MouseButtonFlags x11_GetGlobalMouseState(SDLop_VideoDevice *device, float *x, float *y)
{
    (void)device;
    SDL_MouseButtonFlags buttons = 0;
    xcb_query_pointer_reply_t *r =
        X11_xcb_query_pointer_reply(x11.conn, X11_xcb_query_pointer(x11.conn, x11.root), NULL);
    if (r) {
        *x = (float)r->root_x;
        *y = (float)r->root_y;
        if (r->mask & XCB_BUTTON_MASK_1) {
            buttons |= SDL_BUTTON_LMASK;
        }
        if (r->mask & XCB_BUTTON_MASK_2) {
            buttons |= SDL_BUTTON_MMASK;
        }
        if (r->mask & XCB_BUTTON_MASK_3) {
            buttons |= SDL_BUTTON_RMASK;
        }
        free(r);
    }
    /* X1/X2 are not in the core mask; use what SDL has seen */
    buttons |= sdlop.mouse_buttons & (SDL_BUTTON_X1MASK | SDL_BUTTON_X2MASK);
    return buttons;
}

/* ------------------------------------------------------------------ */
/* event dispatch                                                      */
/* ------------------------------------------------------------------ */

static void handle_configure(SDL_Window *window, xcb_configure_notify_event_t *ce, bool synthetic)
{
    X11WindowData *wd = wdata(window);
    if (wd->sync_state == SYNC_REQUESTED) {
        wd->sync_state = SYNC_CONFIGURED;
    }
    int x = ce->x, y = ce->y;
    if (!synthetic && wd->parent != x11.root) {
        /* real events are relative to the WM frame; synthetic ones (sent
         * by the WM, ICCCM 4.1.5) already carry root coordinates */
        xcb_translate_coordinates_reply_t *tr = X11_xcb_translate_coordinates_reply(
            x11.conn, X11_xcb_translate_coordinates(x11.conn, wd->xid, x11.root, 0, 0), NULL);
        if (tr) {
            x = tr->dst_x;
            y = tr->dst_y;
            free(tr);
        }
    }
    /* compare against both the last confirmed geometry and the app's view
     * (window->w/h already hold a pending SDL_SetWindowSize request) */
    bool size_changed = ce->width != wd->cfg_w || ce->height != wd->cfg_h ||
                        ce->width != window->w || ce->height != window->h;
    bool pos_changed = x != wd->cfg_x || y != wd->cfg_y || x != window->x || y != window->y;
    wd->cfg_x = x;
    wd->cfg_y = y;
    wd->cfg_w = ce->width;
    wd->cfg_h = ce->height;
    wd->cfg_known = true;

    if (size_changed && ce->width > 0 && ce->height > 0) {
        window->w = ce->width;
        window->h = ce->height;
        /* no reallocation here: the window surface follows the size at the
         * next SDL_GetWindowSurface / SDL_UpdateWindowSurface
         * (x11_ResizeWindowFramebuffer), once per burst of ConfigureNotify */
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, window->w, window->h);
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_SAFE_AREA_CHANGED, 0, 0);
    }
    if (pos_changed) {
        window->x = x;
        window->y = y;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOVED, x, y);
    }
}

static void handle_client_message(SDL_Window *window, xcb_client_message_event_t *cm)
{
    if (cm->type != x11.atom.WM_PROTOCOLS) {
        return;
    }
    xcb_atom_t proto = cm->data.data32[0];
    if (proto == x11.atom.WM_DELETE_WINDOW) {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
    } else if (proto == x11.atom._NET_WM_SYNC_REQUEST) {
        /* data: protocol, timestamp, value low, value high */
        X11WindowData *wd = wdata(window);
        if (wd && wd->sync_counter) {
            wd->sync_value.lo = cm->data.data32[2];
            wd->sync_value.hi = (int32_t)cm->data.data32[3];
            wd->sync_state = SYNC_REQUESTED;
        }
    } else if (proto == x11.atom._NET_WM_PING) {
        /* EWMH: answer by bouncing the message to the root */
        X11Event32 reply;
        memset(&reply, 0, sizeof(reply));
        reply.client = *cm;
        reply.client.response_type = XCB_CLIENT_MESSAGE;
        reply.client.window = x11.root;
        send_to_root(&reply);
        x11_flush();
    }
}

static void x11_dispatch(xcb_generic_event_t *ev)
{
    uint8_t type = ev->response_type & 0x7f;
    bool synthetic = (ev->response_type & 0x80) != 0;

    if (type == 0) {
        xcb_generic_error_t *err = (xcb_generic_error_t *)ev;
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "SDLop x11: X error %d (request %d.%d)", err->error_code,
                     err->major_code, err->minor_code);
        return;
    }
    if (x11.has_xkb && type == x11.xkb_event_base) {
        handle_xkb_event(ev);
        return;
    }
    if (SDLOP_X11_Clipboard_HandleEvent(ev) || SDLOP_X11_DnD_HandleEvent(ev)) {
        return;
    }
    bool settings_changed = false;
    if (SDLOP_X11_XSettings_HandleEvent(ev, &settings_changed)) {
        if (settings_changed) {
            x11.displays_dirty = true; /* the content scale may follow them */
        }
        return;
    }
    if (x11.has_randr && (type == x11.randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
                          type == x11.randr_event_base + XCB_RANDR_NOTIFY)) {
        x11.displays_dirty = true;
        return;
    }

    switch (type) {
    case XCB_KEY_PRESS:
    case XCB_KEY_RELEASE:
        handle_key((xcb_key_press_event_t *)ev, type == XCB_KEY_PRESS, false);
        break;
    case XCB_BUTTON_PRESS:
    case XCB_BUTTON_RELEASE:
        handle_button((xcb_button_press_event_t *)ev, type == XCB_BUTTON_PRESS);
        break;
    case XCB_MOTION_NOTIFY:
        handle_motion((xcb_motion_notify_event_t *)ev);
        break;
    case XCB_ENTER_NOTIFY: {
        xcb_enter_notify_event_t *en = (xcb_enter_notify_event_t *)ev;
        SDL_Window *window = window_from_xid(en->event);
        if (window && en->mode == XCB_NOTIFY_MODE_UNGRAB) {
            sdlop.pointer_grabbed_elsewhere = false; /* someone's pointer grab ended */
        }
        if (!window || (en->mode == XCB_NOTIFY_MODE_GRAB && sdlop.mouse_focus == window)) {
            break;
        }
        x11.pointer_focus = window;
        if (sdlop.mouse_focus != window) {
            sdlop.mouse_focus = window;
            window->flags |= SDL_WINDOW_MOUSE_FOCUS;
            sdlop.mouse_has_position = false;
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_ENTER, 0, 0);
        }
        if (x11.relative_window != window) {
            update_hit_test(window, en->event_x, en->event_y, false);
        }
        if (x11.relative_window == window) {
            /* relative mode: no absolute position */
        } else if (SDLOP_RawInputIsSource()) {
            sdlop.mouse_x = en->event_x; /* anchor only; evdev sends motion */
            sdlop.mouse_y = en->event_y;
            sdlop.mouse_has_position = true;
        } else {
            SDLOP_SendMouseMotion((float)en->event_x, (float)en->event_y, 0.0f, 0.0f, SDLOP_MonotonicNS());
        }
        break;
    }
    case XCB_LEAVE_NOTIFY: {
        xcb_leave_notify_event_t *lv = (xcb_leave_notify_event_t *)ev;
        SDL_Window *window = window_from_xid(lv->event);
        if (window && lv->mode == XCB_NOTIFY_MODE_GRAB && !x11.pointer_grabbed && !x11.confine_window) {
            /* another client (WM move/menu, ...) grabbed the pointer: the
               server sends us nothing until it ends, nor may raw input */
            sdlop.pointer_grabbed_elsewhere = true;
            SDLOP_RawInputResetPointerFrame();
        }
        /* grabs (WM moves, our relative grab) and moves into child
         * windows are not real leaves (SDL3 filter) */
        if (!window || lv->mode == XCB_NOTIFY_MODE_GRAB || lv->mode == XCB_NOTIFY_MODE_UNGRAB ||
            lv->detail == XCB_NOTIFY_DETAIL_INFERIOR || x11.relative_window == window) {
            break;
        }
        if (x11.pointer_focus == window) {
            x11.pointer_focus = NULL;
        }
        if (sdlop.mouse_focus == window) {
            window->flags &= ~SDL_WINDOW_MOUSE_FOCUS;
            sdlop.mouse_focus = NULL;
            SDLOP_RawInputResetPointerFrame();
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_LEAVE, 0, 0);
        }
        break;
    }
    case XCB_FOCUS_IN:
    case XCB_FOCUS_OUT: {
        xcb_focus_in_event_t *fe = (xcb_focus_in_event_t *)ev;
        SDL_Window *window = window_from_xid(fe->event);
        if (window && fe->mode == XCB_NOTIFY_MODE_GRAB) {
            /* FocusOut(Grab): someone else's keyboard grab (screen locker,
               WM switcher) - key events go there, raw ones must too.
               FocusIn(Grab): the grab is on one of our windows (ours). */
            sdlop.keyboard_grabbed_elsewhere = (type == XCB_FOCUS_OUT) && !x11.keyboard_grab_window;
            if (sdlop.keyboard_grabbed_elsewhere) { sdlop.repeat_active = false; }
        } else if (window && fe->mode == XCB_NOTIFY_MODE_UNGRAB) {
            sdlop.keyboard_grabbed_elsewhere = false;
        } else if (window && type == XCB_FOCUS_IN) {
            sdlop.keyboard_grabbed_elsewhere = false;
        }
        if (!window || fe->mode == XCB_NOTIFY_MODE_GRAB || fe->mode == XCB_NOTIFY_MODE_UNGRAB ||
            fe->detail == XCB_NOTIFY_DETAIL_INFERIOR || fe->detail == XCB_NOTIFY_DETAIL_POINTER) {
            break;
        }
        if (type == XCB_FOCUS_IN) {
            set_keyboard_focus(window);
            x11_sync_xkb_state();
            X11WindowData *wd = wdata(window);
            if (x11.relative_window == window && !x11.pointer_grabbed && wd->want_grab) {
                grab_pointer(window);
            }
            if (wd->mouse_grab && x11.confine_window != window && x11.relative_window != window) {
                confine_pointer(window);
            }
            if (wd->keyboard_grab && x11.keyboard_grab_window != window) {
                grab_keyboard(window);
            }
            if (wd->pending_move) {
                wd->pending_move = false;
                send_moveresize(window, wd->pending_move_x, wd->pending_move_y, NET_WM_MOVERESIZE_MOVE);
            }
        } else if (sdlop.keyboard_focus == window) {
            set_keyboard_focus(NULL);
        }
        break;
    }
    case XCB_EXPOSE: {
        xcb_expose_event_t *ex = (xcb_expose_event_t *)ev;
        SDL_Window *window = window_from_xid(ex->window);
        if (!window) {
            break;
        }
        X11WindowData *wd = wdata(window);
        if (wd->pixels && !wd->gl_active) {
            fb_put(wd, ex->x, ex->y, ex->width, ex->height);
        }
        if (ex->count == 0) {
            if (wd->pixels && !wd->gl_active) {
                x11_flush();
            }
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_EXPOSED, 0, 0);
        }
        break;
    }
    case XCB_CONFIGURE_NOTIFY: {
        xcb_configure_notify_event_t *ce = (xcb_configure_notify_event_t *)ev;
        SDL_Window *window = window_from_xid(ce->window);
        if (window && ce->event == ce->window) {
            handle_configure(window, ce, synthetic);
        }
        break;
    }
    case XCB_REPARENT_NOTIFY: {
        xcb_reparent_notify_event_t *rp = (xcb_reparent_notify_event_t *)ev;
        SDL_Window *window = window_from_xid(rp->window);
        if (window) {
            wdata(window)->parent = rp->parent;
        }
        break;
    }
    case XCB_MAP_NOTIFY: {
        xcb_map_notify_event_t *mn = (xcb_map_notify_event_t *)ev;
        SDL_Window *window = window_from_xid(mn->window);
        if (window && mn->event == mn->window) {
            wdata(window)->mapped = true;
        }
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        xcb_unmap_notify_event_t *un = (xcb_unmap_notify_event_t *)ev;
        SDL_Window *window = window_from_xid(un->window);
        if (window && un->event == un->window && !synthetic) {
            wdata(window)->mapped = false;
            if (x11.relative_window == window && x11.pointer_grabbed) {
                x11.pointer_grabbed = false; /* the server released it */
                wdata(window)->want_grab = true;
            }
        }
        break;
    }
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *pn = (xcb_property_notify_event_t *)ev;
        if (pn->window == x11.root) {
            if (pn->atom == x11.atom.RESOURCE_MANAGER) {
                x11.displays_dirty = true; /* Xft.dpi may have changed */
            } else if (pn->atom == x11.icc_atom && x11.icc_atom) {
                /* all windows live on this screen */
                for (SDL_Window *w = sdlop.windows; w; w = w->next) {
                    if (wdata(w)) {
                        SDLOP_SendWindowEvent(w, SDL_EVENT_WINDOW_ICCPROF_CHANGED, 0, 0);
                    }
                }
            }
            break;
        }
        SDL_Window *window = window_from_xid(pn->window);
        if (window && (pn->atom == x11.atom._NET_WM_STATE || pn->atom == x11.atom.WM_STATE)) {
            refresh_wm_state(window);
        }
        break;
    }
    case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
        SDL_Window *window = window_from_xid(cm->window);
        if (window) {
            handle_client_message(window, cm);
        }
        break;
    }
    case XCB_MAPPING_NOTIFY:
        if (!x11.has_xkb) {
            x11.keymap_dirty = true; /* core keyboard mapping changed */
        }
        break;
    case XCB_GE_GENERIC: {
        xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *)ev;
        if (x11.has_xi2 && ge->extension == x11.xi_opcode) {
            handle_xi_event(ge);
        }
        break;
    }
    default:
        break;
    }
}

static void x11_PumpEvents(SDLop_VideoDevice *device, int timeout_ms)
{
    (void)device;
    if (!x11.conn || x11.dead) {
        return;
    }
    X11_xcb_flush(x11.conn);

    if (timeout_ms > 0 && !x11.peeked) {
        /* only sleep when xcb has nothing queued: replies read by earlier
         * roundtrips may have pulled events off the socket already */
        x11.peeked = X11_xcb_poll_for_queued_event(x11.conn);
        if (!x11.peeked) {
            struct pollfd pfd[2] = { { X11_xcb_get_file_descriptor(x11.conn), POLLIN, 0 },
                                     { SDLOP_X11_XIM_GetFD(), POLLIN, 0 } };
            poll(pfd, pfd[1].fd >= 0 ? 2 : 1, timeout_ms);
        }
    }

    /* drain completely: xcb_poll_for_event reads the socket and returns
     * queued events first, so nothing is left behind the fd */
    for (;;) {
        xcb_generic_event_t *ev = x11.peeked;
        x11.peeked = NULL;
        if (!ev) {
            ev = X11_xcb_poll_for_event(x11.conn);
        }
        if (!ev) {
            break;
        }
        x11_dispatch(ev);
        free(ev);
    }

    if (X11_xcb_connection_has_error(x11.conn)) {
        /* X server went away (SDL3 would exit from the IO error handler) */
        x11.dead = true;
        SDL_SetError("X11 connection lost");
        SDLOP_SendQuitEvent();
        return;
    }
    SDLOP_X11_XIM_Pump(); /* input method replies: commits, preedit, keys handed back */
    if (x11.keymap_dirty) {
        x11.keymap_dirty = false;
        x11_load_keymap();
    }
    if (x11.displays_dirty) {
        x11.displays_dirty = false;
        x11_update_displays(true);
    }
    x11_update_barriers(); /* focus, mapping, geometry may have moved */
    if (sdlop.suspend_screensaver) {
        /* SDL3 X11: XResetScreenSaver every 30 s while inhibited */
        Uint64 now = SDLOP_MonotonicNS();
        if (now - x11.screensaver_reset_ns >= 30ull * 1000000000ull) {
            x11.screensaver_reset_ns = now;
            X11_xcb_force_screen_saver(x11.conn, XCB_SCREEN_SAVER_RESET);
        }
    }
    X11_xcb_flush(x11.conn);
    SDLOP_KeyboardProcessRepeats();
}

/* text input: XIM (sdlop_x11_xim.c) */
static bool x11_StartTextInput(SDLop_VideoDevice *device, SDL_Window *window, SDL_PropertiesID props)
{
    (void)device;
    (void)props;
    return SDLOP_X11_XIM_Start(window);
}

static bool x11_StopTextInput(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    return SDLOP_X11_XIM_Stop(window);
}

static bool x11_UpdateTextInputArea(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    return SDLOP_X11_XIM_UpdateArea(window);
}

static bool x11_ClearComposition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    return SDLOP_X11_XIM_ClearComposition(window);
}

static int x11_GetEventFD(SDLop_VideoDevice *device)
{
    (void)device;
    if (!x11.conn || x11.dead) {
        return -1;
    }
    return X11_xcb_get_file_descriptor(x11.conn);
}

SDLop_VideoDevice SDLop_x11_device = {
    "x11",
    x11_Init,
    x11_Quit,
    x11_CreateWindow,
    x11_DestroyWindow,
    x11_ShowWindow,
    x11_HideWindow,
    x11_SetWindowTitle,
    x11_SetWindowSize,
    x11_SetWindowPosition,
    x11_MinimizeWindow,
    x11_MaximizeWindow,
    x11_RestoreWindow,
    x11_SetWindowFullscreen,
    x11_RaiseWindow,
    x11_SetWindowClearColor,
    x11_SetWindowRelativeMouseMode,
    x11_PumpEvents,
    x11_GetEventFD,

    /* software surface: MIT-SHM, xcb_put_image fallback */
    x11_CreateWindowFramebuffer,
    x11_UpdateWindowFramebuffer,
    x11_DestroyWindowFramebuffer,

    /* OpenGL via EGL (EGL_PLATFORM_XCB_EXT, EGL_PLATFORM_X11_EXT fallback) */
    SDLOP_X11_GL_CreateContext,
    SDLOP_X11_GL_MakeCurrent,
    SDLOP_X11_GL_SwapBuffers,
    SDLOP_X11_GL_DeleteContext,
    SDLOP_X11_GL_GetProcAddress,
    SDLOP_X11_GL_SetSwapInterval,
    SDLOP_X11_GL_GetSwapInterval,

    /* Vulkan (VK_KHR_xcb_surface) */
    SDLOP_X11_Vulkan_CreateSurface,
    SDLOP_X11_Vulkan_GetInstanceExtensions,

    /* optional hooks */
    .Vulkan_GetPresentationSupport = SDLOP_X11_Vulkan_GetPresentationSupport,
    .supports_popups = true,
    .window_system_input = true,
    .EGL_GetDisplay = SDLOP_X11_EGL_GetDisplay,
    .EGL_GetConfig = SDLOP_X11_EGL_GetConfig,
    .EGL_GetWindowSurface = SDLOP_X11_EGL_GetWindowSurface,
    .SetWindowMinimumSize = x11_UpdateSizeHints,
    .SetWindowMaximumSize = x11_UpdateSizeHints,
    .SetWindowAspectRatio = x11_UpdateSizeHints,
    .GetWindowBordersSize = x11_GetWindowBordersSize,
    .SetWindowOpacity = x11_SetWindowOpacity,
    .SetWindowParent = x11_SetWindowParent,
    .SetWindowHitTest = x11_SetWindowHitTest,
    .ShowWindowSystemMenu = x11_ShowWindowSystemMenu,
    .UpdateWindowShape = x11_UpdateWindowShape,
    .GetWindowICCProfile = x11_GetWindowICCProfile,
    .SetDisplayMode = x11_SetDisplayMode,
    .SetWindowModal = x11_SetWindowModal,
    .SetWindowBordered = x11_SetWindowBordered,
    .SetWindowResizable = x11_SetWindowResizable,
    .SetWindowAlwaysOnTop = x11_SetWindowAlwaysOnTop,
    .SetWindowIcon = x11_SetWindowIcon,
    .SetWindowMouseGrab = x11_SetWindowMouseGrab,
    .SetWindowKeyboardGrab = x11_SetWindowKeyboardGrab,
    .SetWindowMouseRect = x11_SetWindowMouseRect,
    .FlashWindow = x11_FlashWindow,
    .SyncWindow = x11_SyncWindow,
    .SetWindowFocusable = x11_SetWindowFocusable,
    .SuspendScreenSaver = x11_SuspendScreenSaver,
    .CreateCursor = x11_CreateCursor,
    .FreeCursor = x11_FreeCursor,
    .ShowCursor = x11_ShowCursor,
    .WarpMouse = x11_WarpMouse,
    .WarpMouseGlobal = x11_WarpMouseGlobal,
    .CaptureMouse = x11_CaptureMouse,
    .GetGlobalMouseState = x11_GetGlobalMouseState,
    .SetClipboardData = SDLOP_X11_SetClipboardData,
    .GetClipboardData = SDLOP_X11_GetClipboardData,
    .HasClipboardData = SDLOP_X11_HasClipboardData,
    .GetTextMimeTypes = SDLOP_X11_GetTextMimeTypes,
    .SetPrimarySelectionText = SDLOP_X11_SetPrimarySelectionText,
    .GetPrimarySelectionText = SDLOP_X11_GetPrimarySelectionText,
    .HasPrimarySelectionText = SDLOP_X11_HasPrimarySelectionText,
    .AcceptDragAndDrop = SDLOP_X11_AcceptDragAndDrop,
    .StartTextInput = x11_StartTextInput,
    .StopTextInput = x11_StopTextInput,
    .UpdateTextInputArea = x11_UpdateTextInputArea,
    .ResizeWindowFramebuffer = x11_ResizeWindowFramebuffer,
    .ClearComposition = x11_ClearComposition,
};
