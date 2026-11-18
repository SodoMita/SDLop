/*
  SDLop test: X11 driver graceful failure paths - no X server required.
  Verifies that a missing/unreachable display or missing libX11 produces
  clean SDL errors (and proper fallback to other drivers) instead of
  crashes.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fprintf(stderr, "\n");                             \
            failures++;                                        \
        }                                                      \
    } while (0)

int main(void)
{
    failures = 0;
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    /* 1. SDL_VIDEODRIVER=x11 with no DISPLAY: init must fail cleanly */
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
    setenv("SDL_VIDEODRIVER", "x11", 1);
    if (SDL_Init(SDL_INIT_VIDEO)) {
        CHECK(0, "SDL_Init with x11 and no DISPLAY should have failed");
    } else {
        const char *err = SDL_GetError();
        CHECK(err && err[0], "error string set for missing DISPLAY");
        printf("no-DISPLAY error: %s\n", err ? err : "(null)");
    }
    SDL_Quit();

    /* 2. unreachable display: fast, clean failure (nothing listens on
     * the X11 unix socket for display 63333) */
    setenv("DISPLAY", ":63333", 1);
    setenv("SDL_VIDEODRIVER", "x11", 1);
    if (SDL_Init(SDL_INIT_VIDEO)) {
        CHECK(0, "SDL_Init with unreachable X server should have failed");
    } else {
        printf("unreachable-server error: %s\n", SDL_GetError());
    }
    SDL_Quit();

    /* 3. broken library path: driver init must fail, not crash */
    setenv("DISPLAY", ":0", 1);
    setenv("SDLOP_LIB_X11", "/nonexistent/libX11.so.6", 1);
    if (SDL_Init(SDL_INIT_VIDEO)) {
        CHECK(0, "SDL_Init with missing libX11 should have failed");
    } else {
        printf("missing-lib error: %s\n", SDL_GetError());
    }
    unsetenv("SDLOP_LIB_X11");
    SDL_Quit();

    /* 4. fallback: no SDL_VIDEODRIVER and no DISPLAY -> another driver
     * (dummy) must pick up, so SDL_Init succeeds */
    unsetenv("SDL_VIDEODRIVER");
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        CHECK(0, "fallback init without any display failed: %s", SDL_GetError());
    } else {
        const char *drv = SDL_GetCurrentVideoDriver();
        CHECK(drv != NULL, "a video driver is active");
        CHECK(strcmp(drv, "x11") != 0, "did not select x11 without DISPLAY");
        printf("fallback driver: %s\n", drv ? drv : "(null)");
        /* windows still work on the fallback driver */
        SDL_Window *w = SDL_CreateWindow("fallback", 64, 64, 0);
        CHECK(w != NULL, "window on fallback driver");
        SDL_DestroyWindow(w);
    }
    SDL_Quit();

    /* 5. garbage DISPLAY with no driver hint -> falls back, not x11 */
    setenv("DISPLAY", "not-a-display", 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        CHECK(0, "fallback init with garbage DISPLAY failed: %s", SDL_GetError());
    } else {
        const char *drv = SDL_GetCurrentVideoDriver();
        CHECK(drv && strcmp(drv, "x11") != 0, "garbage DISPLAY did not select x11");
    }
    unsetenv("DISPLAY");
    SDL_Quit();

    if (failures) {
        printf("test_x11_nodisplay: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_x11_nodisplay: PASS\n");
    return 0;
}
