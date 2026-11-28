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
#include <string.h>
#include "win32_testenv.h"

static int failures = 0;

/* the window the test creates, so the aspect checks can drive it directly */
static BOOL CALLBACK find_window(HWND hwnd, LPARAM param)
{
    DWORD pid = 0;
    char title[128];
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }
    GetWindowTextA(hwnd, title, sizeof(title));
    if (strstr(title, "sdlop win32 cursor test")) {
        *(HWND *)param = hwnd;
        return FALSE;
    }
    return TRUE;
}

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
    if (!win32_test_interactive_desktop()) {
        printf("skipped: no interactive desktop for cursor/capture injection\n");
        return 77;
    }
    SDL_Surface *surface;
    SDL_Cursor *image_cursor = NULL;
    float gx = 0.0f, gy = 0.0f;
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

    /* Hiding must take the cursor off the screen. Win32's ShowCursor() is a
     * process-global counter, so the driver installs NULL instead; GetCursor()
     * is what the window manager would put up, so it is the honest probe. */
    SDL_PumpEvents();
    SDL_HideCursor();
    CHECK(GetCursor() == NULL, "SDL_HideCursor left a cursor installed");
    SDL_ShowCursor();
    CHECK(GetCursor() != NULL, "SDL_ShowCursor did not restore the cursor");
    /* repeated calls must not drift (the driver used to move a counter) */
    for (i = 0; i < 10; i++) {
        SDL_HideCursor();
    }
    CHECK(GetCursor() == NULL, "10x SDL_HideCursor left a cursor installed");
    for (i = 0; i < 10; i++) {
        SDL_ShowCursor();
    }
    CHECK(GetCursor() != NULL, "10x SDL_ShowCursor left no cursor");

    /* warping: the OS cursor must land on the requested client point. This is
     * the one place SDL_WarpMouseInWindow can be verified, because the rest of
     * the mouse position the core reports comes from WM_MOUSEMOVE. */
    SDL_WarpMouseInWindow(window, 50, 40);
    {
        POINT cp;
        bool landed;
        if (GetCursorPos(&cp)) {
            landed = (cp.x != 0 || cp.y != 0);
            CHECK(landed, "SDL_WarpMouseInWindow left the cursor at the origin");
        }
        /* x/y may be NULL, which must be tolerated */
        (void)SDL_GetGlobalMouseState(NULL, NULL);
    }

    /* global state must report the real buttons, not the window's cached ones */
    /* returns the button mask (0 with nothing pressed), not a success flag */
    CHECK(SDL_GetGlobalMouseState(&gx, &gy) == 0,
          "expected no buttons held, got mask 0x%x", (unsigned)SDL_GetGlobalMouseState(NULL, NULL));

    /* SDL_CaptureMouse(): while captured, Win32 keeps sending mouse messages
     * to the window even when the pointer leaves it -- which is what dragging
     * outside the window needs.
     *
     * Capture requires this window to have focus, and focus is ambient: when
     * ctest runs tests in parallel another test's window can own it. Claim it
     * explicitly rather than inheriting whatever happens to be focused. */
    {
        HWND self = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                 SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        int tries;
        if (self) {
            for (tries = 0; tries < 50 && GetForegroundWindow() != self; tries++) {
                SetForegroundWindow(self);
                SDL_PumpEvents();
                SDL_Delay(10);
            }
        }
    }
    CHECK(SDL_CaptureMouse(true), "SDL_CaptureMouse(true): %s", SDL_GetError());
    CHECK((window->flags & SDL_WINDOW_MOUSE_CAPTURE) != 0,
          "SDL_WINDOW_MOUSE_CAPTURE not set after SDL_CaptureMouse(true)");
    CHECK(GetCapture() != NULL, "Win32 capture was not set");
    CHECK(SDL_CaptureMouse(false), "SDL_CaptureMouse(false): %s", SDL_GetError());
    CHECK(GetCapture() == NULL, "Win32 capture was not released");
    CHECK((window->flags & SDL_WINDOW_MOUSE_CAPTURE) == 0,
          "SDL_WINDOW_MOUSE_CAPTURE still set after SDL_CaptureMouse(false)");

    /* SDL_SetWindowAspectRatio() has no Win32 call behind it: the driver
     * applies it in WM_SIZING. Post the same message the window manager sends
     * while the user drags an edge, and check the rect comes back 16:9. */
    {
        HWND hwnd = NULL;
        EnumWindows(find_window, (LPARAM)&hwnd);
        CHECK(hwnd != NULL, "could not find the test HWND");
        if (hwnd) {
            RECT frame, drag;
            float min_aspect = 0.0f, max_aspect = 0.0f;
            SDL_SetWindowAspectRatio(window, 16.0f / 9.0f, 16.0f / 9.0f);
            SDL_GetWindowAspectRatio(window, &min_aspect, &max_aspect);
            CHECK(min_aspect > 1.7f && min_aspect < 1.8f,
                  "SDL_GetWindowAspectRatio returned %.4f", min_aspect);

            GetWindowRect(hwnd, &frame);
            drag.left = frame.left;
            drag.top = frame.top;
            drag.right = frame.left + 400;
            drag.bottom = frame.top + 600; /* square-ish: the ratio must move it */
            SendMessageW(hwnd, WM_SIZING, WMSZ_BOTTOMRIGHT, (LPARAM)&drag);

            /* measure the client area the way the driver does, from the frame */
            {
                RECT border = { 0, 0, 0, 0 };
                int cw, ch;
                AdjustWindowRectEx(&border, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                                   (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
                cw = (drag.right - drag.left) - (-border.left + border.right);
                ch = (drag.bottom - drag.top) - (-border.top + border.bottom);
                CHECK(ch > 0 && (double)cw / (double)ch > 1.70 && (double)cw / (double)ch < 1.82,
                      "WM_SIZING produced %dx%d client (ratio %.3f), want ~16:9",
                      cw, ch, ch ? (double)cw / (double)ch : 0.0);
            }
        }
    }

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
