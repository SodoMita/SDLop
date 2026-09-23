/*
  SDLop test: OpenGL ES 2 context via EGL (llvmpipe on headless systems).
  Skips cleanly when no Wayland compositor is running.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);         \
            fprintf(stderr, "\n");                \
            failures++;                           \
        }                                         \
    } while (0)

int main(void)
{
    int failures = 0;
    if (!getenv("WAYLAND_DISPLAY")) {
        printf("test_gl: SKIP (WAYLAND_DISPLAY not set)\n");
        return 0;
    }
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);

    SDL_Window *w = SDL_CreateWindow("gl test", 320, 240, SDL_WINDOW_OPENGL);
    if (!w) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(w);
    CHECK(ctx != NULL, "GL context: %s", SDL_GetError());
    if (!ctx) {
        SDL_DestroyWindow(w);
        SDL_Quit();
        return 1;
    }
    CHECK(SDL_GL_GetCurrentContext() == ctx, "current context");
    CHECK(SDL_GL_GetCurrentWindow() == w, "current window");

    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    CHECK(ver && strlen(ver) > 0, "GL_VERSION string");
    CHECK(renderer && strlen(renderer) > 0, "GL_RENDERER string");
    printf("GL_VERSION: %s\nGL_RENDERER: %s\n", ver, renderer);

    /* proc address lookup */
    CHECK(SDL_GL_GetProcAddress("glClear") != NULL, "glClear proc");
    CHECK(SDL_GL_GetProcAddress("glDrawArrays") != NULL, "glDrawArrays proc");

    /* swap interval */
    CHECK(SDL_GL_SetSwapInterval(0), "swap interval 0");
    int interval = -1;
    CHECK(SDL_GL_GetSwapInterval(&interval) && interval == 0, "get swap interval");

    /* clear green and read back */
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    CHECK(SDL_GL_SwapWindow(w), "swap: %s", SDL_GetError());

    Uint8 px[4] = { 0 };
    glReadPixels(160, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px[1] > 250 && px[0] < 5 && px[2] < 5, "center pixel is green (%u %u %u)", px[0], px[1], px[2]);

    /* a few more frames must keep working */
    for (int i = 0; i < 5; i++) {
        glClear(GL_COLOR_BUFFER_BIT);
        CHECK(SDL_GL_SwapWindow(w), "swap %d", i);
    }

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(w);
    SDL_Quit();

    if (failures) {
        printf("test_gl: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_gl: PASS\n");
    return 0;
}
