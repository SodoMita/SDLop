/*
  SDLop test: X11 SDL_SetWindowMouseRect through XFixes pointer barriers.

  The barriers are checked on the server itself: relative XTest motion (the
  kind barriers act on) is sent from a raw xcb connection and the pointer
  is queried before SDLop pumps, so the warp-back fallback can't stand in
  for them.
    - the pointer stops at each edge of the rect (both axes, both ways)
    - moving the window moves the fence along
    - clearing the rect, hiding the window and SDL_Quit drop the barriers
  With SDLOP_LIB_XCB_XFIXES=none there are no barriers: the pointer is
  warped back once SDLop sees it leave instead, which is checked then.
  Skips cleanly when DISPLAY is unset, XFixes 5 is missing or the window
  never gets the keyboard focus.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
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
static xcb_window_t root;

static void pointer(int *x, int *y)
{
    *x = *y = -1;
    xcb_query_pointer_reply_t *p = xcb_query_pointer_reply(obs, xcb_query_pointer(obs, root), NULL);
    if (p) {
        *x = p->root_x;
        *y = p->root_y;
        free(p);
    }
}

static void warp(int x, int y)
{
    xcb_warp_pointer(obs, XCB_NONE, root, 0, 0, 0, 0, (int16_t)x, (int16_t)y);
    free(xcb_get_input_focus_reply(obs, xcb_get_input_focus(obs), NULL)); /* round trip */
}

/* relative motion in steps, as a mouse sends it; returns where it ended */
static void nudge(int dx, int dy, int *x, int *y)
{
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        xcb_test_fake_input(obs, XCB_MOTION_NOTIFY, 1 /* relative */, XCB_CURRENT_TIME, XCB_NONE,
                            (int16_t)(dx / steps), (int16_t)(dy / steps), 0);
    }
    free(xcb_get_input_focus_reply(obs, xcb_get_input_focus(obs), NULL));
    pointer(x, y);
}

static void pump(Uint64 ms)
{
    const Uint64 end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
        }
        SDL_Delay(5);
    }
}

static bool window_origin(SDL_Window *w, int *x, int *y)
{
    xcb_window_t xid =
        (xcb_window_t)SDL_GetNumberProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    xcb_translate_coordinates_reply_t *t =
        xcb_translate_coordinates_reply(obs, xcb_translate_coordinates(obs, xid, root, 0, 0), NULL);
    if (!t) {
        return false;
    }
    *x = t->dst_x;
    *y = t->dst_y;
    free(t);
    return true;
}

/* rect r (window coords) on a window whose client origin is ox,oy */
static void check_fenced(int ox, int oy, const SDL_Rect *r, const char *what)
{
    const int left = ox + r->x, top = oy + r->y;
    const int right = left + r->w - 1, bottom = top + r->h - 1;
    int x, y;
    warp(left + r->w / 2, top + r->h / 2);
    nudge(400, 0, &x, &y);
    CHECK(x >= right - 1 && x <= right, "%s: moving right ended at x=%d, edge %d", what, x, right);
    nudge(-800, 0, &x, &y);
    CHECK(x >= left && x <= left + 1, "%s: moving left ended at x=%d, edge %d", what, x, left);
    warp(left + r->w / 2, top + r->h / 2);
    nudge(0, 400, &x, &y);
    CHECK(y >= bottom - 1 && y <= bottom, "%s: moving down ended at y=%d, edge %d", what, y, bottom);
    nudge(0, -800, &x, &y);
    CHECK(y >= top && y <= top + 1, "%s: moving up ended at y=%d, edge %d", what, y, top);
}

static bool escapes(int ox, int oy, const SDL_Rect *r)
{
    int x, y;
    warp(ox + r->x + r->w / 2, oy + r->y + r->h / 2);
    nudge(400, 0, &x, &y);
    return x > ox + r->x + r->w;
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("SKIP: no DISPLAY\n");
        return 0;
    }
    obs = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(obs)) {
        printf("SKIP: cannot connect\n");
        return 0;
    }
    root = xcb_setup_roots_iterator(xcb_get_setup(obs)).data->root;
    xcb_xfixes_query_version_reply_t *v =
        xcb_xfixes_query_version_reply(obs, xcb_xfixes_query_version(obs, 5, 0), NULL);
    const bool xfixes5 = v && v->major_version >= 5;
    free(v);
    if (!xfixes5) {
        printf("SKIP: no XFixes 5\n");
        xcb_disconnect(obs);
        return 0;
    }

    SDL_setenv_unsafe("SDL_VIDEO_DRIVER", "x11", 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("barriers", 400, 300, 0);
    CHECK(w != NULL, "create: %s", SDL_GetError());
    SDL_SetWindowPosition(w, 200, 150);
    const Uint64 until = SDL_GetTicks() + 3000;
    while (SDL_GetKeyboardFocus() != w && SDL_GetTicks() < until) {
        pump(20);
    }
    if (SDL_GetKeyboardFocus() != w) {
        printf("SKIP: the window never got the keyboard focus\n");
        SDL_Quit();
        xcb_disconnect(obs);
        return 0;
    }
    pump(300); /* let the move settle */
    int ox, oy;
    CHECK(window_origin(w, &ox, &oy), "window origin");
    printf("window client origin %d,%d\n", ox, oy);

    /* --- 1. fenced in on every side --------------------------------- */
    const SDL_Rect r = { 50, 40, 120, 90 };
    CHECK(SDL_SetWindowMouseRect(w, &r), "SetWindowMouseRect: %s", SDL_GetError());
    const char *mod = getenv("SDLOP_LIB_XCB_XFIXES");
    if (mod && strcmp(mod, "none") == 0) {
        /* fallback: warped back by handle_motion */
        int x, y;
        warp(ox + r.x + r.w / 2, oy + r.y + r.h / 2);
        pump(100);
        nudge(60, 0, &x, &y); /* stays inside the window, outside the rect */
        CHECK(x > ox + r.x + r.w - 1, "the nudge itself was fenced (x=%d) without barriers", x);
        pump(300);
        pointer(&x, &y);
        CHECK(x >= ox + r.x && x <= ox + r.x + r.w - 1, "not warped back into the rect: x=%d", x);
        SDL_Quit();
        xcb_disconnect(obs);
        printf(failures ? "test_x11_barriers (no XFixes): FAIL\n" : "test_x11_barriers (no XFixes): OK\n");
        return failures ? 1 : 0;
    }
    check_fenced(ox, oy, &r, "rect");

    /* --- 2. the fence follows the window ---------------------------- */
    SDL_SetWindowPosition(w, 300, 250);
    int nx = ox, ny = oy;
    const Uint64 move_until = SDL_GetTicks() + 3000;
    while ((nx == ox && ny == oy) && SDL_GetTicks() < move_until) {
        pump(20);
        window_origin(w, &nx, &ny);
    }
    pump(100); /* SDLop sees the ConfigureNotify, barriers re-made */
    if (nx != ox || ny != oy) {
        check_fenced(nx, ny, &r, "after moving");
        ox = nx;
        oy = ny;
    } else {
        printf("note: the window did not move; fence-follow check skipped\n");
    }

    /* --- 3. clearing the rect frees the pointer --------------------- */
    CHECK(SDL_SetWindowMouseRect(w, NULL), "clear rect");
    CHECK(escapes(ox, oy, &r), "pointer still fenced after clearing the rect");

    /* --- 4. hidden windows fence nothing ---------------------------- */
    SDL_SetWindowMouseRect(w, &r);
    pump(50);
    CHECK(!escapes(ox, oy, &r), "rect set again but the pointer escapes");
    SDL_HideWindow(w);
    pump(200);
    CHECK(escapes(ox, oy, &r), "pointer fenced by a hidden window");

    /* --- 5. SDL_Quit leaves nothing behind -------------------------- */
    SDL_ShowWindow(w);
    const Uint64 refocus = SDL_GetTicks() + 3000;
    while (SDL_GetKeyboardFocus() != w && SDL_GetTicks() < refocus) {
        pump(20);
    }
    pump(100);
    window_origin(w, &ox, &oy);
    if (SDL_GetKeyboardFocus() == w) {
        CHECK(!escapes(ox, oy, &r), "fence not back after showing again");
    }
    SDL_Quit();
    CHECK(escapes(ox, oy, &r), "barriers outlived SDL_Quit");

    xcb_disconnect(obs);
    if (failures) {
        fprintf(stderr, "test_x11_barriers: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_x11_barriers: OK\n");
    return 0;
}
