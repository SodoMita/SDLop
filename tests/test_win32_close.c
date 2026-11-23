/*
  SDLop test: the window close button (Win32).

  Clicking the title-bar X is not an SDL call: Win32 turns it into
  WM_SYSCOMMAND/SC_CLOSE, which DefWindowProc delivers as WM_CLOSE. SDL3's
  contract is that this produces SDL_EVENT_WINDOW_CLOSE_REQUESTED and nothing
  else - the application decides whether to destroy the window.

  Two things are checked that are easy to get wrong:
    - the X must actually hit-test as HTCLOSE over the button's pixels, or the
      button is drawn but dead;
    - the event must be attributed to the right window, with the window still
      alive and usable afterwards (a driver that calls DestroyWindow itself
      would crash the app on its next SDL call).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>

#include <windows.h>
#include <stdio.h>

static int failures = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
            failures++;                                         \
        }                                                       \
    } while (0)

static HWND hwnd_of(SDL_Window *window)
{
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

/* Pump until the close event arrives or the budget runs out. */
static bool wait_close(SDL_Window *window, const char *what)
{
    const SDL_WindowID id = SDL_GetWindowID(window);
    SDL_Event event;
    for (int i = 0; i < 200; i++) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                CHECK(event.window.windowID == id,
                      "%s: close event for window %u, expected %u", what,
                      (unsigned)event.window.windowID, (unsigned)id);
                return true;
            }
        }
        SDL_Delay(5);
    }
    return false;
}

int main(void)
{
    SDL_Window *window;
    HWND self;
    RECT rect;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 close", 400, 300, 0);
    CHECK(window != NULL, "SDL_CreateWindow: %s", SDL_GetError());
    if (!window) {
        SDL_Quit();
        return 1;
    }

    /* The HWND must be published: SDL3 applications reach the native window
     * through this property, the way the X11 and Wayland drivers publish
     * theirs. */
    self = hwnd_of(window);
    CHECK(self != NULL, "SDL_PROP_WINDOW_WIN32_HWND_POINTER is not set");
    if (!self) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    CHECK(IsWindow(self), "the HWND is not a window");

    SDL_ShowWindow(window);
    SetForegroundWindow(self);
    for (int i = 0; i < 20; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    /* The X must be where Win32 puts it and report HTCLOSE, otherwise the
     * button is drawn but the click goes to the title bar (a drag) instead. */
    GetWindowRect(self, &rect);
    {
        const LONG x = rect.right - GetSystemMetrics(SM_CXSIZE) / 2 - 1;
        const LONG y = rect.top + GetSystemMetrics(SM_CYSIZE) / 2;
        const LRESULT hit = SendMessageW(self, WM_NCHITTEST, 0, MAKELPARAM(x, y));
        CHECK(hit == HTCLOSE, "hit-test over the close button = %ld, want HTCLOSE", (long)hit);
    }

    /* The click, Alt+F4 and a direct WM_CLOSE all end in the same message. */
    PostMessageW(self, WM_SYSCOMMAND, SC_CLOSE, 0);
    CHECK(wait_close(window, "SC_CLOSE (the X)"), "no close event from the close button");
    PostMessageW(self, WM_CLOSE, 0, 0);
    CHECK(wait_close(window, "WM_CLOSE"), "no close event from WM_CLOSE");

    /* The application owns the destroy: SDL must not have taken the window
     * down behind its back. */
    CHECK(SDL_GetWindowFlags(window) != 0, "the window died on its own");
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        CHECK(w > 0 && h > 0, "window unusable after a refused close (%dx%d)", w, h);
    }

    /* A hidden window reports the request too. */
    SDL_HideWindow(window);
    SDL_PumpEvents();
    PostMessageW(self, WM_CLOSE, 0, 0);
    CHECK(wait_close(window, "hidden window"), "no close event from a hidden window");

    /* And a refused close must still leave the window destroyable. */
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        printf("test_win32_close: FAIL (%d)\n", failures);
        return 1;
    }
    printf("test_win32_close: PASS\n");
    return 0;
}
