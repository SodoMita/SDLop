/*
  SDLop test: OpenGL API matrix over EGL/llvmpipe.

  Creates contexts of several flavors and verifies each one:
    - OpenGL ES 2.0
    - OpenGL ES 3.0 + offscreen FBO render/read-back
    - desktop GL 3.3 core profile + FBO
    - desktop GL 4.5 core profile + FBO (skipped if the driver refuses)
    - SDL_GL_SHARE_WITH_CURRENT_CONTEXT: a texture created in context A
      is readable through an FBO in shared context B

  Skips cleanly when no Wayland compositor is running.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <GLES2/gl2.h>
#include <stdio.h>

#ifndef GL_RGBA8_OES /* gl2ext token; same value as core GL_RGBA8 */
#define GL_RGBA8_OES 0x8058
#endif
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int skipped = 0;

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);         \
            fprintf(stderr, "\n");                \
            failures++;                           \
        }                                         \
    } while (0)

/* verify a clear color through an FBO (works on ES2+ and core profiles
 * without needing a shader pipeline) */
static void check_fbo(SDL_GLContext ctx, const char *label)
{
    (void)ctx;
    GLuint fbo = 0, rbo = 0;
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(1, &rbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, 16, 16);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        CHECK(false, "%s: framebuffer incomplete (0x%x)", label, status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteRenderbuffers(1, &rbo);
        glDeleteFramebuffers(1, &fbo);
        return;
    }
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    Uint8 px[4] = { 0 };
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px[2] > 250 && px[0] < 5 && px[1] < 5,
          "%s: FBO pixel not blue (%u %u %u)", label, px[0], px[1], px[2]);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteRenderbuffers(1, &rbo);
    glDeleteFramebuffers(1, &fbo);
}

/* clear the default framebuffer and swap */
static void check_clear_swap(SDL_Window *w, const char *label)
{
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    CHECK(SDL_GL_SwapWindow(w), "%s: swap: %s", label, SDL_GetError());
    Uint8 px[4] = { 0 };
    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px[1] > 250 && px[0] < 5, "%s: clear not green (%u %u %u)", label, px[0], px[1], px[2]);
}

struct cfg
{
    const char *label;
    int profile;      /* SDL_GL_CONTEXT_PROFILE_* or 0 */
    int major, minor;
    const char *must_contain; /* substring in GL_VERSION ("" = none) */
    bool optional;    /* skip (not fail) when unsupported */
};

/* parse the leading "X.Y" (after an optional "OpenGL ES " prefix) */
static bool version_at_least(const char *ver, int major, int minor)
{
    if (!ver) {
        return false;
    }
    const char *p = strstr(ver, "OpenGL ES ");
    p = p ? p + 10 : ver;
    int got_maj = 0, got_min = 0;
    if (sscanf(p, "%d.%d", &got_maj, &got_min) < 1) {
        return false;
    }
    return got_maj > major || (got_maj == major && got_min >= minor);
}

int main(void)
{
    if (!getenv("WAYLAND_DISPLAY")) {
        printf("test_gl_matrix: SKIP (WAYLAND_DISPLAY not set)\n");
        return 0;
    }
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    static const struct cfg cfgs[] = {
        { "GLES 2.0", SDL_GL_CONTEXT_PROFILE_ES, 2, 0, "OpenGL ES", false },
        { "GLES 3.0", SDL_GL_CONTEXT_PROFILE_ES, 3, 0, "OpenGL ES", false },
        { "GL 3.3 core", SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, "Core Profile", false },
        { "GL 4.5 core", SDL_GL_CONTEXT_PROFILE_CORE, 4, 5, "Core Profile", true },
    };

    for (size_t i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); i++) {
        const struct cfg *c = &cfgs[i];
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, c->profile);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, c->major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, c->minor);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

        SDL_Window *w = SDL_CreateWindow(c->label, 256, 192, SDL_WINDOW_OPENGL);
        if (!w) {
            CHECK(!c->optional, "%s: window: %s", c->label, SDL_GetError());
            continue;
        }
        SDL_GLContext ctx = SDL_GL_CreateContext(w);
        if (!ctx) {
            if (c->optional) {
                printf("%s: unsupported (%s), skipped\n", c->label, SDL_GetError());
                skipped++;
            } else {
                CHECK(false, "%s: context: %s", c->label, SDL_GetError());
            }
            SDL_DestroyWindow(w);
            continue;
        }
        CHECK(SDL_GL_MakeCurrent(w, ctx), "%s: make current", c->label);

        const char *ver = (const char *)glGetString(GL_VERSION);
        printf("%s: GL_VERSION='%s' RENDERER='%s'\n", c->label,
               ver ? ver : "(null)", (const char *)glGetString(GL_RENDERER));
        /* Mesa llvmpipe provides at least the requested version and reports
         * the actual context version - check both floor and profile */
        CHECK(version_at_least(ver, c->major, c->minor),
              "%s: version '%s' is below requested %d.%d", c->label,
              ver ? ver : "(null)", c->major, c->minor);
        CHECK(c->must_contain[0] == '\0' || (ver && strstr(ver, c->must_contain)),
              "%s: version '%s' lacks '%s'", c->label, ver ? ver : "(null)", c->must_contain);

        /* attribute round-trip */
        int red = 0;
        CHECK(SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &red) && red >= 8,
              "%s: RED_SIZE %d", c->label, red);

        check_clear_swap(w, c->label);
        if (i > 0) {
            check_fbo(ctx, c->label);
        }

        SDL_GL_DestroyContext(ctx);
        SDL_DestroyWindow(w);
    }

    /* ---- context sharing ---- */
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);

    SDL_Window *wa = SDL_CreateWindow("share A", 64, 64, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx_a = wa ? SDL_GL_CreateContext(wa) : NULL;
    CHECK(ctx_a != NULL, "share: ctx A: %s", SDL_GetError());
    if (ctx_a) {
        SDL_GL_MakeCurrent(wa, ctx_a);
        /* red 8x8 texture in context A */
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        Uint32 red_px[64];
        for (int j = 0; j < 64; j++) {
            red_px[j] = 0xFF0000FF; /* RGBA8 red */
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, red_px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

        /* context B shares with A (A must be current) */
        SDL_Window *wb = SDL_CreateWindow("share B", 64, 64, SDL_WINDOW_OPENGL);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        SDL_GLContext ctx_b = wb ? SDL_GL_CreateContext(wb) : NULL;
        CHECK(ctx_b != NULL, "share: ctx B: %s", SDL_GetError());
        if (ctx_b) {
            CHECK(SDL_GL_MakeCurrent(wb, ctx_b), "share: make B current");
            /* is A's texture visible from B? attach it to an FBO and read */
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            CHECK(st == GL_FRAMEBUFFER_COMPLETE, "share: FBO incomplete (0x%x)", st);
            if (st == GL_FRAMEBUFFER_COMPLETE) {
                Uint8 px[4] = { 0 };
                glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                CHECK(px[0] > 250 && px[1] < 5 && px[2] < 5,
                      "share: B cannot read A's texture (%u %u %u)", px[0], px[1], px[2]);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            SDL_GL_DestroyContext(ctx_b);
        }
        glDeleteTextures(1, &tex);
        SDL_GL_DestroyContext(ctx_a);
        SDL_DestroyWindow(wa);
        if (wb) {
            SDL_DestroyWindow(wb);
        }
    }

    SDL_Quit();
    if (failures) {
        printf("test_gl_matrix: FAIL (%d checks, %d skipped)\n", failures, skipped);
        return 1;
    }
    printf("test_gl_matrix: PASS (%d configs skipped)\n", skipped);
    return 0;
}
