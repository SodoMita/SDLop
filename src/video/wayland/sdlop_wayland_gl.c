/*
  SDLop - Wayland OpenGL support via EGL.

  Uses the Wayland EGL platform; on a GPU-less system Mesa routes this to
  the llvmpipe software rasterizer (desktop GL up to 4.5 core or GLES 3.2).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_wayland_internal.h"

#include "sdlop_egl_dyn.h"
#include <dlfcn.h>
#include <string.h>

#ifndef EGL_PLATFORM_WAYLAND_EXT
#define EGL_PLATFORM_WAYLAND_EXT 0x31D8
#endif
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

/* libwayland-egl, loaded dynamically: required by Mesa for window surfaces
 * but dlopen-ed so SDLop still works without it (software / Vulkan paths) */
struct wl_egl_window;
typedef struct wl_egl_window *(*PFN_wl_egl_window_create)(struct wl_surface *, int, int);
typedef void (*PFN_wl_egl_window_destroy)(struct wl_egl_window *);
typedef void (*PFN_wl_egl_window_resize)(struct wl_egl_window *, int, int, int, int);
static PFN_wl_egl_window_create p_wl_egl_window_create;
static PFN_wl_egl_window_destroy p_wl_egl_window_destroy;
static PFN_wl_egl_window_resize p_wl_egl_window_resize;

static bool load_wayland_egl(void)
{
    if (p_wl_egl_window_create) {
        return true;
    }
    void *lib = dlopen("libwayland-egl.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) {
        lib = dlopen("libwayland-egl.so", RTLD_LAZY | RTLD_LOCAL);
    }
    if (!lib) {
        return SDL_SetError("libwayland-egl is required for OpenGL on Wayland");
    }
    p_wl_egl_window_create = (PFN_wl_egl_window_create)dlsym(lib, "wl_egl_window_create");
    p_wl_egl_window_destroy = (PFN_wl_egl_window_destroy)dlsym(lib, "wl_egl_window_destroy");
    p_wl_egl_window_resize = (PFN_wl_egl_window_resize)dlsym(lib, "wl_egl_window_resize");
    if (!p_wl_egl_window_create || !p_wl_egl_window_destroy || !p_wl_egl_window_resize) {
        return SDL_SetError("libwayland-egl is missing wl_egl_window functions");
    }
    return true;
}

typedef struct WaylandGLContext
{
    EGLContext ctx;
    uint32_t api; /* EGL_OPENGL_API or EGL_OPENGL_ES_API */
} WaylandGLContext;

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static bool egl_initialized;
static int swap_interval = 0;

static bool ensure_egl_display(void)
{
    if (egl_initialized) {
        return true;
    }
    if (!SDLOP_EGL_LoadSymbols()) {
        return false;
    }
    struct wl_display *wl = SDLOP_Wayland_GetDisplay();
    if (!wl) {
        return SDL_SetError("Wayland display not available");
    }
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)SDLOP_EGL_eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display) {
        egl_display = get_platform_display(EGL_PLATFORM_WAYLAND_EXT, wl, NULL);
    }
    if (egl_display == EGL_NO_DISPLAY) {
        egl_display = SDLOP_EGL_eglGetDisplay((EGLNativeDisplayType)wl);
    }
    if (egl_display == EGL_NO_DISPLAY) {
        return SDL_SetError("eglGetDisplay failed");
    }
    EGLint major = 0, minor = 0;
    if (!SDLOP_EGL_eglInitialize(egl_display, &major, &minor)) {
        egl_display = EGL_NO_DISPLAY;
        return SDL_SetError("eglInitialize failed: 0x%x", SDLOP_EGL_eglGetError());
    }
    egl_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop EGL %d.%d on Wayland (%s)",
                major, minor, SDLOP_EGL_eglQueryString(egl_display, EGL_VERSION));
    return true;
}

static bool pick_config(EGLConfig *out, uint32_t api)
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

    EGLConfig configs[32];
    EGLint num = 0;
    if (!SDLOP_EGL_eglChooseConfig(egl_display, attribs, configs, 32, &num) || num == 0) {
        return SDL_SetError("eglChooseConfig found no matching config");
    }
    *out = configs[0];
    return true;
}

void *SDLOP_Wayland_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (!ensure_egl_display()) {
        return NULL;
    }

    bool want_desktop_gl =
        (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_CORE) != 0 ||
        ((sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_ES) == 0 && sdlop_glattrs.major_version >= 3);

    uint32_t api = want_desktop_gl ? EGL_OPENGL_API : EGL_OPENGL_ES_API;
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
            ctx_attribs[n++] = EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
        }
        if (sdlop_glattrs.flags & SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG) {
            ctx_attribs[n++] = EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR;
        }
    } else {
        ctx_attribs[n++] = EGL_CONTEXT_CLIENT_VERSION;
        ctx_attribs[n++] = sdlop_glattrs.major_version >= 3 ? 3 : 2;
    }
    ctx_attribs[n++] = EGL_NONE;

    /* SDL_GL_SHARE_WITH_CURRENT_CONTEXT: share with the currently bound
     * context (EGL requires compatible client APIs for sharing) */
    EGLContext share = EGL_NO_CONTEXT;
    if (sdlop_glattrs.share_with_current_context) {
        SDL_GLContext cur = SDLOP_GL_CurrentContext();
        if (cur) {
            share = ((WaylandGLContext *)cur)->ctx;
        }
    }
    EGLContext ctx = SDLOP_EGL_eglCreateContext(egl_display, config, share, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) {
        return (SDL_SetError("eglCreateContext failed: 0x%x", SDLOP_EGL_eglGetError()), NULL);
    }

    WaylandGLContext *glctx = (WaylandGLContext *)calloc(1, sizeof(*glctx));
    if (!glctx) {
        SDLOP_EGL_eglDestroyContext(egl_display, ctx);
        SDL_OutOfMemory();
        return NULL;
    }
    glctx->ctx = ctx;
    glctx->api = api;
    (void)window;
    return glctx;
}

static EGLConfig config_for_context(WaylandGLContext *glctx)
{
    EGLConfig config;
    EGLint id = 0;
    if (SDLOP_EGL_eglQueryContext(egl_display, glctx->ctx, EGL_CONFIG_ID, &id)) {
        EGLint attribs[3] = { EGL_CONFIG_ID, id, EGL_NONE };
        EGLConfig configs[1];
        EGLint num = 0;
        if (SDLOP_EGL_eglChooseConfig(egl_display, attribs, configs, 1, &num) && num == 1) {
            config = configs[0];
            return config;
        }
    }
    return pick_config(&config, glctx->api) ? config : NULL;
}

bool SDLOP_Wayland_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context)
{
    (void)device;
    if (!egl_initialized && !ensure_egl_display()) {
        return false;
    }
    if (!context) {
        SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        SDLOP_Wayland_SetGLActive(window, false);
        return true;
    }
    WaylandGLContext *glctx = (WaylandGLContext *)context;
    if (!SDLOP_EGL_eglBindAPI(glctx->api)) {
        return SDL_SetError("eglBindAPI failed: 0x%x", SDLOP_EGL_eglGetError());
    }

    EGLSurface surface = EGL_NO_SURFACE;
    if (window) {
        void **slot = SDLOP_Wayland_GetGLSurfaceSlot(window);
        struct wl_surface *wls = SDLOP_Wayland_GetWindowSurfaceHandle(window);
        if (!slot || !wls) {
            return SDL_SetError("Invalid window for GL");
        }
        if (!*slot) {
            if (!load_wayland_egl()) {
                return false;
            }
            void **eglwin_slot = SDLOP_Wayland_GetGLEGLWindowSlot(window);
            if (eglwin_slot && !*eglwin_slot) {
                *eglwin_slot = p_wl_egl_window_create(wls, window->w, window->h);
                if (!*eglwin_slot) {
                    return SDL_SetError("wl_egl_window_create failed");
                }
            }
            if (!eglwin_slot || !*eglwin_slot) {
                return SDL_SetError("No EGL window storage");
            }
            EGLConfig config = config_for_context(glctx);
            if (!config) {
                return SDL_SetError("No EGL config for context");
            }
            *slot = SDLOP_EGL_eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)*eglwin_slot, NULL);
            if (*slot == NULL) {
                return SDL_SetError("eglCreateWindowSurface failed: 0x%x", SDLOP_EGL_eglGetError());
            }
        }
        surface = (EGLSurface)*slot;
        SDLOP_Wayland_SetGLActive(window, true);
    }

    if (!SDLOP_EGL_eglMakeCurrent(egl_display, surface, surface, glctx->ctx)) {
        return SDL_SetError("eglMakeCurrent failed: 0x%x", SDLOP_EGL_eglGetError());
    }
    SDLOP_EGL_eglSwapInterval(egl_display, swap_interval);
    return true;
}

bool SDLOP_Wayland_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    void **slot = SDLOP_Wayland_GetGLSurfaceSlot(window);
    if (!slot || !*slot) {
        return SDL_SetError("Window has no GL surface");
    }
    if (!SDLOP_EGL_eglSwapBuffers(egl_display, (EGLSurface)*slot)) {
        EGLint err = SDLOP_EGL_eglGetError();
        if (err == EGL_BAD_SURFACE || err == EGL_BAD_NATIVE_WINDOW) {
            /* surface went away (window destroyed/hidden); recreate lazily */
            *slot = NULL;
        }
        return SDL_SetError("eglSwapBuffers failed: 0x%x", err);
    }
    return true;
}

void SDLOP_Wayland_GL_DeleteContext(SDLop_VideoDevice *device, void *context)
{
    (void)device;
    WaylandGLContext *glctx = (WaylandGLContext *)context;
    if (!glctx) {
        return;
    }
    if (egl_initialized) {
        SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        SDLOP_EGL_eglDestroyContext(egl_display, glctx->ctx);
    }
    free(glctx);
}

void SDLOP_Wayland_GL_WindowDestroyed(SDL_Window *window)
{
    void **slot = SDLOP_Wayland_GetGLSurfaceSlot(window);
    if (slot && *slot) {
        if (egl_initialized) {
            SDLOP_EGL_eglDestroySurface(egl_display, (EGLSurface)*slot);
        }
        *slot = NULL;
    }
    void **eglwin_slot = SDLOP_Wayland_GetGLEGLWindowSlot(window);
    if (eglwin_slot && *eglwin_slot) {
        if (p_wl_egl_window_destroy) {
            p_wl_egl_window_destroy((struct wl_egl_window *)*eglwin_slot);
        }
        *eglwin_slot = NULL;
    }
    SDLOP_Wayland_SetGLActive(window, false);
}

void SDLOP_Wayland_GL_WindowResized(SDL_Window *window)
{
    void **eglwin_slot = SDLOP_Wayland_GetGLEGLWindowSlot(window);
    if (eglwin_slot && *eglwin_slot && p_wl_egl_window_resize) {
        p_wl_egl_window_resize((struct wl_egl_window *)*eglwin_slot, window->w, window->h, 0, 0);
    }
}

SDL_FunctionPointer SDLOP_Wayland_GL_GetProcAddress(const char *proc)
{
    SDL_FunctionPointer fn = SDLOP_EGL_eglGetProcAddress(proc);
    if (fn) {
        return fn;
    }
    /* eglGetProcAddress only covers GL(E) entry points; fall back to the
     * process symbol table for library functions */
    void *sym = dlsym(RTLD_DEFAULT, proc);
    return (SDL_FunctionPointer)sym;
}

SDL_FunctionPointer SDLOP_Wayland_GL_GetProcAddressThunk(SDLop_VideoDevice *device, const char *proc)
{
    (void)device;
    return SDLOP_Wayland_GL_GetProcAddress(proc);
}

bool SDLOP_Wayland_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval)
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

bool SDLOP_Wayland_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval)
{
    (void)device;
    *interval = swap_interval;
    return true;
}
