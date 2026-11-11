/*
  SDLop - KMS/DRM OpenGL support via GBM + EGL.

  A port of SDL3's kmsdrmopengles.c merged with SDLop's wayland EGL style
  (dlopen-ed libEGL, attributes from sdlop_glattrs, config lookup helpers).
  Software apps that never call SDL_GL_CreateContext never touch this file -
  they use the zero-copy dumb-buffer path in sdlop_kmsdrm.c.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_kmsdrm.h"

#include "../../video/wayland/sdlop_egl_dyn.h"

#include <errno.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif
#ifndef EGL_CONTEXT_MAJOR_VERSION_KHR
#define EGL_CONTEXT_MAJOR_VERSION_KHR 0x3098
#endif
#ifndef EGL_CONTEXT_MINOR_VERSION_KHR
#define EGL_CONTEXT_MINOR_VERSION_KHR 0x30FB
#endif
#ifndef EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR
#define EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR 0x30FD
#endif
#ifndef EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR 0x00000001
#endif
#ifndef EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR
#define EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR 0x00000002
#endif
#ifndef EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR
#define EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR 0x00000001
#endif
#ifndef EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR
#define EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR 0x00000002
#endif

typedef struct kmsdrm_gl_context
{
    EGLContext ctx;
    uint32_t api; /* EGL_OPENGL_API or EGL_OPENGL_ES_API */
} KMSDRMGLContext;

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static bool egl_initialized;
static int swap_interval = 1; /* vsync by default (SDL3 behavior) */
static uint32_t kmsdrm_gl_visual_id = GBM_FORMAT_ARGB8888; /* must match the GBM surface format */

/* the single device instance lives in sdlop_kmsdrm.c; reach it through a
 * small accessor instead of extern-ing the static */
KMSDRM_DeviceData *KMSDRM_GetDevice(void);

static KMSDRM_DeviceData *vid(void)
{
    return KMSDRM_GetDevice();
}

static bool is_permission_error(int err)
{
    return err == EPERM || err == EACCES;
}

static bool ensure_gbm(KMSDRM_DeviceData *dev)
{
    if (dev->gbm_init && dev->gbm_dev) {
        return true;
    }
    if (!SDLOP_GBM_LoadSymbols()) {
        return SDL_SetError("libgbm not available (GL on KMS requires it)");
    }
    if (dev->drm_fd < 0) {
        return SDL_SetError("DRM device not open");
    }
    if (SDLOP_DRM_drmSetMaster) {
        SDLOP_DRM_drmSetMaster(dev->drm_fd);
    }
    dev->gbm_dev = SDLOP_GBM_gbm_create_device(dev->drm_fd);
    if (!dev->gbm_dev) {
        return SDL_SetError("Couldn't create GBM device");
    }
    dev->gbm_init = true;
    uint32_t usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
    if (SDLOP_GBM_gbm_device_is_format_supported(dev->gbm_dev, GBM_FORMAT_ARGB8888, usage)) {
        kmsdrm_gl_visual_id = GBM_FORMAT_ARGB8888;
    } else if (SDLOP_GBM_gbm_device_is_format_supported(dev->gbm_dev, GBM_FORMAT_XRGB8888, usage)) {
        kmsdrm_gl_visual_id = GBM_FORMAT_XRGB8888;
    } else {
        kmsdrm_gl_visual_id = GBM_FORMAT_ARGB8888; /* create may still work; warn there */
    }
    return true;
}

static bool ensure_egl_display(void)
{
    if (egl_initialized) {
        return true;
    }
    if (!SDLOP_EGL_LoadSymbols()) {
        return false;
    }
    KMSDRM_DeviceData *dev = vid();
    if (!ensure_gbm(dev)) {
        return false;
    }

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)SDLOP_EGL_eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display) {
        egl_display = get_platform_display(EGL_PLATFORM_GBM_MESA, dev->gbm_dev, NULL);
    }
    if (egl_display == EGL_NO_DISPLAY) {
        egl_display = SDLOP_EGL_eglGetDisplay((EGLNativeDisplayType)(uintptr_t)dev->gbm_dev);
    }
    if (egl_display == EGL_NO_DISPLAY) {
        return SDL_SetError("eglGetPlatformDisplay(GBM) failed");
    }
    EGLint major = 0, minor = 0;
    if (!SDLOP_EGL_eglInitialize(egl_display, &major, &minor)) {
        egl_display = EGL_NO_DISPLAY;
        return SDL_SetError("eglInitialize failed: 0x%x", SDLOP_EGL_eglGetError());
    }
    egl_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop EGL %d.%d on KMS/GBM (%s)",
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

    /* Mesa's GBM platform silently ignores EGL_NATIVE_VISUAL_ID in the
     * eglChooseConfig attribute list, so passing it there does not narrow
     * the result (observed: all 60 configs returned regardless). Request the
     * rest, then filter manually on EGL_NATIVE_VISUAL_ID below -- the same
     * two-step SDL3's SDL_EGL_ChooseConfig does. */
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

    EGLConfig configs[128];
    EGLint num = 0;
    if (!SDLOP_EGL_eglChooseConfig(egl_display, attribs, configs, 128, &num) || num == 0) {
        return SDL_SetError("eglChooseConfig found no matching config");
    }
    /* keep only configs whose native visual id equals the GBM surface format
     * kmsdrm_gl_create_surfaces() is about to use; mixing another visual into
     * eglCreateWindowSurface() fails with EGL_BAD_MATCH on Mesa */
    int best = -1;
    int32_t best_diff = INT32_MAX;
    for (int i = 0; i < num; i++) {
        EGLint visual = 0;
        if (!SDLOP_EGL_eglGetConfigAttrib(egl_display, configs[i], EGL_NATIVE_VISUAL_ID, &visual)) {
            continue;
        }
        if ((uint32_t)visual != kmsdrm_gl_visual_id) {
            continue;
        }
        /* prefer the closest match on color/depth/stencil sizes */
        int32_t diff = 0;
        static const EGLint size_attribs[] = {
            EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE, EGL_ALPHA_SIZE,
            EGL_DEPTH_SIZE, EGL_STENCIL_SIZE
        };
        const int size_values[] = {
            sdlop_glattrs.red_size, sdlop_glattrs.green_size, sdlop_glattrs.blue_size,
            sdlop_glattrs.alpha_size, sdlop_glattrs.depth_size, sdlop_glattrs.stencil_size
        };
        bool sizes_ok = true;
        for (size_t j = 0; j < sizeof(size_attribs) / sizeof(size_attribs[0]); j++) {
            EGLint value = 0;
            if (!SDLOP_EGL_eglGetConfigAttrib(egl_display, configs[i], size_attribs[j], &value)) {
                sizes_ok = false;
                break;
            }
            diff += value - size_values[j]; /* values are >= attribs */
        }
        if (!sizes_ok) {
            continue;
        }
        if (best == -1 || diff < best_diff) {
            best = i;
            best_diff = diff;
        }
    }
    if (best == -1) {
        return SDL_SetError("eglChooseConfig: no config with native visual 0x%08x",
                            (unsigned)kmsdrm_gl_visual_id);
    }
    *out = configs[best];
    return true;
}

static EGLConfig config_for_context(KMSDRMGLContext *glctx)
{
    EGLConfig config;
    EGLint id = 0;
    if (SDLOP_EGL_eglQueryContext(egl_display, glctx->ctx, EGL_CONFIG_ID, &id)) {
        EGLint attribs[3] = { EGL_CONFIG_ID, id, EGL_NONE };
        EGLConfig configs[1];
        EGLint num = 0;
        if (SDLOP_EGL_eglChooseConfig(egl_display, attribs, configs, 1, &num) && num == 1) {
            return configs[0];
        }
    }
    return pick_config(&config, glctx->api) ? config : NULL;
}

/* Create the GBM surface + EGL surface for a GL window (mode-sized). */
static bool kmsdrm_gl_create_surfaces(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    KMSDRM_DeviceData *dev = vid();
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    KMSDRM_DisplayData *dd = windata->display;
    uint32_t w = dd->mode.hdisplay, h = dd->mode.vdisplay;

    uint32_t fmt = kmsdrm_gl_visual_id;
    uint32_t flags = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;

    if (!SDLOP_GBM_gbm_device_is_format_supported(dev->gbm_dev, fmt, flags)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                    "KMSDRM: GBM format 0x%08x not supported for scanout+rendering, trying anyway",
                    fmt);
    }

    windata->gs = SDLOP_GBM_gbm_surface_create(dev->gbm_dev, w, h, fmt, flags);
    if (!windata->gs) {
        return SDL_SetError("gbm_surface_create failed");
    }

    /* need a config bound to the context before making the surface current */
    SDL_GLContext current = SDLOP_GL_CurrentContext();
    EGLConfig config = NULL;
    if (current) {
        config = config_for_context((KMSDRMGLContext *)current);
    }
    if (!config) {
        uint32_t api = (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_CORE) ||
                       ((sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_ES) == 0 && sdlop_glattrs.major_version >= 3)
                           ? EGL_OPENGL_API : EGL_OPENGL_ES_API;
        if (!pick_config(&config, api)) {
            SDLOP_GBM_gbm_surface_destroy(windata->gs);
            windata->gs = NULL;
            return false;
        }
    }

    windata->egl_surface = SDLOP_EGL_eglCreateWindowSurface(egl_display, config,
                                                            (EGLNativeWindowType)windata->gs, NULL);
    if (windata->egl_surface == EGL_NO_SURFACE) {
        SDLOP_GBM_gbm_surface_destroy(windata->gs);
        windata->gs = NULL;
        return SDL_SetError("eglCreateWindowSurface failed: 0x%x", SDLOP_EGL_eglGetError());
    }

    windata->egl_surface_dirty = false;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, (int)w, (int)h);
    return true;
}

/* Destroy EGL/GBM scanout resources for a window (context survives). */
void KMSDRM_DestroyGLSurfaces(KMSDRM_WindowData *windata)
{
    if (egl_initialized) {
        SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (windata->egl_surface && windata->egl_surface != EGL_NO_SURFACE) {
        if (egl_initialized) {
            SDLOP_EGL_eglDestroySurface(egl_display, windata->egl_surface);
        }
        windata->egl_surface = EGL_NO_SURFACE;
    }
    if (windata->bo) {
        if (windata->gs) {
            SDLOP_GBM_gbm_surface_release_buffer(windata->gs, windata->bo);
        }
        windata->bo = NULL;
    }
    if (windata->next_bo) {
        if (windata->gs) {
            SDLOP_GBM_gbm_surface_release_buffer(windata->gs, windata->next_bo);
        }
        windata->next_bo = NULL;
    }
    if (windata->gs) {
        SDLOP_GBM_gbm_surface_destroy(windata->gs);
        windata->gs = NULL;
    }
    windata->gl_active = false;
    windata->crtc_programmed = false;
}

void *KMSDRM_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    KMSDRM_DeviceData *dev = vid();
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    if (!windata) {
        SDL_SetError("Invalid window");
        return NULL;
    }

    /* switch the window from the software dumb-buffer scanout to GL */
    if (windata->sw_active) {
        /* release the software scanout: GL owns the CRTC from now on */
        KMSDRM_DestroySwScanout(window);
        SDL_DestroyWindowSurface(window);
    }
    windata->gl_active = true;

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

    EGLint ctx_attribs[16];
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

    EGLContext share = EGL_NO_CONTEXT;
    if (sdlop_glattrs.share_with_current_context) {
        SDL_GLContext cur = SDLOP_GL_CurrentContext();
        if (cur) {
            share = ((KMSDRMGLContext *)cur)->ctx;
        }
    }
    EGLContext ctx = SDLOP_EGL_eglCreateContext(egl_display, config, share, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) {
        return (SDL_SetError("eglCreateContext failed: 0x%x", SDLOP_EGL_eglGetError()), NULL);
    }

    KMSDRMGLContext *glctx = (KMSDRMGLContext *)calloc(1, sizeof(*glctx));
    if (!glctx) {
        SDLOP_EGL_eglDestroyContext(egl_display, ctx);
        SDL_OutOfMemory();
        return NULL;
    }
    glctx->ctx = ctx;
    glctx->api = api;

    /* create the scanout + EGL surfaces now so the first swap works */
    if (!kmsdrm_gl_create_surfaces(device, window)) {
        SDLOP_EGL_eglDestroyContext(egl_display, ctx);
        free(glctx);
        return NULL;
    }

    /* bind immediately, like the wayland driver does */
    if (!KMSDRM_GL_MakeCurrent(device, window, glctx)) {
        KMSDRM_DestroyGLSurfaces(windata);
        SDLOP_EGL_eglDestroyContext(egl_display, ctx);
        free(glctx);
        return NULL;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop KMS/GBM: GL context created (%s)",
                api == EGL_OPENGL_API ? "desktop GL" : "GLES");
    return glctx;
}

bool KMSDRM_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context)
{
    (void)device;
    if (!egl_initialized && !ensure_egl_display()) {
        return false;
    }
    if (!context) {
        SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return true;
    }
    KMSDRMGLContext *glctx = (KMSDRMGLContext *)context;
    if (!SDLOP_EGL_eglBindAPI(glctx->api)) {
        return SDL_SetError("eglBindAPI failed: 0x%x", SDLOP_EGL_eglGetError());
    }

    EGLSurface surface = EGL_NO_SURFACE;
    if (window) {
        KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
        if (!windata) {
            return SDL_SetError("Invalid window");
        }
        if (windata->egl_surface_dirty || !windata->gs) {
            KMSDRM_DestroyGLSurfaces(windata);
            windata->gl_active = true;
            if (!kmsdrm_gl_create_surfaces(device, window)) {
                return false;
            }
        }
        if (windata->egl_surface == EGL_NO_SURFACE) {
            /* surface was destroyed elsewhere; recreate lazily */
            if (!kmsdrm_gl_create_surfaces(device, window)) {
                return false;
            }
        }
        surface = windata->egl_surface;
    }

    if (!SDLOP_EGL_eglMakeCurrent(egl_display, surface, surface, glctx->ctx)) {
        return SDL_SetError("eglMakeCurrent failed: 0x%x", SDLOP_EGL_eglGetError());
    }
    SDLOP_EGL_eglSwapInterval(egl_display, swap_interval);
    return true;
}

bool KMSDRM_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window)
{
    KMSDRM_DeviceData *dev = vid();
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    KMSDRM_DisplayData *dd = windata ? windata->display : NULL;
    if (!windata || !dd) {
        return SDL_SetError("Invalid window");
    }

    /* switched away from this VT, or the mode changed: (re)create the
     * GBM/EGL scanout surfaces for the current mode */
    if (windata->egl_surface_dirty || !windata->gs || windata->egl_surface == EGL_NO_SURFACE) {
        KMSDRM_DestroyGLSurfaces(windata);
        windata->gl_active = true;
        if (!kmsdrm_gl_create_surfaces(device, window)) {
            return false;
        }
        /* rebind the current context to the fresh surface */
        SDL_GLContext cur = SDLOP_GL_CurrentContext();
        if (cur && !KMSDRM_GL_MakeCurrent(device, window, cur)) {
            return false;
        }
    }

    uint32_t flip_flags = DRM_MODE_PAGE_FLIP_EVENT;

    /* wait for the previously issued flip before touching front buffers */
    if (!KMSDRM_WaitPageflip(dev, windata, 1500)) {
        return SDL_SetError("Wait for previous pageflip failed");
    }

    /* release the previous front buffer: it can now be reused as back buffer */
    if (windata->bo) {
        SDLOP_GBM_gbm_surface_release_buffer(windata->gs, windata->bo);
        windata->bo = NULL;
    }
    windata->bo = windata->next_bo;

    if (!SDLOP_EGL_eglSwapBuffers(egl_display, windata->egl_surface)) {
        return SDL_SetError("eglSwapBuffers failed: 0x%x", SDLOP_EGL_eglGetError());
    }

    windata->next_bo = SDLOP_GBM_gbm_surface_lock_front_buffer(windata->gs);
    if (!windata->next_bo) {
        return SDL_SetError("Could not lock front buffer on GBM surface");
    }

    KMSDRM_FBInfo *fb_info = KMSDRM_FBFromBO(dev->drm_fd, windata->next_bo);
    if (!fb_info) {
        return SDL_SetError("Could not get a framebuffer");
    }

    int ret = 0;
    if (!windata->bo) {
        /* first swap: the CRTC must be told what to scan out */
        ret = SDLOP_DRM_drmModeSetCrtc(dev->drm_fd, dd->crtc->crtc_id, fb_info->fb_id, 0, 0,
                                       &dd->connector->connector_id, 1, &dd->mode);
        if (ret) {
            return SDL_SetError("Could not set videomode on CRTC");
        }
        windata->crtc_programmed = true;
    } else {
        if (swap_interval == 0 && dev->async_pageflip_support) {
            flip_flags |= DRM_MODE_PAGE_FLIP_ASYNC;
        }
        ret = SDLOP_DRM_drmModePageFlip(dev->drm_fd, dd->crtc->crtc_id, fb_info->fb_id,
                                        flip_flags, &windata->waiting_for_flip);
        if (ret == 0) {
            windata->waiting_for_flip = true;
        } else {
            int err = errno;
            if (err == EBUSY && KMSDRM_WaitPageflip(dev, windata, 1500)) {
                ret = SDLOP_DRM_drmModePageFlip(dev->drm_fd, dd->crtc->crtc_id, fb_info->fb_id,
                                                flip_flags, &windata->waiting_for_flip);
                if (ret == 0) {
                    windata->waiting_for_flip = true;
                    ret = 0;
                }
            }
            if (ret != 0) {
                if (is_permission_error(err)) {
                    windata->lost_master = true;
                    SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: pageflip lost DRM master");
                } else if (err == EINVAL || err == ENOSYS || err == EBUSY) {
                    /* this driver cannot page flip right now (e.g. virtio-gpu
                     * 2D: the flip ioctl always returns EBUSY, even with a
                     * flip event requested), so present with a modeset like
                     * the software path does instead of leaving the CRTC on a
                     * stale framebuffer */
                    if (err == EINVAL || err == ENOSYS) {
                        dev->no_flip = true;
                        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                                    "KMSDRM: pageflip unsupported (%s); using modeset for GL present",
                                    strerror(err));
                    }
                    ret = SDLOP_DRM_drmModeSetCrtc(dev->drm_fd, dd->crtc->crtc_id, fb_info->fb_id,
                                                   0, 0, &dd->connector->connector_id, 1, &dd->mode);
                    if (ret == 0) {
                        windata->crtc_programmed = true;
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                                     "KMSDRM: pageflip failed and modeset fallback failed: %s",
                                     strerror(errno));
                    }
                } else {
                    SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: pageflip failed: %s", strerror(err));
                }
            }
        }
    }

    return true;
}

void KMSDRM_GL_DeleteContext(SDLop_VideoDevice *device, void *context)
{
    (void)device;
    KMSDRMGLContext *glctx = (KMSDRMGLContext *)context;
    if (!glctx) {
        return;
    }
    if (egl_initialized) {
        SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        SDLOP_EGL_eglDestroyContext(egl_display, glctx->ctx);
    }
    free(glctx);
}

SDL_FunctionPointer KMSDRM_GL_GetProcAddress(SDLop_VideoDevice *device, const char *proc)
{
    (void)device;
    if (!egl_initialized && !ensure_egl_display()) {
        return NULL;
    }
    SDL_FunctionPointer fn = SDLOP_EGL_eglGetProcAddress(proc);
    if (fn) {
        return fn;
    }
    return (SDL_FunctionPointer)dlsym(RTLD_DEFAULT, proc);
}

bool KMSDRM_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval)
{
    (void)device;
    if (interval < 0 || interval > 1) {
        return SDL_SetError("Only swap intervals 0 and 1 are supported");
    }
    swap_interval = interval;
    if (egl_initialized) {
        SDLOP_EGL_eglSwapInterval(egl_display, interval);
    }
    return true;
}

bool KMSDRM_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval)
{
    (void)device;
    *interval = swap_interval;
    return true;
}

/* used by the driver core when the LAST GL window is destroyed */
void KMSDRM_GL_Deinit(void)
{
    if (egl_initialized) {
        SDLOP_EGL_eglTerminate(egl_display);
        egl_initialized = false;
        egl_display = EGL_NO_DISPLAY;
        SDLOP_EGL_UnloadSymbols();
    }
    KMSDRM_DeviceData *dev = vid();
    if (dev->gbm_dev) {
        SDLOP_GBM_gbm_device_destroy(dev->gbm_dev);
        dev->gbm_dev = NULL;
        dev->gbm_init = false;
        SDLOP_GBM_UnloadSymbols();
    }
    swap_interval = 1;
}
