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

#ifndef DRM_FORMAT_ARGB8888
#define DRM_FORMAT_ARGB8888 0x34325241u /* 'AR24' */
#endif

typedef struct KMSDRM_DisplayData
{
    drmModeConnector *connector;    /* owned */
    drmModeCrtc *crtc;              /* owned */
    drmModeModeInfo mode;           /* mode selected for the window */
    drmModeModeInfo original_mode;  /* CRTC mode at init, restored on quit */
    uint32_t original_fb_id;        /* CRTC buffer at init (0 if none) */
    char name[32];                  /* e.g. "HDMI-A-1" */

    /* variable refresh rate (VRR): saved at init, re-applied on quit */
    bool saved_vrr;                 /* VRR state at init (restored on quit) */
    bool vrr_enabled;               /* we enabled it (log/restore bookkeeping) */
    int panel_orientation_degrees;  /* connector "panel orientation": 0/90/180/270 (SDLop has no display properties API; logged at init) */

    /* atomic modesetting state (available only after KMSDRM_AtomicTryInit) */
    struct KMSDRM_AtomicData {
        bool available;             /* client caps set + primary plane found + syms loaded */
        uint32_t primary_plane;     /* plane id of the CRTC's primary plane */
        uint32_t mode_blob;         /* drmModeCreatePropertyBlob for dd->mode (0 = none) */
        uint32_t plane_prop_fb_id;
        uint32_t plane_prop_crtc_id;
        uint32_t plane_prop_src_x, plane_prop_src_y, plane_prop_src_w, plane_prop_src_h;
        uint32_t plane_prop_crtc_x, plane_prop_crtc_y, plane_prop_crtc_w, plane_prop_crtc_h;
        uint32_t crtc_prop_active;
        uint32_t crtc_prop_mode_id;
        uint32_t conn_prop_crtc_id;
    } atomic;

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
void KMSDRM_FlipHandler(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
                        unsigned int crtc_id, void *data);
void KMSDRM_DestroySurfaces(KMSDRM_DeviceData *viddata, SDL_Window *window);
bool KMSDRM_RecreateScanout(SDLop_VideoDevice *device, SDL_Window *window);

KMSDRM_DeviceData *KMSDRM_GetDevice(void);
void KMSDRM_DestroySwScanout(SDL_Window *window);
/* Atomic modesetting (sdlop_kmsdrm_atomic.c): enter on first present /
 * mode reprogram; page flip via drmModeAtomicCommit otherwise. Falls back
 * to the legacy drmModeSetCrtc/drmModePageFlip path internally on failure. */
void KMSDRM_AtomicTryInit(KMSDRM_DeviceData *vid, KMSDRM_DisplayData *dd);
bool KMSDRM_AtomicModeset(KMSDRM_DeviceData *vid, KMSDRM_DisplayData *dd, uint32_t fb_id);
int KMSDRM_AtomicFlip(KMSDRM_DeviceData *vid, KMSDRM_DisplayData *dd, uint32_t fb_id,
                      void *flip_cookie);   /* 0 queued, -1 error (errno set) */
bool KMSDRM_AtomicEnabled(void);    /* env knob: SDLOP_KMSDRM_ATOMIC=0 disables */
/* Union of fourccs advertised by every plane on the device (all CRTCs);
 * used to pick a GL scanout format the KMS planes actually accept
 * (virtio-gpu 2D's primary plane is XR24-only). Returns false when the
 * plane API is unavailable. */
bool KMSDRM_PlaneFormatsUnion(int fd, uint32_t *xrgb8888, uint32_t *argb8888);
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

/* Vulkan WSI (VK_KHR_surface + VK_KHR_display) - sdlop_kmsdrm_vulkan.c */
bool SDLOP_KMSDRM_Vulkan_CreateSurface(SDLop_VideoDevice *device, SDL_Window *window,
                                       void *instance, const void *allocator, Uint64 *surface);
const char *const *SDLOP_KMSDRM_Vulkan_GetInstanceExtensions(SDLop_VideoDevice *device, Uint32 *count);

/* shared bits for the Vulkan module (sdlop_kmsdrm_vulkan.c) */
int KMSDRM_ModeRefreshMHz(const drmModeModeInfo *m);

#endif /* sdlop_kmsdrm_h_ */
