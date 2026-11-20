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
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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


/* Read a pixel straight off the framebuffer device (what the screen shows),
   falling back to the window surface when there is no device (dump mode).
   Returns 0xRRGGBB, or 0xFFFFFFFF when neither is available. */
static unsigned screen_pixel(SDL_Surface *surface, int x, int y)
{
    static int fb_fd = -2;
    static struct fb_var_screeninfo var;
    static struct fb_fix_screeninfo fix;
    static unsigned char *map;

    if (fb_fd == -2) {
        fb_fd = -1;
        const char *path = getenv("SDLOP_FBDEV_PATH");
        if (!path || !*path) {
            path = "/dev/fb0";
        }
        fb_fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fb_fd >= 0 && (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) != 0 ||
                           ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) != 0)) {
            close(fb_fd);
            fb_fd = -1;
        }
        if (fb_fd >= 0) {
            map = (unsigned char *)mmap(NULL, (size_t)fix.line_length * var.yres_virtual,
                                        PROT_READ, MAP_SHARED, fb_fd, 0);
            if (map == MAP_FAILED) {
                map = NULL;
                close(fb_fd);
                fb_fd = -1;
            }
        }
    }
    if (fb_fd < 0 || !map) {
        /* dump mode: the driver's own frame is what "the screen" is */
        const char *dump = getenv("SDLOP_FBDEV_DUMP");
        if (dump && *dump) {
            FILE *f = fopen(dump, "rb");
            if (!f) {
                return 0xFFFFFFFFu;
            }
            int dw = 0, dh = 0, maxval = 0;
            char magic[3] = { 0 };
            if (fscanf(f, "%2s %d %d %d", magic, &dw, &dh, &maxval) != 4 || strcmp(magic, "P6") != 0) {
                fclose(f);
                return 0xFFFFFFFFu;
            }
            fgetc(f);
            unsigned px = 0xFFFFFFFFu;
            if (x >= 0 && y >= 0 && x < dw && y < dh) {
                const long offset = (long)y * dw * 3 + (long)x * 3;
                unsigned char rgb[3];
                if (fseek(f, offset, SEEK_CUR) == 0 && fread(rgb, 1, 3, f) == 3) {
                    px = ((unsigned)rgb[0] << 16) | ((unsigned)rgb[1] << 8) | rgb[2];
                }
            }
            fclose(f);
            return px;
        }
        Uint8 r = 0, g = 0, b = 0, a = 0;
        if (!surface || !SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a)) {
            return 0xFFFFFFFFu;
        }
        return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
    }
    if (x < 0 || y < 0 || (unsigned)x >= var.xres || (unsigned)y >= var.yres) {
        return 0xFFFFFFFFu;
    }
    const unsigned char *p = map + (size_t)y * fix.line_length + (size_t)x * (var.bits_per_pixel / 8);
    unsigned raw = 0;
    if (var.bits_per_pixel == 16) {
        unsigned short v;
        memcpy(&v, p, sizeof(v));
        raw = v;
    } else if (var.bits_per_pixel == 24) {
        raw = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
    } else {
        memcpy(&raw, p, sizeof(raw));
    }
    const unsigned r = ((raw >> var.red.offset) & ((1u << var.red.length) - 1u)) * 255u / ((1u << var.red.length) - 1u);
    const unsigned g = ((raw >> var.green.offset) & ((1u << var.green.length) - 1u)) * 255u / ((1u << var.green.length) - 1u);
    const unsigned b = ((raw >> var.blue.offset) & ((1u << var.blue.length) - 1u)) * 255u / ((1u << var.blue.length) - 1u);
    return (r << 16) | (g << 8) | b;
}

/* Does a read-back pixel look like the expected colour? Channel widths differ
   (a 565 console reports 0x00c200 for 0x00c000), so compare with slack. */
static bool pixel_near(unsigned got, unsigned want, int slack)
{
    if (got == 0xFFFFFFFFu) {
        return false;
    }
    for (int shift = 0; shift <= 16; shift += 8) {
        const int a = (int)((got >> shift) & 0xFF);
        const int b = (int)((want >> shift) & 0xFF);
        if (a < b - slack || a > b + slack) {
            return false;
        }
    }
    return true;
}

/* read one pixel of the window surface (the app's own buffer) */
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
    const unsigned band = screen_pixel(s, s->w / 8, s->h / 2);
    int drawn = 0;
    for (int y = s->h / 2 - 10; y < s->h / 2 + 10 && !drawn; y++) {
        for (int x = s->w / 2 - 10; x < s->w / 2 + 10; x++) {
            if (x >= 0 && y >= 0 && x < s->w && y < s->h) {
                const unsigned px = screen_pixel(s, x, y);
                const int dr = (int)((px >> 16) & 0xFF) - (int)((band >> 16) & 0xFF);
                const int dg = (int)((px >> 8) & 0xFF) - (int)((band >> 8) & 0xFF);
                const int db = (int)(px & 0xFF) - (int)(band & 0xFF);
                /* the arrow is white on a black outline: either is far from
                   the band, a dithered 16 bpp band is not */
                if (dr * dr + dg * dg + db * db > 40 * 40) {
                    drawn = 1;
                    break;
                }
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
            if (x >= 0 && y >= 0 && x < s->w && y < s->h && screen_pixel(s, x, y) != band) {
                trail++;
            }
        }
    }
    /* The app's buffer is its own: whatever the cursor does on screen, the
       pixels it drew must be untouched (the old driver drew the arrow straight
       into the app's buffer, which is also what left the trail). */
    const unsigned app_px = surface_pixel(s, s->w / 2, s->h / 2);
    CHECK(pixel_near(app_px, band, 2),
          "app pixels untouched by the cursor (got %06x, want %06x)", app_px, band);

    /* repaint the whole surface: the old spot must come back from it */
    paint_bands(s);
    SDL_UpdateWindowSurface(window);
    const unsigned again = screen_pixel(s, s->w / 2, s->h / 2);
    CHECK(pixel_near(again, band, 2), "surface repaint restores the pixels under the cursor (got %06x)", again);

    SDL_HideCursor();
    SDL_PumpEvents();
    CHECK(SDL_CursorVisible() == false, "SDL_HideCursor takes effect");
    SDL_ShowCursor();

    /* partial updates: after painting the whole surface, presenting only a
       small rect must not disturb anything outside it */
    paint_bands(s);
    SDL_UpdateWindowSurface(window);
    SDL_Rect patch = { 4, 4, 16, 16 };
    SDL_FillSurfaceRect(s, &patch, 0x00FFFF00u);
    CHECK(SDL_UpdateWindowSurfaceRects(window, &patch, 1), "present a small rect");
    const unsigned inside = screen_pixel(s, 8, 8);
    const unsigned outside = screen_pixel(s, s->w / 2, s->h / 2);
    CHECK(pixel_near(inside, 0x00FFFF00u, 8), "the patched rect reached the screen (got %06x)", inside);
    CHECK(pixel_near(outside, 0x00C000u, 8), "the rest of the screen was left alone (got %06x)", outside);
}

/* ---- real console drill: KD_ and VT_ state, a switch away and back.
   Needs root, /dev/fb0 and a VT. It checks the VT the driver owns, named by
   SDLOP_FBDEV_VT_CHECK_TTY (default /dev/tty1), because /dev/tty0 reports the
   *active* VT - which is exactly what must not be ours unless we asked. ---- */
/* the VT the user is looking at, or -1 when there is no console */
static int active_vt(void)
{
    const int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    struct vt_stat state;
    int vt = -1;

    if (fd < 0) {
        return -1;
    }
    memset(&state, 0, sizeof(state));
    if (ioctl(fd, VT_GETSTATE, &state) == 0) {
        vt = (int)state.v_active;
    }
    close(fd);
    return vt;
}

/* Switch VTs and pump until it happened. VT_WAITACTIVE would block here: the
   switch completes only after this very process acknowledges it from its VT
   handler, i.e. after a pump - waiting on it without pumping deadlocks. */
static bool switch_to(SDL_Window *window, int vt)
{
    const int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool asked = (ioctl(fd, VT_ACTIVATE, vt) == 0);
    close(fd);
    if (!asked) {
        return false;
    }
    for (int i = 0; i < 200; i++) {
        SDL_Surface *surface = SDL_GetWindowSurface(window);
        if (surface) {
            (void)surface; /* keep the surface alive across the switch */
        }
        SDL_PumpEvents();
        SDL_Delay(10);
        if (active_vt() == vt) {
            for (int j = 0; j < 5; j++) { /* let fbcon/the repaint settle */
                SDL_PumpEvents();
                SDL_Delay(10);
            }
            return true;
        }
    }
    return false;
}

/* Find the console the driver took over: the one with VT_PROCESS installed and
   KD_GRAPHICS set. That pair is the driver's signature and needs no private
   API to read. */
static int find_driver_vt(int *out_fd)
{
    for (int n = 1; n <= 63; n++) {
        char path[32];
        struct vt_mode vm;
        int mode = -1;

        SDL_snprintf(path, sizeof(path), "/dev/tty%d", n);
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        memset(&vm, 0, sizeof(vm));
        if (ioctl(fd, VT_GETMODE, &vm) == 0 && vm.mode == VT_PROCESS &&
            ioctl(fd, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS) {
            *out_fd = fd;
            return n;
        }
        close(fd);
    }
    *out_fd = -1;
    return 0;
}

/* ---- real console drill: the driver owns a VT (KD_GRAPHICS + K_OFF +
   VT_PROCESS), starting SDL did not take the user's VT away, and switching
   away hands the console back in text mode. Needs root and a VT; the QEMU
   harness sets SDLOP_FBDEV_VT_CHECK=1. ---- */
/* solid fill of the whole window surface */
static void paint_plane(SDL_Surface *s, unsigned color)
{
    SDL_FillSurfaceRect(s, NULL, color);
}

static int vt_phase(SDL_Window *window, SDL_Surface *s, int vt_before, int *failures)
{
    int fd = -1;
    const int our_vt = find_driver_vt(&fd);
    if (our_vt <= 0 || fd < 0) {
        printf("  note: no console taken over (no VT with KD_GRAPHICS); skipping\n");
        return 0;
    }
    printf("  info: driver owns VT %d, the user was on VT %d before init\n", our_vt, vt_before);

    int mode = -1, kbmode = -1;
    struct vt_mode vm;
    memset(&vm, 0, sizeof(vm));
    CHECK(ioctl(fd, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS,
          "the driver's VT is in KD_GRAPHICS (got %d)", mode);
    {
        /* K_OFF is only set when a raw keyboard exists to deliver the keys and
           forward Ctrl+Alt+Fn; with raw input off the console keeps its own
           handler, which is what makes the VT switch chord keep working. Both
           are correct as long as the keyboard is usable one way or the other. */
        char kbbits[8] = { 0 };
        int kbfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (kbfd < 0) {
            kbfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
        const int raw_kb = kbfd >= 0 &&
                           ioctl(kbfd, EVIOCGBIT(EV_KEY, sizeof(kbbits)), kbbits) >= 0 &&
                           (kbbits[KEY_A / 8] & (1 << (KEY_A % 8))) != 0;
        if (kbfd >= 0) {
            close(kbfd);
        }
        const char *raw_knob = getenv("SDLOP_DISABLE_RAW_INPUT");
        const bool raw_off = raw_knob && *raw_knob && strcmp(raw_knob, "0") != 0;
        if (raw_kb && !raw_off) {
            CHECK(ioctl(fd, KDGKBMODE, &kbmode) == 0 && kbmode == K_OFF,
                  "with a raw keyboard the console handler is off (got %d)", kbmode);
        } else {
            CHECK(ioctl(fd, KDGKBMODE, &kbmode) == 0 && kbmode != K_OFF,
                  "without raw input the console keyboard is left alone (got %d)", kbmode);
        }
    }
    CHECK(ioctl(fd, VT_GETMODE, &vm) == 0 && vm.mode == VT_PROCESS,
          "VT_PROCESS is installed (got %d)", (int)vm.mode);

    /* no hijack: the VT the user was looking at is still the active one,
       unless SDLOP_FBDEV_VT_SWITCH asked for the switch */
    const char *opt = getenv("SDLOP_FBDEV_VT_SWITCH");
    const bool opted_in = opt && *opt && strcmp(opt, "0") != 0;
    const int now_active = active_vt();
    if (vt_before > 0 && now_active > 0) {
        if (opted_in) {
            CHECK(now_active == our_vt, "opted-in switch: active VT is the driver's (got %d)", now_active);
        } else {
            CHECK(now_active == vt_before,
                  "no hijack: the user was on VT %d before init and is on VT %d now",
                  vt_before, now_active);
        }
    }

    if (getenv("SDLOP_QEMU_CTTY") && strcmp(getenv("SDLOP_QEMU_CTTY"), "1") == 0) {
        CHECK(our_vt == 3 && now_active == vt_before && vt_before == 2,
              "controlling VT 3 selected without activating it (ours=%d active=%d)", our_vt, now_active);
    }

    const int other_vt = (our_vt == 2) ? 3 : 2;
    char other_path[32];
    SDL_snprintf(other_path, sizeof(other_path), "/dev/tty%d", other_vt);
    const int other = open(other_path, O_RDWR | O_CLOEXEC);
    if (other < 0) {
        printf("  note: cannot open %s; skipping the switch half\n", other_path);
        close(fd);
        return our_vt;
    }

    /* look at the picture: the user's own switch, not ours */
    CHECK(switch_to(window, our_vt), "switched to the driver's VT %d", our_vt);
    paint_bands(s);
    SDL_UpdateWindowSurface(window);
    const unsigned on_screen = screen_pixel(s, s->w / 2, s->h / 2);
    CHECK(pixel_near(on_screen, 0x00C000u, 6),
          "frame is on screen after switching to the driver's VT (got %06x)", on_screen);

    /* away: the driver must hand the console back in text mode */
    CHECK(switch_to(window, other_vt), "switched away to VT %d", other_vt);
    mode = kbmode = -1;
    CHECK(ioctl(fd, KDGETMODE, &mode) == 0 && mode == KD_TEXT,
          "console returned to KD_TEXT while away (got %d)", mode);
    CHECK(ioctl(fd, KDGKBMODE, &kbmode) == 0 && kbmode != K_OFF,
          "console keyboard handler restored while away (got %d)", kbmode);

    /* while the other VT is in front the driver must not touch the scanout:
       present a colour nobody has seen yet and check the screen still shows
       the last frame we put there, not the new one */
    paint_plane(s, 0x00FFFFFFu);
    SDL_UpdateWindowSurface(window);
    const unsigned while_away = screen_pixel(s, s->w / 2, s->h / 2);
    /* the console owns the screen here and may blank it, so do not demand our
       last frame - only that the present did not reach the scanout */
    CHECK(!pixel_near(while_away, 0x00FFFFFFu, 8),
          "nothing was written to the scanout while another VT was in front (got %06x)",
          while_away);
    const unsigned kept = surface_pixel(s, s->w / 2, s->h / 2);
    CHECK(pixel_near(kept, 0x00FFFFFFu, 6),
          "the app's own pixels survived that present (got %06x)", kept);

    /* Ctrl+Alt+Fn: the console keyboard is in K_OFF, so only sdlop can see the
       kernel's switch chord. The host types it into the virtual keyboard once
       this line shows up; here we only watch the VT. */
    if (getenv("SDLOP_QEMU_CHORD") && strcmp(getenv("SDLOP_QEMU_CHORD"), "1") == 0) {
        CHECK(switch_to(window, our_vt), "back on our VT before the chord");
        int chord_kbmode = -1;
        const int chord_kbrc = ioctl(fd, KDGKBMODE, &chord_kbmode);
        CHECK(chord_kbrc == 0 && chord_kbmode == K_OFF, "chord starts with K_OFF (got %d)", chord_kbmode);
        printf("VT-CHORD-READY\n");
        fflush(stdout);
        const int from_vt = active_vt();
        bool moved = false;
        for (int i = 0; i < 250; i++) { /* up to ~5 s */
            SDL_PumpEvents();
            if (active_vt() == other_vt) {
                moved = true;
                break;
            }
            SDL_Delay(20);
        }
        CHECK(moved, "Ctrl+Alt+F%d switched the console, from VT %d now on VT %d",
              other_vt, from_vt, active_vt());
    }

    /* back: graphics mode again, and the frame is repainted */
    CHECK(switch_to(window, our_vt), "switched back to the driver's VT %d", our_vt);
    mode = -1;
    CHECK(ioctl(fd, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS,
          "console back in KD_GRAPHICS after the switch (got %d)", mode);
    paint_bands(s);
    SDL_UpdateWindowSurface(window);
    const unsigned mid = screen_pixel(s, s->w / 2, s->h / 2);
    CHECK(pixel_near(mid, 0x00C000u, 6), "frame intact after switching back (got %06x)", mid);

    close(other);
    close(fd);
    return our_vt;
}

int main(void)
{
    int failures = 0;
    bool dump_mode = dump_mode_requested();

    if (!dump_mode && !fb_present()) {
        printf("test_fbdev: SKIP (no /dev/fb0 and SDLOP_FBDEV_DUMP not set)\n");
        return 0;
    }

    /* what the user was looking at before we touched anything */
    const int vt_before = active_vt();

    setenv("SDL_VIDEODRIVER", "fbdev", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 0); /* default: window-system input only */

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
        CHECK(s->pixels != NULL, "surface has RAM pixels");

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

    /* A second surface must not clear or alias the first window's shadow. */
    if (s) {
        SDL_Window *second = SDL_CreateWindow("second shadow", bounds.w, bounds.h, SDL_WINDOW_HIDDEN);
        CHECK(second != NULL, "second window");
        if (second) {
            SDL_Surface *ss = SDL_GetWindowSurface(second);
            CHECK(ss && ss->pixels != s->pixels, "windows have distinct shadow buffers");
            if (ss) {
                paint_plane(ss, 0x00FFFFFFu);
                CHECK(pixel_near(surface_pixel(s, s->w / 2, s->h / 2), 0x00C000u, 6),
                      "creating and painting another surface preserves the first window");
            }
            SDL_DestroyWindow(second);
            CHECK(pixel_near(surface_pixel(s, s->w / 2, s->h / 2), 0x00C000u, 6),
                  "destroying another surface preserves the first window");
        }
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

    int driver_vt = 0;

    /* these phases repaint the scanout: run them before the dump check and
       leave a banded frame behind for the hold/host observer */
    if (s) {
        /* the VT drill first: it brings the driver's VT on screen, and the
           cursor is only composited while the console is visible */
        const char *vt_check = getenv("SDLOP_FBDEV_VT_CHECK");
        if (vt_check && *vt_check && strcmp(vt_check, "0") != 0) {
            printf("-- console (VT) drill --\n");
            paint_bands(s);
            SDL_UpdateWindowSurface(w);
            driver_vt = vt_phase(w, s, vt_before, &failures);
        }
        printf("-- software cursor --\n");
        cursor_phase(w, s, &failures);
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

    /* after quit the console must be usable again: text mode, keyboard
       handler back, VT_AUTO (no leftover graphics mode or cursor) */
    if (driver_vt > 0) {
        char path[32];
        SDL_snprintf(path, sizeof(path), "/dev/tty%d", driver_vt);
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            int mode = -1, kbmode = -1;
            struct vt_mode vm;
            memset(&vm, 0, sizeof(vm));
            CHECK(ioctl(fd, KDGETMODE, &mode) == 0 && mode == KD_TEXT,
                  "console is back in KD_TEXT after SDL_Quit (got %d)", mode);
            CHECK(ioctl(fd, KDGKBMODE, &kbmode) == 0 && kbmode != K_OFF,
                  "keyboard handler is back after SDL_Quit (got %d)", kbmode);
            CHECK(ioctl(fd, VT_GETMODE, &vm) == 0 && vm.mode == VT_AUTO,
                  "VT_AUTO is restored after SDL_Quit (got %d)", (int)vm.mode);
            close(fd);
        }
    }

    printf("test_fbdev: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
