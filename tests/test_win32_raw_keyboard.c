/*
  SDLop test: raw keyboard input on the Windows driver.

  SDL_HINT_WINDOWS_RAW_KEYBOARD turns on raw keyboard packets, which is the
  only way to tell left and right Shift/Ctrl/Alt apart by hardware scan code.
  Two things have to hold and both were originally wrong:

    * Registration must follow the hints. RIDEV_INPUTSINK additionally
      requires a real hwndTarget -- passing NULL fails with
      ERROR_INVALID_PARAMETER -- so background delivery needs a message-only
      window rather than the NULL target the focus-following case uses.
    * RIDEV_REMOVE requires hwndTarget == NULL. Passing the old target back
      fails and silently leaves the registration in place, so the feature
      could never be turned off.

  This test only observes the OS registration table; it does not inject keys.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

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

/* Is a generic-desktop keyboard registered right now, and with what flags? */
static int keyboard_registered(DWORD *flags_out)
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
            found = 1;
            if (flags_out) {
                *flags_out = (DWORD)devices[i].dwFlags;
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

    /* the default is off: the hint is unset, so nothing is registered */
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 raw keyboard test", 300, 200, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_PumpEvents();
    CHECK(!keyboard_registered(NULL), "the keyboard was registered while the hint was off");

    /* turning it on at runtime must register */
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1");
    SDL_PumpEvents();
    CHECK(keyboard_registered(&flags), "setting the hint to 1 did not register the keyboard");

    /* EXCLUDE_HOTKEYS becomes RIDEV_NOHOTKEYS */
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD_EXCLUDE_HOTKEYS, "1");
    SDL_PumpEvents();
    CHECK(keyboard_registered(&flags) && (flags & RIDEV_NOHOTKEYS),
          "RIDEV_NOHOTKEYS was not applied (flags = 0x%lx)", (unsigned long)flags);

    /* INPUTSINK needs its own window; NULL is rejected by the API */
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD_INPUTSINK, "1");
    SDL_PumpEvents();
    CHECK(keyboard_registered(&flags) && (flags & RIDEV_INPUTSINK),
          "RIDEV_INPUTSINK was not applied (flags = 0x%lx)", (unsigned long)flags);

    /* turning it back off must actually deregister */
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "0");
    SDL_PumpEvents();
    CHECK(!keyboard_registered(NULL), "the keyboard is still registered after the hint went to 0");

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_raw_keyboard: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_raw_keyboard: registration follows the hints, on and off\n");
    return 0;
}
