/*
  SDLop test: X11 _NET_WM_SYNC_REQUEST (SDL_HINT_VIDEO_X11_ENABLE_XSYNC_EXT).

  An observer connection plays the window manager:
    - with the hint, the window advertises _NET_WM_SYNC_REQUEST in
      WM_PROTOCOLS and a _NET_WM_SYNC_REQUEST_COUNTER starting at 0
    - after a sync request the counter stays put until a ConfigureNotify
      arrived *and* a frame was presented, then holds the requested
      64-bit value; further presents leave it alone
    - without the hint: neither the protocol nor the counter
    - SDLOP_LIB_XCB_SYNC=none: the hint degrades to no resize sync
  Skips cleanly when DISPLAY is unset or the server lacks SYNC.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/sync.h>
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

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(obs, xcb_intern_atom(obs, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

static xcb_window_t xid_of(SDL_Window *w)
{
    return (xcb_window_t)SDL_GetNumberProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
}

static bool has_protocol(xcb_window_t win, xcb_atom_t proto)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        obs, xcb_get_property(obs, 0, win, atom("WM_PROTOCOLS"), XCB_ATOM_ATOM, 0, 16), NULL);
    bool found = false;
    if (r && r->format == 32) {
        const xcb_atom_t *a = (const xcb_atom_t *)xcb_get_property_value(r);
        for (int i = 0; i < xcb_get_property_value_length(r) / 4; i++) {
            found = found || a[i] == proto;
        }
    }
    free(r);
    return found;
}

static uint32_t counter_of(xcb_window_t win)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        obs, xcb_get_property(obs, 0, win, atom("_NET_WM_SYNC_REQUEST_COUNTER"), XCB_ATOM_CARDINAL, 0, 1), NULL);
    uint32_t c = 0;
    if (r && r->format == 32 && xcb_get_property_value_length(r) == 4) {
        c = *(const uint32_t *)xcb_get_property_value(r);
    }
    free(r);
    return c;
}

static int64_t counter_value(uint32_t counter)
{
    xcb_sync_query_counter_reply_t *r = xcb_sync_query_counter_reply(obs, xcb_sync_query_counter(obs, counter), NULL);
    int64_t v = r ? (int64_t)(((uint64_t)(uint32_t)r->counter_value.hi << 32) | r->counter_value.lo) : -1;
    free(r);
    return v;
}

static void pump(Uint32 ms, Uint32 want, bool *seen)
{
    const Uint64 end = SDL_GetTicks() + ms;
    do {
        SDL_Event ev;
        while (SDL_WaitEventTimeout(&ev, 5)) {
            if (seen && ev.type == want) {
                *seen = true;
            }
        }
    } while (SDL_GetTicks() < end && !(seen && *seen));
}

static void present(SDL_Window *w, Uint32 color)
{
    SDL_Surface *s = SDL_GetWindowSurface(w);
    CHECK(s != NULL, "window surface: %s", SDL_GetError());
    if (s) {
        SDL_FillSurfaceRect(s, NULL, color);
        CHECK(SDL_UpdateWindowSurface(w), "SDL_UpdateWindowSurface: %s", SDL_GetError());
    }
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_x11_sync: SKIP (DISPLAY not set)\n");
        return 0;
    }
    setenv("SDL_VIDEODRIVER", "x11", 1);
    obs = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(obs)) {
        fprintf(stderr, "xcb_connect failed\n");
        return 1;
    }
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(obs, &xcb_sync_id);
    if (!ext || !ext->present) {
        printf("test_x11_sync: SKIP (no SYNC extension)\n");
        xcb_disconnect(obs);
        return 0;
    }
    free(xcb_sync_initialize_reply(obs, xcb_sync_initialize(obs, 3, 1), NULL));
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const xcb_atom_t SYNC_REQUEST = atom("_NET_WM_SYNC_REQUEST");

    /* without the hint (the default) */
    SDL_Window *plain = SDL_CreateWindow("nosync", 120, 90, 0);
    CHECK(plain != NULL, "create window: %s", SDL_GetError());
    if (plain) {
        CHECK(!has_protocol(xid_of(plain), SYNC_REQUEST), "no _NET_WM_SYNC_REQUEST without the hint");
        CHECK(counter_of(xid_of(plain)) == 0, "no counter without the hint");
        SDL_DestroyWindow(plain);
    }

    SDL_SetHint(SDL_HINT_VIDEO_X11_ENABLE_XSYNC_EXT, "1");
    SDL_Window *w = SDL_CreateWindow("sync", 200, 150, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (!w) {
        return 1;
    }
    const xcb_window_t xid = xid_of(w);
    const char *mod = getenv("SDLOP_LIB_XCB_SYNC");
    if (mod && strcmp(mod, "none") == 0) {
        /* libxcb-sync unavailable: the hint degrades to no resize sync */
        CHECK(!has_protocol(xid, SYNC_REQUEST) && counter_of(xid) == 0, "no resize sync without libxcb-sync");
        present(w, 0xff202020);
        SDL_DestroyWindow(w);
        SDL_Quit();
        xcb_disconnect(obs);
        printf("test_x11_sync: %s\n", failures ? "FAIL" : "OK (no module)");
        return failures ? 1 : 0;
    }
    CHECK(has_protocol(xid, SYNC_REQUEST), "WM_PROTOCOLS has _NET_WM_SYNC_REQUEST");
    const uint32_t counter = counter_of(xid);
    CHECK(counter != 0, "_NET_WM_SYNC_REQUEST_COUNTER set");
    present(w, 0xff202020);
    pump(500, 0, NULL); /* let mapping / WM configures settle */
    if (counter) {
        CHECK(counter_value(counter) == 0, "counter starts at 0 (%lld)", (long long)counter_value(counter));
    }

    /* the request: WM_PROTOCOLS, data = protocol, time, value lo, value hi */
    const int64_t want = ((int64_t)1 << 32) | 42;
    union {
        char bytes[32];
        xcb_client_message_event_t cm;
    } ev;
    memset(&ev, 0, sizeof(ev));
    ev.cm.response_type = XCB_CLIENT_MESSAGE;
    ev.cm.format = 32;
    ev.cm.window = xid;
    ev.cm.type = atom("WM_PROTOCOLS");
    ev.cm.data.data32[0] = SYNC_REQUEST;
    ev.cm.data.data32[1] = XCB_CURRENT_TIME;
    ev.cm.data.data32[2] = 42;
    ev.cm.data.data32[3] = 1;
    xcb_send_event(obs, 0, xid, XCB_EVENT_MASK_NO_EVENT, ev.bytes);
    xcb_flush(obs);
    pump(200, 0, NULL);
    present(w, 0xff404040);
    pump(100, 0, NULL);
    if (counter) {
        CHECK(counter_value(counter) == 0, "a present before the configure keeps the counter (%lld)",
              (long long)counter_value(counter));
    }

    /* the configure */
    bool resized = false;
    SDL_SetWindowSize(w, 260, 180);
    pump(3000, SDL_EVENT_WINDOW_RESIZED, &resized);
    CHECK(resized, "window resized");
    pump(100, 0, NULL);
    if (counter) {
        CHECK(counter_value(counter) == 0, "no counter update before the frame (%lld)",
              (long long)counter_value(counter));
    }
    present(w, 0xff606060);
    if (counter) {
        CHECK(counter_value(counter) == want, "counter = request after the frame (%lld, want %lld)",
              (long long)counter_value(counter), (long long)want);
    }

    /* idle again: presents don't touch it */
    present(w, 0xff808080);
    if (counter) {
        CHECK(counter_value(counter) == want, "counter unchanged by later frames");
    }

    SDL_DestroyWindow(w);
    SDL_Quit();
    xcb_disconnect(obs);
    if (failures) {
        fprintf(stderr, "test_x11_sync: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_x11_sync: OK\n");
    return 0;
}
