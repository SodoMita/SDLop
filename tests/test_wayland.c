/*
  SDLop test: Wayland integration - window lifecycle against a real
  compositor. Skips cleanly when no compositor is running.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);         \
            fprintf(stderr, "\n");                \
            failures++;                           \
        }                                         \
    } while (0)

int main(void)
{
    int failures = 0;

    if (!getenv("WAYLAND_DISPLAY")) {
        printf("test_wayland: SKIP (WAYLAND_DISPLAY not set)\n");
        return 0;
    }
    /* keep the test deterministic: window-system input only */
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    CHECK(strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0, "video driver is wayland");

    SDL_Window *w = SDL_CreateWindow("SDLop wayland test", 320, 200, SDL_WINDOW_RESIZABLE);
    if (!w) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /* pump a few frames so configure events are processed */
    for (int i = 0; i < 10; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    CHECK(SDL_GetWindowTitle(w) && strcmp(SDL_GetWindowTitle(w), "SDLop wayland test") == 0, "title");
    int ww = 0, hh = 0;
    CHECK(SDL_GetWindowSize(w, &ww, &hh), "get size");
    CHECK(ww > 0 && hh > 0, "size positive (%dx%d)", ww, hh);

    /* title change */
    CHECK(SDL_SetWindowTitle(w, "renamed"), "set title");
    SDL_PumpEvents();

    /* explicit resize -> RESIZED event */
    CHECK(SDL_SetWindowSize(w, 400, 300), "set size");
    {
        bool saw = false;
        SDL_Event ev;
        for (int i = 0; i < 20 && !saw; i++) {
            SDL_PumpEvents();
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_WINDOW_RESIZED && ev.window.data1 == 400 && ev.window.data2 == 300) {
                    saw = true;
                }
            }
            SDL_Delay(5);
        }
        CHECK(saw, "RESIZED 400x300 event");
    }
    CHECK(SDL_GetWindowSize(w, &ww, &hh) && ww == 400 && hh == 300, "size now 400x300");

    /* clear color repaint (also exercises shm buffer refill) */
    SDLop_SetWindowClearColor(w, 0x30, 0xA0, 0x40);
    SDL_PumpEvents();

    /* minimize/restore/maximize requests should not crash and flag */
    CHECK(SDL_MinimizeWindow(w), "minimize");
    SDL_PumpEvents();
    CHECK(SDL_MaximizeWindow(w), "maximize");
    for (int i = 0; i < 10; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    CHECK(SDL_RestoreWindow(w), "restore");
    for (int i = 0; i < 10; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    /* fullscreen roundtrip */
    CHECK(SDL_SetWindowFullscreen(w, true), "enter fullscreen");
    for (int i = 0; i < 10; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    CHECK(SDL_SetWindowFullscreen(w, false), "leave fullscreen");
    for (int i = 0; i < 10; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    /* relative mouse mode toggle: succeeds when the compositor provides a
     * seat/pointer; on a seat-less headless compositor it must fail and
     * roll the state back (SDL3 semantics). The full success path is
     * covered deterministically by test_pointer_lock (mini compositor). */
    if (SDL_SetWindowRelativeMouseMode(w, true)) {
        CHECK(SDL_GetWindowRelativeMouseMode(w), "relative mode flag");
        CHECK(SDL_SetWindowRelativeMouseMode(w, false), "relative mode off");
    } else {
        CHECK(!SDL_GetWindowRelativeMouseMode(w), "relative mode flag rolled back on failure");
    }
    for (int i = 0; i < 5; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    /* hide/show */
    CHECK(SDL_HideWindow(w), "hide");
    for (int i = 0; i < 5; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    CHECK(SDL_ShowWindow(w), "show");
    for (int i = 0; i < 5; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    SDL_DestroyWindow(w);
    SDL_Quit();

    if (failures) {
        printf("test_wayland: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_wayland: PASS\n");
    return 0;
}
