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

#include "../sdlop_egl_dyn.h"
#include "../sdlop_gl_offload.h"

#if SDLOP_HAVE_EGL

#include <errno.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef DRM_IOCTL_GEM_CLOSE
struct sdlop_drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};
#define SDLOP_DRM_IOCTL_GEM_CLOSE _IOW('d', 0x09, struct sdlop_drm_gem_close)
#endif

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
    bool is_offload;
    SDLOP_GLOffloadContext *offload_ctx;
} KMSDRMGLContext;

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static bool egl_initialized;
static int swap_interval = 1; /* vsync by default (SDL3 behavior) */
static bool kmsdrm_offload_decided;
static bool kmsdrm_use_offload;
static bool kmsdrm_prime_disabled;
static void kmsdrm_native_gl_deinit(void);

typedef struct KMSDRM_PrimeFBInfo
{
    int drm_fd;
    uint32_t fb_id;
    uint32_t gem_handle;
} KMSDRM_PrimeFBInfo;

static void kmsdrm_close_gem_handle(int drm_fd, uint32_t handle)
{
    if (drm_fd < 0 || handle == 0) {
        return;
    }
    if (SDLOP_DRM_OPT_drmCloseBufferHandle) {
        SDLOP_DRM_OPT_drmCloseBufferHandle(drm_fd, handle);
        return;
    }
#ifdef DRM_IOCTL_GEM_CLOSE
    struct drm_gem_close args = { .handle = handle, .pad = 0 };
    ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &args);
#else
    struct sdlop_drm_gem_close args = { .handle = handle, .pad = 0 };
    ioctl(drm_fd, SDLOP_DRM_IOCTL_GEM_CLOSE, &args);
#endif
}

static void kmsdrm_prime_fb_destroy_cb(struct gbm_bo *bo, void *data)
{
    (void)bo;
    KMSDRM_PrimeFBInfo *info = (KMSDRM_PrimeFBInfo *)data;
    if (!info) {
        return;
    }
    if (info->fb_id && SDLOP_DRM_drmModeRmFB) {
        SDLOP_DRM_drmModeRmFB(info->drm_fd, info->fb_id);
    }
    kmsdrm_close_gem_handle(info->drm_fd, info->gem_handle);
    free(info);
}

static KMSDRM_PrimeFBInfo *kmsdrm_prime_fb_from_bo(int drm_fd, struct gbm_bo *bo)
{
    if (drm_fd < 0 || !bo || !SDLOP_GBM_Available() ||
        !SDLOP_GBM_gbm_bo_get_fd || !SDLOP_DRM_OPT_drmPrimeFDToHandle ||
        !SDLOP_DRM_drmModeAddFB2) {
        return NULL;
    }

    KMSDRM_PrimeFBInfo *info = (KMSDRM_PrimeFBInfo *)SDLOP_GBM_gbm_bo_get_user_data(bo);
    if (info) {
        return info;
    }

    int prime_fd = SDLOP_GBM_gbm_bo_get_fd(bo);
    if (prime_fd < 0) {
        SDL_SetError("gbm_bo_get_fd failed");
        return NULL;
    }

    uint32_t gem_handle = 0;
    int rc = SDLOP_DRM_OPT_drmPrimeFDToHandle(drm_fd, prime_fd, &gem_handle);
    close(prime_fd);
    if (rc != 0 || gem_handle == 0) {
        SDL_SetError("drmPrimeFDToHandle failed: %s", strerror(errno));
        return NULL;
    }

    uint32_t w = SDLOP_GBM_gbm_bo_get_width(bo);
    uint32_t h = SDLOP_GBM_gbm_bo_get_height(bo);
    uint32_t format = SDLOP_GBM_gbm_bo_get_format(bo);
    uint32_t stride = SDLOP_GBM_gbm_bo_get_stride_for_plane
        ? SDLOP_GBM_gbm_bo_get_stride_for_plane(bo, 0)
        : SDLOP_GBM_gbm_bo_get_stride(bo);
    uint32_t offset = SDLOP_GBM_gbm_bo_get_offset
        ? SDLOP_GBM_gbm_bo_get_offset(bo, 0)
        : 0;

    uint32_t handles[4] = { gem_handle, 0, 0, 0 };
    uint32_t pitches[4] = { stride, 0, 0, 0 };
    uint32_t offsets[4] = { offset, 0, 0, 0 };
    uint32_t fb_id = 0;
    rc = SDLOP_DRM_drmModeAddFB2(drm_fd, w, h, format, handles, pitches, offsets, &fb_id, 0);
    if (rc != 0 || fb_id == 0) {
        int err = errno;
        kmsdrm_close_gem_handle(drm_fd, gem_handle);
        SDL_SetError("drmModeAddFB2(0x%08x) failed: %s", format, strerror(err));
        return NULL;
    }

    info = (KMSDRM_PrimeFBInfo *)calloc(1, sizeof(*info));
    if (!info) {
        SDLOP_DRM_drmModeRmFB(drm_fd, fb_id);
        kmsdrm_close_gem_handle(drm_fd, gem_handle);
        return NULL;
    }
    info->drm_fd = drm_fd;
    info->fb_id = fb_id;
    info->gem_handle = gem_handle;
    SDLOP_GBM_gbm_bo_set_user_data(bo, info, kmsdrm_prime_fb_destroy_cb);
    return info;
}

static bool kmsdrm_can_try_prime(KMSDRM_DeviceData *dev)
{
    if (kmsdrm_prime_disabled || !dev || dev->drm_fd < 0) {
        return false;
    }
    if (!SDLOP_HintFlag(SDLOP_HINT_GL_OFFLOAD_PRIME, true)) {
        return false;
    }
    if (!SDLOP_GLOffload_HasGBM() || !SDLOP_GBM_Available() ||
        !SDLOP_GBM_gbm_bo_get_fd || !SDLOP_DRM_OPT_drmPrimeFDToHandle ||
        !SDLOP_DRM_drmModeAddFB2) {
        return false;
    }
    uint64_t prime_cap = 0;
    if (SDLOP_DRM_drmGetCap(dev->drm_fd, DRM_CAP_PRIME, &prime_cap) != 0) {
        return false;
    }
    return (prime_cap & DRM_PRIME_CAP_IMPORT) != 0;
}

static void kmsdrm_release_prime_bos(KMSDRM_WindowData *windata)
{
    if (!windata) {
        return;
    }
    SDLOP_GLOffloadSurface *osurf = (SDLOP_GLOffloadSurface *)windata->gl_offload_surface;
    if (windata->prime_bo) {
        if (osurf) {
            SDLOP_GLOffload_ReleasePrimeBO(osurf, windata->prime_bo);
        }
        windata->prime_bo = NULL;
    }
    if (windata->prime_next_bo) {
        if (osurf) {
            SDLOP_GLOffload_ReleasePrimeBO(osurf, windata->prime_next_bo);
        }
        windata->prime_next_bo = NULL;
    }
}

/* SDL_HINT_VIDEO_DOUBLE_BUFFER equivalent: wait for the flip right after
 * queueing it (matches sdl's optional immediate wait; the software path is
 * always synchronous) */
static bool kmsdrm_double_buffer(void)
{
    return SDLOP_HintFlag("SDLOP_KMSDRM_DOUBLE_BUFFER", false);
}
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

/* The scanout-format ladder for the GL GBM surface (SDL3-era breadth):
 * one try per entry, first GBM-and-plane-supported format wins. Starts
 * with the common 32-bit formats, then 10-bit, 565/1555/4444/5551,
 * 24-bit RGB and 16-bit-per-channel integer. Every entry is a single-plane
 * packed RGB format; planar YUV formats are not GL render targets. */
static const uint32_t scanout_format_ladder[] = {
    /* 32-bit */
    GBM_FORMAT_XRGB8888, GBM_FORMAT_ARGB8888, GBM_FORMAT_XBGR8888, GBM_FORMAT_ABGR8888,
    GBM_FORMAT_RGBX8888, GBM_FORMAT_RGBA8888, GBM_FORMAT_BGRX8888, GBM_FORMAT_BGRA8888,
    /* 10-bit (two common layouts) */
    GBM_FORMAT_XRGB2101010, GBM_FORMAT_ARGB2101010, GBM_FORMAT_XBGR2101010, GBM_FORMAT_ABGR2101010,
    GBM_FORMAT_RGBX1010102, GBM_FORMAT_RGBA1010102, GBM_FORMAT_BGRX1010102, GBM_FORMAT_BGRA1010102,
    /* 565 */
    GBM_FORMAT_RGB565, GBM_FORMAT_BGR565,
    /* 1555 */
    GBM_FORMAT_ARGB1555, GBM_FORMAT_ABGR1555, GBM_FORMAT_XRGB1555, GBM_FORMAT_XBGR1555,
    /* 5551 */
    GBM_FORMAT_RGBA5551, GBM_FORMAT_BGRA5551, GBM_FORMAT_RGBX5551, GBM_FORMAT_BGRX5551,
    /* 4444 */
    GBM_FORMAT_ARGB4444, GBM_FORMAT_ABGR4444, GBM_FORMAT_XRGB4444, GBM_FORMAT_XBGR4444,
    GBM_FORMAT_RGBA4444, GBM_FORMAT_BGRA4444, GBM_FORMAT_RGBX4444, GBM_FORMAT_BGRX4444,
    /* 24-bit RGB */
    GBM_FORMAT_RGB888, GBM_FORMAT_BGR888,
    /* 332 */
    GBM_FORMAT_RGB332, GBM_FORMAT_BGR233,
    /* 16-bit per channel integer (not float) */
    GBM_FORMAT_XBGR16161616, GBM_FORMAT_ABGR16161616,
};

/* cached union of plane fourccs for the GL pick; the KMS device does not
 * change once the driver is up */
static uint32_t plane_union[128];
static size_t plane_union_count;
static bool plane_union_known;
static bool plane_union_fetched;

static void plane_union_load(int fd)
{
    if (plane_union_fetched) {
        return;
    }
    plane_union_fetched = true;
    plane_union_known = KMSDRM_PlaneFormatsUnionList(fd, plane_union,
                                                     sizeof(plane_union) / sizeof(plane_union[0]),
                                                     &plane_union_count);
    if (!plane_union_known) {
        plane_union_count = 0;
    }
}

static bool plane_accepts(uint32_t fourcc)
{
    if (!plane_union_known) {
        return true; /* no plane info: let GBM decide (legacy behavior) */
    }
    for (size_t i = 0; i < plane_union_count; ++i) {
        if (plane_union[i] == fourcc) {
            return true;
        }
    }
    return false;
}

/* channel sizes for the chosen format, reported to the app as the default
 * GL attributes */
static void format_channel_sizes(uint32_t fmt, int *r, int *g, int *b, int *a)
{
    switch (fmt) {
    case GBM_FORMAT_RGB565:
    case GBM_FORMAT_BGR565:
        *r = 5; *g = 6; *b = 5; *a = 0;
        break;
    case GBM_FORMAT_ARGB1555:
    case GBM_FORMAT_ABGR1555:
        *r = 5; *g = 5; *b = 5; *a = 1;
        break;
    case GBM_FORMAT_XRGB1555:
    case GBM_FORMAT_XBGR1555:
        *r = 5; *g = 5; *b = 5; *a = 0;
        break;
    case GBM_FORMAT_RGBA5551:
    case GBM_FORMAT_BGRA5551:
        *r = 5; *g = 5; *b = 5; *a = 1;
        break;
    case GBM_FORMAT_RGBX5551:
    case GBM_FORMAT_BGRX5551:
        *r = 5; *g = 5; *b = 5; *a = 0;
        break;
    case GBM_FORMAT_ARGB4444:
    case GBM_FORMAT_ABGR4444:
    case GBM_FORMAT_RGBA4444:
    case GBM_FORMAT_BGRA4444:
        *r = 4; *g = 4; *b = 4; *a = 4;
        break;
    case GBM_FORMAT_XRGB4444:
    case GBM_FORMAT_XBGR4444:
    case GBM_FORMAT_RGBX4444:
    case GBM_FORMAT_BGRX4444:
        *r = 4; *g = 4; *b = 4; *a = 0;
        break;
    case GBM_FORMAT_RGB332:
    case GBM_FORMAT_BGR233:
        *r = 3; *g = 3; *b = 2; *a = 0;
        break;
    case GBM_FORMAT_XRGB2101010:
    case GBM_FORMAT_XBGR2101010:
    case GBM_FORMAT_RGBX1010102:
    case GBM_FORMAT_BGRX1010102:
        *r = 10; *g = 10; *b = 10; *a = 0;
        break;
    case GBM_FORMAT_ARGB2101010:
    case GBM_FORMAT_ABGR2101010:
    case GBM_FORMAT_RGBA1010102:
    case GBM_FORMAT_BGRA1010102:
        *r = 10; *g = 10; *b = 10; *a = 2;
        break;
    case GBM_FORMAT_XBGR16161616:
        *r = 16; *g = 16; *b = 16; *a = 0;
        break;
    case GBM_FORMAT_ABGR16161616:
        *r = 16; *g = 16; *b = 16; *a = 16;
        break;
    case GBM_FORMAT_ARGB8888:
    case GBM_FORMAT_ABGR8888:
    case GBM_FORMAT_RGBA8888:
    case GBM_FORMAT_BGRA8888:
        *r = 8; *g = 8; *b = 8; *a = 8;
        break;
    default: /* XRGB8888 and friends, RGB888/BGR888: no alpha */
        *r = 8; *g = 8; *b = 8; *a = 0;
        break;
    }
}

static bool ensure_gbm(KMSDRM_DeviceData *dev)
{
    if (!SDLOP_GBM_LoadSymbols()) {
        return SDL_SetError("libgbm not available (GL on KMS requires it)");
    }
    if (dev->drm_fd < 0) {
        return SDL_SetError("DRM device not open");
    }
    /* The driver may already have created the shared GBM device for the
     * cursor path; the GL visual choice below is still ours to make and is
     * idempotent, so it runs on every call instead of only on creation. */
    if (!dev->gbm_dev) {
        if (SDLOP_DRM_drmSetMaster) {
            SDLOP_DRM_drmSetMaster(dev->drm_fd);
        }
        dev->gbm_dev = SDLOP_GBM_gbm_create_device(dev->drm_fd);
        if (!dev->gbm_dev) {
            return SDL_SetError("Couldn't create GBM device");
        }
        dev->gbm_init = true;
    }
    {
        uint32_t usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
        /* GBM does not know what the KMS planes can scan out. Plane format
         * lists are authoritative (e.g. virtio-gpu 2D's primary plane is
         * XR24-only), so consult them first; without plane info prefer
         * XRGB8888, which every scanout plane supporting 32-bit formats does. */
        bool gbm_xrgb = SDLOP_GBM_gbm_device_is_format_supported(dev->gbm_dev, GBM_FORMAT_XRGB8888, usage);
        bool gbm_argb = SDLOP_GBM_gbm_device_is_format_supported(dev->gbm_dev, GBM_FORMAT_ARGB8888, usage);
        uint32_t plane_xrgb = 0, plane_argb = 0;
        bool have_planes = KMSDRM_PlaneFormatsUnion(dev->drm_fd, &plane_xrgb, &plane_argb);
        static bool format_logged;
        if (have_planes && plane_xrgb && gbm_xrgb) {
            kmsdrm_gl_visual_id = GBM_FORMAT_XRGB8888;
        } else if (have_planes && plane_argb && gbm_argb) {
            kmsdrm_gl_visual_id = GBM_FORMAT_ARGB8888;
        } else if (gbm_xrgb) {
            kmsdrm_gl_visual_id = GBM_FORMAT_XRGB8888;
        } else if (gbm_argb) {
            kmsdrm_gl_visual_id = GBM_FORMAT_ARGB8888;
        } else {
            kmsdrm_gl_visual_id = GBM_FORMAT_ARGB8888; /* create may still work; warn there */
        }
        if (!format_logged) {
            format_logged = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "KMSDRM: GL scanout format 0x%08x (planes: %s)",
                        kmsdrm_gl_visual_id,
                        have_planes ? (plane_xrgb ? "XR24" : (plane_argb ? "AR24-only" : "no 32-bit")) : "unknown");
        }
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

    egl_display = SDLOP_EGL_PlatformDisplay(EGL_PLATFORM_GBM_MESA, dev->gbm_dev, NULL);
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
    } else {
        renderable = SDLOP_EGL_ESRenderableType(egl_display);
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

    const uint32_t scanout_flags = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;

    /* Prefer the plane-aware pick (see ensure_gbm), then fall back down a
     * full ladder of common scanout formats (SDL3 behavior, commit 68655e9,
     * extended to 10-bit/565/1555/4444/5551/24-bit/16-bit) -- a single
     * hardcoded format is not portable (XR24-only planes, NVIDIA).
     * The plane-format union (authoritative) filters the ladder.
     * The chosen format updates kmsdrm_gl_visual_id so the EGL config
     * lookup below binds to the right native visual. */
    plane_union_load(vid()->drm_fd);
    uint32_t candidates[sizeof(scanout_format_ladder) / sizeof(scanout_format_ladder[0]) + 1];
    int ncand = 0;
    candidates[ncand++] = kmsdrm_gl_visual_id;
    for (size_t i = 0; i < sizeof(scanout_format_ladder) / sizeof(scanout_format_ladder[0]); i++) {
        bool dup = false;
        for (int j = 0; j < ncand; j++) {
            if (candidates[j] == scanout_format_ladder[i]) {
                dup = true;
                break;
            }
        }
        if (!dup && plane_accepts(scanout_format_ladder[i])) {
            candidates[ncand++] = scanout_format_ladder[i];
        }
    }

    SDL_GLContext current = SDLOP_GL_CurrentContext();
    for (int ci = 0; ci < ncand; ci++) {
        uint32_t fmt = candidates[ci];
        if (!SDLOP_GBM_gbm_device_is_format_supported(dev->gbm_dev, fmt, scanout_flags)) {
            SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO,
                         "KMSDRM: GBM format 0x%08x not supported, trying next", fmt);
            continue;
        }
        windata->gs = SDLOP_GBM_gbm_surface_create(dev->gbm_dev, w, h, fmt, scanout_flags);
        if (!windata->gs && errno == ENOSYS) {
            /* NVIDIA: the scanout flag is rejected; retry without it (SDL3 behavior) */
            windata->gs = SDLOP_GBM_gbm_surface_create(dev->gbm_dev, w, h, fmt, 0);
        }
        if (!windata->gs) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                        "KMSDRM: gbm_surface_create(0x%08x) failed", fmt);
            continue;
        }
        kmsdrm_gl_visual_id = fmt;
        /* report the format's channel sizes to the app's GL attributes
         * (SDL3 sets gl_config from the chosen format too) */
        format_channel_sizes(fmt,
                             &sdlop_glattrs.red_size,
                             &sdlop_glattrs.green_size,
                             &sdlop_glattrs.blue_size,
                             &sdlop_glattrs.alpha_size);

        /* need a config bound to the context before making the surface current */
        EGLConfig config = NULL;
        if (current) {
            config = config_for_context((KMSDRMGLContext *)current);
        }
        if (!config) {
            /* GLES has no 2.1: a 2.1 (or higher) request without an ES profile
             * means desktop GL, like SDL2. Only bare 2.0 stays GLES. */
            uint32_t api = (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_CORE) ||
                           ((sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_ES) == 0 &&
                            (sdlop_glattrs.major_version > 2 ||
                             (sdlop_glattrs.major_version == 2 && sdlop_glattrs.minor_version > 0)))
                               ? EGL_OPENGL_API : EGL_OPENGL_ES_API;
            if (!pick_config(&config, api)) {
                SDLOP_GBM_gbm_surface_destroy(windata->gs);
                windata->gs = NULL;
                continue; /* the next format may match an EGL config */
            }
        }

        EGLint surf_attribs[64];
        if (SDLOP_EGL_AppendUserAttribs(false, egl_display, config, surf_attribs, 0,
                                        (int)SDL_arraysize(surf_attribs)) < 0) {
            SDLOP_GBM_gbm_surface_destroy(windata->gs);
            windata->gs = NULL;
            return false;
        }
        windata->egl_surface = SDLOP_EGL_eglCreateWindowSurface(egl_display, config,
                                                                (EGLNativeWindowType)windata->gs, surf_attribs);
        if (windata->egl_surface == EGL_NO_SURFACE) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                        "KMSDRM: eglCreateWindowSurface(0x%08x) failed: 0x%x",
                        fmt, SDLOP_EGL_eglGetError());
            SDLOP_GBM_gbm_surface_destroy(windata->gs);
            windata->gs = NULL;
            continue;
        }

        windata->egl_surface_dirty = false;
        KMSDRM_DeferEvent(window, KMSDRM_DEFER_SIZE);
        return true;
    }

    return SDL_SetError("KMSDRM: could not create a GBM/EGL scanout surface in any supported format");
}

/* Destroy EGL/GBM scanout resources for a window (context survives). */
void KMSDRM_DestroyGLSurfaces(KMSDRM_WindowData *windata)
{
    kmsdrm_release_prime_bos(windata);
    if (windata->gl_offload_surface) {
        SDLOP_GLOffload_DestroySurface((SDLOP_GLOffloadSurface *)windata->gl_offload_surface);
        windata->gl_offload_surface = NULL;
    }
    windata->gl_offload_active = false;
    windata->gl_offload_prime = false;
    windata->gl_offload_ctx = NULL;

    if (egl_initialized) {
        SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
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
    /* EGL surface goes after the GBM buffers are released: some drivers
     * crash destroying an EGL surface that still owns locked GBM buffers
     * (SDL3 commit 463b6be) */
    if (windata->egl_surface && windata->egl_surface != EGL_NO_SURFACE) {
        if (egl_initialized) {
            SDLOP_EGL_eglDestroySurface(egl_display, windata->egl_surface);
        }
        windata->egl_surface = EGL_NO_SURFACE;
    }
    if (windata->gs) {
        SDLOP_GBM_gbm_surface_destroy(windata->gs);
        windata->gs = NULL;
    }
    windata->gl_active = false;
    windata->crtc_programmed = false;
}

static void *kmsdrm_create_offload_context(SDLop_VideoDevice *device, SDL_Window *window, uint32_t api)
{
    KMSDRM_DeviceData *dev = vid();
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    KMSDRM_DisplayData *dd = windata ? windata->display : NULL;
    if (!windata || !dd) {
        SDL_SetError("Invalid window");
        return NULL;
    }

    SDLOP_GLOffloadContext *share_offload = NULL;
    if (sdlop_glattrs.share_with_current_context) {
        SDL_GLContext cur = SDLOP_GL_CurrentContext();
        if (cur && ((KMSDRMGLContext *)cur)->is_offload) {
            share_offload = ((KMSDRMGLContext *)cur)->offload_ctx;
        }
    }

    SDLOP_GLOffloadContext *octx = SDLOP_GLOffload_CreateContext(api, share_offload);
    if (!octx) {
        return NULL;
    }

    KMSDRMGLContext *glctx = (KMSDRMGLContext *)calloc(1, sizeof(*glctx));
    if (!glctx) {
        SDLOP_GLOffload_DeleteContext(octx);
        SDL_OutOfMemory();
        return NULL;
    }
    glctx->api = api;
    glctx->is_offload = true;
    glctx->offload_ctx = octx;

    SDLOP_GLOffloadSurface *osurf = NULL;
    bool using_prime = false;
    if (kmsdrm_can_try_prime(dev)) {
        osurf = SDLOP_GLOffload_CreatePrimeSurface(octx,
                                                   (int)dd->mode.hdisplay,
                                                   (int)dd->mode.vdisplay);
        if (osurf) {
            using_prime = true;
        }
    }

    if (using_prime) {
        if (windata->sw_active) {
            KMSDRM_DestroySwScanout(window);
            SDL_DestroyWindowSurface(window);
        }
    } else {
        /* Drop any cached software SDL_Surface wrapper without destroying dumb
         * buffers; if none exist yet, allocate dumb buffers for scanout. */
        if (window->surface) {
            SDL_DestroySurface(window->surface);
            window->surface = NULL;
        }
        if (!KMSDRM_EnsureSwScanout(window)) {
            SDLOP_GLOffload_DeleteContext(octx);
            free(glctx);
            return NULL;
        }
        osurf = SDLOP_GLOffload_CreateSurface(octx,
                                              (int)dd->mode.hdisplay,
                                              (int)dd->mode.vdisplay);
        if (!osurf) {
            SDLOP_GLOffload_DeleteContext(octx);
            free(glctx);
            return NULL;
        }
    }

    windata->gl_active = true;
    windata->gl_offload_active = true;
    windata->gl_offload_prime = using_prime;
    windata->gl_offload_surface = osurf;
    windata->gl_offload_ctx = octx;
    windata->egl_surface_dirty = false;

    if (!KMSDRM_GL_MakeCurrent(device, window, glctx)) {
        KMSDRM_DestroyGLSurfaces(windata);
        SDLOP_GLOffload_DeleteContext(octx);
        free(glctx);
        return NULL;
    }
    KMSDRM_DeferEvent(window, KMSDRM_DEFER_SIZE);
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop KMS/Offload: GL context created (%s on %s, %s)",
                api == EGL_OPENGL_API ? "desktop GL" : "GLES",
                SDLOP_GLOffload_GetGPUDescription() ? SDLOP_GLOffload_GetGPUDescription() : "GPU",
                using_prime ? "PRIME scanout" : "dumb-buffer readback");
    return glctx;
}

void *KMSDRM_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    if (!windata) {
        SDL_SetError("Invalid window");
        return NULL;
    }

    /* GLES has no 2.1: a 2.1 (or higher) request without an ES profile means
     * desktop GL, like SDL2. Only bare 2.0 stays GLES. */
    bool want_desktop_gl =
        (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_CORE) != 0 ||
        ((sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_ES) == 0 &&
         (sdlop_glattrs.major_version > 2 ||
          (sdlop_glattrs.major_version == 2 && sdlop_glattrs.minor_version > 0)));
    uint32_t api = want_desktop_gl ? EGL_OPENGL_API : EGL_OPENGL_ES_API;

    SDLOP_GLOffload_SetPreferGBM(true);
    SDLOP_GLOffloadPolicy policy = SDLOP_GLOffload_GetPolicy();
    if (!kmsdrm_offload_decided && policy == SDLOP_GL_OFFLOAD_FORCE &&
        SDLOP_GLOffload_HasHardwareGPU()) {
        kmsdrm_use_offload = true;
        kmsdrm_offload_decided = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                    "KMSDRM: GL offload forced via SDLOP_GL_OFFLOAD (%s)",
                    SDLOP_GLOffload_GetGPUDescription());
    }

    if (kmsdrm_offload_decided && kmsdrm_use_offload) {
        return kmsdrm_create_offload_context(device, window, api);
    }

    /* switch the window from the software dumb-buffer scanout to GL */
    if (windata->sw_active) {
        /* release the software scanout: GL owns the CRTC from now on */
        KMSDRM_DestroySwScanout(window);
        SDL_DestroyWindowSurface(window);
    }
    windata->gl_active = true;

    if (!ensure_egl_display()) {
        if (!kmsdrm_offload_decided && policy != SDLOP_GL_OFFLOAD_NEVER &&
            SDLOP_GLOffload_HasHardwareGPU()) {
            kmsdrm_use_offload = true;
            kmsdrm_offload_decided = true;
            windata->gl_active = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "KMSDRM: output has no hardware GBM/GL; offloading GL to GPU (%s)",
                        SDLOP_GLOffload_GetGPUDescription());
            return kmsdrm_create_offload_context(device, window, api);
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

    EGLContext share = EGL_NO_CONTEXT;
    if (sdlop_glattrs.share_with_current_context) {
        SDL_GLContext cur = SDLOP_GL_CurrentContext();
        if (cur && !((KMSDRMGLContext *)cur)->is_offload) {
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
        if (!kmsdrm_offload_decided && policy != SDLOP_GL_OFFLOAD_NEVER &&
            SDLOP_GLOffload_HasHardwareGPU()) {
            kmsdrm_native_gl_deinit();
            kmsdrm_use_offload = true;
            kmsdrm_offload_decided = true;
            windata->gl_active = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "KMSDRM: GBM/EGL surface creation failed; offloading GL to GPU (%s)",
                        SDLOP_GLOffload_GetGPUDescription());
            return kmsdrm_create_offload_context(device, window, api);
        }
        return NULL;
    }

    /* bind immediately, like the wayland driver does */
    if (!KMSDRM_GL_MakeCurrent(device, window, glctx)) {
        KMSDRM_DestroyGLSurfaces(windata);
        SDLOP_EGL_eglDestroyContext(egl_display, ctx);
        free(glctx);
        return NULL;
    }

    if (!kmsdrm_offload_decided) {
        const char *renderer = SDLOP_GLOffload_QueryCurrentRenderer();
        bool is_sw = SDLOP_GLOffload_IsSoftwareRenderer(renderer);
        if (is_sw && policy != SDLOP_GL_OFFLOAD_NEVER && SDLOP_GLOffload_HasHardwareGPU()) {
            /* Output EGL/GBM is software-rendered (e.g. kms_swrast / llvmpipe)
             * while a hardware GPU exists: switch session to headless GPU offload. */
            KMSDRM_DestroyGLSurfaces(windata);
            SDLOP_EGL_eglDestroyContext(egl_display, ctx);
            free(glctx);
            kmsdrm_native_gl_deinit();
            kmsdrm_use_offload = true;
            kmsdrm_offload_decided = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "KMSDRM: output GL is software (%s); offloading to GPU (%s)",
                        renderer ? renderer : "unknown",
                        SDLOP_GLOffload_GetGPUDescription());
            return kmsdrm_create_offload_context(device, window, api);
        }
        kmsdrm_use_offload = false;
        kmsdrm_offload_decided = true;
        sdlop_glattrs.accelerated_visual = is_sw ? 0 : 1;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop KMS/GBM: GL context created (%s)",
                api == EGL_OPENGL_API ? "desktop GL" : "GLES");
    return glctx;
}

static bool kmsdrm_present_fb(KMSDRM_DeviceData *dev, KMSDRM_WindowData *windata,
                              uint32_t fb_id, bool need_modeset)
{
    if (!dev->vt_active) {
        return true; /* another VT owns the screen; drop the frame */
    }
    KMSDRM_DisplayData *dd = windata->display;
    uint32_t flip_flags = DRM_MODE_PAGE_FLIP_EVENT;
    bool present_ok = true;
    int ret = 0;
    const bool use_atomic = dd->atomic.available;

    if (need_modeset) {
        /* first swap: the CRTC must be told what to scan out */
        if (use_atomic) {
            if (!KMSDRM_AtomicModeset(dev, dd, fb_id)) {
                int merr = errno;
                SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                            "KMSDRM: atomic modeset failed (%s); using legacy modeset for GL present",
                            strerror(merr));
                if (merr == EINVAL || merr == ENOSYS) {
                    dd->atomic.available = false; /* stop trying atomic every frame */
                }
                ret = -1;
            }
        } else {
            ret = -1; /* go straight to the legacy path below */
        }
        if (ret == 0) {
            windata->crtc_programmed = true;
        } else {
            ret = SDLOP_DRM_drmModeSetCrtc(dev->drm_fd, dd->crtc->crtc_id, fb_id, 0, 0,
                                           &dd->connector->connector_id, 1, &dd->mode);
            if (ret) {
                return SDL_SetError("Could not set videomode on CRTC");
            }
            windata->crtc_programmed = true;
        }
    } else if (use_atomic) {
        /* swap_interval 0 async flips are a legacy-path refinement; the
         * atomic flip below is vsync'd (NONBLOCK without ASYNC), which is
         * the conservative behavior for the GL path here */
        (void)flip_flags;
        ret = KMSDRM_AtomicFlip(dev, dd, fb_id, &windata->waiting_for_flip);
        if (ret == 0) {
            windata->waiting_for_flip = true;
            if (kmsdrm_double_buffer()) {
                KMSDRM_WaitPageflip(dev, windata, 1500);
            }
        } else {
            int err = errno;
            if (err == EBUSY && KMSDRM_WaitPageflip(dev, windata, 1500)) {
                ret = KMSDRM_AtomicFlip(dev, dd, fb_id, &windata->waiting_for_flip);
                if (ret == 0) {
                    windata->waiting_for_flip = true;
                }
            }
            if (ret != 0) {
                if (is_permission_error(err)) {
                    windata->lost_master = true;
                    present_ok = false;
                    SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: atomic flip lost DRM master");
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                                "KMSDRM: atomic flip failed (%s); using modeset for GL present",
                                strerror(err));
                    if (err == EINVAL || err == ENOSYS) {
                        dd->atomic.available = false; /* stop trying atomic every frame */
                    }
                    ret = SDLOP_DRM_drmModeSetCrtc(dev->drm_fd, dd->crtc->crtc_id, fb_id,
                                                   0, 0, &dd->connector->connector_id, 1, &dd->mode);
                    if (ret == 0) {
                        windata->crtc_programmed = true;
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                                     "KMSDRM: atomic flip failed and modeset fallback failed: %s",
                                     strerror(errno));
                        present_ok = false;
                    }
                }
            }
        }
    } else {
        if (swap_interval == 0 && dev->async_pageflip_support) {
            flip_flags |= DRM_MODE_PAGE_FLIP_ASYNC;
        }
        ret = SDLOP_DRM_drmModePageFlip(dev->drm_fd, dd->crtc->crtc_id, fb_id,
                                        flip_flags, &windata->waiting_for_flip);
        if (ret == 0) {
            windata->waiting_for_flip = true;
            /* immediate wait (SDL_HINT_VIDEO_DOUBLE_BUFFER) */
            if (kmsdrm_double_buffer()) {
                KMSDRM_WaitPageflip(dev, windata, 1500);
            }
        } else {
            int err = errno;
            if (err == EINVAL && (flip_flags & DRM_MODE_PAGE_FLIP_ASYNC)) {
                /* some drivers claim async flips but can't (or fail
                 * intermittently): retry synchronously without the async
                 * flag (SDL3 commit 69dd29e) */
                flip_flags &= ~DRM_MODE_PAGE_FLIP_ASYNC;
                ret = SDLOP_DRM_drmModePageFlip(dev->drm_fd, dd->crtc->crtc_id, fb_id,
                                                flip_flags, &windata->waiting_for_flip);
                err = errno;
                if (ret == 0) {
                    windata->waiting_for_flip = true;
                    if (kmsdrm_double_buffer()) {
                        KMSDRM_WaitPageflip(dev, windata, 1500);
                    }
                }
            }
            if (ret != 0 && err == EBUSY && KMSDRM_WaitPageflip(dev, windata, 1500)) {
                ret = SDLOP_DRM_drmModePageFlip(dev->drm_fd, dd->crtc->crtc_id, fb_id,
                                                flip_flags, &windata->waiting_for_flip);
                if (ret == 0) {
                    windata->waiting_for_flip = true;
                    ret = 0;
                }
            }
            if (ret != 0) {
                if (is_permission_error(err)) {
                    windata->lost_master = true;
                    present_ok = false;
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
                    ret = SDLOP_DRM_drmModeSetCrtc(dev->drm_fd, dd->crtc->crtc_id, fb_id,
                                                   0, 0, &dd->connector->connector_id, 1, &dd->mode);
                    if (ret == 0) {
                        windata->crtc_programmed = true;
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                                     "KMSDRM: pageflip failed and modeset fallback failed: %s",
                                     strerror(errno));
                        present_ok = false;
                    }
                } else {
                    SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: pageflip failed: %s", strerror(err));
                    present_ok = false;
                }
            }
        }
    }

    if (!present_ok) {
        return SDL_SetError("KMSDRM: unable to present the frame to the scanout");
    }
    return true;
}

static bool kmsdrm_offload_ensure_surface(KMSDRM_DeviceData *dev, SDL_Window *window,
                                        KMSDRM_WindowData *windata, KMSDRM_DisplayData *dd,
                                        SDLOP_GLOffloadContext *octx)
{
    int w = (int)dd->mode.hdisplay;
    int h = (int)dd->mode.vdisplay;

    if (windata->gl_offload_surface && !windata->egl_surface_dirty) {
        if (!windata->gl_offload_prime && !KMSDRM_EnsureSwScanout(window)) {
            return false;
        }
        return true;
    }

    if (windata->gl_offload_prime || (!windata->gl_offload_surface && kmsdrm_can_try_prime(dev))) {
        if (windata->gl_offload_surface) {
            kmsdrm_release_prime_bos(windata);
            if (!SDLOP_GLOffload_ResizeSurface(octx,
                                               (SDLOP_GLOffloadSurface **)&windata->gl_offload_surface,
                                               w, h)) {
                SDLOP_GLOffload_DestroySurface((SDLOP_GLOffloadSurface *)windata->gl_offload_surface);
                windata->gl_offload_surface = NULL;
            }
        } else if (kmsdrm_can_try_prime(dev)) {
            windata->gl_offload_surface = SDLOP_GLOffload_CreatePrimeSurface(octx, w, h);
        }
        if (windata->gl_offload_surface &&
            SDLOP_GLOffload_IsPrimeSurface((SDLOP_GLOffloadSurface *)windata->gl_offload_surface)) {
            windata->gl_offload_prime = true;
            windata->egl_surface_dirty = false;
            return true;
        }
        windata->gl_offload_prime = false;
    }

    if (!KMSDRM_EnsureSwScanout(window)) {
        return false;
    }
    if (!windata->gl_offload_surface) {
        windata->gl_offload_surface = SDLOP_GLOffload_CreateSurface(octx, w, h);
        if (!windata->gl_offload_surface) {
            return false;
        }
    } else if (windata->egl_surface_dirty) {
        if (!SDLOP_GLOffload_ResizeSurface(
                octx,
                (SDLOP_GLOffloadSurface **)&windata->gl_offload_surface,
                w, h)) {
            return false;
        }
    }
    windata->egl_surface_dirty = false;
    return true;
}

bool KMSDRM_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context)
{
    (void)device;
    if (!context) {
        if (kmsdrm_use_offload) {
            SDLOP_GLOffload_MakeCurrent(NULL, NULL);
        }
        if (egl_initialized) {
            SDLOP_EGL_eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        return true;
    }
    KMSDRMGLContext *glctx = (KMSDRMGLContext *)context;
    if (glctx->is_offload) {
        SDLOP_GLOffloadSurface *osurf = NULL;
        if (window) {
            KMSDRM_DeviceData *dev = vid();
            KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
            KMSDRM_DisplayData *dd = windata ? windata->display : NULL;
            if (!windata || !dd) {
                return SDL_SetError("Invalid window");
            }
            windata->gl_active = true;
            windata->gl_offload_active = true;
            windata->gl_offload_ctx = glctx->offload_ctx;
            if (!kmsdrm_offload_ensure_surface(dev, window, windata, dd, glctx->offload_ctx)) {
                return false;
            }
            osurf = (SDLOP_GLOffloadSurface *)windata->gl_offload_surface;
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
    if (!dd->connected) {
        return SDL_SetError("KMSDRM: display disconnected (window has no live output)");
    }

    if (windata->gl_offload_active) {
        KMSDRMGLContext *cur = (KMSDRMGLContext *)SDLOP_GL_CurrentContext();
        SDLOP_GLOffloadContext *octx = (cur && cur->is_offload)
            ? cur->offload_ctx
            : (SDLOP_GLOffloadContext *)windata->gl_offload_ctx;
        if (!octx) {
            return SDL_SetError("No offload GL context bound for swap");
        }
        if (windata->gl_offload_prime) {
            if (windata->egl_surface_dirty || !windata->gl_offload_surface) {
                if (!kmsdrm_offload_ensure_surface(dev, window, windata, dd, octx)) {
                    return false;
                }
                if (cur && cur->is_offload &&
                    !SDLOP_GLOffload_MakeCurrent(octx, (SDLOP_GLOffloadSurface *)windata->gl_offload_surface)) {
                    return false;
                }
            }
            if (windata->gl_offload_prime) {
                if (windata->waiting_for_flip && !KMSDRM_WaitPageflip(dev, windata, 1500)) {
                    return SDL_SetError("Wait for previous pageflip failed");
                }
                if (windata->prime_bo) {
                    SDLOP_GLOffload_ReleasePrimeBO((SDLOP_GLOffloadSurface *)windata->gl_offload_surface,
                                                   windata->prime_bo);
                    windata->prime_bo = NULL;
                }
                windata->prime_bo = windata->prime_next_bo;
                windata->prime_next_bo = SDLOP_GLOffload_SwapAndLockPrimeBO(
                    octx, (SDLOP_GLOffloadSurface *)windata->gl_offload_surface);

                KMSDRM_PrimeFBInfo *pfb = windata->prime_next_bo
                    ? kmsdrm_prime_fb_from_bo(dev->drm_fd, windata->prime_next_bo)
                    : NULL;
                bool prime_ok = pfb != NULL &&
                    kmsdrm_present_fb(dev, windata, pfb->fb_id,
                                      !windata->prime_bo || !windata->crtc_programmed);
                if (prime_ok) {
                    return true;
                }

                /* PRIME import or scanout was rejected by the display controller:
                 * permanently fall back to headless GPU readback -> dumb buffers. */
                SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                            "KMSDRM: PRIME BO scanout unavailable (%s); falling back to dumb-buffer readback",
                            SDL_GetError());
                kmsdrm_prime_disabled = true;
                windata->gl_offload_prime = false;

                bool salvaged = false;
                if (KMSDRM_EnsureSwScanout(window) && windata->gl_offload_surface) {
                    salvaged = SDLOP_GLOffload_ReadToXRGB8888(
                        octx,
                        (SDLOP_GLOffloadSurface *)windata->gl_offload_surface,
                        windata->sw_map[windata->sw_draw],
                        (int)windata->sw_pitch,
                        (int)dd->mode.hdisplay,
                        (int)dd->mode.vdisplay);
                }

                kmsdrm_release_prime_bos(windata);
                if (windata->gl_offload_surface) {
                    SDLOP_GLOffload_DestroySurface((SDLOP_GLOffloadSurface *)windata->gl_offload_surface);
                    windata->gl_offload_surface = NULL;
                }
                if (!KMSDRM_EnsureSwScanout(window)) {
                    return false;
                }
                windata->gl_offload_surface = SDLOP_GLOffload_CreateSurface(
                    octx, (int)dd->mode.hdisplay, (int)dd->mode.vdisplay);
                if (!windata->gl_offload_surface) {
                    return false;
                }
                if (cur && cur->is_offload) {
                    SDLOP_GLOffload_MakeCurrent(octx, (SDLOP_GLOffloadSurface *)windata->gl_offload_surface);
                }
                if (!salvaged) {
                    return false;
                }
                return KMSDRM_PresentSw(dev, window);
            }
        }

        if (!KMSDRM_EnsureSwScanout(window)) {
            return false;
        }
        if (!windata->gl_offload_surface) {
            windata->gl_offload_surface = SDLOP_GLOffload_CreateSurface(
                octx, (int)dd->mode.hdisplay, (int)dd->mode.vdisplay);
            if (!windata->gl_offload_surface) {
                return false;
            }
            windata->egl_surface_dirty = false;
        }
        if (windata->waiting_for_flip && !KMSDRM_WaitPageflip(dev, windata, 1500)) {
            return SDL_SetError("Wait for previous pageflip failed");
        }
        if (!SDLOP_GLOffload_ReadToXRGB8888(octx,
                                            (SDLOP_GLOffloadSurface *)windata->gl_offload_surface,
                                            windata->sw_map[windata->sw_draw],
                                            (int)windata->sw_pitch,
                                            (int)dd->mode.hdisplay,
                                            (int)dd->mode.vdisplay)) {
            return false;
        }
        if (!KMSDRM_PresentSw(dev, window)) {
            return false;
        }
        if (windata->egl_surface_dirty) {
            if (!SDLOP_GLOffload_ResizeSurface(
                    octx,
                    (SDLOP_GLOffloadSurface **)&windata->gl_offload_surface,
                    (int)dd->mode.hdisplay, (int)dd->mode.vdisplay)) {
                return false;
            }
            windata->egl_surface_dirty = false;
        }
        return true;
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

    return kmsdrm_present_fb(dev, windata, fb_info->fb_id, !windata->bo || !windata->crtc_programmed);
}

void KMSDRM_GL_DeleteContext(SDLop_VideoDevice *device, void *context)
{
    (void)device;
    KMSDRMGLContext *glctx = (KMSDRMGLContext *)context;
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

SDL_FunctionPointer KMSDRM_GL_GetProcAddress(SDLop_VideoDevice *device, const char *proc)
{
    (void)device;
    if (kmsdrm_offload_decided && kmsdrm_use_offload) {
        return SDLOP_GLOffload_GetProcAddress(proc);
    }
    if (!egl_initialized && !ensure_egl_display()) {
        if (SDLOP_GLOffload_GetPolicy() != SDLOP_GL_OFFLOAD_NEVER) {
            SDLOP_GLOffload_SetPreferGBM(true);
            if (SDLOP_GLOffload_HasHardwareGPU()) {
                return SDLOP_GLOffload_GetProcAddress(proc);
            }
        }
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

static void kmsdrm_native_gl_deinit(void)
{
    if (egl_initialized) {
        SDLOP_EGL_eglTerminate(egl_display);
        egl_initialized = false;
        egl_display = EGL_NO_DISPLAY;
        SDLOP_EGL_UnloadSymbols();
    }
    KMSDRM_DeviceData *dev = vid();
    if (dev->gbm_dev) {
        /* Cursor BOs are created from this device but outlive GL contexts (they
         * are display state, not window state). Drop them before the device so
         * driver teardown does not call gbm_bo_destroy() on a dangling BO whose
         * parent device is already gone. */
        for (int i = 0; i < dev->num_displays; i++) {
            KMSDRM_DisplayData *dd = &dev->displays[i];
            if (!dd->cursor_bo) {
                continue;
            }
            if (dd->atomic_cursor.fb_id && SDLOP_DRM_drmModeRmFB) {
                SDLOP_DRM_drmModeRmFB(dev->drm_fd, dd->atomic_cursor.fb_id);
            }
            dd->atomic_cursor.fb_id = 0;
            dd->cursor_visible = false;
            SDLOP_GBM_gbm_bo_destroy(dd->cursor_bo);
            dd->cursor_bo = NULL;
        }
        SDLOP_GBM_gbm_device_destroy(dev->gbm_dev);
        dev->gbm_dev = NULL;
        dev->gbm_init = false;
        SDLOP_GBM_UnloadSymbols();
    }
}

/* used by the driver core when the LAST GL window is destroyed */
void KMSDRM_GL_Deinit(void)
{
    kmsdrm_native_gl_deinit();
    SDLOP_GLOffload_Shutdown();
    kmsdrm_offload_decided = false;
    kmsdrm_use_offload = false;
    kmsdrm_prime_disabled = false;
    swap_interval = 1;
}

/* SDL_EGL_GetCurrentDisplay / GetCurrentConfig / GetWindowSurface */
void *KMSDRM_EGL_GetDisplay(SDLop_VideoDevice *device)
{
    (void)device;
    return egl_initialized ? (void *)egl_display : NULL;
}

void *KMSDRM_EGL_GetConfig(SDLop_VideoDevice *device)
{
    (void)device;
    KMSDRMGLContext *cur = (KMSDRMGLContext *)SDLOP_GL_CurrentContext();
    return (egl_initialized && cur) ? (void *)config_for_context(cur) : NULL;
}

void *KMSDRM_EGL_GetWindowSurface(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    KMSDRM_WindowData *windata = window ? (KMSDRM_WindowData *)window->driverdata : NULL;
    return (windata && windata->egl_surface != EGL_NO_SURFACE) ? (void *)windata->egl_surface : NULL;
}

#else /* !SDLOP_HAVE_EGL */

void KMSDRM_GL_Deinit(void)
{
}

void *KMSDRM_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    SDL_SetError("EGL support was not built in");
    return NULL;
}

bool KMSDRM_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context)
{
    (void)device;
    (void)window;
    if (!context) {
        return true;
    }
    return SDL_SetError("EGL support was not built in");
}

bool KMSDRM_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    return SDL_SetError("EGL support was not built in");
}

void KMSDRM_GL_DeleteContext(SDLop_VideoDevice *device, void *context)
{
    (void)device;
    (void)context;
}

SDL_FunctionPointer KMSDRM_GL_GetProcAddress(SDLop_VideoDevice *device, const char *proc)
{
    (void)device;
    (void)proc;
    return NULL;
}

bool KMSDRM_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval)
{
    (void)device;
    (void)interval;
    return SDL_SetError("EGL support was not built in");
}

bool KMSDRM_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval)
{
    (void)device;
    *interval = 1;
    return true;
}

void *KMSDRM_EGL_GetDisplay(SDLop_VideoDevice *device)
{
    (void)device;
    return NULL;
}

void *KMSDRM_EGL_GetConfig(SDLop_VideoDevice *device)
{
    (void)device;
    return NULL;
}

void *KMSDRM_EGL_GetWindowSurface(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    return NULL;
}

#endif /* SDLOP_HAVE_EGL */
