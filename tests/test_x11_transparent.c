/*
  SDLop test: X11 SDL_WINDOW_TRANSPARENT (ARGB visual).

    - a transparent window gets a depth-32 TrueColor ARGB visual; its window
      surface is ARGB8888, starts fully transparent and keeps the alpha of
      what is drawn (read back with GetImage)
    - SDLop_SetWindowClearColor on it gives an opaque background
    - an ordinary window keeps the root depth and an XRGB8888 surface
    - SDL_WINDOW_OPENGL | SDL_WINDOW_TRANSPARENT: depth 32 as well, and a
      GL context works on it whenever one works on an ordinary window
  Skips cleanly when DISPLAY is unset or the server has no ARGB visual.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
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

static xcb_window_t xid_of(SDL_Window *w)
{
    return (xcb_window_t)SDL_GetNumberProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
}

static bool is_argb(xcb_visualid_t v)
{
    for (xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen); di.rem; xcb_depth_next(&di)) {
        if (di.data->depth != 32) {
            continue;
        }
        for (xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data); vi.rem; xcb_visualtype_next(&vi)) {
            if (vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR && vi.data->red_mask == 0xff0000 &&
                vi.data->green_mask == 0xff00 && vi.data->blue_mask == 0xff && (!v || vi.data->visual_id == v)) {
                return true;
            }
        }
    }
    return false;
}

static uint8_t depth_of(xcb_window_t w)
{
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(obs, xcb_get_geometry(obs, w), NULL);
    uint8_t d = g ? g->depth : 0;
    free(g);
    return d;
}

static xcb_visualid_t visual_of(xcb_window_t w)
{
    xcb_get_window_attributes_reply_t *a =
        xcb_get_window_attributes_reply(obs, xcb_get_window_attributes(obs, w), NULL);
    xcb_visualid_t v = a ? a->visual : 0;
    free(a);
    return v;
}

/* pixel (x, y) of a depth-32 window as 0xAARRGGBB, after the server caught up */
static bool read_pixel(xcb_window_t w, int x, int y, uint32_t *out)
{
    xcb_get_image_reply_t *img = xcb_get_image_reply(
        obs, xcb_get_image(obs, XCB_IMAGE_FORMAT_Z_PIXMAP, w, (int16_t)x, (int16_t)y, 1, 1, 0xffffffffu), NULL);
    bool ok = img && xcb_get_image_data_length(img) >= 4;
    if (ok) {
        const uint8_t *p = xcb_get_image_data(img);
        *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    free(img);
    return ok;
}

static bool wait_pixel(xcb_window_t w, int x, int y, uint32_t want, uint32_t *got)
{
    const Uint64 end = SDL_GetTicks() + 2000;
    do {
        SDL_PumpEvents();
        if (read_pixel(w, x, y, got) && *got == want) {
            return true;
        }
        SDL_Delay(10);
    } while (SDL_GetTicks() < end);
    return false;
}

static void settle(void)
{
    const Uint64 end = SDL_GetTicks() + 300;
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
        }
        SDL_Delay(5);
    }
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_x11_transparent: SKIP (DISPLAY not set)\n");
        return 0;
    }
    setenv("SDL_VIDEODRIVER", "x11", 1);
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
    if (!is_argb(0)) {
        printf("test_x11_transparent: SKIP (no ARGB visual)\n");
        xcb_disconnect(obs);
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* ordinary window */
    SDL_Window *plain = SDL_CreateWindow("plain", 64, 48, 0);
    CHECK(plain != NULL, "create window: %s", SDL_GetError());
    if (plain) {
        CHECK(depth_of(xid_of(plain)) == screen->root_depth, "ordinary window: root depth (%d)",
              depth_of(xid_of(plain)));
        SDL_Surface *s = SDL_GetWindowSurface(plain);
        CHECK(s && s->format == SDL_PIXELFORMAT_XRGB8888, "ordinary window surface XRGB8888 (%s)",
              s ? SDL_GetPixelFormatName(s->format) : SDL_GetError());
        SDL_DestroyWindow(plain);
    }

    /* transparent window, software surface */
    SDL_Window *w = SDL_CreateWindow("transparent", 64, 48, SDL_WINDOW_TRANSPARENT);
    CHECK(w != NULL, "create transparent window: %s", SDL_GetError());
    if (!w) {
        return 1;
    }
    const xcb_window_t xid = xid_of(w);
    CHECK(depth_of(xid) == 32, "transparent window: depth 32 (%d)", depth_of(xid));
    CHECK(is_argb(visual_of(xid)), "transparent window: ARGB visual (0x%x)", (unsigned)visual_of(xid));
    SDL_Surface *s = SDL_GetWindowSurface(w);
    CHECK(s && s->format == SDL_PIXELFORMAT_ARGB8888, "transparent surface ARGB8888 (%s)",
          s ? SDL_GetPixelFormatName(s->format) : SDL_GetError());
    settle();
    uint32_t px = 0xdeadbeef;
    if (s) {
        CHECK(SDL_UpdateWindowSurface(w), "update: %s", SDL_GetError());
        CHECK(wait_pixel(xid, 5, 5, 0x00000000u, &px), "starts fully transparent (0x%08x)", px);
        const SDL_Rect r = { 0, 0, 32, 48 };
        SDL_FillSurfaceRect(s, &r, 0x80402010u); /* premultiplied half-transparent */
        CHECK(SDL_UpdateWindowSurface(w), "update: %s", SDL_GetError());
        CHECK(wait_pixel(xid, 5, 5, 0x80402010u, &px), "alpha survives the present (0x%08x)", px);
        CHECK(wait_pixel(xid, 50, 5, 0x00000000u, &px), "undrawn part stays transparent (0x%08x)", px);
    }

    /* an explicit clear colour is opaque */
    SDLop_SetWindowClearColor(w, 0x11, 0x22, 0x33);
    CHECK(wait_pixel(xid, 50, 5, 0xff112233u, &px), "explicit clear colour opaque (0x%08x)", px);
    SDL_DestroyWindow(w);

    /* OpenGL */
    SDL_Window *probe = SDL_CreateWindow("gl probe", 64, 48, SDL_WINDOW_OPENGL);
    SDL_GLContext pctx = probe ? SDL_GL_CreateContext(probe) : NULL;
    const bool have_gl = pctx != NULL;
    if (pctx) {
        SDL_GL_DestroyContext(pctx);
    }
    if (probe) {
        SDL_DestroyWindow(probe);
    }
    SDL_Window *gw = SDL_CreateWindow("gl transparent", 64, 48, SDL_WINDOW_OPENGL | SDL_WINDOW_TRANSPARENT);
    CHECK(gw != NULL, "create GL transparent window: %s", SDL_GetError());
    if (gw) {
        CHECK(depth_of(xid_of(gw)) == 32, "GL transparent window: depth 32 (%d)", depth_of(xid_of(gw)));
        if (have_gl) {
            SDL_GLContext ctx = SDL_GL_CreateContext(gw);
            CHECK(ctx != NULL, "GL context on the ARGB window: %s", SDL_GetError());
            if (ctx) {
                CHECK(SDL_GL_SwapWindow(gw), "swap: %s", SDL_GetError());
                SDL_GL_DestroyContext(ctx);
            }
        } else {
            printf("  (no GL: context part skipped)\n");
        }
        SDL_DestroyWindow(gw);
    }

    SDL_Quit();
    xcb_disconnect(obs);
    if (failures) {
        fprintf(stderr, "test_x11_transparent: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_x11_transparent: OK\n");
    return 0;
}
