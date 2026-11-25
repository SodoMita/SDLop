/*
  SDLop test: SDL_SetWindowHitTest() on the Windows driver (WM_NCHITTEST).

  Applications that draw their own title bar depend on this: they mark a strip
  as SDL_HITTEST_DRAGGABLE so the window manager moves the window, and mark
  edges as resize borders. The Windows driver answered every WM_NCHITTEST with
  DefWindowProc, so such windows were draggable nowhere and SDL_SetWindowHitTest
  returned SDL_Unsupported().

  The test installs a hit-test callback that draws a title strip, a top border
  and a left border, sends the same WM_NCHITTEST the window manager sends, and
  checks the answer is HTCAPTION / HTTOP / HTLEFT / HTCLIENT. It also checks
  SDL_EVENT_WINDOW_HIT_TEST is emitted for each non-normal answer and that
  clearing the callback restores the default.

  Needs a window, so ctest runs it under Wine with a display.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include "internal/sdlop_internal.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

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

/* client coordinates, 400x300 window */
#define TITLE_HEIGHT 40
#define BORDER 8

static SDL_HitTestResult SDLCALL hit_test(SDL_Window *window, const SDL_Point *point, void *data)
{
    (void)window;
    (void)data;
    if (point->y < BORDER) {
        return SDL_HITTEST_RESIZE_TOP;
    }
    if (point->x < BORDER) {
        return SDL_HITTEST_RESIZE_LEFT;
    }
    if (point->y < TITLE_HEIGHT && point->x > BORDER && point->x < 300) {
        return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
}

static BOOL CALLBACK find_window(HWND hwnd, LPARAM param)
{
    DWORD pid = 0;
    char title[128];
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }
    GetWindowTextA(hwnd, title, sizeof(title));
    if (strstr(title, "sdlop win32 hit test")) {
        *(HWND *)param = hwnd;
        return FALSE;
    }
    return TRUE;
}

/* the window manager always asks in screen coordinates */
static LRESULT nchittest(HWND hwnd, int client_x, int client_y)
{
    POINT pt;
    pt.x = client_x;
    pt.y = client_y;
    ClientToScreen(hwnd, &pt);
    return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y));
}

int main(void)
{
    SDL_Window *window;
    HWND hwnd = NULL;
    LRESULT without_hit_test;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 hit test", 400, 300, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_PumpEvents();
    EnumWindows(find_window, (LPARAM)&hwnd);
    if (!hwnd) {
        fprintf(stderr, "could not find the test HWND\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    without_hit_test = nchittest(hwnd, 200, 20);

    if (!SDL_SetWindowHitTest(window, hit_test, NULL)) {
        CHECK(0, "SDL_SetWindowHitTest returned Unsupported: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    CHECK(nchittest(hwnd, 200, 20) == HTCAPTION,
          "draggable area answered %ld, want HTCAPTION (%d)",
          (long)nchittest(hwnd, 200, 20), HTCAPTION);
    CHECK(nchittest(hwnd, 398, 3) == HTTOP,
          "top border answered %ld, want HTTOP (%d)",
          (long)nchittest(hwnd, 398, 3), HTTOP);
    CHECK(nchittest(hwnd, 3, 150) == HTLEFT,
          "left border answered %ld, want HTLEFT (%d)",
          (long)nchittest(hwnd, 3, 150), HTLEFT);
    CHECK(nchittest(hwnd, 200, 200) == HTCLIENT,
          "normal area answered %ld, want HTCLIENT (%d)",
          (long)nchittest(hwnd, 200, 200), HTCLIENT);

    /* one event per non-normal answer */
    {
        int hits = 0;
        SDL_Event e;
        SDL_PumpEvents();
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_WINDOW_HIT_TEST) {
                hits++;
            }
        }
        CHECK(hits >= 3, "only %d SDL_EVENT_WINDOW_HIT_TEST events, want 3", hits);
    }

    /* clearing must put the default behaviour back */
    CHECK(SDL_SetWindowHitTest(window, NULL, NULL), "clearing the hit test failed");
    CHECK(nchittest(hwnd, 200, 20) == without_hit_test,
          "after clearing, the answer changed (%ld, was %ld)",
          (long)nchittest(hwnd, 200, 20), (long)without_hit_test);

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_hittest: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_hittest: drag, resize and normal regions answer correctly\n");
    return 0;
}
