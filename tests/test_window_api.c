/*
  SDLop test: SDL 3.4 window-management, cursor, grab and device-list API.

  Runs on the dummy driver by default (pure core semantics: validation,
  clamping, flags, events) and, as test_window_api_x11, against a real X
  server where the X11 side is verified from a second xcb connection
  (WM_NORMAL_HINTS, _NET_WM_* properties, override-redirect popups,
  transient-for, pointer grabs and warps). Skips the X11 part cleanly
  without DISPLAY.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SDLOP_TEST_HAVE_XCB
#include <xcb/xcb.h>
#endif

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

static bool is_x11 = false;
static bool is_wayland = false;

/* Wayland has no protocol for some requests; stock SDL3 reports
 * SDL_Unsupported() there and so does SDLop */
#define CHECK_OR_UNSUPPORTED(expr, ...)                                              \
    do {                                                                             \
        if (is_wayland) {                                                            \
            bool r_ = (expr);                                                        \
            CHECK(!r_ && strstr(SDL_GetError(), "not supported"), __VA_ARGS__);      \
        } else {                                                                     \
            CHECK((expr), __VA_ARGS__);                                              \
        }                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* X11 observer                                                        */
/* ------------------------------------------------------------------ */

#ifdef SDLOP_TEST_HAVE_XCB
static xcb_connection_t *obs;

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(obs, xcb_intern_atom(obs, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

/* returns item count (32-bit items), -1 if the property is absent */
static int get_prop32(xcb_window_t w, xcb_atom_t prop, uint32_t *out, int max)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        obs, xcb_get_property(obs, 0, w, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, (uint32_t)max), NULL);
    if (!r) {
        return -1;
    }
    int n = -1;
    if (r->type != XCB_NONE && r->format == 32) {
        n = xcb_get_property_value_length(r) / 4;
        if (n > max) {
            n = max;
        }
        memcpy(out, xcb_get_property_value(r), (size_t)n * 4);
    }
    free(r);
    return n;
}

static bool has_atom(xcb_window_t w, xcb_atom_t prop, xcb_atom_t value)
{
    uint32_t v[32];
    int n = get_prop32(w, prop, v, 32);
    for (int i = 0; i < n; ++i) {
        if (v[i] == value) {
            return true;
        }
    }
    return false;
}

static xcb_window_t xid_of(SDL_Window *window)
{
    return (xcb_window_t)SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
}

static void obs_sync(void)
{
    SDL_SyncWindow(NULL);
    free(xcb_get_input_focus_reply(obs, xcb_get_input_focus(obs), NULL));
}

static void query_pointer(xcb_window_t w, int *x, int *y)
{
    xcb_query_pointer_reply_t *r = xcb_query_pointer_reply(obs, xcb_query_pointer(obs, w), NULL);
    *x = r ? r->win_x : -1;
    *y = r ? r->win_y : -1;
    free(r);
}

/* can another client grab the pointer? (fails while SDLop holds a grab) */
static bool pointer_grabbable(void)
{
    const xcb_setup_t *setup = xcb_get_setup(obs);
    xcb_window_t root = xcb_setup_roots_iterator(setup).data->root;
    xcb_grab_pointer_reply_t *r = xcb_grab_pointer_reply(
        obs, xcb_grab_pointer(obs, 0, root, 0, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
                              XCB_CURRENT_TIME),
        NULL);
    bool ok = r && r->status == XCB_GRAB_STATUS_SUCCESS;
    free(r);
    if (ok) {
        xcb_ungrab_pointer(obs, XCB_CURRENT_TIME);
        xcb_flush(obs);
    }
    return ok;
}
#endif

/* ------------------------------------------------------------------ */

static void pump(int ms)
{
    Uint64 end = SDL_GetTicks() + (Uint64)ms;
    SDL_Event ev;
    do {
        while (SDL_PollEvent(&ev)) {
        }
        SDL_Delay(5);
    } while (SDL_GetTicks() < end);
}

static void test_create_validation(void)
{
    SDL_Window *w = SDL_CreateWindow("bad", 100, 100, SDL_WINDOW_UTILITY | SDL_WINDOW_MODAL);
    CHECK(!w, "conflicting type flags rejected");
    w = SDL_CreateWindow("bad", 100, 100, SDL_WINDOW_MODAL);
    CHECK(!w && strstr(SDL_GetError(), "parent"), "modal without parent rejected: %s", SDL_GetError());
    w = SDL_CreatePopupWindow(NULL, 0, 0, 10, 10, SDL_WINDOW_TOOLTIP);
    CHECK(!w, "popup without parent rejected");
    w = SDL_CreateWindow("bad", 100, 100, SDL_WINDOW_OPENGL | SDL_WINDOW_VULKAN);
    CHECK(!w, "conflicting graphics flags rejected");

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "props window");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 320);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 200);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_ALWAYS_ON_TOP_BOOLEAN, true);
    w = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    CHECK(w, "CreateWindowWithProperties: %s", SDL_GetError());
    if (w) {
        SDL_WindowFlags f = SDL_GetWindowFlags(w);
        CHECK(f & SDL_WINDOW_HIDDEN, "hidden flag");
        CHECK(f & SDL_WINDOW_RESIZABLE, "resizable flag");
        CHECK(f & SDL_WINDOW_ALWAYS_ON_TOP, "always-on-top flag");
        CHECK(strcmp(SDL_GetWindowTitle(w), "props window") == 0, "title");
        int ww = 0, hh = 0;
        SDL_GetWindowSize(w, &ww, &hh);
        CHECK(ww == 320 && hh == 200, "size %dx%d", ww, hh);
        SDL_DestroyWindow(w);
    }
}

static void test_size_limits(SDL_Window *w)
{
    CHECK(SDL_SetWindowMinimumSize(w, 200, 150), "min size: %s", SDL_GetError());
    CHECK(!SDL_SetWindowMaximumSize(w, 100, 100), "max < min rejected");
    CHECK(SDL_SetWindowMaximumSize(w, 800, 600), "max size: %s", SDL_GetError());
    int a = 0, b = 0;
    SDL_GetWindowMinimumSize(w, &a, &b);
    CHECK(a == 200 && b == 150, "min readback %dx%d", a, b);
    SDL_GetWindowMaximumSize(w, &a, &b);
    CHECK(a == 800 && b == 600, "max readback %dx%d", a, b);

    /* requests are clamped by the core (stock order) */
    SDL_SetWindowSize(w, 50, 50);
    SDL_SyncWindow(w);
    pump(50);
    SDL_GetWindowSize(w, &a, &b);
    CHECK(a >= 200 && b >= 150, "clamped to min: %dx%d", a, b);

#ifdef SDLOP_TEST_HAVE_XCB
    if (is_x11) {
        uint32_t h[18];
        obs_sync();
        int n = get_prop32(xid_of(w), XCB_ATOM_WM_NORMAL_HINTS, h, 18);
        CHECK(n == 18, "WM_NORMAL_HINTS present");
        if (n == 18) {
            CHECK((h[0] & (1u << 4)) && h[5] == 200 && h[6] == 150, "PMinSize 200x150 (flags %x, %ux%u)", h[0], h[5], h[6]);
            CHECK((h[0] & (1u << 5)) && h[7] == 800 && h[8] == 600, "PMaxSize 800x600 (%ux%u)", h[7], h[8]);
        }
        CHECK(SDL_SetWindowAspectRatio(w, 1.0f, 2.0f), "aspect: %s", SDL_GetError());
        obs_sync();
        n = get_prop32(xid_of(w), XCB_ATOM_WM_NORMAL_HINTS, h, 18);
        CHECK(n == 18 && (h[0] & (1u << 7)) && h[11] == h[12] && h[13] == 2 * h[14], "PAspect 1..2");
    }
#endif
    float lo = 0, hi = 0;
    SDL_SetWindowAspectRatio(w, 1.0f, 2.0f);
    SDL_GetWindowAspectRatio(w, &lo, &hi);
    CHECK(lo == 1.0f && hi == 2.0f, "aspect readback");
    SDL_SetWindowAspectRatio(w, 0.0f, 0.0f);
    SDL_SetWindowMinimumSize(w, 0, 0);
    SDL_SetWindowMaximumSize(w, 0, 0);
}

static void test_state(SDL_Window *w)
{
    CHECK(SDL_SetWindowBordered(w, false), "bordered: %s", SDL_GetError());
    CHECK(SDL_GetWindowFlags(w) & SDL_WINDOW_BORDERLESS, "borderless flag");
    CHECK(SDL_SetWindowBordered(w, true), "bordered");
    CHECK(!(SDL_GetWindowFlags(w) & SDL_WINDOW_BORDERLESS), "bordered flag");

    CHECK(SDL_SetWindowResizable(w, false), "resizable: %s", SDL_GetError());
    CHECK(!(SDL_GetWindowFlags(w) & SDL_WINDOW_RESIZABLE), "not resizable");
    CHECK(SDL_SetWindowResizable(w, true), "resizable");

    CHECK_OR_UNSUPPORTED(SDL_SetWindowAlwaysOnTop(w, true), "on top: %s", SDL_GetError());
    if (!is_wayland) {
        CHECK(SDL_GetWindowFlags(w) & SDL_WINDOW_ALWAYS_ON_TOP, "on-top flag");
    }

    CHECK_OR_UNSUPPORTED(SDL_SetWindowOpacity(w, 0.5f), "opacity: %s", SDL_GetError());
    if (!is_wayland) {
        CHECK(SDL_fabsf(SDL_GetWindowOpacity(w) - 0.5f) < 0.01f, "opacity readback %f", SDL_GetWindowOpacity(w));
    }

    SDL_Surface *icon = SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_RGBA32);
    SDL_FillSurfaceRect(icon, NULL, SDL_MapSurfaceRGBA(icon, 255, 0, 0, 255));
    CHECK_OR_UNSUPPORTED(SDL_SetWindowIcon(w, icon), "icon: %s", SDL_GetError());
    SDL_DestroySurface(icon);

    int t = -1, l = -1, b = -1, r = -1;
    CHECK_OR_UNSUPPORTED(SDL_GetWindowBordersSize(w, &t, &l, &b, &r), "borders: %s", SDL_GetError());
    CHECK(t >= 0 && l >= 0 && b >= 0 && r >= 0, "borders non-negative");

    CHECK(SDL_FlashWindow(w, SDL_FLASH_BRIEFLY), "flash: %s", SDL_GetError());
    CHECK(SDL_FlashWindow(w, SDL_FLASH_CANCEL), "flash cancel");

    CHECK(SDL_SetWindowFocusable(w, false), "focusable: %s", SDL_GetError());
    if (!is_wayland) { /* no protocol: the flag stays (stock) */
        CHECK(SDL_GetWindowFlags(w) & SDL_WINDOW_NOT_FOCUSABLE, "not-focusable flag");
    }
    CHECK(SDL_SetWindowFocusable(w, true), "focusable");

#ifdef SDLOP_TEST_HAVE_XCB
    if (is_x11) {
        SDL_SetWindowFocusable(w, false);
        obs_sync();
        xcb_window_t x = xid_of(w);
        uint32_t v[9];
        CHECK(get_prop32(x, XCB_ATOM_WM_HINTS, v, 9) >= 2 && (v[0] & 1) && v[1] == 0, "WM_HINTS input=False");
        SDL_SetWindowFocusable(w, true);
        CHECK(get_prop32(x, atom("_NET_WM_WINDOW_OPACITY"), v, 1) == 1 && v[0] > 0x7f000000u && v[0] < 0x81000000u,
              "_NET_WM_WINDOW_OPACITY ~0.5");
        uint32_t ic[2 + 256];
        CHECK(get_prop32(x, atom("_NET_WM_ICON"), ic, 2 + 256) == 2 + 256 && ic[0] == 16 && ic[1] == 16 &&
                  ic[2] == 0xffff0000u,
              "_NET_WM_ICON 16x16 red");
        uint32_t mwm[5];
        CHECK(get_prop32(x, atom("_MOTIF_WM_HINTS"), mwm, 5) == 5 && mwm[2] == 1, "motif decorations back on");
    }
#endif
    SDL_SetWindowOpacity(w, 1.0f);
    SDL_SetWindowAlwaysOnTop(w, false);
}

static void test_parent_popup(SDL_Window *w)
{
    SDL_Window *tip = SDL_CreatePopupWindow(w, 10, 20, 80, 30, SDL_WINDOW_TOOLTIP);
    CHECK(tip, "tooltip: %s", SDL_GetError());
    if (!tip) {
        return;
    }
    CHECK(SDL_GetWindowParent(tip) == w, "popup parent");
    CHECK(SDL_GetWindowFlags(tip) & SDL_WINDOW_TOOLTIP, "tooltip flag");
    CHECK(!SDL_SetWindowBordered(tip, false), "popups reject SetWindowBordered");
    int px = 0, py = 0;
    SDL_GetWindowPosition(tip, &px, &py);
    CHECK(px == 10 && py == 20, "popup position is parent relative: %d,%d", px, py);
    CHECK(SDL_SetWindowPosition(tip, 15, 25), "move popup");
    SDL_GetWindowPosition(tip, &px, &py);
    CHECK(px == 15 && py == 25, "moved popup position: %d,%d", px, py);
#ifdef SDLOP_TEST_HAVE_XCB
    if (is_x11) {
        obs_sync();
        xcb_get_window_attributes_reply_t *a =
            xcb_get_window_attributes_reply(obs, xcb_get_window_attributes(obs, xid_of(tip)), NULL);
        CHECK(a && a->override_redirect, "tooltip is override-redirect");
        free(a);
        uint32_t tf = 0;
        CHECK(get_prop32(xid_of(tip), XCB_ATOM_WM_TRANSIENT_FOR, &tf, 1) == 1 && tf == xid_of(w),
              "tooltip WM_TRANSIENT_FOR parent");
        CHECK(has_atom(xid_of(tip), atom("_NET_WM_WINDOW_TYPE"), atom("_NET_WM_WINDOW_TYPE_TOOLTIP")),
              "tooltip window type");
    }
#endif

    SDL_Window *child = SDL_CreateWindow("child", 120, 90, SDL_WINDOW_HIDDEN);
    CHECK(child, "child");
    CHECK(!SDL_SetWindowParent(child, child), "self parent rejected");
    CHECK(SDL_SetWindowParent(child, w), "set parent: %s", SDL_GetError());
    CHECK(SDL_GetWindowParent(child) == w, "parent readback");
    CHECK_OR_UNSUPPORTED(SDL_SetWindowModal(child, true), "modal: %s", SDL_GetError()); /* xdg-dialog */
    if (!is_wayland) {
        CHECK(SDL_GetWindowFlags(child) & SDL_WINDOW_MODAL, "modal flag");
    }
#ifdef SDLOP_TEST_HAVE_XCB
    if (is_x11) {
        SDL_ShowWindow(child);
        obs_sync();
        uint32_t tf = 0;
        CHECK(get_prop32(xid_of(child), XCB_ATOM_WM_TRANSIENT_FOR, &tf, 1) == 1 && tf == xid_of(w),
              "modal WM_TRANSIENT_FOR parent");
    }
#endif
    /* destroying the parent destroys popups and children (stock) */
    SDL_WindowID tip_id = SDL_GetWindowID(tip), child_id = SDL_GetWindowID(child);
    SDL_DestroyWindow(w);
    CHECK(!SDL_GetWindowFromID(tip_id) && !SDL_GetWindowFromID(child_id), "children destroyed with parent");
}

static void test_grabs(SDL_Window *w)
{
    SDL_RaiseWindow(w);
    SDL_SyncWindow(w);
    pump(100);
    CHECK(SDL_SetWindowMouseGrab(w, true), "mouse grab: %s", SDL_GetError());
    pump(50);
    const bool focused = (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) != 0;
    if (focused) {
        CHECK(SDL_GetWindowMouseGrab(w), "mouse grab readback");
        CHECK(SDL_GetGrabbedWindow() == w, "grabbed window");
#ifdef SDLOP_TEST_HAVE_XCB
        if (is_x11) {
            CHECK(!pointer_grabbable(), "X pointer grabbed by SDLop");
        }
#endif
    }
    CHECK(SDL_SetWindowMouseGrab(w, false), "mouse ungrab");
    CHECK(!SDL_GetWindowMouseGrab(w), "mouse ungrab readback");
#ifdef SDLOP_TEST_HAVE_XCB
    if (is_x11) {
        CHECK(pointer_grabbable(), "X pointer released");
    }
#endif
    bool kg = SDL_SetWindowKeyboardGrab(w, true);
    CHECK(kg || (is_wayland && strstr(SDL_GetError(), "keyboard-shortcuts-inhibit")), "keyboard grab: %s",
          SDL_GetError());
    if (focused && kg) {
        CHECK(SDL_GetWindowKeyboardGrab(w), "keyboard grab readback");
    }
    CHECK(SDL_SetWindowKeyboardGrab(w, false), "keyboard ungrab");

    SDL_Rect r = { 10, 10, 50, 40 };
    CHECK(SDL_SetWindowMouseRect(w, &r), "mouse rect: %s", SDL_GetError());
    const SDL_Rect *got = SDL_GetWindowMouseRect(w);
    CHECK(got && got->x == 10 && got->w == 50, "mouse rect readback");
    CHECK(SDL_SetWindowMouseRect(w, NULL), "mouse rect clear");
    CHECK(SDL_GetWindowMouseRect(w) == NULL, "mouse rect cleared");
}

static void test_cursors(SDL_Window *w)
{
    SDL_Cursor *def = SDL_GetDefaultCursor();
    CHECK(def, "default cursor");
    CHECK(SDL_GetCursor() == def, "current = default");

    static const Uint8 data[8] = { 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff };
    static const Uint8 mask[8] = { 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff };
    SDL_Cursor *mono = SDL_CreateCursor(data, mask, 8, 8, 0, 0);
    CHECK(mono, "mono cursor: %s", SDL_GetError());
    CHECK(!SDL_CreateCursor(data, mask, 8, 8, 9, 0), "hotspot outside rejected");

    SDL_Surface *s = SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_FillSurfaceRect(s, NULL, SDL_MapSurfaceRGBA(s, 0, 200, 0, 200));
    SDL_SetNumberProperty(SDL_GetSurfaceProperties(s), SDL_PROP_SURFACE_HOTSPOT_X_NUMBER, 5);
    SDL_Cursor *color = SDL_CreateColorCursor(s, 0, 0);
    CHECK(color, "color cursor: %s", SDL_GetError());

    SDL_Surface *s2 = SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_CursorFrameInfo frames[2] = { { s, 30 }, { s2, 30 } };
    SDL_Cursor *anim = SDL_CreateAnimatedCursor(frames, 2, 1, 1);
    CHECK(anim, "animated cursor: %s", SDL_GetError());
    SDL_Surface *s3 = SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_RGBA32);
    SDL_CursorFrameInfo bad[2] = { { s, 30 }, { s3, 30 } };
    CHECK(!SDL_CreateAnimatedCursor(bad, 2, 0, 0), "mismatched frame sizes rejected");

    SDL_Cursor *sys = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    if (is_x11 || is_wayland) {
        CHECK(sys, "system cursor: %s", SDL_GetError());
        for (int i = 0; i < SDL_SYSTEM_CURSOR_COUNT; ++i) {
            SDL_Cursor *c = SDL_CreateSystemCursor((SDL_SystemCursor)i);
            CHECK(c, "system cursor %d: %s", i, SDL_GetError());
            SDL_DestroyCursor(c);
        }
    }
    CHECK(!SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_COUNT), "invalid system cursor rejected");

    CHECK(SDL_SetCursor(color), "set color cursor");
    CHECK(SDL_GetCursor() == color, "current = color");
    CHECK(SDL_SetCursor(anim), "set animated cursor");
    pump(100); /* frames advance in SDL_PumpEvents */
    CHECK(SDL_HideCursor(), "hide");
    CHECK(!SDL_CursorVisible(), "hidden");
    CHECK(SDL_ShowCursor(), "show");
    CHECK(SDL_CursorVisible(), "visible");
    CHECK(!SDL_SetCursor((SDL_Cursor *)&failures), "foreign cursor rejected");

    SDL_DestroyCursor(anim);
    CHECK(SDL_GetCursor() == def, "destroying the current cursor falls back to the default");
    SDL_DestroyCursor(color);
    SDL_DestroyCursor(mono);
    SDL_DestroyCursor(sys);
    SDL_DestroyCursor(def); /* ignored */
    CHECK(SDL_GetDefaultCursor() == def, "default cursor survives DestroyCursor");
    SDL_DestroySurface(s);
    SDL_DestroySurface(s2);
    SDL_DestroySurface(s3);
    (void)w;
}

static void test_warp(SDL_Window *w)
{
    SDL_RaiseWindow(w);
    SDL_SyncWindow(w);
    pump(50);
    SDL_WarpMouseInWindow(w, 40.0f, 30.0f);
    bool got_motion = false;
    Uint64 end = SDL_GetTicks() + 1000;
    while (!got_motion && SDL_GetTicks() < end) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_MOUSE_MOTION && ev.motion.windowID == SDL_GetWindowID(w) &&
                (int)ev.motion.x == 40 && (int)ev.motion.y == 30) {
                got_motion = true;
            }
        }
        SDL_Delay(5);
    }
    CHECK(got_motion, "warp produces a motion event at 40,30");
#ifdef SDLOP_TEST_HAVE_XCB
    if (is_x11) {
        int x, y;
        query_pointer(xid_of(w), &x, &y);
        CHECK(x == 40 && y == 30, "X pointer at 40,30 (got %d,%d)", x, y);
        float gx = -1, gy = -1;
        SDL_GetGlobalMouseState(&gx, &gy);
        int wx, wy;
        SDL_GetWindowPosition(w, &wx, &wy);
        CHECK(gx >= 0 && gy >= 0, "global mouse state %f,%f", gx, gy);
        CHECK(SDL_WarpMouseGlobal(5.0f, 6.0f), "warp global: %s", SDL_GetError());
        obs_sync();
        SDL_GetGlobalMouseState(&gx, &gy);
        CHECK(gx == 5.0f && gy == 6.0f, "global position after warp %f,%f", gx, gy);
        CHECK(SDL_CaptureMouse(true), "capture: %s", SDL_GetError());
        CHECK(SDL_CaptureMouse(false), "release capture");
    }
#endif
    if (!is_x11) {
        CHECK(!SDL_WarpMouseGlobal(1.0f, 1.0f), "global warp unsupported");
        CHECK(!SDL_CaptureMouse(true), "capture unsupported");
    }
}

static void test_devices(void)
{
    int n = -1;
    SDL_KeyboardID *kbs = SDL_GetKeyboards(&n);
    CHECK(kbs && n >= 0 && kbs[n] == 0, "keyboard list terminated");
    for (int i = 0; i < n; ++i) {
        CHECK(SDL_GetKeyboardNameForID(kbs[i]) != NULL, "keyboard %u has a name", (unsigned)kbs[i]);
    }
    CHECK(SDL_HasKeyboard() == (n > 0), "HasKeyboard matches list");
    SDL_free(kbs);
    SDL_MouseID *mice = SDL_GetMice(&n);
    CHECK(mice && n >= 0 && mice[n] == 0, "mouse list terminated");
    CHECK(SDL_HasMouse() == (n > 0), "HasMouse matches list");
    if (is_x11) {
        CHECK(n >= 1, "x11 reports a mouse");
    }
    SDL_free(mice);
    SDL_TouchID *touch = SDL_GetTouchDevices(&n);
    CHECK(touch && n >= 0 && touch[n] == 0, "touch list terminated");
    SDL_free(touch);
    CHECK(SDL_GetTouchFingers(12345, &n) == NULL, "unknown touch device: NULL");
}

static void test_screensaver(void)
{
    /* stock: the state toggles even when the driver can't act on it, but
     * then the call reports SDL_Unsupported() (dummy) */
    CHECK(!SDL_ScreenSaverEnabled(), "screensaver inhibited by default (stock)");
    bool ok = SDL_EnableScreenSaver();
    CHECK(ok == is_x11 || is_wayland /* needs idle-inhibit */, "enable returns %d", ok);
    CHECK(SDL_ScreenSaverEnabled(), "enabled");
    ok = SDL_DisableScreenSaver();
    CHECK(ok == is_x11 || is_wayland, "disable returns %d", ok);
    CHECK(!SDL_ScreenSaverEnabled(), "disabled");
}

int main(int argc, char **argv)
{
    const char *driver = argc > 1 ? argv[1] : "dummy";
    is_x11 = strcmp(driver, "x11") == 0;
    if (is_x11) {
#ifdef SDLOP_TEST_HAVE_XCB
        if (!getenv("DISPLAY")) {
            printf("test_window_api: SKIP (DISPLAY not set)\n");
            return 0;
        }
        obs = xcb_connect(NULL, NULL);
        if (xcb_connection_has_error(obs)) {
            printf("test_window_api: SKIP (cannot connect to %s)\n", getenv("DISPLAY"));
            return 0;
        }
        SDL_setenv_unsafe("SDLOP_DISABLE_RAW_INPUT", "1", 1);
#else
        printf("test_window_api: SKIP (built without xcb)\n");
        return 0;
#endif
    }
    is_wayland = strcmp(driver, "wayland") == 0;
    if (is_wayland && !getenv("WAYLAND_DISPLAY")) {
        printf("test_window_api: SKIP (WAYLAND_DISPLAY not set)\n");
        return 0;
    }
    SDL_setenv_unsafe("SDL_VIDEODRIVER", driver, 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init(%s) failed: %s\n", driver, SDL_GetError());
        return 1;
    }
    printf("test_window_api: driver %s\n", SDL_GetCurrentVideoDriver());

    test_create_validation();
    test_screensaver();
    test_devices();

    SDL_Window *w = SDL_CreateWindow("window api", 400, 300, SDL_WINDOW_RESIZABLE);
    CHECK(w, "window: %s", SDL_GetError());
    if (w) {
        SDL_SyncWindow(w);
        pump(100);
        test_size_limits(w);
        test_state(w);
        test_grabs(w);
        test_cursors(w);
        test_warp(w);
        test_parent_popup(w); /* destroys w */
    }
    SDL_Quit();
#ifdef SDLOP_TEST_HAVE_XCB
    if (obs) {
        xcb_disconnect(obs);
    }
#endif
    if (failures) {
        fprintf(stderr, "test_window_api (%s): %d failure(s)\n", driver, failures);
        return 1;
    }
    printf("test_window_api (%s): OK\n", driver);
    return 0;
}
