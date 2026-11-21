/*
  SDLop test: desktop OpenGL on X11 goes through GLX (as with stock SDL3).

  Applications built against stock SDL3 expect a GLX context on X11: GLEW's
  glewInit() calls glXGetCurrentDisplay() and fails with
  GLEW_ERROR_NO_GLX_DISPLAY on an EGL context. Checks:
    - core 3.3 and default (legacy 2.x) contexts are GLX: glXGetCurrentContext
      / glXGetCurrentDisplay / glXGetCurrentDrawable (looked up in libGL the
      way an application sees them) report our context and window, and the
      GLEW-style glXQueryVersion on that display works
    - SDL_EGL_* report "not EGL" for a GLX context
    - rendering: clear + glReadPixels on the window, SDL_GL_SwapWindow
    - swap interval round trip
    - shared contexts (SDL_GL_SHARE_WITH_CURRENT_CONTEXT)
    - OpenGL ES 2.0 / 3.0 contexts go through GLX too when the server has
      GLX_EXT_create_context_es_profile (else EGL); SDL_HINT_OPENGL_ES_DRIVER
      and SDL_HINT_VIDEO_FORCE_EGL select EGL; the requested ES version is
      honoured on GLX and EGL (ES 1.1 is ES-CM 1.1, ES 9.0 / 3.9 fail)
    - SDL_GL_CONTEXT_NO_ERROR / RESET_NOTIFICATION + ROBUST_ACCESS_FLAG /
      RELEASE_BEHAVIOR reach the GLX context (read back with glGetIntegerv)
      when the server has the matching GLX extensions
    - destroying the window while its GLX context is current is safe
    - GLX again after SDL_Quit / SDL_Init
    - framebuffer attributes are honoured like stock SDL3 / GLFW: the GLX
      config behind the context (glXQueryContext GLX_FBCONFIG_ID) has the
      requested alpha / depth / stencil sizes when the server offers such a
      config (RGB888 + depth 0 and RGBA8888 + depth 24 / stencil 8), also
      when the attributes change after the window was created, and
      SDL_GL_GetAttribute reports the realised sizes (glGetIntegerv)

  Skips when DISPLAY is unset or the X server / libGL has no GLX.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                          \
    do {                                                          \
        if (!(cond)) {                                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);  \
            fprintf(stderr, __VA_ARGS__);                         \
            fprintf(stderr, " [SDL: %s]\n", SDL_GetError());      \
            failures++;                                           \
        }                                                         \
    } while (0)

/* GL / GLX entry points, resolved at run time (no GL headers needed) */
typedef unsigned int GLenum_;
#define GL_COLOR_BUFFER_BIT_ 0x00004000
#define GL_RGBA_             0x1908
#define GL_UNSIGNED_BYTE_    0x1401
#define GL_VERSION_          0x1F02
#define GL_TEXTURE_2D_       0x0DE1
#define GL_CONTEXT_FLAGS_    0x821E
#define GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT_ 0x4
#define GL_CONTEXT_FLAG_NO_ERROR_BIT_      0x8
#define GL_RESET_NOTIFICATION_STRATEGY_    0x8256
#define GL_LOSE_CONTEXT_ON_RESET_          0x8252
#define GL_CONTEXT_RELEASE_BEHAVIOR_       0x82FB
typedef void (*PFN_glGetIntegerv)(GLenum_, int *);
typedef void (*PFN_glClearColor)(float, float, float, float);
typedef void (*PFN_glClear)(unsigned int);
typedef void (*PFN_glFinish)(void);
typedef void (*PFN_glReadPixels)(int, int, int, int, GLenum_, GLenum_, void *);
typedef const unsigned char *(*PFN_glGetString)(GLenum_);
typedef void (*PFN_glGenTextures)(int, unsigned int *);
typedef unsigned char (*PFN_glIsTexture)(unsigned int);
typedef void (*PFN_glBindTexture)(GLenum_, unsigned int);
typedef void (*PFN_glDeleteTextures)(int, const unsigned int *);

typedef void *(*PFN_glXGetCurrentContext)(void);
typedef void *(*PFN_glXGetCurrentDisplay)(void);
typedef unsigned long (*PFN_glXGetCurrentDrawable)(void);
typedef int (*PFN_glXQueryVersion)(void *, int *, int *);
typedef const char *(*PFN_glXQueryExtensionsString)(void *, int);
typedef int (*PFN_glXQueryContext)(void *, void *, int, int *);
typedef void **(*PFN_glXGetFBConfigs)(void *, int, int *);
typedef int (*PFN_glXGetFBConfigAttrib)(void *, void *, int, int *);
typedef int (*PFN_XFree)(void *);
#define GLX_ALPHA_SIZE_     11
#define GLX_DEPTH_SIZE_     12
#define GLX_STENCIL_SIZE_   13
#define GLX_RED_SIZE_       8
#define GLX_DOUBLEBUFFER_   5
#define GLX_VISUAL_ID_      0x800B
#define GLX_DRAWABLE_TYPE_  0x8010
#define GLX_RENDER_TYPE_    0x8011
#define GLX_FBCONFIG_ID_    0x8013
#define GLX_WINDOW_BIT_     0x1
#define GLX_RGBA_BIT_       0x1
#define GL_ALPHA_BITS_      0x0D55
#define GL_DEPTH_BITS_      0x0D56
#define GL_STENCIL_BITS_    0x0D57

static PFN_glXGetCurrentContext p_glXGetCurrentContext;
static PFN_glXGetCurrentDisplay p_glXGetCurrentDisplay;
static PFN_glXGetCurrentDrawable p_glXGetCurrentDrawable;
static PFN_glXQueryVersion p_glXQueryVersion;
static PFN_glXQueryExtensionsString p_glXQueryExtensionsString;
static PFN_glXQueryContext p_glXQueryContext;
static PFN_glXGetFBConfigs p_glXGetFBConfigs;
static PFN_glXGetFBConfigAttrib p_glXGetFBConfigAttrib;
static PFN_XFree p_XFree;
static char glx_extensions[8192]; /* server+client GLX extensions, read under the first context */

static bool load_glx(void)
{
    void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        lib = dlopen("libGLX.so.0", RTLD_NOW | RTLD_LOCAL);
    }
    if (!lib) {
        return false;
    }
    p_glXGetCurrentContext = (PFN_glXGetCurrentContext)dlsym(lib, "glXGetCurrentContext");
    p_glXGetCurrentDisplay = (PFN_glXGetCurrentDisplay)dlsym(lib, "glXGetCurrentDisplay");
    p_glXGetCurrentDrawable = (PFN_glXGetCurrentDrawable)dlsym(lib, "glXGetCurrentDrawable");
    p_glXQueryVersion = (PFN_glXQueryVersion)dlsym(lib, "glXQueryVersion");
    p_glXQueryExtensionsString = (PFN_glXQueryExtensionsString)dlsym(lib, "glXQueryExtensionsString");
    p_glXQueryContext = (PFN_glXQueryContext)dlsym(lib, "glXQueryContext");
    p_glXGetFBConfigs = (PFN_glXGetFBConfigs)dlsym(lib, "glXGetFBConfigs");
    p_glXGetFBConfigAttrib = (PFN_glXGetFBConfigAttrib)dlsym(lib, "glXGetFBConfigAttrib");
    p_XFree = (PFN_XFree)dlsym(RTLD_DEFAULT, "XFree");
    if (!p_XFree) {
        void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        p_XFree = x11 ? (PFN_XFree)dlsym(x11, "XFree") : NULL;
    }
    return p_glXGetCurrentContext && p_glXGetCurrentDisplay && p_glXGetCurrentDrawable && p_glXQueryVersion &&
           p_glXQueryExtensionsString;
}

static void start_video(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11"); /* SDL_Quit drops hints */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }
}

static unsigned long window_xid(SDL_Window *w)
{
    return (unsigned long)SDL_GetNumberProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
}

/* clear the window to a colour and read it back */
static void check_render(const char *label, SDL_Window *w)
{
    PFN_glClearColor glClearColor_ = (PFN_glClearColor)SDL_GL_GetProcAddress("glClearColor");
    PFN_glClear glClear_ = (PFN_glClear)SDL_GL_GetProcAddress("glClear");
    PFN_glFinish glFinish_ = (PFN_glFinish)SDL_GL_GetProcAddress("glFinish");
    PFN_glReadPixels glReadPixels_ = (PFN_glReadPixels)SDL_GL_GetProcAddress("glReadPixels");
    CHECK(glClearColor_ && glClear_ && glFinish_ && glReadPixels_, "%s: GL entry points", label);
    if (!glClearColor_ || !glClear_ || !glFinish_ || !glReadPixels_) {
        return;
    }
    glClearColor_(1.0f, 0.0f, 1.0f, 1.0f);
    glClear_(GL_COLOR_BUFFER_BIT_);
    glFinish_();
    unsigned char px[4] = { 0, 0, 0, 0 };
    glReadPixels_(4, 4, 1, 1, GL_RGBA_, GL_UNSIGNED_BYTE_, px);
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 255, "%s: readback %u,%u,%u", label, px[0], px[1], px[2]);
    CHECK(SDL_GL_SwapWindow(w), "%s: SDL_GL_SwapWindow", label);
}

static const char *gl_version(void)
{
    PFN_glGetString glGetString_ = (PFN_glGetString)SDL_GL_GetProcAddress("glGetString");
    const char *v = glGetString_ ? (const char *)glGetString_(GL_VERSION_) : NULL;
    return v ? v : "";
}

/* the checks GLEW's glewInit() / glxewInit() perform */
static void check_glx_current(const char *label, SDL_Window *w, SDL_GLContext ctx)
{
    (void)ctx;
    void *dpy = p_glXGetCurrentDisplay();
    CHECK(p_glXGetCurrentContext() != NULL, "%s: glXGetCurrentContext", label);
    CHECK(dpy != NULL, "%s: glXGetCurrentDisplay (GLEW_ERROR_NO_GLX_DISPLAY)", label);
    CHECK(p_glXGetCurrentDrawable() == window_xid(w), "%s: drawable 0x%lx != window 0x%lx", label,
          p_glXGetCurrentDrawable(), window_xid(w));
    int major = 0, minor = 0;
    CHECK(dpy && p_glXQueryVersion(dpy, &major, &minor) && (major > 1 || minor >= 3),
          "%s: glXQueryVersion on the current display (%d.%d)", label, major, minor);
    CHECK(SDL_EGL_GetCurrentDisplay() == NULL, "%s: SDL_EGL_GetCurrentDisplay must be NULL under GLX", label);
    CHECK(SDL_EGL_GetCurrentConfig() == NULL, "%s: SDL_EGL_GetCurrentConfig must be NULL under GLX", label);
    CHECK(SDL_EGL_GetWindowSurface(w) == NULL, "%s: SDL_EGL_GetWindowSurface must be NULL under GLX", label);
    SDL_ClearError();
}

static void reset_attrs(void)
{
    SDL_GL_ResetAttributes();
}

static bool glx_has(const char *ext)
{
    const size_t len = strlen(ext);
    for (const char *p = glx_extensions; (p = strstr(p, ext)) != NULL; p += len) {
        if ((p == glx_extensions || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
            return true;
        }
    }
    return false;
}

static int gl_int(GLenum_ pname)
{
    PFN_glGetIntegerv glGetIntegerv_ = (PFN_glGetIntegerv)SDL_GL_GetProcAddress("glGetIntegerv");
    int v = -12345;
    if (glGetIntegerv_) {
        glGetIntegerv_(pname, &v);
    }
    return v;
}

/* an OpenGL ES context: GLX when the server can, EGL otherwise */
static void check_es(int major, int minor, bool expect_glx, const char *label)
{
    reset_attrs();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_Window *w = SDL_CreateWindow("test_glx es", 32, 32, SDL_WINDOW_OPENGL);
    SDL_GLContext es = w ? SDL_GL_CreateContext(w) : NULL;
    CHECK(es != NULL, "%s: ES %d.%d context", label, major, minor);
    if (es) {
        const char *v = gl_version();
        printf("%s: GL_VERSION='%s' via %s\n", label, v, p_glXGetCurrentContext() ? "GLX" : "EGL");
        CHECK(strncmp(v, "OpenGL ES ", 10) == 0 && v[10] >= '0' + major, "%s: ES >= %d (got '%s')", label, major, v);
        if (expect_glx) {
            check_glx_current(label, w, es);
        } else {
            CHECK(p_glXGetCurrentContext() == NULL, "%s: not GLX", label);
            CHECK(SDL_EGL_GetCurrentDisplay() != NULL, "%s: EGL", label);
        }
        check_render(label, w);
        SDL_GL_DestroyContext(es);
    }
    if (w) {
        SDL_DestroyWindow(w);
    }
}

/* alpha / depth / stencil / visual of the GLX config behind the current
 * context, read from GLX itself */
typedef struct FBSizes
{
    int alpha, depth, stencil, visual;
} FBSizes;

static bool fb_api(void)
{
    return p_glXQueryContext && p_glXGetFBConfigs && p_glXGetFBConfigAttrib && p_XFree;
}

static bool current_fb_sizes(FBSizes *out)
{
    void *dpy = p_glXGetCurrentDisplay();
    void *ctx = p_glXGetCurrentContext();
    int id = 0, n = 0;
    if (!dpy || !ctx || p_glXQueryContext(dpy, ctx, GLX_FBCONFIG_ID_, &id) != 0) {
        return false;
    }
    void **configs = p_glXGetFBConfigs(dpy, 0, &n);
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        int cid = -1;
        if (p_glXGetFBConfigAttrib(dpy, configs[i], GLX_FBCONFIG_ID_, &cid) == 0 && cid == id) {
            p_glXGetFBConfigAttrib(dpy, configs[i], GLX_ALPHA_SIZE_, &out->alpha);
            p_glXGetFBConfigAttrib(dpy, configs[i], GLX_DEPTH_SIZE_, &out->depth);
            p_glXGetFBConfigAttrib(dpy, configs[i], GLX_STENCIL_SIZE_, &out->stencil);
            p_glXGetFBConfigAttrib(dpy, configs[i], GLX_VISUAL_ID_, &out->visual);
            found = true;
        }
    }
    if (configs) {
        p_XFree(configs);
    }
    return found;
}

/* whether the server has a double-buffered RGB(A)8 window config with
 * exactly these sizes (otherwise the driver's nearest one is fine) */
static bool server_has_config(int alpha, int depth, int stencil)
{
    void *dpy = p_glXGetCurrentDisplay();
    int n = 0;
    void **configs = dpy ? p_glXGetFBConfigs(dpy, 0, &n) : NULL;
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        int r = 0, a = -1, d = -1, st = -1, db = 0, dt = 0, rt = 0, vid = 0;
        p_glXGetFBConfigAttrib(dpy, configs[i], GLX_RED_SIZE_, &r);
        p_glXGetFBConfigAttrib(dpy, configs[i], GLX_ALPHA_SIZE_, &a);
        p_glXGetFBConfigAttrib(dpy, configs[i], GLX_DEPTH_SIZE_, &d);
        p_glXGetFBConfigAttrib(dpy, configs[i], GLX_STENCIL_SIZE_, &st);
        p_glXGetFBConfigAttrib(dpy, configs[i], GLX_DOUBLEBUFFER_, &db);
        p_glXGetFBConfigAttrib(dpy, configs[i], GLX_DRAWABLE_TYPE_, &dt);
        p_glXGetFBConfigAttrib(dpy, configs[i], GLX_RENDER_TYPE_, &rt);
        p_glXGetFBConfigAttrib(dpy, configs[i], GLX_VISUAL_ID_, &vid);
        found = r == 8 && a == alpha && d == depth && st == stencil && db && (dt & GLX_WINDOW_BIT_) &&
                (rt & GLX_RGBA_BIT_) && vid != 0;
    }
    if (configs) {
        p_XFree(configs);
    }
    return found;
}

static void request_fb(int alpha, int depth, int stencil)
{
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, alpha);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depth);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencil);
}

/* a context with the current attributes on w: the GLX config honours
 * alpha / depth / stencil (exactly, when the server has such a config), and
 * SDL_GL_GetAttribute reports what the config has */
static void check_fb(const char *label, SDL_Window *w, int alpha, int depth, int stencil, bool core)
{
    if (core) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(w);
    CHECK(ctx != NULL, "%s: context", label);
    if (!ctx) {
        return;
    }
    FBSizes fb = { -1, -1, -1, 0 };
    CHECK(p_glXGetCurrentContext() != NULL && current_fb_sizes(&fb), "%s: GLX config of the context", label);
    printf("%s: requested a%d d%d s%d -> GLX config a%d d%d s%d (visual 0x%x)\n", label, alpha, depth, stencil,
           fb.alpha, fb.depth, fb.stencil, (unsigned)fb.visual);
    if (server_has_config(alpha, depth, stencil)) {
        CHECK(fb.alpha == alpha && fb.depth == depth && fb.stencil == stencil,
              "%s: request a%d d%d s%d realised as a%d d%d s%d", label, alpha, depth, stencil, fb.alpha, fb.depth,
              fb.stencil);
    } else {
        /* Mesa (CI: Xvfb + llvmpipe) has every combination used here */
        const char *strict = getenv("SDLOP_TEST_STRICT");
        CHECK(!(strict && *strict == '1') || (alpha == 0 && depth == 16),
              "%s: server has no exact a%d d%d s%d config (SDLOP_TEST_STRICT)", label, alpha, depth, stencil);
        printf("%s: server has no exact a%d d%d s%d config\n", label, alpha, depth, stencil);
        CHECK(fb.alpha >= alpha && fb.depth >= depth && fb.stencil >= stencil, "%s: minimums", label);
    }
    int a = -1, d = -1, st = -1, r = -1, bs = -1;
    CHECK(SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a) && a == fb.alpha, "%s: SDL_GL_ALPHA_SIZE %d != %d", label, a,
          fb.alpha);
    CHECK(SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &d) && d == fb.depth, "%s: SDL_GL_DEPTH_SIZE %d != %d", label, d,
          fb.depth);
    CHECK(SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &st) && st == fb.stencil, "%s: SDL_GL_STENCIL_SIZE %d != %d",
          label, st, fb.stencil);
    CHECK(SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r) && r == 8, "%s: SDL_GL_RED_SIZE %d", label, r);
    CHECK(SDL_GL_GetAttribute(SDL_GL_BUFFER_SIZE, &bs) && bs == 24 + fb.alpha, "%s: SDL_GL_BUFFER_SIZE %d", label,
          bs);
    if (!core) {
        /* independent of SDL: the legacy *_BITS state of the context */
        CHECK(gl_int(GL_ALPHA_BITS_) == fb.alpha && gl_int(GL_DEPTH_BITS_) == fb.depth &&
                  gl_int(GL_STENCIL_BITS_) == fb.stencil,
              "%s: GL_*_BITS a%d d%d s%d", label, gl_int(GL_ALPHA_BITS_), gl_int(GL_DEPTH_BITS_),
              gl_int(GL_STENCIL_BITS_));
    }
    check_render(label, w);
    SDL_GL_DestroyContext(ctx);
}

static void check_fb_attributes(void)
{
    if (!fb_api()) {
        printf("fb: SKIP (no glXQueryContext / glXGetFBConfigs / XFree)\n");
        return;
    }
    /* RGB888, no depth / stencil (the GLFW-style 8880 + depth 0 request) */
    reset_attrs();
    request_fb(0, 0, 0);
    SDL_Window *w = SDL_CreateWindow("test_glx fb", 32, 32, SDL_WINDOW_OPENGL);
    CHECK(w != NULL, "fb: window");
    if (!w) {
        return;
    }
    check_fb("fb 8880/0/0 legacy", w, 0, 0, 0, false);
    request_fb(0, 0, 0);
    check_fb("fb 8880/0/0 core", w, 0, 0, 0, true);
    /* attributes changed after the window was created */
    request_fb(8, 24, 8);
    check_fb("fb 8888/24/8 on the 8880 window", w, 8, 24, 8, false);
    SDL_DestroyWindow(w);

    reset_attrs();
    request_fb(8, 24, 8);
    w = SDL_CreateWindow("test_glx fb", 32, 32, SDL_WINDOW_OPENGL);
    CHECK(w != NULL, "fb: window 2");
    if (w) {
        check_fb("fb 8888/24/8 legacy", w, 8, 24, 8, false);
        request_fb(8, 24, 8);
        check_fb("fb 8888/24/8 core", w, 8, 24, 8, true);
        request_fb(0, 16, 0);
        check_fb("fb 8880/16/0 on the 8888 window", w, 0, 16, 0, true);
        SDL_DestroyWindow(w);
    }

    /* defaults (depth 16 minimum): whatever the driver prefers, reported */
    reset_attrs();
    w = SDL_CreateWindow("test_glx fb", 32, 32, SDL_WINDOW_OPENGL);
    if (w) {
        SDL_GLContext ctx = SDL_GL_CreateContext(w);
        CHECK(ctx != NULL, "fb defaults: context");
        FBSizes fb = { -1, -1, -1, 0 };
        if (ctx && current_fb_sizes(&fb)) {
            int d = -1;
            printf("fb defaults: GLX config a%d d%d s%d\n", fb.alpha, fb.depth, fb.stencil);
            CHECK(fb.depth >= 16, "fb defaults: depth %d < 16", fb.depth);
            CHECK(SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &d) && d == fb.depth, "fb defaults: reported depth %d", d);
        }
        if (ctx) {
            SDL_GL_DestroyContext(ctx);
        }
        SDL_DestroyWindow(w);
    }
    /* no context current: the request is reported */
    reset_attrs();
    request_fb(0, 0, 0);
    int d = -1;
    CHECK(SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &d) && d == 0, "fb: request without a context (%d)", d);
    reset_attrs();
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_glx: SKIP (DISPLAY not set)\n");
        return 0;
    }
    if (!load_glx()) {
        printf("test_glx: SKIP (no libGL with GLX)\n");
        return 0;
    }
    /* the backend choice is what is tested: environment overrides would
     * turn every GLX expectation into a failure */
    if (SDL_getenv("SDL_VIDEO_FORCE_EGL") || SDL_getenv("SDL_OPENGL_ES_DRIVER")) {
        printf("test_glx: SKIP (SDL_VIDEO_FORCE_EGL / SDL_OPENGL_ES_DRIVER set in the environment)\n");
        return 0;
    }
    start_video();

    SDL_Window *w = SDL_CreateWindow("test_glx", 64, 48, SDL_WINDOW_OPENGL);
    CHECK(w != NULL, "SDL_CreateWindow");
    if (!w) {
        return 1;
    }

    /* 1. core profile 3.3 (Nuklear / Dear ImGui examples) */
    reset_attrs();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GLContext core = SDL_GL_CreateContext(w);
    if (!core && strstr(SDL_GetError(), "GLX")) {
        printf("test_glx: SKIP (%s)\n", SDL_GetError());
        SDL_DestroyWindow(w);
        SDL_Quit();
        return 0;
    }
    CHECK(core != NULL, "core 3.3 context");
    if (core) {
        CHECK(SDL_GL_MakeCurrent(w, core), "MakeCurrent core");
        check_glx_current("core", w, core);
        const char *e = p_glXQueryExtensionsString(p_glXGetCurrentDisplay(), 0);
        snprintf(glx_extensions, sizeof(glx_extensions), "%s", e ? e : "");
        const char *v = gl_version();
        printf("core: GL_VERSION='%s'\n", v);
        CHECK(strstr(v, "OpenGL ES") == NULL && v[0] >= '3', "core: desktop GL >= 3 (got '%s')", v);
        check_render("core", w);

        /* swap interval */
        /* swap interval: like stock, unsupported without a GLX swap
         * control extension (Xvfb/drisw has none); then nothing changes */
        int iv = -5;
        if (SDL_GL_SetSwapInterval(1)) {
            CHECK(SDL_GL_GetSwapInterval(&iv) && iv == 1, "GetSwapInterval == 1 (%d)", iv);
            CHECK(SDL_GL_SetSwapInterval(0), "SetSwapInterval(0)");
            CHECK(SDL_GL_GetSwapInterval(&iv) && iv == 0, "GetSwapInterval == 0 (%d)", iv);
        } else {
            printf("core: swap control unsupported here (%s)\n", SDL_GetError());
            CHECK(SDL_GL_GetSwapInterval(&iv) && iv == 0, "interval unchanged after failure (%d)", iv);
        }

        /* a GL function pointer from SDL is the same one libGL hands out */
        CHECK(SDL_GL_GetProcAddress("glXGetCurrentDisplay") != NULL, "glX* via SDL_GL_GetProcAddress");
        CHECK(SDL_GL_ExtensionSupported("GL_ARB_vertex_array_object"), "GL extension query on the GLX context");

        /* 2. shared context sees the first context's texture name */
        PFN_glGenTextures glGenTextures_ = (PFN_glGenTextures)SDL_GL_GetProcAddress("glGenTextures");
        PFN_glBindTexture glBindTexture_ = (PFN_glBindTexture)SDL_GL_GetProcAddress("glBindTexture");
        PFN_glIsTexture glIsTexture_ = (PFN_glIsTexture)SDL_GL_GetProcAddress("glIsTexture");
        PFN_glDeleteTextures glDeleteTextures_ = (PFN_glDeleteTextures)SDL_GL_GetProcAddress("glDeleteTextures");
        unsigned int tex = 0;
        glGenTextures_(1, &tex);
        glBindTexture_(GL_TEXTURE_2D_, tex);
        PFN_glFinish glFinish_ = (PFN_glFinish)SDL_GL_GetProcAddress("glFinish");
        glFinish_();
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        SDL_GLContext shared = SDL_GL_CreateContext(w);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
        CHECK(shared != NULL, "shared context");
        if (shared) {
            CHECK(SDL_GL_MakeCurrent(w, shared), "MakeCurrent shared");
            CHECK(p_glXGetCurrentContext() != NULL, "shared is GLX");
            CHECK(glIsTexture_(tex), "texture %u visible in the shared context", tex);
            CHECK(SDL_GL_MakeCurrent(w, core), "back to core");
            CHECK(SDL_GL_DestroyContext(shared), "destroy shared");
        }
        glDeleteTextures_(1, &tex);

        CHECK(SDL_GL_MakeCurrent(w, NULL), "release");
        CHECK(p_glXGetCurrentContext() == NULL, "nothing current after release");
        CHECK(SDL_GL_DestroyContext(core), "destroy core");
    }

    /* 3. default attributes: legacy desktop context, still GLX */
    reset_attrs();
    SDL_GLContext legacy = SDL_GL_CreateContext(w);
    CHECK(legacy != NULL, "default context");
    if (legacy) {
        check_glx_current("default", w, legacy);
        printf("default: GL_VERSION='%s'\n", gl_version());
        CHECK(strstr(gl_version(), "OpenGL ES") == NULL, "default context is desktop GL");
        check_render("default", w);
        SDL_GL_DestroyContext(legacy);
    }

    /* 4. OpenGL ES: GLX with GLX_EXT_create_context_es_profile, else EGL */
    const bool glx_es = glx_has("GLX_EXT_create_context_es_profile") && glx_has("GLX_ARB_create_context_profile");
    const bool glx_es2 = glx_es || (glx_has("GLX_EXT_create_context_es2_profile") && glx_has("GLX_ARB_create_context_profile"));
    printf("GLX ES profiles: es_profile=%d es2_profile=%d\n", glx_es, glx_es2);
    check_es(2, 0, glx_es2, "es2");
    check_es(3, 0, glx_es, "es3");
    /* stock's switch back to EGL for ES */
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    check_es(2, 0, false, "es2 + SDL_OPENGL_ES_DRIVER");
    SDL_ResetHint(SDL_HINT_OPENGL_ES_DRIVER);

    /* 4b. context attributes through GLX_ARB_create_context_{no_error,robustness}
     *     and GLX_ARB_context_flush_control */
    reset_attrs();
    int got = -1;
    CHECK(SDL_GL_GetAttribute(SDL_GL_CONTEXT_RELEASE_BEHAVIOR, &got) && got == SDL_GL_CONTEXT_RELEASE_BEHAVIOR_FLUSH,
          "default release behavior FLUSH (%d)", got);
    CHECK(SDL_GL_GetAttribute(SDL_GL_CONTEXT_RESET_NOTIFICATION, &got) && got == SDL_GL_CONTEXT_RESET_NO_NOTIFICATION,
          "default reset notification (%d)", got);
    CHECK(SDL_GL_GetAttribute(SDL_GL_CONTEXT_NO_ERROR, &got) && got == 0, "default no_error (%d)", got);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_NO_ERROR, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_RELEASE_BEHAVIOR, SDL_GL_CONTEXT_RELEASE_BEHAVIOR_NONE);
    CHECK(SDL_GL_GetAttribute(SDL_GL_CONTEXT_NO_ERROR, &got) && got == 1, "no_error round trip (%d)", got);
    SDL_GLContext noerr = SDL_GL_CreateContext(w);
    CHECK(noerr != NULL, "no_error + release-none context");
    if (noerr) {
        check_glx_current("no_error", w, noerr);
        if (glx_has("GLX_ARB_create_context_no_error")) {
            CHECK(gl_int(GL_CONTEXT_FLAGS_) & GL_CONTEXT_FLAG_NO_ERROR_BIT_, "GL_CONTEXT_FLAG_NO_ERROR_BIT (0x%x)",
                  gl_int(GL_CONTEXT_FLAGS_));
        }
        if (glx_has("GLX_ARB_context_flush_control")) {
            CHECK(gl_int(GL_CONTEXT_RELEASE_BEHAVIOR_) == 0, "GL_CONTEXT_RELEASE_BEHAVIOR NONE (0x%x)",
                  gl_int(GL_CONTEXT_RELEASE_BEHAVIOR_));
        }
        check_render("no_error", w);
        SDL_GL_DestroyContext(noerr);
    }
    reset_attrs();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_ROBUST_ACCESS_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_RESET_NOTIFICATION, SDL_GL_CONTEXT_RESET_LOSE_CONTEXT);
    SDL_GLContext robust = SDL_GL_CreateContext(w);
    CHECK(robust != NULL, "robust context");
    if (robust) {
        check_glx_current("robust", w, robust);
        if (glx_has("GLX_ARB_create_context_robustness")) {
            CHECK(gl_int(GL_CONTEXT_FLAGS_) & GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT_, "GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT (0x%x)",
                  gl_int(GL_CONTEXT_FLAGS_));
            CHECK(gl_int(GL_RESET_NOTIFICATION_STRATEGY_) == GL_LOSE_CONTEXT_ON_RESET_,
                  "GL_RESET_NOTIFICATION_STRATEGY lose-context (0x%x)", gl_int(GL_RESET_NOTIFICATION_STRATEGY_));
        }
        check_render("robust", w);
        SDL_GL_DestroyContext(robust);
    }
    if (glx_es) {
        /* the same attributes on an ES context over GLX */
        reset_attrs();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_NO_ERROR, 1);
        SDL_GLContext es_noerr = SDL_GL_CreateContext(w);
        CHECK(es_noerr != NULL, "ES 3.0 no_error context");
        if (es_noerr) {
            check_glx_current("es no_error", w, es_noerr);
            if (glx_has("GLX_ARB_create_context_no_error")) {
                CHECK(gl_int(GL_CONTEXT_FLAGS_) & GL_CONTEXT_FLAG_NO_ERROR_BIT_, "ES: GL_CONTEXT_FLAG_NO_ERROR_BIT (0x%x)",
                      gl_int(GL_CONTEXT_FLAGS_));
            }
            check_render("es no_error", w);
            SDL_GL_DestroyContext(es_noerr);
        }
    }

    /* 5. SDL_HINT_VIDEO_FORCE_EGL, set from code */
    reset_attrs();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    SDL_Window *w_egl = SDL_CreateWindow("test_glx force-egl", 32, 32, SDL_WINDOW_OPENGL);
    SDL_GLContext egl = w_egl ? SDL_GL_CreateContext(w_egl) : NULL;
    CHECK(egl != NULL, "forced-EGL context");
    if (egl) {
        CHECK(p_glXGetCurrentContext() == NULL, "forced EGL: no GLX context");
        CHECK(SDL_EGL_GetCurrentDisplay() != NULL, "forced EGL: SDL_EGL_GetCurrentDisplay");
        check_render("force-egl", w_egl);
        SDL_GL_DestroyContext(egl);
    }
    if (w_egl) {
        SDL_DestroyWindow(w_egl);
    }
    SDL_ResetHint(SDL_HINT_VIDEO_FORCE_EGL);

    /* 5b. the requested ES version is honoured on both paths, like stock:
     *     ES 1.x is ES-CM 1.x (if the driver has it at all), an impossible
     *     version fails instead of quietly returning ES 3.2 */
    for (int forced = 0; forced < 2; forced++) {
        const char *path = forced ? "EGL" : "default";
        if (forced) {
            SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
        }
        reset_attrs();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GLContext es1 = SDL_GL_CreateContext(w);
        if (es1) {
            CHECK(strncmp(gl_version(), "OpenGL ES-CM 1.", 15) == 0, "%s: ES 1.1 request gave '%s'", path, gl_version());
            SDL_GL_DestroyContext(es1);
        } else {
            printf("%s: no ES 1.1 here (%s)\n", path, SDL_GetError());
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 9);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GLContext es9 = SDL_GL_CreateContext(w);
        CHECK(es9 == NULL, "%s: ES 9.0 must fail (got '%s')", path, es9 ? gl_version() : "");
        if (es9) {
            SDL_GL_DestroyContext(es9);
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 9);
        SDL_GLContext es39 = SDL_GL_CreateContext(w);
        CHECK(es39 == NULL, "%s: ES 3.9 must fail (got '%s')", path, es39 ? gl_version() : "");
        if (es39) {
            SDL_GL_DestroyContext(es39);
        }
    }
    SDL_ResetHint(SDL_HINT_VIDEO_FORCE_EGL);
    SDL_ClearError();

    /* 6. destroy the window while its GLX context is current */
    reset_attrs();
    SDL_GLContext dangling = SDL_GL_CreateContext(w);
    CHECK(dangling != NULL, "context for the destroy test");
    CHECK(p_glXGetCurrentDrawable() == window_xid(w), "current on w before destroy");
    SDL_DestroyWindow(w);
    CHECK(p_glXGetCurrentDrawable() == 0, "drawable released with the window");
    if (dangling) {
        SDL_GL_DestroyContext(dangling);
    }

    /* 7. GLX works again after a full SDL_Quit / SDL_Init cycle */
    SDL_Quit();
    start_video();
    w = SDL_CreateWindow("test_glx again", 32, 32, SDL_WINDOW_OPENGL);
    SDL_GLContext again = w ? SDL_GL_CreateContext(w) : NULL;
    CHECK(again != NULL, "context after re-init");
    if (again) {
        check_glx_current("re-init", w, again);
        check_render("re-init", w);
        SDL_GL_DestroyContext(again);
    }
    if (w) {
        SDL_DestroyWindow(w);
    }

    /* 8. framebuffer attributes */
    check_fb_attributes();
    SDL_Quit();

    if (failures) {
        printf("test_glx: FAIL (%d)\n", failures);
        return 1;
    }
    printf("test_glx: PASS\n");
    return 0;
}
