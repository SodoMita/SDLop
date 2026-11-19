/*
  SDLop test: fbdev (Linux framebuffer) driver - window surface, present
  path and image-file dump verification.

  Skips cleanly (exit 0) when no framebuffer device exists and
  SDLOP_FBDEV_DUMP is not set (e.g. a developer machine with KMS only).
  When SDLOP_FBDEV_DUMP=<file.ppm> is set (the ctest default), the driver
  runs headless against a RAM framebuffer and the test parses the dumped
  PPM to verify the visible frame end to end.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);         \
            fprintf(stderr, "\n");                \
            failures++;                           \
        } else {                                  \
            printf("  ok: " __VA_ARGS__);         \
            printf("\n");                         \
        }                                         \
    } while (0)

static bool fb_present(void)
{
    return access("/dev/fb0", R_OK | W_OK) == 0;
}

static bool dump_mode_requested(void)
{
    const char *dump = getenv("SDLOP_FBDEV_DUMP");
    return dump && *dump;
}

/* paint a deterministic 3-band pattern the PPM dump checker can verify */
static void paint_bands(SDL_Surface *s)
{
    int third = s->h / 3;
    SDL_Rect top = { 0, 0, s->w, third };
    SDL_Rect mid = { 0, third, s->w, third };
    SDL_Rect bot = { 0, 2 * third, s->w, s->h - 2 * third };
    SDL_FillSurfaceRect(s, &top, 0x00C00000u); /* dark red */
    SDL_FillSurfaceRect(s, &mid, 0x0000C000u); /* dark green */
    SDL_FillSurfaceRect(s, &bot, 0x000000C0u); /* dark blue */
}

/* parse the P6 dump and sample the three band colors; returns failure count */
static int check_ppm_bands(const char *path, int expect_w, int expect_h, int *failures)
{
    int local_failures = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL: cannot open dumped frame %s: %s\n", path, strerror(errno));
        (*failures)++;
        return 1;
    }
    char magic[3] = { 0 };
    int w = 0, h = 0, maxval = 0;
    if (fscanf(f, "%2s %d %d %d", magic, &w, &h, &maxval) != 4) {
        fprintf(stderr, "FAIL: malformed PPM header in %s\n", path);
        fclose(f);
        (*failures)++;
        return 1;
    }
    if (strcmp(magic, "P6") != 0 || maxval != 255) {
        fprintf(stderr, "FAIL: expected P6/maxval 255, got %s/%d\n", magic, maxval);
        local_failures++;
    }
    if (w != expect_w || h != expect_h) {
        fprintf(stderr, "FAIL: dumped frame is %dx%d, expected %dx%d\n", w, h, expect_w, expect_h);
        local_failures++;
    }
    /* one whitespace byte after the maxval, then raw RGB triplets */
    fgetc(f);

    if (local_failures == 0) {
        size_t row_bytes = (size_t)w * 3;
        uint8_t *row = (uint8_t *)malloc(row_bytes);
        uint8_t sample[3][3] = { { 0 } };
        if (row) {
            int rows[3] = { h / 6, h / 2, (5 * h) / 6 };
            for (int y = 0; y < h; y++) {
                if (fread(row, 1, row_bytes, f) != row_bytes) {
                    fprintf(stderr, "FAIL: truncated PPM payload at row %d\n", y);
                    local_failures++;
                    break;
                }
                for (int i = 0; i < 3; i++) {
                    if (y == rows[i]) {
                        size_t off = (size_t)(w / 2) * 3;
                        memcpy(sample[i], row + off, 3);
                    }
                }
            }
            free(row);
        } else {
            local_failures++;
        }
        if (local_failures == 0) {
            const uint8_t expect[3][3] = { { 0xC0, 0x00, 0x00 }, { 0x00, 0xC0, 0x00 }, { 0x00, 0x00, 0xC0 } };
            const char *names[3] = { "top (red)", "middle (green)", "bottom (blue)" };
            for (int i = 0; i < 3; i++) {
                int dr = (int)sample[i][0] - (int)expect[i][0];
                int dg = (int)sample[i][1] - (int)expect[i][1];
                int db = (int)sample[i][2] - (int)expect[i][2];
                if (dr < -8 || dr > 8 || dg < -8 || dg > 8 || db < -8 || db > 8) {
                    fprintf(stderr, "FAIL: dumped %s band is rgb(%u,%u,%u), expected ~(%u,%u,%u)\n",
                            names[i], sample[i][0], sample[i][1], sample[i][2],
                            expect[i][0], expect[i][1], expect[i][2]);
                    local_failures++;
                } else {
                    printf("  ok: dumped %s band is rgb(%u,%u,%u)\n",
                           names[i], sample[i][0], sample[i][1], sample[i][2]);
                }
            }
        }
    }
    fclose(f);
    *failures += local_failures;
    return local_failures;
}

int main(void)
{
    int failures = 0;
    bool dump_mode = dump_mode_requested();

    if (!dump_mode && !fb_present()) {
        printf("test_fbdev: SKIP (no /dev/fb0 and SDLOP_FBDEV_DUMP not set)\n");
        return 0;
    }

    setenv("SDL_VIDEODRIVER", "fbdev", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1); /* window-system input only */

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        if (dump_mode) {
            fprintf(stderr, "FAIL: SDL_Init failed in dump mode: %s\n", SDL_GetError());
            return 1;
        }
        printf("test_fbdev: SKIP (fbdev unavailable: %s)\n", SDL_GetError());
        return 0;
    }
    const char *driver = SDL_GetCurrentVideoDriver();
    CHECK(driver && strcmp(driver, "fbdev") == 0, "video driver is fbdev (got %s)",
          driver ? driver : "?");

    printf("-- displays --\n");
    int num_displays = 0;
    SDL_DisplayID *display_ids = SDL_GetDisplays(&num_displays);
    CHECK(display_ids != NULL, "SDL_GetDisplays returns a list");
    CHECK(num_displays >= 1, "at least one display (got %d)", num_displays);
    SDL_Rect bounds = { 0, 0, 0, 0 };
    if (num_displays >= 1) {
        CHECK(SDL_GetDisplayBounds(display_ids[0], &bounds), "display bounds");
        CHECK(bounds.w > 0 && bounds.h > 0, "bounds positive (%dx%d)", bounds.w, bounds.h);
        printf("  display %u: %dx%d\n", display_ids[0], bounds.w, bounds.h);
        const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display_ids[0]);
        CHECK(desktop != NULL, "desktop mode");
        if (desktop) {
            CHECK(desktop->w == bounds.w && desktop->h == bounds.h,
                  "desktop mode matches bounds (%dx%d)", desktop->w, desktop->h);
        }
        int nmodes = 0;
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display_ids[0], &nmodes);
        CHECK(modes && nmodes >= 1, "at least one fullscreen mode (got %d)", nmodes);
        SDL_DisplayMode closest;
        memset(&closest, 0, sizeof(closest));
        CHECK(SDL_GetClosestFullscreenDisplayMode(display_ids[0], bounds.w / 2, bounds.h / 2,
                                                  0.0f, false, &closest),
              "closest fullscreen mode");
        free(modes);
    }

    printf("-- software scanout path --\n");
    /* fbdev is console-sized: a 320x200 request snaps to the fb mode */
    SDL_Window *w = SDL_CreateWindow("SDLop fbdev test", 320, 200, 0);
    if (!w) {
        fprintf(stderr, "FAIL: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        free(display_ids);
        return 1;
    }
    int ww = 0, hh = 0;
    CHECK(SDL_GetWindowSize(w, &ww, &hh), "get size");
    CHECK(ww == bounds.w && hh == bounds.h,
          "window snapped to fb mode (got %dx%d, want %dx%d)", ww, hh, bounds.w, bounds.h);

    SDL_Surface *s = SDL_GetWindowSurface(w);
    CHECK(s != NULL, "window surface");
    if (s) {
        CHECK(s->format == SDL_PIXELFORMAT_XRGB8888,
              "surface format XRGB8888 (got 0x%08x)", (unsigned)s->format);
        CHECK(s->w == bounds.w && s->h == bounds.h,
              "surface matches the fb mode (%dx%d)", s->w, s->h);
        CHECK(s->pitch >= s->w * 4, "pitch valid (%d)", s->pitch);
        CHECK(s->pixels != NULL, "surface maps the scanout");

        paint_bands(s);
        Uint8 r = 0, g = 0, b = 0, a = 0;
        if (SDL_ReadSurfacePixel(s, s->w / 2, s->h / 6, &r, &g, &b, &a)) {
            CHECK(r == 0xC0 && g == 0 && b == 0, "top band is red (got %02x %02x %02x)", r, g, b);
        }
        if (SDL_ReadSurfacePixel(s, s->w / 2, s->h / 2, &r, &g, &b, &a)) {
            CHECK(r == 0 && g == 0xC0 && b == 0, "middle band is green (got %02x %02x %02x)", r, g, b);
        }
        CHECK(SDL_UpdateWindowSurface(w), "update window surface (present)");
        /* exercise the flip path a few times */
        SDL_FillSurfaceRect(s, NULL, 0x00C0C000u);
        CHECK(SDL_UpdateWindowSurface(w), "second present");
        paint_bands(s);
        CHECK(SDL_UpdateWindowSurface(w), "third present (bands again)");
    }

    printf("-- resize request snaps back to fb mode --\n");
    if (SDL_SetWindowSize(w, 640, 480)) {
        SDL_GetWindowSize(w, &ww, &hh);
        CHECK(ww == bounds.w && hh == bounds.h,
              "SetWindowSize snaps to the fb mode (got %dx%d)", ww, hh);
    } else {
        printf("  note: SDL_SetWindowSize failed: %s\n", SDL_GetError());
    }

    /* fullscreen mode selection API still works against the fb's mode */
    {
        printf("-- fullscreen display mode request --\n");
        int nmodes = 0;
        SDL_DisplayID d0 = display_ids ? display_ids[0] : SDL_GetPrimaryDisplay();
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(d0, &nmodes);
        CHECK(modes && nmodes >= 1, "display has fullscreen modes");
        if (modes && nmodes >= 1) {
            SDL_DisplayMode want = *modes[0];
            CHECK(SDL_SetWindowFullscreenMode(w, &want), "set window fullscreen mode %dx%d", want.w, want.h);
            const SDL_DisplayMode *stored = SDL_GetWindowFullscreenMode(w);
            CHECK(stored != NULL, "window reports the exclusive fullscreen mode");
            if (stored) {
                CHECK(stored->w == want.w && stored->h == want.h,
                      "exclusive mode matches request (%dx%d)", stored->w, stored->h);
            }
            CHECK(SDL_SetWindowFullscreenMode(w, NULL), "clear fullscreen mode");
            CHECK(SDL_GetWindowFullscreenMode(w) == NULL, "exclusive mode cleared");
        }
        free(modes);
    }

    /* GL must fail cleanly: fbdev has no present API for it */
    printf("-- OpenGL rejected --\n");
    {
        SDL_Window *glw = SDL_CreateWindow("SDLop fbdev gl probe", bounds.w, bounds.h, SDL_WINDOW_OPENGL);
        CHECK(glw == NULL, "GL window creation fails on fbdev (got %p)", (void *)glw);
        if (glw) {
            SDL_DestroyWindow(glw);
        }
    }

    if (dump_mode) {
        printf("-- dumped frame verification --\n");
        const char *dump = getenv("SDLOP_FBDEV_DUMP");
        int w2 = 0, h2 = 0;
        SDL_GetWindowSize(w, &w2, &h2);
        check_ppm_bands(dump, w2, h2, &failures);
    }

    /* optional: keep the banded frame on screen so an external observer
     * (e.g. a QEMU screendump) can verify the actual scanout; prints a
     * flushed marker the host can wait for */
    {
        const char *hold = getenv("SDLOP_TEST_FBDEV_HOLD");
        if (hold && *hold) {
            long secs = atol(hold);
            if (secs <= 0) {
                secs = 1;
            }
            SDL_Surface *s3 = SDL_GetWindowSurface(w);
            if (s3) {
                paint_bands(s3);
                SDL_UpdateWindowSurface(w);
            }
            printf("BANDS-READY\n");
            fflush(stdout);
            for (long t = 0; t < secs * 20; t++) {
                SDL_PumpEvents();
                SDL_Delay(50);
            }
        }
    }

    SDL_DestroyWindow(w);
    free(display_ids);

    SDL_Quit();
    printf("test_fbdev: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
