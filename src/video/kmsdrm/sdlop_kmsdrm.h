/*
  SDLop - KMS/DRM video driver internals.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_kmsdrm_h_
#define sdlop_kmsdrm_h_

#include "internal/sdlop_internal.h"
#include "sdlop_kmsdrm_dyn.h"

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <SDL3/SDL.h>

#ifndef EGL_PLATFORM_GBM_MESA
#define EGL_PLATFORM_GBM_MESA 0x31D7
#endif

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#endif

#ifndef DRM_MODE_FB_MODIFIERS
#define DRM_MODE_FB_MODIFIERS	2
#endif

#ifndef DRM_MODE_PAGE_FLIP_ASYNC
#define DRM_MODE_PAGE_FLIP_ASYNC    2
#endif

#ifndef DRM_CAP_ASYNC_PAGE_FLIP
#define DRM_CAP_ASYNC_PAGE_FLIP 7
#endif

#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH    8
#endif

#ifndef DRM_CAP_CURSOR_HEIGHT
#define DRM_CAP_CURSOR_HEIGHT   9
#endif

#ifndef DRM_FORMAT_XRGB8888
#define DRM_FORMAT_XRGB8888 0x34325258u /* 'XR24' */
#endif

typedef struct KMSDRM_DisplayData
{
    drmModeConnector *connector;    /* owned */
    drmModeCrtc *crtc;              /* owned */
    drmModeModeInfo mode;           /* mode selected for the window */
    drmModeModeInfo original_mode;  /* CRTC mode at init, restored on quit */
    uint32_t original_fb_id;        /* CRTC buffer at init (0 if none) */
    char name[32];                  /* e.g. "HDMI-A-1" */

    /* hardware cursor */
    struct gbm_bo *cursor_bo;
    uint64_t cursor_w, cursor_h;
    int32_t cursor_hot_x, cursor_hot_y;
    bool cursor_visible;
    bool default_cursor_shown;
} KMSDRM_DisplayData;

typedef struct KMSDRM_WindowData
{
    KMSDRM_DisplayData *display;

    /* ---- software (dumb buffer) scanout ---- */
    bool sw_active;                 /* dumb-buffer path in use */
    uint32_t sw_handle[2];
    uint32_t sw_fb_id[2];
    uint32_t sw_pitch;
    uint64_t sw_size;
    void *sw_map[2];                /* mmap'd dumb buffers */
    int sw_draw;                    /* index of the buffer the app draws into */
    int sw_front;                   /* index of the buffer programmed/flipped to the CRTC (-1: none yet) */

    /* ---- GL (GBM) scanout ---- */
    bool gl_active;
    struct gbm_surface *gs;
    struct gbm_bo *bo;              /* buffer currently on screen */
    struct gbm_bo *next_bo;         /* buffer pending front-buffer lock */
    EGLSurface egl_surface;
    bool egl_surface_dirty;         /* mode changed: recreate GBM/EGL surfaces on next swap */

    /* ---- shared ---- */
    bool waiting_for_flip;          /* a pageflip was issued; flip event pending */
    bool crtc_programmed;           /* SetCrtc has been called at least once */
    bool lost_master;               /* last modeset/flip failed with EPERM/EACCES */
    Uint32 clear_color;
    float cursor_x, cursor_y;       /* last cursor position applied (window coords) */
    bool cursor_pos_valid;
} KMSDRM_WindowData;

typedef struct KMSDRM_FBInfo
{
    int drm_fd;
    uint32_t fb_id;
} KMSDRM_FBInfo;

typedef struct KMSDRM_DeviceData
{
    int devindex;
    int drm_fd;
    char devpath[32];

    KMSDRM_DisplayData display;     /* primary (and only) display driven by this device */

    struct gbm_device *gbm_dev;
    bool gbm_init;
    bool video_init;
    bool async_pageflip_support;
    bool no_flip;                   /* SDLOP_KMSDRM_NOFLIP=1: always modeset instead of page flip */
} KMSDRM_DeviceData;

/* fb lifetime helper shared by the GL path */
KMSDRM_FBInfo *KMSDRM_FBFromBO(int drm_fd, struct gbm_bo *bo);
bool KMSDRM_WaitPageflip(KMSDRM_DeviceData *viddata, KMSDRM_WindowData *windata, int timeout_ms);
void KMSDRM_FlipHandler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data);
void KMSDRM_DestroySurfaces(KMSDRM_DeviceData *viddata, SDL_Window *window);
bool KMSDRM_RecreateScanout(SDLop_VideoDevice *device, SDL_Window *window);

KMSDRM_DeviceData *KMSDRM_GetDevice(void);
void KMSDRM_DestroySwScanout(SDL_Window *window);
void KMSDRM_DestroyGLSurfaces(KMSDRM_WindowData *windata);
void KMSDRM_GL_Deinit(void);

/* GL (EGL/GBM) thunks - sdlop_kmsdrm_gl.c */
void *KMSDRM_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window);
bool KMSDRM_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context);
bool KMSDRM_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window);
void KMSDRM_GL_DeleteContext(SDLop_VideoDevice *device, void *context);
SDL_FunctionPointer KMSDRM_GL_GetProcAddress(SDLop_VideoDevice *device, const char *proc);
bool KMSDRM_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval);
bool KMSDRM_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval);

#endif /* sdlop_kmsdrm_h_ */
