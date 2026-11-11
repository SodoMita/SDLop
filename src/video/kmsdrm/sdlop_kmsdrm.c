/*
  SDLop - KMS/DRM video driver (Linux).

  A lean port of SDL3's kmsdrm backend, adapted to SDLop's architecture and
  extended with a zero-copy dumb-buffer software scanout (the wayland driver
  maps its shm buffer directly into the window surface; this driver does the
  same with dumb buffers, so pure software apps never need GBM or EGL).

  Two scanout paths per window:

  - software (default): double-buffered dumb buffers wrapped by an
    SDL_PIXELFORMAT_XRGB8888 surface (SDL_CreateSurfaceFrom, preallocated).
    SDL_UpdateWindowSurface = vsync page flip (drmModePageFlip) with a
    fallback to blocking drmModeSetCrtc when flips are unsupported or
    SDLOP_KMSDRM_NOFLIP=1 is set. No GL, no libgbm required.
  - GL (after SDL_GL_CreateContext): SDL3-style GBM surface + EGL + page
    flip. Requires libgbm + libEGL (dlopen-ed); created lazily so pure
    software apps work on GBM-less (e.g. no-virgl) setups.

  Environment hints:
    SDLOP_KMSDRM_DEVICE=<n|/dev/dri/cardN>  pick the DRM device (default: probe)
    SDLOP_KMSDRM_CONNECTOR=<substr>         pick the output by name, e.g. VGA, HDMI
    SDLOP_KMSDRM_NOFLIP=1                   never page flip; always drmModeSetCrtc
    SDLOP_KMSDRM_FLIP_TIMEOUT_MS=<n>        page flip wait timeout (default 1500)

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_kmsdrm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <strings.h>
#include <stdarg.h>
#include <limits.h>

static KMSDRM_DeviceData kmsdrm;

static void kmsdrm_destroy_dumb(KMSDRM_DeviceData *vid, KMSDRM_WindowData *windata);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int env_int(const char *name, int fallback)
{
    const char *v = getenv(name);
    if (!v || !*v) {
        return fallback;
    }
    errno = 0;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (errno || end == v || (end && *end)) {
        return fallback;
    }
    return (int)n;
}

static void kmsdrm_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SDL_LogMessage(SDL_LOG_CATEGORY_VIDEO, SDL_LOG_PRIORITY_INFO, "%s", buf);
}

static int flip_timeout_ms(void)
{
    int t = env_int("SDLOP_KMSDRM_FLIP_TIMEOUT_MS", 1500);
    return t < 50 ? 50 : t;
}

static bool is_permission_error(int err)
{
    return err == EPERM || err == EACCES;
}

/* ------------------------------------------------------------------ */
/* Device probing                                                      */
/* ------------------------------------------------------------------ */

/* Returns a heap-allocated path of a usable DRM device, or NULL. */
static char *kmsdrm_find_device(void)
{
    const char *hint = getenv("SDLOP_KMSDRM_DEVICE");
    if (hint && *hint) {
        char *path = malloc(64);
        if (!path) {
            return NULL;
        }
        if (hint[0] == '/') {
            snprintf(path, 64, "%s", hint);
        } else {
            snprintf(path, 64, "/dev/dri/card%s", hint);
        }
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            SDL_SetError("SDLOP_KMSDRM_DEVICE=%s: %s", path, strerror(errno));
            free(path);
            return NULL;
        }
        close(fd);
        return path;
    }

    DIR *folder = opendir("/dev/dri");
    if (!folder) {
        SDL_SetError("Cannot open /dev/dri (no DRM devices?)");
        return NULL;
    }

    /* gather cardN indices; readdir order is arbitrary, so probe in
     * ascending numeric order (primary GPU first, matching PCI order) */
    int indices[64];
    int count = 0;
    struct dirent *res;
    while ((res = readdir(folder)) != NULL && count < 64) {
        if (strncmp(res->d_name, "card", 4) != 0) {
            continue;
        }
        const char *digits = res->d_name + 4;
        if (*digits == '\0') {
            continue;
        }
        bool all_digits = true;
        for (const char *p = digits; *p; p++) {
            if (*p < '0' || *p > '9') {
                all_digits = false;
                break;
            }
        }
        if (!all_digits) {
            continue;
        }
        int idx = atoi(digits);
        int i = count;
        while (i > 0 && indices[i - 1] > idx) {
            indices[i] = indices[i - 1];
            i--;
        }
        indices[i] = idx;
        count++;
    }
    closedir(folder);

    char *best = NULL;
    for (int i = 0; i < count && !best; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", indices[i]);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue; /* skip unusable cards (render nodes etc.) */
        }
        drmModeRes *res_mode = SDLOP_DRM_drmModeGetResources(fd);
        if (res_mode) {
            bool usable = res_mode->count_connectors > 0 && res_mode->count_crtcs > 0;
            bool has_connected = false;
            for (int j = 0; j < res_mode->count_connectors && usable; j++) {
                drmModeConnector *conn = SDLOP_DRM_drmModeGetConnector(fd, res_mode->connectors[j]);
                if (!conn) {
                    continue;
                }
                if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
                    has_connected = true;
                }
                SDLOP_DRM_drmModeFreeConnector(conn);
            }
            if (usable && has_connected) {
                best = strdup(path);
            }
            SDLOP_DRM_drmModeFreeResources(res_mode);
        }
        close(fd);
    }

    if (!best) {
        SDL_SetError("No usable DRM device found in /dev/dri");
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Display init / teardown                                             */
/* ------------------------------------------------------------------ */

static void kmsdrm_connector_name(KMSDRM_DisplayData *dd, drmModeConnector *connector)
{
    const char *type = NULL;
    if (SDLOP_DRM_drmModeGetConnectorTypeName) {
        type = SDLOP_DRM_drmModeGetConnectorTypeName(connector->connector_type);
    }
    if (type) {
        snprintf(dd->name, sizeof(dd->name), "%s-%u", type, connector->connector_type_id);
    } else {
        snprintf(dd->name, sizeof(dd->name), "connector-%u", connector->connector_id);
    }
}

/* Find the connector the user wants (SDLOP_KMSDRM_CONNECTOR substring,
 * case-insensitive), or the first connected one with modes. */
static drmModeConnector *kmsdrm_pick_connector(int fd, drmModeRes *resources)
{
    const char *filter = getenv("SDLOP_KMSDRM_CONNECTOR");
    drmModeConnector *first = NULL;

    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *conn = SDLOP_DRM_drmModeGetConnector(fd, resources->connectors[i]);
        if (!conn) {
            continue;
        }
        if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0) {
            SDLOP_DRM_drmModeFreeConnector(conn);
            continue;
        }
        if (!filter || !*filter) {
            return conn; /* first usable */
        }
        char name[64];
        const char *type = SDLOP_DRM_drmModeGetConnectorTypeName
            ? SDLOP_DRM_drmModeGetConnectorTypeName(conn->connector_type) : "connector";
        snprintf(name, sizeof(name), "%s-%u", type, conn->connector_type_id);
        if (strcasestr(name, filter)) {
            if (first) {
                SDLOP_DRM_drmModeFreeConnector(first);
            }
            return conn;
        }
        if (!first) {
            first = conn;
        } else {
            SDLOP_DRM_drmModeFreeConnector(conn);
        }
    }
    return first; /* NULL if filter matched nothing: caller falls back */
}

static bool kmsdrm_init_displays(void)
{
    KMSDRM_DeviceData *vid = &kmsdrm;
    drmModeRes *resources = NULL;
    drmModeEncoder *encoder = NULL;
    drmModeConnector *connector = NULL;
    drmModeCrtc *crtc = NULL;
    bool result = false;

    resources = SDLOP_DRM_drmModeGetResources(vid->drm_fd);
    if (!resources) {
        SDL_SetError("drmModeGetResources failed");
        goto cleanup;
    }

    connector = kmsdrm_pick_connector(vid->drm_fd, resources);
    if (!connector) {
        SDL_SetError("No connected connector%s%s",
                     getenv("SDLOP_KMSDRM_CONNECTOR") ? " matching SDLOP_KMSDRM_CONNECTOR=" : "",
                     getenv("SDLOP_KMSDRM_CONNECTOR") ? getenv("SDLOP_KMSDRM_CONNECTOR") : "");
        goto cleanup;
    }

    /* Find the connector's current encoder, else the first supported one */
    for (int i = 0; i < resources->count_encoders; i++) {
        encoder = SDLOP_DRM_drmModeGetEncoder(vid->drm_fd, resources->encoders[i]);
        if (encoder && encoder->encoder_id == connector->encoder_id) {
            break;
        }
        if (encoder) {
            SDLOP_DRM_drmModeFreeEncoder(encoder);
            encoder = NULL;
        }
    }
    if (!encoder) {
        for (int i = 0; i < resources->count_encoders; i++) {
            encoder = SDLOP_DRM_drmModeGetEncoder(vid->drm_fd, resources->encoders[i]);
            if (!encoder) {
                continue;
            }
            bool supported = false;
            for (int j = 0; j < connector->count_encoders; j++) {
                if (connector->encoders[j] == encoder->encoder_id) {
                    supported = true;
                    break;
                }
            }
            if (supported) {
                break;
            }
            SDLOP_DRM_drmModeFreeEncoder(encoder);
            encoder = NULL;
        }
    }
    if (!encoder) {
        SDL_SetError("No encoder found for connector");
        goto cleanup;
    }

    /* Find a CRTC: the encoder's current one, else the first possible one */
    if (encoder->crtc_id) {
        crtc = SDLOP_DRM_drmModeGetCrtc(vid->drm_fd, encoder->crtc_id);
    }
    if (!crtc) {
        for (int i = 0; i < resources->count_crtcs; i++) {
            if (encoder->possible_crtcs & (1 << i)) {
                encoder->crtc_id = resources->crtcs[i];
                crtc = SDLOP_DRM_drmModeGetCrtc(vid->drm_fd, encoder->crtc_id);
                break;
            }
        }
    }
    if (!crtc) {
        SDL_SetError("No CRTC found for connector");
        goto cleanup;
    }

    /* Save the current CRTC configuration so we can restore it on quit. */
    vid->display.original_mode = crtc->mode;
    vid->display.original_fb_id = crtc->buffer_id;
    if (crtc->mode_valid == 0 || crtc->mode.hdisplay == 0) {
        memset(&vid->display.original_mode, 0, sizeof(vid->display.original_mode));
    }

    /* Initial display mode: the CRTC's current mode if sane, else the
     * preferred (or largest) connector mode. */
    if (crtc->mode_valid && crtc->mode.hdisplay > 0) {
        vid->display.mode = crtc->mode;
    } else {
        drmModeModeInfo *pref = NULL;
        for (int i = 0; i < connector->count_modes; i++) {
            drmModeModeInfo *m = &connector->modes[i];
            if (m->type & DRM_MODE_TYPE_PREFERRED) {
                pref = m;
                break;
            }
            if (!pref || (m->hdisplay * m->vdisplay) > (pref->hdisplay * pref->vdisplay)) {
                pref = m;
            }
        }
        vid->display.mode = *pref;
    }

    vid->display.connector = connector;
    vid->display.crtc = crtc;
    connector = NULL; /* owned by display now */
    crtc = NULL;
    kmsdrm_connector_name(&vid->display, vid->display.connector);

    kmsdrm_log("KMSDRM: using %s on %s (%dx%d, %d modes)",
               vid->display.name, vid->devpath,
               vid->display.mode.hdisplay, vid->display.mode.vdisplay,
               vid->display.connector->count_modes);

    result = true;

cleanup:
    if (encoder) {
        SDLOP_DRM_drmModeFreeEncoder(encoder);
    }
    if (crtc) {
        SDLOP_DRM_drmModeFreeCrtc(crtc);
    }
    if (connector) {
        SDLOP_DRM_drmModeFreeConnector(connector);
    }
    if (resources) {
        SDLOP_DRM_drmModeFreeResources(resources);
    }
    return result;
}

static void kmsdrm_deinit_displays(void)
{
    KMSDRM_DeviceData *vid = &kmsdrm;

    /* Restore the CRTC to its original mode, if we know it and had a
     * framebuffer at init time (fbcon's buffer usually survives). */
    if (vid->drm_fd >= 0 && vid->display.crtc) {
        drmModeModeInfo restore = vid->display.original_mode;
        uint32_t fb = vid->display.original_fb_id;
        if (restore.hdisplay == 0) {
            /* original CRTC had no mode: use the preferred connector mode */
            if (vid->display.connector && vid->display.connector->count_modes > 0) {
                restore = vid->display.connector->modes[0];
                for (int i = 0; i < vid->display.connector->count_modes; i++) {
                    if (vid->display.connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
                        restore = vid->display.connector->modes[i];
                        break;
                    }
                }
            }
        }
        if (fb != 0 && restore.hdisplay > 0 && vid->display.connector) {
            if (SDLOP_DRM_drmModeSetCrtc(vid->drm_fd, vid->display.crtc->crtc_id, fb, 0, 0,
                                         &vid->display.connector->connector_id, 1,
                                         &restore) != 0) {
                /* not fatal: the console may have torn the buffer down */
                SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: could not restore original CRTC mode");
            }
        }
    }

    if (vid->display.cursor_bo) {
        if (vid->display.cursor_visible) {
            SDLOP_DRM_drmModeSetCursor(vid->drm_fd, vid->display.crtc->crtc_id, 0, 0, 0);
            vid->display.cursor_visible = false;
        }
        SDLOP_GBM_gbm_bo_destroy(vid->display.cursor_bo);
        vid->display.cursor_bo = NULL;
    }
    if (vid->display.connector) {
        SDLOP_DRM_drmModeFreeConnector(vid->display.connector);
        vid->display.connector = NULL;
    }
    if (vid->display.crtc) {
        SDLOP_DRM_drmModeFreeCrtc(vid->display.crtc);
        vid->display.crtc = NULL;
    }
    if (vid->drm_fd >= 0) {
        close(vid->drm_fd);
        vid->drm_fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Mode selection                                                      */
/* ------------------------------------------------------------------ */

static int mode_refresh_mhz(const drmModeModeInfo *m)
{
    uint64_t num = (uint64_t)m->clock * 1000;
    uint64_t den = (uint64_t)m->htotal * (uint64_t)m->vtotal;
    if (m->flags & DRM_MODE_FLAG_INTERLACE) {
        num *= 2;
    }
    if (m->flags & DRM_MODE_FLAG_DBLSCAN) {
        den *= 2;
    }
    if (m->vscan > 1) {
        den *= m->vscan;
    }
    return den ? (int)(num / den) : 0;
}

/* Closest mode to w x h; NULL when the connector has no modes. */
static drmModeModeInfo *kmsdrm_closest_mode(KMSDRM_DisplayData *dd, int w, int h)
{
    drmModeConnector *conn = dd->connector;
    drmModeModeInfo *best = NULL;
    long best_cost = LONG_MAX;

    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *m = &conn->modes[i];
        long area_cost = labs((long)m->hdisplay * m->vdisplay - (long)w * h);
        long side_cost = labs(m->hdisplay - w) + labs(m->vdisplay - h);
        long cost = side_cost * 1000000 + area_cost;
        if (cost < best_cost ||
            (cost == best_cost && best && mode_refresh_mhz(m) > mode_refresh_mhz(best))) {
            best = m;
            best_cost = cost;
        }
    }
    return best;
}

static drmModeModeInfo *kmsdrm_preferred_mode(KMSDRM_DisplayData *dd)
{
    drmModeConnector *conn = dd->connector;
    drmModeModeInfo *best = NULL;

    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *m = &conn->modes[i];
        if (m->type & DRM_MODE_TYPE_PREFERRED) {
            return m;
        }
        if (!best || (m->hdisplay * m->vdisplay) > (best->hdisplay * best->vdisplay)) {
            best = m;
        }
    }
    return best;
}

/* Pick the mode for the window; windowed = closest to requested size,
 * fullscreen = preferred. */
static drmModeModeInfo kmsdrm_select_mode(KMSDRM_DisplayData *dd, int w, int h, bool fullscreen)
{
    drmModeModeInfo *m = fullscreen
        ? kmsdrm_preferred_mode(dd)
        : (w > 0 && h > 0 ? kmsdrm_closest_mode(dd, w, h) : NULL);
    if (!m) {
        m = kmsdrm_preferred_mode(dd);
    }
    if (!m) {
        m = &dd->mode; /* paranoia: connector reported modes at probe time */
    }
    return *m;
}

/* ------------------------------------------------------------------ */
/* Framebuffer helpers                                                 */
/* ------------------------------------------------------------------ */

static void KMSDRM_FBDestroyCallback(struct gbm_bo *bo, void *data)
{
    (void)bo;
    KMSDRM_FBInfo *fb_info = (KMSDRM_FBInfo *)data;
    if (fb_info) {
        if (fb_info->drm_fd >= 0 && fb_info->fb_id != 0) {
            SDLOP_DRM_drmModeRmFB(fb_info->drm_fd, fb_info->fb_id);
        }
        free(fb_info);
    }
}

KMSDRM_DeviceData *KMSDRM_GetDevice(void)
{
    return &kmsdrm;
}

void KMSDRM_DestroySwScanout(SDL_Window *window)
{
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    if (windata) {
        kmsdrm_destroy_dumb(&kmsdrm, windata);
    }
}

static uint32_t kmsdrm_add_fb(int fd, uint32_t w, uint32_t h, uint32_t stride,
                              uint32_t handle, uint32_t fourcc)
{
    uint32_t fb = 0;
    if (SDLOP_DRM_drmModeAddFB2) {
        uint32_t handles[4] = { handle, 0, 0, 0 };
        uint32_t pitches[4] = { stride, 0, 0, 0 };
        uint32_t offsets[4] = { 0, 0, 0, 0 };
        if (SDLOP_DRM_drmModeAddFB2(fd, w, h, fourcc, handles, pitches, offsets, &fb, 0) == 0) {
            return fb;
        }
        fb = 0;
    }
    if (SDLOP_DRM_drmModeAddFB(fd, w, h, 24, 32, stride, handle, &fb) == 0) {
        return fb;
    }
    return 0;
}

KMSDRM_FBInfo *KMSDRM_FBFromBO(int drm_fd, struct gbm_bo *bo)
{
    if (!SDLOP_GBM_Available()) {
        return NULL;
    }

    /* reuse an FB already attached to this BO */
    KMSDRM_FBInfo *fb_info = (KMSDRM_FBInfo *)SDLOP_GBM_gbm_bo_get_user_data(bo);
    if (fb_info) {
        return fb_info;
    }

    fb_info = (KMSDRM_FBInfo *)calloc(1, sizeof(KMSDRM_FBInfo));
    if (!fb_info) {
        return NULL;
    }
    fb_info->drm_fd = drm_fd;

    unsigned w = SDLOP_GBM_gbm_bo_get_width(bo);
    unsigned h = SDLOP_GBM_gbm_bo_get_height(bo);
    uint32_t format = SDLOP_GBM_gbm_bo_get_format(bo);

    int rc = -1;
    uint32_t strides[4] = { 0 }, handles[4] = { 0 }, offsets[4] = { 0 };
    uint64_t modifiers[4] = { 0 };
    int num_planes = 0;

    modifiers[0] = SDLOP_GBM_gbm_bo_get_modifier ? SDLOP_GBM_gbm_bo_get_modifier(bo) : 0;
    num_planes = SDLOP_GBM_gbm_bo_get_plane_count ? SDLOP_GBM_gbm_bo_get_plane_count(bo) : 1;
    if (num_planes < 1) {
        num_planes = 1;
    }
    if (num_planes > 4) {
        num_planes = 4;
    }
    for (int i = 0; i < num_planes; i++) {
        strides[i] = SDLOP_GBM_gbm_bo_get_stride_for_plane ? SDLOP_GBM_gbm_bo_get_stride_for_plane(bo, i) : SDLOP_GBM_gbm_bo_get_stride(bo);
        handles[i] = SDLOP_GBM_gbm_bo_get_handle_for_plane ? SDLOP_GBM_gbm_bo_get_handle_for_plane(bo, i).u32 : SDLOP_GBM_gbm_bo_get_handle(bo).u32;
        offsets[i] = SDLOP_GBM_gbm_bo_get_offset ? SDLOP_GBM_gbm_bo_get_offset(bo, i) : 0;
        modifiers[i] = modifiers[0];
    }

    uint32_t flags = 0;
    if (modifiers[0] && modifiers[0] != DRM_FORMAT_MOD_INVALID) {
        flags = DRM_MODE_FB_MODIFIERS;
    }
    if (SDLOP_DRM_drmModeAddFB2WithModifiers) {
        rc = SDLOP_DRM_drmModeAddFB2WithModifiers(drm_fd, w, h, format, handles, strides, offsets,
                                                  modifiers, &fb_info->fb_id, flags);
    }
    if (rc < 0) {
        strides[0] = SDLOP_GBM_gbm_bo_get_stride(bo);
        handles[0] = SDLOP_GBM_gbm_bo_get_handle(bo).u32;
        uint32_t offs[4] = { 0, 0, 0, 0 };
        uint32_t fmt_handles[4] = { handles[0], 0, 0, 0 };
        uint32_t fmt_pitches[4] = { strides[0], 0, 0, 0 };
        rc = -1;
        if (SDLOP_DRM_drmModeAddFB2) {
            rc = SDLOP_DRM_drmModeAddFB2(drm_fd, w, h, format, fmt_handles, fmt_pitches, offs,
                                         &fb_info->fb_id, 0);
        }
        if (rc < 0) {
            rc = SDLOP_DRM_drmModeAddFB(drm_fd, w, h, 24, 32, strides[0], handles[0], &fb_info->fb_id);
        }
    }
    if (rc < 0) {
        free(fb_info);
        return NULL;
    }

    SDLOP_GBM_gbm_bo_set_user_data(bo, fb_info, KMSDRM_FBDestroyCallback);
    return fb_info;
}

void KMSDRM_FlipHandler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
    (void)fd; (void)frame; (void)sec; (void)usec;
    if (data) {
        *((bool *)data) = false;
    }
}

bool KMSDRM_WaitPageflip(KMSDRM_DeviceData *viddata, KMSDRM_WindowData *windata, int timeout_ms)
{
    drmEventContext ev = { 0 };
    struct pollfd pfd = { 0 };
    int elapsed = 0;
    const int step_ms = 10;

    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = KMSDRM_FlipHandler;

    pfd.fd = viddata->drm_fd;
    pfd.events = POLLIN;

    while (windata->waiting_for_flip) {
        pfd.revents = 0;
        int ret = poll(&pfd, 1, timeout_ms < 0 ? -1 : step_ms);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: poll error waiting for page flip");
            return false;
        }
        if (timeout_ms < 0) {
            /* infinite wait: only EINTR above applies */
        } else if (ret == 0) {
            elapsed += step_ms;
            if (elapsed >= timeout_ms) {
                SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                             "KMSDRM: page flip did not complete within %d ms", timeout_ms);
                windata->waiting_for_flip = false; /* assume lost; recover on next present */
                return false;
            }
        }
        if (pfd.revents & (POLLHUP | POLLERR)) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: DRM fd hung up while waiting for page flip");
            windata->waiting_for_flip = false;
            return false;
        }
        if (pfd.revents & POLLIN) {
            SDLOP_DRM_drmHandleEvent(viddata->drm_fd, &ev);
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Software (dumb buffer) scanout                                      */
/* ------------------------------------------------------------------ */

static void kmsdrm_destroy_dumb(KMSDRM_DeviceData *vid, KMSDRM_WindowData *windata)
{
    for (int i = 0; i < 2; i++) {
        if (windata->sw_map[i]) {
            munmap(windata->sw_map[i], (size_t)windata->sw_size);
            windata->sw_map[i] = NULL;
        }
        if (windata->sw_fb_id[i]) {
            SDLOP_DRM_drmModeRmFB(vid->drm_fd, windata->sw_fb_id[i]);
            windata->sw_fb_id[i] = 0;
        }
        if (windata->sw_handle[i]) {
            SDLOP_DRM_drmModeDestroyDumbBuffer(vid->drm_fd, windata->sw_handle[i]);
            windata->sw_handle[i] = 0;
        }
    }
    windata->sw_active = false;
    windata->sw_pitch = 0;
    windata->sw_size = 0;
    windata->sw_draw = 0;
    windata->sw_front = -1;
}

static bool kmsdrm_create_dumb(KMSDRM_DeviceData *vid, KMSDRM_WindowData *windata, uint32_t w, uint32_t h)
{
    kmsdrm_destroy_dumb(vid, windata);

    for (int i = 0; i < 2; i++) {
        windata->sw_handle[i] = 0;
        windata->sw_fb_id[i] = 0;
        windata->sw_map[i] = NULL;
    }

    for (int i = 0; i < 2; i++) {
        uint32_t handle = 0, pitch = 0;
        uint64_t size = 0;
        int ret = SDLOP_DRM_drmModeCreateDumbBuffer(vid->drm_fd, w, h, 32, 0, &handle, &pitch, &size);
        if (ret != 0) {
            SDL_SetError("drmModeCreateDumbBuffer(%ux%u) failed: %s", w, h, strerror(errno));
            goto failed;
        }
        uint64_t map_offset = 0;
        if (SDLOP_DRM_drmModeMapDumbBuffer(vid->drm_fd, handle, &map_offset) != 0) {
            SDL_SetError("drmModeMapDumbBuffer failed: %s", strerror(errno));
            SDLOP_DRM_drmModeDestroyDumbBuffer(vid->drm_fd, handle);
            goto failed;
        }
        void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, vid->drm_fd, (off_t)map_offset);
        if (p == MAP_FAILED) {
            SDL_SetError("mmap dumb buffer failed: %s", strerror(errno));
            SDLOP_DRM_drmModeDestroyDumbBuffer(vid->drm_fd, handle);
            goto failed;
        }
        uint32_t fb = kmsdrm_add_fb(vid->drm_fd, w, h, pitch, handle, DRM_FORMAT_XRGB8888);
        if (!fb) {
            SDL_SetError("drmModeAddFB failed for dumb buffer");
            munmap(p, (size_t)size);
            SDLOP_DRM_drmModeDestroyDumbBuffer(vid->drm_fd, handle);
            goto failed;
        }
        memset(p, 0, (size_t)size);
        windata->sw_handle[i] = handle;
        windata->sw_map[i] = p;
        windata->sw_fb_id[i] = fb;
        windata->sw_pitch = pitch;
        windata->sw_size = size;
    }

    windata->sw_draw = 0;
    windata->sw_front = -1;
    windata->sw_active = true;
    return true;

failed:
    kmsdrm_destroy_dumb(vid, windata);
    return false;
}

/* Fill both dumb buffers with a color (ARGB XRGB8888). */
static void kmsdrm_fill_sw(KMSDRM_WindowData *windata, uint32_t w, uint32_t h, uint32_t argb)
{
    for (int i = 0; i < 2; i++) {
        uint32_t *p = (uint32_t *)windata->sw_map[i];
        if (!p) {
            continue;
        }
        size_t pitch_pix = windata->sw_pitch / 4;
        for (uint32_t y = 0; y < h; y++) {
            uint32_t *row = p + (size_t)y * pitch_pix;
            for (uint32_t x = 0; x < w; x++) {
                row[x] = argb;
            }
        }
    }
}

static void kmsdrm_retarget_surface(SDL_Window *window, KMSDRM_WindowData *windata)
{
    if (!window->surface || !windata->sw_active) {
        return;
    }
    KMSDRM_DisplayData *dd = windata->display;
    window->surface->pixels = windata->sw_map[windata->sw_draw];
    window->surface->pitch = (int)windata->sw_pitch;
    window->surface->w = (int)dd->mode.hdisplay;
    window->surface->h = (int)dd->mode.vdisplay;
}

/* Program the CRTC with the buffer the app just finished (first present,
 * after a mode change, or when page flips are unavailable/disabled). */
static bool kmsdrm_crtc_set(KMSDRM_DeviceData *vid, KMSDRM_WindowData *windata, uint32_t fb_id)
{
    KMSDRM_DisplayData *dd = windata->display;
    int ret = SDLOP_DRM_drmModeSetCrtc(vid->drm_fd, dd->crtc->crtc_id, fb_id, 0, 0,
                                       &dd->connector->connector_id, 1, &dd->mode);
    if (ret != 0) {
        int err = errno;
        if (is_permission_error(err)) {
            windata->lost_master = true;
        }
        return SDL_SetError("drmModeSetCrtc failed: %s", strerror(err));
    }
    windata->crtc_programmed = true;
    windata->lost_master = false;
    return true;
}

/* Present the buffer the app drew into. Returns true on success. */
static bool kmsdrm_present_sw(KMSDRM_DeviceData *vid, SDL_Window *window)
{
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    int idx = windata->sw_draw;
    uint32_t fb_id = windata->sw_fb_id[idx];

    /* If we lost master earlier, try to get it back before programming */
    if (windata->lost_master) {
        if (SDLOP_DRM_drmSetMaster(vid->drm_fd) == 0) {
            windata->lost_master = false;
            windata->crtc_programmed = false; /* CRTC state unknown: reprogram */
        }
    }

    /* Drain any flip that is somehow still pending */
    if (windata->waiting_for_flip) {
        KMSDRM_WaitPageflip(vid, windata, flip_timeout_ms());
    }

    bool ok;
    if (!windata->crtc_programmed || vid->no_flip) {
        ok = kmsdrm_crtc_set(vid, windata, fb_id);
    } else {
        int ret = SDLOP_DRM_drmModePageFlip(vid->drm_fd, windata->display->crtc->crtc_id,
                                            fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                                            &windata->waiting_for_flip);
        if (ret == 0) {
            windata->waiting_for_flip = true;
            ok = true;
        } else {
            int err = errno;
            if (err == EBUSY && KMSDRM_WaitPageflip(vid, windata, flip_timeout_ms())) {
                /* a flip was pending; retry once */
                ret = SDLOP_DRM_drmModePageFlip(vid->drm_fd, windata->display->crtc->crtc_id,
                                                fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                                                &windata->waiting_for_flip);
                if (ret == 0) {
                    windata->waiting_for_flip = true;
                    ok = true;
                } else {
                    ok = kmsdrm_crtc_set(vid, windata, fb_id);
                }
            } else if (is_permission_error(err)) {
                windata->lost_master = true;
                ok = SDL_SetError("drmModePageFlip: no DRM master (%s)", strerror(err));
            } else {
                /* flip unsupported by this driver/CRTC: fall back to modeset */
                if (err == EINVAL || err == ENOSYS) {
                    vid->no_flip = true;
                    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                                "KMSDRM: page flip failed (%s); falling back to drmModeSetCrtc",
                                strerror(err));
                }
                ok = kmsdrm_crtc_set(vid, windata, fb_id);
            }
        }
    }

    /* Wait for the flip to land, then make the previously displayed buffer
     * the draw buffer. Synchronous vsync, same trade as GL swap with
     * swap interval 1. */
    if (ok && windata->waiting_for_flip) {
        KMSDRM_WaitPageflip(vid, windata, flip_timeout_ms());
    }

    if (ok) {
        windata->sw_front = idx;
        windata->sw_draw = 1 - idx;
        kmsdrm_retarget_surface(window, windata);
    }
    return ok;
}

static void kmsdrm_destroy_surfaces(KMSDRM_DeviceData *vid, SDL_Window *window)
{
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    if (!windata) {
        return;
    }

    if (windata->waiting_for_flip) {
        KMSDRM_WaitPageflip(vid, windata, 500);
    }

    /* GL side */
    if (windata->gs) {
        KMSDRM_DestroyGLSurfaces(windata);
    }

    /* software side */
    kmsdrm_destroy_dumb(vid, windata);

    /* the CRTC keeps showing whatever FB it had; if nothing restores it the
     * next window/present will reprogram it. */
    windata->crtc_programmed = false;
    windata->sw_front = -1;
}
void KMSDRM_DestroySurfaces(KMSDRM_DeviceData *vid, SDL_Window *window)
{
    kmsdrm_destroy_surfaces(vid, window);
}

/* Recreate scanout buffers for the current mode (after a mode change). */
bool KMSDRM_RecreateScanout(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    KMSDRM_DeviceData *vid = &kmsdrm;
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    KMSDRM_DisplayData *dd = windata->display;

    if (windata->waiting_for_flip) {
        KMSDRM_WaitPageflip(vid, windata, 500);
    }

    uint32_t w = dd->mode.hdisplay, h = dd->mode.vdisplay;

    if (windata->gl_active) {
        /* mode-sized GBM/EGL scanout is stale; drop it and let the GL
         * swap path recreate it at the new size */
        if (windata->waiting_for_flip) {
            KMSDRM_WaitPageflip(vid, windata, 500);
        }
        KMSDRM_DestroyGLSurfaces(windata);
        windata->gl_active = true;
        return true;
    }

    if (!kmsdrm_create_dumb(vid, windata, w, h)) {
        return false;
    }
    windata->crtc_programmed = false;

    /* resize the cached software surface to the new mode */
    if (window->surface) {
        SDL_Surface *old = window->surface;
        SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_XRGB8888,
                                               windata->sw_map[windata->sw_draw],
                                               (int)windata->sw_pitch);
        if (s) {
            window->surface = s;
            SDL_DestroySurface(old);
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Hardware cursor (via GBM; unavailable -> cursor hidden, mouse fine) */
/* ------------------------------------------------------------------ */

/* 16x16 classic arrow; 0 = transparent, 1 = outline (black), 2 = fill (white) */
static const char cursor_arrow[16][17] = {
    "2...............",
    "22..............",
    "212.............",
    "2112............",
    "21112...........",
    "211112..........",
    "2111112.........",
    "21111112........",
    "211111112.......",
    "2111111112......",
    "21111111112.....",
    "21111122222.....",
    "2111212.........",
    "212.1212........",
    "22..1212........",
    ".....112........",
};

static bool kmsdrm_ensure_gbm(void)
{
    KMSDRM_DeviceData *vid = &kmsdrm;
    if (vid->gbm_init && vid->gbm_dev) {
        return true;
    }
    if (!SDLOP_GBM_LoadSymbols()) {
        return false; /* software path still works; no cursor */
    }
    if (SDLOP_DRM_drmSetMaster) {
        SDLOP_DRM_drmSetMaster(vid->drm_fd);
    }
    vid->gbm_dev = SDLOP_GBM_gbm_create_device(vid->drm_fd);
    if (!vid->gbm_dev) {
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: gbm_create_device failed");
        return false;
    }
    vid->gbm_init = true;
    return true;
}

static bool kmsdrm_ensure_cursor_bo(void)
{
    KMSDRM_DeviceData *vid = &kmsdrm;
    KMSDRM_DisplayData *dd = &vid->display;

    if (dd->cursor_bo) {
        return true;
    }
    if (!kmsdrm_ensure_gbm()) {
        return false;
    }

    uint64_t cw = 0, ch = 0;
    if (SDLOP_DRM_drmGetCap(vid->drm_fd, DRM_CAP_CURSOR_WIDTH, &cw) != 0 || cw == 0 || cw > 256) {
        cw = 64;
    }
    if (SDLOP_DRM_drmGetCap(vid->drm_fd, DRM_CAP_CURSOR_HEIGHT, &ch) != 0 || ch == 0 || ch > 256) {
        ch = 64;
    }

    if (!SDLOP_GBM_gbm_device_is_format_supported(vid->gbm_dev, GBM_FORMAT_ARGB8888,
                                                  GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: GBM cursor format not supported");
        return false;
    }

    struct gbm_bo *bo = SDLOP_GBM_gbm_bo_create(vid->gbm_dev, (uint32_t)cw, (uint32_t)ch,
                                                GBM_FORMAT_ARGB8888,
                                                GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE | GBM_BO_USE_LINEAR);
    if (!bo) {
        return false;
    }

    uint32_t *buf = calloc((size_t)cw * ch, sizeof(uint32_t));
    if (!buf) {
        SDLOP_GBM_gbm_bo_destroy(bo);
        return false;
    }
    for (uint64_t y = 0; y < ch; y++) {
        for (uint64_t x = 0; x < cw; x++) {
            char c = cursor_arrow[(y * 16) / ch][(x * 16) / cw];
            uint32_t px = 0;
            if (c == '1') {
                px = 0xFF000000u; /* opaque black */
            } else if (c == '2') {
                px = 0xFFFFFFFFu; /* opaque white */
            }
            buf[y * cw + x] = px;
        }
    }
    int ret = SDLOP_GBM_gbm_bo_write(bo, buf, (size_t)cw * ch * 4);
    free(buf);
    if (ret != 0) {
        SDLOP_GBM_gbm_bo_destroy(bo);
        return false;
    }

    dd->cursor_bo = bo;
    dd->cursor_w = cw;
    dd->cursor_h = ch;
    dd->cursor_hot_x = 0;
    dd->cursor_hot_y = 0;
    dd->cursor_visible = false;
    return true;
}

static void kmsdrm_show_cursor(void)
{
    KMSDRM_DeviceData *vid = &kmsdrm;
    KMSDRM_DisplayData *dd = &vid->display;

    if (dd->cursor_visible) {
        return;
    }
    if (!kmsdrm_ensure_cursor_bo()) {
        return;
    }
    uint32_t handle = SDLOP_GBM_gbm_bo_get_handle(dd->cursor_bo).u32;
    int ret = SDLOP_DRM_drmModeSetCursor2(vid->drm_fd, dd->crtc->crtc_id, handle,
                                          (uint32_t)dd->cursor_w, (uint32_t)dd->cursor_h,
                                          dd->cursor_hot_x, dd->cursor_hot_y);
    if (ret != 0) {
        /* SetCursor2 unsupported: try the legacy path */
        ret = SDLOP_DRM_drmModeSetCursor(vid->drm_fd, dd->crtc->crtc_id, handle,
                                         (uint32_t)dd->cursor_w, (uint32_t)dd->cursor_h);
    }
    dd->cursor_visible = (ret == 0);
    if (ret != 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: hardware cursor unavailable: %s", strerror(errno));
    }
}

static void kmsdrm_hide_cursor(void)
{
    KMSDRM_DeviceData *vid = &kmsdrm;
    KMSDRM_DisplayData *dd = &vid->display;

    if (!dd->cursor_visible) {
        return;
    }
    SDLOP_DRM_drmModeSetCursor(vid->drm_fd, dd->crtc->crtc_id, 0, 0, 0);
    dd->cursor_visible = false;
}

static void kmsdrm_move_cursor(float x, float y)
{
    KMSDRM_DeviceData *vid = &kmsdrm;
    KMSDRM_DisplayData *dd = &vid->display;

    if (!dd->cursor_visible) {
        return;
    }
    int ix = (int)x, iy = (int)y;
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    if (ix > (int)dd->mode.hdisplay - 1) ix = (int)dd->mode.hdisplay - 1;
    if (iy > (int)dd->mode.vdisplay - 1) iy = (int)dd->mode.vdisplay - 1;
    SDLOP_DRM_drmModeMoveCursor(vid->drm_fd, dd->crtc->crtc_id, ix, iy);
}

/* ------------------------------------------------------------------ */
/* Window lifecycle                                                    */
/* ------------------------------------------------------------------ */

static bool kmsdrm_CreateWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    KMSDRM_DeviceData *vid = &kmsdrm;
    KMSDRM_DisplayData *dd = &vid->display;

    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)calloc(1, sizeof(*windata));
    if (!windata) {
        return SDL_OutOfMemory();
    }
    windata->display = dd;
    windata->sw_draw = 0;
    windata->sw_front = -1;
    windata->egl_surface = EGL_NO_SURFACE;
    windata->clear_color = ((uint32_t)window->clear_r << 16) | ((uint32_t)window->clear_g << 8) | window->clear_b;

    /* pick the mode (windowed = closest requested, fullscreen = preferred) */
    bool fullscreen = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
    drmModeModeInfo chosen = kmsdrm_select_mode(dd, window->w, window->h, fullscreen);
    dd->mode = chosen;

    /* KMS windows always fill their mode */
    window->w = (int)dd->mode.hdisplay;
    window->h = (int)dd->mode.vdisplay;
    window->x = 0;
    window->y = 0;

    if (!kmsdrm_create_dumb(vid, windata, dd->mode.hdisplay, dd->mode.vdisplay)) {
        free(windata);
        return false; /* error set by kmsdrm_create_dumb */
    }

    /* paint both buffers with the clear color so the first frame is sane */
    kmsdrm_fill_sw(windata, dd->mode.hdisplay, dd->mode.vdisplay, windata->clear_color);

    /* hardware cursor (best effort; needs GBM) */
    if (kmsdrm_ensure_cursor_bo()) {
        kmsdrm_move_cursor((float)window->w / 2.0f, (float)window->h / 2.0f);
        kmsdrm_show_cursor();
    }

    window->driverdata = windata;

    /* KMS owns the screen: the window has input focus */
    sdlop.keyboard_focus = window;
    sdlop.mouse_focus = window;
    window->flags |= SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;

    return true;
}

static void kmsdrm_DestroyWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    KMSDRM_DeviceData *vid = &kmsdrm;
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    if (!windata) {
        return;
    }
    (void)vid;

    if (windata->waiting_for_flip) {
        KMSDRM_WaitPageflip(vid, windata, 500);
    }

    /* destroy GL scanout (if any) */
    KMSDRM_DestroyGLSurfaces(windata);

    /* destroy software scanout */
    kmsdrm_destroy_dumb(vid, windata);

    window->driverdata = NULL;
    free(windata);
}

/* present the clear-color frame if nothing has been shown yet */
static void kmsdrm_ShowWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    KMSDRM_DeviceData *vid = &kmsdrm;
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;

    sdlop.keyboard_focus = window;
    sdlop.mouse_focus = window;
    window->flags |= SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;

    if (!windata) {
        return;
    }
    if (windata->gl_active) {
        return; /* GL owns presentation */
    }
    if (!windata->crtc_programmed) {
        kmsdrm_present_sw(vid, window);
    }
    kmsdrm_show_cursor();
}

static void kmsdrm_HideWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    window->flags &= ~(SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
    if (sdlop.keyboard_focus == window) {
        sdlop.keyboard_focus = NULL;
    }
    if (sdlop.mouse_focus == window) {
        sdlop.mouse_focus = NULL;
    }
}

static bool kmsdrm_SetWindowTitle(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    return true; /* no window chrome on KMS */
}

static bool kmsdrm_SetWindowPosition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    return SDL_SetError("KMS does not allow absolute window positioning");
}

static bool kmsdrm_mode_change(SDLop_VideoDevice *device, SDL_Window *window)
{
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    KMSDRM_DisplayData *dd = windata->display;

    bool fullscreen = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
    drmModeModeInfo chosen = kmsdrm_select_mode(dd, window->w, window->h, fullscreen);

    bool changed = chosen.hdisplay != dd->mode.hdisplay ||
                   chosen.vdisplay != dd->mode.vdisplay ||
                   chosen.clock != dd->mode.clock ||
                   chosen.htotal != dd->mode.htotal ||
                   chosen.vtotal != dd->mode.vtotal;

    if (!changed) {
        return true; /* nothing to do; surface size unchanged */
    }

    dd->mode = chosen;
    window->w = (int)dd->mode.hdisplay;
    window->h = (int)dd->mode.vdisplay;

    if (!KMSDRM_RecreateScanout(device, window)) {
        return false;
    }

    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, window->w, window->h);
    return true;
}

static bool kmsdrm_SetWindowSize(SDLop_VideoDevice *device, SDL_Window *window)
{
    return kmsdrm_mode_change(device, window);
}

static bool kmsdrm_SetWindowFullscreen(SDLop_VideoDevice *device, SDL_Window *window)
{
    bool fullscreen = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
    if (kmsdrm_mode_change(device, window)) {
        SDLOP_SendWindowEvent(window,
                              fullscreen ? SDL_EVENT_WINDOW_ENTER_FULLSCREEN
                                         : SDL_EVENT_WINDOW_LEAVE_FULLSCREEN,
                              0, 0);
        return true;
    }
    return false;
}

static void kmsdrm_MinimizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    window->flags |= SDL_WINDOW_MINIMIZED;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MINIMIZED, 0, 0);
}

static void kmsdrm_MaximizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    window->flags |= SDL_WINDOW_MAXIMIZED;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MAXIMIZED, 0, 0);
}

static void kmsdrm_RestoreWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    window->flags &= ~(SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESTORED, 0, 0);
}

static void kmsdrm_RaiseWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    kmsdrm_ShowWindow(device, window);
}

static void kmsdrm_SetWindowClearColor(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    KMSDRM_DeviceData *vid = &kmsdrm;
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    if (!windata) {
        return;
    }
    windata->clear_color = ((uint32_t)window->clear_r << 16) |
                           ((uint32_t)window->clear_g << 8) |
                           window->clear_b;
    if (windata->sw_active) {
        kmsdrm_fill_sw(windata, windata->display->mode.hdisplay,
                       windata->display->mode.vdisplay, windata->clear_color);
        /* if nothing has been shown yet, lighting up immediately is nice */
        if (!windata->crtc_programmed) {
            kmsdrm_present_sw(vid, window);
        }
    }
}

static bool kmsdrm_SetWindowRelativeMouseMode(SDLop_VideoDevice *device, SDL_Window *window, bool enabled)
{
    (void)device;
    (void)window;
    if (enabled) {
        kmsdrm_hide_cursor();
    } else {
        kmsdrm_show_cursor();
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void kmsdrm_PumpEvents(SDLop_VideoDevice *device, int timeout_ms)
{
    (void)device;
    (void)timeout_ms;
    KMSDRM_DeviceData *vid = &kmsdrm;

    /* drain pending DRM events (page flip completions), non-blocking */
    if (vid->drm_fd >= 0) {
        struct pollfd pfd = { vid->drm_fd, POLLIN, 0 };
        int ret = poll(&pfd, 1, 0);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            drmEventContext ev = { 0 };
            ev.version = DRM_EVENT_CONTEXT_VERSION;
            ev.page_flip_handler = KMSDRM_FlipHandler;
            SDLOP_DRM_drmHandleEvent(vid->drm_fd, &ev);
        }
    }

    SDLOP_KeyboardProcessRepeats();

    /* move the hardware cursor to the logically tracked mouse position
     * (SDLop's evdev worker already tracks window-relative coordinates) */
    SDL_Window *focus = sdlop.mouse_focus;
    if (focus && focus->driverdata) {
        KMSDRM_WindowData *windata = (KMSDRM_WindowData *)focus->driverdata;
        float mx = sdlop.mouse_x, my = sdlop.mouse_y;
        if (!sdlop.relative_mode_window &&
            (!windata->cursor_pos_valid ||
             windata->cursor_x != mx || windata->cursor_y != my)) {
            kmsdrm_move_cursor(mx, my);
            windata->cursor_x = mx;
            windata->cursor_y = my;
            windata->cursor_pos_valid = true;
        }
    }
}

static int kmsdrm_GetEventFD(SDLop_VideoDevice *device)
{
    (void)device;
    /* the DRM fd only signals flip/vblank events which we drain ourselves;
     * input arrives via the evdev worker's wake fd */
    return -1;
}

/* ------------------------------------------------------------------ */
/* Software window surfaces                                            */
/* ------------------------------------------------------------------ */

static bool kmsdrm_CreateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface **surface)
{
    (void)device;
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    KMSDRM_DisplayData *dd = windata ? windata->display : NULL;
    if (!windata || !dd) {
        return SDL_SetError("Invalid window");
    }
    if (windata->gl_active) {
        return SDL_SetError("OpenGL is active: window surface is not presentable");
    }
    if (!windata->sw_active) {
        return SDL_SetError("Software framebuffer is not available");
    }
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)dd->mode.hdisplay, (int)dd->mode.vdisplay,
                                           SDL_PIXELFORMAT_XRGB8888,
                                           windata->sw_map[windata->sw_draw],
                                           (int)windata->sw_pitch);
    if (!s) {
        return false;
    }
    *surface = s;
    return true;
}

static bool kmsdrm_UpdateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    (void)device;
    (void)rects;
    (void)numrects;
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    if (!windata) {
        return SDL_SetError("Invalid window");
    }
    if (windata->gl_active) {
        return SDL_SetError("OpenGL is active: window surface is not presentable");
    }
    return kmsdrm_present_sw(&kmsdrm, window);
}

static void kmsdrm_DestroyWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    KMSDRM_WindowData *windata = (KMSDRM_WindowData *)window->driverdata;
    if (windata) {
        kmsdrm_destroy_dumb(&kmsdrm, windata);
    }
}

/* ------------------------------------------------------------------ */
/* Device init / quit                                                  */
/* ------------------------------------------------------------------ */

static bool kmsdrm_Init(SDLop_VideoDevice *device)
{
    (void)device;
    KMSDRM_DeviceData *vid = &kmsdrm;

    memset(vid, 0, sizeof(*vid));
    vid->drm_fd = -1;
    vid->devindex = -1;

    if (!SDLOP_KMSDRM_LoadSymbols()) {
        return false; /* error set by the loader */
    }

    const char *no_flip = getenv("SDLOP_KMSDRM_NOFLIP");
    vid->no_flip = no_flip && *no_flip && strcmp(no_flip, "0") != 0;

    char *path = kmsdrm_find_device();
    if (!path) {
        goto failed;
    }
    snprintf(vid->devpath, sizeof(vid->devpath), "%s", path);
    free(path);

    if (sscanf(vid->devpath, "/dev/dri/card%d", &vid->devindex) != 1) {
        vid->devindex = 0;
    }

    vid->drm_fd = open(vid->devpath, O_RDWR | O_CLOEXEC);
    if (vid->drm_fd < 0) {
        SDL_SetError("Could not open %s: %s", vid->devpath, strerror(errno));
        goto failed;
    }

    /* become DRM master if we aren't already (best effort) */
    if (SDLOP_DRM_drmSetMaster) {
        SDLOP_DRM_drmSetMaster(vid->drm_fd);
    }
    if (SDLOP_DRM_drmAuthMagic(vid->drm_fd, 0) == -EACCES) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                    "KMSDRM: no DRM master on %s (another master holds it?); modesetting will fail",
                    vid->devpath);
    }

    if (!kmsdrm_init_displays()) {
        goto failed_close;
    }

    uint64_t async_flip = 0;
    if (SDLOP_DRM_drmGetCap(vid->drm_fd, DRM_CAP_ASYNC_PAGE_FLIP, &async_flip) == 0) {
        vid->async_pageflip_support = async_flip != 0;
    }

    vid->video_init = true;
    return true;

failed_close:
    if (vid->drm_fd >= 0) {
        close(vid->drm_fd);
        vid->drm_fd = -1;
    }
failed:
    SDLOP_KMSDRM_UnloadSymbols();
    return false;
}

static void kmsdrm_Quit(SDLop_VideoDevice *device)
{
    (void)device;
    KMSDRM_DeviceData *vid = &kmsdrm;

    /* windows are destroyed by the core before this runs */
    kmsdrm_deinit_displays();      /* restores the CRTC, destroys cursor bo */
    if (vid->gbm_dev) {
        SDLOP_GBM_gbm_device_destroy(vid->gbm_dev);
        vid->gbm_dev = NULL;
        vid->gbm_init = false;
    }
    KMSDRM_GL_Deinit();            /* eglTerminate (if any) + EGL/GBM symbol unload */
    SDLOP_GBM_UnloadSymbols();
    SDLOP_KMSDRM_UnloadSymbols();
    vid->video_init = false;
}

/* ------------------------------------------------------------------ */
/* GL thunks (implemented in sdlop_kmsdrm_gl.c)                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Driver registration                                                  */
/* ------------------------------------------------------------------ */

SDLop_VideoDevice SDLop_kmsdrm_device = {
    "kmsdrm",
    kmsdrm_Init,
    kmsdrm_Quit,
    kmsdrm_CreateWindow,
    kmsdrm_DestroyWindow,
    kmsdrm_ShowWindow,
    kmsdrm_HideWindow,
    kmsdrm_SetWindowTitle,
    kmsdrm_SetWindowSize,
    kmsdrm_SetWindowPosition,
    kmsdrm_MinimizeWindow,
    kmsdrm_MaximizeWindow,
    kmsdrm_RestoreWindow,
    kmsdrm_SetWindowFullscreen,
    kmsdrm_RaiseWindow,
    kmsdrm_SetWindowClearColor,
    kmsdrm_SetWindowRelativeMouseMode,
    kmsdrm_PumpEvents,
    kmsdrm_GetEventFD,
    kmsdrm_CreateWindowFramebuffer,
    kmsdrm_UpdateWindowFramebuffer,
    kmsdrm_DestroyWindowFramebuffer,
    /* OpenGL (EGL/GBM, implemented in sdlop_kmsdrm_gl.c) */
    KMSDRM_GL_CreateContext,
    KMSDRM_GL_MakeCurrent,
    KMSDRM_GL_SwapBuffers,
    KMSDRM_GL_DeleteContext,
    KMSDRM_GL_GetProcAddress,
    KMSDRM_GL_SetSwapInterval,
    KMSDRM_GL_GetSwapInterval,
    /* Vulkan: not supported on this driver */
    NULL,
};
