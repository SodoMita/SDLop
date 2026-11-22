/*
  SDLop test: X11 SDL_SetWindowShape (XShape input shape, as in stock).

    - only SDL_WINDOW_TRANSPARENT windows take a shape
    - the alpha channel, stretched to the window size, becomes the input
      shape: alpha 0 lets clicks through
    - the shape follows window resizes
    - SDL_SetWindowShape(NULL) restores the full input region
    - tooltips get an empty input shape
    - SDLOP_LIB_XCB_SHAPE=none: SDL_SetWindowShape reports unsupported
  Skips cleanly when DISPLAY is unset or the server lacks SHAPE.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/shape.h>
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

static xcb_window_t xid_of(SDL_Window *w)
{
    return (xcb_window_t)SDL_GetNumberProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
}

/* the input shape of win: rectangle count, and whether (x, y) is inside */
static int input_rects(xcb_window_t win, int x, int y, bool *inside)
{
    xcb_shape_get_rectangles_reply_t *r =
        xcb_shape_get_rectangles_reply(obs, xcb_shape_get_rectangles(obs, win, XCB_SHAPE_SK_INPUT), NULL);
    if (!r) {
        return -1;
    }
    const xcb_rectangle_t *rect = xcb_shape_get_rectangles_rectangles(r);
    const int n = xcb_shape_get_rectangles_rectangles_length(r);
    *inside = false;
    for (int i = 0; i < n; i++) {
        if (x >= rect[i].x && y >= rect[i].y && x < rect[i].x + rect[i].width && y < rect[i].y + rect[i].height) {
            *inside = true;
        }
    }
    free(r);
    return n;
}

/* waits until (x, y) has the wanted state (the requests of the SDL
 * connection are not ordered with ours) */
static bool wait_point(xcb_window_t win, int x, int y, bool want)
{
    const Uint64 end = SDL_GetTicks() + 2000;
    bool inside = !want;
    do {
        SDL_PumpEvents();
        if (input_rects(win, x, y, &inside) >= 0 && inside == want) {
            return true;
        }
        SDL_Delay(10);
    } while (SDL_GetTicks() < end);
    return false;
}

/* opaque left quarter and top band, transparent elsewhere */
static SDL_Surface *make_shape(int w, int h)
{
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    if (s) {
        SDL_FillSurfaceRect(s, NULL, 0x00000000u);
        const SDL_Rect left = { 0, 0, w / 4, h };
        const SDL_Rect top = { 0, 0, w, h / 8 };
        SDL_FillSurfaceRect(s, &left, 0xffffffffu);
        SDL_FillSurfaceRect(s, &top, 0xffffffffu);
    }
    return s;
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_x11_shape: SKIP (DISPLAY not set)\n");
        return 0;
    }
    setenv("SDL_VIDEODRIVER", "x11", 1);
    obs = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(obs)) {
        fprintf(stderr, "xcb_connect failed\n");
        return 1;
    }
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(obs, &xcb_shape_id);
    if (!ext || !ext->present) {
        printf("test_x11_shape: SKIP (no SHAPE extension)\n");
        xcb_disconnect(obs);
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const char *mod = getenv("SDLOP_LIB_XCB_SHAPE");
    const bool no_module = mod && strcmp(mod, "none") == 0;
    SDL_Surface *shape = make_shape(50, 40);
    CHECK(shape != NULL, "shape surface: %s", SDL_GetError());
    if (!shape) {
        return 1;
    }

    /* opaque windows take no shape */
    SDL_Window *opaque = SDL_CreateWindow("opaque", 100, 80, 0);
    CHECK(opaque && !SDL_SetWindowShape(opaque, shape), "SDL_SetWindowShape refuses an opaque window");
    SDL_DestroyWindow(opaque);

    SDL_Window *w = SDL_CreateWindow("shaped", 100, 80, SDL_WINDOW_TRANSPARENT);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (!w) {
        return 1;
    }
    const xcb_window_t xid = xid_of(w);
    if (no_module) {
        CHECK(!SDL_SetWindowShape(w, shape), "without libxcb-shape SDL_SetWindowShape fails");
        SDL_Window *tip = SDL_CreatePopupWindow(w, 10, 10, 40, 20, SDL_WINDOW_TOOLTIP);
        CHECK(tip != NULL, "tooltip without libxcb-shape: %s", SDL_GetError());
        SDL_DestroyWindow(tip);
        SDL_DestroyWindow(w);
        SDL_DestroySurface(shape);
        SDL_Quit();
        xcb_disconnect(obs);
        printf("test_x11_shape: %s\n", failures ? "FAIL" : "OK (no module)");
        return failures ? 1 : 0;
    }

    CHECK(SDL_SetWindowShape(w, shape), "SDL_SetWindowShape: %s", SDL_GetError());
    /* 50x40 stretched to 100x80: left 25 px and top 10 px take input */
    CHECK(wait_point(xid, 10, 40, true), "opaque left part takes input");
    CHECK(wait_point(xid, 80, 5, true), "opaque top band takes input");
    CHECK(wait_point(xid, 80, 50, false), "transparent part lets input through");

    /* resize: the shape is stretched again */
    bool resized = false;
    SDL_SetWindowSize(w, 200, 160);
    const Uint64 end = SDL_GetTicks() + 3000;
    while (!resized && SDL_GetTicks() < end) {
        SDL_Event ev;
        while (SDL_WaitEventTimeout(&ev, 10)) {
            resized = resized || (ev.type == SDL_EVENT_WINDOW_RESIZED && ev.window.data1 == 200);
        }
    }
    CHECK(resized, "window resized to 200x160");
    if (resized) {
        CHECK(wait_point(xid, 40, 100, true), "left part after the resize (x 40 < 50)");
        CHECK(wait_point(xid, 150, 12, true), "top band after the resize (y 12 < 20)");
        CHECK(wait_point(xid, 150, 100, false), "transparent part after the resize");
    }

    /* reset */
    CHECK(SDL_SetWindowShape(w, NULL), "SDL_SetWindowShape(NULL): %s", SDL_GetError());
    CHECK(wait_point(xid, 150, 100, true), "no shape: the whole window takes input");

    /* tooltips take no input */
    SDL_Window *tip = SDL_CreatePopupWindow(w, 10, 10, 40, 20, SDL_WINDOW_TOOLTIP);
    CHECK(tip != NULL, "tooltip: %s", SDL_GetError());
    if (tip) {
        bool inside = true;
        int n = input_rects(xid_of(tip), 5, 5, &inside);
        CHECK(n == 0 && !inside, "tooltip input shape is empty (%d rects)", n);
        SDL_DestroyWindow(tip);
    }

    SDL_DestroyWindow(w);
    SDL_DestroySurface(shape);
    SDL_Quit();
    xcb_disconnect(obs);
    if (failures) {
        fprintf(stderr, "test_x11_shape: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_x11_shape: OK\n");
    return 0;
}
