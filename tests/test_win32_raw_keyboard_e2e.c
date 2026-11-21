/*
  SDLop test: raw keyboard input end to end, through the worker.

  The point of SDL_HINT_WINDOWS_RAW_KEYBOARD is telling left and right
  modifiers apart. WM_KEYDOWN cannot: Windows sends the same virtual key for
  both, and the distinction lives only in the hardware scan code (and the E0
  prefix for keys that share a scan code).

  This drives real Raw Input packets into the worker by injecting scan codes
  with SendInput(KEYEVENTF_SCANCODE), so the whole path is exercised: Win32
  produces a RAWKEYBOARD, the worker maps it through the scan code table and
  the E0 prefix, and publishes evdev codes on the rings.

  Three of the six cases share a scan code and are separated only by E0, which
  is the reason the fix cannot be a table lookup alone.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* evdev key codes, as published in SDLop.h */
#define KEY_LEFTSHIFT  42
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTCTRL   29
#define KEY_RIGHTCTRL  97
#define KEY_LEFTALT    56
#define KEY_RIGHTALT   100

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

/* Press and release one key by scan code. KEYEVENTF_EXTENDEDKEY is what
 * carries the E0 prefix through SendInput. */
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
    Sleep(25);
}

static int drain(SDLop_RawEvent *out, int max)
{
    SDLop_RawEvent buffer[64];
    int total = 0;
    int chunk;

    SDL_PumpEvents();
    while ((chunk = SDLop_PollRawEvents(buffer, 64)) > 0) {
        int i;
        for (i = 0; i < chunk && total < max; i++) {
            out[total++] = buffer[i];
        }
        if (chunk < 64) {
            break;
        }
    }
    return total;
}

int main(void)
{
    static const struct {
        WORD scan;
        int extended;
        const char *name;
        Uint16 want;
    } cases[] = {
        /* distinct scan codes */
        { 0x2A, 0, "LSHIFT", KEY_LEFTSHIFT },
        { 0x36, 0, "RSHIFT", KEY_RIGHTSHIFT },
        /* same scan code, separated only by the E0 prefix */
        { 0x1D, 0, "LCTRL", KEY_LEFTCTRL },
        { 0x1D, 1, "RCTRL", KEY_RIGHTCTRL },
        { 0x38, 0, "LALT", KEY_LEFTALT },
        { 0x38, 1, "RALT", KEY_RIGHTALT },
    };
    SDL_Window *window;
    unsigned k;

    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 raw keyboard e2e", 400, 300, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
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
    if (!SDLop_RawInputAvailable()) {
        CHECK(0, "the raw input worker is not running, so keyboard input cannot be "
                 "read at all (SDLOP_RAW_INPUT=1)");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    for (k = 0; k < SDL_arraysize(cases); k++) {
        SDLop_RawEvent events[64];
        Uint16 pressed = 0;
        Uint16 released = 0;
        int count;
        int i;

        drain(events, 64); /* start clean */
        inject(cases[k].scan, cases[k].extended);
        count = drain(events, 64);

        for (i = 0; i < count; i++) {
            if (events[i].type != SDLop_EV_KEY || events[i].code != cases[k].want) {
                continue;
            }
            if (events[i].value) {
                pressed = events[i].code;
            } else {
                released = events[i].code;
            }
        }
        CHECK(pressed == cases[k].want,
              "%s (scan 0x%02x, extended %d): press published evdev %u, want %u -- "
              "left and right are not being distinguished",
              cases[k].name, cases[k].scan, cases[k].extended, pressed, cases[k].want);
        CHECK(released == cases[k].want,
              "%s: release published evdev %u, want %u",
              cases[k].name, released, cases[k].want);
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    restore_focus();

    if (failures) {
        fprintf(stderr, "test_win32_raw_keyboard_e2e: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_raw_keyboard_e2e: left/right modifiers distinguished end to end\n");
    return 0;
}
