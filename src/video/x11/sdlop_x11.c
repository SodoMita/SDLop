/*
  SDLop - X11 video driver (Xlib, dlopen'd at run time).

  Windowing, input and software rendering over the core X11 protocol.
  libX11 is loaded with dlopen (see sdlop_x11_dyn.c): nothing links
  against X11 at build time, and the driver degrades to a clean
  SDL error when libX11 or the X display is missing.

  Input policy matches the Wayland driver: when the asyncinput-style
  evdev worker owns a device class, the corresponding X input is ignored
  (evdev wins - lower latency). Keyboard layout/text goes through the
  shared xkbcommon layer; the keymap is built from the server's
  _XKB_RULES_NAMES property (falling back to the system default keymap).

  Window-manager interaction follows ICCCM/EWMH (WM_DELETE_WINDOW,
  _NET_WM_PING, _NET_WM_STATE), like SDL3's X11 backend; under a WM-less
  server (Xvfb) those requests are simply no-ops.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include "internal/scancode_evdev.h"

/* cursor font glyph (stable X11 constant, <X11/cursorfont.h>) */
#define XC_left_ptr 68

/* EWMH _NET_WM_STATE actions */
#define NET_WM_STATE_REMOVE 0
#define NET_WM_STATE_ADD    1
#define NET_WM_STATE_TOGGLE 2

typedef struct X11DeviceData
{
    SDLOP_Display *display;
    int screen;
    SDLOP_Window root;
    int display_width, display_height;
    bool dead; /* IO error: connection lost */

    /* atoms (interned at init) */
    SDLOP_Atom wm_protocols;
    SDLOP_Atom wm_delete_window;
    SDLOP_Atom net_wm_ping;
    SDLOP_Atom net_wm_pid;
    SDLOP_Atom net_wm_name;
    SDLOP_Atom utf8_string;
    SDLOP_Atom net_wm_state;
    SDLOP_Atom net_wm_state_fullscreen;
    SDLOP_Atom net_wm_state_maximized_vert;
    SDLOP_Atom net_wm_state_maximized_horz;
    SDLOP_Atom net_wm_state_hidden;
    SDLOP_Atom net_active_window;
    SDLOP_Atom xkb_rules_names;

    bool xkb_ext; /* XKB extension present (for XkbGetState mod sync) */
    int xkb_event_base;
    int xkb_error_base;

    SDLOP_Cursor default_cursor;
    SDLOP_Cursor invisible_cursor;
    SDLOP_Pixmap invisible_pixmap;

    SDL_Window *relative_window; /* window holding the relative-mode grab */

    /* protocol error captured by the error handler around sync points */
    volatile int last_error_code;
} X11DeviceData;

static X11DeviceData x11_data;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

SDLOP_Display *SDLOP_X11_GetDisplay(SDLop_VideoDevice *device)
{
    (void)device;
    return x11_data.display;
}

SDLOP_Window SDLOP_X11_GetWindowHandle(SDL_Window *window)
{
    X11WindowData *wd = window ? (X11WindowData *)window->driverdata : NULL;
    return wd ? wd->xwindow : SDLOP_None;
}

void SDLOP_X11_SetGLActive(SDL_Window *window, bool active)
{
    X11WindowData *wd = window ? (X11WindowData *)window->driverdata : NULL;
    if (wd) {
        wd->gl_active = active;
    }
}

static SDL_Window *window_from_xwindow(SDLOP_Window xwindow)
{
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        X11WindowData *wd = (X11WindowData *)w->driverdata;
        if (wd && wd->xwindow == xwindow) {
            return w;
        }
    }
    return NULL;
}

static SDLOP_Window root_window(void)
{
    return x11_data.root;
}

/* ------------------------------------------------------------------ */
/* error handlers                                                      */
/* ------------------------------------------------------------------ */

static int x11_error_handler(SDLOP_Display *dpy, SDLOP_XErrorEvent *err)
{
    (void)dpy;
    x11_data.last_error_code = err->error_code;
    char buf[128];
    buf[0] = '\0';
    if (SDLOP_X11_XGetErrorText) {
        SDLOP_X11_XGetErrorText(dpy, err->error_code, buf, (int)sizeof(buf));
        buf[sizeof(buf) - 1] = '\0';
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "SDLop X11 protocol error %d (%s) for request %d.%d",
                err->error_code, buf, err->request_code, err->minor_code);
    return 0;
}

static int x11_ioerror_handler(SDLOP_Display *dpy)
{
    (void)dpy;
    x11_data.dead = true;
    SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "SDLop X11: connection to display lost");
    /* Best effort: let the app see a QUIT before libX11 exits the process.
     * (An X IO error means the server is gone; like SDL3, there is no
     * meaningful recovery - libX11 terminates the client when this handler
     * returns.) */
    SDLOP_SendQuitEvent();
    return 0;
}

/* ------------------------------------------------------------------ */
/* keymap (server RMLVO -> xkbcommon)                                  */
/* ------------------------------------------------------------------ */

/* Read _XKB_RULES_NAMES from the root window (five NUL-separated strings:
 * rules, model, layout, variant, options) and build the xkb keymap from
 * it. This is how the server itself was configured, so the result matches
 * the server's layout without needing the XKB protocol extension. */
static void x11_setup_keymap(void)
{
    unsigned char *data = NULL;
    SDLOP_Atom actual_type = SDLOP_None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;

    if (SDLOP_X11_XGetWindowProperty(x11_data.display, root_window(),
                                     x11_data.xkb_rules_names, 0, 1024,
                                     SDLOP_False, SDLOP_XA_STRING,
                                     &actual_type, &actual_format,
                                     &nitems, &bytes_after, &data) != 0 || !data) {
        return; /* no rules property: keep the core default keymap */
    }
    if (actual_type != SDLOP_XA_STRING || actual_format != 8 || nitems == 0) {
        SDLOP_X11_XFree(data);
        return;
    }

    char *fields[5] = { NULL, NULL, NULL, NULL, NULL };
    int field = 0;
    fields[field] = (char *)data;
    for (unsigned long i = 0; i < nitems && field < 4; i++) {
        if (data[i] == '\0') {
            fields[++field] = (char *)&data[i + 1];
        }
    }
    if (!SDLOP_KeyboardSetKeymapNames(fields[0], fields[1], fields[2], fields[3], fields[4])) {
        SDL_ClearError(); /* optional: core falls back to the default keymap */
    }
    SDLOP_X11_XFree(data);
}

/* resync xkb modifier state from the server (focus changes can leave the
 * per-key-event tracking stale: locks toggled while unfocused etc.) */
static void x11_sync_modifiers(void)
{
    if (!x11_data.xkb_ext) {
        return;
    }
    SDLOP_XkbStateRec state;
    memset(&state, 0, sizeof(state));
    if (SDLOP_X11_XkbGetState(x11_data.display, SDLOP_XkbUseCoreKbd, &state) == 0 /* Success */) {
        SDLOP_KeyboardUpdateXkbModifiers(state.mods, state.latched_mods, state.locked_mods);
    }
}

/* ------------------------------------------------------------------ */
/* cursors                                                             */
/* ------------------------------------------------------------------ */

static void x11_create_cursors(void)
{
    if (!x11_data.default_cursor) {
        x11_data.default_cursor = SDLOP_X11_XCreateFontCursor(x11_data.display, XC_left_ptr);
    }
    if (!x11_data.invisible_cursor) {
        /* 1x1 empty pixmap cursor (classic invisible cursor, core X11) */
        SDLOP_Pixmap pm = SDLOP_X11_XCreatePixmap(x11_data.display, root_window(), 1, 1, 1);
        if (pm != SDLOP_None) {
            SDLOP_XColor black;
            memset(&black, 0, sizeof(black));
            SDLOP_Cursor c = SDLOP_X11_XCreatePixmapCursor(x11_data.display, pm, pm, &black, &black, 0, 0);
            if (c != SDLOP_None) {
                x11_data.invisible_cursor = c;
                x11_data.invisible_pixmap = pm;
            } else {
                SDLOP_X11_XFreePixmap(x11_data.display, pm);
            }
        }
    }
}

static void x11_apply_cursor(SDL_Window *window)
{
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd) {
        return;
    }
    SDLOP_Cursor c = wd->relative_mode ? x11_data.invisible_cursor : x11_data.default_cursor;
    if (c != SDLOP_None) {
        SDLOP_X11_XDefineCursor(x11_data.display, wd->xwindow, c);
    }
}

/* ------------------------------------------------------------------ */
/* relative mouse mode (grab + hidden cursor + recentering)            */
/* ------------------------------------------------------------------ */

static void x11_release_relative_mode(SDL_Window *window)
{
    X11WindowData *wd = window ? (X11WindowData *)window->driverdata : NULL;
    if (!wd || !wd->relative_mode) {
        return;
    }
    SDLOP_X11_XUngrabPointer(x11_data.display, SDLOP_CurrentTime);
    wd->relative_mode = false;
    wd->warp_pending = false;
    if (x11_data.relative_window == window) {
        x11_data.relative_window = NULL;
    }
    x11_apply_cursor(window);
    SDLOP_X11_XFlush(x11_data.display);
}

static bool x11_arm_relative_mode(SDL_Window *window)
{
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    if (wd->relative_mode) {
        return true;
    }
    x11_create_cursors();

    unsigned int mask = SDLOP_ButtonPressMask | SDLOP_ButtonReleaseMask |
                        SDLOP_PointerMotionMask | SDLOP_EnterWindowMask |
                        SDLOP_LeaveWindowMask;
    int rc = SDLOP_X11_XGrabPointer(x11_data.display, wd->xwindow, SDLOP_False, mask,
                                    SDLOP_GrabModeAsync, SDLOP_GrabModeAsync,
                                    wd->xwindow, x11_data.invisible_cursor, SDLOP_CurrentTime);
    if (rc != SDLOP_GrabSuccess) {
        return SDL_SetError("XGrabPointer failed (status %d)", rc);
    }
    wd->relative_mode = true;
    wd->warp_center_x = window->w / 2;
    wd->warp_center_y = window->h / 2;
    wd->warp_pending = true;
    SDLOP_X11_XWarpPointer(x11_data.display, SDLOP_None, wd->xwindow, 0, 0, 0, 0,
                           wd->warp_center_x, wd->warp_center_y);
    SDLOP_X11_XFlush(x11_data.display);
    x11_data.relative_window = window;
    return true;
}

/* ------------------------------------------------------------------ */
/* software framebuffer (XImage + XPutImage)                           */
/* ------------------------------------------------------------------ */

static void x11_destroy_ximage(X11WindowData *wd)
{
    if (wd->ximage) {
        /* XDestroyImage frees image->data; the pixels are owned by the
         * SDL surface, so detach them first (same as SDL3). */
        wd->ximage->data = NULL;
        wd->ximage->f.destroy_image(wd->ximage);
        wd->ximage = NULL;
    }
    wd->fb_pixels = NULL; /* owned by window->surface */
    wd->fb_w = wd->fb_h = 0;
}

static bool x11_create_ximage(SDL_Window *window)
{
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    x11_destroy_ximage(wd);

    int depth = SDLOP_X11_XDefaultDepth(x11_data.display, x11_data.screen);
    if (depth != 24 && depth != 32) {
        return SDL_SetError("X11 software surface requires a 24/32 bpp display (got %d)", depth);
    }
    SDL_Surface *s = window->surface;
    SDLOP_XImage *img = SDLOP_X11_XCreateImage(x11_data.display,
                                               SDLOP_X11_XDefaultVisual(x11_data.display, x11_data.screen),
                                               (unsigned int)depth, SDLOP_ZPixmap, 0,
                                               (char *)s->pixels,
                                               (unsigned int)s->w, (unsigned int)s->h,
                                               32, s->pitch);
    if (!img) {
        return SDL_SetError("XCreateImage failed");
    }
    wd->ximage = img;
    wd->fb_pixels = s->pixels;
    wd->fb_w = s->w;
    wd->fb_h = s->h;
    return true;
}

static bool x11_CreateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface **surface)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    SDL_Surface *s = SDL_CreateSurface(window->w, window->h, SDL_PIXELFORMAT_XRGB8888);
    if (!s) {
        return false;
    }
    window->surface = s; /* needed by x11_create_ximage */
    if (!x11_create_ximage(window)) {
        window->surface = NULL;
        SDL_DestroySurface(s);
        return false;
    }
    *surface = s;
    return true;
}

static bool x11_UpdateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || !wd->ximage) {
        return SDL_SetError("Window has no X11 framebuffer");
    }
    if (wd->gl_active || (window->flags & SDL_WINDOW_HIDDEN) || !wd->mapped) {
        return true; /* nothing to present to (SDL3 silently drops) */
    }
    if (rects && numrects > 0) {
        for (int i = 0; i < numrects; i++) {
            int x = rects[i].x < 0 ? 0 : rects[i].x;
            int y = rects[i].y < 0 ? 0 : rects[i].y;
            int r = rects[i].x + rects[i].w > wd->fb_w ? wd->fb_w : rects[i].x + rects[i].w;
            int b = rects[i].y + rects[i].h > wd->fb_h ? wd->fb_h : rects[i].y + rects[i].h;
            if (r > x && b > y) {
                SDLOP_X11_XPutImage(x11_data.display, wd->xwindow, wd->gc, wd->ximage,
                                    x, y, x, y, (unsigned int)(r - x), (unsigned int)(b - y));
            }
        }
    } else {
        SDLOP_X11_XPutImage(x11_data.display, wd->xwindow, wd->gc, wd->ximage,
                            0, 0, 0, 0, (unsigned int)wd->fb_w, (unsigned int)wd->fb_h);
    }
    SDLOP_X11_XFlush(x11_data.display);
    return true;
}

static void x11_DestroyWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (wd) {
        x11_destroy_ximage(wd);
    }
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* EWMH helpers                                                        */
/* ------------------------------------------------------------------ */

static void x11_ewmh_state_message(SDL_Window *window, int action, SDLOP_Atom a1, SDLOP_Atom a2)
{
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    SDLOP_XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = SDLOP_ClientMessage;
    ev.xclient.window = wd->xwindow;
    ev.xclient.message_type = x11_data.net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = action;
    ev.xclient.data.l[1] = (long)a1;
    ev.xclient.data.l[2] = (long)a2;
    ev.xclient.data.l[3] = 1; /* source indication: application */
    SDLOP_X11_XSendEvent(x11_data.display, root_window(), SDLOP_False,
                         SDLOP_SubstructureNotifyMask | SDLOP_SubstructureRedirectMask, &ev);
    SDLOP_X11_XFlush(x11_data.display);
}

static void x11_update_size_hints(SDL_Window *window)
{
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    SDLOP_XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    if (!(window->flags & SDL_WINDOW_RESIZABLE)) {
        /* non-resizable: pin min == max (ICCCM) */
        hints.flags = SDLOP_PMinSize | SDLOP_PMaxSize;
        hints.min_width = hints.max_width = window->w;
        hints.min_height = hints.max_height = window->h;
    } else {
        hints.flags = SDLOP_PMinSize;
        hints.min_width = 1;
        hints.min_height = 1;
    }
    SDLOP_X11_XSetWMNormalHints(x11_data.display, wd->xwindow, &hints);
}

/* parse _NET_WM_STATE and sync window flags/events (SDL3 semantics) */
static void x11_sync_wm_state(SDL_Window *window)
{
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    unsigned char *data = NULL;
    SDLOP_Atom actual_type = SDLOP_None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;

    bool fullscreen = false, maximized = false, hidden = false;

    if (SDLOP_X11_XGetWindowProperty(x11_data.display, wd->xwindow, x11_data.net_wm_state,
                                     0, 64, SDLOP_False, SDLOP_XA_ATOM,
                                     &actual_type, &actual_format, &nitems, &bytes_after, &data) == 0 && data) {
        if (actual_format == 32) {
            const unsigned long *atoms = (const unsigned long *)data;
            for (unsigned long i = 0; i < nitems; i++) {
                if ((SDLOP_Atom)atoms[i] == x11_data.net_wm_state_fullscreen) {
                    fullscreen = true;
                } else if ((SDLOP_Atom)atoms[i] == x11_data.net_wm_state_maximized_vert ||
                           (SDLOP_Atom)atoms[i] == x11_data.net_wm_state_maximized_horz) {
                    maximized = true; /* vert||horz: treat either as maximized-ish; both checked below */
                } else if ((SDLOP_Atom)atoms[i] == x11_data.net_wm_state_hidden) {
                    hidden = true;
                }
            }
        }
        SDLOP_X11_XFree(data);
    }

    if (fullscreen && !(window->flags & SDL_WINDOW_FULLSCREEN)) {
        window->flags |= SDL_WINDOW_FULLSCREEN;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_ENTER_FULLSCREEN, 0, 0);
    } else if (!fullscreen && (window->flags & SDL_WINDOW_FULLSCREEN)) {
        window->flags &= ~SDL_WINDOW_FULLSCREEN;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 0, 0);
    }
    if (maximized && !(window->flags & SDL_WINDOW_MAXIMIZED)) {
        window->flags |= SDL_WINDOW_MAXIMIZED;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MAXIMIZED, 0, 0);
    } else if (!maximized && (window->flags & SDL_WINDOW_MAXIMIZED)) {
        window->flags &= ~SDL_WINDOW_MAXIMIZED;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESTORED, 0, 0);
    }
    (void)hidden; /* iconify is reported via UnmapNotify */
}

/* ------------------------------------------------------------------ */
/* event translation                                                   */
/* ------------------------------------------------------------------ */

static Uint8 x11_button_to_sdl(unsigned int button)
{
    switch (button) {
    case SDLOP_Button1: return SDL_BUTTON_LEFT;
    case SDLOP_Button2: return SDL_BUTTON_MIDDLE;
    case SDLOP_Button3: return SDL_BUTTON_RIGHT;
    case 8: return SDL_BUTTON_X1;
    case 9: return SDL_BUTTON_X2;
    default: return 0;
    }
}

static void x11_handle_key(SDL_Window *window, SDLOP_XEvent *ev, bool down)
{
    (void)window; /* key events go to the focused window tracked by the core */
    if (SDLOP_RawInputKeyboardActive()) {
        return; /* evdev worker owns the keyboard (same policy as wayland) */
    }
    unsigned int keycode = ev->xkey.keycode;
    if (keycode < 8 || keycode > 255) {
        return;
    }
    Uint16 evdev = (Uint16)(keycode - 8); /* X11 keycodes are evdev + 8 */

    if (!down) {
        /* X11 server auto-repeat emits release+press pairs with the same
         * keycode and timestamp; detect the pair and report it as one
         * repeat press (SDL3 behavior). */
        if (SDLOP_X11_XEventsQueued(x11_data.display, SDLOP_QueuedAfterReading) > 0) {
            SDLOP_XEvent next;
            SDLOP_X11_XPeekEvent(x11_data.display, &next);
            if (next.type == SDLOP_KeyPress && next.xkey.keycode == keycode &&
                next.xkey.time == ev->xkey.time) {
                SDLOP_X11_XNextEvent(x11_data.display, &next); /* consume the press */
                SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
                if (evdev <= SDLOP_EVDEV_KEY_MAX) {
                    sc = (SDL_Scancode)sdlop_evdev_to_scancode[evdev];
                }
                if (sc != SDL_SCANCODE_UNKNOWN) {
                    SDLOP_SendKeyboardKey(true, true, sc, evdev, SDLOP_MonotonicNS());
                    /* the server is driving repeats: suppress the core's
                     * own repeat emulation for this press */
                    sdlop.repeat_active = false;
                }
                return;
            }
        }
    }

    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    if (evdev <= SDLOP_EVDEV_KEY_MAX) {
        sc = (SDL_Scancode)sdlop_evdev_to_scancode[evdev];
    }
    if (sc != SDL_SCANCODE_UNKNOWN) {
        SDLOP_SendKeyboardKey(down, false, sc, evdev, SDLOP_MonotonicNS());
    }
}

static void x11_handle_motion(SDL_Window *window, SDLOP_XEvent *ev)
{
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    float x = (float)ev->xmotion.x;
    float y = (float)ev->xmotion.y;

    if (wd->relative_mode) {
        /* swallow the motion event generated by our own recentering warp */
        if (wd->warp_pending && ev->xmotion.x == wd->warp_center_x && ev->xmotion.y == wd->warp_center_y) {
            wd->warp_pending = false;
            return;
        }
        float dx = x - (float)wd->warp_center_x;
        float dy = y - (float)wd->warp_center_y;
        if (dx == 0.0f && dy == 0.0f) {
            return;
        }
        /* recenter when approaching the window edge (grab confines the
         * pointer; without recentring the deltas would clamp out) */
        int margin_x = window->w / 4 > 16 ? window->w / 4 : 16;
        int margin_y = window->h / 4 > 16 ? window->h / 4 : 16;
        if (ev->xmotion.x < margin_x || ev->xmotion.x > window->w - margin_x ||
            ev->xmotion.y < margin_y || ev->xmotion.y > window->h - margin_y) {
            wd->warp_center_x = window->w / 2;
            wd->warp_center_y = window->h / 2;
            wd->warp_pending = true;
            SDLOP_X11_XWarpPointer(x11_data.display, SDLOP_None, wd->xwindow, 0, 0, 0, 0,
                                   wd->warp_center_x, wd->warp_center_y);
        } else {
            wd->warp_center_x = ev->xmotion.x;
            wd->warp_center_y = ev->xmotion.y;
        }
        if (!SDLOP_RawInputMouseActive()) {
            SDLOP_SendMouseMotion(SDLOP_NO_POS, SDLOP_NO_POS, dx, dy, SDLOP_MonotonicNS());
        }
        return;
    }

    if (SDLOP_RawInputMouseActive()) {
        /* evdev owns motion: re-anchor without emitting (wayland policy) */
        sdlop.mouse_x = x;
        sdlop.mouse_y = y;
        return;
    }
    SDLOP_SendMouseMotion(x, y, 0.0f, 0.0f, SDLOP_MonotonicNS());
}

static void x11_handle_crossing(SDL_Window *window, SDLOP_XEvent *ev, bool enter)
{
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    /* ignore grab bookkeeping crossings and crossings between our window
     * and its own children (we have none, but override-redirect tooltips
     * of the WM could report NotifyInferior) */
    if (ev->xcrossing.mode == SDLOP_NotifyGrab || ev->xcrossing.mode == SDLOP_NotifyUngrab) {
        return;
    }
    if (ev->xcrossing.detail == SDLOP_NotifyInferior) {
        return;
    }
    if (enter) {
        sdlop.mouse_focus = window;
        window->flags |= SDL_WINDOW_MOUSE_FOCUS;
        sdlop.mouse_has_position = false;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_ENTER, 0, 0);
        if (!wd->relative_mode) {
            SDLOP_SendMouseMotion((float)ev->xcrossing.x, (float)ev->xcrossing.y, 0.0f, 0.0f, SDLOP_MonotonicNS());
        }
    } else {
        if (sdlop.mouse_focus == window) {
            sdlop.mouse_focus = NULL;
        }
        sdlop.mouse_has_position = false;
        window->flags &= ~SDL_WINDOW_MOUSE_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_LEAVE, 0, 0);
    }
}

static void x11_handle_focus(SDL_Window *window, SDLOP_XEvent *ev, bool gained)
{
    if (ev->xfocus.mode == SDLOP_NotifyGrab || ev->xfocus.mode == SDLOP_NotifyUngrab) {
        return;
    }
    if (ev->xfocus.detail == SDLOP_NotifyPointer || ev->xfocus.detail == SDLOP_NotifyDetailNone) {
        return;
    }
    if (gained) {
        if (sdlop.keyboard_focus && sdlop.keyboard_focus != window) {
            sdlop.keyboard_focus->flags &= ~SDL_WINDOW_INPUT_FOCUS;
            SDLOP_SendWindowEvent(sdlop.keyboard_focus, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
        }
        sdlop.keyboard_focus = window;
        window->flags |= SDL_WINDOW_INPUT_FOCUS;
        x11_sync_modifiers();
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_FOCUS_GAINED, 0, 0);
    } else {
        if (sdlop.keyboard_focus == window) {
            sdlop.keyboard_focus = NULL;
        }
        window->flags &= ~SDL_WINDOW_INPUT_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
        SDL_ResetKeyboard();
    }
}

static void x11_handle_configure(SDL_Window *window, SDLOP_XEvent *ev)
{
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    int x = ev->xconfigure.x;
    int y = ev->xconfigure.y;
    int w = ev->xconfigure.width;
    int h = ev->xconfigure.height;

    /* for reparented windows only synthetic ConfigureNotify events carry
     * root coordinates (ICCCM); translate otherwise */
    if (!ev->xconfigure.send_event && wd->reparented) {
        int rx = 0, ry = 0;
        SDLOP_Window child = SDLOP_None;
        if (SDLOP_X11_XTranslateCoordinates(x11_data.display, wd->xwindow, root_window(), 0, 0, &rx, &ry, &child)) {
            x = rx;
            y = ry;
        }
    }

    if (x != window->x || y != window->y) {
        window->x = x;
        window->y = y;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOVED, x, y);
    }
    bool awaited = wd->size_request_pending && w == wd->pending_w && h == wd->pending_h;
    if (awaited || (w == window->w && h == window->h && wd->size_request_pending)) {
        wd->size_request_pending = false; /* any matching/explicit answer clears it */
    }
    if (w != window->w || h != window->h || awaited) {
        window->w = w;
        window->h = h;
        /* keep an existing software framebuffer in sync (SDL3 semantics:
         * the SDL_Surface object survives resizes) */
        if (window->surface) {
            SDL_Surface *s = window->surface;
            void *pixels = calloc(1, (size_t)w * 4u * (size_t)h);
            if (pixels) {
                free(s->pixels);
                s->pixels = pixels;
                s->w = w;
                s->h = h;
                s->pitch = w * 4;
                x11_destroy_ximage(wd);
                x11_create_ximage(window);
            } else {
                x11_destroy_ximage(wd);
            }
        }
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, w, h);
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, w, h);
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_SAFE_AREA_CHANGED, 0, 0);
    }
}

static void x11_handle_client_message(SDL_Window *window, SDLOP_XEvent *ev)
{
    if (ev->xclient.message_type == x11_data.wm_protocols) {
        SDLOP_Atom protocol = (SDLOP_Atom)ev->xclient.data.l[0];
        if (protocol == x11_data.wm_delete_window) {
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
        } else if (protocol == x11_data.net_wm_ping) {
            /* EWMH: answer pings by forwarding the message to the root
             * window (proves the client is alive) */
            ev->xclient.window = root_window();
            SDLOP_X11_XSendEvent(x11_data.display, root_window(), SDLOP_False,
                                 SDLOP_SubstructureNotifyMask | SDLOP_SubstructureRedirectMask, ev);
        }
    }
}

void SDLOP_X11_DispatchEvent(SDLOP_XEvent *ev)
{
    SDL_Window *window = NULL;

    switch (ev->type) {
    case SDLOP_KeyPress:
    case SDLOP_KeyRelease: {
        window = window_from_xwindow(ev->xkey.window);
        if (!window) {
            window = SDLOP_GetFocusedKeyboardWindow();
            if (!window) {
                return;
            }
        }
        x11_handle_key(window, ev, ev->type == SDLOP_KeyPress);
        break;
    }
    case SDLOP_ButtonPress:
    case SDLOP_ButtonRelease: {
        window = window_from_xwindow(ev->xbutton.window);
        if (!window) {
            return;
        }
        unsigned int b = ev->xbutton.button;
        if (b >= SDLOP_Button4 && b <= 7) {
            /* wheel: 4 up, 5 down, 6 left, 7 right (X11 convention);
             * only presses generate wheel events */
            if (ev->type == SDLOP_ButtonPress && !SDLOP_RawInputMouseActive()) {
                float wx = (b == 6) ? -1.0f : (b == 7) ? 1.0f : 0.0f;
                float wy = (b == SDLOP_Button4) ? 1.0f : (b == SDLOP_Button5) ? -1.0f : 0.0f;
                SDLOP_SendMouseWheel(wx, wy, SDLOP_MonotonicNS());
            }
            break;
        }
        Uint8 btn = x11_button_to_sdl(b);
        if (btn && !SDLOP_RawInputMouseActive()) {
            SDLOP_SendMouseButton(ev->type == SDLOP_ButtonPress, btn,
                                  (float)ev->xbutton.x, (float)ev->xbutton.y, SDLOP_MonotonicNS());
        }
        break;
    }
    case SDLOP_MotionNotify:
        window = window_from_xwindow(ev->xmotion.window);
        if (window) {
            x11_handle_motion(window, ev);
        }
        break;
    case SDLOP_EnterNotify:
    case SDLOP_LeaveNotify:
        window = window_from_xwindow(ev->xcrossing.window);
        if (window) {
            x11_handle_crossing(window, ev, ev->type == SDLOP_EnterNotify);
        }
        break;
    case SDLOP_FocusIn:
    case SDLOP_FocusOut:
        window = window_from_xwindow(ev->xfocus.window);
        if (window) {
            x11_handle_focus(window, ev, ev->type == SDLOP_FocusIn);
        }
        break;
    case SDLOP_Expose:
        window = window_from_xwindow(ev->xexpose.window);
        if (window && ev->xexpose.count == 0) {
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_EXPOSED, 0, 0);
        }
        break;
    case SDLOP_ConfigureNotify:
        window = window_from_xwindow(ev->xconfigure.window);
        if (window) {
            x11_handle_configure(window, ev);
        }
        break;
    case SDLOP_ReparentNotify:
        window = window_from_xwindow(ev->xreparent.window);
        if (window) {
            X11WindowData *wd = (X11WindowData *)window->driverdata;
            wd->reparented = (ev->xreparent.parent != root_window());
        }
        break;
    case SDLOP_MapNotify:
        window = window_from_xwindow(ev->xmap.window);
        if (window) {
            X11WindowData *wd = (X11WindowData *)window->driverdata;
            wd->mapped = true;
        }
        break;
    case SDLOP_UnmapNotify:
        window = window_from_xwindow(ev->xunmap.window);
        if (window) {
            X11WindowData *wd = (X11WindowData *)window->driverdata;
            wd->mapped = false;
            if (wd->expecting_unmap) {
                wd->expecting_unmap = false; /* our own HideWindow */
            } else if (!(window->flags & SDL_WINDOW_HIDDEN) &&
                       !(window->flags & SDL_WINDOW_MINIMIZED)) {
                /* WM iconified us */
                window->flags |= SDL_WINDOW_MINIMIZED;
                SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MINIMIZED, 0, 0);
            }
        }
        break;
    case SDLOP_DestroyNotify:
        /* our own XDestroyWindow arrives here after the SDL window is
         * already gone; external destroys are handled defensively */
        window = window_from_xwindow(ev->xdestroywindow.window);
        if (window) {
            X11WindowData *wd = (X11WindowData *)window->driverdata;
            if (wd) {
                wd->xwindow = SDLOP_None;
            }
        }
        break;
    case SDLOP_PropertyNotify:
        if (ev->xproperty.atom == x11_data.net_wm_state && ev->xproperty.state == SDLOP_PropertyNewValue) {
            window = window_from_xwindow(ev->xproperty.window);
            if (window) {
                x11_sync_wm_state(window);
            }
        }
        break;
    case SDLOP_ClientMessage:
        window = window_from_xwindow(ev->xclient.window);
        if (window) {
            x11_handle_client_message(window, ev);
        }
        break;
    case SDLOP_MappingNotify:
        if (ev->xmapping.request == SDLOP_MappingKeyboard ||
            ev->xmapping.request == SDLOP_MappingModifier) {
            /* the server's keyboard setup changed: rebuild the keymap
             * from the (possibly updated) rules property */
            x11_setup_keymap();
        }
        break;
    case SDLOP_VisibilityNotify:
        window = window_from_xwindow(ev->xvisibility.window);
        if (window) {
            bool occluded = (ev->xvisibility.state == SDLOP_VisibilityFullyObscured);
            if (occluded && !(window->flags & SDL_WINDOW_OCCLUDED)) {
                window->flags |= SDL_WINDOW_OCCLUDED;
                SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_OCCLUDED, 0, 0);
            } else if (!occluded && (window->flags & SDL_WINDOW_OCCLUDED)) {
                window->flags &= ~SDL_WINDOW_OCCLUDED;
                SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_EXPOSED, 0, 0);
            }
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* driver interface                                                    */
/* ------------------------------------------------------------------ */

static bool x11_Init(SDLop_VideoDevice *device)
{
    (void)device;
    memset(&x11_data, 0, sizeof(x11_data));

    const char *display_name = getenv("DISPLAY");
    if ((!display_name || !display_name[0]) && !getenv("SDL_VIDEODRIVER")) {
        return SDL_SetError("DISPLAY not set"); /* probing: fail quietly */
    }

    if (!SDLOP_X11_LoadSymbols()) {
        return false;
    }

    /* XOpenDisplay handles NULL ($DISPLAY) and reports unreachable or
     * garbage-speaking servers by returning NULL (no crash) */
    x11_data.display = SDLOP_X11_XOpenDisplay(display_name);
    if (!x11_data.display) {
        SDLOP_X11_UnloadSymbols();
        return SDL_SetError("Could not open X display %s (is an X server running?)",
                            display_name ? display_name : "(null)");
    }

    SDLOP_X11_XSetErrorHandler(x11_error_handler);
    SDLOP_X11_XSetIOErrorHandler(x11_ioerror_handler);

    x11_data.screen = SDLOP_X11_XDefaultScreen(x11_data.display);
    x11_data.root = SDLOP_X11_XRootWindow(x11_data.display, x11_data.screen);
    x11_data.display_width = SDLOP_X11_XDisplayWidth(x11_data.display, x11_data.screen);
    x11_data.display_height = SDLOP_X11_XDisplayHeight(x11_data.display, x11_data.screen);

    SDLOP_Display *dpy = x11_data.display;
#define SDLOP_INTERN(field, name) \
    x11_data.field = SDLOP_X11_XInternAtom(dpy, name, SDLOP_False)
    SDLOP_INTERN(wm_protocols, "WM_PROTOCOLS");
    SDLOP_INTERN(wm_delete_window, "WM_DELETE_WINDOW");
    SDLOP_INTERN(net_wm_ping, "_NET_WM_PING");
    SDLOP_INTERN(net_wm_pid, "_NET_WM_PID");
    SDLOP_INTERN(net_wm_name, "_NET_WM_NAME");
    SDLOP_INTERN(utf8_string, "UTF8_STRING");
    SDLOP_INTERN(net_wm_state, "_NET_WM_STATE");
    SDLOP_INTERN(net_wm_state_fullscreen, "_NET_WM_STATE_FULLSCREEN");
    SDLOP_INTERN(net_wm_state_maximized_vert, "_NET_WM_STATE_MAXIMIZED_VERT");
    SDLOP_INTERN(net_wm_state_maximized_horz, "_NET_WM_STATE_MAXIMIZED_HORZ");
    SDLOP_INTERN(net_wm_state_hidden, "_NET_WM_STATE_HIDDEN");
    SDLOP_INTERN(net_active_window, "_NET_ACTIVE_WINDOW");
    SDLOP_INTERN(xkb_rules_names, "_XKB_RULES_NAMES");
#undef SDLOP_INTERN

    /* XKB extension (in every X server since X11R6.1): used to resync
     * modifier state on focus changes */
    {
        int opcode = 0, ev_base = 0, err_base = 0, major = 1, minor = 0;
        if (SDLOP_X11_XkbQueryExtension(dpy, &opcode, &ev_base, &err_base, &major, &minor)) {
            x11_data.xkb_ext = true;
            x11_data.xkb_event_base = ev_base;
            x11_data.xkb_error_base = err_base;
        }
    }

    x11_create_cursors();

    /* display enumeration: one display per screen (X11 screens are
     * effectively single-display on any modern server; XRandR multi-head
     * enumeration is future work). Content scale from physical DPI. */
    float scale = 1.0f;
    int mm_w = SDLOP_X11_XDisplayWidthMM(dpy, x11_data.screen);
    if (mm_w > 0 && x11_data.display_width > 0) {
        float dpi = (float)x11_data.display_width * 25.4f / (float)mm_w;
        float s = dpi / 96.0f;
        /* quantize to 0.25 steps, clamp to a sane range */
        s = (float)((int)(s * 4.0f + 0.5f)) / 4.0f;
        if (s >= 1.0f && s <= 4.0f) {
            scale = s;
        }
    }
    char name[128];
    snprintf(name, sizeof(name), "X11 %s", SDLOP_X11_XDisplayString(dpy));
    SDL_DisplayID disp = SDLOP_AddDisplay(name, x11_data.display_width, x11_data.display_height, 0,
                                          SDL_ORIENTATION_LANDSCAPE, NULL, sdlop.init_done);
    if (disp) {
        int idx = SDLOP_GetDisplayIndexById(disp);
        if (idx >= 0) {
            sdlop.displays[idx].content_scale = scale;
        }
    }

    /* keyboard layout from the server's own rules (falls back to the
     * core default keymap when the property or xkbcommon is missing) */
    x11_setup_keymap();

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop X11: connected to %s (%s)",
                SDLOP_X11_XDisplayString(dpy), SDLOP_X11_XServerVendor(dpy));
    return true;
}

static void x11_Quit(SDLop_VideoDevice *device)
{
    (void)device;
    SDLOP_X11_GL_Quit(device);
    if (x11_data.display) {
        /* remaining displays (normally one) */
        while (sdlop.num_displays > 0) {
            SDLOP_DelDisplay(sdlop.displays[sdlop.num_displays - 1].id, false);
        }
        if (x11_data.default_cursor != SDLOP_None) {
            SDLOP_X11_XFreeCursor(x11_data.display, x11_data.default_cursor);
            x11_data.default_cursor = SDLOP_None;
        }
        if (x11_data.invisible_cursor != SDLOP_None) {
            SDLOP_X11_XFreeCursor(x11_data.display, x11_data.invisible_cursor);
            x11_data.invisible_cursor = SDLOP_None;
        }
        if (x11_data.invisible_pixmap != SDLOP_None) {
            SDLOP_X11_XFreePixmap(x11_data.display, x11_data.invisible_pixmap);
            x11_data.invisible_pixmap = SDLOP_None;
        }
        SDLOP_X11_XCloseDisplay(x11_data.display);
        x11_data.display = NULL;
    }
    memset(&x11_data, 0, sizeof(x11_data));
    SDLOP_X11_UnloadSymbols();
}

static bool x11_CreateWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLOP_Display *dpy = x11_data.display;
    int screen = x11_data.screen;

    X11WindowData *wd = (X11WindowData *)calloc(1, sizeof(*wd));
    if (!wd) {
        return SDL_OutOfMemory();
    }

    x11_create_cursors();

    /* visual: for GL windows try to get a GLX visual so a later
     * SDL_GL_CreateContext matches the window (SDL3 strategy); otherwise
     * the default visual is fine */
    SDLOP_Visual *visual = SDLOP_X11_XDefaultVisual(dpy, screen);
    int depth = SDLOP_X11_XDefaultDepth(dpy, screen);
    SDLOP_Colormap colormap = SDLOP_CopyFromParent;
    if (window->flags & SDL_WINDOW_OPENGL) {
        SDLOP_Colormap gl_colormap = SDLOP_CopyFromParent;
        if (SDLOP_X11_GL_ChooseVisual(window, &visual, &depth, &gl_colormap)) {
            colormap = gl_colormap;
        } else {
            SDL_ClearError(); /* GLX unavailable/absent: plain window, GL will fail later */
        }
    }

    long event_mask = SDLOP_KeyPressMask | SDLOP_KeyReleaseMask |
                      SDLOP_ButtonPressMask | SDLOP_ButtonReleaseMask |
                      SDLOP_PointerMotionMask |
                      SDLOP_EnterWindowMask | SDLOP_LeaveWindowMask |
                      SDLOP_ExposureMask | SDLOP_StructureNotifyMask |
                      SDLOP_FocusChangeMask | SDLOP_PropertyChangeMask |
                      SDLOP_VisibilityChangeMask;

    SDLOP_XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.event_mask = event_mask;
    attrs.background_pixel = SDLOP_X11_XBlackPixel(dpy, screen);
    attrs.border_pixel = SDLOP_X11_XBlackPixel(dpy, screen);
    unsigned long valuemask = SDLOP_CWEventMask | SDLOP_CWBackPixel | SDLOP_CWBorderPixel;
    if (colormap != SDLOP_CopyFromParent) {
        attrs.colormap = colormap;
        valuemask |= SDLOP_CWColormap;
    }
    if (x11_data.default_cursor != SDLOP_None) {
        attrs.cursor = x11_data.default_cursor;
        valuemask |= SDLOP_CWCursor;
    }

    wd->xwindow = SDLOP_X11_XCreateWindow(dpy, root_window(),
                                          window->x, window->y,
                                          (unsigned int)window->w, (unsigned int)window->h,
                                          0, depth, SDLOP_InputOutput, visual,
                                          valuemask, &attrs);
    if (wd->xwindow == SDLOP_None) {
        free(wd);
        return SDL_SetError("XCreateWindow failed");
    }
    window->driverdata = wd;
    SDLOP_X11_GL_CommitVisual(window); /* adopt a pending GLX visual */

    wd->gc = SDLOP_X11_XCreateGC(dpy, wd->xwindow, 0, NULL);

    /* title: ICCCM (XA_WM_NAME, latin-1) + EWMH (_NET_WM_NAME, UTF-8) */
    SDLOP_X11_XStoreName(dpy, wd->xwindow, window->title ? window->title : "SDLop");
    if (window->title && x11_data.utf8_string != SDLOP_None) {
        SDLOP_X11_XChangeProperty(dpy, wd->xwindow, x11_data.net_wm_name, x11_data.utf8_string,
                                  8, SDLOP_PropModeReplace,
                                  (const unsigned char *)window->title, (int)strlen(window->title));
    }
    {
        SDLOP_XClassHint class_hint;
        class_hint.res_name = (char *)"sdlop";
        class_hint.res_class = (char *)"SDLop";
        SDLOP_X11_XSetClassHint(dpy, wd->xwindow, &class_hint);
    }
    {
        /* _NET_WM_PID (EWMH, CARDINAL/32) */
        pid_t pid = getpid();
        SDLOP_X11_XChangeProperty(dpy, wd->xwindow, x11_data.net_wm_pid, SDLOP_XA_CARDINAL,
                                  32, SDLOP_PropModeReplace, (const unsigned char *)&pid, 1);
    }

    /* window manager protocols: graceful close + ping */
    SDLOP_Atom protocols[2] = { x11_data.wm_delete_window, x11_data.net_wm_ping };
    SDLOP_X11_XSetWMProtocols(dpy, wd->xwindow, protocols, 2);

    x11_update_size_hints(window);

    /* initial EWMH state (pre-map property setting) */
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        SDLOP_X11_XChangeProperty(dpy, wd->xwindow, x11_data.net_wm_state, SDLOP_XA_ATOM,
                                  32, SDLOP_PropModeReplace,
                                  (const unsigned char *)&x11_data.net_wm_state_fullscreen, 1);
    }
    if (window->flags & SDL_WINDOW_MAXIMIZED) {
        SDLOP_Atom max_atoms[2] = { x11_data.net_wm_state_maximized_horz, x11_data.net_wm_state_maximized_vert };
        SDLOP_X11_XChangeProperty(dpy, wd->xwindow, x11_data.net_wm_state, SDLOP_XA_ATOM,
                                  32, SDLOP_PropModeReplace,
                                  (const unsigned char *)max_atoms, 2);
    }

    SDLOP_X11_XFlush(dpy);
    return true;
}

static void x11_DestroyWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd) {
        return;
    }
    if (wd->relative_mode) {
        x11_release_relative_mode(window);
    }
    SDLOP_X11_GL_WindowDestroyed(window);
    x11_destroy_ximage(wd);
    if (x11_data.display && !x11_data.dead) {
        if (wd->colormap != SDLOP_None) {
            SDLOP_X11_XFreeColormap(x11_data.display, wd->colormap);
            wd->colormap = SDLOP_None;
        }
        if (wd->gc) {
            SDLOP_X11_XFreeGC(x11_data.display, wd->gc);
            wd->gc = NULL;
        }
        if (wd->xwindow != SDLOP_None) {
            SDLOP_X11_XDestroyWindow(x11_data.display, wd->xwindow);
        }
        SDLOP_X11_XFlush(x11_data.display);
    }
    free(wd);
    window->driverdata = NULL;
}

static void x11_ShowWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || wd->xwindow == SDLOP_None) {
        return;
    }
    SDLOP_X11_XMapRaised(x11_data.display, wd->xwindow);
    SDLOP_X11_XFlush(x11_data.display);
}

static void x11_HideWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || wd->xwindow == SDLOP_None) {
        return;
    }
    if (wd->relative_mode) {
        x11_release_relative_mode(window);
    }
    wd->expecting_unmap = true;
    SDLOP_X11_XUnmapWindow(x11_data.display, wd->xwindow);
    SDLOP_X11_XFlush(x11_data.display);
}

static bool x11_SetWindowTitle(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || wd->xwindow == SDLOP_None) {
        return SDL_SetError("Invalid window");
    }
    SDLOP_Display *dpy = x11_data.display;
    SDLOP_X11_XStoreName(dpy, wd->xwindow, window->title ? window->title : "");
    if (window->title && x11_data.utf8_string != SDLOP_None) {
        SDLOP_X11_XChangeProperty(dpy, wd->xwindow, x11_data.net_wm_name, x11_data.utf8_string,
                                  8, SDLOP_PropModeReplace,
                                  (const unsigned char *)window->title, (int)strlen(window->title));
    }
    SDLOP_X11_XFlush(dpy);
    return true;
}

static bool x11_SetWindowSize(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || wd->xwindow == SDLOP_None) {
        return SDL_SetError("Invalid window");
    }
    x11_update_size_hints(window);
    wd->pending_w = window->w;
    wd->pending_h = window->h;
    wd->size_request_pending = true;
    SDLOP_X11_XResizeWindow(x11_data.display, wd->xwindow,
                            (unsigned int)window->w, (unsigned int)window->h);
    SDLOP_X11_XFlush(x11_data.display);
    /* the RESIZED event arrives with the server's ConfigureNotify */
    return true;
}

static bool x11_SetWindowPosition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || wd->xwindow == SDLOP_None) {
        return SDL_SetError("Invalid window");
    }
    SDLOP_X11_XMoveWindow(x11_data.display, wd->xwindow, window->x, window->y);
    SDLOP_X11_XFlush(x11_data.display);
    return true;
}

static void x11_MinimizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || wd->xwindow == SDLOP_None) {
        return;
    }
    /* ICCCM: XIconifyWindow posts WM_CHANGE_STATE to the root window */
    SDLOP_X11_XIconifyWindow(x11_data.display, wd->xwindow, x11_data.screen);
    SDLOP_X11_XFlush(x11_data.display);
}

static void x11_MaximizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (!window->driverdata) {
        return;
    }
    x11_ewmh_state_message(window, NET_WM_STATE_ADD,
                           x11_data.net_wm_state_maximized_horz,
                           x11_data.net_wm_state_maximized_vert);
    /* a WM-less server never answers: apply the flag immediately so the
     * state query stays truthful (SDL3 reports the request) */
    if (!(window->flags & SDL_WINDOW_MAXIMIZED)) {
        window->flags |= SDL_WINDOW_MAXIMIZED;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MAXIMIZED, 0, 0);
    }
}

static void x11_RestoreWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd) {
        return;
    }
    if (window->flags & SDL_WINDOW_MINIMIZED) {
        /* de-iconify */
        window->flags &= ~SDL_WINDOW_MINIMIZED;
        SDLOP_X11_XMapRaised(x11_data.display, wd->xwindow);
        SDLOP_X11_XFlush(x11_data.display);
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESTORED, 0, 0);
        return;
    }
    x11_ewmh_state_message(window, NET_WM_STATE_REMOVE,
                           x11_data.net_wm_state_maximized_horz,
                           x11_data.net_wm_state_maximized_vert);
    x11_ewmh_state_message(window, NET_WM_STATE_REMOVE,
                           x11_data.net_wm_state_fullscreen, SDLOP_None);
    if (window->flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) {
        bool was_fs = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
        window->flags &= ~(SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN);
        if (was_fs) {
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 0, 0);
        }
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESTORED, 0, 0);
    }
}

static bool x11_SetWindowFullscreen(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || wd->xwindow == SDLOP_None) {
        return SDL_SetError("Invalid window");
    }
    bool want = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
    x11_ewmh_state_message(window, want ? NET_WM_STATE_ADD : NET_WM_STATE_REMOVE,
                           x11_data.net_wm_state_fullscreen, SDLOP_None);
    /* Under a real WM the PropertyNotify/ConfigureNotify round updates
     * flags + events; on a WM-less server nothing answers, so report the
     * transition directly (keeps flag/event pairing consistent). */
    if (want) {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_ENTER_FULLSCREEN, 0, 0);
    } else {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 0, 0);
    }
    return true;
}

static void x11_RaiseWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || wd->xwindow == SDLOP_None) {
        return;
    }
    SDLOP_X11_XRaiseWindow(x11_data.display, wd->xwindow);
    SDLOP_X11_XFlush(x11_data.display);
}

static void x11_SetWindowClearColor(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || !window->surface || !window->surface->pixels) {
        return;
    }
    Uint32 color = ((Uint32)window->clear_r << 16) | ((Uint32)window->clear_g << 8) | window->clear_b;
    Uint32 *pix = (Uint32 *)window->surface->pixels;
    size_t npix = (size_t)window->surface->w * (size_t)window->surface->h;
    for (size_t i = 0; i < npix; i++) {
        pix[i] = color;
    }
    if (wd->ximage && wd->mapped && !wd->gl_active) {
        SDLOP_X11_XPutImage(x11_data.display, wd->xwindow, wd->gc, wd->ximage,
                            0, 0, 0, 0, (unsigned int)wd->fb_w, (unsigned int)wd->fb_h);
        SDLOP_X11_XFlush(x11_data.display);
    }
}

static bool x11_SetWindowRelativeMouseMode(SDLop_VideoDevice *device, SDL_Window *window, bool enabled)
{
    (void)device;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    if (enabled) {
        return x11_arm_relative_mode(window);
    }
    x11_release_relative_mode(window);
    return true;
}

static void x11_PumpEvents(SDLop_VideoDevice *device, int timeout_ms)
{
    (void)device;
    SDLOP_Display *dpy = x11_data.display;
    if (!dpy || x11_data.dead) {
        return;
    }

    /* optional blocking wait (used by SDL_WaitEvent) */
    if (timeout_ms >= 0 && SDLOP_X11_XPending(dpy) == 0) {
        struct pollfd pfd = { SDLOP_X11_XConnectionNumber(dpy), POLLIN, 0 };
        poll(&pfd, 1, timeout_ms);
    }

    while (SDLOP_X11_XPending(dpy) > 0) {
        SDLOP_XEvent ev;
        SDLOP_X11_XNextEvent(dpy, &ev);
        if (getenv("SDLOP_X11_DEBUG_EVENTS")) {
            fprintf(stderr, "[x11dbg] event type=%d win=0x%lx\n", ev.type, (unsigned long)ev.xany.window);
        }
        SDLOP_X11_DispatchEvent(&ev);
    }
    SDLOP_X11_XFlush(dpy);

    SDLOP_KeyboardProcessRepeats();
}

static int x11_GetEventFD(SDLop_VideoDevice *device)
{
    (void)device;
    return x11_data.display ? SDLOP_X11_XConnectionNumber(x11_data.display) : -1;
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

    /* software surface */
    x11_CreateWindowFramebuffer,
    x11_UpdateWindowFramebuffer,
    x11_DestroyWindowFramebuffer,

    /* OpenGL (GLX) - implemented in sdlop_x11_glx.c */
    SDLOP_X11_GL_CreateContext,
    SDLOP_X11_GL_MakeCurrent,
    SDLOP_X11_GL_SwapBuffers,
    SDLOP_X11_GL_DeleteContext,
    SDLOP_X11_GL_GetProcAddressThunk,
    SDLOP_X11_GL_SetSwapInterval,
    SDLOP_X11_GL_GetSwapInterval,

    /* Vulkan (VK_KHR_xlib_surface) - implemented in sdlop_x11_vulkan.c */
    SDLOP_X11_Vulkan_CreateSurface,
    SDLOP_X11_Vulkan_GetInstanceExtensions,
};
