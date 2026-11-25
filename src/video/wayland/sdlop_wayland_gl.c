/*
  SDLop - Wayland OpenGL support via EGL.

  Uses the Wayland EGL platform; on a GPU-less system Mesa routes this to
  the llvmpipe software rasterizer (desktop GL up to 4.5 core or GLES 3.2).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_wayland_internal.h"

#include "../sdlop_egl_dyn.h"
#include "../sdlop_gl_offload.h"
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
    bool is_offload;
    SDLOP_GLOffloadContext *offload_ctx;
} WaylandGLContext;

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static bool egl_initialized;
static int swap_interval = 0;
static bool wayland_offload_decided;
static bool wayland_use_offload;

static void wayland_native_gl_deinit(void)
{
    if (egl_initialized) {
        SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        SDLOP_EGL_eglTerminate(egl_display);
        egl_initialized = false;
        egl_display = EGL_NO_DISPLAY;
        SDLOP_EGL_UnloadSymbols();
    }
}

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
    egl_display = SDLOP_EGL_PlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, wl, NULL);
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

    EGLConfig configs[32];
    EGLint num = 0;
    if (!SDLOP_EGL_eglChooseConfig(egl_display, attribs, configs, 32, &num) || num == 0) {
        return SDL_SetError("eglChooseConfig found no matching config");
    }
    *out = configs[0];
    return true;
}

static void *wayland_create_offload_context(SDLop_VideoDevice *device, SDL_Window *window, uint32_t api)
{
    (void)device;
    if (!window) {
        SDL_SetError("Invalid window for GL offload");
        return NULL;
    }
    if (!SDLOP_Wayland_AcquireOffloadShm(window, NULL, NULL, NULL, NULL)) {
        return NULL;
    }

    SDLOP_GLOffloadContext *share_offload = NULL;
    if (sdlop_glattrs.share_with_current_context) {
        SDL_GLContext cur = SDLOP_GL_CurrentContext();
        if (cur && ((WaylandGLContext *)cur)->is_offload) {
            share_offload = ((WaylandGLContext *)cur)->offload_ctx;
        }
    }

    SDLOP_GLOffloadContext *octx = SDLOP_GLOffload_CreateContext(api, share_offload);
    if (!octx) {
        return NULL;
    }

    WaylandGLContext *glctx = (WaylandGLContext *)calloc(1, sizeof(*glctx));
    if (!glctx) {
        SDLOP_GLOffload_DeleteContext(octx);
        SDL_OutOfMemory();
        return NULL;
    }
    glctx->api = api;
    glctx->is_offload = true;
    glctx->offload_ctx = octx;

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop Wayland/Offload: GL context created (%s on %s)",
                api == EGL_OPENGL_API ? "desktop GL" : "GLES",
                SDLOP_GLOffload_GetGPUDescription() ? SDLOP_GLOffload_GetGPUDescription() : "GPU");
    return glctx;
}

void *SDLOP_Wayland_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    /* GLES has no 2.1: a 2.1 (or higher) request without an ES profile means
     * desktop GL, like SDL2. Only bare 2.0 stays GLES. */
    bool want_desktop_gl =
        (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_CORE) != 0 ||
        ((sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_ES) == 0 &&
         (sdlop_glattrs.major_version > 2 ||
          (sdlop_glattrs.major_version == 2 && sdlop_glattrs.minor_version > 0)));
    uint32_t api = want_desktop_gl ? EGL_OPENGL_API : EGL_OPENGL_ES_API;

    SDLOP_GLOffloadPolicy policy = SDLOP_GLOffload_GetPolicy();
    if (!wayland_offload_decided) {
        if (policy == SDLOP_GL_OFFLOAD_FORCE && SDLOP_GLOffload_HasHardwareGPU()) {
            wayland_use_offload = true;
            wayland_offload_decided = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "Wayland: GL offload forced via SDLOP_GL_OFFLOAD (%s)",
                        SDLOP_GLOffload_GetGPUDescription());
        } else if (policy != SDLOP_GL_OFFLOAD_NEVER &&
                   !SDLOP_Wayland_CompositorHasHardwareBuffers() &&
                   SDLOP_GLOffload_HasHardwareGPU()) {
            wayland_use_offload = true;
            wayland_offload_decided = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "Wayland: compositor lacks hardware buffer sharing; offloading GL to GPU (%s)",
                        SDLOP_GLOffload_GetGPUDescription());
        }
    }

    if (wayland_offload_decided && wayland_use_offload) {
        return wayland_create_offload_context(device, window, api);
    }

    if (!ensure_egl_display()) {
        if (!wayland_offload_decided && policy != SDLOP_GL_OFFLOAD_NEVER &&
            SDLOP_GLOffload_HasHardwareGPU()) {
            wayland_use_offload = true;
            wayland_offload_decided = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "Wayland: native EGL unavailable; offloading GL to GPU (%s)",
                        SDLOP_GLOffload_GetGPUDescription());
            return wayland_create_offload_context(device, window, api);
        }
        return NULL;
    }

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
        /* EGL 1.5 boolean attributes (the *_BIT_KHR values belong to
         * EGL_CONTEXT_FLAGS_KHR and are not attribute names) */
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

    /* SDL_GL_SHARE_WITH_CURRENT_CONTEXT: share with the currently bound
     * context (EGL requires compatible client APIs for sharing) */
    EGLContext share = EGL_NO_CONTEXT;
    if (sdlop_glattrs.share_with_current_context) {
        SDL_GLContext cur = SDLOP_GL_CurrentContext();
        if (cur && !((WaylandGLContext *)cur)->is_offload) {
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

    if (!wayland_offload_decided) {
        /* Bind once to inspect GL_RENDERER on the native Wayland path. */
        if (!SDLOP_Wayland_GL_MakeCurrent(device, window, glctx)) {
            SDLOP_Wayland_GL_WindowDestroyed(window);
            SDLOP_EGL_eglDestroyContext(egl_display, ctx);
            free(glctx);
            if (policy != SDLOP_GL_OFFLOAD_NEVER && SDLOP_GLOffload_HasHardwareGPU()) {
                wayland_native_gl_deinit();
                wayland_use_offload = true;
                wayland_offload_decided = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                            "Wayland: EGL window surface failed; offloading GL to GPU (%s)",
                            SDLOP_GLOffload_GetGPUDescription());
                return wayland_create_offload_context(device, window, api);
            }
            return NULL;
        }
        const char *renderer = SDLOP_GLOffload_QueryCurrentRenderer();
        bool is_sw = SDLOP_GLOffload_IsSoftwareRenderer(renderer);
        if (is_sw && policy != SDLOP_GL_OFFLOAD_NEVER && SDLOP_GLOffload_HasHardwareGPU()) {
            SDLOP_Wayland_GL_WindowDestroyed(window);
            SDLOP_EGL_eglDestroyContext(egl_display, ctx);
            free(glctx);
            wayland_native_gl_deinit();
            wayland_use_offload = true;
            wayland_offload_decided = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "Wayland: output GL is software (%s); offloading to GPU (%s)",
                        renderer ? renderer : "unknown",
                        SDLOP_GLOffload_GetGPUDescription());
            return wayland_create_offload_context(device, window, api);
        }
        wayland_use_offload = false;
        wayland_offload_decided = true;
        sdlop_glattrs.accelerated_visual = is_sw ? 0 : 1;
    }
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
    if (!context) {
        if (wayland_use_offload) {
            SDLOP_GLOffload_MakeCurrent(NULL, NULL);
        }
        if (egl_initialized) {
            SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        SDLOP_Wayland_SetGLActive(window, false);
        SDLOP_Wayland_SetGLOffloadActive(window, false);
        return true;
    }

    WaylandGLContext *glctx = (WaylandGLContext *)context;
    if (glctx->is_offload) {
        SDLOP_GLOffloadSurface *osurf = NULL;
        if (window) {
            void **slot = SDLOP_Wayland_GetGLSurfaceSlot(window);
            void **octx_slot = SDLOP_Wayland_GetGLOffloadContextSlot(window);
            if (!slot || !octx_slot) {
                return SDL_SetError("Invalid window for GL offload");
            }
            if (!SDLOP_Wayland_AcquireOffloadShm(window, NULL, NULL, NULL, NULL)) {
                return false;
            }
            int w, h;
            SDLOP_Wayland_GetWindowPixelSize(window, &w, &h);
            if (!*slot) {
                *slot = SDLOP_GLOffload_CreateSurface(glctx->offload_ctx, w, h);
                if (!*slot) {
                    return false;
                }
            } else {
                int sw = 0, sh = 0;
                SDLOP_GLOffload_GetSurfaceSize((SDLOP_GLOffloadSurface *)*slot, &sw, &sh);
                if (sw != w || sh != h) {
                    if (!SDLOP_GLOffload_ResizeSurface(glctx->offload_ctx,
                                                       (SDLOP_GLOffloadSurface **)slot, w, h)) {
                        return false;
                    }
                }
            }
            osurf = (SDLOP_GLOffloadSurface *)*slot;
            *octx_slot = glctx->offload_ctx;
            SDLOP_Wayland_SetGLActive(window, true);
            SDLOP_Wayland_SetGLOffloadActive(window, true);
        }
        return SDLOP_GLOffload_MakeCurrent(glctx->offload_ctx, osurf);
    }

    if (!egl_initialized && !ensure_egl_display()) {
        return false;
    }
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
                int pw, ph;
                SDLOP_Wayland_GetWindowPixelSize(window, &pw, &ph);
                *eglwin_slot = p_wl_egl_window_create(wls, pw, ph);
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
            EGLint surf_attribs[64];
            if (SDLOP_EGL_AppendUserAttribs(false, egl_display, config, surf_attribs, 0,
                                            (int)SDL_arraysize(surf_attribs)) < 0) {
                return false;
            }
            *slot = SDLOP_EGL_eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)*eglwin_slot,
                                                     surf_attribs);
            if (*slot == NULL) {
                return SDL_SetError("eglCreateWindowSurface failed: 0x%x", SDLOP_EGL_eglGetError());
            }
        }
        surface = (EGLSurface)*slot;
        SDLOP_Wayland_SetGLActive(window, true);
        SDLOP_Wayland_SetGLOffloadActive(window, false);
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
    if (SDLOP_Wayland_IsGLOffloadActive(window)) {
        void **slot = SDLOP_Wayland_GetGLSurfaceSlot(window);
        void **octx_slot = SDLOP_Wayland_GetGLOffloadContextSlot(window);
        WaylandGLContext *cur = (WaylandGLContext *)SDLOP_GL_CurrentContext();
        SDLOP_GLOffloadContext *octx = (cur && cur->is_offload)
            ? cur->offload_ctx
            : (octx_slot ? (SDLOP_GLOffloadContext *)*octx_slot : NULL);
        if (!octx || !slot || !*slot) {
            return SDL_SetError("Window has no offload GL surface");
        }
        void *shm_pixels = NULL;
        int shm_pitch = 0, shm_w = 0, shm_h = 0;
        if (!SDLOP_Wayland_AcquireOffloadShm(window, &shm_pixels, &shm_pitch, &shm_w, &shm_h)) {
            return false;
        }
        /* transparent windows present ARGB8888 buffers: keep GL's alpha */
        if (!SDLOP_GLOffload_ReadToARGB8888(octx, (SDLOP_GLOffloadSurface *)*slot, shm_pixels, shm_pitch, shm_w,
                                            shm_h, (window->flags & SDL_WINDOW_TRANSPARENT) != 0)) {
            return false;
        }
        if (!SDLOP_Wayland_PresentOffloadShm(window)) {
            return false;
        }
        int cur_w, cur_h;
        SDLOP_Wayland_GetWindowPixelSize(window, &cur_w, &cur_h);
        int sw = 0, sh = 0;
        SDLOP_GLOffload_GetSurfaceSize((SDLOP_GLOffloadSurface *)*slot, &sw, &sh);
        if (sw != cur_w || sh != cur_h) {
            if (!SDLOP_GLOffload_ResizeSurface(octx, (SDLOP_GLOffloadSurface **)slot, cur_w, cur_h)) {
                return false;
            }
        }
        return true;
    }

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
    if (glctx->is_offload) {
        SDLOP_GLOffload_DeleteContext(glctx->offload_ctx);
        free(glctx);
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
    if (SDLOP_Wayland_IsGLOffloadActive(window)) {
        if (slot && *slot) {
            SDLOP_GLOffload_DestroySurface((SDLOP_GLOffloadSurface *)*slot);
            *slot = NULL;
        }
        void **octx_slot = SDLOP_Wayland_GetGLOffloadContextSlot(window);
        if (octx_slot) {
            *octx_slot = NULL;
        }
        SDLOP_Wayland_SetGLOffloadActive(window, false);
        SDLOP_Wayland_SetGLActive(window, false);
        return;
    }

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
    if (SDLOP_Wayland_IsGLOffloadActive(window)) {
        void **slot = SDLOP_Wayland_GetGLSurfaceSlot(window);
        void **octx_slot = SDLOP_Wayland_GetGLOffloadContextSlot(window);
        WaylandGLContext *cur = (WaylandGLContext *)SDLOP_GL_CurrentContext();
        SDLOP_GLOffloadContext *octx = (cur && cur->is_offload)
            ? cur->offload_ctx
            : (octx_slot ? (SDLOP_GLOffloadContext *)*octx_slot : NULL);
        if (octx && slot && *slot) {
            int w, h;
            SDLOP_Wayland_GetWindowPixelSize(window, &w, &h);
            int sw = 0, sh = 0;
            SDLOP_GLOffload_GetSurfaceSize((SDLOP_GLOffloadSurface *)*slot, &sw, &sh);
            if (sw != w || sh != h) {
                if (!SDLOP_GLOffload_ResizeSurface(octx, (SDLOP_GLOffloadSurface **)slot, w, h)) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                                "Wayland/Offload: failed to resize GL surface to %dx%d: %s",
                                w, h, SDL_GetError());
                }
            }
        }
        return;
    }
    void **eglwin_slot = SDLOP_Wayland_GetGLEGLWindowSlot(window);
    if (eglwin_slot && *eglwin_slot && p_wl_egl_window_resize) {
        int pw, ph;
        SDLOP_Wayland_GetWindowPixelSize(window, &pw, &ph);
        p_wl_egl_window_resize((struct wl_egl_window *)*eglwin_slot, pw, ph, 0, 0);
    }
}

SDL_FunctionPointer SDLOP_Wayland_GL_GetProcAddress(const char *proc)
{
    if (wayland_offload_decided && wayland_use_offload) {
        return SDLOP_GLOffload_GetProcAddress(proc);
    }
    if (!egl_initialized &&
        SDLOP_GLOffload_GetPolicy() == SDLOP_GL_OFFLOAD_FORCE &&
        SDLOP_GLOffload_HasHardwareGPU()) {
        return SDLOP_GLOffload_GetProcAddress(proc);
    }
    SDL_FunctionPointer fn = SDLOP_EGL_eglGetProcAddress ? SDLOP_EGL_eglGetProcAddress(proc) : NULL;
    if (fn) {
        return fn;
    }
    /* eglGetProcAddress only covers GL(E) entry points; fall back to the
     * process symbol table for library functions */
    void *sym = dlsym(RTLD_DEFAULT, proc);
    return (SDL_FunctionPointer)sym;
}

/* SDL_EGL_GetCurrentDisplay / GetCurrentConfig / GetWindowSurface */
void *SDLOP_Wayland_EGL_GetDisplay(SDLop_VideoDevice *device)
{
    (void)device;
    return egl_initialized ? (void *)egl_display : NULL;
}

void *SDLOP_Wayland_EGL_GetConfig(SDLop_VideoDevice *device)
{
    (void)device;
    WaylandGLContext *cur = (WaylandGLContext *)SDLOP_GL_CurrentContext();
    if (!egl_initialized || !cur) {
        return NULL;
    }
    return (void *)config_for_context(cur);
}

void *SDLOP_Wayland_EGL_GetWindowSurface(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (!window || SDLOP_Wayland_IsGLOffloadActive(window)) {
        return NULL;
    }
    void **slot = SDLOP_Wayland_GetGLSurfaceSlot(window);
    return slot ? *slot : NULL;
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

void SDLOP_Wayland_GL_Deinit(void)
{
    wayland_native_gl_deinit();
    SDLOP_GLOffload_Shutdown();
    wayland_offload_decided = false;
    wayland_use_offload = false;
    swap_interval = 0;
}
