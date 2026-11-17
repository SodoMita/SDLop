/*
  SDLop test: X11 driver integration against a real X server (Xvfb in CI).

  Runs with or without a window manager - every assertion holds for both
  (EWMH WM path and the no-WM "legacy" path). Input is injected with XTest
  from a second connection and window state is verified from that
  connection too, so the test checks what the X server actually sees, not
  just SDLop's own bookkeeping. Skips cleanly when DISPLAY is unset.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xtest.h>

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

/* --- observer connection ------------------------------------------- */

static xcb_connection_t *obs;
static xcb_screen_t *obs_screen;

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(obs, xcb_intern_atom(obs, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

static char *get_string_prop(xcb_window_t w, xcb_atom_t prop)
{
    xcb_get_property_reply_t *r =
        xcb_get_property_reply(obs, xcb_get_property(obs, 0, w, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 1024), NULL);
    if (!r) {
        return NULL;
    }
    int len = xcb_get_property_value_length(r);
    char *s = (char *)malloc((size_t)len + 1);
    memcpy(s, xcb_get_property_value(r), (size_t)len);
    s[len] = '\0';
    free(r);
    return s;
}

static int get_card32_prop(xcb_window_t w, xcb_atom_t prop, uint32_t *out, int max)
{
    xcb_get_property_reply_t *r =
        xcb_get_property_reply(obs, xcb_get_property(obs, 0, w, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, max), NULL);
    if (!r) {
        return 0;
    }
    int n = r->format == 32 ? xcb_get_property_value_length(r) / 4 : 0;
    if (n > max) {
        n = max;
    }
    memcpy(out, xcb_get_property_value(r), (size_t)n * 4);
    free(r);
    return n;
}

static bool viewable(xcb_window_t w)
{
    xcb_get_window_attributes_reply_t *r =
        xcb_get_window_attributes_reply(obs, xcb_get_window_attributes(obs, w), NULL);
    bool v = r && r->map_state == XCB_MAP_STATE_VIEWABLE;
    free(r);
    return v;
}

/* client-area rectangle in root coordinates */
static SDL_Rect server_rect(xcb_window_t w)
{
    SDL_Rect r = { 0, 0, 0, 0 };
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(obs, xcb_get_geometry(obs, w), NULL);
    xcb_translate_coordinates_reply_t *t = xcb_translate_coordinates_reply(
        obs, xcb_translate_coordinates(obs, w, obs_screen->root, 0, 0), NULL);
    if (g) {
        r.w = g->width;
        r.h = g->height;
    }
    if (t) {
        r.x = t->dst_x;
        r.y = t->dst_y;
    }
    free(g);
    free(t);
    return r;
}

static uint32_t read_pixel(xcb_window_t w, int x, int y)
{
    xcb_get_image_reply_t *img = xcb_get_image_reply(
        obs, xcb_get_image(obs, XCB_IMAGE_FORMAT_Z_PIXMAP, w, (int16_t)x, (int16_t)y, 1, 1, 0xffffffffu), NULL);
    uint32_t px = 0xdeadbeef;
    if (img && xcb_get_image_data_length(img) >= 4) {
        memcpy(&px, xcb_get_image_data(img), 4);
        px &= 0xffffff;
    }
    free(img);
    return px;
}

/* depth-first search for our client window (WMs reparent into frames) */
static xcb_window_t find_window(xcb_window_t parent, const char *title)
{
    xcb_query_tree_reply_t *t = xcb_query_tree_reply(obs, xcb_query_tree(obs, parent), NULL);
    if (!t) {
        return XCB_NONE;
    }
    xcb_window_t found = XCB_NONE;
    xcb_window_t *kids = xcb_query_tree_children(t);
    int n = xcb_query_tree_children_length(t);
    for (int i = 0; i < n && !found; i++) {
        uint32_t pid = 0;
        char *name = get_string_prop(kids[i], atom("_NET_WM_NAME"));
        if (name && strcmp(name, title) == 0 && get_card32_prop(kids[i], atom("_NET_WM_PID"), &pid, 1) == 1 &&
            pid == (uint32_t)getpid()) {
            found = kids[i];
        }
        free(name);
        if (!found) {
            found = find_window(kids[i], title);
        }
    }
    free(t);
    return found;
}

static bool wm_running(void)
{
    uint32_t check = 0, self = 0;
    xcb_atom_t a = atom("_NET_SUPPORTING_WM_CHECK");
    return get_card32_prop(obs_screen->root, a, &check, 1) == 1 &&
           get_card32_prop(check, a, &self, 1) == 1 && self == check;
}

static void fake(uint8_t type, uint8_t detail, int16_t x, int16_t y, xcb_window_t root)
{
    xcb_test_fake_input(obs, type, detail, XCB_CURRENT_TIME, root, x, y, 0);
    xcb_flush(obs);
}

static void fake_key(uint8_t keycode, bool down)
{
    fake(down ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode, 0, 0, XCB_NONE);
}

static void fake_button(uint8_t button)
{
    fake(XCB_BUTTON_PRESS, button, 0, 0, XCB_NONE);
    fake(XCB_BUTTON_RELEASE, button, 0, 0, XCB_NONE);
}

static void fake_motion_abs(int x, int y)
{
    fake(XCB_MOTION_NOTIFY, 0, (int16_t)x, (int16_t)y, obs_screen->root);
}

static void fake_motion_rel(int dx, int dy)
{
    fake(XCB_MOTION_NOTIFY, 1, (int16_t)dx, (int16_t)dy, XCB_NONE);
}

static void send_client_message(xcb_window_t w, xcb_atom_t type, uint32_t d0, uint32_t d1, uint32_t d2)
{
    union {
        char bytes[32];
        xcb_client_message_event_t cm;
    } ev;
    memset(&ev, 0, sizeof(ev));
    ev.cm.response_type = XCB_CLIENT_MESSAGE;
    ev.cm.format = 32;
    ev.cm.window = w;
    ev.cm.type = type;
    ev.cm.data.data32[0] = d0;
    ev.cm.data.data32[1] = d1;
    ev.cm.data.data32[2] = d2;
    xcb_send_event(obs, 0, w, XCB_EVENT_MASK_NO_EVENT, ev.bytes);
    xcb_flush(obs);
}

/* --- SDL side helpers ------------------------------------------------ */

typedef bool (*event_pred)(const SDL_Event *e, void *user);

/* pump until pred matches (events before it are dropped) */
static bool wait_event(event_pred pred, void *user, int timeout_ms, SDL_Event *out)
{
    Uint64 end = SDL_GetTicks() + (Uint64)timeout_ms;
    SDL_Event e;
    while (SDL_GetTicks() < end) {
        while (SDL_WaitEventTimeout(&e, 20)) {
            if (pred(&e, user)) {
                if (out) {
                    *out = e;
                }
                return true;
            }
        }
    }
    return false;
}

static bool is_type(const SDL_Event *e, void *user)
{
    return e->type == (Uint32)(uintptr_t)user;
}

static bool wait_type(Uint32 type, int timeout_ms, SDL_Event *out)
{
    return wait_event(is_type, (void *)(uintptr_t)type, timeout_ms, out);
}

typedef struct
{
    Uint32 type;
    int a, b;
} typed_xy;

static bool is_resized_to(const SDL_Event *e, void *user)
{
    typed_xy *t = (typed_xy *)user;
    return e->type == t->type && e->window.data1 == t->a && e->window.data2 == t->b;
}

static bool is_motion_to(const SDL_Event *e, void *user)
{
    typed_xy *t = (typed_xy *)user;
    return e->type == SDL_EVENT_MOUSE_MOTION && (int)e->motion.x == t->a && (int)e->motion.y == t->b;
}

static void pump_ms(int ms)
{
    Uint64 end = SDL_GetTicks() + (Uint64)ms;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
}

static void drain(void)
{
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
}

static bool wait_server(bool (*cond)(xcb_window_t, void *), xcb_window_t w, void *user, int timeout_ms)
{
    Uint64 end = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < end) {
        if (cond(w, user)) {
            return true;
        }
        SDL_PumpEvents();
        SDL_Delay(5);
    }
    return cond(w, user);
}

static bool cond_size(xcb_window_t w, void *user)
{
    SDL_Rect *want = (SDL_Rect *)user;
    SDL_Rect r = server_rect(w);
    return r.w == want->w && r.h == want->h;
}

static bool cond_rect(xcb_window_t w, void *user)
{
    SDL_Rect *want = (SDL_Rect *)user;
    SDL_Rect r = server_rect(w);
    return r.x == want->x && r.y == want->y && r.w == want->w && r.h == want->h;
}

static bool cond_viewable(xcb_window_t w, void *user)
{
    return viewable(w) == (user != NULL);
}

static bool cond_pixel(xcb_window_t w, void *user)
{
    return read_pixel(w, 5, 5) == *(uint32_t *)user;
}

/* -------------------------------------------------------------------- */

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_x11: SKIP (DISPLAY not set)\n");
        return 0;
    }
    /* keep the test deterministic: window-system input only */
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDL_VIDEODRIVER", "x11", 1);

    int screen_num = 0;
    obs = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(obs)) {
        printf("test_x11: SKIP (cannot connect to %s)\n", getenv("DISPLAY"));
        return 0;
    }
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(xcb_get_setup(obs));
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&si);
    }
    obs_screen = si.data;
    const bool wm = wm_running();
    printf("test_x11: %s, window manager: %s, shm: %s\n", getenv("DISPLAY"), wm ? "yes" : "no",
           getenv("SDLOP_X11_NO_SHM") ? "disabled" : "auto");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    CHECK(strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0, "video driver is x11");
    {
        SDL_Event e;
        CHECK(wait_type(SDL_EVENT_KEYBOARD_ADDED, 200, &e), "KEYBOARD_ADDED at init");
    }

    /* --- displays ---------------------------------------------------- */
    int ndisp = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&ndisp);
    CHECK(displays && ndisp >= 1, "at least one display (%d)", ndisp);
    SDL_Rect db = { 0, 0, 0, 0 };
    if (displays && ndisp >= 1) {
        CHECK(SDL_GetDisplayBounds(displays[0], &db), "display bounds");
        if (ndisp == 1) {
            CHECK(db.w == obs_screen->width_in_pixels && db.h == obs_screen->height_in_pixels,
                  "single display covers the root window (%dx%d vs %dx%d)", db.w, db.h,
                  obs_screen->width_in_pixels, obs_screen->height_in_pixels);
        }
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(displays[0]);
        CHECK(dm && dm->w == db.w && dm->h == db.h, "desktop mode matches bounds");
        printf("display 0: '%s' %d,%d %dx%d @ %.2f Hz, scale %.2f\n", SDL_GetDisplayName(displays[0]), db.x,
               db.y, db.w, db.h, dm ? dm->refresh_rate : 0.0f, SDL_GetDisplayContentScale(displays[0]));
        CHECK(SDL_GetDisplayContentScale(displays[0]) >= 1.0f, "content scale");
    }
    free(displays);

    /* --- window creation & ICCCM/EWMH properties ---------------------- */
    SDL_Window *w = SDL_CreateWindow("SDLop x11 test", 320, 200, SDL_WINDOW_RESIZABLE);
    if (!w) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    xcb_window_t xid = find_window(obs_screen->root, "SDLop x11 test");
    CHECK(xid != 0, "found our X window by _NET_WM_NAME + _NET_WM_PID");
    if (!xid) {
        SDL_Quit();
        return 1;
    }
    CHECK(viewable(xid), "window is viewable after SDL_CreateWindow");
    CHECK(wait_type(SDL_EVENT_WINDOW_FOCUS_GAINED, 1000, NULL), "FOCUS_GAINED after show");
    CHECK(SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS, "INPUT_FOCUS flag");
    {
        char *name = get_string_prop(xid, atom("_NET_WM_NAME"));
        CHECK(name && strcmp(name, "SDLop x11 test") == 0, "_NET_WM_NAME is '%s'", name ? name : "(null)");
        free(name);
        uint32_t protos[4] = { 0 };
        int n = get_card32_prop(xid, atom("WM_PROTOCOLS"), protos, 4);
        bool del = false, ping = false;
        for (int i = 0; i < n; i++) {
            del |= protos[i] == atom("WM_DELETE_WINDOW");
            ping |= protos[i] == atom("_NET_WM_PING");
        }
        CHECK(del && ping, "WM_PROTOCOLS has WM_DELETE_WINDOW and _NET_WM_PING");
        uint32_t pid = 0;
        CHECK(get_card32_prop(xid, atom("_NET_WM_PID"), &pid, 1) == 1 && pid == (uint32_t)getpid(), "_NET_WM_PID");
    }
    CHECK(SDL_SetWindowTitle(w, "Привет, X11"), "set UTF-8 title");
    pump_ms(30);
    {
        char *name = get_string_prop(xid, atom("_NET_WM_NAME"));
        CHECK(name && strcmp(name, "Привет, X11") == 0, "UTF-8 _NET_WM_NAME");
        free(name);
    }

    /* --- size & position (server-verified) ----------------------------- */
    drain();
    CHECK(SDL_SetWindowSize(w, 400, 300), "set size");
    {
        typed_xy t = { SDL_EVENT_WINDOW_RESIZED, 400, 300 };
        CHECK(wait_event(is_resized_to, &t, 1000, NULL), "RESIZED 400x300 event");
        SDL_Rect want = { 0, 0, 400, 300 };
        CHECK(wait_server(cond_size, xid, &want, 500), "server size 400x300");
        int ww = 0, hh = 0;
        SDL_GetWindowSize(w, &ww, &hh);
        CHECK(ww == 400 && hh == 300, "SDL size 400x300 (%dx%d)", ww, hh);
    }
    drain();
    CHECK(SDL_SetWindowPosition(w, 50, 60), "set position");
    {
        SDL_Event e;
        typed_xy t = { SDL_EVENT_WINDOW_MOVED, 50, 60 };
        CHECK(wait_event(is_resized_to, &t, 1000, &e), "MOVED to 50,60");
        SDL_Rect want = { 50, 60, 400, 300 };
        CHECK(wait_server(cond_rect, xid, &want, 500), "server position 50,60 (StaticGravity)");
        int x = 0, y = 0;
        SDL_GetWindowPosition(w, &x, &y);
        CHECK(x == 50 && y == 60, "SDL position 50,60 (%d,%d)", x, y);
    }

    /* --- software framebuffer (MIT-SHM or xcb_put_image) --------------- */
    {
        SDL_Surface *s = SDL_GetWindowSurface(w);
        CHECK(s && s->w == 400 && s->h == 300, "window surface 400x300");
        if (s) {
            SDL_FillSurfaceRect(s, NULL, 0x00ff0000);
            CHECK(SDL_UpdateWindowSurface(w), "update surface");
            uint32_t red = 0xff0000;
            CHECK(wait_server(cond_pixel, xid, &red, 500), "server pixel is red (0x%06x)", read_pixel(xid, 5, 5));
            /* partial update */
            SDL_Rect r = { 0, 0, 10, 10 };
            SDL_FillSurfaceRect(s, &r, 0x000000ff);
            CHECK(SDL_UpdateWindowSurfaceRects(w, &r, 1), "update rects");
            uint32_t blue = 0x0000ff;
            CHECK(wait_server(cond_pixel, xid, &blue, 500), "server pixel is blue after rect update");
            CHECK(read_pixel(xid, 200, 150) == 0xff0000, "pixels outside the rect untouched");
        }
        SDLop_SetWindowClearColor(w, 0x30, 0xA0, 0x40);
        uint32_t green = 0x30a040;
        CHECK(wait_server(cond_pixel, xid, &green, 500), "clear color reaches the server (0x%06x)",
              read_pixel(xid, 5, 5));
    }

    /* --- input via XTest ------------------------------------------------ */
    {
        SDL_Rect r = server_rect(xid);
        drain();
        fake_motion_abs(r.x + 100, r.y + 120);
        typed_xy t = { 0, 100, 120 };
        CHECK(wait_event(is_motion_to, &t, 1000, NULL), "MOUSE_MOTION to 100,120");
        CHECK(SDL_GetMouseFocus() == w, "mouse focus");
        float mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        CHECK((int)mx == 100 && (int)my == 120, "SDL_GetMouseState %g,%g", mx, my);

        drain();
        fake_button(1);
        SDL_Event e;
        CHECK(wait_type(SDL_EVENT_MOUSE_BUTTON_DOWN, 500, &e) && e.button.button == SDL_BUTTON_LEFT,
              "left button down");
        CHECK(wait_type(SDL_EVENT_MOUSE_BUTTON_UP, 500, &e) && e.button.button == SDL_BUTTON_LEFT, "left button up");
        fake_button(3);
        CHECK(wait_type(SDL_EVENT_MOUSE_BUTTON_DOWN, 500, &e) && e.button.button == SDL_BUTTON_RIGHT,
              "right button down");
        fake_button(8);
        CHECK(wait_type(SDL_EVENT_MOUSE_BUTTON_DOWN, 500, &e) && e.button.button == SDL_BUTTON_X1, "X1 button");
        drain();
        fake_button(4);
        CHECK(wait_type(SDL_EVENT_MOUSE_WHEEL, 500, &e) && e.wheel.y == 1.0f && e.wheel.x == 0.0f, "wheel up");
        fake_button(7);
        CHECK(wait_type(SDL_EVENT_MOUSE_WHEEL, 500, &e) && e.wheel.x == 1.0f, "wheel right");

        /* keys: X keycode = evdev + 8 (KEY_A = 30 -> 38, KEY_LEFTSHIFT = 42 -> 50) */
        drain();
        fake_key(38, true);
        fake_key(38, false);
        CHECK(wait_type(SDL_EVENT_KEY_DOWN, 500, &e) && e.key.scancode == SDL_SCANCODE_A && e.key.key == 'a' &&
                  !e.key.repeat,
              "key A down (sc %d key 0x%x rep %d)", (int)e.key.scancode, (unsigned)e.key.key, (int)e.key.repeat);
        CHECK(wait_type(SDL_EVENT_KEY_UP, 500, &e) && e.key.scancode == SDL_SCANCODE_A, "key A up");
        fake_key(50, true);
        fake_key(38, true);
        CHECK(wait_type(SDL_EVENT_KEY_DOWN, 500, &e) && e.key.scancode == SDL_SCANCODE_LSHIFT, "shift down");
        CHECK(wait_type(SDL_EVENT_KEY_DOWN, 500, &e) && e.key.scancode == SDL_SCANCODE_A &&
                  (e.key.mod & SDL_KMOD_SHIFT),
              "shift+A carries KMOD_SHIFT (mod 0x%x)", (unsigned)e.key.mod);
        fake_key(38, false);
        fake_key(50, false);
        pump_ms(50);
        CHECK(!(SDL_GetModState() & SDL_KMOD_SHIFT), "shift released");

        /* text input */
        SDL_StartTextInput(w);
        drain();
        fake_key(38, true);
        fake_key(38, false);
        CHECK(wait_type(SDL_EVENT_TEXT_INPUT, 500, &e) && strcmp(e.text.text, "a") == 0, "TEXT_INPUT 'a'");
        SDL_StopTextInput(w);

        /* held key: server autorepeat must not leak; SDLop repeats itself */
        drain();
        fake_key(38, true);
        int downs = 0, repeats = 0;
        Uint64 end = SDL_GetTicks() + 900;
        while (SDL_GetTicks() < end) {
            SDL_PumpEvents();
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_A) {
                    if (e.key.repeat) {
                        repeats++;
                    } else {
                        downs++;
                    }
                }
                CHECK(e.type != SDL_EVENT_KEY_UP, "no KEY_UP while the key is held");
            }
            SDL_Delay(5);
        }
        fake_key(38, false);
        CHECK(downs == 1, "exactly one non-repeat KEY_DOWN for a held key (%d)", downs);
        CHECK(repeats >= 1, "client-side repeats while held (%d)", repeats);
        CHECK(wait_type(SDL_EVENT_KEY_UP, 500, &e), "KEY_UP after release");
        pump_ms(100);
        drain();
        pump_ms(100);
        CHECK(!SDL_HasEvent(SDL_EVENT_KEY_DOWN), "no repeats after release");
    }

    /* --- SDL_WaitEventTimeout wakes up on X input promptly ------------- */
    {
        drain();
        SDL_Rect r = server_rect(xid);
        fake_motion_abs(r.x + 10, r.y + 11);
        SDL_Event e;
        Uint64 t0 = SDL_GetTicks();
        bool got = false;
        while (!got && SDL_GetTicks() - t0 < 1000) {
            got = SDL_WaitEventTimeout(&e, 1000) && e.type == SDL_EVENT_MOUSE_MOTION;
        }
        Uint64 dt = SDL_GetTicks() - t0;
        CHECK(got && dt < 200, "WaitEventTimeout returned motion in %llu ms", (unsigned long long)dt);
    }

    /* --- WM protocols ---------------------------------------------------- */
    {
        drain();
        send_client_message(xid, atom("WM_PROTOCOLS"), atom("WM_DELETE_WINDOW"), XCB_CURRENT_TIME, 0);
        CHECK(wait_type(SDL_EVENT_WINDOW_CLOSE_REQUESTED, 500, NULL), "WM_DELETE_WINDOW -> CLOSE_REQUESTED");

        /* _NET_WM_PING must come back to the root window */
        uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
        xcb_change_window_attributes(obs, obs_screen->root, XCB_CW_EVENT_MASK, &mask);
        xcb_flush(obs);
        send_client_message(xid, atom("WM_PROTOCOLS"), atom("_NET_WM_PING"), 0x1234, xid);
        bool pong = false;
        Uint64 end = SDL_GetTicks() + 1000;
        while (!pong && SDL_GetTicks() < end) {
            SDL_PumpEvents();
            xcb_generic_event_t *ev;
            while ((ev = xcb_poll_for_event(obs)) != NULL) {
                if ((ev->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
                    xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
                    if (cm->window == obs_screen->root && cm->data.data32[0] == atom("_NET_WM_PING") &&
                        cm->data.data32[1] == 0x1234) {
                        pong = true;
                    }
                }
                free(ev);
            }
            SDL_Delay(5);
        }
        CHECK(pong, "_NET_WM_PING answered");
        mask = 0;
        xcb_change_window_attributes(obs, obs_screen->root, XCB_CW_EVENT_MASK, &mask);
        xcb_flush(obs);
    }

    /* --- maximize / restore ---------------------------------------------- */
    {
        SDL_Rect before = server_rect(xid);
        drain();
        CHECK(SDL_MaximizeWindow(w), "maximize");
        CHECK(wait_type(SDL_EVENT_WINDOW_MAXIMIZED, 1500, NULL), "MAXIMIZED event");
        CHECK(SDL_GetWindowFlags(w) & SDL_WINDOW_MAXIMIZED, "MAXIMIZED flag");
        pump_ms(100);
        SDL_Rect r = server_rect(xid);
        CHECK(r.w > before.w && r.h > before.h, "maximized window grew (%dx%d -> %dx%d)", before.w, before.h, r.w,
              r.h);
        int ww = 0, hh = 0;
        SDL_GetWindowSize(w, &ww, &hh);
        CHECK(ww == r.w && hh == r.h, "SDL size follows the server (%dx%d vs %dx%d)", ww, hh, r.w, r.h);
        drain();
        CHECK(SDL_RestoreWindow(w), "restore");
        CHECK(wait_type(SDL_EVENT_WINDOW_RESTORED, 1500, NULL), "RESTORED event");
        CHECK(!(SDL_GetWindowFlags(w) & SDL_WINDOW_MAXIMIZED), "MAXIMIZED flag cleared");
        CHECK(wait_server(cond_size, xid, &before, 1000), "size restored to %dx%d", before.w, before.h);
    }

    /* --- fullscreen ------------------------------------------------------ */
    {
        SDL_Rect before = server_rect(xid);
        drain();
        CHECK(SDL_SetWindowFullscreen(w, true), "enter fullscreen");
        CHECK(wait_type(SDL_EVENT_WINDOW_ENTER_FULLSCREEN, 1500, NULL), "ENTER_FULLSCREEN event");
        CHECK(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN, "FULLSCREEN flag");
        CHECK(wait_server(cond_rect, xid, &db, 1000), "window covers the display");
        {
            SDL_Rect r = server_rect(xid);
            printf("fullscreen rect %d,%d %dx%d\n", r.x, r.y, r.w, r.h);
        }
        int ww = 0, hh = 0;
        pump_ms(50);
        SDL_GetWindowSize(w, &ww, &hh);
        CHECK(ww == db.w && hh == db.h, "SDL size is the display size (%dx%d)", ww, hh);
        drain();
        CHECK(SDL_SetWindowFullscreen(w, false), "leave fullscreen");
        CHECK(wait_type(SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 1500, NULL), "LEAVE_FULLSCREEN event");
        CHECK(!(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN), "FULLSCREEN flag cleared");
        CHECK(wait_server(cond_size, xid, &before, 1000), "size back to %dx%d", before.w, before.h);
    }

    /* --- minimize / restore ---------------------------------------------- */
    {
        drain();
        CHECK(SDL_MinimizeWindow(w), "minimize");
        CHECK(wait_type(SDL_EVENT_WINDOW_MINIMIZED, 1000, NULL), "MINIMIZED event");
        CHECK(SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED, "MINIMIZED flag");
        if (wm) {
            CHECK(wait_server(cond_viewable, xid, NULL, 1000), "WM iconified (unmapped) the window");
        }
        pump_ms(50);
        CHECK(SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED, "still MINIMIZED after WM confirmation");
        drain();
        CHECK(SDL_RestoreWindow(w), "restore from minimized");
        CHECK(wait_type(SDL_EVENT_WINDOW_RESTORED, 1000, NULL), "RESTORED event");
        CHECK(!(SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED), "MINIMIZED flag cleared");
        CHECK(wait_server(cond_viewable, xid, (void *)1, 1000), "viewable again");
        pump_ms(100);
        CHECK(!(SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED), "WM did not re-minimize");
        if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS)) {
            wait_type(SDL_EVENT_WINDOW_FOCUS_GAINED, 1000, NULL);
        }
    }

    /* --- relative mouse mode (XInput2 raw motion + pointer grab) ------- */
    {
        SDL_Rect r = server_rect(xid);
        fake_motion_abs(r.x + r.w / 2, r.y + r.h / 2);
        pump_ms(50);
        drain();
        CHECK(SDL_SetWindowRelativeMouseMode(w, true), "relative mode on: %s", SDL_GetError());
        CHECK(SDL_GetWindowRelativeMouseMode(w), "relative mode flag");
        pump_ms(50);
        /* our pointer grab must be active: a second client cannot grab */
        xcb_grab_pointer_reply_t *g = xcb_grab_pointer_reply(
            obs, xcb_grab_pointer(obs, 0, obs_screen->root, 0, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
                                  XCB_NONE, XCB_CURRENT_TIME),
            NULL);
        CHECK(g && g->status == XCB_GRAB_STATUS_ALREADY_GRABBED, "pointer grabbed by SDLop (status %d)",
              g ? g->status : -1);
        if (g && g->status == XCB_GRAB_STATUS_SUCCESS) {
            xcb_ungrab_pointer(obs, XCB_CURRENT_TIME);
        }
        free(g);
        drain();
        float sx = 0, sy = 0;
        int events = 0;
        for (int i = 0; i < 5; i++) {
            fake_motion_rel(7, -3);
            pump_ms(20);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_MOUSE_MOTION) {
                sx += e.motion.xrel;
                sy += e.motion.yrel;
                events++;
            }
        }
        CHECK(events >= 1 && (int)sx == 35 && (int)sy == -15, "relative deltas sum to 35,-15 (%g,%g in %d events)",
              sx, sy, events);
        CHECK(SDL_SetWindowRelativeMouseMode(w, false), "relative mode off");
        pump_ms(30);
        g = xcb_grab_pointer_reply(obs,
                                   xcb_grab_pointer(obs, 0, obs_screen->root, 0, XCB_GRAB_MODE_ASYNC,
                                                    XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME),
                                   NULL);
        CHECK(g && g->status == XCB_GRAB_STATUS_SUCCESS, "pointer released after relative mode");
        free(g);
        xcb_ungrab_pointer(obs, XCB_CURRENT_TIME);
        xcb_flush(obs);
    }

    /* --- hide / show ------------------------------------------------------ */
    {
        CHECK(SDL_HideWindow(w), "hide");
        CHECK(!viewable(xid), "unmapped after hide");
        CHECK(SDL_ShowWindow(w), "show");
        CHECK(viewable(xid), "viewable after show");
    }

    /* --- second window, borderless, destroyed ----------------------------- */
    {
        SDL_Window *w2 = SDL_CreateWindow("second", 100, 80, SDL_WINDOW_BORDERLESS);
        CHECK(w2 != NULL, "second window");
        if (w2) {
            pump_ms(50);
            SDL_DestroyWindow(w2);
            pump_ms(50);
        }
    }

    SDL_DestroyWindow(w);
    SDL_Quit();

    /* re-init must work (static driver state fully reset) */
    CHECK(SDL_Init(SDL_INIT_VIDEO), "re-init: %s", SDL_GetError());
    w = SDL_CreateWindow("again", 64, 64, 0);
    CHECK(w != NULL, "window after re-init");
    SDL_DestroyWindow(w);
    SDL_Quit();
    xcb_disconnect(obs);

    if (failures) {
        printf("test_x11: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_x11: PASS\n");
    return 0;
}
