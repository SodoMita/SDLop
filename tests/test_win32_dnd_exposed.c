/*
  SDLop test: Win32 drag-and-drop and WM_PAINT exposure, plus input state
  resynchronisation when the window loses focus.

  The HDROP and the messages are synthetic, so no desktop, mouse or real drag
  source is needed (unlike the input-injection tests).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

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

static void pump(void)
{
    SDL_Delay(40);
    SDL_PumpEvents();
}

int main(void)
{
    SDL_Window *window;
    HWND hwnd;
    SDL_Event ev;
    int files, complete, exposed, keyups;
    char path_utf8[256];

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    window = SDL_CreateWindow("sdlop win32 dnd", 320, 240, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    SDL_ShowWindow(window);
    pump();
    hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    CHECK(hwnd != NULL, "no HWND");
    if (!hwnd) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 2;
    }

    /* --- a real HDROP, delivered through the window procedure --------- */
    /* This SDL3 subset enables events with SDL_SetEventEnabled (no SDL2-style
     * SDL_EventState/SDL_ENABLE); enabling DROP_FILE is what opts into DnD. */
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);
    CHECK(SDL_EventEnabled(SDL_EVENT_DROP_FILE),
          "could not opt into SDL_EVENT_DROP_FILE: %s", SDL_GetError());
    {
        const wchar_t *path = L"C:\\headless\\drop-test.txt";
        const size_t chars = wcslen(path) + 1;
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE,
                                  sizeof(DROPFILES) + (chars + 1) * sizeof(wchar_t));
        CHECK(mem != NULL, "GlobalAlloc for HDROP");
        if (mem) {
            DROPFILES *drop = (DROPFILES *)GlobalLock(mem);
            memset(drop, 0, sizeof(DROPFILES));
            drop->pFiles = sizeof(DROPFILES);
            drop->fWide = TRUE;
            wchar_t *dst = (wchar_t *)((unsigned char *)drop + drop->pFiles);
            memcpy(dst, path, chars * sizeof(wchar_t));
            dst[chars] = 0; /* the double NUL that ends a CF_HDROP */
            GlobalUnlock(mem);
            PostMessageW(hwnd, WM_DROPFILES, (WPARAM)mem, 0);
        }

        files = complete = exposed = keyups = 0;
        path_utf8[0] = '\0';
        pump();
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_DROP_FILE) {
                files++;
                if (ev.drop.data) {
                    SDL_snprintf(path_utf8, sizeof(path_utf8), "%s", ev.drop.data);
                }
            } else if (ev.type == SDL_EVENT_DROP_COMPLETE) {
                complete++;
            }
        }
        CHECK(files == 1, "WM_DROPFILES produced %d DROP_FILE events, want 1", files);
        CHECK(strcmp(path_utf8, "C:\\headless\\drop-test.txt") == 0,
              "dropped path was \"%s\"", path_utf8);
        CHECK(complete == 1, "DROP_COMPLETE count %d, want 1", complete);
    }

    /* --- WM_PAINT -> SDL_EVENT_WINDOW_EXPOSED -------------------------- */
    PostMessageW(hwnd, WM_PAINT, 0, 0);
    pump();
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_WINDOW_EXPOSED) {
            exposed++;
        }
    }
    CHECK(exposed >= 1, "WM_PAINT produced %d EXPOSED events", exposed);

    /* --- focus loss must release keys held before Alt-Tab -------------- */
    PostMessageW(hwnd, WM_SETFOCUS, 0, 0);
    pump();
    {
        const LPARAM down = (LPARAM)(1u | ((UINT)0x1E << 16)); /* VK_A, scan 0x1E */
        PostMessageW(hwnd, WM_KEYDOWN, 0x41, down); /* VK_A */
    }
    pump();
    CHECK(SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A] != 0, "A was not held before focus loss");
    PostMessageW(hwnd, WM_KILLFOCUS, 0, 0);
    pump();
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_KEY_UP && ev.key.scancode == SDL_SCANCODE_A) {
            keyups++;
        }
    }
    CHECK(keyups >= 1, "focus loss produced %d A key-up events", keyups);
    CHECK(SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A] == 0, "A is still held after focus loss");

    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) {
        printf("test_win32_dnd_exposed: FAIL (%d)\n", failures);
        return 1;
    }
    puts("test_win32_dnd_exposed: PASS");
    return 0;
}
