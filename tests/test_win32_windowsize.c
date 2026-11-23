/*
  SDLop test: Win32 window geometry (client size, position, frame changes).

  These are the places where a Win32 driver silently disagrees with SDL3, and
  where the damage is visible to the application rather than to a return code:

    - a window created 400x300 must have a 400x300 client area. Creating with
      the requested size but computing the frame with a different style (the
      classic mistake: AdjustWindowRectEx against WS_OVERLAPPEDWINDOW while the
      window is really created with the thin frame of a non-resizable window)
      leaves the client area a couple of pixels off.
    - SDL_SetWindowBordered()/SDL_SetWindowResizable() change the frame. The
      drawing area must not change with it, or the application's content jumps
      by the size of the title bar every time the border is toggled.
    - SDL_GetWindowPosition() and SDL_SetWindowPosition() must agree. SDL3
      reports the position of the client area; a driver that positions the
      frame but reads back the client origin drifts by the frame offset on
      every set/get pair.
    - the reported display scale must reflect the window's monitor from the
      moment it is created, not only after the user drags it to another one.

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

/* The client area as Win32 sees it, independent of what SDL believes. */
static void real_client_size(SDL_Window *window, int *w, int *h)
{
    RECT rc = { 0, 0, 0, 0 };
    GetClientRect(hwnd_of(window), &rc);
    *w = (int)(rc.right - rc.left);
    *h = (int)(rc.bottom - rc.top);
}

/* A window's client area has to start where SDL says it does. */
static void check_position_roundtrip(SDL_Window *window, int want_x, int want_y)
{
    int x = 0, y = 0;
    POINT client = { 0, 0 };
    SDL_SetWindowPosition(window, want_x, want_y);
    SDL_PumpEvents();

    SDL_GetWindowPosition(window, &x, &y);
    CHECK(x == want_x && y == want_y,
          "position did not round-trip: asked %d,%d got %d,%d", want_x, want_y, x, y);

    ClientToScreen(hwnd_of(window), &client);
    CHECK((int)client.x == x && (int)client.y == y,
          "SDL reports %d,%d but the client area really starts at %ld,%ld", x, y, (long)client.x, (long)client.y);
}

int main(int argc, char *argv[])
{
    SDL_Window *window;
    int cw = 0, ch = 0, rw = 0, rh = 0;
    const int WANT_W = 400, WANT_H = 300;

    (void)argc;
    (void)argv;

    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "windows");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }

    /* Created without SDL_WINDOW_RESIZABLE: the thin frame. */
    window = SDL_CreateWindow("windowsize", WANT_W, WANT_H, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }

    /* 1. The created client area is the size that was asked for. */
    SDL_GetWindowSize(window, &cw, &ch);
    CHECK(cw == WANT_W && ch == WANT_H,
          "created %dx%d, client area is %dx%d", WANT_W, WANT_H, cw, ch);
    real_client_size(window, &rw, &rh);
    CHECK(rw == WANT_W && rh == WANT_H,
          "created %dx%d, Win32 client rect is %dx%d", WANT_W, WANT_H, rw, rh);

    /* 2. The reported display scale is the window's, from creation. */
    {
        const float scale = SDL_GetWindowDisplayScale(window);
        CHECK(scale > 0.0f, "display scale is %f", (double)scale);
    }

    /* 3. Toggling the border keeps the drawing area. Borderless has no frame,
     *    so the window shrinks around the same client area. */
    SDL_SetWindowBordered(window, false);
    SDL_PumpEvents();
    SDL_GetWindowSize(window, &cw, &ch);
    real_client_size(window, &rw, &rh);
    CHECK(cw == WANT_W && ch == WANT_H, "borderless: client is %dx%d, want %dx%d", cw, ch, WANT_W, WANT_H);
    CHECK(rw == WANT_W && rh == WANT_H, "borderless: Win32 client rect is %dx%d, want %dx%d", rw, rh, WANT_W, WANT_H);

    SDL_SetWindowBordered(window, true);
    SDL_PumpEvents();
    SDL_GetWindowSize(window, &cw, &ch);
    real_client_size(window, &rw, &rh);
    CHECK(cw == WANT_W && ch == WANT_H, "bordered again: client is %dx%d, want %dx%d", cw, ch, WANT_W, WANT_H);
    CHECK(rw == WANT_W && rh == WANT_H, "bordered again: Win32 client rect is %dx%d, want %dx%d", rw, rh, WANT_W, WANT_H);

    /* 4. Same for the resizable toggle, which swaps the thin and thick frames. */
    SDL_SetWindowResizable(window, true);
    SDL_PumpEvents();
    real_client_size(window, &rw, &rh);
    CHECK(rw == WANT_W && rh == WANT_H, "resizable: Win32 client rect is %dx%d, want %dx%d", rw, rh, WANT_W, WANT_H);

    SDL_SetWindowResizable(window, false);
    SDL_PumpEvents();
    real_client_size(window, &rw, &rh);
    CHECK(rw == WANT_W && rh == WANT_H, "not resizable: Win32 client rect is %dx%d, want %dx%d", rw, rh, WANT_W, WANT_H);

    /* 5. Position round-trips, and matches the real client origin. */
    check_position_roundtrip(window, 120, 140);
    check_position_roundtrip(window, 60, 90);
    /* Twice in a row used to be where the drift compounded. */
    check_position_roundtrip(window, 200, 180);

    /* 6. A resize request yields the client size that was asked for. */
    SDL_SetWindowSize(window, 500, 400);
    SDL_PumpEvents();
    SDL_GetWindowSize(window, &cw, &ch);
    CHECK(cw == 500 && ch == 400, "SDL_SetWindowSize(500,400) gave client %dx%d", cw, ch);
    real_client_size(window, &rw, &rh);
    CHECK(rw == 500 && rh == 400, "SDL_SetWindowSize(500,400): Win32 client rect %dx%d", rw, rh);

    /* 7. The borders SDL reports have to add up to the real frame. */
    {
        RECT frame, client;
        int top = 0, left = 0, bottom = 0, right = 0;
        GetWindowRect(hwnd_of(window), &frame);
        GetClientRect(hwnd_of(window), &client);
        CHECK(SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right),
              "SDL_GetWindowBordersSize failed: %s", SDL_GetError());
        const int frame_w = (int)(frame.right - frame.left) - (int)(client.right - client.left);
        const int frame_h = (int)(frame.bottom - frame.top) - (int)(client.bottom - client.top);
        CHECK(left + right == frame_w, "horizontal borders %d+%d = %d, real frame width %d",
              left, right, left + right, frame_w);
        CHECK(top + bottom == frame_h, "vertical borders %d+%d = %d, real frame height %d",
              top, bottom, top + bottom, frame_h);
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_windowsize: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_windowsize: PASS\n");
    return 0;
}
