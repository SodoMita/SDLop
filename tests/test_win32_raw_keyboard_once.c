/*
  SDLop test: a key press must reach the SDL queue exactly once.

  On Windows a keypress arrives twice when a raw keyboard path is on: once as
  a raw packet (SDL_HINT_WINDOWS_RAW_KEYBOARD or the raw input worker) and
  once as WM_KEYDOWN/WM_KEYUP. Measured before this was handled:

      LSHIFT -> DOWN Left Shift UP Left Shift UP Left Shift

  -- every release delivered twice. The opposite mistake is worse and was
  also made: standing down WM_KEY* unconditionally made keys vanish entirely
  while text input was active, because the raw path deliberately ignores
  presses then (the IME owns the keyboard).

  So this test runs both configurations -- text input off and on -- and
  requires exactly one DOWN and one UP per injected key in each, with the
  correct left/right scancode.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "win32_testenv.h"

static int failures = 0;

/* Focus is process-global, so an injecting test must put back what it took or
 * the next test inherits a window it does not own. */
static HWND focus_before_injection;

static void restore_focus(void)
{
    if (focus_before_injection) {
        SetForegroundWindow(focus_before_injection);
        focus_before_injection = NULL;
    }
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

static void inject(WORD scan, int extended)
{
    INPUT in[2];
    DWORD flags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0);

    SDL_zeroa(in);
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wScan = scan;
    in[0].ki.dwFlags = flags;
    in[1] = in[0];
    in[1].ki.dwFlags = flags | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    Sleep(30);
}

/* Count the DOWN/UP events for one key; return them in down/up. */
static void count_key(SDL_Scancode want, int *downs, int *ups)
{
    SDL_Event e;
    int i;

    *downs = 0;
    *ups = 0;
    for (i = 0; i < 20; i++) {
        SDL_PumpEvents();
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == want && !e.key.repeat) {
                (*downs)++;
            } else if (e.type == SDL_EVENT_KEY_UP && e.key.scancode == want) {
                (*ups)++;
            }
        }
        Sleep(5);
    }
}

static int run_case(const char *label, int text_input)
{
    static const struct {
        WORD scan;
        int extended;
        SDL_Scancode want;
        const char *name;
    } cases[] = {
        { 0x2A, 0, SDL_SCANCODE_LSHIFT, "LSHIFT" },
        { 0x36, 0, SDL_SCANCODE_RSHIFT, "RSHIFT" },
        { 0x1D, 0, SDL_SCANCODE_LCTRL,  "LCTRL"  },
        { 0x1D, 1, SDL_SCANCODE_RCTRL,  "RCTRL"  },
        { 0x38, 0, SDL_SCANCODE_LALT,   "LALT"   },
        { 0x38, 1, SDL_SCANCODE_RALT,   "RALT"   },
    };
    SDL_Window *window;
    int start_failures = failures;
    unsigned k;

    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "0"); /* the hint path, not the worker */
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    window = SDL_CreateWindow("sdlop win32 raw keyboard once", 400, 300, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }
    SDL_ShowWindow(window);
    /* Keyboard injection follows the foreground window, and under a parallel
     * ctest run that is another test's window. Claim focus explicitly and
     * check it took, or the packets go to the wrong process. */
    {
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                 SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        int tries;
        if (hwnd) {
            focus_before_injection = GetForegroundWindow();
            if (focus_before_injection == hwnd) {
                focus_before_injection = NULL; /* ours already; nothing to restore */
            }
            for (tries = 0; tries < 50 && GetForegroundWindow() != hwnd; tries++) {
                SetForegroundWindow(hwnd);
                SDL_PumpEvents();
                Sleep(10);
            }
            if (GetForegroundWindow() != hwnd) {
                CHECK(0, "could not take the foreground window; keyboard injection "
                         "would go to another process");
                SDL_DestroyWindow(window);
                SDL_Quit();
                return 0;
            }
        }
    }
    if (text_input) {
        SDL_StartTextInput(window);
    }
    {
        int i;
        for (i = 0; i < 20; i++) {
            SDL_PumpEvents();
            Sleep(5);
        }
    }
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

    for (k = 0; k < SDL_arraysize(cases); k++) {
        int downs = 0;
        int ups = 0;

        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        inject(cases[k].scan, cases[k].extended);
        count_key(cases[k].want, &downs, &ups);

        CHECK(downs == 1, "%s (%s): %d key-down events, want exactly 1 (double delivery "
                          "or the key was lost)", cases[k].name, label, downs);
        CHECK(ups == 1, "%s (%s): %d key-up events, want exactly 1", cases[k].name, label, ups);
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    restore_focus();
    return failures == start_failures;
}

int main(void)
{
    if (!win32_test_interactive_desktop()) {
        printf("skipped: no interactive desktop for input injection\n");
        return 77;
    }
    run_case("text input off", 0);
    run_case("text input on", 1);

    if (failures) {
        fprintf(stderr, "test_win32_raw_keyboard_once: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_raw_keyboard_once: one down and one up per key, both configurations\n");
    return 0;
}
