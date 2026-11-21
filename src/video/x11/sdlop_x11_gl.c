/*
  SDLop - X11 OpenGL support: GLX and EGL.

  Backend per context, as stock SDL3's X11 driver chooses it:
    - GLX (libGL.so.1, else libGLX.so.0) for desktop OpenGL. This is what
      applications built against stock SDL3 expect: GLEW and other loaders
      call glXGetCurrentDisplay()/glXGetProcAddress() and fail on an EGL
      context ("GLEW_ERROR_NO_GLX_DISPLAY").
    - GLX also for OpenGL ES when the server has
      GLX_EXT_create_context_es_profile (any ES version) or the older
      GLX_EXT_create_context_es2_profile (ES 2.0 only), with
      GLX_ARB_create_context(_profile): the ES bit goes into
      GLX_CONTEXT_PROFILE_MASK_ARB. No EGL detour on drivers whose GLX
      path is the fast one. ES requests the driver refuses fall back to
      EGL, as do ES 1.x on es2-only servers.
    - EGL when SDL_HINT_VIDEO_FORCE_EGL ("SDL_VIDEO_FORCE_EGL") is true,
      for ES when SDL_HINT_OPENGL_ES_DRIVER ("SDL_OPENGL_ES_DRIVER") is
      true (both env or SDL_SetHint, as in stock), and as a fallback when
      GLX is unusable (no libGL, no GLX extension, no framebuffer config
      for SDLop's window visual).

  GLX context attributes, each sent only when the extension is present:
  GLX_ARB_create_context_robustness (SDL_GL_CONTEXT_ROBUST_ACCESS_FLAG,
  SDL_GL_CONTEXT_RESET_NOTIFICATION), GLX_ARB_create_context_no_error
  (SDL_GL_CONTEXT_NO_ERROR), GLX_ARB_context_flush_control
  (SDL_GL_CONTEXT_RELEASE_BEHAVIOR). Swap control: EXT (+ _tear for
  adaptive vsync, interval -1), MESA, SGI.

  GLX runs on a private Xlib connection (libX11 dlopen-ed on demand): X
  window ids are server-global, so it renders into SDLop's xcb windows.

  EGL display selection, best first:
    1. EGL_PLATFORM_XCB_EXT on SDLop's own xcb connection (Mesa >= 21)
    2. EGL_PLATFORM_X11_EXT on the private Xlib connection
    3. eglGetDisplay(Display *)
  SDLOP_X11_EGL_PLATFORM=x11 skips (1), which is how (2) is tested.

  Without a GPU Mesa routes all of this to llvmpipe.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"
#include "../sdlop_egl_dyn.h"
#include "../sdlop_gl_offload.h"

#include <dlfcn.h>
#include <string.h>

#ifndef EGL_PLATFORM_X11_EXT
#define EGL_PLATFORM_X11_EXT 0x31D5
#endif
#ifndef EGL_PLATFORM_X11_SCREEN_EXT
#define EGL_PLATFORM_X11_SCREEN_EXT 0x31D6
#endif
#ifndef EGL_PLATFORM_XCB_EXT
#define EGL_PLATFORM_XCB_EXT 0x31DC
#endif
#ifndef EGL_PLATFORM_XCB_SCREEN_EXT
#define EGL_PLATFORM_XCB_SCREEN_EXT 0x31DE
#endif
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

typedef enum X11GLBackend
{
    X11GL_BACKEND_EGL = 1,
    X11GL_BACKEND_GLX
} X11GLBackend;

typedef struct X11GLContext
{
    X11GLBackend backend;
    /* EGL */
    EGLContext ctx;
    EGLenum api; /* EGL_OPENGL_API or EGL_OPENGL_ES_API */
    /* GLX */
    void *glx_ctx; /* GLXContext */
} X11GLContext;

static bool current_is_glx(void);

/* private Xlib connection: GLX, and the EGL_PLATFORM_X11 fallback */
typedef struct _XDisplay XDisplay_;
typedef XDisplay_ *(*PFN_XOpenDisplay_)(const char *);
typedef int (*PFN_XCloseDisplay_)(XDisplay_ *);
static void *xlib_handle;
static XDisplay_ *xlib_display;
static PFN_XCloseDisplay_ p_XCloseDisplay;

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static bool egl_initialized;
static bool egl_loaded;
static int swap_interval = 0;

static bool has_extension(const char *list, const char *ext)
{
    if (!list) {
        return false;
    }
    size_t n = strlen(ext);
    for (const char *p = list; (p = strstr(p, ext)) != NULL; p += n) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) {
            return true;
        }
    }
    return false;
}

static XDisplay_ *open_private_xlib_display(void)
{
    if (xlib_display) {
        return xlib_display;
    }
    if (!xlib_handle) {
        const char *name = SDL_GetHint("SDLOP_LIB_X11");
        xlib_handle = dlopen(name && *name ? name : "libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!xlib_handle) {
            SDL_SetError("OpenGL on X11 needs libX11 (GLX, or EGL without EGL_EXT_platform_xcb): %s", dlerror());
            return NULL;
        }
    }
    PFN_XOpenDisplay_ open_display = (PFN_XOpenDisplay_)dlsym(xlib_handle, "XOpenDisplay");
    p_XCloseDisplay = (PFN_XCloseDisplay_)dlsym(xlib_handle, "XCloseDisplay");
    if (!open_display || !p_XCloseDisplay) {
        SDL_SetError("libX11 lacks XOpenDisplay/XCloseDisplay");
        return NULL;
    }
    xlib_display = open_display(NULL);
    if (!xlib_display) {
        SDL_SetError("XOpenDisplay failed for the OpenGL connection");
    }
    return xlib_display;
}

static bool ensure_egl_display(void)
{
    if (egl_initialized) {
        return true;
    }
    if (!egl_loaded) {
        if (!SDLOP_EGL_LoadSymbols()) {
            return false;
        }
        egl_loaded = true;
    }
    xcb_connection_t *conn = SDLOP_X11_GetConnection();
    if (!conn) {
        return SDL_SetError("X11 connection not available");
    }
    const char *client_ext = SDLOP_EGL_eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)SDLOP_EGL_eglGetProcAddress("eglGetPlatformDisplayEXT");
    const char *force = SDL_GetHint("SDLOP_X11_EGL_PLATFORM");
    bool allow_xcb = !(force && strcmp(force, "x11") == 0);
    const char *platform = "?";

    if (get_platform_display && allow_xcb && has_extension(client_ext, "EGL_EXT_platform_xcb")) {
        EGLint attribs[] = { EGL_PLATFORM_XCB_SCREEN_EXT, SDLOP_X11_GetScreenNumber(), EGL_NONE };
        egl_display = SDLOP_EGL_PlatformDisplay(EGL_PLATFORM_XCB_EXT, conn, attribs);
        platform = "xcb";
    }
    if (egl_display == EGL_NO_DISPLAY) {
        XDisplay_ *dpy = open_private_xlib_display();
        if (!dpy) {
            return false;
        }
        if (get_platform_display && (has_extension(client_ext, "EGL_EXT_platform_x11") ||
                                     has_extension(client_ext, "EGL_KHR_platform_x11"))) {
            EGLint attribs[] = { EGL_PLATFORM_X11_SCREEN_EXT, SDLOP_X11_GetScreenNumber(), EGL_NONE };
            egl_display = SDLOP_EGL_PlatformDisplay(EGL_PLATFORM_X11_EXT, dpy, attribs);
            platform = "x11";
        }
        if (egl_display == EGL_NO_DISPLAY) {
            egl_display = SDLOP_EGL_eglGetDisplay((EGLNativeDisplayType)dpy);
            platform = "x11 (legacy)";
        }
    }
    if (egl_display == EGL_NO_DISPLAY) {
        return SDL_SetError("eglGetDisplay failed for X11");
    }
    EGLint major = 0, minor = 0;
    if (!SDLOP_EGL_eglInitialize(egl_display, &major, &minor)) {
        egl_display = EGL_NO_DISPLAY;
        return SDL_SetError("eglInitialize failed: 0x%x", SDLOP_EGL_eglGetError());
    }
    egl_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop EGL %d.%d on X11, platform %s (%s)", major, minor, platform,
                SDLOP_EGL_eglQueryString(egl_display, EGL_VENDOR));
    return true;
}

static bool pick_config(EGLConfig *out, EGLenum api)
{
    EGLint renderable;
    if (api == EGL_OPENGL_API) {
        renderable = EGL_OPENGL_BIT;
    } else {
        renderable = SDLOP_EGL_ESRenderableType(egl_display);
    }
    EGLint attribs[24];
    int n = 0;
    attribs[n++] = EGL_SURFACE_TYPE;    attribs[n++] = EGL_WINDOW_BIT;
    attribs[n++] = EGL_RENDERABLE_TYPE; attribs[n++] = renderable;
    attribs[n++] = EGL_RED_SIZE;        attribs[n++] = sdlop_glattrs.red_size;
    attribs[n++] = EGL_GREEN_SIZE;      attribs[n++] = sdlop_glattrs.green_size;
    attribs[n++] = EGL_BLUE_SIZE;       attribs[n++] = sdlop_glattrs.blue_size;
    attribs[n++] = EGL_ALPHA_SIZE;      attribs[n++] = sdlop_glattrs.alpha_size;
    attribs[n++] = EGL_DEPTH_SIZE;      attribs[n++] = sdlop_glattrs.depth_size;
    attribs[n++] = EGL_STENCIL_SIZE;    attribs[n++] = sdlop_glattrs.stencil_size;
    if (sdlop_glattrs.multisamplesamples > 0) {
        attribs[n++] = EGL_SAMPLE_BUFFERS; attribs[n++] = 1;
        attribs[n++] = EGL_SAMPLES;        attribs[n++] = sdlop_glattrs.multisamplesamples;
    }
    attribs[n++] = EGL_NONE;

    EGLConfig configs[64];
    EGLint num = 0;
    if (!SDLOP_EGL_eglChooseConfig(egl_display, attribs, configs, 64, &num) || num == 0) {
        return SDL_SetError("eglChooseConfig found no matching config");
    }
    /* windows use the root visual: a config with a different native
     * visual would fail eglCreateWindowSurface with EGL_BAD_MATCH */
    xcb_visualid_t visual = SDLOP_X11_GetVisual();
    for (EGLint i = 0; i < num; i++) {
        EGLint vid = 0;
        if (SDLOP_EGL_eglGetConfigAttrib(egl_display, configs[i], EGL_NATIVE_VISUAL_ID, &vid) &&
            (xcb_visualid_t)vid == visual) {
            *out = configs[i];
            return true;
        }
    }
    *out = configs[0];
    return true;
}

static void *egl_CreateContext(void)
{
    if (!ensure_egl_display()) {
        return NULL;
    }
    bool want_desktop_gl =
        (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_CORE) != 0 ||
        ((sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_ES) == 0 && sdlop_glattrs.major_version >= 3);
    EGLenum api = want_desktop_gl ? EGL_OPENGL_API : EGL_OPENGL_ES_API;
    if (!SDLOP_EGL_eglBindAPI(api)) {
        SDL_SetError("eglBindAPI failed: 0x%x", SDLOP_EGL_eglGetError());
        return NULL;
    }
    EGLConfig config;
    if (!pick_config(&config, api)) {
        return NULL;
    }
    EGLint ctx_attribs[64];
    int n = 0;
    if (api == EGL_OPENGL_API) {
        ctx_attribs[n++] = EGL_CONTEXT_MAJOR_VERSION_KHR; ctx_attribs[n++] = sdlop_glattrs.major_version;
        ctx_attribs[n++] = EGL_CONTEXT_MINOR_VERSION_KHR; ctx_attribs[n++] = sdlop_glattrs.minor_version;
        ctx_attribs[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
        ctx_attribs[n++] = (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_COMPATIBILITY)
                               ? EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR
                               : EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
        if (sdlop_glattrs.flags & SDL_GL_CONTEXT_DEBUG_FLAG) {
            ctx_attribs[n++] = EGL_CONTEXT_OPENGL_DEBUG; ctx_attribs[n++] = EGL_TRUE;
        }
        if (sdlop_glattrs.flags & SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG) {
            ctx_attribs[n++] = EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE; ctx_attribs[n++] = EGL_TRUE;
        }
    } else {
        n = SDLOP_EGL_AppendESVersion(egl_display, ctx_attribs, n);
        if (n < 0) {
            return NULL;
        }
    }
    n = SDLOP_EGL_AppendUserAttribs(true, egl_display, config, ctx_attribs, n, (int)SDL_arraysize(ctx_attribs));
    if (n < 0) {
        return NULL;
    }

    EGLContext share = EGL_NO_CONTEXT;
    if (sdlop_glattrs.share_with_current_context) {
        SDL_GLContext cur = SDLOP_GL_CurrentContext();
        if (cur && ((X11GLContext *)cur)->backend == X11GL_BACKEND_EGL) {
            share = ((X11GLContext *)cur)->ctx;
        }
    }
    EGLContext ctx = SDLOP_EGL_eglCreateContext(egl_display, config, share, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) {
        SDL_SetError("eglCreateContext failed: 0x%x", SDLOP_EGL_eglGetError());
        return NULL;
    }
    X11GLContext *glctx = (X11GLContext *)calloc(1, sizeof(*glctx));
    if (!glctx) {
        SDLOP_EGL_eglDestroyContext(egl_display, ctx);
        SDL_OutOfMemory();
        return NULL;
    }
    glctx->backend = X11GL_BACKEND_EGL;
    glctx->ctx = ctx;
    glctx->api = api;
    return glctx;
}

static bool config_for_context(X11GLContext *glctx, EGLConfig *out)
{
    EGLint id = 0;
    if (SDLOP_EGL_eglQueryContext(egl_display, glctx->ctx, EGL_CONFIG_ID, &id)) {
        EGLint attribs[3] = { EGL_CONFIG_ID, id, EGL_NONE };
        EGLint num = 0;
        if (SDLOP_EGL_eglChooseConfig(egl_display, attribs, out, 1, &num) && num == 1) {
            return true;
        }
    }
    return pick_config(out, glctx->api);
}

static bool egl_MakeCurrent(SDL_Window *window, X11GLContext *context)
{
    if (!ensure_egl_display()) {
        return false;
    }
    if (!context) {
        SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (window) {
            SDLOP_X11_SetGLActive(window, false);
        }
        return true;
    }
    X11GLContext *glctx = (X11GLContext *)context;
    if (!SDLOP_EGL_eglBindAPI(glctx->api)) {
        return SDL_SetError("eglBindAPI failed: 0x%x", SDLOP_EGL_eglGetError());
    }
    EGLSurface surface = EGL_NO_SURFACE;
    if (window) {
        void **slot = SDLOP_X11_GetGLSurfaceSlot(window);
        xcb_window_t xid = SDLOP_X11_GetWindowXID(window);
        if (!slot || xid == XCB_WINDOW_NONE) {
            return SDL_SetError("Invalid window for GL");
        }
        if (!*slot) {
            EGLConfig config;
            if (!config_for_context(glctx, &config)) {
                return false;
            }
            /* legacy entry point takes the XID by value on both platforms */
            EGLint surf_attribs[64];
            if (SDLOP_EGL_AppendUserAttribs(false, egl_display, config, surf_attribs, 0,
                                            (int)SDL_arraysize(surf_attribs)) < 0) {
                return false;
            }
            *slot = SDLOP_EGL_eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)xid, surf_attribs);
            if (*slot == EGL_NO_SURFACE) {
                *slot = NULL;
                return SDL_SetError("eglCreateWindowSurface failed: 0x%x", SDLOP_EGL_eglGetError());
            }
        }
        surface = (EGLSurface)*slot;
        SDLOP_X11_SetGLActive(window, true);
    }
    if (!SDLOP_EGL_eglMakeCurrent(egl_display, surface, surface, glctx->ctx)) {
        return SDL_SetError("eglMakeCurrent failed: 0x%x", SDLOP_EGL_eglGetError());
    }
    if (surface != EGL_NO_SURFACE) {
        SDLOP_EGL_eglSwapInterval(egl_display, swap_interval);
    }
    return true;
}

/* SDL_EGL_GetCurrentDisplay / GetCurrentConfig / GetWindowSurface */
void *SDLOP_X11_EGL_GetDisplay(SDLop_VideoDevice *device)
{
    (void)device;
    if (current_is_glx()) {
        return NULL; /* SDL_EGL_* report "not EGL" for GLX contexts */
    }
    return egl_initialized ? (void *)egl_display : NULL;
}

void *SDLOP_X11_EGL_GetConfig(SDLop_VideoDevice *device)
{
    (void)device;
    X11GLContext *cur = (X11GLContext *)SDLOP_GL_CurrentContext();
    EGLConfig config;
    if (!egl_initialized || !cur || cur->backend != X11GL_BACKEND_EGL || !config_for_context(cur, &config)) {
        return NULL;
    }
    return (void *)config;
}

void *SDLOP_X11_EGL_GetWindowSurface(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (current_is_glx()) {
        return NULL;
    }
    void **slot = SDLOP_X11_GetGLSurfaceSlot(window);
    return slot ? *slot : NULL;
}

static bool egl_SwapBuffers(SDL_Window *window)
{
    void **slot = SDLOP_X11_GetGLSurfaceSlot(window);
    if (!slot || !*slot) {
        return SDL_SetError("Window has no GL surface");
    }
    /* Mesa's X11 platform tracks drawable size itself: no resize hook */
    if (!SDLOP_EGL_eglSwapBuffers(egl_display, (EGLSurface)*slot)) {
        return SDL_SetError("eglSwapBuffers failed: 0x%x", SDLOP_EGL_eglGetError());
    }
    return true;
}

static void egl_DeleteContext(X11GLContext *glctx)
{
    if (!glctx) {
        return;
    }
    if (egl_initialized) {
        if (SDLOP_EGL_eglGetCurrentContext() == glctx->ctx) {
            SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        SDLOP_EGL_eglDestroyContext(egl_display, glctx->ctx);
    }
    free(glctx);
}

static void egl_WindowDestroyed(SDL_Window *window)
{
    void **slot = SDLOP_X11_GetGLSurfaceSlot(window);
    if (slot && *slot) {
        if (egl_initialized) {
            /* EGL forbids destroying a current surface out from under us */
            SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            SDLOP_EGL_eglDestroySurface(egl_display, (EGLSurface)*slot);
        }
        *slot = NULL;
    }
    SDLOP_X11_SetGLActive(window, false);
}

static SDL_FunctionPointer egl_GetProcAddress(const char *proc)
{
    if (!egl_loaded) {
        if (!SDLOP_EGL_LoadSymbols()) {
            return NULL;
        }
        egl_loaded = true;
    }
    SDL_FunctionPointer fn = SDLOP_EGL_eglGetProcAddress(proc);
    if (fn) {
        return fn;
    }
    return (SDL_FunctionPointer)dlsym(RTLD_DEFAULT, proc);
}

static bool egl_SetSwapInterval(int interval)
{
    if (interval < 0) {
        return SDL_SetError("Adaptive vsync (-1) is not supported");
    }
    swap_interval = interval;
    if (egl_initialized) {
        SDLOP_EGL_eglSwapInterval(egl_display, interval);
    }
    return true;
}

/* driver shutdown: terminate EGL and drop our libEGL reference so a later
 * SDL_Init starts from scratch */
static void egl_Quit(void)
{
    if (egl_initialized) {
        SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        SDLOP_EGL_eglTerminate(egl_display);
    }
    egl_display = EGL_NO_DISPLAY;
    egl_initialized = false;
    if (egl_loaded) {
        SDLOP_EGL_UnloadSymbols();
        egl_loaded = false;
    }
}

/* ------------------------------------------------------------------ */
/* GLX                                                                 */
/* ------------------------------------------------------------------ */

/* Only the handful of GLX/Xlib entry points SDLop needs, declared here so
 * the build needs no GL headers (everything is dlopen-ed). */
typedef unsigned long GLXDrawable_;
typedef void *GLXContext_;
typedef void *GLXFBConfig_;
typedef void (*GLXProc_)(void);

#define GLX_BUFFER_SIZE_                     2
#define GLX_DOUBLEBUFFER_                    5
#define GLX_STEREO_                          6
#define GLX_RED_SIZE_                        8
#define GLX_GREEN_SIZE_                      9
#define GLX_BLUE_SIZE_                       10
#define GLX_ALPHA_SIZE_                      11
#define GLX_DEPTH_SIZE_                      12
#define GLX_STENCIL_SIZE_                    13
#define GLX_ACCUM_RED_SIZE_                  14
#define GLX_ACCUM_GREEN_SIZE_                15
#define GLX_ACCUM_BLUE_SIZE_                 16
#define GLX_ACCUM_ALPHA_SIZE_                17
#define GLX_X_VISUAL_TYPE_                   0x22
#define GLX_TRUE_COLOR_                      0x8002
#define GLX_DRAWABLE_TYPE_                   0x8010
#define GLX_RENDER_TYPE_                     0x8011
#define GLX_X_RENDERABLE_                    0x8012
#define GLX_VISUAL_ID_                       0x800B
#define GLX_WINDOW_BIT_                      0x0001
#define GLX_RGBA_BIT_                        0x0001
#define GLX_RGBA_FLOAT_BIT_ARB_              0x0004
#define GLX_RGBA_TYPE_                       0x8014
#define GLX_SAMPLE_BUFFERS_                  100000
#define GLX_SAMPLES_                         100001
#define GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB_    0x20B2
#define GLX_CONTEXT_MAJOR_VERSION_ARB_       0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB_       0x2092
#define GLX_CONTEXT_FLAGS_ARB_               0x2094
#define GLX_CONTEXT_PROFILE_MASK_ARB_        0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB_    0x0001
#define GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB_ 0x0002
#define GLX_CONTEXT_ES_PROFILE_BIT_EXT_      0x0004 /* == SDL_GL_CONTEXT_PROFILE_ES */
#define GLX_CONTEXT_RELEASE_BEHAVIOR_ARB_    0x2097
#define GLX_CONTEXT_RELEASE_BEHAVIOR_NONE_ARB_ 0
#define GLX_CONTEXT_RELEASE_BEHAVIOR_FLUSH_ARB_ 0x2098
#define GLX_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB_ 0x8256
#define GLX_NO_RESET_NOTIFICATION_ARB_       0x8261
#define GLX_LOSE_CONTEXT_ON_RESET_ARB_       0x8252
#define GLX_CONTEXT_OPENGL_NO_ERROR_ARB_     0x31B3

typedef struct
{
    int type;
    XDisplay_ *display;
    unsigned long resourceid;
    unsigned long serial;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
} XErrorEvent_;
typedef int (*XErrorHandler_)(XDisplay_ *, XErrorEvent_ *);

static struct
{
    void *lib;          /* libGL / libGLX: never dlclose-d (TLS, atexit) */
    bool tried;         /* load attempted (result cached for the session) */
    bool usable;        /* display has GLX >= 1.3 */
    XDisplay_ *dpy;     /* == xlib_display while GLX is up */
    int screen;
    bool has_create_context, has_create_context_profile;
    bool has_swap_ext, has_swap_tear, has_swap_mesa, has_swap_sgi;
    bool has_srgb, has_float;
    bool has_es_profile, has_es2_profile;
    bool has_robustness, has_no_error, has_flush_control;

    int (*QueryExtension)(XDisplay_ *, int *, int *);
    int (*QueryVersion)(XDisplay_ *, int *, int *);
    const char *(*QueryExtensionsString)(XDisplay_ *, int);
    GLXFBConfig_ *(*ChooseFBConfig)(XDisplay_ *, int, const int *, int *);
    int (*GetFBConfigAttrib)(XDisplay_ *, GLXFBConfig_, int, int *);
    GLXContext_ (*CreateNewContext)(XDisplay_ *, GLXFBConfig_, int, GLXContext_, int);
    GLXContext_ (*CreateContextAttribsARB)(XDisplay_ *, GLXFBConfig_, GLXContext_, int, const int *);
    void (*DestroyContext)(XDisplay_ *, GLXContext_);
    int (*MakeCurrent)(XDisplay_ *, GLXDrawable_, GLXContext_);
    void (*SwapBuffers)(XDisplay_ *, GLXDrawable_);
    GLXContext_ (*GetCurrentContext)(void);
    GLXDrawable_ (*GetCurrentDrawable)(void);
    GLXProc_ (*GetProcAddress)(const unsigned char *);
    void (*SwapIntervalEXT)(XDisplay_ *, GLXDrawable_, int);
    int (*SwapIntervalMESA)(unsigned int);
    int (*SwapIntervalSGI)(int);

    /* libX11 */
    XErrorHandler_ (*XSetErrorHandler)(XErrorHandler_);
    int (*XSync)(XDisplay_ *, int);
    int (*XFree)(void *);
} glx;

static int glx_error_code;

static int glx_trap_handler(XDisplay_ *dpy, XErrorEvent_ *ev)
{
    (void)dpy;
    if (!glx_error_code) {
        glx_error_code = ev->error_code ? ev->error_code : -1;
    }
    return 0;
}

/* Xlib's default error handler exit()s the process: GLX requests that can
 * legitimately fail (context version not supported, ...) run trapped. */
static XErrorHandler_ glx_trap_begin(void)
{
    glx.XSync(glx.dpy, 0);
    glx_error_code = 0;
    return glx.XSetErrorHandler(glx_trap_handler);
}

static int glx_trap_end(XErrorHandler_ old)
{
    glx.XSync(glx.dpy, 0);
    glx.XSetErrorHandler(old);
    return glx_error_code;
}

static bool force_egl(void)
{
    return SDL_GetHintBoolean(SDL_HINT_VIDEO_FORCE_EGL, false);
}

static void *glx_sym(const char *name)
{
    void *p = dlsym(glx.lib, name);
    if (!p && glx.GetProcAddress) {
        p = (void *)glx.GetProcAddress((const unsigned char *)name);
    }
    return p;
}

/* Load libGL, open the private display, query GLX. The outcome is cached:
 * a system without GLX keeps using EGL without retrying every context. */
static bool glx_init(void)
{
    if (glx.usable) {
        return true;
    }
    if (glx.tried) {
        return SDL_SetError("GLX is not available");
    }
    glx.tried = true;

    if (!glx.lib) {
        const char *name = SDL_GetHint(SDL_HINT_OPENGL_LIBRARY);
        if (name && *name) {
            glx.lib = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
        } else {
            glx.lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
            if (!glx.lib) {
                glx.lib = dlopen("libGLX.so.0", RTLD_NOW | RTLD_GLOBAL);
            }
        }
        if (!glx.lib) {
            return SDL_SetError("Couldn't load libGL for GLX: %s", dlerror());
        }
    }
#define GLX_LOAD(field, name)                                         \
    do {                                                              \
        *(void **)&glx.field = dlsym(glx.lib, name);                  \
        if (!glx.field) {                                             \
            return SDL_SetError("libGL lacks %s", name);              \
        }                                                             \
    } while (0)
    GLX_LOAD(QueryExtension, "glXQueryExtension");
    GLX_LOAD(QueryVersion, "glXQueryVersion");
    GLX_LOAD(QueryExtensionsString, "glXQueryExtensionsString");
    GLX_LOAD(ChooseFBConfig, "glXChooseFBConfig");
    GLX_LOAD(GetFBConfigAttrib, "glXGetFBConfigAttrib");
    GLX_LOAD(CreateNewContext, "glXCreateNewContext");
    GLX_LOAD(DestroyContext, "glXDestroyContext");
    GLX_LOAD(MakeCurrent, "glXMakeCurrent");
    GLX_LOAD(SwapBuffers, "glXSwapBuffers");
    GLX_LOAD(GetCurrentContext, "glXGetCurrentContext");
    GLX_LOAD(GetCurrentDrawable, "glXGetCurrentDrawable");
    *(void **)&glx.GetProcAddress = dlsym(glx.lib, "glXGetProcAddressARB");
    if (!glx.GetProcAddress) {
        GLX_LOAD(GetProcAddress, "glXGetProcAddress");
    }
#undef GLX_LOAD

    XDisplay_ *dpy = open_private_xlib_display();
    if (!dpy) {
        return false;
    }
    *(void **)&glx.XSetErrorHandler = dlsym(xlib_handle, "XSetErrorHandler");
    *(void **)&glx.XSync = dlsym(xlib_handle, "XSync");
    *(void **)&glx.XFree = dlsym(xlib_handle, "XFree");
    if (!glx.XSetErrorHandler || !glx.XSync || !glx.XFree) {
        return SDL_SetError("libX11 lacks XSetErrorHandler/XSync/XFree");
    }
    int ev = 0, err = 0, major = 0, minor = 0;
    if (!glx.QueryExtension(dpy, &err, &ev) || !glx.QueryVersion(dpy, &major, &minor)) {
        return SDL_SetError("X server has no GLX extension");
    }
    if (major < 1 || (major == 1 && minor < 3)) {
        return SDL_SetError("GLX %d.%d is too old (1.3 needed)", major, minor);
    }
    glx.dpy = dpy;
    glx.screen = SDLOP_X11_GetScreenNumber();
    const char *ext = glx.QueryExtensionsString(dpy, glx.screen);
    if (has_extension(ext, "GLX_ARB_create_context")) {
        *(void **)&glx.CreateContextAttribsARB = glx_sym("glXCreateContextAttribsARB");
        glx.has_create_context = glx.CreateContextAttribsARB != NULL;
        glx.has_create_context_profile = has_extension(ext, "GLX_ARB_create_context_profile");
    }
    if (has_extension(ext, "GLX_EXT_swap_control")) {
        *(void **)&glx.SwapIntervalEXT = glx_sym("glXSwapIntervalEXT");
        glx.has_swap_ext = glx.SwapIntervalEXT != NULL;
        glx.has_swap_tear = glx.has_swap_ext && has_extension(ext, "GLX_EXT_swap_control_tear");
    }
    if (has_extension(ext, "GLX_MESA_swap_control")) {
        *(void **)&glx.SwapIntervalMESA = glx_sym("glXSwapIntervalMESA");
        glx.has_swap_mesa = glx.SwapIntervalMESA != NULL;
    }
    if (has_extension(ext, "GLX_SGI_swap_control")) {
        *(void **)&glx.SwapIntervalSGI = glx_sym("glXSwapIntervalSGI");
        glx.has_swap_sgi = glx.SwapIntervalSGI != NULL;
    }
    glx.has_srgb = has_extension(ext, "GLX_ARB_framebuffer_sRGB") || has_extension(ext, "GLX_EXT_framebuffer_sRGB");
    glx.has_float = has_extension(ext, "GLX_ARB_fbconfig_float");
    if (glx.has_create_context) {
        /* the ES bit and the other attributes are only meaningful through
         * glXCreateContextAttribsARB (the ES bit also needs the profile mask) */
        glx.has_es_profile = glx.has_create_context_profile && has_extension(ext, "GLX_EXT_create_context_es_profile");
        glx.has_es2_profile = glx.has_create_context_profile && has_extension(ext, "GLX_EXT_create_context_es2_profile");
        glx.has_robustness = has_extension(ext, "GLX_ARB_create_context_robustness");
        glx.has_no_error = has_extension(ext, "GLX_ARB_create_context_no_error");
        glx.has_flush_control = has_extension(ext, "GLX_ARB_context_flush_control");
    }
    glx.usable = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop GLX %d.%d on X11 (create_context=%d es=%s swap=%s)", major, minor,
                glx.has_create_context,
                glx.has_es_profile ? "all" : glx.has_es2_profile ? "2.0" : "none",
                glx.has_swap_ext ? "EXT" : glx.has_swap_mesa ? "MESA" : glx.has_swap_sgi ? "SGI" : "none");
    return true;
}

/* GLXFBConfig for the current attributes whose X visual is the one SDLop
 * creates its windows with (glXMakeCurrent fails with BadMatch otherwise). */
static bool glx_pick_config(GLXFBConfig_ *out)
{
    int attribs[64];
    int n = 0;
    attribs[n++] = GLX_X_RENDERABLE_;  attribs[n++] = 1;
    attribs[n++] = GLX_DRAWABLE_TYPE_; attribs[n++] = GLX_WINDOW_BIT_;
    attribs[n++] = GLX_RENDER_TYPE_;
    attribs[n++] = (sdlop_glattrs.floatbuffers && glx.has_float) ? GLX_RGBA_FLOAT_BIT_ARB_ : GLX_RGBA_BIT_;
    attribs[n++] = GLX_RED_SIZE_;      attribs[n++] = sdlop_glattrs.red_size;
    attribs[n++] = GLX_GREEN_SIZE_;    attribs[n++] = sdlop_glattrs.green_size;
    attribs[n++] = GLX_BLUE_SIZE_;     attribs[n++] = sdlop_glattrs.blue_size;
    if (sdlop_glattrs.alpha_size) {
        attribs[n++] = GLX_ALPHA_SIZE_; attribs[n++] = sdlop_glattrs.alpha_size;
    }
    if (sdlop_glattrs.buffer_size) {
        attribs[n++] = GLX_BUFFER_SIZE_; attribs[n++] = sdlop_glattrs.buffer_size;
    }
    attribs[n++] = GLX_DOUBLEBUFFER_;  attribs[n++] = sdlop_glattrs.doublebuffer ? 1 : 0;
    attribs[n++] = GLX_DEPTH_SIZE_;    attribs[n++] = sdlop_glattrs.depth_size;
    if (sdlop_glattrs.stencil_size) {
        attribs[n++] = GLX_STENCIL_SIZE_; attribs[n++] = sdlop_glattrs.stencil_size;
    }
    if (sdlop_glattrs.accum_red) {
        attribs[n++] = GLX_ACCUM_RED_SIZE_; attribs[n++] = sdlop_glattrs.accum_red;
    }
    if (sdlop_glattrs.accum_green) {
        attribs[n++] = GLX_ACCUM_GREEN_SIZE_; attribs[n++] = sdlop_glattrs.accum_green;
    }
    if (sdlop_glattrs.accum_blue) {
        attribs[n++] = GLX_ACCUM_BLUE_SIZE_; attribs[n++] = sdlop_glattrs.accum_blue;
    }
    if (sdlop_glattrs.accum_alpha) {
        attribs[n++] = GLX_ACCUM_ALPHA_SIZE_; attribs[n++] = sdlop_glattrs.accum_alpha;
    }
    if (sdlop_glattrs.stereo) {
        attribs[n++] = GLX_STEREO_; attribs[n++] = 1;
    }
    if (sdlop_glattrs.multisamplebuffers || sdlop_glattrs.multisamplesamples) {
        attribs[n++] = GLX_SAMPLE_BUFFERS_; attribs[n++] = sdlop_glattrs.multisamplebuffers ? sdlop_glattrs.multisamplebuffers : 1;
        attribs[n++] = GLX_SAMPLES_;        attribs[n++] = sdlop_glattrs.multisamplesamples;
    }
    if (sdlop_glattrs.framebuffer_srgb_capable && glx.has_srgb) {
        attribs[n++] = GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB_; attribs[n++] = 1;
    }
    attribs[n++] = 0; /* None */

    int num = 0;
    GLXFBConfig_ *configs = glx.ChooseFBConfig(glx.dpy, glx.screen, attribs, &num);
    if (!configs || num <= 0) {
        if (configs) {
            glx.XFree(configs);
        }
        return SDL_SetError("glXChooseFBConfig found no matching config");
    }
    const xcb_visualid_t visual = SDLOP_X11_GetVisual();
    bool found = false;
    for (int i = 0; i < num && !found; i++) {
        int vid = 0;
        if (glx.GetFBConfigAttrib(glx.dpy, configs[i], GLX_VISUAL_ID_, &vid) == 0 &&
            (xcb_visualid_t)vid == visual) {
            *out = configs[i];
            found = true;
        }
    }
    glx.XFree(configs);
    if (!found) {
        return SDL_SetError("No GLX config matches the window visual 0x%x", (unsigned)visual);
    }
    return true;
}

static void *glx_CreateContext(void)
{
    const bool es = sdlop_glattrs.profile_mask == SDL_GL_CONTEXT_PROFILE_ES;
    GLXFBConfig_ config = NULL;
    if (!glx_pick_config(&config)) {
        return NULL;
    }
    GLXContext_ share = NULL;
    if (sdlop_glattrs.share_with_current_context) {
        X11GLContext *cur = (X11GLContext *)SDLOP_GL_CurrentContext();
        if (cur && cur->backend == X11GL_BACKEND_GLX) {
            share = cur->glx_ctx;
        }
    }

    GLXContext_ ctx = NULL;
    XErrorHandler_ old = glx_trap_begin();
    /* same rule as stock SDL: plain 1.x/2.x requests without profile or
     * flags get a legacy context, everything else goes through
     * GLX_ARB_create_context */
    const bool legacy = sdlop_glattrs.major_version < 3 && sdlop_glattrs.profile_mask == 0 &&
                        sdlop_glattrs.flags == 0 && !sdlop_glattrs.no_error &&
                        sdlop_glattrs.reset_notification == SDL_GL_CONTEXT_RESET_NO_NOTIFICATION &&
                        sdlop_glattrs.release_behavior == SDL_GL_CONTEXT_RELEASE_BEHAVIOR_FLUSH;
    if (legacy || !glx.has_create_context) {
        ctx = glx.CreateNewContext(glx.dpy, config, GLX_RGBA_TYPE_, share, 1);
    } else {
        int attribs[24];
        int n = 0;
        attribs[n++] = GLX_CONTEXT_MAJOR_VERSION_ARB_; attribs[n++] = sdlop_glattrs.major_version;
        attribs[n++] = GLX_CONTEXT_MINOR_VERSION_ARB_; attribs[n++] = sdlop_glattrs.minor_version;
        if (es) {
            attribs[n++] = GLX_CONTEXT_PROFILE_MASK_ARB_;
            attribs[n++] = GLX_CONTEXT_ES_PROFILE_BIT_EXT_;
        } else if (sdlop_glattrs.profile_mask && glx.has_create_context_profile) {
            attribs[n++] = GLX_CONTEXT_PROFILE_MASK_ARB_;
            attribs[n++] = (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_COMPATIBILITY)
                               ? GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB_
                               : GLX_CONTEXT_CORE_PROFILE_BIT_ARB_;
        }
        /* SDL_GL_CONTEXT_*_FLAG values equal the GLX_CONTEXT_*_BIT_ARB bits;
         * ROBUST_ACCESS is only defined with the robustness extension and
         * forward-compatible only for desktop GL 3.0+ */
        int flags = sdlop_glattrs.flags;
        if (!glx.has_robustness) {
            flags &= ~SDL_GL_CONTEXT_ROBUST_ACCESS_FLAG;
        }
        if (es) {
            flags &= ~SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG;
        }
        if (flags) {
            attribs[n++] = GLX_CONTEXT_FLAGS_ARB_; attribs[n++] = flags;
        }
        /* the rest only when the extension exists and the value is not the
         * default, like stock */
        if (glx.has_flush_control && sdlop_glattrs.release_behavior == SDL_GL_CONTEXT_RELEASE_BEHAVIOR_NONE) {
            attribs[n++] = GLX_CONTEXT_RELEASE_BEHAVIOR_ARB_;
            attribs[n++] = GLX_CONTEXT_RELEASE_BEHAVIOR_NONE_ARB_;
        }
        if (glx.has_robustness && sdlop_glattrs.reset_notification == SDL_GL_CONTEXT_RESET_LOSE_CONTEXT) {
            attribs[n++] = GLX_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB_;
            attribs[n++] = GLX_LOSE_CONTEXT_ON_RESET_ARB_;
        }
        if (glx.has_no_error && sdlop_glattrs.no_error) {
            attribs[n++] = GLX_CONTEXT_OPENGL_NO_ERROR_ARB_; attribs[n++] = 1;
        }
        attribs[n++] = 0;
        ctx = glx.CreateContextAttribsARB(glx.dpy, config, share, 1, attribs);
    }
    const int xerr = glx_trap_end(old);
    if (!ctx || xerr) {
        if (ctx) {
            glx.DestroyContext(glx.dpy, ctx);
        }
        SDL_SetError("Couldn't create GLX context for OpenGL%s %d.%d%s (X error %d)", es ? " ES" : "",
                     sdlop_glattrs.major_version, sdlop_glattrs.minor_version,
                     (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_CORE) ? " core" : "", xerr);
        return NULL;
    }
    X11GLContext *glctx = (X11GLContext *)calloc(1, sizeof(*glctx));
    if (!glctx) {
        glx.DestroyContext(glx.dpy, ctx);
        SDL_OutOfMemory();
        return NULL;
    }
    glctx->backend = X11GL_BACKEND_GLX;
    glctx->glx_ctx = ctx;
    return glctx;
}

static void glx_apply_swap_interval(GLXDrawable_ drawable)
{
    if (glx.has_swap_ext && drawable) {
        glx.SwapIntervalEXT(glx.dpy, drawable, swap_interval);
    } else if (glx.has_swap_mesa) {
        glx.SwapIntervalMESA((unsigned int)(swap_interval < 0 ? 1 : swap_interval));
    } else if (glx.has_swap_sgi && swap_interval > 0) {
        glx.SwapIntervalSGI(swap_interval);
    }
}

static bool glx_MakeCurrent(SDL_Window *window, X11GLContext *glctx)
{
    if (!glx.usable) {
        return glctx ? SDL_SetError("GLX is not initialized") : true;
    }
    GLXDrawable_ drawable = 0;
    if (glctx && window) {
        drawable = SDLOP_X11_GetWindowXID(window);
        if (!drawable) {
            return SDL_SetError("Invalid window for GL");
        }
    }
    XErrorHandler_ old = glx_trap_begin();
    const int ok = glx.MakeCurrent(glx.dpy, drawable, glctx ? (GLXContext_)glctx->glx_ctx : NULL);
    const int xerr = glx_trap_end(old);
    if (!ok || xerr) {
        return SDL_SetError("glXMakeCurrent failed (X error %d)", xerr);
    }
    if (window) {
        SDLOP_X11_SetGLActive(window, glctx != NULL);
    }
    if (glctx && drawable) {
        glx_apply_swap_interval(drawable);
    }
    return true;
}

static void glx_DeleteContext(X11GLContext *glctx)
{
    if (glx.usable && glctx->glx_ctx) {
        if (glx.GetCurrentContext() == glctx->glx_ctx) {
            glx.MakeCurrent(glx.dpy, 0, NULL);
        }
        glx.DestroyContext(glx.dpy, (GLXContext_)glctx->glx_ctx);
        glx.XSync(glx.dpy, 0);
    }
    free(glctx);
}

static bool glx_SwapBuffers(SDL_Window *window)
{
    xcb_window_t xid = SDLOP_X11_GetWindowXID(window);
    if (!glx.usable || xid == XCB_WINDOW_NONE) {
        return SDL_SetError("Window has no GL context");
    }
    glx.SwapBuffers(glx.dpy, xid);
    return true;
}

static void glx_Quit(void)
{
    if (glx.usable && glx.GetCurrentContext()) {
        glx.MakeCurrent(glx.dpy, 0, NULL);
    }
    void *lib = glx.lib;
    memset(&glx, 0, sizeof(glx));
    glx.lib = lib; /* see the struct: libGL stays loaded */
}

/* ------------------------------------------------------------------ */
/* Backend dispatch (driver entry points)                              */
/* ------------------------------------------------------------------ */

/* Whether a new context with the current attributes goes to GLX (may load
 * GLX). Same decision as stock's X11_GL_UseEGL, except that ES versions are
 * not capped at what GL_ARB_ES{2,3}_compatibility suggests: the driver is
 * asked, and a refused request falls back to EGL in CreateContext. */
static bool want_glx(void)
{
    if (force_egl()) {
        return false;
    }
    if (sdlop_glattrs.profile_mask != SDL_GL_CONTEXT_PROFILE_ES) {
        return glx_init();
    }
    if (SDL_GetHintBoolean(SDL_HINT_OPENGL_ES_DRIVER, false)) {
        return false;
    }
    if (!glx_init()) {
        return false;
    }
    if (glx.has_es_profile) {
        return true;
    }
    /* GLX_EXT_create_context_es2_profile alone only defines ES 2.0 */
    if (glx.has_es2_profile && sdlop_glattrs.major_version == 2 && sdlop_glattrs.minor_version == 0) {
        return true;
    }
    return SDL_SetError("GLX has no OpenGL ES %d.%d profile", sdlop_glattrs.major_version, sdlop_glattrs.minor_version);
}

static bool current_is_glx(void)
{
    X11GLContext *cur = (X11GLContext *)SDLOP_GL_CurrentContext();
    return cur && cur->backend == X11GL_BACKEND_GLX;
}

void *SDLOP_X11_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    /* A forced GL offload must use the generic headless-GPU path in
     * SDL_gl.c rather than GLX/EGL rendering into the X11 drawable. */
    if (SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_FORCE &&
        SDLOP_GLOffload_HasHardwareGPU()) {
        SDL_SetError("GL offload forced; X11 native GL disabled");
        return NULL;
    }
    const bool es = sdlop_glattrs.profile_mask == SDL_GL_CONTEXT_PROFILE_ES;
    if (want_glx()) {
        GLXFBConfig_ probe = NULL;
        if (glx_pick_config(&probe)) {
            void *ctx = glx_CreateContext();
            if (ctx || !es) {
                /* desktop GL: a refused version is the answer, as in stock */
                return ctx;
            }
            /* the driver advertises ES over GLX but not this version */
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop: %s, trying EGL", SDL_GetError());
            SDL_ClearError();
            return egl_CreateContext();
        }
    }
    if (!force_egl() && (!es || !SDL_GetHintBoolean(SDL_HINT_OPENGL_ES_DRIVER, false))) {
        /* GLX unusable here: say why, then try EGL */
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop: GLX not used (%s), using EGL", SDL_GetError());
        SDL_ClearError();
    }
    return egl_CreateContext();
}

bool SDLOP_X11_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context)
{
    (void)device;
    X11GLContext *glctx = (X11GLContext *)context;
    if (!glctx) {
        /* release whatever is current */
        bool ok = true;
        if (glx.usable && glx.GetCurrentContext()) {
            ok = glx_MakeCurrent(window, NULL);
        }
        if (egl_initialized) {
            ok = egl_MakeCurrent(window, NULL) && ok;
        }
        if (window) {
            SDLOP_X11_SetGLActive(window, false);
        }
        return ok;
    }
    if (glctx->backend == X11GL_BACKEND_GLX) {
        if (egl_initialized && SDLOP_EGL_eglGetCurrentContext() != EGL_NO_CONTEXT) {
            SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        return glx_MakeCurrent(window, glctx);
    }
    if (glx.usable && glx.GetCurrentContext()) {
        glx.MakeCurrent(glx.dpy, 0, NULL);
    }
    return egl_MakeCurrent(window, glctx);
}

bool SDLOP_X11_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (current_is_glx()) {
        return glx_SwapBuffers(window);
    }
    return egl_SwapBuffers(window);
}

void SDLOP_X11_GL_DeleteContext(SDLop_VideoDevice *device, void *context)
{
    (void)device;
    X11GLContext *glctx = (X11GLContext *)context;
    if (!glctx) {
        return;
    }
    if (glctx->backend == X11GL_BACKEND_GLX) {
        glx_DeleteContext(glctx);
    } else {
        egl_DeleteContext(glctx);
    }
}

void SDLOP_X11_GL_WindowDestroyed(SDL_Window *window)
{
    /* the window is destroyed on SDLop's xcb connection right after this:
     * release it on the GLX connection first and wait for the server, or
     * Mesa may still reference the drawable */
    if (glx.usable) {
        xcb_window_t xid = SDLOP_X11_GetWindowXID(window);
        if (xid != XCB_WINDOW_NONE && glx.GetCurrentDrawable() == xid) {
            glx.MakeCurrent(glx.dpy, 0, NULL);
        }
        glx.XSync(glx.dpy, 0);
    }
    egl_WindowDestroyed(window);
}

SDL_FunctionPointer SDLOP_X11_GL_GetProcAddress(SDLop_VideoDevice *device, const char *proc)
{
    (void)device;
    X11GLContext *cur = (X11GLContext *)SDLOP_GL_CurrentContext();
    bool use_glx = cur ? cur->backend == X11GL_BACKEND_GLX : want_glx();
    if (use_glx) {
        SDL_FunctionPointer fn = (SDL_FunctionPointer)glx.GetProcAddress((const unsigned char *)proc);
        if (fn) {
            return fn;
        }
        return (SDL_FunctionPointer)dlsym(glx.lib, proc);
    }
    SDL_ClearError();
    return egl_GetProcAddress(proc);
}

bool SDLOP_X11_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval)
{
    (void)device;
    if (current_is_glx()) {
        if (interval < 0 && !glx.has_swap_tear) {
            return SDL_SetError("Adaptive vsync (-1) needs GLX_EXT_swap_control_tear");
        }
        /* same order and results as stock SDL3 (SDL_x11opengl.c) */
        if (glx.has_swap_ext) {
            GLXDrawable_ drawable = glx.GetCurrentDrawable();
            if (!drawable) {
                return SDL_SetError("No current GLX drawable");
            }
            glx.SwapIntervalEXT(glx.dpy, drawable, interval);
        } else if (glx.has_swap_mesa) {
            if (glx.SwapIntervalMESA((unsigned int)interval) != 0) {
                return SDL_SetError("glXSwapIntervalMESA failed");
            }
        } else if (glx.has_swap_sgi) {
            /* 0 is invalid for SGI: stock passes it through and reports
             * the failure, so do we */
            if (glx.SwapIntervalSGI(interval) != 0) {
                return SDL_SetError("glXSwapIntervalSGI failed");
            }
        } else {
            return SDL_Unsupported();
        }
        swap_interval = interval;
        return true;
    }
    return egl_SetSwapInterval(interval);
}

bool SDLOP_X11_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval)
{
    (void)device;
    *interval = swap_interval;
    return true;
}

/* driver shutdown: terminate EGL, release GLX, drop libEGL and the private
 * Xlib connection so a later SDL_Init starts from scratch */
void SDLOP_X11_GL_Quit(void)
{
    glx_Quit();
    egl_Quit();
    if (xlib_display && p_XCloseDisplay) {
        p_XCloseDisplay(xlib_display);
    }
    xlib_display = NULL;
    swap_interval = 0;
}
