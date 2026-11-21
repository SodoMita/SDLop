/*
  SDLop test: SDL_SetWindowShape() on the Windows driver.

  SDL_WINDOW_TRANSPARENT windows can be clipped to a shape; Win32 expresses
  that as a window region, so the driver turns the shape's alpha channel into
  one span per run of opaque pixels and calls SetWindowRgn(). Without it,
  SDL_SetWindowShape() accepted the call and did nothing -- the window stayed
  a plain rectangle.

  The test builds a shape that is opaque only in the left half, checks the
  window region really is that half, then clears the shape and checks the
  window is a normal rectangle again.

  Needs a window, so ctest runs it under Wine with a display.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define SHAPE_W 200
#define SHAPE_H 200

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

static BOOL CALLBACK find_window(HWND hwnd, LPARAM param)
{
    DWORD pid = 0;
    char title[128];
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }
    GetWindowTextA(hwnd, title, sizeof(title));
    if (strstr(title, "sdlop win32 shape test")) {
        *(HWND *)param = hwnd;
        return FALSE;
    }
    return TRUE;
}

int main(void)
{
    SDL_Window *window;
    SDL_Surface *shape;
    Uint32 *pixels;
    HWND hwnd = NULL;
    HRGN region;
    RECT box;
    int kind;
    int x, y;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    /* borderless: the region is then exactly the shape, with no frame spans */
    window = SDL_CreateWindow("sdlop win32 shape test", SHAPE_W, SHAPE_H,
                              SDL_WINDOW_TRANSPARENT | SDL_WINDOW_BORDERLESS);
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

    pixels = (Uint32 *)SDL_calloc((size_t)SHAPE_W * SHAPE_H, sizeof(Uint32));
    if (!pixels) {
        fprintf(stderr, "SDL_calloc failed\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    /* opaque left half only */
    for (y = 0; y < SHAPE_H; y++) {
        for (x = 0; x < SHAPE_W / 2; x++) {
            pixels[y * SHAPE_W + x] = 0xFFFFFFFF;
        }
    }
    shape = SDL_CreateSurfaceFrom(SHAPE_W, SHAPE_H, SDL_PIXELFORMAT_ARGB8888, pixels, SHAPE_W * 4);
    if (!shape) {
        fprintf(stderr, "SDL_CreateSurfaceFrom failed: %s\n", SDL_GetError());
        SDL_free(pixels);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    CHECK(SDL_SetWindowShape(window, shape), "SDL_SetWindowShape: %s", SDL_GetError());

    region = CreateRectRgn(0, 0, 0, 0);
    kind = GetWindowRgn(hwnd, region);
    GetRgnBox(region, &box);
    CHECK(kind != ERROR, "the window has no region after SDL_SetWindowShape");
    /* the right half must be cut away */
    CHECK(box.right <= SHAPE_W / 2 + 1,
          "the region reaches x=%ld, so the transparent half was not cut (shape is %d wide)",
          (long)box.right, SHAPE_W);
    CHECK(box.bottom >= SHAPE_H - 1,
          "the region is only %ld tall, want the full %d", (long)box.bottom, SHAPE_H);

    /* clearing the shape makes the window rectangular again; GetWindowRgn
     * reports that as "no region" (0) rather than as a rectangle. */
    CHECK(SDL_SetWindowShape(window, NULL), "clearing the shape: %s", SDL_GetError());
    kind = GetWindowRgn(hwnd, region);
    CHECK(kind == 0 || kind == ERROR,
          "still clipped after clearing the shape (GetWindowRgn = %d)", kind);

    DeleteObject(region);
    SDL_DestroySurface(shape);
    SDL_free(pixels);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_shape: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_shape: shape becomes a window region, clearing restores it\n");
    return 0;
}
