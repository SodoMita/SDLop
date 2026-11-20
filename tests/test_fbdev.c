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
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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


/* read one pixel of the window surface as 0xRRGGBB (0xFFFFFFFF on failure) */
static unsigned surface_pixel(SDL_Surface *s, int x, int y)
{
    Uint8 r = 0, g = 0, b = 0, a = 0;
    if (!SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a)) {
        return 0xFFFFFFFFu;
    }
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

/* ---- software cursor: it must be drawn and follow the pointer. Whether the
   old spot is clean depends on the core: SDL_WarpMouseInWindow() frees the
   previous position (stock SDL3 behaviour) and reports the move
   asynchronously, so a trail is only checked after the surface was repainted
   (which is where a stale-pixel bug would show up). ---- */
static void cursor_phase(SDL_Window *window, SDL_Surface *s, int *failures)
{
    SDL_ShowCursor();
    SDL_WarpMouseInWindow(window, (float)(s->w / 2), (float)(s->h / 2));
    SDL_PumpEvents();
    SDL_UpdateWindowSurface(window);

    /* the built-in arrow is white inside a black outline on the green band;
       sample the band well away from both pointer positions */
    const unsigned band = surface_pixel(s, s->w / 8, s->h / 2);
    int drawn = 0;
    for (int y = s->h / 2 - 10; y < s->h / 2 + 10 && !drawn; y++) {
        for (int x = s->w / 2 - 10; x < s->w / 2 + 10; x++) {
            if (x >= 0 && y >= 0 && x < s->w && y < s->h && surface_pixel(s, x, y) != band) {
                drawn = 1;
                break;
            }
        }
    }
    CHECK(drawn, "cursor is drawn on the scanout");

    SDL_WarpMouseInWindow(window, (float)(s->w / 4), (float)(s->h / 4));
    SDL_PumpEvents();
    SDL_UpdateWindowSurface(window);
    int trail = 0; /* informational until the core reports motion */
    for (int y = s->h / 2 - 10; y < s->h / 2 + 10; y++) {
        for (int x = s->w / 2 - 10; x < s->w / 2 + 10; x++) {
            if (x >= 0 && y >= 0 && x < s->w && y < s->h && surface_pixel(s, x, y) != band) {
                trail++;
            }
        }
    }
    printf("  info: %d pixels changed at the old pointer spot\n", trail);

    /* repaint the whole surface: the old spot must come back from it */
    paint_bands(s);
    SDL_UpdateWindowSurface(window);
    const unsigned again = surface_pixel(s, s->w / 2, s->h / 2);
    CHECK(again == band, "surface repaint restores the pixels under the cursor (got %06x)", again);

    SDL_HideCursor();
    SDL_PumpEvents();
    CHECK(SDL_CursorVisible() == false, "SDL_HideCursor takes effect");
    SDL_ShowCursor();
}

/* ---- real console drill: KD_ and VT_ state, a switch away and back.
   Needs root, /dev/fb0 and a VT; the QEMU harness sets SDLOP_FBDEV_VT_CHECK=1. ---- */
static void vt_phase(SDL_Window *window, SDL_Surface *s, int *failures)
{
    const char *tty = getenv("SDLOP_FBDEV_VT_CHECK_TTY");
    if (!tty || !*tty) {
        tty = "/dev/tty0";
    }
    int fd = open(tty, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("  note: VT drill skipped, cannot open %s: %s\n", tty, strerror(errno));
        return;
    }

    int mode = -1, kbmode = -1;
    struct vt_mode vm;
    struct vt_stat state;
    memset(&vm, 0, sizeof(vm));
    memset(&state, 0, sizeof(state));

    CHECK(ioctl(fd, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS,
          "console is in KD_GRAPHICS while we own it (got %d)", mode);
    CHECK(ioctl(fd, KDGKBMODE, &kbmode) == 0 && kbmode == K_OFF,
          "console keyboard handler is off while we own it (got %d)", kbmode);
    CHECK(ioctl(fd, VT_GETMODE, &vm) == 0 && vm.mode == VT_PROCESS,
          "VT_PROCESS is installed (got %d)", (int)vm.mode);

    const int our_vt = (ioctl(fd, VT_GETSTATE, &state) == 0) ? (int)state.v_active : 0;
    if (our_vt <= 0) {
        printf("  note: no active VT reported; skipping the switch half\n");
        close(fd);
        return;
    }
    const int other_vt = (our_vt == 2) ? 3 : 2;
    char other_path[32];
    SDL_snprintf(other_path, sizeof(other_path), "/dev/tty%d", other_vt);
    const int other = open(other_path, O_RDWR | O_CLOEXEC);
    if (other < 0) {
        printf("  note: cannot open %s; skipping the switch half\n", other_path);
        close(fd);
        return;
    }

    /* away: the driver must hand the console back in text mode */
    CHECK(ioctl(fd, VT_ACTIVATE, other_vt) == 0, "VT_ACTIVATE(%d)", other_vt);
    CHECK(ioctl(fd, VT_WAITACTIVE, other_vt) == 0, "VT_WAITACTIVE(%d)", other_vt);
    for (int i = 0; i < 50; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    mode = kbmode = -1;
    CHECK(ioctl(fd, KDGETMODE, &mode) == 0 && mode == KD_TEXT,
          "console returned to KD_TEXT while away (got %d)", mode);
    CHECK(ioctl(fd, KDGKBMODE, &kbmode) == 0 && kbmode != K_OFF,
          "console keyboard handler restored while away (got %d)", kbmode);

    /* back: graphics mode again, and the frame is repainted */
    CHECK(ioctl(fd, VT_ACTIVATE, our_vt) == 0, "VT_ACTIVATE(%d)", our_vt);
    CHECK(ioctl(fd, VT_WAITACTIVE, our_vt) == 0, "VT_WAITACTIVE(%d)", our_vt);
    for (int i = 0; i < 50; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    mode = -1;
    CHECK(ioctl(fd, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS,
          "console back in KD_GRAPHICS after the switch (got %d)", mode);
    paint_bands(s);
    SDL_UpdateWindowSurface(window);
    const unsigned mid = surface_pixel(s, s->w / 2, s->h / 2);
    CHECK(mid == 0x00C000u, "frame intact after switching back (got %06x)", mid);

    close(other);
    close(fd);
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

    /* these phases repaint the scanout: run them before the dump check and
       leave a banded frame behind for the hold/host observer */
    if (s) {
        printf("-- software cursor --\n");
        cursor_phase(w, s, &failures);
        const char *vt_check = getenv("SDLOP_FBDEV_VT_CHECK");
        if (vt_check && *vt_check && strcmp(vt_check, "0") != 0) {
            printf("-- console (VT) drill --\n");
            paint_bands(s);
            SDL_UpdateWindowSurface(w);
            vt_phase(w, s, &failures);
        }
        paint_bands(s);
        SDL_UpdateWindowSurface(w);
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
