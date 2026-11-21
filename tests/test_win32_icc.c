/*
  SDLop test: SDL_GetWindowICCProfile() on the Windows driver.

  Colour-managed applications ask for the display's ICC profile to convert
  their output to the panel's actual gamut. The Windows driver had no
  GetWindowICCProfile slot, so the call returned SDL_Unsupported() and the
  profile was simply unavailable.

  Windows stores the profile as a file path on the display's device context,
  so the driver asks GDI for the path and reads the file. This test checks
  that either a structurally valid ICC profile comes back -- its header
  length field must equal the returned size and the "acsp" signature must sit
  at offset 36 -- or that the call fails cleanly because the monitor has no
  profile installed (common, so not a failure), with *size still zeroed.

  Needs a window, so ctest runs it under Wine with a display.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
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
    size_t size = 12345; /* deliberately not zero: the driver must overwrite it */
    void *profile;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 icc test", 300, 200, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    profile = SDL_GetWindowICCProfile(window, &size);

    if (profile) {
        const unsigned char *bytes = (const unsigned char *)profile;
        const unsigned long declared = ((unsigned long)bytes[0] << 24) |
                                       ((unsigned long)bytes[1] << 16) |
                                       ((unsigned long)bytes[2] << 8) |
                                       (unsigned long)bytes[3];
        CHECK(size > 128, "profile is only %lu bytes", (unsigned long)size);
        CHECK(declared == (unsigned long)size,
              "header length field says %lu but %lu bytes were returned",
              declared, (unsigned long)size);
        CHECK(bytes[36] == 'a' && bytes[37] == 'c' && bytes[38] == 's' && bytes[39] == 'p',
              "no \"acsp\" signature at offset 36, so this is not an ICC profile");
        SDL_free(profile);
    } else {
        /* no profile installed: must be a clean failure, not a crash or a
         * stale size left behind */
        CHECK(SDL_GetError()[0] != '\0', "failed without setting an error");
        CHECK(size == 0, "failed but left *size at %lu", (unsigned long)size);
        printf("no ICC profile on this display (%s)\n", SDL_GetError());
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_icc: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_icc: ICC profile is well formed or cleanly absent\n");
    return 0;
}
