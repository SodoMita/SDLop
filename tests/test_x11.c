/*
  SDLop X11 integration test. Run with DISPLAY set (for example under Xvfb);
  skips when no X server is available.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                      \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                     \
            fprintf(stderr, "\n");                           \
            failures++;                                       \
        }                                                     \
    } while (0)

static void pump_briefly(int count)
{
    for (int i = 0; i < count; ++i) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
}

int main(void)
{
    int failures = 0;
    if (!getenv("DISPLAY") || !getenv("DISPLAY")[0]) {
        printf("test_x11: SKIP (DISPLAY not set)\n");
        return 0;
    }

    setenv("SDL_VIDEODRIVER", "x11", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    fprintf(stderr, "test_x11: initializing video\n");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init(x11) failed: %s\n", SDL_GetError());
        return 1;
    }

    fprintf(stderr, "test_x11: video initialized\n");
    CHECK(SDL_GetCurrentVideoDriver() &&
          strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0,
          "selected x11 video driver (got %s)", SDL_GetCurrentVideoDriver());

    int display_count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&display_count);
    CHECK(displays != NULL && display_count >= 1,
          "X11 exposes at least one display (count %d)", display_count);
    if (displays && display_count > 0) {
        SDL_Rect bounds = { 0, 0, 0, 0 };
        CHECK(SDL_GetDisplayBounds(displays[0], &bounds), "read display bounds");
        CHECK(bounds.w > 0 && bounds.h > 0,
              "display dimensions are positive (%dx%d)", bounds.w, bounds.h);
        CHECK(SDL_GetPrimaryDisplay() == displays[0], "first X11 display is primary");
        free(displays);
    }

    fprintf(stderr, "test_x11: creating window\n");
    SDL_Window *window = SDL_CreateWindow("SDLop X11 integration", 320, 200,
                                           SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow(x11) failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    fprintf(stderr, "test_x11: pumping after create\n");
    pump_briefly(10);
    fprintf(stderr, "test_x11: reading title\n");
    const char *title = SDL_GetWindowTitle(window);
    CHECK(title && strcmp(title, "SDLop X11 integration") == 0,
          "window title");

    fprintf(stderr, "test_x11: creating software surface\n");
    SDL_Surface *surface = SDL_GetWindowSurface(window);
    fprintf(stderr, "test_x11: surface created\n");
    CHECK(surface != NULL, "create XImage-backed window surface: %s", SDL_GetError());
    if (surface) {
        CHECK(surface->w == 320 && surface->h == 200,
              "initial surface dimensions (%dx%d)", surface->w, surface->h);
        CHECK(surface->pixels != NULL && surface->pitch > 0,
              "surface has pixel storage and a positive pitch");
        CHECK(SDL_FillSurfaceRect(surface, NULL,
                                  SDL_MapSurfaceRGB(surface, 0xD0, 0x40, 0x20)),
              "fill window surface");
        CHECK(SDL_UpdateWindowSurface(window), "present XImage surface: %s", SDL_GetError());

        if (surface->format == SDL_PIXELFORMAT_XRGB8888 ||
            surface->format == SDL_PIXELFORMAT_XBGR8888 ||
            surface->format == SDL_PIXELFORMAT_ARGB8888 ||
            surface->format == SDL_PIXELFORMAT_ABGR8888) {
            Uint8 r = 0, g = 0, b = 0;
            CHECK(SDL_ReadSurfacePixel(surface, 0, 0, &r, &g, &b, NULL),
                  "read XImage surface pixel");
            CHECK(r == 0xD0 && g == 0x40 && b == 0x20,
                  "surface pixel is red/orange (%02x %02x %02x)", r, g, b);
        }
    }

    fprintf(stderr, "test_x11: setting title and size\n");
    CHECK(SDL_SetWindowTitle(window, "SDLop X11 renamed"), "set window title");
    CHECK(SDL_SetWindowSize(window, 400, 240), "resize X11 window: %s", SDL_GetError());
    pump_briefly(10);
    int width = 0, height = 0;
    CHECK(SDL_GetWindowSize(window, &width, &height), "get window size");
    CHECK(width == 400 && height == 240,
          "window size follows resize (%dx%d)", width, height);
    surface = SDL_GetWindowSurface(window);
    CHECK(surface && surface->w == 400 && surface->h == 240,
          "window surface follows resize (%dx%d)",
          surface ? surface->w : 0, surface ? surface->h : 0);

    fprintf(stderr, "test_x11: destroying and recreating software surface\n");
    CHECK(SDL_DestroyWindowSurface(window), "destroy X11 window surface");
    CHECK(!SDL_WindowHasSurface(window), "surface is destroyed");
    surface = SDL_GetWindowSurface(window);
    CHECK(surface && surface->w == 400 && surface->h == 240,
          "window surface can be recreated");

    fprintf(stderr, "test_x11: maximize and restore\n");
    CHECK(SDL_MaximizeWindow(window), "maximize X11 window");
    pump_briefly(5);
    CHECK(SDL_RestoreWindow(window), "restore X11 window");
    pump_briefly(5);
    fprintf(stderr, "test_x11: enter and leave fullscreen\n");
    CHECK(SDL_SetWindowFullscreen(window, true), "enter fullscreen");
    pump_briefly(5);
    CHECK(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN,
          "fullscreen flag after enter");
    CHECK(SDL_SetWindowFullscreen(window, false), "leave fullscreen");
    pump_briefly(5);
    CHECK(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN),
          "fullscreen flag after leave");

    fprintf(stderr, "test_x11: relative mouse mode\n");
    /* XGrabPointer can legitimately fail on a headless server with an
     * unmapped/obscured window; on success, verify a clean off transition. */
    if (SDL_SetWindowRelativeMouseMode(window, true)) {
        CHECK(SDL_GetWindowRelativeMouseMode(window), "relative mouse mode on");
        CHECK(SDL_SetWindowRelativeMouseMode(window, false), "relative mouse mode off");
    } else {
        CHECK(!SDL_GetWindowRelativeMouseMode(window),
              "relative mouse flag rolls back after a failed grab");
    }

    fprintf(stderr, "test_x11: hide/show/destroy\n");
    CHECK(SDL_HideWindow(window), "hide X11 window");
    pump_briefly(2);
    CHECK(SDL_ShowWindow(window), "show X11 window");
    pump_briefly(5);

    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) {
        printf("test_x11: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_x11: PASS\n");
    return 0;
}
