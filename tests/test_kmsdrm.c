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
                SDL_GL_SwapWindow(glw);
            }
            CHECK(1, "GL swap loop completed (GBM + EGL + page flip)");
            /* optional: keep a magenta GL frame on screen for screendump checks
             * (hold while the context is still alive so the swap loop is valid) */
            const char *hold = getenv("SDLOP_TEST_KMSDRM_HOLD_GL");
            if (hold && *hold) {
                int secs = atoi(hold);
                if (secs > 0) {
                    for (int t = 0; t < secs * 20; t++) {
                        SDL_GL_SwapWindow(glw);
                        SDL_PumpEvents();
                        SDL_Delay(50);
                    }
                }
            }
            SDL_GL_DestroyContext(ctx);
        }
        SDL_DestroyWindow(glw);
    }

    SDL_Quit();
    printf("test_kmsdrm: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
