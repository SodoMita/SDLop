/*
  SDLop - Headless GPU OpenGL offload for software-rendered outputs.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_gl_offload.h"
#include "sdlop_egl_dyn.h"

#ifdef SDLop_VIDEO_KMSDRM
#include "kmsdrm/sdlop_kmsdrm_dyn.h"
#include <gbm.h>
#endif

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_DEVICE_EXT
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#endif
#ifndef EGL_PLATFORM_GBM_MESA
#define EGL_PLATFORM_GBM_MESA 0x31D7
#endif
#ifndef EGL_DRM_DEVICE_FILE_EXT
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#endif
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
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
#ifndef EGL_CONTEXT_FLAGS_KHR
#define EGL_CONTEXT_FLAGS_KHR 0x30FC
#endif
#ifndef EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR
#define EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR 0x00000001
#endif
#ifndef EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR
#define EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR 0x00000002
#endif
#ifndef EGL_SWAP_BEHAVIOR
#define EGL_SWAP_BEHAVIOR 0x3093
#endif
#ifndef EGL_BUFFER_PRESERVED
#define EGL_BUFFER_PRESERVED 0x3094
#endif

#ifndef GBM_FORMAT_XRGB8888
#define GBM_FORMAT_XRGB8888 0x34325258u
#endif
#ifndef GBM_FORMAT_ARGB8888
#define GBM_FORMAT_ARGB8888 0x34325241u
#endif
#ifndef GBM_BO_USE_SCANOUT
#define GBM_BO_USE_SCANOUT (1u << 0)
#endif
#ifndef GBM_BO_USE_RENDERING
#define GBM_BO_USE_RENDERING (1u << 2)
#endif
#ifndef GBM_BO_USE_LINEAR
#define GBM_BO_USE_LINEAR (1u << 4)
#endif

/* Minimal GL constants (avoid requiring <GL/gl.h> or <GLES2/gl2.h>) */
#define GL_NO_ERROR_                   0x0000u
#define GL_INVALID_ENUM_               0x0500u
#define GL_PACK_ROW_LENGTH_            0x0D02u
#define GL_PACK_ALIGNMENT_             0x0D05u
#define GL_RGBA_                       0x1908u
#define GL_UNSIGNED_BYTE_              0x1401u
#define GL_RENDERER_                   0x1F01u
#define GL_BGRA_                       0x80E1u
#define GL_FRAMEBUFFER_BINDING_        0x8CA6u
#define GL_READ_FRAMEBUFFER_           0x8CA8u
#define GL_READ_FRAMEBUFFER_BINDING_   0x8CAAu
#define GL_FRAMEBUFFER_                0x8D40u

typedef void *SDLOP_EGLDeviceEXT;
typedef EGLBoolean (*SDLOP_PFN_eglQueryDevicesEXT)(EGLint max_devices, SDLOP_EGLDeviceEXT *devices, EGLint *num_devices);
typedef const char *(*SDLOP_PFN_eglQueryDeviceStringEXT)(SDLOP_EGLDeviceEXT device, EGLint name);
typedef EGLDisplay (*SDLOP_PFN_eglGetPlatformDisplayEXT)(EGLenum platform, void *native_display, const EGLint *attrib_list);

typedef const unsigned char *(*SDLOP_PFN_glGetString)(unsigned int name);
typedef unsigned int (*SDLOP_PFN_glGetError)(void);
typedef void (*SDLOP_PFN_glGetIntegerv)(unsigned int pname, int *data);
typedef void (*SDLOP_PFN_glPixelStorei)(unsigned int pname, int param);
typedef void (*SDLOP_PFN_glBindFramebuffer)(unsigned int target, unsigned int framebuffer);
typedef void (*SDLOP_PFN_glReadPixels)(int x, int y, int width, int height,
                                       unsigned int format, unsigned int type, void *pixels);
typedef void (*SDLOP_PFN_glFinish)(void);

struct SDLOP_GLOffloadContext
{
    EGLContext egl_ctx;
    uint32_t api; /* EGL_OPENGL_API or EGL_OPENGL_ES_API */
};

struct SDLOP_GLOffloadSurface
{
    EGLSurface egl_surface;
    int w;
    int h;
    bool is_pbuffer;
    bool is_prime;
#ifdef SDLop_VIDEO_KMSDRM
    struct gbm_surface *gbm_surf;
#endif
};

static struct
{
    bool probed;
    bool available;
    bool prefer_gbm;
    bool egl_ref_held;
    bool is_pbuffer;
    EGLDisplay dpy;
    uint32_t gbm_visual_id;
    int render_fd;
#ifdef SDLop_VIDEO_KMSDRM
    struct gbm_device *gbm_dev;
    bool gbm_ref_held;
#endif
    char desc[256];

    /* GL proc cache for readback */
    void *gl_fallback_lib;
    bool gl_fallback_tried;
    bool bgra_checked;
    bool bgra_supported;

    /* Reusable CPU readback staging buffer */
    uint8_t *staging;
    size_t staging_cap;
} offload = {
    .render_fd = -1,
};

/* ------------------------------------------------------------------ */
/* Policy + string helpers                                             */
/* ------------------------------------------------------------------ */

SDLOP_GLOffloadPolicy SDLOP_GLOffload_GetPolicy(void)
{
    /* SDL_SetHint or the environment; the GL option before the global one */
    static const char *const names[] = { SDLOP_HINT_GL_OFFLOAD, SDLOP_HINT_OFFLOAD, NULL };
    const char *v = SDLOP_HintFirst(names);
    if (!v || !*v || strcasecmp(v, "auto") == 0) {
        return SDLOP_GL_OFFLOAD_AUTO;
    }
    if (strcmp(v, "0") == 0 || strcasecmp(v, "off") == 0 ||
        strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0) {
        return SDLOP_GL_OFFLOAD_NEVER;
    }
    if (strcmp(v, "1") == 0 || strcasecmp(v, "on") == 0 ||
        strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
        strcasecmp(v, "force") == 0) {
        return SDLOP_GL_OFFLOAD_FORCE;
    }
    return SDLOP_GL_OFFLOAD_AUTO;
}

static bool strcasestr_ascii(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) {
        return false;
    }
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

bool SDLOP_GLOffload_IsSoftwareRenderer(const char *renderer)
{
    if (!renderer || !*renderer) {
        return false;
    }
    if (strcasestr_ascii(renderer, "llvmpipe") ||
        strcasestr_ascii(renderer, "softpipe") ||
        strcasestr_ascii(renderer, "swrast") ||
        strcasestr_ascii(renderer, "software rasterizer") ||
        strcasestr_ascii(renderer, "openswr") ||
        strcasestr_ascii(renderer, "lavapipe")) {
        return true;
    }
    /* Match "SWR" as a whole token (e.g. "SWR ..." or "Mesa SWR") without
     * false-positives on arbitrary hardware names. */
    for (const char *p = renderer; *p; p++) {
        if ((p == renderer || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')) &&
            tolower((unsigned char)p[0]) == 's' &&
            tolower((unsigned char)p[1]) == 'w' &&
            tolower((unsigned char)p[2]) == 'r' &&
            !isalnum((unsigned char)p[3]) && p[3] != '_') {
            return true;
        }
    }
    return false;
}

static SDL_FunctionPointer lookup_gl_proc(const char *proc)
{
    if (!proc) {
        return NULL;
    }
    if (SDLOP_EGL_eglGetProcAddress) {
        SDL_FunctionPointer fn = SDLOP_EGL_eglGetProcAddress(proc);
        if (fn) {
            return fn;
        }
    }
    void *sym = dlsym(RTLD_DEFAULT, proc);
    if (sym) {
        return (SDL_FunctionPointer)(uintptr_t)sym;
    }
    if (!offload.gl_fallback_tried) {
        offload.gl_fallback_tried = true;
        static const char *const candidates[] = {
            "libGLESv2.so.2",
            "libGLESv2.so",
            "libGL.so.1",
            "libGL.so",
        };
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            offload.gl_fallback_lib = dlopen(candidates[i], RTLD_LAZY | RTLD_LOCAL);
            if (offload.gl_fallback_lib) {
                break;
            }
        }
    }
    if (offload.gl_fallback_lib) {
        sym = dlsym(offload.gl_fallback_lib, proc);
        if (sym) {
            return (SDL_FunctionPointer)(uintptr_t)sym;
        }
    }
    return NULL;
}

SDL_FunctionPointer SDLOP_GLOffload_GetProcAddress(const char *proc)
{
    return lookup_gl_proc(proc);
}

const char *SDLOP_GLOffload_QueryCurrentRenderer(void)
{
    SDLOP_PFN_glGetString p_glGetString =
        (SDLOP_PFN_glGetString)lookup_gl_proc("glGetString");
    if (!p_glGetString) {
        return NULL;
    }
    return (const char *)p_glGetString(GL_RENDERER_);
}

/* ------------------------------------------------------------------ */
/* EGL config picking                                                  */
/* ------------------------------------------------------------------ */

static bool pick_offload_config(EGLDisplay dpy,
                                uint32_t api,
                                bool is_pbuffer,
                                uint32_t visual_id,
                                int r, int g, int b, int a,
                                int depth, int stencil, int samples,
                                EGLConfig *out)
{
    EGLint renderable;
    if (api == EGL_OPENGL_API) {
        renderable = EGL_OPENGL_BIT;
    } else {
        renderable = SDLOP_EGL_ESRenderableType(dpy);
    }

    EGLint attribs[28];
    int n = 0;
    attribs[n++] = EGL_SURFACE_TYPE;    attribs[n++] = is_pbuffer ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT;
    attribs[n++] = EGL_RENDERABLE_TYPE; attribs[n++] = renderable;
    attribs[n++] = EGL_COLOR_BUFFER_TYPE; attribs[n++] = EGL_RGB_BUFFER;
    attribs[n++] = EGL_RED_SIZE;        attribs[n++] = r;
    attribs[n++] = EGL_GREEN_SIZE;      attribs[n++] = g;
    attribs[n++] = EGL_BLUE_SIZE;       attribs[n++] = b;
    attribs[n++] = EGL_ALPHA_SIZE;      attribs[n++] = a;
    attribs[n++] = EGL_DEPTH_SIZE;      attribs[n++] = depth;
    attribs[n++] = EGL_STENCIL_SIZE;    attribs[n++] = stencil;
    if (samples > 0) {
        attribs[n++] = EGL_SAMPLE_BUFFERS; attribs[n++] = 1;
        attribs[n++] = EGL_SAMPLES;        attribs[n++] = samples;
    }
    attribs[n++] = EGL_NONE;

    EGLConfig configs[128];
    EGLint num = 0;
    if (!SDLOP_EGL_eglChooseConfig(dpy, attribs, configs, 128, &num) || num == 0) {
        return false;
    }

    int best = -1;
    int32_t best_diff = INT32_MAX;
    for (int i = 0; i < num; i++) {
        if (!is_pbuffer && visual_id != 0) {
            EGLint vis = 0;
            if (!SDLOP_EGL_eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &vis) ||
                (uint32_t)vis != visual_id) {
                continue;
            }
        }
        static const EGLint sattrs[] = {
            EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE, EGL_ALPHA_SIZE,
            EGL_DEPTH_SIZE, EGL_STENCIL_SIZE
        };
        const int svals[] = { r, g, b, a, depth, stencil };
        int32_t diff = 0;
        bool ok = true;
        for (size_t j = 0; j < sizeof(sattrs) / sizeof(sattrs[0]); j++) {
            EGLint val = 0;
            if (!SDLOP_EGL_eglGetConfigAttrib(dpy, configs[i], sattrs[j], &val)) {
                ok = false;
                break;
            }
            diff += (val >= svals[j]) ? (val - svals[j]) : (svals[j] - val);
        }
        if (!ok) {
            continue;
        }
        if (best == -1 || diff < best_diff) {
            best = i;
            best_diff = diff;
        }
    }
    if (best < 0) {
        return false;
    }
    *out = configs[best];
    return true;
}

static EGLConfig config_for_offload_context(SDLOP_GLOffloadContext *ctx)
{
    EGLConfig cfg = NULL;
    EGLint id = 0;
    if (SDLOP_EGL_eglQueryContext(offload.dpy, ctx->egl_ctx, EGL_CONFIG_ID, &id)) {
        EGLint attribs[3] = { EGL_CONFIG_ID, id, EGL_NONE };
        EGLConfig cfgs[1];
        EGLint num = 0;
        if (SDLOP_EGL_eglChooseConfig(offload.dpy, attribs, cfgs, 1, &num) && num == 1) {
            return cfgs[0];
        }
    }
    uint32_t vis = offload.is_pbuffer ? 0 : offload.gbm_visual_id;
    if (pick_offload_config(offload.dpy, ctx->api, offload.is_pbuffer, vis,
                            sdlop_glattrs.red_size, sdlop_glattrs.green_size,
                            sdlop_glattrs.blue_size, sdlop_glattrs.alpha_size,
                            sdlop_glattrs.depth_size, sdlop_glattrs.stencil_size,
                            sdlop_glattrs.multisamplesamples, &cfg)) {
        return cfg;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Hardware GPU detection                                              */
/* ------------------------------------------------------------------ */

typedef struct SavedEGLState
{
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface draw;
    EGLSurface read;
    EGLenum api;
} SavedEGLState;

static void save_egl_state(SavedEGLState *s)
{
    s->dpy = SDLOP_EGL_eglGetCurrentDisplay ? SDLOP_EGL_eglGetCurrentDisplay() : EGL_NO_DISPLAY;
    s->ctx = SDLOP_EGL_eglGetCurrentContext ? SDLOP_EGL_eglGetCurrentContext() : EGL_NO_CONTEXT;
    s->draw = SDLOP_EGL_eglGetCurrentSurface ? SDLOP_EGL_eglGetCurrentSurface(EGL_DRAW) : EGL_NO_SURFACE;
    s->read = SDLOP_EGL_eglGetCurrentSurface ? SDLOP_EGL_eglGetCurrentSurface(EGL_READ) : EGL_NO_SURFACE;
    s->api = SDLOP_EGL_eglQueryAPI ? SDLOP_EGL_eglQueryAPI() : EGL_OPENGL_ES_API;
}

static void restore_egl_state(const SavedEGLState *s)
{
    if (SDLOP_EGL_eglBindAPI && s->api) {
        SDLOP_EGL_eglBindAPI(s->api);
    }
    if (SDLOP_EGL_eglMakeCurrent) {
        if (s->dpy != EGL_NO_DISPLAY && s->ctx != EGL_NO_CONTEXT) {
            SDLOP_EGL_eglMakeCurrent(s->dpy, s->draw, s->read, s->ctx);
        } else if (s->dpy != EGL_NO_DISPLAY) {
            SDLOP_EGL_eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    }
}

static EGLDisplay get_platform_display(EGLenum platform, void *native_display)
{
    SDLOP_PFN_eglGetPlatformDisplayEXT p_get =
        (SDLOP_PFN_eglGetPlatformDisplayEXT)SDLOP_EGL_eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!p_get) {
        p_get = (SDLOP_PFN_eglGetPlatformDisplayEXT)SDLOP_EGL_eglGetProcAddress("eglGetPlatformDisplay");
    }
    if (!p_get) {
        return EGL_NO_DISPLAY;
    }
    return p_get(platform, native_display, NULL);
}

/* Probe an initialized EGLDisplay (pbuffer mode) to check whether its GL
 * renderer is hardware-accelerated. Saves and restores caller EGL state. */
static bool probe_pbuffer_display_is_hw(EGLDisplay dpy, char *renderer_out, size_t renderer_cap)
{
    SavedEGLState saved;
    save_egl_state(&saved);

    bool is_hw = false;
    static const struct { uint32_t api; EGLint bit; } apis[] = {
        { EGL_OPENGL_ES_API, EGL_OPENGL_ES2_BIT },
        { EGL_OPENGL_API,    EGL_OPENGL_BIT },
    };

    for (size_t i = 0; i < sizeof(apis) / sizeof(apis[0]) && !is_hw; i++) {
        if (!SDLOP_EGL_eglBindAPI(apis[i].api)) {
            continue;
        }
        EGLint cfg_attribs[] = {
            EGL_SURFACE_TYPE,      EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE,   apis[i].bit,
            EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
            EGL_RED_SIZE,   8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE,  8,
            EGL_NONE
        };
        EGLConfig cfg = NULL;
        EGLint num = 0;
        if (!SDLOP_EGL_eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &num) || num == 0) {
            continue;
        }
        EGLint pb_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        EGLSurface surf = SDLOP_EGL_eglCreatePbufferSurface(dpy, cfg, pb_attribs);
        if (surf == EGL_NO_SURFACE) {
            continue;
        }
        EGLint ctx_attribs[4];
        if (apis[i].api == EGL_OPENGL_ES_API) {
            ctx_attribs[0] = EGL_CONTEXT_CLIENT_VERSION;
            ctx_attribs[1] = 2;
            ctx_attribs[2] = EGL_NONE;
        } else {
            ctx_attribs[0] = EGL_NONE;
        }
        EGLContext ctx = SDLOP_EGL_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (ctx != EGL_NO_CONTEXT) {
            if (SDLOP_EGL_eglMakeCurrent(dpy, surf, surf, ctx)) {
                const char *r = SDLOP_GLOffload_QueryCurrentRenderer();
                if (r && *r && !SDLOP_GLOffload_IsSoftwareRenderer(r)) {
                    is_hw = true;
                    if (renderer_out && renderer_cap > 0) {
                        snprintf(renderer_out, renderer_cap, "%s", r);
                    }
                }
                SDLOP_EGL_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            SDLOP_EGL_eglDestroyContext(dpy, ctx);
        }
        SDLOP_EGL_eglDestroySurface(dpy, surf);
    }

    restore_egl_state(&saved);
    return is_hw;
}

static bool probe_egl_devices(void)
{
    SDLOP_PFN_eglQueryDevicesEXT p_query_devices =
        (SDLOP_PFN_eglQueryDevicesEXT)SDLOP_EGL_eglGetProcAddress("eglQueryDevicesEXT");
    SDLOP_PFN_eglQueryDeviceStringEXT p_query_dev_str =
        (SDLOP_PFN_eglQueryDeviceStringEXT)SDLOP_EGL_eglGetProcAddress("eglQueryDeviceStringEXT");
    if (!p_query_devices) {
        return false;
    }

    EGLint num_devs = 0;
    if (!p_query_devices(0, NULL, &num_devs) || num_devs <= 0) {
        return false;
    }
    if (num_devs > 32) {
        num_devs = 32;
    }
    SDLOP_EGLDeviceEXT devs[32];
    EGLint got = 0;
    if (!p_query_devices(num_devs, devs, &got) || got <= 0) {
        return false;
    }

    for (EGLint i = 0; i < got; i++) {
        const char *dev_exts = p_query_dev_str ? p_query_dev_str(devs[i], EGL_EXTENSIONS) : NULL;
        if (dev_exts && strstr(dev_exts, "EGL_MESA_device_software") != NULL) {
            continue; /* explicitly a software EGL device */
        }

        EGLDisplay dpy = get_platform_display(EGL_PLATFORM_DEVICE_EXT, devs[i]);
        if (dpy == EGL_NO_DISPLAY) {
            continue;
        }
        EGLint maj = 0, min = 0;
        if (!SDLOP_EGL_eglInitialize(dpy, &maj, &min)) {
            continue;
        }
        char renderer[112] = { 0 };
        if (probe_pbuffer_display_is_hw(dpy, renderer, sizeof(renderer))) {
            const char *node = NULL;
            if (p_query_dev_str && dev_exts && strstr(dev_exts, "EGL_EXT_device_drm")) {
                node = p_query_dev_str(devs[i], EGL_DRM_RENDER_NODE_FILE_EXT);
                if (!node) {
                    node = p_query_dev_str(devs[i], EGL_DRM_DEVICE_FILE_EXT);
                }
            }
            if (node && *node) {
                snprintf(offload.desc, sizeof(offload.desc), "%s (%s)", renderer, node);
            } else {
                snprintf(offload.desc, sizeof(offload.desc), "%s", renderer);
            }
            offload.dpy = dpy;
            offload.is_pbuffer = true;
            return true;
        }
        SDLOP_EGL_eglTerminate(dpy);
    }
    return false;
}

#ifdef SDLop_VIDEO_KMSDRM
static bool probe_gbm_display_is_hw(EGLDisplay dpy,
                                    struct gbm_device *gbm,
                                    uint32_t visual_id,
                                    char *renderer_out,
                                    size_t renderer_cap)
{
    SavedEGLState saved;
    save_egl_state(&saved);

    bool is_hw = false;
    struct gbm_surface *gs = SDLOP_GBM_gbm_surface_create(gbm, 16, 16, visual_id, GBM_BO_USE_RENDERING);
    if (!gs) {
        restore_egl_state(&saved);
        return false;
    }

    static const struct { uint32_t api; EGLint bit; } apis[] = {
        { EGL_OPENGL_ES_API, EGL_OPENGL_ES2_BIT },
        { EGL_OPENGL_API,    EGL_OPENGL_BIT },
    };

    for (size_t i = 0; i < sizeof(apis) / sizeof(apis[0]) && !is_hw; i++) {
        if (!SDLOP_EGL_eglBindAPI(apis[i].api)) {
            continue;
        }
        EGLConfig cfg = NULL;
        if (!pick_offload_config(dpy, apis[i].api, false, visual_id, 8, 8, 8, 0, 0, 0, 0, &cfg)) {
            continue;
        }
        EGLSurface surf = SDLOP_EGL_eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gs, NULL);
        if (surf == EGL_NO_SURFACE) {
            continue;
        }
        EGLint ctx_attribs[4];
        if (apis[i].api == EGL_OPENGL_ES_API) {
            ctx_attribs[0] = EGL_CONTEXT_CLIENT_VERSION;
            ctx_attribs[1] = 2;
            ctx_attribs[2] = EGL_NONE;
        } else {
            ctx_attribs[0] = EGL_NONE;
        }
        EGLContext ctx = SDLOP_EGL_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (ctx != EGL_NO_CONTEXT) {
            if (SDLOP_EGL_eglMakeCurrent(dpy, surf, surf, ctx)) {
                const char *r = SDLOP_GLOffload_QueryCurrentRenderer();
                if (r && *r && !SDLOP_GLOffload_IsSoftwareRenderer(r)) {
                    is_hw = true;
                    if (renderer_out && renderer_cap > 0) {
                        snprintf(renderer_out, renderer_cap, "%s", r);
                    }
                }
                SDLOP_EGL_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            SDLOP_EGL_eglDestroyContext(dpy, ctx);
        }
        SDLOP_EGL_eglDestroySurface(dpy, surf);
    }

    SDLOP_GBM_gbm_surface_destroy(gs);
    restore_egl_state(&saved);
    return is_hw;
}

static bool probe_gbm_render_nodes(void)
{
    DIR *dir = opendir("/dev/dri");
    if (!dir) {
        return false;
    }

    int indices[64];
    int count = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < 64) {
        if (strncmp(de->d_name, "renderD", 7) != 0 || de->d_name[7] == '\0') {
            continue;
        }
        bool all_digits = true;
        for (const char *p = de->d_name + 7; *p; p++) {
            if (*p < '0' || *p > '9') {
                all_digits = false;
                break;
            }
        }
        if (!all_digits) {
            continue;
        }
        int idx = atoi(de->d_name + 7);
        int i = count;
        while (i > 0 && indices[i - 1] > idx) {
            indices[i] = indices[i - 1];
            i--;
        }
        indices[i] = idx;
        count++;
    }
    closedir(dir);

    if (count == 0) {
        return false;
    }
    if (!SDLOP_GBM_LoadSymbols()) {
        return false;
    }
    offload.gbm_ref_held = true;

    for (int i = 0; i < count; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", indices[i]);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        struct gbm_device *gbm = SDLOP_GBM_gbm_create_device(fd);
        if (!gbm) {
            close(fd);
            continue;
        }
        uint32_t vis = 0;
        if (SDLOP_GBM_gbm_device_is_format_supported(gbm, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING)) {
            vis = GBM_FORMAT_XRGB8888;
        } else if (SDLOP_GBM_gbm_device_is_format_supported(gbm, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING)) {
            vis = GBM_FORMAT_ARGB8888;
        }
        if (!vis) {
            SDLOP_GBM_gbm_device_destroy(gbm);
            close(fd);
            continue;
        }

        EGLDisplay dpy = get_platform_display(EGL_PLATFORM_GBM_MESA, gbm);
        if (dpy == EGL_NO_DISPLAY) {
            dpy = SDLOP_EGL_eglGetDisplay((EGLNativeDisplayType)(uintptr_t)gbm);
        }
        if (dpy == EGL_NO_DISPLAY) {
            SDLOP_GBM_gbm_device_destroy(gbm);
            close(fd);
            continue;
        }
        EGLint maj = 0, min = 0;
        if (!SDLOP_EGL_eglInitialize(dpy, &maj, &min)) {
            SDLOP_GBM_gbm_device_destroy(gbm);
            close(fd);
            continue;
        }

        char renderer[112] = { 0 };
        if (probe_gbm_display_is_hw(dpy, gbm, vis, renderer, sizeof(renderer))) {
            snprintf(offload.desc, sizeof(offload.desc), "%s (%s)", renderer, path);
            offload.dpy = dpy;
            offload.gbm_dev = gbm;
            offload.render_fd = fd;
            offload.gbm_visual_id = vis;
            offload.is_pbuffer = false;
            return true;
        }
        SDLOP_EGL_eglTerminate(dpy);
        SDLOP_GBM_gbm_device_destroy(gbm);
        close(fd);
    }

    SDLOP_GBM_UnloadSymbols();
    offload.gbm_ref_held = false;
    return false;
}
#endif

void SDLOP_GLOffload_SetPreferGBM(bool prefer)
{
    offload.prefer_gbm = prefer;
}

bool SDLOP_GLOffload_HasGBM(void)
{
#ifdef SDLop_VIDEO_KMSDRM
    return offload.available && !offload.is_pbuffer && offload.gbm_dev != NULL;
#else
    return false;
#endif
}

bool SDLOP_GLOffload_HasHardwareGPU(void)
{
    if (offload.probed) {
        return offload.available;
    }
    offload.probed = true;
    offload.available = false;

    if (!SDLOP_EGL_LoadSymbols()) {
        return false;
    }
    offload.egl_ref_held = true;

#ifdef SDLop_VIDEO_KMSDRM
    if (offload.prefer_gbm) {
        if (probe_gbm_render_nodes()) {
            offload.available = true;
            return true;
        }
        if (probe_egl_devices()) {
            offload.available = true;
            return true;
        }
        SDLOP_EGL_UnloadSymbols();
        offload.egl_ref_held = false;
        return false;
    }
#endif

    if (probe_egl_devices()) {
        offload.available = true;
        return true;
    }
#ifdef SDLop_VIDEO_KMSDRM
    if (probe_gbm_render_nodes()) {
        offload.available = true;
        return true;
    }
#endif

    SDLOP_EGL_UnloadSymbols();
    offload.egl_ref_held = false;
    return false;
}

const char *SDLOP_GLOffload_GetGPUDescription(void)
{
    return offload.available ? offload.desc : NULL;
}

/* ------------------------------------------------------------------ */
/* Context and surface lifecycle                                       */
/* ------------------------------------------------------------------ */

int SDLOP_GLOffload_BuildContextAttribs(uint32_t egl_api,
                                        int major_version,
                                        int minor_version,
                                        int profile_mask,
                                        int flags,
                                        int *attribs,
                                        int max_attribs)
{
    if (!attribs || max_attribs < 10) {
        return -1;
    }
    int n = 0;
    if (egl_api == EGL_OPENGL_API) {
        int ctx_flags = 0;
        attribs[n++] = EGL_CONTEXT_MAJOR_VERSION_KHR; attribs[n++] = major_version;
        attribs[n++] = EGL_CONTEXT_MINOR_VERSION_KHR; attribs[n++] = minor_version;
        attribs[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
        attribs[n++] = (profile_mask & SDL_GL_CONTEXT_PROFILE_COMPATIBILITY)
            ? EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR
            : EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
        if (flags & SDL_GL_CONTEXT_DEBUG_FLAG) {
            ctx_flags |= EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
        }
        if (flags & SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG) {
            ctx_flags |= EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR;
        }
        if (ctx_flags != 0) {
            attribs[n++] = EGL_CONTEXT_FLAGS_KHR;
            attribs[n++] = ctx_flags;
        }
    } else {
        /* as stock SDL_egl.c: X.0 through the EGL 1.3 client version, X.Y
         * through EGL_KHR_create_context (which the debug flag below needs
         * anyway); the old "3 or 2" mapping turned ES 1.1 into ES 3.x and
         * let impossible versions succeed */
        if (minor_version == 0) {
            attribs[n++] = EGL_CONTEXT_CLIENT_VERSION;
            attribs[n++] = major_version > 1 ? major_version : 1;
        } else {
            attribs[n++] = EGL_CONTEXT_MAJOR_VERSION_KHR; attribs[n++] = major_version;
            attribs[n++] = EGL_CONTEXT_MINOR_VERSION_KHR; attribs[n++] = minor_version;
        }
        if (flags & SDL_GL_CONTEXT_DEBUG_FLAG) {
            attribs[n++] = EGL_CONTEXT_FLAGS_KHR;
            attribs[n++] = EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
        }
    }
    return n;
}

SDLOP_GLOffloadContext *SDLOP_GLOffload_CreateContext(uint32_t egl_api,
                                                      SDLOP_GLOffloadContext *share_ctx)
{
    if (!SDLOP_GLOffload_HasHardwareGPU()) {
        SDL_SetError("No hardware GPU available for headless GL offload");
        return NULL;
    }
    if (!SDLOP_EGL_eglBindAPI(egl_api)) {
        SDL_SetError("Offload eglBindAPI failed: 0x%x", SDLOP_EGL_eglGetError());
        return NULL;
    }

    EGLConfig config = NULL;
    uint32_t vis = offload.is_pbuffer ? 0 : offload.gbm_visual_id;
    if (!pick_offload_config(offload.dpy, egl_api, offload.is_pbuffer, vis,
                            sdlop_glattrs.red_size, sdlop_glattrs.green_size,
                            sdlop_glattrs.blue_size, sdlop_glattrs.alpha_size,
                            sdlop_glattrs.depth_size, sdlop_glattrs.stencil_size,
                            sdlop_glattrs.multisamplesamples, &config)) {
        SDL_SetError("Offload eglChooseConfig found no matching config");
        return NULL;
    }

    int base_attribs[16];
    int n = SDLOP_GLOffload_BuildContextAttribs(egl_api,
                                                sdlop_glattrs.major_version,
                                                sdlop_glattrs.minor_version,
                                                sdlop_glattrs.profile_mask,
                                                sdlop_glattrs.flags,
                                                base_attribs,
                                                (int)(sizeof(base_attribs) / sizeof(base_attribs[0])));
    if (n < 0) {
        return NULL;
    }

    EGLint ctx_attribs[64];
    for (int i = 0; i < n; i++) {
        ctx_attribs[i] = (EGLint)base_attribs[i];
    }
    n = SDLOP_EGL_AppendUserAttribs(true, offload.dpy, config, ctx_attribs, n,
                                    (int)(sizeof(ctx_attribs) / sizeof(ctx_attribs[0])));
    if (n < 0) {
        return NULL;
    }

    EGLContext share = share_ctx ? share_ctx->egl_ctx : EGL_NO_CONTEXT;
    EGLContext egl_ctx = SDLOP_EGL_eglCreateContext(offload.dpy, config, share, ctx_attribs);
    if (egl_ctx == EGL_NO_CONTEXT) {
        SDL_SetError("Offload eglCreateContext failed: 0x%x", SDLOP_EGL_eglGetError());
        return NULL;
    }

    SDLOP_GLOffloadContext *ctx = (SDLOP_GLOffloadContext *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        SDLOP_EGL_eglDestroyContext(offload.dpy, egl_ctx);
        SDL_OutOfMemory();
        return NULL;
    }
    ctx->egl_ctx = egl_ctx;
    ctx->api = egl_api;

    /* Update reported channel/buffer sizes from the chosen GPU config. */
    EGLint val = 0;
    if (SDLOP_EGL_eglGetConfigAttrib(offload.dpy, config, EGL_RED_SIZE, &val)) {
        sdlop_glattrs.red_size = val;
    }
    if (SDLOP_EGL_eglGetConfigAttrib(offload.dpy, config, EGL_GREEN_SIZE, &val)) {
        sdlop_glattrs.green_size = val;
    }
    if (SDLOP_EGL_eglGetConfigAttrib(offload.dpy, config, EGL_BLUE_SIZE, &val)) {
        sdlop_glattrs.blue_size = val;
    }
    if (SDLOP_EGL_eglGetConfigAttrib(offload.dpy, config, EGL_ALPHA_SIZE, &val)) {
        sdlop_glattrs.alpha_size = val;
    }
    if (SDLOP_EGL_eglGetConfigAttrib(offload.dpy, config, EGL_DEPTH_SIZE, &val)) {
        sdlop_glattrs.depth_size = val;
    }
    if (SDLOP_EGL_eglGetConfigAttrib(offload.dpy, config, EGL_STENCIL_SIZE, &val)) {
        sdlop_glattrs.stencil_size = val;
    }
    sdlop_glattrs.accelerated_visual = 1;
    return ctx;
}

SDLOP_GLOffloadContext *SDLOP_GLOffload_CreateContextAuto(SDLOP_GLOffloadContext *share_ctx)
{
    if (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_ES) {
        return SDLOP_GLOffload_CreateContext(EGL_OPENGL_ES_API, share_ctx);
    }
    SDLOP_GLOffloadContext *ctx = SDLOP_GLOffload_CreateContext(EGL_OPENGL_API, share_ctx);
    if (!ctx && sdlop_glattrs.profile_mask == 0) {
        ctx = SDLOP_GLOffload_CreateContext(EGL_OPENGL_ES_API, share_ctx);
    }
    return ctx;
}

void SDLOP_GLOffload_DeleteContext(SDLOP_GLOffloadContext *ctx)
{
    if (!ctx) {
        return;
    }
    if (offload.available && offload.dpy != EGL_NO_DISPLAY) {
        if (SDLOP_EGL_eglGetCurrentContext &&
            SDLOP_EGL_eglGetCurrentContext() == ctx->egl_ctx) {
            SDLOP_EGL_eglMakeCurrent(offload.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        SDLOP_EGL_eglDestroyContext(offload.dpy, ctx->egl_ctx);
    }
    free(ctx);
}

SDLOP_GLOffloadSurface *SDLOP_GLOffload_CreateSurface(SDLOP_GLOffloadContext *ctx, int w, int h)
{
    if (!offload.available || offload.dpy == EGL_NO_DISPLAY || !ctx) {
        SDL_SetError("Offload display/context not initialized");
        return NULL;
    }
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }

    EGLConfig config = config_for_offload_context(ctx);
    if (!config) {
        SDL_SetError("No EGL config for offload context");
        return NULL;
    }

    SDLOP_GLOffloadSurface *surf = (SDLOP_GLOffloadSurface *)calloc(1, sizeof(*surf));
    if (!surf) {
        SDL_OutOfMemory();
        return NULL;
    }
    surf->w = w;
    surf->h = h;
    surf->is_pbuffer = offload.is_pbuffer;

    if (offload.is_pbuffer) {
        EGLint pb_attribs[] = {
            EGL_WIDTH,  w,
            EGL_HEIGHT, h,
            EGL_NONE
        };
        surf->egl_surface = SDLOP_EGL_eglCreatePbufferSurface(offload.dpy, config, pb_attribs);
        if (surf->egl_surface == EGL_NO_SURFACE) {
            SDL_SetError("Offload eglCreatePbufferSurface(%dx%d) failed: 0x%x",
                         w, h, SDLOP_EGL_eglGetError());
            free(surf);
            return NULL;
        }
    } else {
#ifdef SDLop_VIDEO_KMSDRM
        surf->gbm_surf = SDLOP_GBM_gbm_surface_create(offload.gbm_dev, (uint32_t)w, (uint32_t)h,
                                                      offload.gbm_visual_id, GBM_BO_USE_RENDERING);
        if (!surf->gbm_surf) {
            SDL_SetError("Offload gbm_surface_create(%dx%d) failed", w, h);
            free(surf);
            return NULL;
        }
        surf->egl_surface = SDLOP_EGL_eglCreateWindowSurface(offload.dpy, config,
                                                             (EGLNativeWindowType)surf->gbm_surf, NULL);
        if (surf->egl_surface == EGL_NO_SURFACE) {
            SDL_SetError("Offload eglCreateWindowSurface(%dx%d) failed: 0x%x",
                         w, h, SDLOP_EGL_eglGetError());
            SDLOP_GBM_gbm_surface_destroy(surf->gbm_surf);
            free(surf);
            return NULL;
        }
        if (SDLOP_EGL_eglSurfaceAttrib) {
            SDLOP_EGL_eglSurfaceAttrib(offload.dpy, surf->egl_surface,
                                       EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED);
        }
#else
        SDL_SetError("Offload GBM surface unavailable in this build");
        free(surf);
        return NULL;
#endif
    }
    return surf;
}

void SDLOP_GLOffload_DestroySurface(SDLOP_GLOffloadSurface *surf)
{
    if (!surf) {
        return;
    }
    if (offload.available && offload.dpy != EGL_NO_DISPLAY) {
        if (SDLOP_EGL_eglGetCurrentSurface &&
            (SDLOP_EGL_eglGetCurrentSurface(EGL_DRAW) == surf->egl_surface ||
             SDLOP_EGL_eglGetCurrentSurface(EGL_READ) == surf->egl_surface)) {
            SDLOP_EGL_eglMakeCurrent(offload.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        if (surf->egl_surface != EGL_NO_SURFACE) {
            SDLOP_EGL_eglDestroySurface(offload.dpy, surf->egl_surface);
        }
    }
#ifdef SDLop_VIDEO_KMSDRM
    if (surf->gbm_surf) {
        SDLOP_GBM_gbm_surface_destroy(surf->gbm_surf);
    }
#endif
    free(surf);
}

bool SDLOP_GLOffload_ResizeSurface(SDLOP_GLOffloadContext *ctx,
                                   SDLOP_GLOffloadSurface **surf,
                                   int w, int h)
{
    if (!surf) {
        return false;
    }
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    if (*surf && (*surf)->w == w && (*surf)->h == h) {
        return true;
    }
    if (!ctx) {
        return false;
    }

    bool was_current = (SDLOP_EGL_eglGetCurrentContext &&
                        SDLOP_EGL_eglGetCurrentContext() == ctx->egl_ctx &&
                        *surf &&
                        SDLOP_EGL_eglGetCurrentSurface &&
                        SDLOP_EGL_eglGetCurrentSurface(EGL_DRAW) == (*surf)->egl_surface);

    SDLOP_GLOffloadSurface *next = ( *surf && (*surf)->is_prime )
        ? SDLOP_GLOffload_CreatePrimeSurface(ctx, w, h)
        : SDLOP_GLOffload_CreateSurface(ctx, w, h);
    if (!next) {
        return false;
    }
    if (was_current) {
        if (!SDLOP_GLOffload_MakeCurrent(ctx, next)) {
            SDLOP_GLOffload_DestroySurface(next);
            return false;
        }
    }
    SDLOP_GLOffload_DestroySurface(*surf);
    *surf = next;
    return true;
}

SDLOP_GLOffloadSurface *SDLOP_GLOffload_CreatePrimeSurface(SDLOP_GLOffloadContext *ctx, int w, int h)
{
#ifdef SDLop_VIDEO_KMSDRM
    if (!offload.available || offload.dpy == EGL_NO_DISPLAY || offload.is_pbuffer ||
        !offload.gbm_dev || !ctx) {
        return NULL;
    }
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    EGLConfig config = config_for_offload_context(ctx);
    if (!config) {
        return NULL;
    }

    const uint32_t usages[2] = {
        GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR,
        GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR,
    };

    for (int ui = 0; ui < 2; ui++) {
        if (!SDLOP_GBM_gbm_device_is_format_supported(offload.gbm_dev, offload.gbm_visual_id, usages[ui])) {
            continue;
        }
        struct gbm_surface *gs = SDLOP_GBM_gbm_surface_create(offload.gbm_dev,
                                                              (uint32_t)w, (uint32_t)h,
                                                              offload.gbm_visual_id, usages[ui]);
        if (!gs) {
            continue;
        }
        EGLSurface es = SDLOP_EGL_eglCreateWindowSurface(offload.dpy, config,
                                                         (EGLNativeWindowType)gs, NULL);
        if (es == EGL_NO_SURFACE) {
            SDLOP_GBM_gbm_surface_destroy(gs);
            continue;
        }
        SDLOP_GLOffloadSurface *surf = (SDLOP_GLOffloadSurface *)calloc(1, sizeof(*surf));
        if (!surf) {
            SDLOP_EGL_eglDestroySurface(offload.dpy, es);
            SDLOP_GBM_gbm_surface_destroy(gs);
            SDL_OutOfMemory();
            return NULL;
        }
        surf->w = w;
        surf->h = h;
        surf->is_pbuffer = false;
        surf->is_prime = true;
        surf->gbm_surf = gs;
        surf->egl_surface = es;
        if (SDLOP_EGL_eglSurfaceAttrib) {
            SDLOP_EGL_eglSurfaceAttrib(offload.dpy, surf->egl_surface,
                                       EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED);
        }
        return surf;
    }
    return NULL;
#else
    (void)ctx;
    (void)w;
    (void)h;
    return NULL;
#endif
}

bool SDLOP_GLOffload_IsPrimeSurface(const SDLOP_GLOffloadSurface *surf)
{
    return surf && surf->is_prime;
}

struct gbm_bo *SDLOP_GLOffload_SwapAndLockPrimeBO(SDLOP_GLOffloadContext *ctx,
                                                  SDLOP_GLOffloadSurface *surf)
{
#ifdef SDLop_VIDEO_KMSDRM
    if (!offload.available || offload.dpy == EGL_NO_DISPLAY || !ctx || !surf ||
        !surf->is_prime || !surf->gbm_surf || surf->egl_surface == EGL_NO_SURFACE) {
        return NULL;
    }
    if (!SDLOP_EGL_eglGetCurrentContext ||
        !SDLOP_EGL_eglGetCurrentSurface ||
        SDLOP_EGL_eglGetCurrentContext() != ctx->egl_ctx ||
        SDLOP_EGL_eglGetCurrentSurface(EGL_DRAW) != surf->egl_surface) {
        if (!SDLOP_GLOffload_MakeCurrent(ctx, surf)) {
            return NULL;
        }
    }
    if (!SDLOP_EGL_eglSwapBuffers(offload.dpy, surf->egl_surface)) {
        SDL_SetError("Offload eglSwapBuffers failed: 0x%x", SDLOP_EGL_eglGetError());
        return NULL;
    }
    struct gbm_bo *bo = SDLOP_GBM_gbm_surface_lock_front_buffer(surf->gbm_surf);
    if (!bo) {
        SDL_SetError("Offload gbm_surface_lock_front_buffer failed");
    }
    return bo;
#else
    (void)ctx;
    (void)surf;
    return NULL;
#endif
}

void SDLOP_GLOffload_ReleasePrimeBO(SDLOP_GLOffloadSurface *surf, struct gbm_bo *bo)
{
#ifdef SDLop_VIDEO_KMSDRM
    if (surf && surf->gbm_surf && bo && SDLOP_GBM_gbm_surface_release_buffer) {
        SDLOP_GBM_gbm_surface_release_buffer(surf->gbm_surf, bo);
    }
#else
    (void)surf;
    (void)bo;
#endif
}

void SDLOP_GLOffload_GetSurfaceSize(const SDLOP_GLOffloadSurface *surf, int *w, int *h)
{
    if (w) {
        *w = surf ? surf->w : 0;
    }
    if (h) {
        *h = surf ? surf->h : 0;
    }
}

bool SDLOP_GLOffload_MakeCurrent(SDLOP_GLOffloadContext *ctx, SDLOP_GLOffloadSurface *surf)
{
    if (!offload.available || offload.dpy == EGL_NO_DISPLAY) {
        return SDL_SetError("Offload EGL display not initialized");
    }
    if (!ctx) {
        SDLOP_EGL_eglMakeCurrent(offload.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return true;
    }
    if (!SDLOP_EGL_eglBindAPI(ctx->api)) {
        return SDL_SetError("Offload eglBindAPI failed: 0x%x", SDLOP_EGL_eglGetError());
    }
    EGLSurface egl_surf = surf ? surf->egl_surface : EGL_NO_SURFACE;
    if (!SDLOP_EGL_eglMakeCurrent(offload.dpy, egl_surf, egl_surf, ctx->egl_ctx)) {
        return SDL_SetError("Offload eglMakeCurrent failed: 0x%x", SDLOP_EGL_eglGetError());
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Readback + top-down XRGB8888 copy                                   */
/* ------------------------------------------------------------------ */

void SDLOP_GLOffload_FlipCopyToXRGB8888(const uint8_t *src_bottom_up,
                                        int src_w, int src_h,
                                        bool src_is_bgra,
                                        void *dst_pixels, int dst_pitch,
                                        int dst_w, int dst_h)
{
    if (!src_bottom_up || !dst_pixels || src_w <= 0 || src_h <= 0 ||
        dst_w <= 0 || dst_h <= 0 || dst_pitch <= 0) {
        return;
    }
    int copy_w = src_w < dst_w ? src_w : dst_w;
    int copy_h = src_h < dst_h ? src_h : dst_h;
    size_t src_stride = (size_t)src_w * 4u;
    size_t row_bytes = (size_t)copy_w * 4u;
    uint8_t *dst_base = (uint8_t *)dst_pixels;

    for (int y = 0; y < copy_h; y++) {
        /* Bottom-up GL row (src_h - 1 - y) maps to top-down dst row y. */
        const uint8_t *src_row = src_bottom_up + (size_t)(src_h - 1 - y) * src_stride;
        uint8_t *dst_row = dst_base + (size_t)y * (size_t)dst_pitch;

        if (src_is_bgra) {
            memcpy(dst_row, src_row, row_bytes);
        } else {
            uint32_t *dst32 = (uint32_t *)dst_row;
            for (int x = 0; x < copy_w; x++) {
                uint32_t r = src_row[x * 4 + 0];
                uint32_t g = src_row[x * 4 + 1];
                uint32_t b = src_row[x * 4 + 2];
                dst32[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
}

bool SDLOP_GLOffload_ReadToXRGB8888(SDLOP_GLOffloadContext *ctx,
                                    SDLOP_GLOffloadSurface *surf,
                                    void *dst_pixels, int dst_pitch,
                                    int dst_w, int dst_h)
{
    if (!offload.available || offload.dpy == EGL_NO_DISPLAY || !ctx || !surf || !dst_pixels) {
        return SDL_SetError("Invalid offload readback arguments");
    }

    SDLOP_PFN_glReadPixels p_read = (SDLOP_PFN_glReadPixels)lookup_gl_proc("glReadPixels");
    SDLOP_PFN_glGetIntegerv p_getiv = (SDLOP_PFN_glGetIntegerv)lookup_gl_proc("glGetIntegerv");
    SDLOP_PFN_glPixelStorei p_pixstore = (SDLOP_PFN_glPixelStorei)lookup_gl_proc("glPixelStorei");
    SDLOP_PFN_glBindFramebuffer p_bindfb = (SDLOP_PFN_glBindFramebuffer)lookup_gl_proc("glBindFramebuffer");
    SDLOP_PFN_glGetError p_geterr = (SDLOP_PFN_glGetError)lookup_gl_proc("glGetError");
    SDLOP_PFN_glFinish p_finish = (SDLOP_PFN_glFinish)lookup_gl_proc("glFinish");

    if (!p_read) {
        return SDL_SetError("glReadPixels not available for offload readback");
    }

    size_t needed = (size_t)surf->w * (size_t)surf->h * 4u;
    if (needed > offload.staging_cap) {
        uint8_t *buf = (uint8_t *)realloc(offload.staging, needed);
        if (!buf) {
            return SDL_OutOfMemory();
        }
        offload.staging = buf;
        offload.staging_cap = needed;
    }

    /* Ensure the target context + surface is current; restore afterwards if we switched. */
    SavedEGLState saved;
    bool switched = false;
    if (!SDLOP_EGL_eglGetCurrentContext ||
        !SDLOP_EGL_eglGetCurrentSurface ||
        SDLOP_EGL_eglGetCurrentContext() != ctx->egl_ctx ||
        SDLOP_EGL_eglGetCurrentSurface(EGL_READ) != surf->egl_surface) {
        save_egl_state(&saved);
        if (!SDLOP_GLOffload_MakeCurrent(ctx, surf)) {
            return false;
        }
        switched = true;
    }

    /* Temporarily ensure framebuffer 0 is bound for reading and packing is tight. */
    int prev_fbo = 0;
    unsigned int fbo_target = GL_FRAMEBUFFER_;
    bool restore_fbo = false;
    if (p_getiv && p_bindfb) {
        if (p_geterr) {
            for (int i = 0; i < 16 && p_geterr() != GL_NO_ERROR_; i++) {}
        }
        p_getiv(GL_READ_FRAMEBUFFER_BINDING_, &prev_fbo);
        if (p_geterr && p_geterr() == GL_INVALID_ENUM_) {
            p_getiv(GL_FRAMEBUFFER_BINDING_, &prev_fbo);
            fbo_target = GL_FRAMEBUFFER_;
        } else {
            fbo_target = GL_READ_FRAMEBUFFER_;
        }
        if (prev_fbo != 0) {
            p_bindfb(fbo_target, 0);
            restore_fbo = true;
        }
    }

    int prev_align = 4;
    int prev_rowlen = 0;
    bool restore_rowlen = false;
    if (p_getiv && p_pixstore) {
        p_getiv(GL_PACK_ALIGNMENT_, &prev_align);
        if (prev_align != 4) {
            p_pixstore(GL_PACK_ALIGNMENT_, 4);
        }
        if (p_geterr) {
            for (int i = 0; i < 16 && p_geterr() != GL_NO_ERROR_; i++) {}
        }
        p_getiv(GL_PACK_ROW_LENGTH_, &prev_rowlen);
        if ((!p_geterr || p_geterr() == GL_NO_ERROR_) && prev_rowlen != 0) {
            p_pixstore(GL_PACK_ROW_LENGTH_, 0);
            restore_rowlen = true;
        }
    }

    if (p_finish) {
        p_finish();
    }

    bool used_bgra = false;
    if (!offload.bgra_checked || offload.bgra_supported) {
        if (p_geterr) {
            for (int i = 0; i < 16 && p_geterr() != GL_NO_ERROR_; i++) {}
        }
        p_read(0, 0, surf->w, surf->h, GL_BGRA_, GL_UNSIGNED_BYTE_, offload.staging);
        unsigned int err = p_geterr ? p_geterr() : GL_NO_ERROR_;
        if (err == GL_NO_ERROR_) {
            offload.bgra_checked = true;
            offload.bgra_supported = true;
            used_bgra = true;
        } else if (!offload.bgra_checked) {
            offload.bgra_checked = true;
            offload.bgra_supported = false;
            p_read(0, 0, surf->w, surf->h, GL_RGBA_, GL_UNSIGNED_BYTE_, offload.staging);
        }
    } else {
        p_read(0, 0, surf->w, surf->h, GL_RGBA_, GL_UNSIGNED_BYTE_, offload.staging);
    }

    /* Restore GL packing + FBO state cleanly. */
    if (p_pixstore) {
        if (prev_align != 4) {
            p_pixstore(GL_PACK_ALIGNMENT_, prev_align);
        }
        if (restore_rowlen) {
            p_pixstore(GL_PACK_ROW_LENGTH_, prev_rowlen);
        }
    }
    if (restore_fbo) {
        p_bindfb(fbo_target, (unsigned int)prev_fbo);
    }

#ifdef SDLop_VIDEO_KMSDRM
    if (!surf->is_pbuffer && !surf->is_prime && surf->gbm_surf) {
        SDLOP_EGL_eglSwapBuffers(offload.dpy, surf->egl_surface);
        struct gbm_bo *bo = SDLOP_GBM_gbm_surface_lock_front_buffer(surf->gbm_surf);
        if (bo) {
            SDLOP_GBM_gbm_surface_release_buffer(surf->gbm_surf, bo);
        }
    }
#endif

    if (switched) {
        restore_egl_state(&saved);
    }

    SDLOP_GLOffload_FlipCopyToXRGB8888(offload.staging, surf->w, surf->h, used_bgra,
                                        dst_pixels, dst_pitch, dst_w, dst_h);
    return true;
}

void SDLOP_GLOffload_Shutdown(void)
{
    if (offload.dpy != EGL_NO_DISPLAY && SDLOP_EGL_eglTerminate) {
        if (SDLOP_EGL_eglMakeCurrent) {
            SDLOP_EGL_eglMakeCurrent(offload.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        SDLOP_EGL_eglTerminate(offload.dpy);
        offload.dpy = EGL_NO_DISPLAY;
    }
#ifdef SDLop_VIDEO_KMSDRM
    if (offload.gbm_dev && SDLOP_GBM_gbm_device_destroy) {
        SDLOP_GBM_gbm_device_destroy(offload.gbm_dev);
        offload.gbm_dev = NULL;
    }
    if (offload.gbm_ref_held) {
        SDLOP_GBM_UnloadSymbols();
        offload.gbm_ref_held = false;
    }
#endif
    if (offload.render_fd >= 0) {
        close(offload.render_fd);
        offload.render_fd = -1;
    }
    if (offload.egl_ref_held) {
        SDLOP_EGL_UnloadSymbols();
        offload.egl_ref_held = false;
    }
    if (offload.gl_fallback_lib) {
        dlclose(offload.gl_fallback_lib);
        offload.gl_fallback_lib = NULL;
    }
    free(offload.staging);
    offload.staging = NULL;
    offload.staging_cap = 0;

    offload.probed = false;
    offload.available = false;
    offload.prefer_gbm = false;
    offload.is_pbuffer = false;
    offload.gbm_visual_id = 0;
    offload.gl_fallback_tried = false;
    offload.bgra_checked = false;
    offload.bgra_supported = false;
    offload.desc[0] = '\0';
}
