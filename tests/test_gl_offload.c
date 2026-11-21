/*
  SDLop test: headless GPU GL offload detection, policy, pixel Y-flip/format
  conversion, and live backend integration (Wayland/KMS when present).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include "video/sdlop_gl_offload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                           \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
            fprintf(stderr, __VA_ARGS__);                          \
            fprintf(stderr, "\n");                                 \
            failures++;                                            \
        }                                                          \
    } while (0)

static void test_policy(void)
{
    SDL_unsetenv_unsafe("SDLOP_GL_OFFLOAD");
    SDL_unsetenv_unsafe("SDLOP_OFFLOAD");
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_AUTO, "unset -> AUTO");

    SDL_setenv_unsafe("SDLOP_OFFLOAD", "0", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_NEVER, "SDLOP_OFFLOAD=0 -> NEVER");
    SDL_setenv_unsafe("SDLOP_OFFLOAD", "1", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_FORCE, "SDLOP_OFFLOAD=1 -> FORCE");
    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "0", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_NEVER, "SDLOP_GL_OFFLOAD overrides SDLOP_OFFLOAD");
    SDL_unsetenv_unsafe("SDLOP_OFFLOAD");

    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "auto", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_AUTO, "auto -> AUTO");

    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "0", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_NEVER, "0 -> NEVER");
    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "off", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_NEVER, "off -> NEVER");
    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "FALSE", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_NEVER, "FALSE -> NEVER");
    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "no", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_NEVER, "no -> NEVER");

    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "1", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_FORCE, "1 -> FORCE");
    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "on", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_FORCE, "on -> FORCE");
    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "TRUE", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_FORCE, "TRUE -> FORCE");
    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "force", 1);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_FORCE, "force -> FORCE");

    SDL_unsetenv_unsafe("SDLOP_GL_OFFLOAD");

    /* the same options from code (SDL_SetHint) */
    CHECK(SDL_SetHint(SDLOP_HINT_GL_OFFLOAD, "1"), "set hint: %s", SDL_GetError());
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_FORCE, "hint SDLOP_GL_OFFLOAD=1 -> FORCE");
    SDL_ResetHint(SDLOP_HINT_GL_OFFLOAD);
    SDL_SetHint(SDLOP_HINT_OFFLOAD, "no");
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_NEVER, "hint SDLOP_OFFLOAD=no -> NEVER");
    SDL_SetHint(SDLOP_HINT_GL_OFFLOAD, "force");
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_FORCE, "hint SDLOP_GL_OFFLOAD wins over hint SDLOP_OFFLOAD");
    SDL_ResetHint(SDLOP_HINT_GL_OFFLOAD);
    SDL_ResetHint(SDLOP_HINT_OFFLOAD);
    CHECK(SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_AUTO, "reset hints -> AUTO");
}

static void test_software_renderer_detection(void)
{
    CHECK(SDLOP_GLOffload_IsSoftwareRenderer("llvmpipe (LLVM 19.1.1, 256 bits)"), "llvmpipe");
    CHECK(SDLOP_GLOffload_IsSoftwareRenderer("softpipe"), "softpipe");
    CHECK(SDLOP_GLOffload_IsSoftwareRenderer("Mesa DRI swrast"), "swrast");
    CHECK(SDLOP_GLOffload_IsSoftwareRenderer("Software Rasterizer"), "Software Rasterizer");
    CHECK(SDLOP_GLOffload_IsSoftwareRenderer("SWR (LLVM 15.0)"), "SWR");
    CHECK(SDLOP_GLOffload_IsSoftwareRenderer("Mesa SWR"), "Mesa SWR");
    CHECK(SDLOP_GLOffload_IsSoftwareRenderer("OpenSWR"), "OpenSWR");
    CHECK(SDLOP_GLOffload_IsSoftwareRenderer("zink Vulkan 1.3(llvmpipe (LLVM 19.1))"), "zink llvmpipe");
    CHECK(SDLOP_GLOffload_IsSoftwareRenderer("lavapipe"), "lavapipe");

    CHECK(!SDLOP_GLOffload_IsSoftwareRenderer(NULL), "NULL is not software");
    CHECK(!SDLOP_GLOffload_IsSoftwareRenderer(""), "empty is not software");
    CHECK(!SDLOP_GLOffload_IsSoftwareRenderer("AMD Radeon RX 7900 XT (radeonsi, navi31)"), "AMD GPU");
    CHECK(!SDLOP_GLOffload_IsSoftwareRenderer("Mesa Intel(R) UHD Graphics 770 (ADL-S GT1)"), "Intel GPU");
    CHECK(!SDLOP_GLOffload_IsSoftwareRenderer("NVIDIA GeForce RTX 4090/PCIe/SSE2"), "NVIDIA GPU");
    CHECK(!SDLOP_GLOffload_IsSoftwareRenderer("virgl (NVIDIA GeForce)"), "virgl GPU");
    CHECK(!SDLOP_GLOffload_IsSoftwareRenderer("swr_custom_hw"), "swr_custom_hw token boundary");
}

static void test_flip_copy(void)
{
    /* 2x2 source bottom-up:
     *   row 0 (bottom): [Red, Green]
     *   row 1 (top):    [Blue, Yellow]
     * Expected top-down XRGB8888:
     *   row 0 (top):    [Blue, Yellow]
     *   row 1 (bottom): [Red, Green]
     */
    const uint8_t rgba_src[2 * 2 * 4] = {
        /* bottom row (y=0 in GL) */
        0xFF, 0x00, 0x00, 0x80,   /* Red */
        0x00, 0xFF, 0x00, 0x80,   /* Green */
        /* top row (y=1 in GL) */
        0x00, 0x00, 0xFF, 0x80,   /* Blue */
        0xFF, 0xFF, 0x00, 0x80,   /* Yellow */
    };

    /* Use pitch = 16 (4 extra padding bytes per row) with canary bytes. */
    uint8_t dst[2 * 16];
    memset(dst, 0xA5, sizeof(dst));
    SDLOP_GLOffload_FlipCopyToXRGB8888(rgba_src, 2, 2, SDLOP_GL_OFFLOAD_RGBA8888, dst, 16, 2, 2);

    const uint32_t *row0 = (const uint32_t *)(dst + 0);
    const uint32_t *row1 = (const uint32_t *)(dst + 16);
    CHECK(row0[0] == 0xFF0000FFu, "RGBA top-left blue (got 0x%08x)", row0[0]);
    CHECK(row0[1] == 0xFFFFFF00u, "RGBA top-right yellow (got 0x%08x)", row0[1]);
    CHECK(row1[0] == 0xFFFF0000u, "RGBA bottom-left red (got 0x%08x)", row1[0]);
    CHECK(row1[1] == 0xFF00FF00u, "RGBA bottom-right green (got 0x%08x)", row1[1]);
    CHECK(dst[8] == 0xA5 && dst[15] == 0xA5 && dst[24] == 0xA5, "padding untouched");

    /* BGRA path (little-endian XRGB8888 bytes [B, G, R, X]) */
    const uint8_t bgra_src[2 * 2 * 4] = {
        /* bottom row */
        0x00, 0x00, 0xFF, 0xFF,   /* Red */
        0x00, 0xFF, 0x00, 0xFF,   /* Green */
        /* top row */
        0xFF, 0x00, 0x00, 0xFF,   /* Blue */
        0x00, 0xFF, 0xFF, 0xFF,   /* Yellow */
    };
    memset(dst, 0x5A, sizeof(dst));
    SDLOP_GLOffload_FlipCopyToXRGB8888(bgra_src, 2, 2, SDLOP_GL_OFFLOAD_BGRA8888, dst, 16, 2, 2);
    CHECK(row0[0] == 0xFF0000FFu, "BGRA top-left blue (got 0x%08x)", row0[0]);
    CHECK(row0[1] == 0xFFFFFF00u, "BGRA top-right yellow (got 0x%08x)", row0[1]);
    CHECK(row1[0] == 0xFFFF0000u, "BGRA bottom-left red (got 0x%08x)", row1[0]);
    CHECK(row1[1] == 0xFF00FF00u, "BGRA bottom-right green (got 0x%08x)", row1[1]);
    CHECK(dst[8] == 0x5A && dst[24] == 0x5A, "BGRA padding untouched");

    /* RGB path (driver-preferred format on NVIDIA) */
    const uint8_t rgb_src[2 * 2 * 3] = {
        0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00,
        0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00,
    };
    memset(dst, 0x3C, sizeof(dst));
    SDLOP_GLOffload_FlipCopyToXRGB8888(rgb_src, 2, 2, SDLOP_GL_OFFLOAD_RGB888, dst, 16, 2, 2);
    CHECK(row0[0] == 0xFF0000FFu, "RGB top-left blue (got 0x%08x)", row0[0]);
    CHECK(row0[1] == 0xFFFFFF00u, "RGB top-right yellow (got 0x%08x)", row0[1]);
    CHECK(row1[0] == 0xFFFF0000u, "RGB bottom-left red (got 0x%08x)", row1[0]);
    CHECK(row1[1] == 0xFF00FF00u, "RGB bottom-right green (got 0x%08x)", row1[1]);

    /* Clipping: 2x2 source into 1x1 destination -> gets top-left pixel (Blue). */
    uint32_t small[1] = { 0 };
    SDLOP_GLOffload_FlipCopyToXRGB8888(rgba_src, 2, 2, SDLOP_GL_OFFLOAD_RGBA8888, small, 4, 1, 1);
    CHECK(small[0] == 0xFF0000FFu, "clipped 1x1 gets top-left blue (got 0x%08x)", small[0]);

    /* Wide row: exercises the SIMD loops in both RGB and RGBA paths. The
     * RGB source is exactly sized so an overreading load would be caught. */
    const uint8_t rgb8[8 * 3] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
    };
    const uint8_t rgba8[8 * 4] = {
        0x11, 0x22, 0x33, 0x80, 0x44, 0x55, 0x66, 0x80,
        0x77, 0x88, 0x99, 0x80, 0xAA, 0xBB, 0xCC, 0x80,
        0x01, 0x02, 0x03, 0x80, 0x04, 0x05, 0x06, 0x80,
        0x07, 0x08, 0x09, 0x80, 0x0A, 0x0B, 0x0C, 0x80,
    };
    const uint32_t expected[8] = {
        0xFF112233u, 0xFF445566u, 0xFF778899u, 0xFFAABBCCu,
        0xFF010203u, 0xFF040506u, 0xFF070809u, 0xFF0A0B0Cu,
    };
    uint32_t simd_dst[8];
    SDLOP_GLOffload_FlipCopyToXRGB8888(rgb8, 8, 1, SDLOP_GL_OFFLOAD_RGB888, simd_dst, 32, 8, 1);
    for (int i = 0; i < 8; i++) {
        CHECK(simd_dst[i] == expected[i], "RGB SIMD pixel %d (got 0x%08x)", i, simd_dst[i]);
    }
    SDLOP_GLOffload_FlipCopyToXRGB8888(rgba8, 8, 1, SDLOP_GL_OFFLOAD_RGBA8888, simd_dst, 32, 8, 1);
    for (int i = 0; i < 8; i++) {
        CHECK(simd_dst[i] == expected[i], "RGBA SIMD pixel %d (got 0x%08x)", i, simd_dst[i]);
    }
}

static void test_gl_attrs(void)
{
    SDL_GL_ResetAttributes();
    int accel = -1;
    CHECK(SDL_GL_GetAttribute(SDL_GL_ACCELERATED_VISUAL, &accel) && accel == 0,
          "default ACCELERATED_VISUAL is 0 before context (got %d)", accel);
    CHECK(SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1), "set ACCELERATED_VISUAL=1");
    CHECK(SDL_GL_GetAttribute(SDL_GL_ACCELERATED_VISUAL, &accel) && accel == 1,
          "get ACCELERATED_VISUAL==1 (got %d)", accel);
    SDL_GL_ResetAttributes();
}

static void test_live_backend(void)
{
    if (!getenv("WAYLAND_DISPLAY")) {
        printf("test_gl_offload: live Wayland GL check skipped (WAYLAND_DISPLAY not set)\n");
        return;
    }

    typedef void (*PFN_glClearColor)(float, float, float, float);
    typedef void (*PFN_glClear)(unsigned int);
    typedef void (*PFN_glReadPixels)(int, int, int, int, unsigned int, unsigned int, void *);

    SDL_setenv_unsafe("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    static const char *const modes[] = { "0", "1" };
    for (size_t i = 0; i < 2; i++) {
        SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", modes[i], 1);
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            fprintf(stderr, "live mode %s: SDL_Init failed: %s\n", modes[i], SDL_GetError());
            failures++;
            continue;
        }
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_Window *w = SDL_CreateWindow("offload live", 160, 120, SDL_WINDOW_OPENGL);
        CHECK(w != NULL, "mode %s: window: %s", modes[i], SDL_GetError());
        if (w) {
            SDL_GLContext ctx = SDL_GL_CreateContext(w);
            CHECK(ctx != NULL, "mode %s: context: %s", modes[i], SDL_GetError());
            if (ctx) {
                PFN_glClearColor pClearColor = (PFN_glClearColor)SDL_GL_GetProcAddress("glClearColor");
                PFN_glClear pClear = (PFN_glClear)SDL_GL_GetProcAddress("glClear");
                PFN_glReadPixels pReadPixels = (PFN_glReadPixels)SDL_GL_GetProcAddress("glReadPixels");
                CHECK(pClearColor && pClear && pReadPixels, "mode %s: GL procs", modes[i]);
                if (pClearColor && pClear && pReadPixels) {
                    pClearColor(0.2f, 0.8f, 0.4f, 1.0f);
                    pClear(0x00004000u);
                    CHECK(SDL_GL_SwapWindow(w), "mode %s: swap: %s", modes[i], SDL_GetError());
                    Uint8 px[4] = { 0 };
                    pReadPixels(80, 60, 1, 1, 0x1908u, 0x1401u, px);
                    CHECK(px[1] > 190 && px[0] < 70 && px[2] > 80 && px[2] < 130,
                          "mode %s: pixel (%u %u %u)", modes[i], px[0], px[1], px[2]);
                }
                SDL_GL_DestroyContext(ctx);
            }
            SDL_DestroyWindow(w);
        }
        SDL_Quit();
    }
    SDL_unsetenv_unsafe("SDLOP_GL_OFFLOAD");
}

static int find_attrib_val(const int *attribs, int n, int key, int *val_out)
{
    for (int i = 0; i + 1 < n; i += 2) {
        if (attribs[i] == key) {
            if (val_out) {
                *val_out = attribs[i + 1];
            }
            return 1;
        }
    }
    return 0;
}

static void test_build_context_attribs(void)
{
    enum {
        kEGL_OPENGL_ES_API = 0x30A0,
        kEGL_OPENGL_API = 0x30A2,
        kEGL_CONTEXT_CLIENT_VERSION = 0x3098,
        kEGL_CONTEXT_MAJOR_VERSION_KHR = 0x3098,
        kEGL_CONTEXT_MINOR_VERSION_KHR = 0x30FB,
        kEGL_CONTEXT_FLAGS_KHR = 0x30FC,
        kEGL_CONTEXT_OPENGL_PROFILE_MASK_KHR = 0x30FD,
        kEGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR = 0x00000001,
        kEGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR = 0x00000002,
        kEGL_CONTEXT_OPENGL_DEBUG_BIT_KHR = 0x00000001,
        kEGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR = 0x00000002,
    };

    int attribs[16];
    int n = 0;
    int val = 0;

    /* Core 3.2, no flags -> no EGL_CONTEXT_FLAGS_KHR emitted */
    n = SDLOP_GLOffload_BuildContextAttribs(kEGL_OPENGL_API, 3, 2,
                                            SDL_GL_CONTEXT_PROFILE_CORE, 0,
                                            attribs, 16);
    CHECK(n > 0 && (n % 2) == 0, "Core no-flags attrib count valid (%d)", n);
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_MAJOR_VERSION_KHR, &val) && val == 3, "major==3");
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_MINOR_VERSION_KHR, &val) && val == 2, "minor==2");
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, &val) &&
          val == kEGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, "core profile mask");
    CHECK(!find_attrib_val(attribs, n, kEGL_CONTEXT_FLAGS_KHR, NULL), "no flags -> no EGL_CONTEXT_FLAGS_KHR");

    /* Core 3.2 + DEBUG */
    n = SDLOP_GLOffload_BuildContextAttribs(kEGL_OPENGL_API, 3, 2,
                                            SDL_GL_CONTEXT_PROFILE_CORE,
                                            SDL_GL_CONTEXT_DEBUG_FLAG,
                                            attribs, 16);
    CHECK(n > 0 && (n % 2) == 0, "Core DEBUG attrib count valid (%d)", n);
    for (int i = 0; i < n; i += 2) {
        CHECK(attribs[i] >= 0x3000, "Core DEBUG key[%d]=0x%x must be valid EGL attrib", i, attribs[i]);
    }
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_FLAGS_KHR, &val) &&
          val == kEGL_CONTEXT_OPENGL_DEBUG_BIT_KHR,
          "Core DEBUG emits EGL_CONTEXT_FLAGS_KHR=0x%x (got 0x%x)",
          kEGL_CONTEXT_OPENGL_DEBUG_BIT_KHR, val);

    /* Core 3.2 + FORWARD_COMPATIBLE */
    n = SDLOP_GLOffload_BuildContextAttribs(kEGL_OPENGL_API, 3, 2,
                                            SDL_GL_CONTEXT_PROFILE_CORE,
                                            SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG,
                                            attribs, 16);
    CHECK(n > 0 && (n % 2) == 0, "Core FWD attrib count valid (%d)", n);
    for (int i = 0; i < n; i += 2) {
        CHECK(attribs[i] >= 0x3000, "Core FWD key[%d]=0x%x must be valid EGL attrib", i, attribs[i]);
    }
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_FLAGS_KHR, &val) &&
          val == kEGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR,
          "Core FWD emits EGL_CONTEXT_FLAGS_KHR=0x%x (got 0x%x)",
          kEGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR, val);

    /* Core 3.2 + DEBUG | FORWARD_COMPATIBLE combined bitmask */
    n = SDLOP_GLOffload_BuildContextAttribs(kEGL_OPENGL_API, 3, 2,
                                            SDL_GL_CONTEXT_PROFILE_CORE,
                                            SDL_GL_CONTEXT_DEBUG_FLAG |
                                            SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG,
                                            attribs, 16);
    CHECK(n > 0 && (n % 2) == 0, "Core DEBUG|FWD attrib count valid (%d)", n);
    for (int i = 0; i < n; i += 2) {
        CHECK(attribs[i] >= 0x3000, "Core DEBUG|FWD key[%d]=0x%x must be valid EGL attrib", i, attribs[i]);
    }
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_FLAGS_KHR, &val) &&
          val == (kEGL_CONTEXT_OPENGL_DEBUG_BIT_KHR | kEGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR),
          "Core DEBUG|FWD emits combined bitmask 0x%x (got 0x%x)",
          kEGL_CONTEXT_OPENGL_DEBUG_BIT_KHR | kEGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR, val);

    /* Compatibility profile + DEBUG */
    n = SDLOP_GLOffload_BuildContextAttribs(kEGL_OPENGL_API, 3, 0,
                                            SDL_GL_CONTEXT_PROFILE_COMPATIBILITY,
                                            SDL_GL_CONTEXT_DEBUG_FLAG,
                                            attribs, 16);
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, &val) &&
          val == kEGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR, "compat profile mask");
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_FLAGS_KHR, &val) &&
          val == kEGL_CONTEXT_OPENGL_DEBUG_BIT_KHR, "compat DEBUG flag");

    /* ES 2.0 + DEBUG */
    n = SDLOP_GLOffload_BuildContextAttribs(kEGL_OPENGL_ES_API, 2, 0,
                                            SDL_GL_CONTEXT_PROFILE_ES,
                                            SDL_GL_CONTEXT_DEBUG_FLAG,
                                            attribs, 16);
    CHECK(n > 0 && (n % 2) == 0, "ES DEBUG attrib count valid (%d)", n);
    for (int i = 0; i < n; i += 2) {
        CHECK(attribs[i] >= 0x3000, "ES DEBUG key[%d]=0x%x must be valid EGL attrib", i, attribs[i]);
    }
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_CLIENT_VERSION, &val) && val == 2, "ES2 version");
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_FLAGS_KHR, &val) &&
          val == kEGL_CONTEXT_OPENGL_DEBUG_BIT_KHR, "ES2 DEBUG emits EGL_CONTEXT_FLAGS_KHR");

    /* requested ES versions pass through like stock: 1.0 stays 1, 3.1 is
     * major/minor (it used to collapse to "3"), 9.0 is not clamped */
    n = SDLOP_GLOffload_BuildContextAttribs(kEGL_OPENGL_ES_API, 1, 0, SDL_GL_CONTEXT_PROFILE_ES, 0, attribs, 16);
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_CLIENT_VERSION, &val) && val == 1, "ES1 version (%d)", val);
    n = SDLOP_GLOffload_BuildContextAttribs(kEGL_OPENGL_ES_API, 3, 1, SDL_GL_CONTEXT_PROFILE_ES, 0, attribs, 16);
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_MAJOR_VERSION_KHR, &val) && val == 3, "ES3.1 major (%d)", val);
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_MINOR_VERSION_KHR, &val) && val == 1, "ES3.1 minor (%d)", val);
    n = SDLOP_GLOffload_BuildContextAttribs(kEGL_OPENGL_ES_API, 9, 0, SDL_GL_CONTEXT_PROFILE_ES, 0, attribs, 16);
    CHECK(find_attrib_val(attribs, n, kEGL_CONTEXT_CLIENT_VERSION, &val) && val == 9, "ES9 not clamped (%d)", val);
}

static void test_context_flags_offload(void)
{
    typedef void (*PFN_glClearColor)(float, float, float, float);
    typedef void (*PFN_glClear)(unsigned int);
    typedef void (*PFN_glGetIntegerv)(unsigned int, int *);

    SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv_unsafe("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "1", 1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("test_gl_offload: context flags check skipped (SDL_Init: %s)\n", SDL_GetError());
        goto done;
    }

    /* Probe whether EGL + Mesa is present at all using a plain ES2 context. */
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_Window *probe_w = SDL_CreateWindow("gl-flags-probe", 32, 32, SDL_WINDOW_OPENGL);
    if (!probe_w) {
        SDL_Quit();
        printf("test_gl_offload: context flags check skipped (no GL window)\n");
        goto done;
    }
    SDL_GLContext probe_ctx = SDL_GL_CreateContext(probe_w);
    if (!probe_ctx) {
        SDL_DestroyWindow(probe_w);
        SDL_Quit();
        printf("test_gl_offload: context flags check skipped (no EGL offload: %s)\n", SDL_GetError());
        goto done;
    }
    SDL_GL_DestroyContext(probe_ctx);
    SDL_DestroyWindow(probe_w);

    static const struct {
        const char *name;
        int profile;
        int major;
        int minor;
        int flags;
    } cases[] = {
        { "Core 3.2 + DEBUG",
          SDL_GL_CONTEXT_PROFILE_CORE, 3, 2, SDL_GL_CONTEXT_DEBUG_FLAG },
        { "Core 3.2 + FORWARD_COMPATIBLE",
          SDL_GL_CONTEXT_PROFILE_CORE, 3, 2, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG },
        { "Core 3.2 + DEBUG | FORWARD_COMPATIBLE",
          SDL_GL_CONTEXT_PROFILE_CORE, 3, 2,
          SDL_GL_CONTEXT_DEBUG_FLAG | SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG },
        { "ES 2.0 + DEBUG",
          SDL_GL_CONTEXT_PROFILE_ES, 2, 0, SDL_GL_CONTEXT_DEBUG_FLAG },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, cases[i].profile);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, cases[i].major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, cases[i].minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, cases[i].flags);

        SDL_Window *w = SDL_CreateWindow(cases[i].name, 64, 48, SDL_WINDOW_OPENGL);
        CHECK(w != NULL, "%s: SDL_CreateWindow: %s", cases[i].name, SDL_GetError());
        if (!w) {
            continue;
        }
        SDL_GLContext ctx = SDL_GL_CreateContext(w);
        CHECK(ctx != NULL, "%s: SDL_GL_CreateContext: %s", cases[i].name, SDL_GetError());
        if (ctx) {
            if (cases[i].profile == SDL_GL_CONTEXT_PROFILE_CORE) {
                PFN_glGetIntegerv pGetIntegerv = (PFN_glGetIntegerv)SDL_GL_GetProcAddress("glGetIntegerv");
                if (pGetIntegerv) {
                    int gl_flags = 0;
                    /* GL_CONTEXT_FLAGS == 0x821E; bit 0 = FORWARD_COMPATIBLE (1), bit 1 = DEBUG (2) */
                    pGetIntegerv(0x821Eu, &gl_flags);
                    int expect = 0;
                    if (cases[i].flags & SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG) {
                        expect |= 1;
                    }
                    if (cases[i].flags & SDL_GL_CONTEXT_DEBUG_FLAG) {
                        expect |= 2;
                    }
                    CHECK((gl_flags & expect) == expect,
                          "%s: GL_CONTEXT_FLAGS=0x%x missing expected 0x%x",
                          cases[i].name, gl_flags, expect);
                }
            }

            PFN_glClearColor pClearColor = (PFN_glClearColor)SDL_GL_GetProcAddress("glClearColor");
            PFN_glClear pClear = (PFN_glClear)SDL_GL_GetProcAddress("glClear");
            CHECK(pClearColor && pClear, "%s: GL procs", cases[i].name);
            if (pClearColor && pClear) {
                pClearColor(0.25f, 0.5f, 0.75f, 1.0f);
                pClear(0x00004000u);
                CHECK(SDL_GL_SwapWindow(w), "%s: swap: %s", cases[i].name, SDL_GetError());
                SDL_Surface *fb = SDL_GetWindowSurface(w);
                CHECK(fb && fb->pixels, "%s: window surface populated", cases[i].name);
            }
            SDL_GL_DestroyContext(ctx);
        }
        SDL_DestroyWindow(w);
    }

    SDL_Quit();
done:
    SDL_unsetenv_unsafe("SDLOP_GL_OFFLOAD");
    SDL_unsetenv_unsafe("SDL_VIDEODRIVER");
}

static void test_dummy_opt_out(void)
{
    SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv_unsafe("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    SDL_setenv_unsafe("SDLOP_GL_OFFLOAD", "0", 1);
    if (SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Window *w = SDL_CreateWindow("dummy no offload", 64, 64, SDL_WINDOW_OPENGL);
        if (w) {
            CHECK(SDL_GL_CreateContext(w) == NULL, "dummy GL fails when SDLOP_GL_OFFLOAD=0");
            SDL_DestroyWindow(w);
        }
        SDL_Quit();
    }
    SDL_unsetenv_unsafe("SDLOP_GL_OFFLOAD");
    SDL_unsetenv_unsafe("SDL_VIDEODRIVER");
}

int main(void)
{
    test_policy();
    test_software_renderer_detection();
    test_flip_copy();
    test_gl_attrs();
    test_build_context_attribs();
    test_live_backend();
    test_context_flags_offload();
    test_dummy_opt_out();

    if (failures) {
        printf("test_gl_offload: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_gl_offload: PASS\n");
    return 0;
}
