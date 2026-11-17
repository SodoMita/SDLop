/*
  SDLop - X11 OpenGL support via EGL.

  Display selection, best first:
    1. EGL_PLATFORM_XCB_EXT on SDLop's own xcb connection (Mesa >= 21)
    2. EGL_PLATFORM_X11_EXT on a private Xlib connection (libX11 dlopen-ed
       on demand: X window ids are server-global, so a second connection
       renders into SDLop's windows just fine)
    3. eglGetDisplay(Display *)
  SDLOP_X11_EGL_PLATFORM=x11 skips (1), which is how (2) is tested.

  Without a GPU Mesa routes all of this to llvmpipe.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"
#include "../wayland/sdlop_egl_dyn.h"

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

typedef struct X11GLContext
{
    EGLContext ctx;
    EGLenum api; /* EGL_OPENGL_API or EGL_OPENGL_ES_API */
} X11GLContext;

/* private Xlib connection for the EGL_PLATFORM_X11 fallback */
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
        const char *name = getenv("SDLOP_LIB_X11");
        xlib_handle = dlopen(name && *name ? name : "libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!xlib_handle) {
            SDL_SetError("EGL needs libX11 on this system (no EGL_EXT_platform_xcb): %s", dlerror());
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
        SDL_SetError("XOpenDisplay failed for the EGL connection");
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
    const char *force = getenv("SDLOP_X11_EGL_PLATFORM");
    bool allow_xcb = !(force && strcmp(force, "x11") == 0);
    const char *platform = "?";

    if (get_platform_display && allow_xcb && has_extension(client_ext, "EGL_EXT_platform_xcb")) {
        EGLint attribs[] = { EGL_PLATFORM_XCB_SCREEN_EXT, SDLOP_X11_GetScreenNumber(), EGL_NONE };
        egl_display = get_platform_display(EGL_PLATFORM_XCB_EXT, conn, attribs);
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
            egl_display = get_platform_display(EGL_PLATFORM_X11_EXT, dpy, attribs);
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
    } else if (sdlop_glattrs.major_version >= 3) {
        renderable = EGL_OPENGL_ES3_BIT;
    } else {
        renderable = EGL_OPENGL_ES2_BIT;
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

void *SDLOP_X11_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
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
    EGLint ctx_attribs[12];
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
        ctx_attribs[n++] = EGL_CONTEXT_CLIENT_VERSION;
        ctx_attribs[n++] = sdlop_glattrs.major_version >= 3 ? 3 : 2;
    }
    ctx_attribs[n++] = EGL_NONE;

    EGLContext share = EGL_NO_CONTEXT;
    if (sdlop_glattrs.share_with_current_context) {
        SDL_GLContext cur = SDLOP_GL_CurrentContext();
        if (cur) {
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

bool SDLOP_X11_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context)
{
    (void)device;
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
            *slot = SDLOP_EGL_eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)xid, NULL);
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

bool SDLOP_X11_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
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

void SDLOP_X11_GL_DeleteContext(SDLop_VideoDevice *device, void *context)
{
    (void)device;
    X11GLContext *glctx = (X11GLContext *)context;
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

void SDLOP_X11_GL_WindowDestroyed(SDL_Window *window)
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

SDL_FunctionPointer SDLOP_X11_GL_GetProcAddress(SDLop_VideoDevice *device, const char *proc)
{
    (void)device;
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

bool SDLOP_X11_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval)
{
    (void)device;
    if (interval < 0) {
        return SDL_SetError("Adaptive vsync (-1) is not supported");
    }
    swap_interval = interval;
    if (egl_initialized) {
        SDLOP_EGL_eglSwapInterval(egl_display, interval);
    }
    return true;
}

bool SDLOP_X11_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval)
{
    (void)device;
    *interval = swap_interval;
    return true;
}

/* driver shutdown: terminate EGL and drop our libEGL reference so a later
 * SDL_Init starts from scratch */
void SDLOP_X11_GL_Quit(void)
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
    if (xlib_display && p_XCloseDisplay) {
        p_XCloseDisplay(xlib_display);
    }
    xlib_display = NULL;
    swap_interval = 0;
}
