/*
  SDLop - atomic modesetting for the KMSDRM driver.

  Approach derived from swa (github.com/nyorain/swa, src/swa/kms/kms.c -
  MIT licensed, see that project) with SDLop-specific changes:

  - no dlg / pml: SDLop logging macros and the existing PumpEvents /
    KMSDRM_WaitPageflip event flow are used instead;
  - the primary plane is found via possible_crtcs + the plane "type"
    property, so this works also when the CRTC is inactive at startup
    (swa instead required an active CRTC and matched plane fb/crtc
    against the CRTC's current state);
  - the legacy drmModeSetCrtc / drmModePageFlip path is kept and is
    used automatically whenever anything here fails;
  - the mode blob is cached on the display and rebuilt only on
    (re)programming; flips reuse it;
  - the modeset commit is blocking and carries no flip event; flips are
    DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_kmsdrm.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DRM_CLIENT_CAP_ATOMIC is not always in older xf86drm headers */
#ifndef DRM_CLIENT_CAP_ATOMIC
#define DRM_CLIENT_CAP_ATOMIC 3
#endif
#ifndef DRM_CLIENT_CAP_UNIVERSAL_PLANES
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#endif

bool KMSDRM_AtomicEnabled(void)
{
    const char *v = getenv("SDLOP_KMSDRM_ATOMIC");
    return !(v && v[0] == '0'); /* default: try atomic first */
}

/* ------------------------------------------------------------------ */
/* property helpers (fd-based; SDLop's kmsdrm.c has static equivalents
 * but does not export them)                                            */
/* ------------------------------------------------------------------ */

static uint32_t atomic_prop_id(int fd, uint32_t object_id, uint32_t object_type, const char *name)
{
    if (!SDLOP_DRM_drmModeObjectGetProperties || !SDLOP_DRM_drmModeGetProperty
        || !SDLOP_DRM_drmModeFreeProperty || !SDLOP_DRM_drmModeFreeObjectProperties) {
        return 0;
    }
    drmModeObjectPropertiesPtr props =
        SDLOP_DRM_drmModeObjectGetProperties(fd, object_id, object_type);
    if (!props) {
        return 0;
    }
    uint32_t id = 0;
    for (uint32_t i = 0; !id && i < props->count_props; ++i) {
        drmModePropertyPtr p = SDLOP_DRM_drmModeGetProperty(fd, props->props[i]);
        if (!p) {
            continue;
        }
        if (strcmp(p->name, name) == 0) {
            id = p->prop_id;
        }
        SDLOP_DRM_drmModeFreeProperty(p);
    }
    SDLOP_DRM_drmModeFreeObjectProperties(props);
    return id;
}

static bool atomic_prop_get_value(int fd, uint32_t object_id, uint32_t object_type,
                                  const char *name, uint64_t *value)
{
    if (!SDLOP_DRM_drmModeObjectGetProperties || !SDLOP_DRM_drmModeGetProperty
        || !SDLOP_DRM_drmModeFreeProperty || !SDLOP_DRM_drmModeFreeObjectProperties) {
        return false;
    }
    drmModeObjectPropertiesPtr props =
        SDLOP_DRM_drmModeObjectGetProperties(fd, object_id, object_type);
    if (!props) {
        return false;
    }
    bool found = false;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr p = SDLOP_DRM_drmModeGetProperty(fd, props->props[i]);
        if (!p) {
            continue;
        }
        if (strcmp(p->name, name) == 0) {
            *value = props->prop_values[i];
            found = true;
        }
        SDLOP_DRM_drmModeFreeProperty(p);
        if (found) {
            break;
        }
    }
    SDLOP_DRM_drmModeFreeObjectProperties(props);
    return found;
}

/* Union of fourcc formats advertised by all planes on the device.
 * Deliberately CRTC-agnostic: a GBM surface has to be created before any
 * particular window/CRTC matters, so we accept any plane present. */
bool KMSDRM_PlaneFormatsUnion(int fd, uint32_t *xrgb8888, uint32_t *argb8888)
{
    if (!xrgb8888 || !argb8888) {
        return false;
    }
    if (!SDLOP_DRM_ATOMIC_Available()) {
        return false;
    }
    drmModePlaneResPtr plane_res = SDLOP_DRM_ATOMIC_drmModeGetPlaneResources(fd);
    if (!plane_res) {
        return false;
    }
    uint32_t xrgb = 0, argb = 0;
    for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
        drmModePlanePtr plane = SDLOP_DRM_ATOMIC_drmModeGetPlane(fd, plane_res->planes[i]);
        if (!plane) {
            continue;
        }
        for (uint32_t j = 0; j < plane->count_formats; ++j) {
            if (plane->formats[j] == DRM_FORMAT_XRGB8888) {
                xrgb = 1;
            } else if (plane->formats[j] == DRM_FORMAT_ARGB8888) {
                argb = 1;
            }
        }
        SDLOP_DRM_ATOMIC_drmModeFreePlane(plane);
    }
    SDLOP_DRM_ATOMIC_drmModeFreePlaneResources(plane_res);
    *xrgb8888 = xrgb;
    *argb8888 = argb;
    return true;
}

/* ------------------------------------------------------------------ */
/* capability probe                                                    */
/* ------------------------------------------------------------------ */

void KMSDRM_AtomicTryInit(KMSDRM_DeviceData *vid, KMSDRM_DisplayData *dd)
{
    memset(&dd->atomic, 0, sizeof(dd->atomic));

    if (!KMSDRM_AtomicEnabled()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: atomic modesetting disabled (SDLOP_KMSDRM_ATOMIC=0)");
        return;
    }
    if (!SDLOP_DRM_ATOMIC_Available()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: atomic modesetting unavailable (libdrm without the atomic API)");
        return;
    }
    int fd = vid->drm_fd;

    /* Enabling the atomic client cap enables universal planes as well.
     * Refused on very old kernels/drivers -> stay on legacy modesetting. */
    if (SDLOP_DRM_drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                    "KMSDRM: DRM_CLIENT_CAP_ATOMIC refused (%s); using legacy modesetting",
                    strerror(errno));
        return;
    }

    /* the KMS API anchors plane assignment to a CRTC *index* into res->crtcs */
    drmModeResPtr res = SDLOP_DRM_drmModeGetResources(fd);
    if (!res) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: atomic probe: drmModeGetResources failed");
        return;
    }
    int crtc_index = -1;
    for (int i = 0; i < res->count_crtcs; ++i) {
        if (res->crtcs[i] == dd->crtc->crtc_id) {
            crtc_index = i;
            break;
        }
    }
    if (crtc_index < 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: atomic probe: CRTC not in resource list");
        SDLOP_DRM_drmModeFreeResources(res);
        return;
    }
    SDLOP_DRM_drmModeFreeResources(res);

    drmModePlaneResPtr plane_res = SDLOP_DRM_ATOMIC_drmModeGetPlaneResources(fd);
    if (!plane_res || plane_res->count_planes <= 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: atomic probe: no planes reported");
        if (plane_res) {
            SDLOP_DRM_ATOMIC_drmModeFreePlaneResources(plane_res);
        }
        return;
    }

    for (uint32_t i = 0; i < plane_res->count_planes && !dd->atomic.primary_plane; ++i) {
        drmModePlanePtr plane = SDLOP_DRM_ATOMIC_drmModeGetPlane(fd, plane_res->planes[i]);
        if (!plane) {
            continue;
        }
        uint64_t type = 0;
        bool primary = atomic_prop_get_value(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type)
                       && type == DRM_PLANE_TYPE_PRIMARY;
        if (primary && (plane->possible_crtcs & (1u << crtc_index))) {
            dd->atomic.primary_plane = plane->plane_id;
        }
        SDLOP_DRM_ATOMIC_drmModeFreePlane(plane);
    }
    SDLOP_DRM_ATOMIC_drmModeFreePlaneResources(plane_res);

    if (!dd->atomic.primary_plane) {
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                    "KMSDRM: no primary plane for CRTC %u; using legacy modesetting",
                    dd->crtc->crtc_id);
        return;
    }

    /* resolve & cache the property ids used on every commit */
    dd->atomic.conn_prop_crtc_id =
        atomic_prop_id(fd, dd->connector->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    dd->atomic.crtc_prop_active =
        atomic_prop_id(fd, dd->crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    dd->atomic.crtc_prop_mode_id =
        atomic_prop_id(fd, dd->crtc->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    dd->atomic.plane_prop_fb_id =
        atomic_prop_id(fd, dd->atomic.primary_plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    dd->atomic.plane_prop_crtc_id =
        atomic_prop_id(fd, dd->atomic.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    dd->atomic.plane_prop_src_x = atomic_prop_id(fd, dd->atomic.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    dd->atomic.plane_prop_src_y = atomic_prop_id(fd, dd->atomic.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    dd->atomic.plane_prop_src_w = atomic_prop_id(fd, dd->atomic.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    dd->atomic.plane_prop_src_h = atomic_prop_id(fd, dd->atomic.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    dd->atomic.plane_prop_crtc_x = atomic_prop_id(fd, dd->atomic.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    dd->atomic.plane_prop_crtc_y = atomic_prop_id(fd, dd->atomic.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    dd->atomic.plane_prop_crtc_w = atomic_prop_id(fd, dd->atomic.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    dd->atomic.plane_prop_crtc_h = atomic_prop_id(fd, dd->atomic.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");

    if (!dd->atomic.conn_prop_crtc_id || !dd->atomic.crtc_prop_active || !dd->atomic.crtc_prop_mode_id
        || !dd->atomic.plane_prop_fb_id || !dd->atomic.plane_prop_crtc_id
        || !dd->atomic.plane_prop_src_w || !dd->atomic.plane_prop_src_h
        || !dd->atomic.plane_prop_crtc_w || !dd->atomic.plane_prop_crtc_h) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                    "KMSDRM: atomic properties not fully exposed; using legacy modesetting");
        memset(&dd->atomic, 0, sizeof(dd->atomic));
        return;
    }

    dd->atomic.available = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "KMSDRM: atomic modesetting enabled (primary plane %u)", dd->atomic.primary_plane);
}

/* ------------------------------------------------------------------ */
/* commits                                                             */
/* ------------------------------------------------------------------ */

struct atomic_req {
    drmModeAtomicReq *req;
    bool failed;
};

static void atomic_add(struct atomic_req *a, uint32_t object, uint32_t prop, uint64_t value)
{
    if (a->failed) {
        return;
    }
    if (!prop) { /* property legitimately absent (optional) - skip */
        return;
    }
    if (SDLOP_DRM_ATOMIC_drmModeAtomicAddProperty(a->req, object, prop, value) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                    "KMSDRM: atomic add property %u failed: %s", prop, strerror(errno));
        a->failed = true;
    }
}

static void atomic_add_plane(struct atomic_req *a, KMSDRM_DisplayData *dd,
                             uint32_t fb_id)
{
    uint64_t w = dd->mode.hdisplay, h = dd->mode.vdisplay;
    uint32_t plane = dd->atomic.primary_plane;

    atomic_add(a, plane, dd->atomic.plane_prop_crtc_id, dd->crtc->crtc_id);
    atomic_add(a, plane, dd->atomic.plane_prop_fb_id, fb_id);
    atomic_add(a, plane, dd->atomic.plane_prop_src_x, 0);
    atomic_add(a, plane, dd->atomic.plane_prop_src_y, 0);
    /* SRC_* are 16.16 fixed point */
    atomic_add(a, plane, dd->atomic.plane_prop_src_w, w << 16);
    atomic_add(a, plane, dd->atomic.plane_prop_src_h, h << 16);
    atomic_add(a, plane, dd->atomic.plane_prop_crtc_x, 0);
    atomic_add(a, plane, dd->atomic.plane_prop_crtc_y, 0);
    atomic_add(a, plane, dd->atomic.plane_prop_crtc_w, w);
    atomic_add(a, plane, dd->atomic.plane_prop_crtc_h, h);
}

/* Blocking (re)programming commit: carries the mode blob and ACTIVE,
 * no flip event. Returns true on success; on failure errno is preserved. */
bool KMSDRM_AtomicModeset(KMSDRM_DeviceData *vid, KMSDRM_DisplayData *dd, uint32_t fb_id)
{
    int fd = vid->drm_fd;

    /* rebuild the mode blob (mode may have changed since the last one) */
    if (dd->atomic.mode_blob) {
        SDLOP_DRM_ATOMIC_drmModeDestroyPropertyBlob(fd, dd->atomic.mode_blob);
        dd->atomic.mode_blob = 0;
    }
    uint32_t blob = 0;
    if (SDLOP_DRM_ATOMIC_drmModeCreatePropertyBlob(fd, &dd->mode, sizeof(dd->mode), &blob) != 0
        || !blob) {
        SDL_SetError("KMSDRM: atomic: failed to create mode blob: %s", strerror(errno));
        return false;
    }
    dd->atomic.mode_blob = blob;

    drmModeAtomicReq *req = SDLOP_DRM_ATOMIC_drmModeAtomicAlloc();
    if (!req) {
        SDL_SetError("KMSDRM: atomic: drmModeAtomicAlloc failed");
        return false;
    }
    struct atomic_req a = { req, false };

    atomic_add(&a, dd->connector->connector_id, dd->atomic.conn_prop_crtc_id, dd->crtc->crtc_id);
    atomic_add(&a, dd->crtc->crtc_id, dd->atomic.crtc_prop_mode_id, blob);
    atomic_add(&a, dd->crtc->crtc_id, dd->atomic.crtc_prop_active, 1);
    atomic_add_plane(&a, dd, fb_id);

    if (a.failed) {
        SDLOP_DRM_ATOMIC_drmModeAtomicFree(req);
        SDL_SetError("KMSDRM: atomic: could not assemble modeset request");
        return false;
    }

    int ret = SDLOP_DRM_ATOMIC_drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    int err = errno;
    SDLOP_DRM_ATOMIC_drmModeAtomicFree(req);
    if (ret != 0) {
        errno = err;
        return false;
    }
    return true;
}

/* Non-blocking page flip: reuses the cached mode blob; completion is
 * signalled with DRM_EVENT_FLIP_COMPLETE into flip_cookie (a bool*,
 * cleared by KMSDRM_FlipHandler). Returns 0 when queued, -1 on error
 * with errno set (EBUSY = a flip is still pending). */
int KMSDRM_AtomicFlip(KMSDRM_DeviceData *vid, KMSDRM_DisplayData *dd, uint32_t fb_id,
                      void *flip_cookie)
{
    int fd = vid->drm_fd;

    if (!dd->atomic.mode_blob) {
        /* no mode programmed through the atomic path yet; do a modeset instead */
        errno = EINVAL;
        return -1;
    }

    drmModeAtomicReq *req = SDLOP_DRM_ATOMIC_drmModeAtomicAlloc();
    if (!req) {
        errno = ENOMEM;
        return -1;
    }
    struct atomic_req a = { req, false };

    atomic_add(&a, dd->connector->connector_id, dd->atomic.conn_prop_crtc_id, dd->crtc->crtc_id);
    atomic_add(&a, dd->crtc->crtc_id, dd->atomic.crtc_prop_mode_id, dd->atomic.mode_blob);
    atomic_add(&a, dd->crtc->crtc_id, dd->atomic.crtc_prop_active, 1);
    atomic_add_plane(&a, dd, fb_id);

    if (a.failed) {
        SDLOP_DRM_ATOMIC_drmModeAtomicFree(req);
        errno = EINVAL;
        return -1;
    }

    int ret = SDLOP_DRM_ATOMIC_drmModeAtomicCommit(fd, req,
                                                   DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
                                                   flip_cookie);
    int err = errno;
    SDLOP_DRM_ATOMIC_drmModeAtomicFree(req);
    errno = err;
    return ret;
}
