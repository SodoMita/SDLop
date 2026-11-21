/*
  test_egl_api - SDL_EGL_* and SDL_GL_ExtensionSupported on a real EGL
  stack (llvmpipe): attribute callbacks are called and honored, the
  current display/config/surface are the ones EGL itself reports.

  Usage: test_egl_api [x11|wayland]  (skips without a display server)
*/
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EGL_NONE_ 0x3038
#define EGL_RENDER_BUFFER_ 0x3086
#define EGL_BACK_BUFFER_ 0x3084
#define EGL_CONFIG_ID_ 0x3028
#define EGL_RENDER_BUFFER_QUERY EGL_RENDER_BUFFER_
#define GL_EXTENSIONS_ 0x1F03

static int failures = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            ++failures;                                               \
        }                                                             \
    } while (0)

static int platform_calls, surface_calls, context_calls;
static void *seen_display;

static SDL_EGLAttrib *SDLCALL platform_cb(void *userdata)
{
    CHECK(userdata == (void *)&platform_calls, "platform userdata");
    ++platform_calls;
    SDL_EGLAttrib *a = (SDL_EGLAttrib *)SDL_malloc(sizeof(SDL_EGLAttrib));
    a[0] = EGL_NONE_; /* nothing to add; must still be accepted */
    return a;
}

static SDL_EGLint *SDLCALL surface_cb(void *userdata, SDL_EGLDisplay display, SDL_EGLConfig config)
{
    (void)userdata;
    ++surface_calls;
    CHECK(display != NULL && config != NULL, "surface callback gets display + config");
    seen_display = display;
    SDL_EGLint *a = (SDL_EGLint *)SDL_malloc(3 * sizeof(SDL_EGLint));
    a[0] = EGL_RENDER_BUFFER_;
    a[1] = EGL_BACK_BUFFER_;
    a[2] = EGL_NONE_;
    return a;
}

static SDL_EGLint *SDLCALL context_cb(void *userdata, SDL_EGLDisplay display, SDL_EGLConfig config)
{
    (void)userdata;
    ++context_calls;
    CHECK(display != NULL && config != NULL, "context callback gets display + config");
    SDL_EGLint *a = (SDL_EGLint *)SDL_malloc(sizeof(SDL_EGLint));
    a[0] = EGL_NONE_;
    return a;
}

typedef void *(*PFN_eglGetCurrentDisplay)(void);
typedef void *(*PFN_eglGetCurrentSurface)(int);
typedef unsigned int (*PFN_eglQuerySurface)(void *, void *, int, int *);
typedef unsigned int (*PFN_eglGetConfigAttrib)(void *, void *, int, int *);
typedef const unsigned char *(*PFN_glGetString)(unsigned int);

int main(int argc, char **argv)
{
    const char *driver = argc > 1 ? argv[1] : "x11";
    const char *need = strcmp(driver, "wayland") == 0 ? "WAYLAND_DISPLAY" : "DISPLAY";
    if (!getenv(need)) {
        printf("test_egl_api: SKIP (%s not set)\n", need);
        return 0;
    }
    SDL_setenv_unsafe("SDL_VIDEODRIVER", driver, 1);
    /* X11 uses GLX for desktop GL and, with GLX_EXT_create_context_es_profile,
     * for ES too (like stock): SDL_EGL_* are NULL there. Ask for EGL, as a
     * stock SDL3 application that uses SDL_EGL_* on X11 has to. */
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");

    /* before SDL_Init: everything reports an error, nothing crashes */
    CHECK(SDL_EGL_GetCurrentDisplay() == NULL, "no display before init");
    CHECK(SDL_EGL_GetProcAddress("eglGetCurrentDisplay") == NULL, "no proc before init");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init(%s): %s\n", driver, SDL_GetError());
        return 1;
    }
    CHECK(SDL_EGL_GetCurrentDisplay() == NULL, "no display before a GL context");
    CHECK(SDL_EGL_GetCurrentConfig() == NULL, "no config before a GL context");
    CHECK(!SDL_GL_ExtensionSupported("GL_OES_rgb8_rgba8"), "no extensions without a context");

    SDL_EGL_SetAttributeCallbacks(platform_cb, surface_cb, context_cb, &platform_calls);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_Window *w = SDL_CreateWindow("egl api", 160, 120, SDL_WINDOW_OPENGL);
    CHECK(w, "window: %s", SDL_GetError());
    SDL_GLContext ctx = w ? SDL_GL_CreateContext(w) : NULL;
    if (!ctx) {
        fprintf(stderr, "test_egl_api: GL context failed (%s) - no EGL stack?\n", SDL_GetError());
        SDL_Quit();
        return failures ? 1 : 0;
    }
    printf("test_egl_api: driver %s\n", SDL_GetCurrentVideoDriver());
    CHECK(platform_calls <= 1, "platform callback called %d times", platform_calls);
    CHECK(context_calls == 1, "context callback called %d times", context_calls);
    CHECK(surface_calls == 1, "surface callback called %d times", surface_calls);

    SDL_EGLDisplay dpy = SDL_EGL_GetCurrentDisplay();
    SDL_EGLConfig cfg = SDL_EGL_GetCurrentConfig();
    SDL_EGLSurface surf = SDL_EGL_GetWindowSurface(w);
    CHECK(dpy && cfg && surf, "display %p config %p surface %p (%s)", dpy, cfg, surf, SDL_GetError());
    CHECK(dpy == seen_display, "callbacks saw the same display");

    PFN_eglGetCurrentDisplay get_dpy = (PFN_eglGetCurrentDisplay)SDL_EGL_GetProcAddress("eglGetCurrentDisplay");
    PFN_eglGetCurrentSurface get_surf = (PFN_eglGetCurrentSurface)SDL_EGL_GetProcAddress("eglGetCurrentSurface");
    PFN_eglQuerySurface query_surf = (PFN_eglQuerySurface)SDL_EGL_GetProcAddress("eglQuerySurface");
    CHECK(get_dpy && get_surf && query_surf, "EGL entry points resolve");
    if (get_dpy && get_surf && query_surf) {
        CHECK(get_dpy() == dpy, "current display matches EGL");
        CHECK(get_surf(0x3059 /* EGL_DRAW */) == surf, "window surface matches EGL");
        int rb = 0;
        CHECK(query_surf(dpy, surf, EGL_RENDER_BUFFER_QUERY, &rb) && rb == EGL_BACK_BUFFER_,
              "surface callback attribute applied (render buffer 0x%x)", rb);
    }

    /* GL_ExtensionSupported against the real extension string */
    PFN_glGetString gl_get_string = (PFN_glGetString)SDL_GL_GetProcAddress("glGetString");
    const char *exts = gl_get_string ? (const char *)gl_get_string(GL_EXTENSIONS_) : NULL;
    CHECK(exts && *exts, "GL_EXTENSIONS string");
    if (exts && *exts) {
        char first[128] = { 0 }, last[128] = { 0 };
        const char *sp = strchr(exts, ' ');
        size_t n = sp ? (size_t)(sp - exts) : strlen(exts);
        if (n < sizeof(first)) {
            memcpy(first, exts, n);
        }
        const char *end = exts + strlen(exts);
        while (end > exts && end[-1] == ' ') {
            --end;
        }
        const char *ls = end;
        while (ls > exts && ls[-1] != ' ') {
            --ls;
        }
        if ((size_t)(end - ls) < sizeof(last)) {
            memcpy(last, ls, (size_t)(end - ls));
        }
        CHECK(SDL_GL_ExtensionSupported(first), "first extension %s supported", first);
        CHECK(SDL_GL_ExtensionSupported(last), "last extension %s supported", last);
        char prefix[128];
        SDL_strlcpy(prefix, first, sizeof(prefix));
        if (strlen(prefix) > 4) {
            prefix[strlen(prefix) - 1] = '\0'; /* a prefix is not a match */
            CHECK(!SDL_GL_ExtensionSupported(prefix), "prefix %s rejected", prefix);
        }
        SDL_SetHint(first, "0");
        CHECK(!SDL_GL_ExtensionSupported(first), "hint override disables %s", first);
        SDL_ResetHint(first);
    }
    CHECK(!SDL_GL_ExtensionSupported("GL_SDLOP_no_such_extension"), "unknown extension");
    CHECK(!SDL_GL_ExtensionSupported("GL_A GL_B"), "names with spaces rejected");
    CHECK(!SDL_GL_ExtensionSupported(""), "empty name rejected");

    SDL_EGL_SetAttributeCallbacks(NULL, NULL, NULL, NULL);
    SDL_GL_ResetAttributes();
    SDL_GL_DestroyContext(ctx);

    /* desktop GL 3.3 core: the glGetStringi path */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    ctx = SDL_GL_CreateContext(w);
    if (ctx) {
        typedef const unsigned char *(*PFN_glGetStringi)(unsigned int, unsigned int);
        PFN_glGetStringi get_stringi = (PFN_glGetStringi)SDL_GL_GetProcAddress("glGetStringi");
        const char *e0 = get_stringi ? (const char *)get_stringi(GL_EXTENSIONS_, 0) : NULL;
        CHECK(e0 && SDL_GL_ExtensionSupported(e0), "core profile extension %s", e0 ? e0 : "(none)");
        CHECK(!SDL_GL_ExtensionSupported("GL_SDLOP_no_such_extension"), "core: unknown extension");
        SDL_GL_DestroyContext(ctx);
    } else {
        printf("test_egl_api: no GL 3.3 core context (%s), skipping that part\n", SDL_GetError());
    }
    SDL_GL_ResetAttributes();
    SDL_DestroyWindow(w);
    SDL_Quit();
    if (failures) {
        printf("test_egl_api: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_egl_api: OK\n");
    return 0;
}
