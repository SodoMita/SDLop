/*
  SDLop - Headless GPU OpenGL offload for software-rendered outputs.

  Detects when the active display/compositor GL path is software-rendered
  (or lacks hardware 3D support, e.g. display-only KMS devices or wl_shm-only
  Wayland compositors) while a hardware GPU exists on the system. Renders
  offscreen on the hardware GPU (EGL_PLATFORM_DEVICE_EXT pbuffer, with a
  /dev/dri/renderD* GBM fallback) and copies the finished framebuffer into
  the output's software buffer (KMS dumb buffer or Wayland wl_shm).

  Controlled by SDLOP_GL_OFFLOAD:
    0 / off / false / no    - disable offload
    1 / on / true / force   - force offload when a hardware GPU is available
    unset / auto            - automatic when output is software + GPU exists

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_gl_offload_h_
#define sdlop_gl_offload_h_

#include "internal/sdlop_internal.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum SDLOP_GLOffloadPolicy
{
    SDLOP_GL_OFFLOAD_AUTO = 0,
    SDLOP_GL_OFFLOAD_NEVER = 1,
    SDLOP_GL_OFFLOAD_FORCE = 2,
} SDLOP_GLOffloadPolicy;

typedef struct SDLOP_GLOffloadContext SDLOP_GLOffloadContext;
typedef struct SDLOP_GLOffloadSurface SDLOP_GLOffloadSurface;
struct gbm_bo;

/* Prefer /dev/dri/renderD* GBM offload over EGL_PLATFORM_DEVICE_EXT pbuffer
 * when probing for a hardware GPU (used by KMS/DRM to enable PRIME scanout). */
void SDLOP_GLOffload_SetPreferGBM(bool prefer);

/* True when the active offload backend uses a GBM render node (PRIME-capable). */
bool SDLOP_GLOffload_HasGBM(void);

/* Read SDLOP_GL_OFFLOAD (falling back to SDLOP_OFFLOAD) env var. */
SDLOP_GLOffloadPolicy SDLOP_GLOffload_GetPolicy(void);

/* Case-insensitive check for software GL_RENDERER strings
 * (llvmpipe, softpipe, swrast, Software Rasterizer, OpenSWR/SWR, lavapipe). */
bool SDLOP_GLOffload_IsSoftwareRenderer(const char *renderer);

/* Query GL_RENDERER on the currently bound GL/GLES context via dynamic lookup. */
const char *SDLOP_GLOffload_QueryCurrentRenderer(void);

/* Probe whether a hardware GPU EGL device is available for headless offload.
 * Preserves any active EGL display/context/surface/API on the calling thread. */
bool SDLOP_GLOffload_HasHardwareGPU(void);

/* Human-readable GPU description (valid while offload is initialized), or NULL. */
const char *SDLOP_GLOffload_GetGPUDescription(void);

/* Build EGL context attribute list (without EGL_NONE terminator) for `egl_api`
 * (`EGL_OPENGL_API` = 0x30A2 or `EGL_OPENGL_ES_API` = 0x30A0). Returns the
 * number of EGLint entries written, or -1 if `max_attribs` is insufficient. */
int SDLOP_GLOffload_BuildContextAttribs(uint32_t egl_api,
                                        int major_version,
                                        int minor_version,
                                        int profile_mask,
                                        int flags,
                                        int *attribs,
                                        int max_attribs);

/* Offload context lifecycle (egl_api is EGL_OPENGL_API or EGL_OPENGL_ES_API). */
SDLOP_GLOffloadContext *SDLOP_GLOffload_CreateContext(uint32_t egl_api,
                                                      SDLOP_GLOffloadContext *share_ctx);
/* Pick API from sdlop_glattrs (with ES fallback when desktop GL fails). */
SDLOP_GLOffloadContext *SDLOP_GLOffload_CreateContextAuto(SDLOP_GLOffloadContext *share_ctx);
void SDLOP_GLOffload_DeleteContext(SDLOP_GLOffloadContext *ctx);

/* Offscreen GPU surface lifecycle. */
SDLOP_GLOffloadSurface *SDLOP_GLOffload_CreateSurface(SDLOP_GLOffloadContext *ctx, int w, int h);
SDLOP_GLOffloadSurface *SDLOP_GLOffload_CreatePrimeSurface(SDLOP_GLOffloadContext *ctx, int w, int h);
bool SDLOP_GLOffload_ResizeSurface(SDLOP_GLOffloadContext *ctx,
                                   SDLOP_GLOffloadSurface **surf,
                                   int w, int h);
void SDLOP_GLOffload_DestroySurface(SDLOP_GLOffloadSurface *surf);
void SDLOP_GLOffload_GetSurfaceSize(const SDLOP_GLOffloadSurface *surf, int *w, int *h);
bool SDLOP_GLOffload_IsPrimeSurface(const SDLOP_GLOffloadSurface *surf);

/* Swap buffers on a PRIME offload surface and lock its linear front BO. */
struct gbm_bo *SDLOP_GLOffload_SwapAndLockPrimeBO(SDLOP_GLOffloadContext *ctx,
                                                  SDLOP_GLOffloadSurface *surf);
void SDLOP_GLOffload_ReleasePrimeBO(SDLOP_GLOffloadSurface *surf, struct gbm_bo *bo);

/* Bind offload context + surface (pass ctx == NULL to unbind). */
bool SDLOP_GLOffload_MakeCurrent(SDLOP_GLOffloadContext *ctx, SDLOP_GLOffloadSurface *surf);

/* Resolve GL procedure via offload EGL + libGLESv2/libGL fallback. */
SDL_FunctionPointer SDLOP_GLOffload_GetProcAddress(const char *proc);

typedef enum SDLOP_GLOffloadPixelFormat
{
    SDLOP_GL_OFFLOAD_RGBA8888 = 0,
    SDLOP_GL_OFFLOAD_BGRA8888 = 1,
    SDLOP_GL_OFFLOAD_RGB888 = 2,
} SDLOP_GLOffloadPixelFormat;

/* Pure pixel-copy helper: flip a bottom-up RGBA/BGRA/RGB buffer into a
 * top-down XRGB8888 buffer (respecting dst_pitch and clipping to min dims). */
void SDLOP_GLOffload_FlipCopyToXRGB8888(const uint8_t *src_bottom_up,
                                        int src_w, int src_h,
                                        SDLOP_GLOffloadPixelFormat src_format,
                                        void *dst_pixels, int dst_pitch,
                                        int dst_w, int dst_h);

/* The same, keeping the source alpha when `keep_alpha` (ARGB8888 targets
 * of SDL_WINDOW_TRANSPARENT windows); RGB888 sources are opaque. */
void SDLOP_GLOffload_FlipCopyToARGB8888(const uint8_t *src_bottom_up,
                                        int src_w, int src_h,
                                        SDLOP_GLOffloadPixelFormat src_format,
                                        void *dst_pixels, int dst_pitch,
                                        int dst_w, int dst_h, bool keep_alpha);

/* Read framebuffer 0 of `surf` on `ctx` and copy top-down XRGB8888 into
 * `dst_pixels` (`dst_pitch` bytes/row, `dst_w` x `dst_h`). */
bool SDLOP_GLOffload_ReadToXRGB8888(SDLOP_GLOffloadContext *ctx,
                                    SDLOP_GLOffloadSurface *surf,
                                    void *dst_pixels, int dst_pitch,
                                    int dst_w, int dst_h);

/* The same into ARGB8888, with the framebuffer's alpha when `keep_alpha`
 * (a four-channel read format is used then). */
bool SDLOP_GLOffload_ReadToARGB8888(SDLOP_GLOffloadContext *ctx,
                                    SDLOP_GLOffloadSurface *surf,
                                    void *dst_pixels, int dst_pitch,
                                    int dst_w, int dst_h, bool keep_alpha);

/* Release cached offload EGL / GBM resources and reset probe cache. */
void SDLOP_GLOffload_Shutdown(void);

#endif /* sdlop_gl_offload_h_ */
