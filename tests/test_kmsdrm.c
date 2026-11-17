/*
  SDLop test: KMS/DRM integration - window surfaces, mode switching and GL
  against a real KMS device (QEMU stdvga/bochs-display/virtio-gpu).

  Skips cleanly (exit 0) when no usable /dev/dri card exists.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

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

static bool kms_present(void)
{
    DIR *folder = opendir("/dev/dri");
    if (!folder) {
        return false;
    }
    bool found = false;
    struct dirent *res;
    while ((res = readdir(folder)) != NULL && !found) {
        if (strncmp(res->d_name, "card", 4) == 0) {
            found = true;
        }
    }
    closedir(folder);
    return found;
}

/* paint a deterministic 3-band pattern + marker rows the QEMU screendump
 * checker can identify */
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

int main(void)
{
    int failures = 0;

    if (!kms_present()) {
        printf("test_kmsdrm: SKIP (no /dev/dri/cardN)\n");
        return 0;
    }

    setenv("SDL_VIDEODRIVER", "kmsdrm", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1); /* window-system input only */

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("test_kmsdrm: SKIP (kmsdrm unavailable: %s)\n", SDL_GetError());
        return 0;
    }
    CHECK(strcmp(SDL_GetCurrentVideoDriver(), "kmsdrm") == 0,
          "video driver is kmsdrm (got %s)", SDL_GetCurrentVideoDriver());

    printf("-- displays --\n");
    int num_displays = 0;
    SDL_DisplayID *display_ids = SDL_GetDisplays(&num_displays);
    CHECK(display_ids != NULL, "SDL_GetDisplays returns a list");
    CHECK(num_displays >= 1, "at least one display (got %d)", num_displays);
    {
        const char *exp = getenv("SDLOP_TEST_EXPECT_DISPLAYS");
        if (exp && *exp) {
            CHECK(num_displays == atoi(exp), "expected %s displays (got %d)", exp, num_displays);
        }
    }
    SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    CHECK(primary == (display_ids && num_displays > 0 ? display_ids[0] : 0),
          "primary display is the first entry");
    for (int i = 0; i < num_displays; i++) {
        const char *name = SDL_GetDisplayName(display_ids[i]);
        CHECK(name != NULL, "display %d has a name", i);
        printf("  display %d: id=%u name=%s\n", i, display_ids[i], name ? name : "?");
        SDL_Rect bounds = { 0, 0, 0, 0 };
        CHECK(SDL_GetDisplayBounds(display_ids[i], &bounds), "display %d bounds", i);
        CHECK(bounds.w > 0 && bounds.h > 0, "display %d bounds positive (%dx%d)",
              i, bounds.w, bounds.h);
        const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display_ids[i]);
        CHECK(desktop != NULL, "display %d desktop mode", i);
        if (desktop) {
            CHECK(desktop->w == bounds.w && desktop->h == bounds.h,
                  "display %d desktop mode matches bounds (%dx%d)", i, desktop->w, desktop->h);
        }
        const SDL_DisplayMode *current = SDL_GetCurrentDisplayMode(display_ids[i]);
        CHECK(current != NULL, "display %d current mode", i);
        int nmodes = 0;
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display_ids[i], &nmodes);
        CHECK(modes != NULL && nmodes >= 1, "display %d has fullscreen modes (got %d)", i, nmodes);
        SDL_DisplayMode closest;
        memset(&closest, 0, sizeof(closest));
        CHECK(SDL_GetClosestFullscreenDisplayMode(display_ids[i], bounds.w / 2, bounds.h / 2,
                                                  0.0f, false, &closest),
              "display %d closest fullscreen mode", i);
        CHECK(SDL_GetDisplayProperties(display_ids[i]) != 0,
              "display %d properties", i);
        free(modes);
    }
    /* bounds are laid out left-to-right and non-overlapping */
    for (int i = 1; i < num_displays; i++) {
        SDL_Rect b0 = { 0, 0, 0, 0 }, b1 = { 0, 0, 0, 0 };
        SDL_GetDisplayBounds(display_ids[i - 1], &b0);
        SDL_GetDisplayBounds(display_ids[i], &b1);
        CHECK(b1.x >= b0.x + b0.w, "display %d does not overlap display %d (x %d vs %d)",
              i, i - 1, b1.x, b0.x + b0.w);
    }

    printf("-- software (dumb buffer) path --\n");
    SDL_Window *w = SDL_CreateWindow("SDLop kmsdrm test", 320, 200, 0);
    if (!w) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    int ww = 0, hh = 0;
    CHECK(SDL_GetWindowSize(w, &ww, &hh), "get size");
    printf("  mode-sized window: %dx%d\n", ww, hh);
    CHECK(ww > 0 && hh > 0, "size positive");

    SDL_Surface *s = SDL_GetWindowSurface(w);
    CHECK(s != NULL, "window surface");
    if (s) {
        CHECK(s->format == SDL_PIXELFORMAT_XRGB8888, "surface format XRGB8888");
        CHECK(s->w == ww && s->h == hh, "surface matches window (%dx%d)", s->w, s->h);
        CHECK(s->pitch >= s->w * 4, "pitch valid");
        CHECK(s->pixels != NULL, "surface maps the dumb buffer");
        /* read the bands BEFORE presenting: after a page flip the cached
         * surface targets the other dumb buffer (double buffering) */
        paint_bands(s);
        Uint8 r = 0, g = 0, b = 0, a = 0;
        if (SDL_ReadSurfacePixel(s, s->w / 2, s->h / 6, &r, &g, &b, &a)) {
            CHECK(r == 0xC0 && g == 0 && b == 0, "top band is red (got %02x %02x %02x)", r, g, b);
        }
        if (SDL_ReadSurfacePixel(s, s->w / 2, s->h / 2, &r, &g, &b, &a)) {
            CHECK(r == 0 && g == 0xC0 && b == 0, "middle band is green (got %02x %02x %02x)", r, g, b);
        }
        CHECK(SDL_UpdateWindowSurface(w), "update window surface (present)");
        /* second frame, different color order -> exercise flip path twice */
        SDL_FillSurfaceRect(s, NULL, 0x00C0C000u);
        CHECK(SDL_UpdateWindowSurface(w), "second present");
        paint_bands(s);
        CHECK(SDL_UpdateWindowSurface(w), "third present (bands again)");
    }

    /* resize: driver should pick the closest mode and RESIZE the surface */
    printf("-- mode change --\n");
    if (SDL_SetWindowSize(w, 640, 480)) {
        SDL_Surface *s2 = SDL_GetWindowSurface(w); /* core returns cached; driver retargeted it */
        SDL_GetWindowSize(w, &ww, &hh);
        printf("  after SetWindowSize(640,480): window %dx%d surface %dx%d\n",
               ww, hh, s2 ? s2->w : -1, s2 ? s2->h : -1);
        CHECK(ww > 0 && hh > 0, "size after resize positive");
        paint_bands(s2 ? s2 : s);
        CHECK(SDL_UpdateWindowSurface(w), "present after resize");
    } else {
        printf("  note: SDL_SetWindowSize failed: %s\n", SDL_GetError());
    }

    /* multi-display: move the window to the second display and back */
    if (num_displays >= 2) {
        printf("-- multi-display window move --\n");
        SDL_Rect b1 = { 0, 0, 0, 0 };
        SDL_GetDisplayBounds(display_ids[1], &b1);
        SDL_Point p1 = { b1.x + b1.w / 2, b1.y + b1.h / 2 };
        CHECK(SDL_GetDisplayForPoint(&p1) == display_ids[1],
              "point on the second display resolves to it");
        CHECK(SDL_SetWindowPosition(w, p1.x, p1.y), "move window to second display");
        SDL_DisplayID on = SDL_GetDisplayForWindow(w);
        printf("  window now on display %u (second=%u)\n", on, display_ids[1]);
        CHECK(on == display_ids[1], "window reports second display");
        SDL_Surface *smove = SDL_GetWindowSurface(w);
        if (smove) {
            paint_bands(smove);
            CHECK(SDL_UpdateWindowSurface(w), "present on second display");
        }
        /* and back to the first display */
        CHECK(SDL_SetWindowPosition(w, 0, 0), "move window back to first display");
        CHECK(SDL_GetDisplayForWindow(w) == display_ids[0], "window reports first display");
    }

    /* fullscreen mode selection (exclusive fullscreen mode API) */
    {
        printf("-- fullscreen display mode request --\n");
        int nmodes = 0;
        SDL_DisplayID d0 = (num_displays > 0) ? display_ids[0] : primary;
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(d0, &nmodes);
        CHECK(modes && nmodes >= 1, "first display has fullscreen modes");
        if (modes && nmodes >= 1) {
            /* request the smallest listed mode; window must store it */
            SDL_DisplayMode want = *modes[nmodes - 1]; /* sorted largest first */
            CHECK(SDL_SetWindowFullscreenMode(w, &want),
                  "set window fullscreen mode %dx%d", want.w, want.h);
            const SDL_DisplayMode *stored = SDL_GetWindowFullscreenMode(w);
            CHECK(stored != NULL, "window reports the exclusive fullscreen mode");
            if (stored) {
                CHECK(stored->w == want.w && stored->h == want.h,
                      "exclusive mode matches request (%dx%d)", stored->w, stored->h);
            }
            /* NULL restores borderless/desktop fullscreen mode */
            CHECK(SDL_SetWindowFullscreenMode(w, NULL), "clear fullscreen mode");
            CHECK(SDL_GetWindowFullscreenMode(w) == NULL, "exclusive mode cleared");
        }
        free(modes);
    }

    /* fullscreen toggle */
    printf("-- fullscreen --\n");
    if (SDL_SetWindowFullscreen(w, true)) {
        SDL_GetWindowSize(w, &ww, &hh);
        printf("  fullscreen mode: %dx%d\n", ww, hh);
        CHECK(SDL_UpdateWindowSurface(w), "present in fullscreen");
        SDL_SetWindowFullscreen(w, false);
        SDL_GetWindowSize(w, &ww, &hh);
        printf("  back to windowed: %dx%d\n", ww, hh);
    } else {
        printf("  note: fullscreen toggle failed: %s\n", SDL_GetError());
    }

    /* optional: keep the banded frame on screen so an external observer
     * (QEMU screendump) can verify the actual scanout */
    {
        const char *hold = getenv("SDLOP_TEST_KMSDRM_HOLD");
        if (hold && *hold) {
            int secs = atoi(hold);
            if (secs > 0) {
                /* re-present the bands right before holding */
                SDL_Surface *s3 = SDL_GetWindowSurface(w);
                if (s3) {
                    paint_bands(s3);
                    SDL_UpdateWindowSurface(w);
                }
                for (int t = 0; t < secs * 20; t++) {
                    SDL_PumpEvents();
                    SDL_Delay(50);
                }
            }
        }
    }
    SDL_DestroyWindow(w);
    free(display_ids);
    display_ids = NULL;

    printf("-- GL (GBM + EGL) path --\n");
    SDL_Window *glw = SDL_CreateWindow("SDLop kmsdrm gl test", 640, 480, SDL_WINDOW_OPENGL);
    if (!glw) {
        printf("  note: GL window failed: %s\n", SDL_GetError());
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GLContext ctx = SDL_GL_CreateContext(glw);
        if (!ctx) {
            printf("  SKIP GL context (GBM/EGL missing?): %s\n", SDL_GetError());
        } else {
            SDL_GL_SetSwapInterval(0);
            int swap_failures = 0;
            /* clear to a distinctive color a few times */
            for (int i = 0; i < 5; i++) {
                /* GLES2 clear via raw GL symbols resolved lazily by the test */
                typedef void (*PFN_glClearColor)(float, float, float, float);
                typedef void (*PFN_glClear)(unsigned int);
                static PFN_glClearColor pClearColor;
                static PFN_glClear pClear;
                if (!pClearColor) {
                    pClearColor = (PFN_glClearColor)SDL_GL_GetProcAddress("glClearColor");
                    pClear = (PFN_glClear)SDL_GL_GetProcAddress("glClear");
                }
                if (pClearColor && pClear) {
                    pClearColor(0.9f, 0.1f, 0.5f, 1.0f); /* magenta-ish */
                    pClear(0x00004000u);                /* GL_COLOR_BUFFER_BIT */
                }
                if (!SDL_GL_SwapWindow(glw)) {
                    swap_failures++;
                    fprintf(stderr, "  note: GL swap %d failed: %s\n", i, SDL_GetError());
                }
            }
            /* optional: keep a magenta GL frame on screen for screendump checks
             * (hold while the context is still alive so the swap loop is valid) */
            const char *hold = getenv("SDLOP_TEST_KMSDRM_HOLD_GL");
            if (hold && *hold) {
                int secs = atoi(hold);
                if (secs > 0) {
                    for (int t = 0; t < secs * 20; t++) {
                        if (!SDL_GL_SwapWindow(glw)) {
                            swap_failures++;
                            fprintf(stderr, "  note: GL hold swap failed: %s\n", SDL_GetError());
                            break;
                        }
                        SDL_PumpEvents();
                        SDL_Delay(50);
                    }
                }
            }
            CHECK(swap_failures == 0, "GL swaps all presented (GBM + EGL + page flip)");
            SDL_GL_DestroyContext(ctx);
        }
        SDL_DestroyWindow(glw);
    }

    SDL_Quit();
    printf("test_kmsdrm: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
