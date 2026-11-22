/*
  SDLop test: the WGL driver's context and swap-interval handling.

  This is the Windows counterpart of test_glx.c; there was no WGL test at all.

  Swap interval is checked against the *driver*, not only against SDL.
  SDL_GL_GetSwapInterval can answer from a cached value, and an implementation
  that only ever asks itself will agree with itself -- a test written that way
  proves nothing. wglGetSwapIntervalEXT reports the actual GL state, so that is
  what is asserted when it is available.

  WGL keeps the interval per context, so it is also checked across a context
  switch: setting it on one window's context and moving the current context
  elsewhere and back is exactly what an app with two GL windows does.

  Skips cleanly when no GL implementation is available (no wglCreateContext
  result), because a machine without one is not a broken SDLop.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdio.h>

static int failures = 0;
static int skipped = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            failures++;                                          \
        }                                                        \
    } while (0)

#define SKIP(...)                    \
    do {                             \
        skipped++;                   \
        printf("skipped: " __VA_ARGS__); \
        printf("\n");                \
        return;                      \
    } while (0)

typedef unsigned int GLenum;
extern const unsigned char *glGetString(GLenum name);
#define GL_VERSION 0x1F02
#define GL_VENDOR  0x1F00
#define GL_RENDERER 0x1F01

static void test_context_and_swap_interval(SDL_Window *window)
{
    SDL_GLContext context;
    const char *version;
    int interval;

    context = SDL_GL_CreateContext(window);
    if (!context) {
        /* Not a failure: a machine with no GL implementation is a valid
         * configuration (headless CI, a driver without WGL). */
        SKIP("no GL context available: %s", SDL_GetError());
    }

    version = (const char *)glGetString(GL_VERSION);
    printf("test_win32_gl: %s / %s / GL %s\n",
           (const char *)glGetString(GL_VENDOR),
           (const char *)glGetString(GL_RENDERER),
           version ? version : "(null)");
    /* glGetString going through the current context is itself the check that
     * SDL_GL_MakeCurrent actually worked: with no current context it returns
     * NULL, so a broken MakeCurrent shows up here rather than as a blank screen
     * much later. */
    CHECK(version != NULL, "glGetString(GL_VERSION) returned NULL with a context "
                           "made current, so WGL_MakeCurrent did not take effect");

    /* --- swap interval round trip ------------------------------------- */
    if (SDL_GL_SetSwapInterval(0)) {
        CHECK(SDL_GL_GetSwapInterval(&interval), "GetSwapInterval failed after a "
                                                 "successful SetSwapInterval");
        CHECK(interval == 0, "SetSwapInterval(0) then GetSwapInterval gave %d", interval);

        if (SDL_GL_SetSwapInterval(1)) {
            CHECK(SDL_GL_GetSwapInterval(&interval), "GetSwapInterval failed");
            CHECK(interval == 1, "SetSwapInterval(1) then GetSwapInterval gave %d "
                                 "-- the interval is not being read back from the "
                                 "context that is actually current", interval);
        } else {
            printf("test_win32_gl: driver refused interval 1 (no wglSwapIntervalEXT)\n");
        }
    } else {
        printf("test_win32_gl: driver refused interval 0 (no wglSwapIntervalEXT)\n");
    }

    /* Swap must not fail: it is the one call every frame makes. */
    CHECK(SDL_GL_SwapWindow(window), "SDL_GL_SwapWindow reported failure");

    /* --- the interval must survive a context switch ---------------------
     * Ask the driver directly: SDL's own answer can be a cached value that no
     * longer reflects the GL context. */
    {
        SDL_Window *other;
        SDL_GLContext other_context;
        int (*driver_interval)(void) = NULL;

        other = SDL_CreateWindow("sdlop gl second", 160, 120, SDL_WINDOW_OPENGL);
        other_context = other ? SDL_GL_CreateContext(other) : NULL;
        if (other_context) {
            driver_interval = (int (*)(void))SDL_GL_GetProcAddress("wglGetSwapIntervalEXT");
            if (driver_interval) {
                if (SDL_GL_MakeCurrent(window, context)) {
                    SDL_GL_SetSwapInterval(1);
                    CHECK(driver_interval() == 1, "the driver reports interval %d after "
                                                  "SetSwapInterval(1)", driver_interval());
                    if (SDL_GL_MakeCurrent(other, other_context)) {
                        /* Switch away and back; the first context must still have it. */
                        if (SDL_GL_MakeCurrent(window, context)) {
                            CHECK(driver_interval() == 1,
                                  "after switching to another context and back, the "
                                  "driver reports interval %d for the original context, "
                                  "want 1 -- the interval is being lost on switch",
                                  driver_interval());
                        }
                    }
                }
            } else {
                printf("test_win32_gl: wglGetSwapIntervalEXT unavailable; "
                       "interval verified through SDL only\n");
            }
            SDL_GL_DestroyContext(other_context);
        } else if (other) {
            printf("test_win32_gl: second window got no context: %s\n", SDL_GetError());
        }
        if (other) {
            SDL_DestroyWindow(other);
        }
        /* Restore so the rest of the test starts from a known state. */
        SDL_GL_MakeCurrent(window, context);
    }

    /* --- two contexts on one window ------------------------------------ */
    {
        SDL_GLContext second = SDL_GL_CreateContext(window);
        if (second) {
            CHECK(SDL_GL_MakeCurrent(window, second), "MakeCurrent with a second "
                                                      "context failed");
            CHECK(glGetString(GL_VERSION) != NULL, "second context is not current");
            CHECK(SDL_GL_MakeCurrent(window, context), "switching back to the first "
                                                       "context failed");
            CHECK(glGetString(GL_VERSION) != NULL, "first context did not come back");
            SDL_GL_DestroyContext(second);
        } else {
            printf("test_win32_gl: a second context was refused: %s\n", SDL_GetError());
        }
    }

    SDL_GL_DestroyContext(context);
    /* Must tolerate being asked for a context after the only one is gone. */
    CHECK(!SDL_GL_MakeCurrent(window, NULL) || SDL_GL_GetCurrentWindow() == window,
          "releasing the current context left the window in an inconsistent state");
}

/* An OpenGL ES request must either produce a real ES context or fail.
 *
 * It must never silently produce a desktop context: an ES app would then run
 * against GL 4.x and only discover the mismatch when its ES-only shaders or
 * missing entry points fail, with nothing in SDL_GetError() to explain it.
 *
 * Measured before the fix: requesting SDL_GL_CONTEXT_PROFILE_ES with major 2
 * returned "GL 4.5 (Compatibility Profile)" and reported success. Two separate
 * defects produced that: the legacy wglCreateContext branch was taken because
 * the attribs path tested only "major > 2 || core", and when the profile
 * context genuinely could not be created the driver fell back to a desktop
 * context anyway.
 *
 * Both outcomes are accepted here; the silent downgrade is not. */
static void test_es_profile_is_honest(void)
{
    SDL_Window *window;
    SDL_GLContext context;
    const char *version;

    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    window = SDL_CreateWindow("sdlop gl es", 160, 120, SDL_WINDOW_OPENGL);
    if (!window) {
        printf("test_win32_gl: ES section skipped, no window: %s\n", SDL_GetError());
        SDL_GL_ResetAttributes();
        return;
    }
    context = SDL_GL_CreateContext(window);
    if (!context) {
        /* Correct: the driver has no ES2 profile bit, and saying so is better
         * than pretending. This is what llvmpipe does. */
        const char *error = SDL_GetError();
        printf("test_win32_gl: ES context refused (expected on a driver without "
               "WGL_EXT_create_context_es2_profile): %s\n",
               (error && *error) ? error : "(no error text)");
        CHECK(error != NULL && *error != '\0',
              "SDL_GL_CreateContext failed for an ES profile but left no error "
              "message, so the caller cannot tell why");
    } else {
        version = (const char *)glGetString(GL_VERSION);
        CHECK(version != NULL, "ES context created but GL_VERSION is unreadable");
        CHECK(version && SDL_strncmp(version, "OpenGL ES", 9) == 0,
              "an OpenGL ES profile was requested and a context was returned, but "
              "GL_VERSION is \"%s\" -- a desktop context was substituted instead of "
              "reporting that ES is unavailable",
              version ? version : "(null)");
        SDL_GL_DestroyContext(context);
    }
    SDL_DestroyWindow(window);
    SDL_GL_ResetAttributes();
}

static void test_two_windows(void)
{
    SDL_Window *a;
    SDL_Window *b;
    SDL_GLContext ca;
    SDL_GLContext cb;

    a = SDL_CreateWindow("sdlop gl a", 160, 120, SDL_WINDOW_OPENGL);
    b = SDL_CreateWindow("sdlop gl b", 160, 120, SDL_WINDOW_OPENGL);
    if (!a || !b) {
        if (a) { SDL_DestroyWindow(a); }
        if (b) { SDL_DestroyWindow(b); }
        SKIP("could not create two GL windows: %s", SDL_GetError());
    }
    ca = SDL_GL_CreateContext(a);
    cb = SDL_GL_CreateContext(b);
    if (!ca || !cb) {
        if (ca) { SDL_GL_DestroyContext(ca); }
        if (cb) { SDL_GL_DestroyContext(cb); }
        SDL_DestroyWindow(a);
        SDL_DestroyWindow(b);
        SKIP("could not create two contexts: %s", SDL_GetError());
    }
    /* Moving the current context between windows is where a driver that keys
     * state off the wrong HWND breaks. */
    CHECK(SDL_GL_MakeCurrent(a, ca), "MakeCurrent(a) failed");
    CHECK(SDL_GL_GetCurrentWindow() == a, "the current window is not the one that was made current");
    CHECK(SDL_GL_MakeCurrent(b, cb), "MakeCurrent(b) failed");
    CHECK(SDL_GL_GetCurrentWindow() == b, "switching current to b did not take");
    CHECK(SDL_GL_SwapWindow(b), "SwapWindow on the second window failed");
    SDL_GL_DestroyContext(cb);
    SDL_GL_DestroyContext(ca);
    SDL_DestroyWindow(b);
    SDL_DestroyWindow(a);
}

int main(void)
{
    SDL_Window *window;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 gl", 320, 240, SDL_WINDOW_OPENGL);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow(OPENGL) failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_ShowWindow(window);

    test_context_and_swap_interval(window);
    test_two_windows();
    test_es_profile_is_honest();

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_gl: %d failure(s)\n", failures);
        return 1;
    }
    if (skipped) {
        printf("test_win32_gl: %d section(s) skipped, nothing verified\n", skipped);
        return 77; /* ctest SKIP_RETURN_CODE */
    }
    printf("test_win32_gl: context, swap interval round trip, and context switching\n");
    return 0;
}
