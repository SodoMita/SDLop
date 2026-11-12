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
    SDLOP_KMSDRM_REQUIRE_MASTER=0           do not require DRM master at startup
                                          (default: require it; SDL3 semantics)
    SDLOP_KMSDRM_DOUBLE_BUFFER=1           wait for the flip immediately after
                                          queueing it (GL path; SDL_HINT_VIDEO_DOUBLE_BUFFER)
    SDLOP_KMSDRM_ATOMIC=0                   never use DRM atomic modesetting; always the
                                          legacy drmModeSetCrtc/drmModePageFlip path
                                          (default: try atomic, fall back automatically)

  VRR is enabled automatically when the connector reports VRR_CAPABLE and is
  restored to its saved state on quit. Panel orientation is logged when set.
  VT switching (Ctrl+Alt+Fn) is handled with VT_PROCESS mode: scanout is torn
  down and DRM master dropped on switch-away, re-acquired on switch-back.
  Vulkan windows (SDL_WINDOW_VULKAN) get a VK_KHR_display surface and own the
  output; the dumb/GL paths re-acquire master in kmsdrm_CreateWindow().

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  /* for SIGRTMIN/SIGRTMAX in VT signal setup */
#endif

#include "sdlop_kmsdrm.h"
#include "sdlop_xcursor.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/kd.h>
#include <linux/vt.h>
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

/* SDL3's SDL_HINT_KMSDRM_REQUIRE_DRM_MASTER semantics: default to requiring
 * DRM master (probing skips cards that cannot be mastered, startup fails
 * with a clear error otherwise). Set SDLOP_KMSDRM_REQUIRE_MASTER=0 to opt
 * out and get the historical warn-and-continue behavior. */
static bool kmsdrm_require_master(void)
{
    const char *v = getenv("SDLOP_KMSDRM_REQUIRE_MASTER");
    return !(v && *v && strcmp(v, "0") == 0);
}


/* ------------------------------------------------------------------ */
/* DRM master                                                          */
/* ------------------------------------------------------------------ */

/* Best-effort re-acquisition of DRM master. The device probing code drops
 * master once at the end of init (so a Vulkan window can grab it); the
 * dumb/GL scanout paths need it back. Returns false if we still don't hold
 * it afterwards (modesetting will fail with a lost-master error later). */
static bool kmsdrm_ensure_master(KMSDRM_DeviceData *vid)
{
    if (vid->drm_fd < 0) {
        vid->drm_fd = open(vid->devpath, O_RDWR | O_CLOEXEC);
        if (vid->drm_fd < 0) {
            return false;
        }
    }
    if (SDLOP_DRM_drmSetMaster) {
        SDLOP_DRM_drmSetMaster(vid->drm_fd);
    }
    if (SDLOP_DRM_drmAuthMagic && SDLOP_DRM_drmAuthMagic(vid->drm_fd, 0) == -EACCES) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* KMS object properties (VRR / panel orientation), ported from SDL3   */
/* ------------------------------------------------------------------ */

/* find the prop_id of the property `name` among `props` (frees nothing) */
static uint32_t kmsdrm_prop_id(int fd, drmModeObjectPropertiesPtr props, const char *name)
{
    uint32_t prop_id = 0;
    if (!SDLOP_DRM_drmModeGetProperty) {
        return 0;
    }
    for (uint32_t i = 0; !prop_id && i < props->count_props; ++i) {
        drmModePropertyPtr drm_prop = SDLOP_DRM_drmModeGetProperty(fd, props->props[i]);
        if (!drm_prop) {
            continue;
        }
        if (strcmp(drm_prop->name, name) == 0) {
            prop_id = drm_prop->prop_id;
        }
        SDLOP_DRM_drmModeFreeProperty(drm_prop);
    }
    return prop_id;
}

/* read the raw value of property `name` on `object_id`/`object_type`;
 * returns false if the property does not exist */
static bool kmsdrm_prop_get_value(int fd, uint32_t object_id, uint32_t object_type,
                                  const char *name, uint64_t *value)
{
    if (!SDLOP_DRM_drmModeObjectGetProperties || !SDLOP_DRM_drmModeFreeObjectProperties) {
        return false;
    }
    drmModeObjectPropertiesPtr props =
        SDLOP_DRM_drmModeObjectGetProperties(fd, object_id, object_type);
    if (!props) {
        return false;
    }
    bool found = false;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr drm_prop = SDLOP_DRM_drmModeGetProperty(fd, props->props[i]);
        if (!drm_prop) {
            continue;
        }
        if (strcmp(drm_prop->name, name) == 0) {
            *value = props->prop_values[i];
            found = true;
        }
        SDLOP_DRM_drmModeFreeProperty(drm_prop);
        if (found) {
            break;
        }
    }
    SDLOP_DRM_drmModeFreeObjectProperties(props);
    return found;
}

/* set a property on an object; best effort (caller holds DRM master) */
static void kmsdrm_prop_set_value(int fd, uint32_t object_id, uint32_t object_type,
                                  const char *name, uint64_t value)
{
    if (!SDLOP_DRM_drmModeObjectGetProperties || !SDLOP_DRM_drmModeObjectSetProperty) {
        return;
    }
    drmModeObjectPropertiesPtr props =
        SDLOP_DRM_drmModeObjectGetProperties(fd, object_id, object_type);
    if (!props) {
        return;
    }
    uint32_t prop_id = kmsdrm_prop_id(fd, props, name);
    SDLOP_DRM_drmModeFreeObjectProperties(props);
    if (prop_id) {
        SDLOP_DRM_drmModeObjectSetProperty(fd, object_id, object_type, prop_id, value);
    }
}

static bool kmsdrm_connector_check_vrr_capable(int fd, uint32_t connector_id, const char *name)
{
    uint64_t value = 0;
    if (!kmsdrm_prop_get_value(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, name, &value)) {
        return false;
    }
    return value != 0;
}

static bool KMSDRM_CrtcGetVrr(int fd, uint32_t crtc_id)
{
    uint64_t value = 0;
    if (!kmsdrm_prop_get_value(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", &value)) {
        return false;
    }
    return value != 0;
}

static void KMSDRM_CrtcSetVrr(int fd, uint32_t crtc_id, bool enabled)
{
    kmsdrm_prop_set_value(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", enabled ? 1 : 0);
}

/* panel orientation is a *connector* property ("panel orientation"), enum
 * valued; map it to degrees (0 = unknown/normal). SDLop has no display
 * properties API, so the value is stored on the display and logged. */
static int KMSDRM_ConnectorGetOrientation(int fd, uint32_t connector_id)
{
    if (!SDLOP_DRM_drmModeObjectGetProperties || !SDLOP_DRM_drmModeGetProperty
        || !SDLOP_DRM_drmModeFreeProperty || !SDLOP_DRM_drmModeFreeObjectProperties) {
        return 0;
    }
    drmModeObjectPropertiesPtr props =
        SDLOP_DRM_drmModeObjectGetProperties(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!props) {
        return 0;
    }
    int orientation = 0;
    bool done = false;
    for (uint32_t i = 0; i < props->count_props && !done; ++i) {
        drmModePropertyPtr drm_prop = SDLOP_DRM_drmModeGetProperty(fd, props->props[i]);
        if (!drm_prop) {
            continue;
        }
        if (strcmp(drm_prop->name, "panel orientation") == 0
            && (drm_prop->flags & DRM_MODE_PROP_ENUM)) {
            /* match the *current* value against the enum list (SDL3 reads
             * enums[0] which is not necessarily the current one) */
            for (int j = 0; j < drm_prop->count_enums; ++j) {
                if (drm_prop->enums[j].value == props->prop_values[i]) {
                    if (strcmp(drm_prop->enums[j].name, "Upside Down") == 0) {
                        orientation = 180;
                    } else if (strcmp(drm_prop->enums[j].name, "Left Side Up") == 0) {
                        orientation = 90;
                    } else if (strcmp(drm_prop->enums[j].name, "Right Side Up") == 0) {
                        orientation = 270;
                    }
                    break;
                }
            }
            done = true;
        }
        SDLOP_DRM_drmModeFreeProperty(drm_prop);
    }
    SDLOP_DRM_drmModeFreeObjectProperties(props);
    return orientation;
}

/* ------------------------------------------------------------------ */
/* VT switch handling (VT_PROCESS mode, ported from SDL3)              */
/* ------------------------------------------------------------------ */

#define SDLOP_KMSDRM_VT_NONE    0
#define SDLOP_KMSDRM_VT_RELEASE 1
#define SDLOP_KMSDRM_VT_ACQUIRE 2

static volatile sig_atomic_t kmsdrm_vt_pending = SDLOP_KMSDRM_VT_NONE;
static int kmsdrm_vt_console_fd = -1;
static int kmsdrm_vt_saved_kbmode = -1;
static int kmsdrm_vt_release_sig = 0;
static int kmsdrm_vt_acquire_sig = 0;
static struct sigaction kmsdrm_vt_old_release;
static struct sigaction kmsdrm_vt_old_acquire;

static void kmsdrm_vt_release_action(int signum)
{
    (void)signum;
    kmsdrm_vt_pending = SDLOP_KMSDRM_VT_RELEASE;
}

static void kmsdrm_vt_acquire_action(int signum)
{
    (void)signum;
    kmsdrm_vt_pending = SDLOP_KMSDRM_VT_ACQUIRE;
}

static bool kmsdrm_vt_setup_signal(int signum, void (*handler)(int), struct sigaction *old_out)
{
    struct sigaction new_action;
    memset(&new_action, 0, sizeof(new_action));
    new_action.sa_handler = handler;
    new_action.sa_flags = SA_RESTART;
    if (sigaction(signum, &new_action, old_out) < 0) {
        return false;
    }
    if (old_out->sa_handler != SIG_DFL) {
        /* signal in use: restore and report failure */
        sigaction(signum, old_out, NULL);
        return false;
    }
    return true;
}

static int kmsdrm_vt_find_free_signal(void (*handler)(int), struct sigaction *old_out)
{
#ifdef SIGRTMIN
    for (int signum = SIGRTMIN + 2; signum <= SIGRTMAX; ++signum) {
        if (kmsdrm_vt_setup_signal(signum, handler, old_out)) {
            return signum;
        }
    }
#endif
    if (kmsdrm_vt_setup_signal(SIGUSR1, handler, old_out)) {
        return SIGUSR1;
    }
    if (kmsdrm_vt_setup_signal(SIGUSR2, handler, old_out)) {
        return SIGUSR2;
    }
    return 0;
}

static void kmsdrm_vt_shutdown(void)
{
    if (kmsdrm_vt_release_sig) {
        sigaction(kmsdrm_vt_release_sig, &kmsdrm_vt_old_release, NULL);
        kmsdrm_vt_release_sig = 0;
    }
    if (kmsdrm_vt_acquire_sig) {
        sigaction(kmsdrm_vt_acquire_sig, &kmsdrm_vt_old_acquire, NULL);
        kmsdrm_vt_acquire_sig = 0;
    }
    if (kmsdrm_vt_console_fd >= 0) {
        if (kmsdrm_vt_saved_kbmode >= 0) {
            ioctl(kmsdrm_vt_console_fd, KDSKBMODE, kmsdrm_vt_saved_kbmode);
            kmsdrm_vt_saved_kbmode = -1;
        }
        ioctl(kmsdrm_vt_console_fd, KDSETMODE, KD_TEXT);
        struct vt_mode mode;
        memset(&mode, 0, sizeof(mode));
        mode.mode = VT_AUTO;
        ioctl(kmsdrm_vt_console_fd, VT_SETMODE, &mode);
        close(kmsdrm_vt_console_fd);
        kmsdrm_vt_console_fd = -1;
    }
    kmsdrm_vt_pending = SDLOP_KMSDRM_VT_NONE;
}

static void kmsdrm_vt_init(void)
{
    /* opening /dev/tty may fail (no controlling console, e.g. service or
     * ssh) - in that case the kernel keeps VT_AUTO mode and we rely on the
     * lost-master recovery paths instead */
    int fd = open("/dev/tty", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        kmsdrm_log("KMSDRM: no controlling tty; VT switching left to the kernel");
        return;
    }
    char kbtype = 0;
    if (ioctl(fd, KDGKBTYPE, &kbtype) != 0 || (kbtype != KB_101 && kbtype != KB_84)) {
        close(fd);
        kmsdrm_log("KMSDRM: /dev/tty is not a keyboard console; VT switching left to the kernel");
        return;
    }

    kmsdrm_vt_release_sig = kmsdrm_vt_find_free_signal(kmsdrm_vt_release_action, &kmsdrm_vt_old_release);
    kmsdrm_vt_acquire_sig = kmsdrm_vt_find_free_signal(kmsdrm_vt_acquire_action, &kmsdrm_vt_old_acquire);
    if (!kmsdrm_vt_release_sig || !kmsdrm_vt_acquire_sig) {
        kmsdrm_vt_shutdown();
        kmsdrm_log("KMSDRM: could not claim signals for VT switching; leaving VT_AUTO");
        return;
    }

    struct vt_mode mode;
    memset(&mode, 0, sizeof(mode));
    mode.mode = VT_PROCESS;
    mode.relsig = (short)kmsdrm_vt_release_sig;
    mode.acqsig = (short)kmsdrm_vt_acquire_sig;
    mode.frsig = SIGIO;
    if (ioctl(fd, VT_SETMODE, &mode) < 0) {
        kmsdrm_vt_shutdown();
        kmsdrm_log("KMSDRM: VT_SETMODE failed (%s); leaving VT_AUTO", strerror(errno));
        return;
    }
    /* full graphics takeover (swa behavior): disable the kernel keyboard
     * handler so Ctrl-C etc. don't kill us, and switch the VT to graphics
     * mode so fbcon/text does not draw over us. Restored in
     * kmsdrm_vt_shutdown(). */
    {
        int kbmode = 0;
        if (ioctl(fd, KDGKBMODE, &kbmode) == 0) {
            kmsdrm_vt_saved_kbmode = kbmode;
            if (ioctl(fd, KDSKBMODE, K_OFF) != 0) {
                kmsdrm_log("KMSDRM: KDSKBMODE K_OFF failed: %s", strerror(errno));
            }
        } else {
            kmsdrm_vt_saved_kbmode = -1;
            kmsdrm_log("KMSDRM: KDGKBMODE failed: %s", strerror(errno));
        }
        if (ioctl(fd, KDSETMODE, KD_GRAPHICS) != 0) {
            kmsdrm_log("KMSDRM: KDSETMODE KD_GRAPHICS failed: %s", strerror(errno));
        }
    }

    kmsdrm_vt_console_fd = fd;
    kmsdrm_log("KMSDRM: VT switching handled (relsig=%d acqsig=%d)",
               kmsdrm_vt_release_sig, kmsdrm_vt_acquire_sig);
}

/* release the current VT: free GL scanouts (GBM/EGL does not know we lost
 * the VT), keep the dumb buffers (they stay valid; the CRTC is simply
 * reprogrammed on switch-back), drop DRM master */
static void kmsdrm_vt_release(void)
{
    KMSDRM_DeviceData *vid = &kmsdrm;
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        KMSDRM_WindowData *windata = (KMSDRM_WindowData *)w->driverdata;
        if (!windata) {
            continue;
        }
        if (windata->waiting_for_flip) {
            KMSDRM_WaitPageflip(vid, windata, 500);
        }
        if (windata->gl_active) {
            KMSDRM_DestroyGLSurfaces(windata);
        }
        windata->crtc_programmed = false;
        windata->sw_front = -1;
    }
    if (SDLOP_DRM_drmDropMaster) {
        SDLOP_DRM_drmDropMaster(vid->drm_fd);
    }
    if (kmsdrm_vt_console_fd >= 0) {
        ioctl(kmsdrm_vt_console_fd, VT_RELDISP, 1);
    }
}

/* acquire the VT: take DRM master back; windows reprogram their CRTC on the
 * next present (software: drmModeSetCrtc; GL: lazily on next swap) */
static void kmsdrm_vt_acquire(void)
{
    KMSDRM_DeviceData *vid = &kmsdrm;
    kmsdrm_ensure_master(vid);
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        KMSDRM_WindowData *windata = (KMSDRM_WindowData *)w->driverdata;
        if (windata) {
            windata->crtc_programmed = false;
        }
    }
    if (kmsdrm_vt_console_fd >= 0) {
        ioctl(kmsdrm_vt_console_fd, VT_RELDISP, VT_ACKACQ);
    }
}

static void kmsdrm_vt_check(void)
{
    int pending = kmsdrm_vt_pending;
    if (pending == SDLOP_KMSDRM_VT_RELEASE) {
        kmsdrm_vt_pending = SDLOP_KMSDRM_VT_NONE;
        kmsdrm_vt_release();
    } else if (pending == SDLOP_KMSDRM_VT_ACQUIRE) {
        kmsdrm_vt_pending = SDLOP_KMSDRM_VT_NONE;
        kmsdrm_vt_acquire();
    }
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
            if (usable && has_connected && kmsdrm_require_master()
                && SDLOP_DRM_drmSetMaster && SDLOP_DRM_drmAuthMagic) {
                /* SDL3's SDL_HINT_KMSDRM_REQUIRE_DRM_MASTER: do not pick a
                 * card we cannot become master on. Master is re-acquired in
                 * kmsdrm_CreateWindow() (and released at the end of init). */
                SDLOP_DRM_drmSetMaster(fd);
                if (SDLOP_DRM_drmAuthMagic(fd, 0) == -EACCES) {
                    has_connected = false;
                }
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

    /* VRR: save the CRTC's current state; enable it if the connector says
     * it can do variable refresh rate (SDL3 behavior). Restored on quit. */
    vid->display.saved_vrr = KMSDRM_CrtcGetVrr(vid->drm_fd, vid->display.crtc->crtc_id);
    vid->display.vrr_enabled = false;
    if (kmsdrm_connector_check_vrr_capable(vid->drm_fd, vid->display.connector->connector_id,
                                           "VRR_CAPABLE")) {
        KMSDRM_CrtcSetVrr(vid->drm_fd, vid->display.crtc->crtc_id, true);
        vid->display.vrr_enabled = true;
        kmsdrm_log("KMSDRM: enabling VRR (connector reports VRR_CAPABLE)");
    }

    /* panel orientation (connector property); SDLop has no display
     * properties API, so store + log it for diagnostics */
    vid->display.panel_orientation_degrees =
        KMSDRM_ConnectorGetOrientation(vid->drm_fd, vid->display.connector->connector_id);
    if (vid->display.panel_orientation_degrees != 0) {
        kmsdrm_log("KMSDRM: panel orientation: %d degrees",
                   vid->display.panel_orientation_degrees);
    }

    /* atomic modesetting probe: sets the client cap, finds the CRTC's
     * primary plane and caches the property ids. Never fatal - on any
     * failure the driver silently keeps the legacy path. */
    KMSDRM_AtomicTryInit(vid, &vid->display);

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

    /* drop the cached atomic mode blob before the fd goes away */
    if (vid->drm_fd >= 0 && vid->display.atomic.mode_blob
        && SDLOP_DRM_ATOMIC_Available()) {
        SDLOP_DRM_ATOMIC_drmModeDestroyPropertyBlob(vid->drm_fd, vid->display.atomic.mode_blob);
    }
    memset(&vid->display.atomic, 0, sizeof(vid->display.atomic));

    /* restore the CRTC's VRR state to what it was at init (best effort) */
    if (vid->drm_fd >= 0 && vid->display.crtc) {
        KMSDRM_CrtcSetVrr(vid->drm_fd, vid->display.crtc->crtc_id, vid->display.saved_vrr);
        vid->display.vrr_enabled = false;
    }

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

int KMSDRM_ModeRefreshMHz(const drmModeModeInfo *m)
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
            (cost == best_cost && best && KMSDRM_ModeRefreshMHz(m) > KMSDRM_ModeRefreshMHz(best))) {
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

void KMSDRM_FlipHandler(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
                        unsigned int crtc_id, void *data)
{
    (void)fd; (void)frame; (void)sec; (void)usec; (void)crtc_id;
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
    ev.page_flip_handler2 = KMSDRM_FlipHandler;

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

    if (dd->atomic.available) {
        if (KMSDRM_AtomicModeset(vid, dd, fb_id)) {
            windata->crtc_programmed = true;
            windata->lost_master = false;
            return true;
        }
        int atomic_err = errno;
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                    "KMSDRM: atomic modeset failed (%s); falling back to drmModeSetCrtc",
                    strerror(atomic_err));
        if (is_permission_error(atomic_err)) {
            windata->lost_master = true;
        } else if (atomic_err == EINVAL || atomic_err == ENOSYS) {
            /* hard rejection: this driver will not do atomic for this
             * display either; stop paying for a failing ioctl per flip */
            windata->display->atomic.available = false;
        }
        /* fall through to the legacy path */
    }

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
    const bool use_atomic = windata->display->atomic.available;
    if (!windata->crtc_programmed || vid->no_flip) {
        ok = kmsdrm_crtc_set(vid, windata, fb_id);
    } else if (use_atomic) {
        int ret = KMSDRM_AtomicFlip(vid, windata->display, fb_id, &windata->waiting_for_flip);
        if (ret == 0) {
            windata->waiting_for_flip = true;
            ok = true;
        } else {
            int err = errno;
            if (err == EBUSY && KMSDRM_WaitPageflip(vid, windata, flip_timeout_ms())) {
                /* a flip was pending; retry once */
                ret = KMSDRM_AtomicFlip(vid, windata->display, fb_id, &windata->waiting_for_flip);
                if (ret == 0) {
                    windata->waiting_for_flip = true;
                    ok = true;
                } else {
                    ok = kmsdrm_crtc_set(vid, windata, fb_id);
                }
            } else if (is_permission_error(err)) {
                windata->lost_master = true;
                ok = SDL_SetError("atomic flip: no DRM master (%s)", strerror(err));
            } else {
                /* atomic not usable for this CRTC after all: fall back to a
                 * modeset for this present; latch no_flip on hard errors so
                 * we stop asking for flips the driver rejected outright */
                if (err == EINVAL || err == ENOSYS) {
                    vid->no_flip = true;
                    windata->display->atomic.available = false;
                }
                SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                            "KMSDRM: atomic flip failed (%s); falling back to modeset",
                            strerror(err));
                ok = kmsdrm_crtc_set(vid, windata, fb_id);
            }
        }
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

/* XCursor theme cache (loaded at most once per driver lifetime).
 * Returns NULL when no theme is available (e.g. minimal systems); the
 * built-in arrow below is used as the fallback cursor then. */
static struct sdlop_xcursor_theme *kmsdrm_cursor_theme_cache = NULL;
static bool kmsdrm_cursor_theme_tried = false;

static struct sdlop_xcursor_theme *kmsdrm_cursor_theme_load_once(void)
{
    if (!kmsdrm_cursor_theme_tried) {
        kmsdrm_cursor_theme_tried = true;
        const char *theme_name = getenv("XCURSOR_THEME");
        int size = 32;
        const char *ss = getenv("XCURSOR_SIZE");
        if (ss) {
            long s = strtol(ss, NULL, 10);
            if (s >= 8 && s <= 256) {
                size = (int)s;
            }
        }
        kmsdrm_cursor_theme_cache = sdlop_xcursor_theme_load(theme_name, size);
        if (!kmsdrm_cursor_theme_cache) {
            SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO,
                         "KMSDRM: no XCursor theme found; using built-in arrow cursor");
        }
    }
    return kmsdrm_cursor_theme_cache;
}

static struct sdlop_xcursor *kmsdrm_theme_arrow(void)
{
    struct sdlop_xcursor_theme *t = kmsdrm_cursor_theme_load_once();
    if (!t) {
        return NULL;
    }
    static const char *const names[] = { "left_ptr", "default", "arrow", "top_left_arrow", NULL };
    for (int i = 0; names[i]; i++) {
        struct sdlop_xcursor *c = sdlop_xcursor_theme_get_cursor(t, names[i]);
        if (c) {
            return c;
        }
    }
    return NULL;
}

void KMSDRM_XCursorQuit(void)
{
    if (kmsdrm_cursor_theme_cache) {
        sdlop_xcursor_theme_destroy(kmsdrm_cursor_theme_cache);
        kmsdrm_cursor_theme_cache = NULL;
    }
    kmsdrm_cursor_theme_tried = false;
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

    /* prefer the system's XCursor theme arrow when it fits the hardware
     * cursor size (swa behavior); otherwise draw the built-in 16x16 arrow */
    struct sdlop_xcursor *theme_cursor = kmsdrm_theme_arrow();
    struct sdlop_xcursor_image *theme_image = NULL;
    if (theme_cursor && theme_cursor->image_count > 0) {
        theme_image = theme_cursor->images[0];
        if (theme_image->width == 0 || theme_image->height == 0
            || theme_image->width > cw || theme_image->height > ch) {
            SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO,
                         "KMSDRM: XCursor image %ux%u exceeds hardware cursor %llux%llu; "
                         "using built-in arrow",
                         theme_image->width, theme_image->height,
                         (unsigned long long)cw, (unsigned long long)ch);
            theme_image = NULL;
        }
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
    int32_t hot_x = 0, hot_y = 0;
    if (theme_image) {
        /* XCursor pixels are ARGB8888 in memory on little-endian hosts,
         * which is exactly GBM_FORMAT_ARGB8888; rows are packed */
        for (uint32_t y = 0; y < theme_image->height; y++) {
            memcpy(buf + (size_t)y * cw, theme_image->buffer + (size_t)y * theme_image->width * 4,
                   (size_t)theme_image->width * 4);
        }
        hot_x = (int32_t)theme_image->hotspot_x;
        hot_y = (int32_t)theme_image->hotspot_y;
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO,
                     "KMSDRM: using XCursor theme cursor '%s' (%ux%u, hot %+d,%+d)",
                     theme_cursor->name, theme_image->width, theme_image->height, hot_x, hot_y);
    } else {
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
    dd->cursor_hot_x = hot_x;
    dd->cursor_hot_y = hot_y;
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

    /* Vulkan windows get their scanout from VK_KHR_display instead of the
     * dumb/GBM paths; leave the CRTC alone and keep the requested size */
    if (window->flags & SDL_WINDOW_VULKAN) {
        window->driverdata = windata;
        sdlop.keyboard_focus = window;
        sdlop.mouse_focus = window;
        window->flags |= SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
        kmsdrm_log("KMSDRM: Vulkan window (scanout handled by VK_KHR_display)");
        return true;
    }

    /* init dropped DRM master (see kmsdrm_Init); take it back for the
     * dumb/GBM scanout paths. Best effort - modesetting fails cleanly
     * later with a lost-master error if we cannot get it. */
    if (!kmsdrm_ensure_master(vid)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                    "KMSDRM: cannot re-acquire DRM master on %s; modesetting will fail",
                    vid->devpath);
    }

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
    if ((window->flags & SDL_WINDOW_VULKAN) || windata->gl_active) {
        return; /* Vulkan/GL owns presentation */
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

    /* Vulkan windows: the display mode is negotiated with the Vulkan driver
     * when the VK_KHR_display surface is created; don't modeset here */
    if (window->flags & SDL_WINDOW_VULKAN) {
        return true;
    }

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

    /* handle VT switch requests (release: drop master; acquire: re-take) */
    kmsdrm_vt_check();

    /* drain pending DRM events (page flip completions), non-blocking */
    if (vid->drm_fd >= 0) {
        struct pollfd pfd = { vid->drm_fd, POLLIN, 0 };
        int ret = poll(&pfd, 1, 0);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            drmEventContext ev = { 0 };
            ev.version = DRM_EVENT_CONTEXT_VERSION;
            ev.page_flip_handler2 = KMSDRM_FlipHandler;
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
    if (window->flags & SDL_WINDOW_VULKAN) {
        return SDL_SetError("Vulkan is active: window surface is not presentable");
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
    if (window->flags & SDL_WINDOW_VULKAN) {
        return SDL_SetError("Vulkan is active: window surface is not presentable");
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

    /* become DRM master; the modeset path requires it. With
     * SDLOP_KMSDRM_REQUIRE_MASTER=1 (default) refusal is fatal - clearer
     * than failing on the first present; set =0 for the historical
     * warn-and-continue behavior. */
    if (SDLOP_DRM_drmSetMaster) {
        SDLOP_DRM_drmSetMaster(vid->drm_fd);
    }
    if (SDLOP_DRM_drmAuthMagic && SDLOP_DRM_drmAuthMagic(vid->drm_fd, 0) == -EACCES) {
        if (kmsdrm_require_master()) {
            SDL_SetError("KMSDRM: cannot become DRM master of %s "
                         "(set SDLOP_KMSDRM_REQUIRE_MASTER=0 to skip this check)",
                         vid->devpath);
            goto failed_close;
        }
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

    kmsdrm_vt_init();

    /* SDL3 drops DRM master after probing so a Vulkan window (VK_KHR_display)
     * can acquire it; non-Vulkan windows re-acquire master in
     * kmsdrm_CreateWindow(). Best effort: staying master works for the
     * dumb/GL paths too. */
    if (SDLOP_DRM_drmDropMaster) {
        SDLOP_DRM_drmDropMaster(vid->drm_fd);
    }

    vid->video_init = true;
    return true;

failed_close:
    kmsdrm_vt_shutdown();
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
    KMSDRM_XCursorQuit();          /* drop the cached XCursor theme */
    kmsdrm_vt_shutdown();          /* restore VT_AUTO mode on /dev/tty */
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
    /* Vulkan (VK_KHR_surface + VK_KHR_display) - sdlop_kmsdrm_vulkan.c */
    SDLOP_KMSDRM_Vulkan_CreateSurface,
    SDLOP_KMSDRM_Vulkan_GetInstanceExtensions,
};
