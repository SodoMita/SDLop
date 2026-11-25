/*
  SDLop test: raw input worker and raw keyboard hints must not fight over the
  same device registration.

  Windows keeps exactly one Raw Input registration per usage page/usage.
  SDLop has two features that want the Generic Desktop / Keyboard usage:

    - SDL_HINT_WINDOWS_RAW_KEYBOARD=1, which registers the keyboard with the
      focus-following hwndTarget = NULL
    - the asyncinput-style worker (SDLOP_RAW_INPUT=1), which registers it on
      its own sink window with RIDEV_INPUTSINK

  Measured before this was handled: registering the worker's sink silently
  replaced the hint's registration, and one later RIDEV_REMOVE wiped both --
  leaving no raw keyboard input at all while both features still reported
  success.

  This test pins the rule: the worker owns the registration while it runs,
  the hint's registration is restored when it stops, and there is never more
  than one keyboard entry.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, " ");                                \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            failures++;                                          \
        }                                                        \
    } while (0)

static int keyboard_entries(DWORD *flags_out, HWND *target_out)
{
    UINT count = 0;
    RAWINPUTDEVICE *devices;
    int found = 0;
    UINT i;

    if (GetRegisteredRawInputDevices(NULL, &count, sizeof(RAWINPUTDEVICE)) == (UINT)-1 || count == 0) {
        return 0;
    }
    devices = (RAWINPUTDEVICE *)malloc(sizeof(RAWINPUTDEVICE) * count);
    if (!devices) {
        return 0;
    }
    if (GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE)) == (UINT)-1) {
        free(devices);
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (devices[i].usUsagePage == 0x01 && devices[i].usUsage == 0x06) {
            found++;
            if (flags_out) {
                *flags_out = (DWORD)devices[i].dwFlags;
            }
            if (target_out) {
                *target_out = (HWND)devices[i].hwndTarget;
            }
        }
    }
    free(devices);
    return found;
}

int main(void)
{
    SDL_Window *window;
    DWORD flags = 0;
    HWND target = NULL;
    int entries;

    /* phase 1: hint only, worker off */
    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "0");
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 raw coexist test", 320, 240, 0);
    SDL_PumpEvents();
    entries = keyboard_entries(&flags, &target);
    CHECK(entries == 1, "hint only: %d keyboard registrations, want 1", entries);
    CHECK(target == NULL, "hint only: the registration should follow focus (NULL target)");

    /* phase 2: the worker takes over. SDL_Quit() drops all hints, so they are
     * re-set before every re-init. */
    SDL_DestroyWindow(window);
    SDL_Quit();
    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 raw coexist test", 320, 240, 0);
    SDL_PumpEvents();
    CHECK(SDLop_RawInputAvailable(), "the worker did not start");
    entries = keyboard_entries(&flags, &target);
    CHECK(entries == 1, "worker on: %d keyboard registrations, want exactly 1 (they must not both claim it)",
          entries);
    CHECK(target != NULL, "worker on: the registration should target the worker's sink window");
    CHECK((flags & RIDEV_INPUTSINK) != 0,
          "worker on: the registration should use RIDEV_INPUTSINK (flags = 0x%lx)", (unsigned long)flags);

    /* phase 3: worker off again -- the hint's registration must come back */
    SDL_DestroyWindow(window);
    SDL_Quit();
    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "0");
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 raw coexist test", 320, 240, 0);
    SDL_PumpEvents();
    entries = keyboard_entries(&flags, &target);
    CHECK(entries == 1, "worker off: %d keyboard registrations, want 1 -- the hint's registration "
                        "was lost when the worker stopped", entries);
    CHECK(target == NULL, "worker off: the registration should be the focus-following one again");

    SDL_DestroyWindow(window);
    /* leave the process clean for the tests that follow */
    SDL_Quit();
    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "0");
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "0");
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_raw_coexist: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_raw_coexist: one keyboard owner at a time, restored on handover\n");
    return 0;
}
