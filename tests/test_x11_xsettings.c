/*
  SDLop test: X11 XSETTINGS client (content scale, as in stock SDL3).

  A raw xcb client plays the settings manager (owner of _XSETTINGS_S<n>,
  settings in _XSETTINGS_SETTINGS):
    - manager present before SDL_Init, Xft/DPI = 144 * 1024 -> scale 1.5
      (the data also carries a string and a colour setting)
    - property change adding Gdk/WindowScalingFactor = 2 -> scale 2 (it
      wins over Xft/DPI), with SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED
    - manager window destroyed -> scale 1
    - new manager announced with MANAGER, MSB-first data, Xft/DPI =
      120 * 1024 -> scale 1.25
    - malformed data -> ignored, scale 1
  With SDLOP_X11_NO_XSETTINGS=1 the scale stays 1. Skips cleanly without
  DISPLAY, or when the session already has a settings manager or Xft.dpi.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

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

static xcb_connection_t *obs;
static xcb_screen_t *screen;
static xcb_atom_t A_selection, A_settings, A_manager;

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(obs, xcb_intern_atom(obs, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

/* --- XSETTINGS data writer --------------------------------------------- */

typedef struct
{
    uint8_t buf[512];
    size_t len;
    bool msb;
    int count;
} Data;

static void put8(Data *d, uint8_t v)
{
    d->buf[d->len++] = v;
}

static void put16(Data *d, uint16_t v)
{
    if (d->msb) {
        put8(d, (uint8_t)(v >> 8));
        put8(d, (uint8_t)v);
    } else {
        put8(d, (uint8_t)v);
        put8(d, (uint8_t)(v >> 8));
    }
}

static void put32(Data *d, uint32_t v)
{
    if (d->msb) {
        put16(d, (uint16_t)(v >> 16));
        put16(d, (uint16_t)v);
    } else {
        put16(d, (uint16_t)v);
        put16(d, (uint16_t)(v >> 16));
    }
}

static void put_padded(Data *d, const char *s, size_t n)
{
    memcpy(d->buf + d->len, s, n);
    d->len += n;
    while (d->len % 4) {
        put8(d, 0);
    }
}

static void begin(Data *d, bool msb)
{
    memset(d, 0, sizeof(*d));
    d->msb = msb;
    put8(d, msb ? 1 : 0);
    put8(d, 0);
    put8(d, 0);
    put8(d, 0);
    put32(d, 7); /* serial */
    put32(d, 0); /* count, patched in finish() */
}

static void header(Data *d, uint8_t type, const char *name)
{
    put8(d, type);
    put8(d, 0);
    put16(d, (uint16_t)strlen(name));
    put_padded(d, name, strlen(name));
    put32(d, 3); /* last-change-serial */
    d->count++;
}

static void add_int(Data *d, const char *name, int32_t v)
{
    header(d, 0, name);
    put32(d, (uint32_t)v);
}

static void add_string(Data *d, const char *name, const char *v)
{
    header(d, 1, name);
    put32(d, (uint32_t)strlen(v));
    put_padded(d, v, strlen(v));
}

static void add_color(Data *d, const char *name)
{
    header(d, 2, name);
    put16(d, 0xffff);
    put16(d, 0x8000);
    put16(d, 0);
    put16(d, 0xffff);
}

static void finish(Data *d)
{
    size_t len = d->len;
    d->len = 8;
    put32(d, (uint32_t)d->count);
    d->len = len;
}

static void publish(xcb_window_t mgr, const Data *d)
{
    xcb_change_property(obs, XCB_PROP_MODE_REPLACE, mgr, A_settings, A_settings, 8, (uint32_t)d->len, d->buf);
    xcb_flush(obs);
}

static xcb_window_t new_manager(void)
{
    xcb_window_t w = xcb_generate_id(obs);
    xcb_create_window(obs, XCB_COPY_FROM_PARENT, w, screen->root, -10, -10, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, 0, NULL);
    return w;
}

static void take_selection(xcb_window_t mgr, bool announce)
{
    xcb_set_selection_owner(obs, mgr, A_selection, XCB_CURRENT_TIME);
    if (announce) {
        /* ICCCM 2.8 MANAGER: timestamp, selection, owner */
        union {
            char bytes[32];
            xcb_client_message_event_t cm;
        } ev;
        memset(&ev, 0, sizeof(ev));
        ev.cm.response_type = XCB_CLIENT_MESSAGE;
        ev.cm.format = 32;
        ev.cm.window = screen->root;
        ev.cm.type = A_manager;
        ev.cm.data.data32[0] = XCB_CURRENT_TIME;
        ev.cm.data.data32[1] = A_selection;
        ev.cm.data.data32[2] = mgr;
        xcb_send_event(obs, 0, screen->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, ev.bytes);
    }
    xcb_flush(obs);
}

/* --- SDL side ------------------------------------------------------------ */

static int scale_events = 0;

static float scale_now(void)
{
    return SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
}

static bool wait_scale(float want)
{
    const Uint64 end = SDL_GetTicks() + 3000;
    do {
        SDL_Event ev;
        while (SDL_WaitEventTimeout(&ev, 10)) {
            if (ev.type == SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED) {
                scale_events++;
            }
        }
        if (fabsf(scale_now() - want) < 0.001f) {
            return true;
        }
    } while (SDL_GetTicks() < end);
    return false;
}

static bool has_xft_dpi(void)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        obs, xcb_get_property(obs, 0, screen->root, atom("RESOURCE_MANAGER"), XCB_ATOM_STRING, 0, 1u << 16), NULL);
    bool found = false;
    if (r && xcb_get_property_value_length(r) > 0) {
        const char *v = (const char *)xcb_get_property_value(r);
        const int n = xcb_get_property_value_length(r);
        for (int i = 0; i + 8 <= n && !found; i++) {
            found = (i == 0 || v[i - 1] == '\n') && memcmp(v + i, "Xft.dpi:", 8) == 0;
        }
    }
    free(r);
    return found;
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_x11_xsettings: SKIP (DISPLAY not set)\n");
        return 0;
    }
    setenv("SDL_VIDEODRIVER", "x11", 1);
    unsetenv("GDK_SCALE");
    unsetenv("SDL_VIDEO_X11_SCALING_FACTOR");
    int scr = 0;
    obs = xcb_connect(NULL, &scr);
    if (xcb_connection_has_error(obs)) {
        fprintf(stderr, "xcb_connect failed\n");
        return 1;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(obs));
    for (int i = 0; i < scr; i++) {
        xcb_screen_next(&it);
    }
    screen = it.data;
    char name[32];
    snprintf(name, sizeof(name), "_XSETTINGS_S%d", scr);
    A_selection = atom(name);
    A_settings = atom("_XSETTINGS_SETTINGS");
    A_manager = atom("MANAGER");
    xcb_get_selection_owner_reply_t *own =
        xcb_get_selection_owner_reply(obs, xcb_get_selection_owner(obs, A_selection), NULL);
    const bool managed = own && own->owner != XCB_NONE;
    free(own);
    if (managed || has_xft_dpi()) {
        printf("test_x11_xsettings: SKIP (session has a settings manager or Xft.dpi)\n");
        xcb_disconnect(obs);
        return 0;
    }
    const char *off = getenv("SDLOP_X11_NO_XSETTINGS");
    const bool disabled = off && strcmp(off, "1") == 0;

    /* manager before SDL_Init */
    Data d;
    xcb_window_t m1 = new_manager();
    begin(&d, false);
    add_string(&d, "Net/ThemeName", "Adwaita-dark");
    add_int(&d, "Xft/DPI", 144 * 1024);
    add_color(&d, "Gtk/ColorTest");
    finish(&d);
    publish(m1, &d);
    take_selection(m1, true);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (disabled) {
        CHECK(fabsf(scale_now() - 1.0f) < 0.001f, "SDLOP_X11_NO_XSETTINGS: scale 1 (%g)", scale_now());
        SDL_Quit();
        xcb_destroy_window(obs, m1);
        xcb_disconnect(obs);
        printf("test_x11_xsettings: %s\n", failures ? "FAIL" : "OK (disabled)");
        return failures ? 1 : 0;
    }
    CHECK(fabsf(scale_now() - 1.5f) < 0.001f, "Xft/DPI 144 -> scale 1.5 at init (%g)", scale_now());

    /* property change: the GDK window scale wins */
    begin(&d, false);
    add_int(&d, "Xft/DPI", 144 * 1024);
    add_int(&d, "Gdk/WindowScalingFactor", 2);
    finish(&d);
    publish(m1, &d);
    CHECK(wait_scale(2.0f), "Gdk/WindowScalingFactor 2 -> scale 2 (%g)", scale_now());
    CHECK(scale_events > 0, "SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED sent");

    /* the manager goes away */
    xcb_destroy_window(obs, m1);
    xcb_flush(obs);
    CHECK(wait_scale(1.0f), "manager gone -> scale 1 (%g)", scale_now());

    /* a new manager announces itself; big-endian data */
    xcb_window_t m2 = new_manager();
    begin(&d, true);
    add_int(&d, "Xft/DPI", 120 * 1024);
    add_string(&d, "Net/IconThemeName", "x");
    finish(&d);
    publish(m2, &d);
    take_selection(m2, true);
    CHECK(wait_scale(1.25f), "MANAGER + MSB-first Xft/DPI 120 -> scale 1.25 (%g)", scale_now());

    /* malformed: count claims more settings than there are */
    begin(&d, false);
    add_int(&d, "Gdk/WindowScalingFactor", 3);
    d.count = 5;
    finish(&d);
    publish(m2, &d);
    CHECK(wait_scale(1.0f), "malformed data ignored -> scale 1 (%g)", scale_now());

    SDL_Quit();
    xcb_destroy_window(obs, m2);
    xcb_disconnect(obs);
    if (failures) {
        fprintf(stderr, "test_x11_xsettings: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_x11_xsettings: OK\n");
    return 0;
}
