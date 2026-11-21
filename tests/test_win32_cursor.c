/*
  SDLop test: Windows cursors actually get created.

  The driver's WM_SETCURSOR handler installs cursor->internal, but nothing used
  to fill it in: Win32Cursor had no CreateCursor/FreeCursor in the device
  table, so every SDL_CreateSystemCursor() failed with "CreateSystemCursor is
  not currently supported" and no application could show a cursor other than
  whatever Win32 picked, let alone a custom image.

  This creates every SDL system cursor, an image cursor and a legacy 1bpp
  cursor, and checks the driver produced a handle for each. It needs a window,
  so ctest runs it under Wine with a display (like test_win32_keycodes).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include "internal/sdlop_internal.h" /* SDL_Cursor is opaque in the public API */
#include <windows.h>
#include <stdio.h>

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

int main(void)
{
    SDL_Window *window;
    SDL_Surface *surface;
    SDL_Cursor *image_cursor = NULL;
    Uint32 pixels[16 * 16];
    Uint8 data[32], mask[32];
    int i;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 cursor test", 200, 100, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /* every system cursor must come back with a Win32 handle */
    for (i = 0; i < SDL_SYSTEM_CURSOR_COUNT; i++) {
        SDL_Cursor *cursor = SDL_CreateSystemCursor((SDL_SystemCursor)i);
        CHECK(cursor != NULL, "SDL_CreateSystemCursor(%d): %s", i, SDL_GetError());
        if (!cursor) {
            continue;
        }
        CHECK(cursor->internal != NULL, "system cursor %d has no Win32 handle", i);
        SDL_SetCursor(cursor);
        SDL_DestroyCursor(cursor);
    }

    /* an image cursor: an opaque 16x16 white square */
    for (i = 0; i < 16 * 16; i++) {
        pixels[i] = 0xFFFFFFFF;
    }
    surface = SDL_CreateSurfaceFrom(16, 16, SDL_PIXELFORMAT_ARGB8888, pixels, 16 * 4);
    if (surface) {
        image_cursor = SDL_CreateColorCursor(surface, 0, 0);
        CHECK(image_cursor != NULL, "SDL_CreateColorCursor: %s", SDL_GetError());
        if (image_cursor) {
            CHECK(image_cursor->internal != NULL, "image cursor has no Win32 handle");
            SDL_SetCursor(image_cursor);
        }
        SDL_DestroySurface(surface);
    } else {
        CHECK(0, "SDL_CreateSurfaceFrom: %s", SDL_GetError());
    }

    /* the legacy 1bpp path funnels through the same driver hook */
    SDL_memset(data, 0xFF, sizeof(data));
    SDL_memset(mask, 0xFF, sizeof(mask));
    {
        SDL_Cursor *mono = SDL_CreateCursor(data, mask, 16, 16, 0, 0);
        CHECK(mono != NULL, "SDL_CreateCursor: %s", SDL_GetError());
        if (mono) {
            CHECK(mono->internal != NULL, "mono cursor has no Win32 handle");
            SDL_SetCursor(mono);
            SDL_DestroyCursor(mono);
        }
    }

    /* showing and hiding must not leak or crash with a live cursor */
    SDL_HideCursor();
    SDL_ShowCursor();
    SDL_HideCursor();
    SDL_ShowCursor();

    if (image_cursor) {
        SDL_DestroyCursor(image_cursor);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_cursor: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_cursor: %d system cursors, an image cursor and a 1bpp cursor\n",
           SDL_SYSTEM_CURSOR_COUNT);
    return 0;
}
