/*
  SDLop - GPU present offload, Windows build.

  On Linux SDLop can render on a headless GPU (EGL + GBM over /dev/dri) and
  copy the frames into a software scanout, which is how the fbdev and KMS/DRM
  drivers stay hardware accelerated. Windows has no GBM and its offload
  decisions are made by the graphics driver, so this build provides the same
  entry points as no-ops: every probe reports "no offload available" and the
  callers fall back to their normal path, which is exactly what
  SDLOP_GLOffload_GetPolicy() returning NEVER means.

  Keeping the API (rather than #ifdef-ing the call sites) means SDL_gl.c,
  the Vulkan code and the tests all compile unchanged on both platforms.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_gl_offload.h"

#ifdef _WIN32

#include "internal/sdlop_internal.h"

/* SDL_gl.c calls this for a transparent window's GL swap. There is no GL
 * offload context on Win32 (GetPolicy() is NEVER, so ctx is always NULL),
 * and returning false makes SDL_GL_SwapWindow report the failure rather than
 * silently drop the frame's alpha. */
bool SDLOP_GLOffload_ReadToARGB8888(SDLOP_GLOffloadContext *ctx, SDLOP_GLOffloadSurface *surf,
                                    void *dst_pixels, int dst_pitch, int dst_w, int dst_h, bool keep_alpha)
{
    (void)ctx; (void)surf; (void)dst_pixels; (void)dst_pitch; (void)dst_w; (void)dst_h; (void)keep_alpha;
    return SDL_SetError("GL offload readback is not available on this platform");
}

void SDLOP_GLOffload_SetPreferGBM(bool prefer)
{
    (void)prefer;
}

bool SDLOP_GLOffload_HasGBM(void)
{
    return false;
}

SDLOP_GLOffloadPolicy SDLOP_GLOffload_GetPolicy(void)
{
    /* Offload is a Linux feature; honour an explicit "never" and otherwise
     * stay off, so SDL_gl.c takes the normal WGL path. Options are read
     * through the hint layer so SDL_SetHint works, like every other knob. */
    static const char *const names[] = { SDLOP_HINT_GL_OFFLOAD, SDLOP_HINT_OFFLOAD, NULL };
    const char *env = SDLOP_HintFirst(names);
    if (env && (SDL_strcasecmp(env, "force") == 0 || SDL_strcmp(env, "1") == 0 ||
                SDL_strcasecmp(env, "on") == 0 || SDL_strcasecmp(env, "true") == 0 ||
                SDL_strcasecmp(env, "yes") == 0)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "SDLOP_GL_OFFLOAD=force is not supported on Windows, ignoring");
    }
    return SDLOP_GL_OFFLOAD_NEVER;
}

bool SDLOP_GLOffload_IsSoftwareRenderer(const char *renderer)
{
    /* Still useful on Windows: the driver name is shown to the app. */
    if (!renderer) {
        return false;
    }
    static const char *const known[] = {
        "llvmpipe", "softpipe", "swrast", "software rasterizer", "openswr", "swr", "lavapipe"
    };
    for (size_t i = 0; i < SDL_arraysize(known); i++) {
        if (SDL_strcasestr(renderer, known[i])) {
            return true;
        }
    }
    return false;
}

const char *SDLOP_GLOffload_QueryCurrentRenderer(void)
{
    return NULL;
}

bool SDLOP_GLOffload_HasHardwareGPU(void)
{
    return false; /* no EGL device probe without EGL/GBM */
}

const char *SDLOP_GLOffload_GetGPUDescription(void)
{
    return NULL;
}

int SDLOP_GLOffload_BuildContextAttribs(uint32_t egl_api, int major_version, int minor_version,
                                        int profile_mask, int flags, int *attribs, int max_attribs)
{
    (void)egl_api; (void)major_version; (void)minor_version;
    (void)profile_mask; (void)flags; (void)attribs; (void)max_attribs;
    return 0;
}

SDLOP_GLOffloadContext *SDLOP_GLOffload_CreateContext(uint32_t egl_api, SDLOP_GLOffloadContext *share_ctx)
{
    (void)egl_api; (void)share_ctx;
    SDL_SetError("GPU present offload is not available on Windows");
    return NULL;
}

SDLOP_GLOffloadContext *SDLOP_GLOffload_CreateContextAuto(SDLOP_GLOffloadContext *share_ctx)
{
    (void)share_ctx;
    SDL_SetError("GPU present offload is not available on Windows");
    return NULL;
}

void SDLOP_GLOffload_DeleteContext(SDLOP_GLOffloadContext *ctx)
{
    (void)ctx;
}

SDLOP_GLOffloadSurface *SDLOP_GLOffload_CreateSurface(SDLOP_GLOffloadContext *ctx, int w, int h)
{
    (void)ctx; (void)w; (void)h;
    return NULL;
}

SDLOP_GLOffloadSurface *SDLOP_GLOffload_CreatePrimeSurface(SDLOP_GLOffloadContext *ctx, int w, int h)
{
    (void)ctx; (void)w; (void)h;
    return NULL;
}

bool SDLOP_GLOffload_ResizeSurface(SDLOP_GLOffloadContext *ctx, SDLOP_GLOffloadSurface **surf, int w, int h)
{
    (void)ctx; (void)surf; (void)w; (void)h;
    return false;
}

void SDLOP_GLOffload_DestroySurface(SDLOP_GLOffloadSurface *surf)
{
    (void)surf;
}

void SDLOP_GLOffload_GetSurfaceSize(const SDLOP_GLOffloadSurface *surf, int *w, int *h)
{
    (void)surf;
    if (w) {
        *w = 0;
    }
    if (h) {
        *h = 0;
    }
}

bool SDLOP_GLOffload_IsPrimeSurface(const SDLOP_GLOffloadSurface *surf)
{
    (void)surf;
    return false;
}

struct gbm_bo *SDLOP_GLOffload_SwapAndLockPrimeBO(SDLOP_GLOffloadContext *ctx, SDLOP_GLOffloadSurface *surf)
{
    (void)ctx; (void)surf;
    return NULL;
}

void SDLOP_GLOffload_ReleasePrimeBO(SDLOP_GLOffloadSurface *surf, struct gbm_bo *bo)
{
    (void)surf; (void)bo;
}

bool SDLOP_GLOffload_MakeCurrent(SDLOP_GLOffloadContext *ctx, SDLOP_GLOffloadSurface *surf)
{
    (void)ctx; (void)surf;
    return false;
}

SDL_FunctionPointer SDLOP_GLOffload_GetProcAddress(const char *proc)
{
    (void)proc;
    return NULL;
}

void SDLOP_GLOffload_FlipCopyToXRGB8888(const uint8_t *src_bottom_up, int src_w, int src_h,
                                        SDLOP_GLOffloadPixelFormat src_format,
                                        void *dst_pixels, int dst_pitch, int dst_w, int dst_h)
{
    /* A pure pixel helper with no EGL/GBM dependency: keep it working so that
     * callers shared with the Linux path behave identically. */
    if (!src_bottom_up || !dst_pixels || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }
    const int w = src_w < dst_w ? src_w : dst_w;
    const int h = src_h < dst_h ? src_h : dst_h;
    const int src_bpp = (src_format == SDLOP_GL_OFFLOAD_RGB888) ? 3 : 4;
    uint8_t *dst = (uint8_t *)dst_pixels;
    for (int y = 0; y < h; y++) {
        const uint8_t *srow = src_bottom_up + (size_t)(src_h - 1 - y) * (size_t)src_w * (size_t)src_bpp;
        uint32_t *drow = (uint32_t *)(void *)(dst + (size_t)y * (size_t)dst_pitch);
        for (int x = 0; x < w; x++) {
            const uint8_t *p = srow + (size_t)x * (size_t)src_bpp;
            uint32_t rgb;
            if (src_format == SDLOP_GL_OFFLOAD_BGRA8888) {
                rgb = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
            } else {
                rgb = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
            }
            drow[x] = rgb;
        }
    }
}

bool SDLOP_GLOffload_ReadToXRGB8888(SDLOP_GLOffloadContext *ctx, SDLOP_GLOffloadSurface *surf,
                                    void *dst_pixels, int dst_pitch, int dst_w, int dst_h)
{
    (void)ctx; (void)surf; (void)dst_pixels; (void)dst_pitch; (void)dst_w; (void)dst_h;
    return false;
}

void SDLOP_GLOffload_Shutdown(void)
{
}

#endif /* _WIN32 */
