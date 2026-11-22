/*
  SDLop test: X11 hit test and window menu (stock SDL3).

  An observer connection selects SubstructureNotify on the root window and
  so sees the EWMH client messages SDL sends there; XTest drives the
  pointer:
    - left press on a SDL_HITTEST_DRAGGABLE area -> _NET_WM_MOVERESIZE
      (root position, _NET_WM_MOVERESIZE_MOVE, button 1), no button event
    - left press on a SDL_HITTEST_RESIZE_BOTTOMRIGHT area ->
      _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT
    - normal areas, other buttons and a cleared hit test: plain events
    - SDL_ShowWindowSystemMenu -> _GTK_SHOW_WINDOW_MENU at the root
      position of the window point
  A running WM may act on the messages (move / resize / menu); the test
  only relies on what SDL sends. Skips cleanly when DISPLAY is unset.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static xcb_connection_t *obs;
static xcb_screen_t *screen;
static xcb_window_t target;
static xcb_atom_t A_moveresize, A_menu;

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(obs, xcb_intern_atom(obs, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

static void fake(uint8_t type, uint8_t detail, int16_t x, int16_t y, xcb_window_t root)
{
    xcb_test_fake_input(obs, type, detail, XCB_CURRENT_TIME, root, x, y, 0);
    xcb_flush(obs);
}

/* root coordinates of a window point */
static void to_root(int x, int y, int *rx, int *ry)
{
    xcb_translate_coordinates_reply_t *tr = xcb_translate_coordinates_reply(
        obs, xcb_translate_coordinates(obs, target, screen->root, (int16_t)x, (int16_t)y), NULL);
    *rx = tr ? tr->dst_x : x;
    *ry = tr ? tr->dst_y : y;
    free(tr);
}

typedef struct
{
    int downs, ups;
} Buttons;

static void pump_events(Buttons *b, Uint32 ms)
{
    const Uint64 end = SDL_GetTicks() + ms;
    do {
        SDL_Event ev;
        SDL_PumpEvents();
        while (SDL_PollEvent(&ev)) {
            if (b && ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                b->downs++;
            } else if (b && ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                b->ups++;
            }
        }
        SDL_Delay(5);
    } while (SDL_GetTicks() < end);
}

/* move the pointer to window point (x, y) and wait until SDL saw it */
static bool pointer_to(int x, int y)
{
    int rx, ry;
    to_root(x, y, &rx, &ry);
    fake(XCB_MOTION_NOTIFY, 0, (int16_t)rx, (int16_t)ry, screen->root);
    const Uint64 end = SDL_GetTicks() + 2000;
    while (SDL_GetTicks() < end) {
        float mx, my;
        pump_events(NULL, 5);
        SDL_GetMouseState(&mx, &my);
        if ((int)mx == x && (int)my == y) {
            return true;
        }
    }
    fprintf(stderr, "  pointer did not reach %d,%d\n", x, y);
    return false;
}

/* drop observer events */
static void drain(void)
{
    xcb_generic_event_t *e;
    while ((e = xcb_poll_for_event(obs))) {
        free(e);
    }
}

/* the next client message of type for the SDL window, pumping SDL meanwhile */
static bool wait_message(xcb_atom_t type, uint32_t data[5], Buttons *b, Uint32 ms)
{
    const Uint64 end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        xcb_generic_event_t *e;
        while ((e = xcb_poll_for_event(obs))) {
            if ((e->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
                xcb_client_message_event_t *cm = (xcb_client_message_event_t *)e;
                if (cm->type == type && cm->window == target) {
                    memcpy(data, cm->data.data32, 5 * sizeof(uint32_t));
                    free(e);
                    return true;
                }
            }
            free(e);
        }
        pump_events(b, 5);
    }
    return false;
}

static SDL_HitTestResult hit_test(SDL_Window *win, const SDL_Point *p, void *data)
{
    (void)win;
    (void)data;
    if (p->y < 20) {
        return SDL_HITTEST_DRAGGABLE;
    }
    if (p->x >= 300 && p->y >= 220) {
        return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    }
    return SDL_HITTEST_NORMAL;
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_x11_hittest: SKIP (DISPLAY not set)\n");
        return 0;
    }
    setenv("SDL_VIDEODRIVER", "x11", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    int scr = 0;
    obs = xcb_connect(NULL, &scr);
    if (xcb_connection_has_error(obs) || !SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "setup failed: %s\n", SDL_GetError());
        return 1;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(obs));
    for (int i = 0; i < scr; i++) {
        xcb_screen_next(&it);
    }
    screen = it.data;
    A_moveresize = atom("_NET_WM_MOVERESIZE");
    A_menu = atom("_GTK_SHOW_WINDOW_MENU");
    uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(obs, screen->root, XCB_CW_EVENT_MASK, &mask);
    xcb_flush(obs);

    SDL_Window *w = SDL_CreateWindow("hittest", 320, 240, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (!w) {
        return 1;
    }
    target = (xcb_window_t)SDL_GetNumberProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    SDL_Surface *s = SDL_GetWindowSurface(w);
    if (s) {
        SDL_FillSurfaceRect(s, NULL, 0xff000000);
        SDL_UpdateWindowSurface(w);
    }
    SDL_RaiseWindow(w);
    Uint64 end = SDL_GetTicks() + 3000;
    while (!(SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) && SDL_GetTicks() < end) {
        pump_events(NULL, 10);
    }
    CHECK(SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS, "window focused");
    CHECK(SDL_SetWindowHitTest(w, hit_test, NULL), "SDL_SetWindowHitTest: %s", SDL_GetError());

    uint32_t d[5];
    Buttons b;
    int rx, ry;

    /* draggable */
    CHECK(pointer_to(50, 10), "pointer on the draggable area");
    drain();
    memset(&b, 0, sizeof(b));
    to_root(50, 10, &rx, &ry);
    fake(XCB_BUTTON_PRESS, 1, 0, 0, XCB_NONE);
    const bool moved = wait_message(A_moveresize, d, &b, 2000);
    CHECK(moved, "_NET_WM_MOVERESIZE on a press in the draggable area");
    if (moved) {
        CHECK((int)d[0] == rx && (int)d[1] == ry && d[2] == 8 && d[3] == 1 && d[4] == 1,
              "move at %d,%d (want %d,%d), direction %u (8), button %u, source %u", (int)d[0], (int)d[1], rx, ry,
              d[2], d[3], d[4]);
    }
    fake(XCB_BUTTON_RELEASE, 1, 0, 0, XCB_NONE);
    pump_events(&b, 300);
    CHECK(b.downs == 0, "the press is not passed on (%d)", b.downs);

    /* resize corner */
    CHECK(pointer_to(310, 230), "pointer on the resize corner");
    drain();
    memset(&b, 0, sizeof(b));
    fake(XCB_BUTTON_PRESS, 1, 0, 0, XCB_NONE);
    const bool resized = wait_message(A_moveresize, d, &b, 2000);
    CHECK(resized && d[2] == 4, "_NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT (%u)", resized ? d[2] : 99);
    fake(XCB_BUTTON_RELEASE, 1, 0, 0, XCB_NONE);
    pump_events(&b, 300);
    CHECK(b.downs == 0, "resize press not passed on (%d)", b.downs);

    /* normal area */
    CHECK(pointer_to(150, 120), "pointer on a normal area");
    drain();
    memset(&b, 0, sizeof(b));
    fake(XCB_BUTTON_PRESS, 1, 0, 0, XCB_NONE);
    fake(XCB_BUTTON_RELEASE, 1, 0, 0, XCB_NONE);
    CHECK(!wait_message(A_moveresize, d, &b, 300), "no move/resize from a normal area");
    CHECK(b.downs == 1 && b.ups == 1, "normal press: down %d up %d", b.downs, b.ups);

    /* other buttons are never hit-tested */
    CHECK(pointer_to(60, 10), "pointer back on the draggable area");
    drain();
    memset(&b, 0, sizeof(b));
    fake(XCB_BUTTON_PRESS, 2, 0, 0, XCB_NONE);
    fake(XCB_BUTTON_RELEASE, 2, 0, 0, XCB_NONE);
    CHECK(!wait_message(A_moveresize, d, &b, 300), "no move for the middle button");
    CHECK(b.downs == 1, "middle press reaches the app (%d)", b.downs);

    /* hit test cleared */
    CHECK(SDL_SetWindowHitTest(w, NULL, NULL), "clear the hit test");
    memset(&b, 0, sizeof(b));
    fake(XCB_BUTTON_PRESS, 1, 0, 0, XCB_NONE);
    fake(XCB_BUTTON_RELEASE, 1, 0, 0, XCB_NONE);
    CHECK(!wait_message(A_moveresize, d, &b, 300), "no move without a hit test");
    CHECK(b.downs == 1, "press reaches the app without a hit test (%d)", b.downs);

    /* window menu (last: a WM may open it and grab the pointer) */
    to_root(40, 50, &rx, &ry);
    drain();
    CHECK(SDL_ShowWindowSystemMenu(w, 40, 50), "SDL_ShowWindowSystemMenu");
    const bool menu = wait_message(A_menu, d, NULL, 2000);
    CHECK(menu, "_GTK_SHOW_WINDOW_MENU sent");
    if (menu) {
        CHECK(d[0] == 0 && (int)d[1] == rx && (int)d[2] == ry, "menu at %d,%d (want %d,%d)", (int)d[1], (int)d[2], rx,
              ry);
    }

    SDL_DestroyWindow(w);
    SDL_Quit();
    xcb_disconnect(obs);
    if (failures) {
        fprintf(stderr, "test_x11_hittest: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_x11_hittest: OK\n");
    return 0;
}
