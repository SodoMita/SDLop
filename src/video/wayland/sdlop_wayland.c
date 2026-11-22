/*
  SDLop - Wayland video driver.

  Windowing (wl_compositor + xdg-shell) and fallback input (wl_seat:
  keyboard + pointer). When the asyncinput-style evdev worker is running,
  seat input is ignored for the covered device class (evdev wins - lower
  latency); the wl_keyboard keymap is still used for layout translation.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "internal/scancode_evdev.h"
#include "sdlop_wayland_internal.h"
#include "sdlop_wayland_scroll.h"

/* dlopen-ed libwayland-client (SDL3-style); must precede every
 * *-client-protocol.h include (their inlines compile against the
 * SDLOP_WL_* pointers) */
#include "sdlop_wayland_dyn.h"
#include "../sdlop_egl_dyn.h"

#include "pointer-constraints-client-protocol.h"
#include "relative-pointer-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "idle-inhibit-client-protocol.h"
#include "keyboard-shortcuts-inhibit-client-protocol.h"
#include "xdg-activation-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "alpha-modifier-v1-client-protocol.h"
#include "pointer-warp-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"
#include "../sdlop_xcursor.h"

#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#include "xdg-shell-client-protocol.h"

/* wayland-protocols < 1.32 (Debian 12, Ubuntu 22.04) predates the
 * xdg_toplevel "suspended" state; the value is fixed by the protocol */
#ifndef XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION
#define XDG_TOPLEVEL_STATE_SUSPENDED 9
#endif

/* one wl_shm buffer of a window's software-presentation cycle */
#define SDLOP_WL_SHM_MAX_BUFFERS 3
typedef struct WaylandShmBuffer
{
    struct wl_buffer *buffer;
    void *map;
    size_t size;
    bool busy;      /* committed, wl_buffer.release not yet received */
    SDL_Rect stale; /* shadow region not yet copied into this buffer (w == 0: none) */
} WaylandShmBuffer;

typedef struct WaylandWindowData
{
    SDL_Window *window;
    struct wl_surface *surface;
    struct xdg_surface *xsurface;
    struct xdg_toplevel *toplevel;
    struct xdg_popup *popup;      /* SDL_WINDOW_TOOLTIP / SDL_WINDOW_POPUP_MENU */

    /* software presentation (see "shm buffers" below) */
    WaylandShmBuffer shm[SDLOP_WL_SHM_MAX_BUFFERS];
    int num_shm;
    int buffer_w, buffer_h;         /* size of the shm[] buffers */
    WaylandShmBuffer *front;        /* last committed shm[] buffer; NULL: clear colour or nothing */
    WaylandShmBuffer *offload_buf;  /* acquired by the GL offload, committed by PresentOffloadShm */
    void *shadow;                   /* window surface pixels (stable between presents) */
    int shadow_w, shadow_h;
    struct wl_buffer *clear_buffer; /* 1x1 clear colour, scaled by the viewport */
    uint32_t clear_buffer_color;
    struct wp_viewport *viewport;
    int dest_w, dest_h;             /* last requested viewport destination (-1: none) */

    /* HiDPI (stock scale_factor model): the preferred scale of the surface
     * comes from wp_fractional_scale_v1, else wl_surface v6
     * preferred_buffer_scale, else the largest scale of the entered outputs.
     * SDL_WINDOW_HIGH_PIXEL_DENSITY windows render round(size * scale)
     * pixels, shown at the logical size by the viewport (or an integer
     * buffer scale without wp_viewporter, the scale then rounded up). */
    struct wp_fractional_scale_v1 *fractional;
    double scale_factor;
    bool buffer_scale_received;
    struct wl_output *entered[8];
    int num_entered;
    int pixel_w, pixel_h; /* pixel size last reported (PIXEL_SIZE_CHANGED) */

    bool configured;
    bool mapped;
    bool moved_reported; /* stock emits exactly one MOVED(0,0) per window */
    struct wl_output *output; /* output the surface currently intersects */
    uint32_t toplevel_states;
    int pending_w, pending_h;

    /* SDL 3.4 window management */
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct zwp_idle_inhibitor_v1 *idle_inhibitor;
    struct zwp_keyboard_shortcuts_inhibitor_v1 *shortcuts_inhibitor;
    struct xdg_dialog_v1 *dialog;      /* SDL_WINDOW_MODAL (xdg_wm_dialog_v1) */
    int hit_test_result;               /* SDL_HitTestResult under the pointer */
    struct wp_alpha_modifier_surface_v1 *alpha; /* SDL_SetWindowOpacity, created on first use */
    bool transparent;    /* SDL_WINDOW_TRANSPARENT: ARGB8888 buffers, no opaque region */
    bool clear_explicit; /* SDLop_SetWindowClearColor called: an opaque clear even when transparent */
    struct xdg_toplevel_icon_v1 *icon; /* SDL_SetWindowIcon, kept for re-shown toplevels */
    struct zxdg_exported_v2 *exported; /* xdg-foreign handle of the toplevel (portal dialog parent) */
    struct wl_buffer **icon_buffers;   /* must outlive the icon */
    int num_icon_buffers;

    /* GL presentation state (managed by sdlop_wayland_gl.c) */
    void *gl_surface; /* EGLSurface or SDLOP_GLOffloadSurface* */
    void *gl_egl_window; /* wl_egl_window backing the EGLSurface */
    void *gl_offload_ctx; /* SDLOP_GLOffloadContext* last bound */
    bool gl_active;
    bool gl_offload_active;
} WaylandWindowData;

typedef struct WaylandDeviceData
{
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    SDL_KeyboardID keyboard_id;
    SDL_MouseID pointer_id;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_touch *touch;
#define SDLOP_MAX_OUTPUTS 16
    struct
    {
        struct wl_output *output;
        uint32_t name; /* registry name, for global_remove */
        int32_t scale;
        int32_t x, y;             /* geometry (logical origin) */
        int32_t mode_w, mode_h;   /* current mode, pixels */
        char name_str[64];        /* xdg output name, when known */
        SDL_DisplayID display_id; /* core display, once registered */
    } outputs[SDLOP_MAX_OUTPUTS];
    int num_outputs;
    struct
    {
        bool active;
        Uint64 id;
        float nx, ny; /* last normalized position, for dx/dy */
    } fingers[16];
    struct xdg_wm_base *wm_base;
    uint32_t compositor_version;
    uint32_t seat_version;
    bool has_dmabuf;
    bool has_wl_drm;

    SDL_Window *pointer_focus;
    SDL_Window *touch_focus;
    /* wheel, per pointer frame (stock SDL3's model): a frame's value120
     * (seat v8+) or discrete (v5-7) steps win over its continuous value,
     * which counts 10 units per detent */
    float axis_x_acc, axis_y_acc;
    Uint8 axis_x_type, axis_y_type; /* AXIS_CONTINUOUS / AXIS_DISCRETE / AXIS_VALUE120 */
    SDL_MouseWheelDirection axis_direction;

    /* relative mouse mode (pointer lock) */
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_relative_pointer_manager_v1 *relative_manager;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct xdg_toplevel_icon_manager_v1 *toplevel_icon_manager;
    struct zxdg_exporter_v2 *exporter; /* xdg-foreign: toplevel handles for other clients */
    struct xdg_wm_dialog_v1 *wm_dialog;
    struct wp_alpha_modifier_v1 *alpha_modifier;
    struct wp_pointer_warp_v1 *pointer_warp;
    /* touchpad pinch (stock: zwp_pointer_gestures_v1 up to v3) */
    struct zwp_pointer_gestures_v1 *pointer_gestures;
    struct zwp_pointer_gesture_pinch_v1 *gesture_pinch; /* of `pointer` */
    SDL_Window *pinch_focus;
    struct wp_cursor_shape_device_v1 *cursor_shape_device; /* of `pointer` */
    struct zwp_locked_pointer_v1 *locked_pointer;
    struct zwp_relative_pointer_v1 *relative_pointer;
    SDL_Window *relative_lock_window;
    float rel_x_acc, rel_y_acc;

    /* SDL 3.4 window management / cursors */
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct zwp_idle_inhibit_manager_v1 *idle_inhibit_manager;
    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *shortcuts_inhibit_manager;
    struct xdg_activation_v1 *activation;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct wl_event_queue *shm_queue; /* wl_buffer.release of window buffers only */
    struct zwp_confined_pointer_v1 *confined_pointer;
    SDL_Window *confine_window;
    uint32_t pointer_serial;      /* last wl_pointer.enter serial (set_cursor) */
    uint32_t input_serial;        /* last button/key serial (xdg_popup grab) */
    uint32_t keyboard_serial;     /* last wl_keyboard.enter serial */
    struct sdlop_xcursor_theme *cursor_theme;
    bool cursor_theme_tried;
} WaylandDeviceData;

static WaylandDeviceData wl_data;

/* forward decls (pointer lock helpers, defined below) */
static bool wayland_arm_pointer_lock(SDL_Window *window);
static void wayland_release_pointer_lock(void);

/* ------------------------------------------------------------------ */
/* shm buffers                                                         */
/* ------------------------------------------------------------------ */
/*
  Software presentation follows the wl_buffer contract: a committed buffer
  belongs to the compositor until wl_buffer.release and is never written
  while it is busy.

  - The window surface's pixels are a client-side shadow at a fixed address
    between presents (like stock SDL3's framebuffer), so applications may
    keep the pointer.
  - SDL_UpdateWindowSurface(Rects) copies into a free buffer of a small
    per-window cycle (normally 2, a 3rd only while the compositor holds the
    others) and commits it. Each buffer remembers the shadow region it has
    not seen yet, so a present copies the new damage plus what that buffer
    missed - in steady state the damage of the last two presents, not the
    whole window.
  - Release events go to a private event queue: looking for a free buffer
    never dispatches input or window events behind the application's back.
    Only when every buffer is held does a present wait for a release.
  - Until something is presented a window shows its clear colour: a 1x1
    buffer scaled to the window by wp_viewporter (no per-pixel fill when a
    window is created), or a filled full-size buffer without viewporter.
*/

static void shm_buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    ((WaylandShmBuffer *)data)->busy = false;
}

static const struct wl_buffer_listener shm_buffer_listener = {
    shm_buffer_release,
};

static void rect_add(SDL_Rect *acc, const SDL_Rect *r)
{
    if (r->w <= 0 || r->h <= 0) {
        return;
    }
    if (acc->w <= 0 || acc->h <= 0) {
        *acc = *r;
        return;
    }
    const int x0 = SDL_min(acc->x, r->x), y0 = SDL_min(acc->y, r->y);
    const int x1 = SDL_max(acc->x + acc->w, r->x + r->w), y1 = SDL_max(acc->y + acc->h, r->y + r->h);
    acc->x = x0;
    acc->y = y0;
    acc->w = x1 - x0;
    acc->h = y1 - y0;
}

/* the clear colour as a buffer pixel: transparent windows start fully
 * transparent unless a colour was set (as on X11) */
static uint32_t window_clear_xrgb(SDL_Window *window)
{
    const WaylandWindowData *wd = (const WaylandWindowData *)window->driverdata;
    const uint32_t rgb = ((uint32_t)window->clear_r << 16) | ((uint32_t)window->clear_g << 8) | window->clear_b;
    if (wd && wd->transparent) {
        return wd->clear_explicit ? (rgb | 0xff000000u) : 0;
    }
    return rgb;
}

/* the opaque region lets the compositor skip blending (stock: every
 * window but transparent or translucent ones). Parts outside the surface
 * are ignored by the compositor, so it needn't follow the size. */
static void window_update_opaque_region(SDL_Window *window, WaylandWindowData *wd)
{
    if (!wd->transparent && window->opacity >= 1.0f) {
        struct wl_region *region = wl_compositor_create_region(wl_data.compositor);
        wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_set_opaque_region(wd->surface, region);
        wl_region_destroy(region);
    } else {
        wl_surface_set_opaque_region(wd->surface, NULL);
    }
}

static void fill_xrgb(void *pixels, size_t npix, uint32_t color)
{
    uint32_t *p = (uint32_t *)pixels;
    if (color == 0) {
        memset(p, 0, npix * 4);
        return;
    }
    for (size_t i = 0; i < npix; i++) {
        p[i] = color;
    }
}

/* memfd-backed XRGB8888 (ARGB8888 for transparent windows) wl_buffer; the
 * pool and fd are not kept (the buffer keeps the memory alive, the
 * compositor has its own mapping) */
static struct wl_buffer *shm_create_wl_buffer(int w, int h, bool alpha, bool populate, void **map_out,
                                              size_t *size_out)
{
    const size_t stride = (size_t)w * 4;
    const size_t size = stride * (size_t)h;
    int fd = memfd_create("sdlop-shm", MFD_CLOEXEC);
    if (fd < 0) {
        SDL_SetError("memfd_create failed: %s", strerror(errno));
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        SDL_SetError("ftruncate failed: %s", strerror(errno));
        return NULL;
    }
    /* buffers of the cycle are written completely at least once: fault
     * the pages in with one call instead of one fault per page */
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | (populate ? MAP_POPULATE : 0), fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        SDL_SetError("mmap failed: %s", strerror(errno));
        return NULL;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_data.shm, fd, (int32_t)size);
    struct wl_buffer *buffer = pool ? wl_shm_pool_create_buffer(pool, 0, w, h, (int32_t)stride,
                                                                     alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888)
                                    : NULL;
    if (pool) {
        wl_shm_pool_destroy(pool);
    }
    close(fd);
    if (!buffer) {
        munmap(map, size);
        SDL_SetError("wl_shm buffer creation failed");
        return NULL;
    }
    *map_out = map;
    *size_out = size;
    return buffer;
}

static void shm_destroy_all(WaylandWindowData *wd)
{
    for (int i = 0; i < wd->num_shm; i++) {
        /* destroying a buffer the compositor still shows is allowed for
         * wl_shm (it keeps its own mapping); a late release is dropped */
        wl_buffer_destroy(wd->shm[i].buffer);
        munmap(wd->shm[i].map, wd->shm[i].size);
    }
    memset(wd->shm, 0, sizeof(wd->shm));
    wd->num_shm = 0;
    wd->front = NULL;
    wd->offload_buf = NULL;
}

/* the cycle's buffers follow the window size; other sizes are dropped */
static void shm_set_size(WaylandWindowData *wd, int w, int h)
{
    if (wd->buffer_w != w || wd->buffer_h != h) {
        shm_destroy_all(wd);
        wd->buffer_w = w;
        wd->buffer_h = h;
    }
}

static WaylandShmBuffer *shm_add_buffer(WaylandWindowData *wd)
{
    WaylandShmBuffer *b = &wd->shm[wd->num_shm];
    b->buffer = shm_create_wl_buffer(wd->buffer_w, wd->buffer_h, wd->transparent, true, &b->map, &b->size);
    if (!b->buffer) {
        return NULL;
    }
    wl_proxy_set_queue((struct wl_proxy *)b->buffer, wl_data.shm_queue);
    wl_buffer_add_listener(b->buffer, &shm_buffer_listener, b);
    b->busy = false;
    b->stale.x = 0;
    b->stale.y = 0;
    b->stale.w = wd->buffer_w;
    b->stale.h = wd->buffer_h;
    wd->num_shm++;
    return b;
}

/* read what the compositor has sent (waiting up to timeout_ms for more) and
 * dispatch the buffer-release queue only; other events stay queued for
 * the next SDL_PumpEvents */
static void shm_dispatch_releases(int timeout_ms)
{
    struct wl_display *display = wl_data.display;
    struct wl_event_queue *queue = wl_data.shm_queue;
    while (wl_display_prepare_read_queue(display, queue) != 0) {
        wl_display_dispatch_queue_pending(display, queue);
    }
    wl_display_flush(display);
    struct pollfd pfd = { wl_display_get_fd(display), POLLIN, 0 };
    if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN)) {
        wl_display_read_events(display);
    } else {
        wl_display_cancel_read(display);
    }
    wl_display_dispatch_queue_pending(display, queue);
}

static WaylandShmBuffer *shm_pick_free(WaylandWindowData *wd)
{
    /* the free buffer that missed the least: smallest copy */
    WaylandShmBuffer *best = NULL;
    Sint64 best_area = 0;
    for (int i = 0; i < wd->num_shm; i++) {
        WaylandShmBuffer *b = &wd->shm[i];
        const Sint64 area = (Sint64)b->stale.w * b->stale.h;
        if (!b->busy && (!best || area < best_area)) {
            best = b;
            best_area = area;
        }
    }
    return best;
}

/* a buffer the compositor does not hold */
static WaylandShmBuffer *shm_acquire(WaylandWindowData *wd)
{
    WaylandShmBuffer *b = shm_pick_free(wd);
    if (b) {
        return b;
    }
    shm_dispatch_releases(0);
    if ((b = shm_pick_free(wd)) != NULL) {
        return b;
    }
    if (wd->num_shm < SDLOP_WL_SHM_MAX_BUFFERS) {
        return shm_add_buffer(wd);
    }
    /* all held (compositor still reading them): wait for a release, but
     * do not hang forever on a compositor that keeps them */
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 50) {
        shm_dispatch_releases(50);
        if ((b = shm_pick_free(wd)) != NULL) {
            return b;
        }
        if (wl_display_get_error(wl_data.display)) {
            SDL_SetError("Wayland connection error while waiting for a window buffer");
            return NULL;
        }
    }
    SDL_SetError("Compositor did not release any of the %d window buffers", wd->num_shm);
    return NULL;
}

/* bring b up to date with the shadow */
static void shm_copy_from_shadow(WaylandWindowData *wd, WaylandShmBuffer *b)
{
    const SDL_Rect r = b->stale;
    b->stale.w = b->stale.h = 0;
    if (r.w <= 0 || r.h <= 0 || !wd->shadow) {
        return;
    }
    const size_t pitch = (size_t)wd->buffer_w * 4;
    const Uint8 *src = (const Uint8 *)wd->shadow + (size_t)r.y * pitch + (size_t)r.x * 4;
    Uint8 *dst = (Uint8 *)b->map + (size_t)r.y * pitch + (size_t)r.x * 4;
    if (r.x == 0 && r.w == wd->buffer_w) {
        memcpy(dst, src, pitch * (size_t)r.h);
        return;
    }
    for (int y = 0; y < r.h; y++) {
        memcpy(dst, src, (size_t)r.w * 4);
        src += pitch;
        dst += pitch;
    }
}

/* scale the window's buffers are rendered at (1 unless HIGH_PIXEL_DENSITY) */
static double window_buffer_scale(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    return (wd && (window->flags & SDL_WINDOW_HIGH_PIXEL_DENSITY)) ? wd->scale_factor : 1.0;
}

static int point_to_pixel(int points, double scale)
{
    const int p = (int)SDL_lround((double)points * scale);
    return p > 0 ? p : 1;
}

void SDLOP_Wayland_GetWindowPixelSize(SDL_Window *window, int *pw, int *ph)
{
    const double scale = window_buffer_scale(window);
    *pw = point_to_pixel(window->w > 0 ? window->w : 1, scale);
    *ph = point_to_pixel(window->h > 0 ? window->h : 1, scale);
}

/* viewport destination, requested only when it changes (w <= 0: unset) */
static void viewport_set_dest(WaylandWindowData *wd, int w, int h)
{
    if (!wd->viewport) {
        return;
    }
    if (w <= 0 || h <= 0) {
        w = h = -1;
    }
    if (wd->dest_w != w || wd->dest_h != h) {
        wp_viewport_set_destination(wd->viewport, w, h);
        wd->dest_w = w;
        wd->dest_h = h;
    }
}

/* Geometry for a buffer of bw x bh pixels showing the whole window. The app
 * can reach the wl_surface (SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER) and may
 * have left a buffer scale or transform pending or committed (e.g. for its
 * own rendering), so both are set with every attach of ours: a 1x1 buffer at
 * scale 2 is even a protocol error (size not a multiple of the scale). */
static void surface_set_buffer_geometry(SDL_Window *window, WaylandWindowData *wd, int bw, int bh)
{
    const int w = window->w > 0 ? window->w : 1;
    const int h = window->h > 0 ? window->h : 1;
    int32_t scale = 1;
    if (bw == w && bh == h) {
        viewport_set_dest(wd, -1, -1);
    } else if (wd->viewport) {
        viewport_set_dest(wd, w, h);
    } else {
        /* integer scales only without a viewport (scale_factor is rounded
         * up then); a stale buffer of another size stays unscaled */
        scale = (int32_t)SDL_lround(window_buffer_scale(window));
        if (scale < 1 || bw % scale != 0 || bh % scale != 0) {
            scale = 1;
        }
    }
    if (wl_data.compositor_version >= 3) {
        wl_surface_set_buffer_scale(wd->surface, scale);
    }
    if (wl_data.compositor_version >= 2) {
        wl_surface_set_buffer_transform(wd->surface, WL_OUTPUT_TRANSFORM_NORMAL);
    }
}

/* attach + damage + commit one buffer of the cycle */
static void shm_commit(WaylandWindowData *wd, WaylandShmBuffer *b, const SDL_Rect *rects, int numrects)
{
    /* damage is relative to the previous buffer; after the clear colour (or
     * nothing) everything changed */
    const bool full = !wd->front || !rects || numrects <= 0;
    SDL_Window *window = wd->window;
    surface_set_buffer_geometry(window, wd, wd->buffer_w, wd->buffer_h);
    wl_surface_attach(wd->surface, b->buffer, 0, 0);
    if (full) {
        wl_surface_damage_buffer(wd->surface, 0, 0, INT32_MAX, INT32_MAX);
    } else {
        for (int i = 0; i < numrects; i++) {
            wl_surface_damage_buffer(wd->surface, rects[i].x, rects[i].y, rects[i].w, rects[i].h);
        }
    }
    wl_surface_commit(wd->surface);
    b->busy = true;
    wd->front = b;
    wd->mapped = true;
    wl_display_flush(wl_data.display);
}

/* show the clear colour (no content presented at this size yet) */
static bool window_attach_clear(SDL_Window *window, WaylandWindowData *wd)
{
    const uint32_t color = window_clear_xrgb(window);
    const int w = window->w > 0 ? window->w : 1;
    const int h = window->h > 0 ? window->h : 1;
    if (wl_data.compositor_version >= 3) {
        wl_surface_set_buffer_scale(wd->surface, 1);
    }
    if (wl_data.compositor_version >= 2) {
        wl_surface_set_buffer_transform(wd->surface, WL_OUTPUT_TRANSFORM_NORMAL);
    }
    if (wd->viewport) {
        if (!wd->clear_buffer || wd->clear_buffer_color != color) {
            /* immutable once created: a new colour gets a new buffer */
            if (wd->clear_buffer) {
                wl_buffer_destroy(wd->clear_buffer);
                wd->clear_buffer = NULL;
            }
            void *map = NULL;
            size_t size = 0;
            wd->clear_buffer = shm_create_wl_buffer(1, 1, wd->transparent, false, &map, &size);
            if (!wd->clear_buffer) {
                return false;
            }
            *(uint32_t *)map = color;
            munmap(map, size);
            wd->clear_buffer_color = color;
        }
        wl_surface_attach(wd->surface, wd->clear_buffer, 0, 0);
        viewport_set_dest(wd, w, h);
    } else {
        int pw, ph;
        SDLOP_Wayland_GetWindowPixelSize(window, &pw, &ph);
        shm_set_size(wd, pw, ph);
        WaylandShmBuffer *b = shm_acquire(wd);
        if (!b) {
            return false;
        }
        fill_xrgb(b->map, (size_t)pw * (size_t)ph, color);
        b->stale.x = b->stale.y = 0;
        b->stale.w = pw;
        b->stale.h = ph;
        surface_set_buffer_geometry(window, wd, pw, ph);
        wl_surface_attach(wd->surface, b->buffer, 0, 0);
        b->busy = true;
    }
    wl_surface_damage_buffer(wd->surface, 0, 0, INT32_MAX, INT32_MAX);
    wd->front = NULL;
    return true;
}

/* window size changed: drop buffers of the old size, resize the shadow of
 * an existing window surface (SDL3: the surface survives resizes; its
 * contents restart at the clear colour) */
static bool window_resize_buffers(SDL_Window *window, int w, int h)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd) {
        return false;
    }
    shm_set_size(wd, w, h);
    if (window->surface && wd->shadow && (wd->shadow_w != w || wd->shadow_h != h)) {
        void *shadow = malloc((size_t)w * (size_t)h * 4);
        if (!shadow) {
            return SDL_OutOfMemory();
        }
        fill_xrgb(shadow, (size_t)w * (size_t)h, window_clear_xrgb(window));
        free(wd->shadow);
        wd->shadow = shadow;
        wd->shadow_w = w;
        wd->shadow_h = h;
        window->surface->w = w;
        window->surface->h = h;
        window->surface->pitch = w * 4;
        window->surface->pixels = shadow;
        SDL_SetSurfaceClipRect(window->surface, NULL); /* the old clip may exceed the new size */
    }
    SDLOP_Wayland_GL_WindowResized(window);
    if (wd->gl_active && !wd->gl_offload_active) {
        /* native EGL buffers (resized above) get the same geometry with
         * EGL's next commit */
        surface_set_buffer_geometry(window, wd, w, h);
    }
    return true;
}

/* new pixel size after a size or scale change: resize the buffers and
 * report it (stock: PIXEL_SIZE_CHANGED only when the pixel size changed) */
static void window_update_pixel_size(SDL_Window *window, bool force_resize)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    int pw, ph;
    SDLOP_Wayland_GetWindowPixelSize(window, &pw, &ph);
    const bool changed = pw != wd->pixel_w || ph != wd->pixel_h;
    if (changed || force_resize) {
        window_resize_buffers(window, pw, ph);
    }
    if (changed) {
        wd->pixel_w = pw;
        wd->pixel_h = ph;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, pw, ph);
    }
}

/* a new preferred scale for the window's surface */
static void window_commit(SDL_Window *window);
static void window_set_scale_factor(SDL_Window *window, double factor)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || factor <= 0.0) {
        return;
    }
    if (!wd->viewport) {
        factor = SDL_ceil(factor); /* only integer buffer scales then */
    }
    if (SDL_fabs(factor - wd->scale_factor) < 1e-6) {
        return;
    }
    wd->scale_factor = factor;
    window->display_scale = (float)factor;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED, 0, 0);
    if (window->flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) {
        const int old_w = wd->pixel_w, old_h = wd->pixel_h;
        window_update_pixel_size(window, false);
        if ((wd->pixel_w != old_w || wd->pixel_h != old_h) && !(window->flags & SDL_WINDOW_HIDDEN)) {
            window_commit(window); /* clear colour at the new size until the app presents */
        }
    }
}

static float wayland_output_scale(struct wl_output *output);

/* without fractional scale / preferred_buffer_scale: the largest scale of
 * the outputs the surface is on (kept while it is on none) */
static void window_update_output_scale(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || wd->fractional || wd->buffer_scale_received || wd->num_entered == 0) {
        return;
    }
    float scale = 0.0f;
    for (int i = 0; i < wd->num_entered; i++) {
        const float s = wayland_output_scale(wd->entered[i]);
        scale = s > scale ? s : scale;
    }
    window_set_scale_factor(window, scale);
}

static void window_destroy_buffers(WaylandWindowData *wd)
{
    shm_destroy_all(wd);
    if (wd->clear_buffer) {
        wl_buffer_destroy(wd->clear_buffer);
        wd->clear_buffer = NULL;
    }
    free(wd->shadow);
    wd->shadow = NULL;
    wd->shadow_w = wd->shadow_h = 0;
    wd->buffer_w = wd->buffer_h = 0;
}

/* (re)show the window's content: the last presented buffer, else the
 * software surface, else the clear colour */
static void window_commit(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || !wd->configured) {
        return;
    }
    if (wd->gl_active && !wd->gl_offload_active) {
        return; /* native Wayland EGL owns buffer management for this window */
    }
    int w, h;
    SDLOP_Wayland_GetWindowPixelSize(window, &w, &h);
    if (wd->front && wd->buffer_w == w && wd->buffer_h == h) {
        /* contents are not modified while attached: re-attaching is fine */
        WaylandShmBuffer *b = wd->front;
        wd->front = NULL; /* full damage */
        shm_commit(wd, b, NULL, 0);
        return;
    }
    if (wd->shadow && wd->shadow_w == w && wd->shadow_h == h) {
        shm_set_size(wd, w, h);
        WaylandShmBuffer *b = shm_acquire(wd);
        if (b) {
            shm_copy_from_shadow(wd, b);
            shm_commit(wd, b, NULL, 0);
            return;
        }
    }
    if (window_attach_clear(window, wd)) {
        wl_surface_commit(wd->surface);
        wd->mapped = true;
    }
}

/* ------------------------------------------------------------------ */
/* software window surface                                             */
/* ------------------------------------------------------------------ */

static bool wayland_CreateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface **surface)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    int w, h;
    SDLOP_Wayland_GetWindowPixelSize(window, &w, &h);
    shm_set_size(wd, w, h);
    if (!wd->shadow || wd->shadow_w != w || wd->shadow_h != h) {
        void *shadow = malloc((size_t)w * (size_t)h * 4);
        if (!shadow) {
            return SDL_OutOfMemory();
        }
        fill_xrgb(shadow, (size_t)w * (size_t)h, window_clear_xrgb(window));
        free(wd->shadow);
        wd->shadow = shadow;
        wd->shadow_w = w;
        wd->shadow_h = h;
    }
    /* buffers may hold GL offload frames or an older surface: resync */
    for (int i = 0; i < wd->num_shm; i++) {
        wd->shm[i].stale.x = wd->shm[i].stale.y = 0;
        wd->shm[i].stale.w = w;
        wd->shm[i].stale.h = h;
    }
    /* transparent windows: ARGB8888 (wl_shm's is premultiplied, as stock
     * hands it to the app) */
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, wd->transparent ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_XRGB8888,
                                           wd->shadow, w * 4);
    if (!s) {
        return false;
    }
    *surface = s;
    return true;
}

static bool wayland_UpdateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || !wd->configured || wd->gl_active) {
        return SDL_SetError("Window surface is not presentable");
    }
    if (!wd->shadow || wd->shadow_w != wd->buffer_w || wd->shadow_h != wd->buffer_h) {
        return SDL_SetError("Window surface is not valid (window resized?)");
    }
    const SDL_Rect all = { 0, 0, wd->buffer_w, wd->buffer_h };
    SDL_Rect damage = { 0, 0, 0, 0 };
    if (rects && numrects > 0) {
        for (int i = 0; i < numrects; i++) {
            SDL_Rect clipped;
            if (SDL_GetRectIntersection(&rects[i], &all, &clipped)) {
                rect_add(&damage, &clipped);
            }
        }
        if (damage.w <= 0) {
            return true; /* nothing on screen changes */
        }
    } else {
        damage = all;
    }
    for (int i = 0; i < wd->num_shm; i++) {
        rect_add(&wd->shm[i].stale, &damage);
    }
    WaylandShmBuffer *b = shm_acquire(wd);
    if (!b) {
        return false;
    }
    shm_copy_from_shadow(wd, b);
    shm_commit(wd, b, rects, numrects);
    return true;
}

static void wayland_DestroyWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    /* the SDL_Surface wraps the shadow: free both; the last presented
     * buffer stays on screen */
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
    if (wd) {
        free(wd->shadow);
        wd->shadow = NULL;
        wd->shadow_w = wd->shadow_h = 0;
    }
}

/* ------------------------------------------------------------------ */
/* internals shared with the GL/Vulkan modules                         */
/* ------------------------------------------------------------------ */

uint32_t SDLOP_Wayland_GetSelectionSerial(void)
{
    /* set_selection wants the serial of a recent input event; compositors
     * accept the keyboard focus serial too */
    return wl_data.input_serial ? wl_data.input_serial : wl_data.keyboard_serial;
}

struct wl_display *SDLOP_Wayland_GetDisplay(void)
{
    return wl_data.display;
}

struct wl_surface *SDLOP_Wayland_GetWindowSurfaceHandle(SDL_Window *window)
{
    WaylandWindowData *wd = window ? (WaylandWindowData *)window->driverdata : NULL;
    return wd ? wd->surface : NULL;
}

void SDLOP_Wayland_SetGLActive(SDL_Window *window, bool active)
{
    WaylandWindowData *wd = window ? (WaylandWindowData *)window->driverdata : NULL;
    if (wd) {
        wd->gl_active = active;
        if (active) {
            /* native EGL buffers (pixel size) replace the clear colour's
             * geometry with EGL's next commit */
            int pw, ph;
            SDLOP_Wayland_GetWindowPixelSize(window, &pw, &ph);
            surface_set_buffer_geometry(window, wd, pw, ph);
        }
    }
}

bool SDLOP_Wayland_IsGLActive(SDL_Window *window)
{
    WaylandWindowData *wd = window ? (WaylandWindowData *)window->driverdata : NULL;
    return wd ? wd->gl_active : false;
}

void *SDLOP_Wayland_GetGLSurfaceSlot(SDL_Window *window)
{
    WaylandWindowData *wd = window ? (WaylandWindowData *)window->driverdata : NULL;
    return wd ? &wd->gl_surface : NULL;
}

void *SDLOP_Wayland_GetGLEGLWindowSlot(SDL_Window *window)
{
    WaylandWindowData *wd = window ? (WaylandWindowData *)window->driverdata : NULL;
    return wd ? &wd->gl_egl_window : NULL;
}

void SDLOP_Wayland_SetGLOffloadActive(SDL_Window *window, bool active)
{
    WaylandWindowData *wd = window ? (WaylandWindowData *)window->driverdata : NULL;
    if (wd) {
        wd->gl_offload_active = active;
    }
}

bool SDLOP_Wayland_IsGLOffloadActive(SDL_Window *window)
{
    WaylandWindowData *wd = window ? (WaylandWindowData *)window->driverdata : NULL;
    return wd ? wd->gl_offload_active : false;
}

void *SDLOP_Wayland_GetGLOffloadContextSlot(SDL_Window *window)
{
    WaylandWindowData *wd = window ? (WaylandWindowData *)window->driverdata : NULL;
    return wd ? &wd->gl_offload_ctx : NULL;
}

bool SDLOP_Wayland_CompositorHasHardwareBuffers(void)
{
    return wl_data.has_dmabuf || wl_data.has_wl_drm;
}

/* GL offload without dmabuf: frames are read back into a buffer of the same
 * cycle as software presentation (so the offload also never writes a
 * buffer the compositor holds). Without pixels_out this only sizes the
 * cycle; with it, a free buffer is reserved for PresentOffloadShm. */
bool SDLOP_Wayland_AcquireOffloadShm(SDL_Window *window, void **pixels_out, int *pitch_out, int *w_out, int *h_out)
{
    WaylandWindowData *wd = window ? (WaylandWindowData *)window->driverdata : NULL;
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    int w, h;
    SDLOP_Wayland_GetWindowPixelSize(window, &w, &h);
    shm_set_size(wd, w, h);
    if (pixels_out) {
        WaylandShmBuffer *b = wd->offload_buf;
        if (!b) {
            b = shm_acquire(wd);
            if (!b) {
                return false;
            }
        }
        /* overwritten by the GL frame: out of sync with any shadow */
        b->stale.x = b->stale.y = 0;
        b->stale.w = w;
        b->stale.h = h;
        wd->offload_buf = b;
        *pixels_out = b->map;
    }
    if (pitch_out) {
        *pitch_out = wd->buffer_w * 4;
    }
    if (w_out) {
        *w_out = wd->buffer_w;
    }
    if (h_out) {
        *h_out = wd->buffer_h;
    }
    return true;
}

bool SDLOP_Wayland_PresentOffloadShm(SDL_Window *window)
{
    WaylandWindowData *wd = window ? (WaylandWindowData *)window->driverdata : NULL;
    if (!wd || !wd->offload_buf) {
        return SDL_SetError("Window has no acquired shm buffer");
    }
    WaylandShmBuffer *b = wd->offload_buf;
    wd->offload_buf = NULL;
    if (!wd->configured) {
        /* xsurface_configure commits wd->front once the handshake arrives */
        wd->front = b;
        return true;
    }
    shm_commit(wd, b, NULL, 0);
    return true;
}

/* ------------------------------------------------------------------ */
/* xdg-shell listeners                                                 */
/* ------------------------------------------------------------------ */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    wm_base_ping,
};

static void xsurface_configure(void *data, struct xdg_surface *xsurface, uint32_t serial)
{
    SDL_Window *window = (SDL_Window *)data;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    xdg_surface_ack_configure(xsurface, serial);
    wd->configured = true;

    if (wd->pending_w > 0 && wd->pending_h > 0 &&
        (wd->pending_w != window->w || wd->pending_h != window->h)) {
        window->w = wd->pending_w;
        window->h = wd->pending_h;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
        window_update_pixel_size(window, true);
        /* Stock 3.2.10 sends SAFE_AREA_CHANGED with every size change on
         * Wayland (measured: 4 SAFE for 4 resizes) and exactly one
         * MOVED(0,0) per window once a size is applied. The previous guard
         * additionally required this to be the FIRST configure, but
         * sway/wlroots send toplevel configure(0,0) first, so the block
         * only ever ran when first was already false - never fired. */
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_SAFE_AREA_CHANGED, 0, 0);
        if (!wd->moved_reported) {
            wd->moved_reported = true;
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOVED, 0, 0);
        }
    }
    if (!(window->flags & SDL_WINDOW_HIDDEN)) {
        window_commit(window);
    }
}

static const struct xdg_surface_listener xsurface_listener = {
    xsurface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states)
{
    SDL_Window *window = (SDL_Window *)data;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    (void)toplevel;

    wd->pending_w = width;
    wd->pending_h = height;

    /* update maximized/fullscreen flags */
    bool maximized = false, fullscreen = false, suspended = false;
    const uint32_t *s;
    wl_array_for_each(s, states) {
        if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED) {
            maximized = true;
        } else if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN) {
            fullscreen = true;
        } else if (*s == XDG_TOPLEVEL_STATE_SUSPENDED) {
            suspended = true;
        }
    }
    if (maximized && !(window->flags & SDL_WINDOW_MAXIMIZED)) {
        window->flags |= SDL_WINDOW_MAXIMIZED;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MAXIMIZED, 0, 0);
    } else if (!maximized && (window->flags & SDL_WINDOW_MAXIMIZED)) {
        window->flags &= ~SDL_WINDOW_MAXIMIZED;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESTORED, 0, 0);
    }
    if (fullscreen && !(window->flags & SDL_WINDOW_FULLSCREEN)) {
        window->flags |= SDL_WINDOW_FULLSCREEN;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_ENTER_FULLSCREEN, 0, 0);
    } else if (!fullscreen && (window->flags & SDL_WINDOW_FULLSCREEN)) {
        window->flags &= ~SDL_WINDOW_FULLSCREEN;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 0, 0);
    }
    (void)suspended;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    SDL_Window *window = (SDL_Window *)data;
    (void)toplevel;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
}

static void toplevel_bounds(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
}

static void toplevel_caps(void *data, struct xdg_toplevel *toplevel, struct wl_array *capabilities)
{
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    toplevel_configure,
    toplevel_close,
    toplevel_bounds,
    toplevel_caps,
};

/* ------------------------------------------------------------------ */
/* wl_keyboard                                                         */
/* ------------------------------------------------------------------ */

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data;
    (void)keyboard;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        return;
    }
    SDLOP_KeyboardSetKeymapString(map, size);
    munmap(map, size);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
    (void)keyboard;
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    SDL_Window *window = NULL;
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        WaylandWindowData *wd = (WaylandWindowData *)w->driverdata;
        if (wd && wd->surface == surface) {
            window = w;
            break;
        }
    }
    d->keyboard_serial = serial;
    if (window) {
        if (sdlop.keyboard_focus && sdlop.keyboard_focus != window) {
            sdlop.keyboard_focus->flags &= ~SDL_WINDOW_INPUT_FOCUS;
            SDLOP_SendWindowEvent(sdlop.keyboard_focus, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
        }
        sdlop.keyboard_focus = window;
        window->flags |= SDL_WINDOW_INPUT_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_FOCUS_GAINED, 0, 0);
        /* Restore only held modifiers, never action keys/text/repeats, and
         * never mix seat keyboard events into a running raw source. */
        if (!SDLOP_RawInputIsSource()) {
            uint32_t *key;
            wl_array_for_each(key, keys) {
                if (*key > SDLOP_EVDEV_KEY_MAX) { continue; }
                SDL_Scancode sc = (SDL_Scancode)sdlop_evdev_to_scancode[*key];
                switch (sc) {
                case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT:
                case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL:
                case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT:
                case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI:
                case SDL_SCANCODE_MODE:
                    if (!sdlop.keystate[sc]) {
                        SDLOP_SendKeyboardKey(true, false, sc, (Uint16)*key, 0);
                    }
                    break;
                default: break;
                }
            }
            sdlop.repeat_active = false;
        }
    }
    SDLOP_Wayland_TextInput_Update();
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    if (sdlop.keyboard_focus) {
        sdlop.keyboard_focus->flags &= ~SDL_WINDOW_INPUT_FOCUS;
        SDLOP_SendWindowEvent(sdlop.keyboard_focus, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
        sdlop.keyboard_focus = NULL;
        SDL_ResetKeyboard();
    }
    SDLOP_Wayland_TextInput_Update();
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)data;
    (void)keyboard;
    (void)time;
    wl_data.input_serial = serial;

    /* SDLOP_RAW_INPUT: evdev delivers every key; never mix the two. The
     * modifiers/keymap/repeat events below still keep the layout state. */
    if (SDLOP_RawInputIsSource()) {
        return;
    }

    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    if (key <= SDLOP_EVDEV_KEY_MAX) {
        sc = (SDL_Scancode)sdlop_evdev_to_scancode[key];
    }
    if (sc != SDL_SCANCODE_UNKNOWN) {
        SDLOP_SendKeyboardKey(state == WL_KEYBOARD_KEY_STATE_PRESSED, false, sc, (Uint16)key, SDLOP_MonotonicNS());
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    /* wl_keyboard.modifiers carries the effective (locked) layout group */
    SDLOP_KeyboardUpdateXkbState(mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
    (void)data;
    (void)keyboard;
    /* rate 0: the compositor turned key repeat off */
    sdlop.repeat_disabled = rate <= 0;
    if (rate > 0 && delay > 0) {
        SDLOP_RawInputSetRepeatInfo((Uint32)delay, (Uint32)SDL_max(1000 / rate, 1));
    }
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_keymap,
    keyboard_enter,
    keyboard_leave,
    keyboard_key,
    keyboard_modifiers,
    keyboard_repeat_info,
};

/* ------------------------------------------------------------------ */
/* wl_pointer                                                          */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* built-in cursor (lean: no cursor-theme dependency)                  */
/* ------------------------------------------------------------------ */

#define CURSOR_W 16
#define CURSOR_H 16

/* classic arrow, 'X' = black outline, 'x' = white fill, ' ' = transparent */
static const char *arrow_rows[CURSOR_H] = {
    "X               ",
    "XX              ",
    "XxX             ",
    "XxxX            ",
    "XxxxX           ",
    "XxxxxX          ",
    "XxxxxxxX        ",
    "XxxxxxxxxX      ",
    "XxxxxxxxxxX     ",
    "XxxxxxxXxxxX    ",
    "XxxXxxxX XxX    ",
    "XX  XxX   XxX   ",
    "X    XX    XxX  ",
    "            XxX ",
    "             XX ",
    "                ",
};

static struct wl_surface *cursor_surface;

typedef struct WaylandCursor
{
    struct wl_buffer *buffer;
    int w, h, hot_x, hot_y;
    uint32_t shape; /* wp_cursor_shape_device_v1 shape of a system cursor, 0 = none */
} WaylandCursor;

/* stock Wayland_GetSystemCursorShape */
static uint32_t system_cursor_shape(SDL_SystemCursor id)
{
    switch (id) {
    case SDL_SYSTEM_CURSOR_DEFAULT:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    case SDL_SYSTEM_CURSOR_TEXT:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
    case SDL_SYSTEM_CURSOR_WAIT:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
    case SDL_SYSTEM_CURSOR_CROSSHAIR:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
    case SDL_SYSTEM_CURSOR_PROGRESS:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS;
    case SDL_SYSTEM_CURSOR_NWSE_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
    case SDL_SYSTEM_CURSOR_NESW_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
    case SDL_SYSTEM_CURSOR_EW_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
    case SDL_SYSTEM_CURSOR_NS_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
    case SDL_SYSTEM_CURSOR_MOVE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL;
    case SDL_SYSTEM_CURSOR_NOT_ALLOWED:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
    case SDL_SYSTEM_CURSOR_POINTER:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    case SDL_SYSTEM_CURSOR_NW_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
    case SDL_SYSTEM_CURSOR_N_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
    case SDL_SYSTEM_CURSOR_NE_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
    case SDL_SYSTEM_CURSOR_E_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
    case SDL_SYSTEM_CURSOR_SE_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
    case SDL_SYSTEM_CURSOR_S_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
    case SDL_SYSTEM_CURSOR_SW_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
    case SDL_SYSTEM_CURSOR_W_RESIZE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
    default:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }
}

/* premultiplied ARGB8888 wl_shm buffer of bw x bh holding the w x h
 * straight-alpha image at (ox, oy), transparent elsewhere (the pool is
 * released right away; the buffer keeps the memory alive) */
static struct wl_buffer *create_shm_argb_buffer(const uint32_t *argb, int w, int h, int pitch_px, int bw, int bh,
                                                int ox, int oy)
{
    if (!wl_data.shm) {
        return NULL;
    }
    const size_t size = (size_t)bw * (size_t)bh * 4;
    int fd = memfd_create("sdlop-argb", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    uint32_t *pix = (uint32_t *)map; /* memfd pages start zeroed */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint32_t p = argb[(size_t)y * (size_t)pitch_px + (size_t)x];
            const uint32_t a = p >> 24;
            pix[(size_t)(y + oy) * (size_t)bw + (size_t)(x + ox)] =
                (a << 24) | ((((p >> 16) & 0xff) * a / 255) << 16) | ((((p >> 8) & 0xff) * a / 255) << 8) |
                ((p & 0xff) * a / 255);
        }
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_data.shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, bw, bh, bw * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(map, size);
    close(fd);
    return buffer;
}

static struct wl_buffer *create_shm_cursor_buffer(const uint32_t *argb, int w, int h, int pitch_px)
{
    return create_shm_argb_buffer(argb, w, h, pitch_px, w, h, 0, 0);
}

static bool builtin_arrow_pixels(uint32_t *pix)
{
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            char c = arrow_rows[y][x];
            pix[y * CURSOR_W + x] = c == 'X' ? 0xFF000000u : c == 'x' ? 0xFFFFFFFFu : 0u;
        }
    }
    return true;
}

static struct sdlop_xcursor_theme *wayland_cursor_theme(void)
{
    if (!wl_data.cursor_theme_tried) {
        wl_data.cursor_theme_tried = true;
        int size = 24;
        const char *ss = getenv("XCURSOR_SIZE");
        if (ss) {
            long v = strtol(ss, NULL, 10);
            if (v >= 8 && v <= 256) {
                size = (int)v;
            }
        }
        wl_data.cursor_theme = sdlop_xcursor_theme_load(getenv("XCURSOR_THEME"), size);
    }
    return wl_data.cursor_theme;
}

/* surface: ARGB8888 custom cursor, else the system cursor system_id */
static WaylandCursor *make_cursor(const SDL_Surface *surface, int hot_x, int hot_y, SDL_SystemCursor system_id)
{
    WaylandCursor *wc = (WaylandCursor *)calloc(1, sizeof(*wc));
    if (!wc) {
        SDL_OutOfMemory();
        return NULL;
    }
    if (surface) {
        const SDL_Surface *s = surface;
        wc->buffer = create_shm_cursor_buffer((const uint32_t *)s->pixels, s->w, s->h, s->pitch / 4);
        wc->w = s->w;
        wc->h = s->h;
        wc->hot_x = hot_x;
        wc->hot_y = hot_y;
    } else if (wl_data.cursor_shape_manager) {
        /* stock: the compositor draws system cursors (theme, scale) */
        wc->shape = system_cursor_shape(system_id);
    } else {
        /* themed (CSS name, fallback, legacy X name - like SDL3's
         * wayland-cursor path); the built-in arrow when nothing matches */
        struct sdlop_xcursor_theme *theme = wayland_cursor_theme();
        if (theme) {
            const char *fallback = NULL;
            const char *names[3] = { SDLOP_GetCSSCursorName(system_id, &fallback), NULL, NULL };
            names[1] = fallback;
            names[2] = system_id == SDL_SYSTEM_CURSOR_DEFAULT ? "left_ptr" : NULL;
            for (int i = 0; i < 3 && !wc->buffer; ++i) {
                struct sdlop_xcursor *xc = names[i] ? sdlop_xcursor_theme_get_cursor(theme, names[i]) : NULL;
                if (xc && xc->image_count > 0) {
                    const struct sdlop_xcursor_image *img = xc->images[0];
                    wc->buffer = create_shm_cursor_buffer((const uint32_t *)img->buffer, (int)img->width,
                                                          (int)img->height, (int)img->width);
                    wc->w = (int)img->width;
                    wc->h = (int)img->height;
                    wc->hot_x = (int)img->hotspot_x;
                    wc->hot_y = (int)img->hotspot_y;
                }
            }
        }
        if (!wc->buffer) {
            uint32_t pix[CURSOR_W * CURSOR_H];
            builtin_arrow_pixels(pix);
            wc->buffer = create_shm_cursor_buffer(pix, CURSOR_W, CURSOR_H, CURSOR_W);
            wc->w = CURSOR_W;
            wc->h = CURSOR_H;
            wc->hot_x = wc->hot_y = 0;
        }
    }
    if (!wc->buffer && !wc->shape) {
        free(wc);
        SDL_SetError("Couldn't create Wayland cursor buffer");
        return NULL;
    }
    return wc;
}

static void free_cursor_data(WaylandCursor *wc)
{
    if (wc) {
        if (wc->buffer) {
            wl_buffer_destroy(wc->buffer);
        }
        free(wc);
    }
}

static bool wayland_CreateCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    WaylandCursor *wc = make_cursor(cursor->surface, cursor->hot_x, cursor->hot_y, cursor->system_id);
    if (!wc) {
        return false;
    }
    cursor->internal = wc;
    return true;
}

/* resize cursors shown over SDL_HITTEST_RESIZE_* areas (stock) */
static WaylandCursor *hit_test_cursors[SDL_HITTEST_RESIZE_LEFT + 1];

static WaylandCursor *hit_test_cursor(int result)
{
    static const SDL_SystemCursor ids[] = {
        SDL_SYSTEM_CURSOR_NW_RESIZE, SDL_SYSTEM_CURSOR_N_RESIZE, SDL_SYSTEM_CURSOR_NE_RESIZE,
        SDL_SYSTEM_CURSOR_E_RESIZE,  SDL_SYSTEM_CURSOR_SE_RESIZE, SDL_SYSTEM_CURSOR_S_RESIZE,
        SDL_SYSTEM_CURSOR_SW_RESIZE, SDL_SYSTEM_CURSOR_W_RESIZE
    };
    if (result < SDL_HITTEST_RESIZE_TOPLEFT || result > SDL_HITTEST_RESIZE_LEFT) {
        return NULL;
    }
    if (!hit_test_cursors[result]) {
        hit_test_cursors[result] = make_cursor(NULL, 0, 0, ids[result - SDL_HITTEST_RESIZE_TOPLEFT]);
    }
    return hit_test_cursors[result];
}

static void wayland_FreeCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    WaylandCursor *wc = (WaylandCursor *)cursor->internal;
    if (wc) {
        if (wc->buffer && wl_data.display) {
            wl_buffer_destroy(wc->buffer);
        }
        free(wc);
        cursor->internal = NULL;
    }
}

/* show SDL's current cursor (NULL = hidden) on the focused surface */
static void apply_cursor(uint32_t serial)
{
    if (!wl_data.pointer || !wl_data.pointer_focus) {
        return;
    }
    SDL_Cursor *cursor = SDLOP_GetCurrentCursor();
    WaylandCursor *wc = cursor ? (WaylandCursor *)cursor->internal : NULL;
    const WaylandWindowData *fd = (const WaylandWindowData *)wl_data.pointer_focus->driverdata;
    if (wc && fd && wl_data.pointer_focus->hit_test) {
        WaylandCursor *edge = hit_test_cursor(fd->hit_test_result);
        if (edge) {
            wc = edge;
        }
    }
    if (!wc || (!wc->buffer && !wc->shape) ||
        (wl_data.locked_pointer && wl_data.relative_lock_window == wl_data.pointer_focus)) {
        wl_pointer_set_cursor(wl_data.pointer, serial, NULL, 0, 0);
        return;
    }
    if (wc->shape) {
        if (!wl_data.cursor_shape_device && wl_data.cursor_shape_manager) {
            wl_data.cursor_shape_device =
                wp_cursor_shape_manager_v1_get_pointer(wl_data.cursor_shape_manager, wl_data.pointer);
        }
        if (wl_data.cursor_shape_device) {
            wp_cursor_shape_device_v1_set_shape(wl_data.cursor_shape_device, serial, wc->shape);
        }
        return;
    }
    if (!cursor_surface && wl_data.compositor) {
        cursor_surface = wl_compositor_create_surface(wl_data.compositor);
    }
    if (!cursor_surface) {
        return;
    }
    wl_surface_attach(cursor_surface, wc->buffer, 0, 0);
    wl_surface_damage_buffer(cursor_surface, 0, 0, wc->w, wc->h);
    wl_surface_commit(cursor_surface);
    wl_pointer_set_cursor(wl_data.pointer, serial, cursor_surface, wc->hot_x, wc->hot_y);
}

static bool wayland_ShowCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    (void)cursor; /* == SDLOP_GetCurrentCursor() */
    apply_cursor(wl_data.pointer_serial);
    if (wl_data.display) {
        wl_display_flush(wl_data.display);
    }
    return true;
}

static void wayland_cursor_quit(void)
{
    if (cursor_surface) {
        wl_surface_destroy(cursor_surface);
        cursor_surface = NULL;
    }
    if (wl_data.cursor_theme) {
        sdlop_xcursor_theme_destroy(wl_data.cursor_theme);
        wl_data.cursor_theme = NULL;
    }
    wl_data.cursor_theme_tried = false;
}

static SDL_Window *window_from_surface(struct wl_surface *surface)
{
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        WaylandWindowData *wd = (WaylandWindowData *)w->driverdata;
        if (wd && wd->surface == surface) {
            return w;
        }
    }
    return NULL;
}

SDL_Window *SDLOP_Wayland_WindowFromSurface(struct wl_surface *surface)
{
    return window_from_surface(surface);
}

struct wp_cursor_shape_manager_v1 *SDLOP_Wayland_GetCursorShapeManager(void)
{
    return wl_data.cursor_shape_manager;
}

/* SDL_SetWindowHitTest (stock): evaluated on pointer motion while no
 * button is held; resize areas show resize cursors */
static void update_hit_test(SDL_Window *window, float x, float y)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || !window->hit_test || SDL_GetMouseState(NULL, NULL) != 0) {
        return;
    }
    const SDL_Point p = { (int)SDL_floorf(x), (int)SDL_floorf(y) };
    const int rc = (int)window->hit_test(window, &p, window->hit_test_data);
    if (rc != wd->hit_test_result) {
        wd->hit_test_result = rc;
        apply_cursor(wl_data.pointer_serial);
    }
}

/* a left press on a draggable / resize area starts the compositor's
 * interactive move / resize instead of reaching the app (stock) */
static bool process_hit_test(SDL_Window *window, uint32_t serial)
{
    static const uint32_t edges[] = {
        XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,     XDG_TOPLEVEL_RESIZE_EDGE_TOP,    XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
        XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,        XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,
        XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT,  XDG_TOPLEVEL_RESIZE_EDGE_LEFT
    };
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || !window->hit_test || !wd->toplevel || !wl_data.seat || wl_data.locked_pointer) {
        return false;
    }
    if (wd->hit_test_result == SDL_HITTEST_DRAGGABLE) {
        xdg_toplevel_move(wd->toplevel, wl_data.seat, serial);
    } else if (wd->hit_test_result >= SDL_HITTEST_RESIZE_TOPLEFT && wd->hit_test_result <= SDL_HITTEST_RESIZE_LEFT) {
        xdg_toplevel_resize(wd->toplevel, wl_data.seat, serial, edges[wd->hit_test_result - SDL_HITTEST_RESIZE_TOPLEFT]);
    } else {
        return false;
    }
    wl_display_flush(wl_data.display);
    return true;
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    SDL_Window *window = window_from_surface(surface);
    if (!window) {
        return;
    }
    d->pointer_serial = serial;
    d->axis_x_acc = d->axis_y_acc = 0.0f; /* drop a half-accumulated frame */
    d->pointer_focus = window;
    sdlop.mouse_focus = window;
    window->flags |= SDL_WINDOW_MOUSE_FOCUS;
    sdlop.mouse_has_position = false; /* stock: the enter's own motion carries no delta */
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_ENTER, 0, 0);
    if (SDLOP_RawInputIsSource()) {
        /* evdev sends motion; the enter position only anchors it */
        sdlop.mouse_x = (float)wl_fixed_to_double(surface_x);
        sdlop.mouse_y = (float)wl_fixed_to_double(surface_y);
        sdlop.mouse_has_position = true;
    } else {
        /* stock delivers the enter position through the motion path (a
           movement event with zero delta); a confine warp may arrive with no
           following motion, so the enter coordinates are the position. */
        SDLOP_SendMouseMotion((float)wl_fixed_to_double(surface_x),
                              (float)wl_fixed_to_double(surface_y), 0.0f, 0.0f,
                              SDLOP_MonotonicNS());
    }

    if (sdlop.relative_mode_window == window) {
        if (!wl_data.locked_pointer) {
            wayland_arm_pointer_lock(window); /* re-arm after policy unlock */
        } else {
            wl_pointer_set_cursor(pointer, serial, NULL, 0, 0); /* hide */
        }
    } else {
        WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
        if (wd) {
            wd->hit_test_result = SDL_HITTEST_NORMAL;
        }
        update_hit_test(window, (float)wl_fixed_to_double(surface_x), (float)wl_fixed_to_double(surface_y));
        apply_cursor(serial);
    }
}

/* ---- touchpad pinch: SDL_EVENT_PINCH_* (stock handle_pinch_*) ---- */

static void pinch_begin(void *data, struct zwp_pointer_gesture_pinch_v1 *pinch, uint32_t serial, uint32_t time,
                        struct wl_surface *surface, uint32_t fingers)
{
    (void)data;
    (void)pinch;
    (void)serial;
    (void)time;
    (void)fingers;
    SDL_Window *window = surface ? window_from_surface(surface) : NULL;
    if (window) {
        wl_data.pinch_focus = window;
        SDLOP_SendPinch(SDL_EVENT_PINCH_BEGIN, 0, window, 0.0f);
    }
}

static void pinch_update(void *data, struct zwp_pointer_gesture_pinch_v1 *pinch, uint32_t time, wl_fixed_t dx,
                         wl_fixed_t dy, wl_fixed_t scale, wl_fixed_t rotation)
{
    (void)data;
    (void)pinch;
    (void)time;
    (void)dx;
    (void)dy;
    (void)rotation;
    if (wl_data.pinch_focus) {
        SDLOP_SendPinch(SDL_EVENT_PINCH_UPDATE, 0, wl_data.pinch_focus, (float)wl_fixed_to_double(scale));
    }
}

static void pinch_end(void *data, struct zwp_pointer_gesture_pinch_v1 *pinch, uint32_t serial, uint32_t time,
                      int32_t cancelled)
{
    (void)data;
    (void)pinch;
    (void)serial;
    (void)time;
    (void)cancelled;
    if (wl_data.pinch_focus) {
        SDLOP_SendPinch(SDL_EVENT_PINCH_END, 0, wl_data.pinch_focus, 0.0f);
        wl_data.pinch_focus = NULL;
    }
}

static const struct zwp_pointer_gesture_pinch_v1_listener pinch_listener = {
    pinch_begin,
    pinch_update,
    pinch_end,
};

/* once both the gestures global and the pointer exist (either can come first) */
static void ensure_pinch_gesture(void)
{
    if (wl_data.pointer_gestures && wl_data.pointer && !wl_data.gesture_pinch) {
        wl_data.gesture_pinch = zwp_pointer_gestures_v1_get_pinch_gesture(wl_data.pointer_gestures, wl_data.pointer);
        zwp_pointer_gesture_pinch_v1_add_listener(wl_data.gesture_pinch, &pinch_listener, NULL);
    }
}

static void destroy_pinch_gesture(void)
{
    if (wl_data.pinch_focus) {
        SDLOP_SendPinch(SDL_EVENT_PINCH_END, 0, wl_data.pinch_focus, 0.0f); /* stock: end active gestures */
        wl_data.pinch_focus = NULL;
    }
    if (wl_data.gesture_pinch) {
        if (zwp_pointer_gesture_pinch_v1_get_version(wl_data.gesture_pinch) >=
            ZWP_POINTER_GESTURE_PINCH_V1_DESTROY_SINCE_VERSION) {
            zwp_pointer_gesture_pinch_v1_destroy(wl_data.gesture_pinch);
        } else {
            wl_proxy_destroy((struct wl_proxy *)wl_data.gesture_pinch);
        }
        wl_data.gesture_pinch = NULL;
    }
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)pointer;
    (void)serial;
    (void)surface;
    SDL_Window *window = d->pointer_focus;
    d->pointer_focus = NULL;
    SDLOP_RawInputResetPointerFrame();
    d->axis_x_acc = d->axis_y_acc = 0.0f; /* drop a half-accumulated frame */
    sdlop.mouse_has_position = false;
    if (sdlop.mouse_focus == window) {
        sdlop.mouse_focus = NULL;
    }
    if (window) {
        window->flags &= ~SDL_WINDOW_MOUSE_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_LEAVE, 0, 0);
    }
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)pointer;
    (void)time;
    SDL_Window *window = d->pointer_focus;
    if (!window) {
        return;
    }
    float x = (float)wl_fixed_to_double(surface_x);
    float y = (float)wl_fixed_to_double(surface_y);
    update_hit_test(window, x, y);

    if (SDLOP_RawInputIsSource()) {
        if (sdlop.relative_mode_window == window) {
            return; /* relative mode: evdev owns motion entirely */
        }
        /* re-anchor accumulated position to compositor truth (no event:
         * evdev already emitted the low-latency motion event) */
        sdlop.mouse_x = x;
        sdlop.mouse_y = y;
        sdlop.mouse_has_position = true;
        return;
    }
    SDLOP_SendMouseMotion(x, y, 0.0f, 0.0f, SDLOP_MonotonicNS()); /* core derives deltas from clamped positions (stock) */
}

static Uint8 wl_button_to_sdl(uint32_t button)
{
    switch (button) {
    case BTN_LEFT:   return SDL_BUTTON_LEFT;
    case BTN_RIGHT:  return SDL_BUTTON_RIGHT;
    case BTN_MIDDLE: return SDL_BUTTON_MIDDLE;
    case BTN_SIDE:   return SDL_BUTTON_X1;
    case BTN_EXTRA:  return SDL_BUTTON_X2;
    case BTN_BACK:   return SDL_BUTTON_X1;
    case BTN_FORWARD:return SDL_BUTTON_X2;
    default:         return 0;
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    (void)data;
    (void)pointer;
    (void)time;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        wl_data.input_serial = serial; /* implicit grab serial (popups, menus, move) */
    }
    Uint8 btn = wl_button_to_sdl(button);
    if (btn == SDL_BUTTON_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED && wl_data.pointer_focus &&
        process_hit_test(wl_data.pointer_focus, serial)) {
        return; /* not passed on to the app (stock) */
    }
    if (SDLOP_RawInputIsSource()) {
        return; /* evdev owns buttons */
    }
    if (btn) {
        SDLOP_SendMouseButton(state == WL_POINTER_BUTTON_STATE_PRESSED, btn, SDLOP_NO_POS, SDLOP_NO_POS, SDLOP_MonotonicNS());
    }
}

enum
{
    AXIS_CONTINUOUS = 0,
    AXIS_DISCRETE,
    AXIS_VALUE120
};

/* one axis contribution; vertical is negated (Wayland: positive = down,
 * SDL: positive = away from the user) */
static void pointer_axis_common(WaylandDeviceData *d, Uint8 type, uint32_t axis, double value)
{
    if (SDLOP_RawInputIsSource()) {
        return;
    }
    const bool vertical = axis == WL_POINTER_AXIS_VERTICAL_SCROLL;
    if (!vertical && axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        return;
    }
    float *acc = vertical ? &d->axis_y_acc : &d->axis_x_acc;
    Uint8 *kind = vertical ? &d->axis_y_type : &d->axis_x_type;
    const float v = (float)(vertical ? -value : value);
    switch (type) {
    case AXIS_VALUE120: /* may come several times per frame: accumulate */
        if (*kind != AXIS_VALUE120) {
            *kind = AXIS_VALUE120;
            *acc = 0.0f;
        }
        *acc += v;
        break;
    case AXIS_DISCRETE:
        if (*kind != AXIS_DISCRETE) {
            *kind = AXIS_DISCRETE;
            *acc = v;
        }
        break;
    default: /* only without steps in this frame */
        if (*kind == AXIS_CONTINUOUS) {
            *acc = v;
        }
        break;
    }
}

static void flush_pointer_frame(WaylandDeviceData *d);
static bool pointer_has_frames(const WaylandDeviceData *d);

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)pointer;
    (void)time;
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    pointer_axis_common(d, AXIS_CONTINUOUS, axis, wl_fixed_to_double(value));
    if (!pointer_has_frames(d)) {
        flush_pointer_frame(d);
    }
}

static float axis_units(float acc, Uint8 type)
{
    switch (type) {
    case AXIS_VALUE120:
        return acc / 120.0f;
    case AXIS_DISCRETE:
        return acc;
    default:
        return acc / 10.0f; /* weston/sway/libinput: 10 units per detent-ish */
    }
}

/* deliver what a wl_pointer frame accumulated */
static void flush_pointer_frame(WaylandDeviceData *d)
{
    if (d->axis_x_acc != 0.0f || d->axis_y_acc != 0.0f) { /* accumulators stay empty under raw input */
        SDLOP_SendMouseWheelDirection(axis_units(d->axis_x_acc, d->axis_x_type), axis_units(d->axis_y_acc, d->axis_y_type),
                                      d->axis_direction, SDLOP_MonotonicNS());
    }
    d->axis_x_acc = d->axis_y_acc = 0.0f;
    d->axis_x_type = d->axis_y_type = AXIS_CONTINUOUS;
    d->axis_direction = SDL_MOUSEWHEEL_NORMAL;
    /* relative mouse mode: flush accumulated relative-pointer motion
     * (skipped when the raw evdev worker owns the mouse) */
    if (d->relative_pointer && (d->rel_x_acc != 0.0f || d->rel_y_acc != 0.0f)) {
        if (!SDLOP_RawInputIsSource()) {
            SDLOP_SendMouseMotion(SDLOP_NO_POS, SDLOP_NO_POS, d->rel_x_acc, d->rel_y_acc, SDLOP_MonotonicNS());
        }
        d->rel_x_acc = d->rel_y_acc = 0.0f;
    }
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)pointer;
    flush_pointer_frame((WaylandDeviceData *)data);
}

/* wl_pointer before v5 has no frame event: every event is a frame */
static bool pointer_has_frames(const WaylandDeviceData *d)
{
    return d->pointer && wl_pointer_get_version(d->pointer) >= WL_POINTER_FRAME_SINCE_VERSION;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source)
{
    (void)data;
    (void)pointer;
    (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
    (void)pointer;
    pointer_axis_common((WaylandDeviceData *)data, AXIS_DISCRETE, axis, (double)discrete);
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120)
{
    (void)pointer;
    pointer_axis_common((WaylandDeviceData *)data, AXIS_VALUE120, axis, (double)value120);
}

static void pointer_axis_relative_direction(void *data, struct wl_pointer *pointer, uint32_t axis, uint32_t direction)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)pointer;
    (void)axis;
    d->axis_direction =
        direction == WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED ? SDL_MOUSEWHEEL_FLIPPED : SDL_MOUSEWHEEL_NORMAL;
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_enter,
    pointer_leave,
    pointer_motion,
    pointer_button,
    pointer_axis,
    pointer_frame,
    pointer_axis_source,
    pointer_axis_stop,
    pointer_axis_discrete,
    pointer_axis_value120,
    pointer_axis_relative_direction,
};

/* ------------------------------------------------------------------ */
/* wl_touch                                                            */
/* ------------------------------------------------------------------ */

static float wayland_output_scale(struct wl_output *output)
{
    for (int i = 0; i < wl_data.num_outputs; i++) {
        if (wl_data.outputs[i].output == output) {
            return (float)wl_data.outputs[i].scale;
        }
    }
    return 1.0f;
}

static void touch_finger_track(int32_t id, float *nx, float *ny, float *dx, float *dy, bool activate)
{
    for (int i = 0; i < (int)SDL_arraysize(wl_data.fingers); i++) {
        if (wl_data.fingers[i].active && wl_data.fingers[i].id == (Uint64)id) {
            *dx = *nx - wl_data.fingers[i].nx;
            *dy = *ny - wl_data.fingers[i].ny;
            wl_data.fingers[i].nx = *nx;
            wl_data.fingers[i].ny = *ny;
            if (!activate) {
                wl_data.fingers[i].active = false;
            }
            return;
        }
    }
    *dx = 0.0f;
    *dy = 0.0f;
    if (activate) {
        for (int i = 0; i < (int)SDL_arraysize(wl_data.fingers); i++) {
            if (!wl_data.fingers[i].active) {
                wl_data.fingers[i].active = true;
                wl_data.fingers[i].id = (Uint64)id;
                wl_data.fingers[i].nx = *nx;
                wl_data.fingers[i].ny = *ny;
                return;
            }
        }
    }
}

static void touch_down(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time,
                       struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)touch;
    (void)serial;
    (void)time;
    SDL_Window *window = window_from_surface(surface);
    if (!window || window->w <= 0 || window->h <= 0) {
        return;
    }
    float nx = (float)(wl_fixed_to_double(x) / window->w);
    float ny = (float)(wl_fixed_to_double(y) / window->h);
    if (nx < 0.0f) nx = 0.0f;
    if (nx > 1.0f) nx = 1.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (ny > 1.0f) ny = 1.0f;
    float dx, dy;
    touch_finger_track(id, &nx, &ny, &dx, &dy, true);
    d->touch_focus = window;
    SDLOP_SendTouch(SDL_EVENT_FINGER_DOWN, (Uint64)(uintptr_t)d->seat, (Uint64)id,
                    nx, ny, 0.0f, 0.0f, 1.0f, window->id, SDLOP_MonotonicNS());
}

static void touch_up(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time, int32_t id)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)touch;
    (void)serial;
    (void)time;
    /* report the last known position */
    float nx = 0.0f, ny = 0.0f;
    for (int i = 0; i < (int)SDL_arraysize(d->fingers); i++) {
        if (d->fingers[i].active && d->fingers[i].id == (Uint64)id) {
            nx = d->fingers[i].nx;
            ny = d->fingers[i].ny;
            break;
        }
    }
    float dx, dy;
    touch_finger_track(id, &nx, &ny, &dx, &dy, false);
    SDL_WindowID wid = d->touch_focus ? d->touch_focus->id : 0;
    SDLOP_SendTouch(SDL_EVENT_FINGER_UP, (Uint64)(uintptr_t)d->seat, (Uint64)id,
                    nx, ny, 0.0f, 0.0f, 0.0f, wid, SDLOP_MonotonicNS());
}

static void touch_motion(void *data, struct wl_touch *touch, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)touch;
    (void)time;
    SDL_Window *window = d->touch_focus; /* surface of the active touch sequence */
    if (!window || window->w <= 0 || window->h <= 0) {
        return;
    }
    float nx = (float)(wl_fixed_to_double(x) / window->w);
    float ny = (float)(wl_fixed_to_double(y) / window->h);
    if (nx < 0.0f) nx = 0.0f;
    if (nx > 1.0f) nx = 1.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (ny > 1.0f) ny = 1.0f;
    float dx, dy;
    touch_finger_track(id, &nx, &ny, &dx, &dy, true);
    SDLOP_SendTouch(SDL_EVENT_FINGER_MOTION, (Uint64)(uintptr_t)d->seat, (Uint64)id,
                    nx, ny, dx, dy, 1.0f, window->id, SDLOP_MonotonicNS());
}

static void touch_frame(void *data, struct wl_touch *touch)
{
    (void)data;
    (void)touch;
}

static void touch_cancel(void *data, struct wl_touch *touch)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)touch;
    for (int i = 0; i < (int)SDL_arraysize(d->fingers); i++) {
        if (d->fingers[i].active) {
            d->fingers[i].active = false;
            SDLOP_SendTouch(SDL_EVENT_FINGER_CANCELED, (Uint64)(uintptr_t)d->seat, d->fingers[i].id,
                            d->fingers[i].nx, d->fingers[i].ny, 0.0f, 0.0f, 0.0f, 0, SDLOP_MonotonicNS());
        }
    }
}

static void touch_shape(void *data, struct wl_touch *touch, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{
    (void)data; (void)touch; (void)id; (void)major; (void)minor;
}

static void touch_orientation(void *data, struct wl_touch *touch, int32_t id, wl_fixed_t orientation)
{
    (void)data; (void)touch; (void)id; (void)orientation;
}

static const struct wl_touch_listener touch_listener = {
    touch_down,
    touch_up,
    touch_motion,
    touch_frame,
    touch_cancel,
    touch_shape,
    touch_orientation,
};

/* ------------------------------------------------------------------ */
/* wl_output + per-surface output tracking (display scale)             */
/* ------------------------------------------------------------------ */

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                            int32_t physical_width, int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model, int32_t transform)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)physical_width; (void)physical_height; (void)subpixel;
    (void)make; (void)model; (void)transform;
    for (int i = 0; i < d->num_outputs; i++) {
        if (d->outputs[i].output == output) {
            d->outputs[i].x = x;
            d->outputs[i].y = y;
            return;
        }
    }
}

#define WL_OUTPUT_MODE_CURRENT 0x1

static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)refresh;
    for (int i = 0; i < d->num_outputs; i++) {
        if (d->outputs[i].output == output) {
            if ((flags & WL_OUTPUT_MODE_CURRENT) || d->outputs[i].mode_w == 0) {
                d->outputs[i].mode_w = width;
                d->outputs[i].mode_h = height;
            }
            return;
        }
    }
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    for (int i = 0; i < d->num_outputs; i++) {
        if (d->outputs[i].output == output) {
            d->outputs[i].scale = factor;
            return;
        }
    }
}

static void output_done(void *data, struct wl_output *output)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    /* register the core display as soon as the server told us everything */
    for (int i = 0; i < d->num_outputs; i++) {
        if (d->outputs[i].output == output && d->outputs[i].display_id == 0) {
            char name[64];
            if (d->outputs[i].name_str[0]) {
                snprintf(name, sizeof(name), "%s", d->outputs[i].name_str);
            } else {
                snprintf(name, sizeof(name), "wayland-%u", i + 1);
            }
            int w = d->outputs[i].mode_w > 0 ? d->outputs[i].mode_w : 1920;
            int h = d->outputs[i].mode_h > 0 ? d->outputs[i].mode_h : 1080;
            d->outputs[i].display_id =
                SDLOP_AddDisplay(name, w, h, 0, SDL_ORIENTATION_LANDSCAPE, NULL, sdlop.init_done);
            if (d->outputs[i].display_id != 0) {
                int idx = SDLOP_GetDisplayIndexById(d->outputs[i].display_id);
                if (idx >= 0 && d->outputs[i].scale > 0) {
                    sdlop.displays[idx].content_scale = (float)d->outputs[i].scale;
                }
            }
        }
    }
    /* an output scale change rescales windows on it (without a better
     * scale source) */
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        window_update_output_scale(w);
    }
}

static void output_name(void *data, struct wl_output *output, const char *name)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    for (int i = 0; i < d->num_outputs; i++) {
        if (d->outputs[i].output == output && name) {
            snprintf(d->outputs[i].name_str, sizeof(d->outputs[i].name_str), "%s", name);
            return;
        }
    }
}

static void output_description(void *data, struct wl_output *output, const char *description)
{
    (void)data; (void)output; (void)description;
}

static const struct wl_output_listener output_listener = {
    output_geometry,
    output_mode,
    output_done,
    output_scale,
    output_name,
    output_description,
};

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    SDL_Window *window = (SDL_Window *)data;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    (void)surface;
    wd->output = output;
    bool known = false;
    for (int i = 0; i < wd->num_entered; i++) {
        known = known || wd->entered[i] == output;
    }
    if (!known && wd->num_entered < (int)SDL_arraysize(wd->entered)) {
        wd->entered[wd->num_entered++] = output;
    }
    window_update_output_scale(window);
}

static void wayland_GetWindowSizeInPixels(SDLop_VideoDevice *device, SDL_Window *window, int *w, int *h)
{
    (void)device;
    SDLOP_Wayland_GetWindowPixelSize(window, w, h);
}

/* text input: zwp_text_input_v3 (sdlop_wayland_textinput.c) */
static bool wayland_StartTextInput(SDLop_VideoDevice *device, SDL_Window *window, SDL_PropertiesID props)
{
    (void)device;
    (void)props; /* read from window->text_input_props */
    return SDLOP_Wayland_TextInput_Start(window);
}

static bool wayland_StopTextInput(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    return SDLOP_Wayland_TextInput_Stop(window);
}

static bool wayland_UpdateTextInputArea(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    return SDLOP_Wayland_TextInput_UpdateArea(window);
}

static bool wayland_ClearComposition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    return SDLOP_Wayland_TextInput_ClearComposition(window);
}

static void window_forget_output(SDL_Window *window, struct wl_output *output)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    for (int i = 0; i < wd->num_entered; i++) {
        if (wd->entered[i] == output) {
            wd->entered[i] = wd->entered[--wd->num_entered];
            break;
        }
    }
    if (wd->output == output) {
        wd->output = wd->num_entered > 0 ? wd->entered[wd->num_entered - 1] : NULL;
    }
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    SDL_Window *window = (SDL_Window *)data;
    (void)surface;
    window_forget_output(window, output);
    /* the scale follows the remaining outputs; on none it stays until the
     * next enter (SDL3 behavior) */
    window_update_output_scale(window);
}

static void surface_preferred_buffer_scale(void *data, struct wl_surface *surface, int32_t factor)
{
    SDL_Window *window = (SDL_Window *)data;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    (void)surface;
    if (wd && !wd->fractional) { /* the fractional scale wins (stock) */
        wd->buffer_scale_received = true;
        window_set_scale_factor(window, (double)factor);
    }
}

static void fractional_preferred_scale(void *data, struct wp_fractional_scale_v1 *fractional, uint32_t scale)
{
    (void)fractional;
    window_set_scale_factor((SDL_Window *)data, (double)scale / 120.0);
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    fractional_preferred_scale,
};

static void surface_preferred_buffer_transform(void *data, struct wl_surface *surface, uint32_t transform)
{
    (void)data; (void)surface; (void)transform;
}

static const struct wl_surface_listener surface_listener = {
    surface_enter,
    surface_leave,
    surface_preferred_buffer_scale,
    surface_preferred_buffer_transform,
};

/* ------------------------------------------------------------------ */
/* wl_seat                                                             */
/* ------------------------------------------------------------------ */

/* stock SDL3 reports seat device hotplug as KEYBOARD/MOUSE_ADDED|REMOVED */
static Uint32 sdlop_device_next_id(void)
{
    static Uint32 next_id = 0;
    return ++next_id;
}

static void send_keyboard_device_event(bool added, SDL_KeyboardID which)
{
    if (added) {
        SDLOP_AddKeyboard(which, "Keyboard");
    } else {
        SDLOP_RemoveKeyboard(which, true);
    }
}

static void send_mouse_device_event(bool added, SDL_MouseID which)
{
    if (added) {
        SDLOP_AddMouse(which, "Mouse");
    } else {
        SDLOP_RemoveMouse(which, true);
    }
}

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
        d->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
        d->keyboard_id = sdlop_device_next_id();
        send_keyboard_device_event(true, d->keyboard_id);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
        wl_keyboard_destroy(d->keyboard);
        d->keyboard = NULL;
        send_keyboard_device_event(false, d->keyboard_id);
        d->keyboard_id = 0;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
        d->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(d->pointer, &pointer_listener, d);
        ensure_pinch_gesture();
        d->pointer_id = sdlop_device_next_id();
        send_mouse_device_event(true, d->pointer_id);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
        destroy_pinch_gesture();
        if (d->cursor_shape_device) {
            wp_cursor_shape_device_v1_destroy(d->cursor_shape_device);
            d->cursor_shape_device = NULL;
        }
        wl_pointer_destroy(d->pointer);
        d->pointer = NULL;
        d->axis_x_acc = d->axis_y_acc = 0.0f; /* drop a half-accumulated frame */
        send_mouse_device_event(false, d->pointer_id);
        d->pointer_id = 0;
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->touch) {
        d->touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(d->touch, &touch_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->touch) {
        wl_touch_destroy(d->touch);
        d->touch = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    seat_capabilities,
    seat_name,
};

/* ------------------------------------------------------------------ */
/* registry                                                            */
/* ------------------------------------------------------------------ */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        /* v6: wl_surface.preferred_buffer_scale (v5 only forbids attach
         * offsets, which SDLop never uses) */
        const uint32_t max_version = (uint32_t)wl_compositor_interface.version < 6 ? (uint32_t)wl_compositor_interface.version : 6;
        d->compositor_version = version < max_version ? version : max_version;
        d->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, d->compositor_version);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        d->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        d->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, version < 2 ? version : 2);
        xdg_wm_base_add_listener(d->wm_base, &wm_base_listener, d);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !d->seat) {
        d->seat_version = version < 9 ? version : 9;
        d->seat = wl_registry_bind(registry, name, &wl_seat_interface, d->seat_version);
        wl_seat_add_listener(d->seat, &seat_listener, d);
        SDLOP_Wayland_Clipboard_SeatAdded(d->seat);
        SDLOP_Wayland_TextInput_SeatAdded(d->seat);
        SDLOP_Wayland_Tablet_SeatAdded(d->seat);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && d->num_outputs < SDLOP_MAX_OUTPUTS) {
        struct wl_output *o = wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
        d->outputs[d->num_outputs].output = o;
        d->outputs[d->num_outputs].name = name;
        d->outputs[d->num_outputs].scale = 1;
        d->num_outputs++;
        wl_output_add_listener(o, &output_listener, d);
    } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0 && !d->pointer_constraints) {
        d->pointer_constraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0 && !d->relative_manager) {
        d->relative_manager = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0 && !d->cursor_shape_manager &&
               !SDLOP_HintFlag(SDLOP_HINT_WAYLAND_NO_CURSOR_SHAPE, false)) {
        d->cursor_shape_manager = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (strcmp(interface, xdg_toplevel_icon_manager_v1_interface.name) == 0 && !d->toplevel_icon_manager) {
        /* icon_size preferences are ignored (stock) */
        d->toplevel_icon_manager = wl_registry_bind(registry, name, &xdg_toplevel_icon_manager_v1_interface, 1);
    } else if (strcmp(interface, zxdg_exporter_v2_interface.name) == 0 && !d->exporter) {
        d->exporter = wl_registry_bind(registry, name, &zxdg_exporter_v2_interface, 1);
    } else if (strcmp(interface, wp_pointer_warp_v1_interface.name) == 0 && !d->pointer_warp) {
        d->pointer_warp = wl_registry_bind(registry, name, &wp_pointer_warp_v1_interface, 1);
    } else if (strcmp(interface, zwp_pointer_gestures_v1_interface.name) == 0 && !d->pointer_gestures &&
               !SDLOP_HintFlag(SDLOP_HINT_WAYLAND_NO_POINTER_GESTURES, false)) {
        d->pointer_gestures =
            wl_registry_bind(registry, name, &zwp_pointer_gestures_v1_interface, version < 3 ? version : 3);
        ensure_pinch_gesture();
    } else if (strcmp(interface, wp_alpha_modifier_v1_interface.name) == 0 && !d->alpha_modifier) {
        d->alpha_modifier = wl_registry_bind(registry, name, &wp_alpha_modifier_v1_interface, 1);
    } else if (strcmp(interface, xdg_wm_dialog_v1_interface.name) == 0 && !d->wm_dialog) {
        d->wm_dialog = wl_registry_bind(registry, name, &xdg_wm_dialog_v1_interface, 1);
    } else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
        d->has_dmabuf = true;
    } else if (strcmp(interface, "wl_drm") == 0) {
        d->has_wl_drm = true;
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0 && !d->decoration_manager) {
        d->decoration_manager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0 && !d->idle_inhibit_manager) {
        d->idle_inhibit_manager = wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, 1);
    } else if (strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0 &&
               !d->shortcuts_inhibit_manager) {
        d->shortcuts_inhibit_manager =
            wl_registry_bind(registry, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    } else if (strcmp(interface, xdg_activation_v1_interface.name) == 0 && !d->activation) {
        d->activation = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0 && !d->viewporter &&
               !SDLOP_HintFlag(SDLOP_HINT_WAYLAND_NO_VIEWPORTER, false)) {
        d->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0 && !d->fractional_manager &&
               !SDLOP_HintFlag(SDLOP_HINT_WAYLAND_NO_FRACTIONAL_SCALE, false)) {
        d->fractional_manager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else {
        if (!SDLOP_Wayland_TextInput_Global(registry, name, interface, version) &&
            !SDLOP_Wayland_Tablet_Global(registry, name, interface, version)) {
            SDLOP_Wayland_Clipboard_Global(registry, name, interface, version);
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)registry;

    for (int i = 0; i < d->num_outputs; i++) {
        if (d->outputs[i].name != name) {
            continue;
        }
        /* drop window references to the dying output first */
        for (SDL_Window *w = sdlop.windows; w; w = w->next) {
            if (w->driverdata) {
                window_forget_output(w, d->outputs[i].output); /* scale kept until re-enter */
            }
        }
        if (d->outputs[i].display_id) {
            SDLOP_DelDisplay(d->outputs[i].display_id, sdlop.init_done);
        }
        wl_output_destroy(d->outputs[i].output);
        /* compact the list */
        for (int j = i; j < d->num_outputs - 1; j++) {
            d->outputs[j] = d->outputs[j + 1];
        }
        d->num_outputs--;
        return;
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};

/* ------------------------------------------------------------------ */
/* driver interface                                                    */
/* ------------------------------------------------------------------ */

static void wayland_Quit(SDLop_VideoDevice *device);
static bool wayland_SetWindowIcon(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface *icon);
static bool wayland_SetWindowOpacity(SDLop_VideoDevice *device, SDL_Window *window, float opacity);

static bool wayland_Init(SDLop_VideoDevice *device)
{
    (void)device;
    memset(&wl_data, 0, sizeof(wl_data));

    const char *display_name = getenv("WAYLAND_DISPLAY");
    if (!display_name && !SDL_GetHint(SDL_HINT_VIDEO_DRIVER)) {
        /* don't fail hard when probing: no wayland session */
        return SDL_SetError("WAYLAND_DISPLAY not set");
    }

    /* SDL3-style: libwayland-client is dlopen-ed, never linked */
    if (!SDLOP_Wayland_LoadSymbols()) {
        return false;
    }

    wl_data.display = wl_display_connect(NULL);
    if (!wl_data.display) {
        return SDL_SetError("Could not connect to Wayland display");
    }
    wl_data.registry = wl_display_get_registry(wl_data.display);
    wl_registry_add_listener(wl_data.registry, &registry_listener, &wl_data);
    if (wl_display_roundtrip(wl_data.display) < 0) {
        SDLOP_Wayland_Clipboard_Quit();
        SDLOP_Wayland_TextInput_Quit();
        SDLOP_Wayland_Tablet_Quit();
        wl_display_disconnect(wl_data.display);
        wl_data.display = NULL;
        return SDL_SetError("Wayland registry roundtrip failed");
    }
    if (!wl_data.compositor || !wl_data.wm_base || !wl_data.shm) {
        SDLOP_Wayland_Clipboard_Quit();
        SDLOP_Wayland_TextInput_Quit();
        SDLOP_Wayland_Tablet_Quit();
        wl_display_disconnect(wl_data.display);
        wl_data.display = NULL;
        return SDL_SetError("Wayland compositor missing required globals (wl_compositor/xdg_wm_base/wl_shm)");
    }
    /* second roundtrip so seat capabilities (keyboard/pointer) arrive */
    wl_display_roundtrip(wl_data.display);
    wl_data.shm_queue = wl_display_create_queue(wl_data.display);
    if (!wl_data.shm_queue) {
        wayland_Quit(device);
        return SDL_SetError("wl_display_create_queue failed");
    }

    /* system clipboard when the compositor has wl_data_device_manager;
     * otherwise the generic in-process clipboard (hooks left NULL) */
    const bool clip = SDLOP_Wayland_Clipboard_Available();
    const bool prim = SDLOP_Wayland_PrimarySelection_Available();
    device->GetWindowSizeInPixels = wayland_GetWindowSizeInPixels;
    device->SetWindowIcon = wl_data.toplevel_icon_manager ? wayland_SetWindowIcon : NULL;
    device->SetWindowOpacity = wl_data.alpha_modifier ? wayland_SetWindowOpacity : NULL;
    device->StartTextInput = wayland_StartTextInput;
    device->StopTextInput = wayland_StopTextInput;
    device->UpdateTextInputArea = wayland_UpdateTextInputArea;
    device->ClearComposition = wayland_ClearComposition;
    device->SetClipboardData = clip ? SDLOP_Wayland_SetClipboardData : NULL;
    device->GetClipboardData = clip ? SDLOP_Wayland_GetClipboardData : NULL;
    device->HasClipboardData = clip ? SDLOP_Wayland_HasClipboardData : NULL;
    device->GetTextMimeTypes = clip ? SDLOP_Wayland_GetTextMimeTypes : NULL;
    device->SetPrimarySelectionText = prim ? SDLOP_Wayland_SetPrimarySelectionText : NULL;
    device->GetPrimarySelectionText = prim ? SDLOP_Wayland_GetPrimarySelectionText : NULL;
    device->HasPrimarySelectionText = prim ? SDLOP_Wayland_HasPrimarySelectionText : NULL;
    return true;
}

static void wayland_Quit(SDLop_VideoDevice *device)
{
    (void)device;
    SDLOP_Wayland_GL_Deinit();
    SDLOP_Wayland_Clipboard_Quit();
    SDLOP_Wayland_TextInput_Quit();
    SDLOP_Wayland_Tablet_Quit();
    wayland_release_pointer_lock();
    if (wl_data.confined_pointer) {
        zwp_confined_pointer_v1_destroy(wl_data.confined_pointer);
    }
    wayland_cursor_quit();
    if (wl_data.decoration_manager) {
        zxdg_decoration_manager_v1_destroy(wl_data.decoration_manager);
    }
    if (wl_data.idle_inhibit_manager) {
        zwp_idle_inhibit_manager_v1_destroy(wl_data.idle_inhibit_manager);
    }
    if (wl_data.shortcuts_inhibit_manager) {
        zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(wl_data.shortcuts_inhibit_manager);
    }
    if (wl_data.activation) {
        xdg_activation_v1_destroy(wl_data.activation);
    }
    if (wl_data.pointer_constraints) {
        zwp_pointer_constraints_v1_destroy(wl_data.pointer_constraints);
    }
    if (wl_data.relative_manager) {
        zwp_relative_pointer_manager_v1_destroy(wl_data.relative_manager);
    }
    if (wl_data.keyboard) {
        wl_keyboard_destroy(wl_data.keyboard);
    }
    for (size_t i = 0; i < SDL_arraysize(hit_test_cursors); i++) {
        free_cursor_data(hit_test_cursors[i]);
        hit_test_cursors[i] = NULL;
    }
    if (wl_data.cursor_shape_device) {
        wp_cursor_shape_device_v1_destroy(wl_data.cursor_shape_device);
    }
    if (wl_data.cursor_shape_manager) {
        wp_cursor_shape_manager_v1_destroy(wl_data.cursor_shape_manager);
    }
    if (wl_data.toplevel_icon_manager) {
        xdg_toplevel_icon_manager_v1_destroy(wl_data.toplevel_icon_manager);
    }
    if (wl_data.exporter) {
        zxdg_exporter_v2_destroy(wl_data.exporter);
    }
    if (wl_data.wm_dialog) {
        xdg_wm_dialog_v1_destroy(wl_data.wm_dialog);
    }
    if (wl_data.alpha_modifier) {
        wp_alpha_modifier_v1_destroy(wl_data.alpha_modifier);
    }
    if (wl_data.pointer_warp) {
        wp_pointer_warp_v1_destroy(wl_data.pointer_warp);
    }
    destroy_pinch_gesture();
    if (wl_data.pointer_gestures) {
        if (zwp_pointer_gestures_v1_get_version(wl_data.pointer_gestures) >=
            ZWP_POINTER_GESTURES_V1_RELEASE_SINCE_VERSION) {
            zwp_pointer_gestures_v1_release(wl_data.pointer_gestures);
        } else {
            zwp_pointer_gestures_v1_destroy(wl_data.pointer_gestures);
        }
    }
    if (wl_data.pointer) {
        wl_pointer_destroy(wl_data.pointer);
    }
    for (int i = 0; i < wl_data.num_outputs; i++) {
        if (wl_data.outputs[i].display_id) {
            SDLOP_DelDisplay(wl_data.outputs[i].display_id, false);
            wl_data.outputs[i].display_id = 0;
        }
        wl_output_destroy(wl_data.outputs[i].output);
        wl_data.outputs[i].output = NULL;
    }
    wl_data.num_outputs = 0;
    if (wl_data.seat) {
        if (wl_data.touch) {
            wl_touch_destroy(wl_data.touch);
            wl_data.touch = NULL;
        }
        wl_seat_destroy(wl_data.seat);
        wl_data.seat = NULL;
    }
    if (wl_data.wm_base) {
        xdg_wm_base_destroy(wl_data.wm_base);
    }
    if (wl_data.fractional_manager) {
        wp_fractional_scale_manager_v1_destroy(wl_data.fractional_manager);
    }
    if (wl_data.viewporter) {
        wp_viewporter_destroy(wl_data.viewporter);
    }
    if (wl_data.shm_queue) {
        wl_event_queue_destroy(wl_data.shm_queue);
    }
    if (wl_data.shm) {
        wl_shm_destroy(wl_data.shm);
    }
    if (wl_data.compositor) {
        wl_compositor_destroy(wl_data.compositor);
    }
    if (wl_data.registry) {
        wl_registry_destroy(wl_data.registry);
    }
    if (wl_data.display) {
        wl_display_flush(wl_data.display);
        wl_display_disconnect(wl_data.display);
    }
    memset(&wl_data, 0, sizeof(wl_data));
    SDLOP_Wayland_UnloadSymbols();
}

/* (Re)create the xdg role objects and start the configure handshake. Used at
 * creation and on every remap: hiding destroys the role objects (as stock
 * SDL3's Wayland driver does), so showing is a fresh handshake instead of a
 * re-commit of an unmapped surface - compositors require a new configure
 * before any buffer may be attached again ("xdg_surface has never been
 * configured" otherwise). */
/* xdg_toplevel min/max size: SDL limits, or the fixed size of a
 * non-resizable window */
static void apply_size_limits(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || !wd->toplevel) {
        return;
    }
    int min_w = window->min_w, min_h = window->min_h, max_w = window->max_w, max_h = window->max_h;
    if (!(window->flags & SDL_WINDOW_RESIZABLE) && !(window->flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
        min_w = max_w = window->w;
        min_h = max_h = window->h;
    }
    xdg_toplevel_set_min_size(wd->toplevel, min_w, min_h);
    xdg_toplevel_set_max_size(wd->toplevel, max_w, max_h);
}

/* SDL3 window properties: the native Wayland objects, as stock SDL
 * publishes them (apps with their own renderer: wgpu, bgfx, ...). The xdg
 * role objects follow hide/show and popup re-creation. */
static void wayland_update_window_props(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    /* window->props directly: SDL_GetWindowProperties() rejects a window
     * that is still being created */
    if (!window->props) {
        if (!wd) {
            return;
        }
        window->props = SDL_CreateProperties();
    }
    const SDL_PropertiesID props = window->props;
    if (!props) {
        return;
    }
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, wd ? wl_data.display : NULL);
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, wd ? wd->surface : NULL);
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_VIEWPORT_POINTER, wd ? wd->viewport : NULL);
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_XDG_SURFACE_POINTER, wd ? wd->xsurface : NULL);
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_POINTER, wd ? wd->toplevel : NULL);
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_XDG_POPUP_POINTER, wd ? wd->popup : NULL);
}

static void destroy_window_icon(WaylandWindowData *wd)
{
    if (wd->icon) {
        xdg_toplevel_icon_v1_destroy(wd->icon); /* before its buffers */
        wd->icon = NULL;
    }
    for (int i = 0; i < wd->num_icon_buffers; i++) {
        wl_buffer_destroy(wd->icon_buffers[i]);
    }
    free(wd->icon_buffers);
    wd->icon_buffers = NULL;
    wd->num_icon_buffers = 0;
}

static int icon_size_compare(const void *a, const void *b)
{
    const SDL_Surface *s1 = *(SDL_Surface *const *)a, *s2 = *(SDL_Surface *const *)b;
    return (s1->w * s1->h > s2->w * s2->h) - (s1->w * s1->h < s2->w * s2->h);
}

/* xdg_toplevel_icon_v1 (stock): one square buffer per integer scale of the
 * icon's alternate images, non-square images centred; the icon object is
 * immutable once set, so every call builds a new one */
static bool wayland_SetWindowIcon(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface *icon)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    int count = 0;
    SDL_Surface **images = SDL_GetSurfaceImages(icon, &count);
    if (!images) {
        return false;
    }
    /* ascending size: the largest image of each scale wins */
    SDL_qsort(images, (size_t)count, sizeof(*images), icon_size_compare);
    destroy_window_icon(wd);
    wd->icon_buffers = (struct wl_buffer **)calloc((size_t)count, sizeof(*wd->icon_buffers));
    if (!wd->icon_buffers) {
        SDL_free(images);
        return SDL_OutOfMemory();
    }
    wd->icon = xdg_toplevel_icon_manager_v1_create_icon(wl_data.toplevel_icon_manager);
    const int base = SDL_max(icon->w, icon->h);
    bool ok = base > 0;
    for (int i = 0; ok && i < count; i++) {
        const int size = SDL_max(images[i]->w, images[i]->h);
        const int scale = size / base;
        if (scale < 1) {
            continue; /* smaller than the base image (stock) */
        }
        SDL_Surface *s = images[i]->format == SDL_PIXELFORMAT_ARGB8888
                             ? images[i]
                             : SDL_ConvertSurface(images[i], SDL_PIXELFORMAT_ARGB8888);
        struct wl_buffer *b = NULL;
        if (s) {
            b = create_shm_argb_buffer((const uint32_t *)s->pixels, s->w, s->h, s->pitch / 4, size, size,
                                       (size - s->w) / 2, (size - s->h) / 2);
            if (s != images[i]) {
                SDL_DestroySurface(s);
            }
        }
        if (!b) {
            ok = SDL_SetError("wayland: failed to create an icon buffer");
            break;
        }
        wd->icon_buffers[wd->num_icon_buffers++] = b;
        xdg_toplevel_icon_v1_add_buffer(wd->icon, b, scale);
    }
    SDL_free(images);
    if (!ok) {
        destroy_window_icon(wd);
        return false;
    }
    if (wd->toplevel) {
        xdg_toplevel_icon_manager_v1_set_icon(wl_data.toplevel_icon_manager, wd->toplevel, wd->icon);
    }
    wl_display_flush(wl_data.display);
    return true;
}

/* xdg_dialog_v1 (stock): modal state of a toplevel with a parent; without
 * the protocol the parent relation alone is all the compositor gets */
static void apply_modal(SDL_Window *window, bool modal)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || !wd->toplevel || !wl_data.wm_dialog) {
        return;
    }
    if (modal) {
        if (!wd->dialog) {
            wd->dialog = xdg_wm_dialog_v1_get_xdg_dialog(wl_data.wm_dialog, wd->toplevel);
        }
        xdg_dialog_v1_set_modal(wd->dialog);
    } else if (wd->dialog) {
        xdg_dialog_v1_unset_modal(wd->dialog);
    }
}

static bool wayland_SetWindowModal(SDLop_VideoDevice *device, SDL_Window *window, bool modal)
{
    (void)device;
    apply_modal(window, modal);
    wl_display_flush(wl_data.display);
    return true;
}

/* wp_alpha_modifier_v1 (stock): the compositor multiplies the surface
 * alpha; double-buffered, so commit it now unless EGL commits every frame
 * anyway (a bare commit does not touch the attached buffer) */
static bool wayland_SetWindowOpacity(SDLop_VideoDevice *device, SDL_Window *window, float opacity)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    if (!wd->alpha) {
        wd->alpha = wp_alpha_modifier_v1_get_surface(wl_data.alpha_modifier, wd->surface);
    }
    wp_alpha_modifier_surface_v1_set_multiplier(wd->alpha, (uint32_t)((double)UINT32_MAX * (double)opacity));
    window->opacity = opacity; /* the region follows it now */
    window_update_opaque_region(window, wd);
    if (wd->configured) {
        if (wd->gl_active && !wd->gl_offload_active) {
            wl_surface_commit(wd->surface);
        } else {
            window_commit(window);
        }
    }
    wl_display_flush(wl_data.display);
    return true;
}

/* xdg-foreign (stock): SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_EXPORT_HANDLE_STRING
 * names the toplevel to other clients, e.g. the file chooser portal
 * ("wayland:<handle>" parent) */
static void exported_handle(void *data, struct zxdg_exported_v2 *exported, const char *handle)
{
    (void)exported;
    SDL_Window *window = (SDL_Window *)data;
    if (!window->props) { /* still being created: SDL_GetWindowProperties() refuses it */
        window->props = SDL_CreateProperties();
    }
    if (window->props) {
        SDL_SetStringProperty(window->props, SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_EXPORT_HANDLE_STRING, handle);
    }
}

static const struct zxdg_exported_v2_listener exported_listener = { exported_handle };

static void destroy_role_objects(WaylandWindowData *wd)
{
    if (wd->exported) {
        zxdg_exported_v2_destroy(wd->exported);
        wd->exported = NULL;
        if (wd->window && wd->window->props) {
            SDL_SetStringProperty(wd->window->props, SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_EXPORT_HANDLE_STRING, NULL);
        }
    }
    if (wd->dialog) {
        xdg_dialog_v1_destroy(wd->dialog); /* before its toplevel */
        wd->dialog = NULL;
    }
    if (wd->decoration) {
        zxdg_toplevel_decoration_v1_destroy(wd->decoration); /* before its toplevel */
        wd->decoration = NULL;
    }
    if (wd->toplevel) {
        xdg_toplevel_destroy(wd->toplevel);
        wd->toplevel = NULL;
    }
    if (wd->popup) {
        xdg_popup_destroy(wd->popup);
        wd->popup = NULL;
    }
    if (wd->xsurface) {
        xdg_surface_destroy(wd->xsurface);
        wd->xsurface = NULL;
    }
}

/* xdg_popup: placed relative to the parent surface at the SDL offset */
static void popup_configure(void *data, struct xdg_popup *popup, int32_t x, int32_t y, int32_t width, int32_t height)
{
    SDL_Window *window = (SDL_Window *)data;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    (void)popup;
    if (width > 0 && height > 0) {
        wd->pending_w = width;
        wd->pending_h = height;
    }
    if (window->parent) {
        const int nx = window->parent->x + x;
        const int ny = window->parent->y + y;
        if (nx != window->x || ny != window->y) {
            window->x = nx;
            window->y = ny;
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOVED, nx, ny); /* reported parent-relative */
        }
    }
}

static void popup_done(void *data, struct xdg_popup *popup)
{
    (void)popup;
    /* dismissed by the compositor (click outside a popup menu) */
    SDL_HideWindow((SDL_Window *)data);
}

static void popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token)
{
    (void)data;
    (void)popup;
    (void)token;
}

static const struct xdg_popup_listener popup_listener = {
    popup_configure,
    popup_done,
    popup_repositioned,
};

static bool create_popup_role(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    WaylandWindowData *pd = window->parent ? (WaylandWindowData *)window->parent->driverdata : NULL;
    if (!pd || !pd->xsurface) {
        xdg_surface_destroy(wd->xsurface);
        wd->xsurface = NULL;
        return SDL_SetError("Popup parent window is not mapped");
    }
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(wl_data.wm_base);
    xdg_positioner_set_size(pos, window->w, window->h);
    xdg_positioner_set_anchor_rect(pos, window->popup_offset_x, window->popup_offset_y, 1, 1);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    /* stock SDL3: keep menus on screen by sliding/flipping */
    xdg_positioner_set_constraint_adjustment(pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                                                      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                                                      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
    wd->popup = xdg_surface_get_popup(wd->xsurface, pd->xsurface, pos);
    xdg_positioner_destroy(pos);
    if (!wd->popup) {
        xdg_surface_destroy(wd->xsurface);
        wd->xsurface = NULL;
        return SDL_SetError("xdg_surface_get_popup failed");
    }
    xdg_popup_add_listener(wd->popup, &popup_listener, window);
    wd->moved_reported = true; /* popup positions come from popup_configure */
    if (window->flags & SDL_WINDOW_TOOLTIP) {
        /* tooltips never take input */
        struct wl_region *empty = wl_compositor_create_region(wl_data.compositor);
        wl_surface_set_input_region(wd->surface, empty);
        wl_region_destroy(empty);
    } else if (wl_data.seat && wl_data.input_serial && !(window->flags & SDL_WINDOW_NOT_FOCUSABLE)) {
        /* popup menus take the keyboard and are dismissed on outside clicks */
        xdg_popup_grab(wd->popup, wl_data.seat, wl_data.input_serial);
    }
    wl_surface_commit(wd->surface);
    return true;
}

static bool wayland_create_role_objects_raw(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    wd->xsurface = xdg_wm_base_get_xdg_surface(wl_data.wm_base, wd->surface);
    if (!wd->xsurface) {
        return SDL_SetError("xdg_wm_base_get_xdg_surface failed");
    }
    xdg_surface_add_listener(wd->xsurface, &xsurface_listener, window);
    if (SDLOP_WINDOW_IS_POPUP(window)) {
        return create_popup_role(window);
    }
    wd->toplevel = xdg_surface_get_toplevel(wd->xsurface);
    if (!wd->toplevel) {
        xdg_surface_destroy(wd->xsurface);
        wd->xsurface = NULL;
        return SDL_SetError("xdg_surface_get_toplevel failed");
    }
    xdg_toplevel_add_listener(wd->toplevel, &toplevel_listener, window);
    xdg_toplevel_set_title(wd->toplevel, window->title ? window->title : "SDLop");
    xdg_toplevel_set_app_id(wd->toplevel, SDLOP_GetAppID());
    if (wd->icon) {
        /* re-shown window: a fresh toplevel (an icon can be set on many) */
        xdg_toplevel_icon_manager_v1_set_icon(wl_data.toplevel_icon_manager, wd->toplevel, wd->icon);
    }
    if (wl_data.decoration_manager) {
        wd->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(wl_data.decoration_manager, wd->toplevel);
        zxdg_toplevel_decoration_v1_set_mode(wd->decoration, (window->flags & SDL_WINDOW_BORDERLESS)
                                                                 ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                                                                 : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    if (wl_data.exporter) {
        wd->exported = zxdg_exporter_v2_export_toplevel(wl_data.exporter, wd->surface);
        zxdg_exported_v2_add_listener(wd->exported, &exported_listener, window);
    }
    apply_size_limits(window);
    if (window->parent && window->parent->driverdata &&
        ((WaylandWindowData *)window->parent->driverdata)->toplevel) {
        xdg_toplevel_set_parent(wd->toplevel, ((WaylandWindowData *)window->parent->driverdata)->toplevel);
    }
    if ((window->flags & SDL_WINDOW_MODAL) && window->parent) {
        apply_modal(window, true);
    }
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        xdg_toplevel_set_fullscreen(wd->toplevel, NULL);
    }
    if (window->flags & SDL_WINDOW_MAXIMIZED) {
        xdg_toplevel_set_maximized(wd->toplevel);
    }
    /* kick off the configure sequence (buffer is attached by
     * xsurface_configure once the compositor answers) */
    wl_surface_commit(wd->surface);
    return true;
}

static bool wayland_create_role_objects(SDL_Window *window)
{
    const bool ok = wayland_create_role_objects_raw(window);
    wayland_update_window_props(window);
    return ok;
}

static bool wayland_CreateWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)calloc(1, sizeof(*wd));
    if (!wd) {
        return SDL_OutOfMemory();
    }
    wd->surface = wl_compositor_create_surface(wl_data.compositor);
    if (!wd->surface) {
        free(wd);
        return SDL_SetError("wl_compositor_create_surface failed");
    }
    wl_surface_add_listener(wd->surface, &surface_listener, window);
    wd->window = window;
    wd->dest_w = wd->dest_h = -1;
    wd->transparent = (window->flags & SDL_WINDOW_TRANSPARENT) != 0;
    window->driverdata = wd; /* window_clear_xrgb() needs it from here on */
    window_update_opaque_region(window, wd);
    if (wd->transparent && sdlop_glattrs.alpha_size == 0) {
        sdlop_glattrs.alpha_size = 8; /* an EGL config with alpha (stock) */
    }
    if (wl_data.viewporter) {
        wd->viewport = wp_viewporter_get_viewport(wl_data.viewporter, wd->surface);
    }
    /* fractional scales need the viewport (stock) */
    if (wl_data.fractional_manager && wd->viewport) {
        wd->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(wl_data.fractional_manager, wd->surface);
        wp_fractional_scale_v1_add_listener(wd->fractional, &fractional_listener, window);
    }
    /* initial scale until the compositor tells (stock): the parent's for
     * popups, else the largest output scale */
    wd->scale_factor = 1.0;
    if (SDLOP_WINDOW_IS_POPUP(window) && window->parent && window->parent->driverdata) {
        wd->scale_factor = ((WaylandWindowData *)window->parent->driverdata)->scale_factor;
    } else {
        for (int i = 0; i < wl_data.num_outputs; i++) {
            if (wl_data.outputs[i].scale > wd->scale_factor) {
                wd->scale_factor = wl_data.outputs[i].scale;
            }
        }
    }
    window->display_scale = (float)wd->scale_factor;
    window->driverdata = wd;
    SDLOP_Wayland_GetWindowPixelSize(window, &wd->pixel_w, &wd->pixel_h);

    if (!wayland_create_role_objects(window)) {
        if (wd->fractional) {
            wp_fractional_scale_v1_destroy(wd->fractional);
        }
        if (wd->viewport) {
            wp_viewport_destroy(wd->viewport);
        }
        wl_surface_destroy(wd->surface);
        window->driverdata = NULL;
        free(wd);
        return false;
    }
    if (sdlop.suspend_screensaver && wl_data.idle_inhibit_manager) {
        wd->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(wl_data.idle_inhibit_manager, wd->surface);
    }
    wayland_update_window_props(window);
    wl_display_roundtrip(wl_data.display);
    return true;
}

static void wayland_DestroyWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd) {
        return;
    }
    if (wl_data.pointer_focus == window) {
        wl_data.pointer_focus = NULL;
    }
    if (wl_data.relative_lock_window == window) {
        wayland_release_pointer_lock();
        wl_data.relative_lock_window = NULL;
    }
    if (wl_data.confine_window == window) {
        zwp_confined_pointer_v1_destroy(wl_data.confined_pointer);
        wl_data.confined_pointer = NULL;
        wl_data.confine_window = NULL;
    }
    if (wd->idle_inhibitor) {
        zwp_idle_inhibitor_v1_destroy(wd->idle_inhibitor);
    }
    if (wd->shortcuts_inhibitor) {
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(wd->shortcuts_inhibitor);
    }
    SDLOP_Wayland_GL_WindowDestroyed(window);
    SDLOP_Wayland_TextInput_WindowDestroyed(window);
    SDLOP_Wayland_Tablet_WindowDestroyed(window);
    if (wl_data.pinch_focus == window) {
        wl_data.pinch_focus = NULL;
    }
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
    destroy_role_objects(wd);
    destroy_window_icon(wd);
    if (wd->alpha) {
        wp_alpha_modifier_surface_v1_destroy(wd->alpha);
    }
    if (wd->fractional) {
        wp_fractional_scale_v1_destroy(wd->fractional);
    }
    if (wd->viewport) {
        wp_viewport_destroy(wd->viewport);
    }
    if (wd->surface) {
        wl_surface_destroy(wd->surface);
    }
    window_destroy_buffers(wd);
    free(wd);
    window->driverdata = NULL;
    wayland_update_window_props(window);
}

static void wayland_ShowWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd) {
        return;
    }
    if (!wd->toplevel && !wd->popup) {
        /* remap after hide: fresh role objects + configure handshake;
         * xsurface_configure attaches the buffer */
        if (wayland_create_role_objects(window)) {
            wl_display_flush(wl_data.display);
        }
        return;
    }
    window_commit(window);
    wl_display_flush(wl_data.display);
}

static void wayland_HideWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd) {
        return;
    }
    /* Unmap the stock way: destroy the role objects first - leaving them on
     * a buffer-less surface strands the xdg_surface in a state where the
     * next buffer attach is a protocol error, and a bare commit alone will
     * not restart the configure handshake. */
    destroy_role_objects(wd);
    wayland_update_window_props(window);
    wl_surface_attach(wd->surface, NULL, 0, 0);
    wl_surface_commit(wd->surface);
    wd->configured = false;
    wd->mapped = false;
    wd->pending_w = 0;
    wd->pending_h = 0;
    wl_display_flush(wl_data.display);
}

static bool wayland_SetWindowTitle(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (wd && wd->toplevel) {
        xdg_toplevel_set_title(wd->toplevel, window->title);
        wl_display_flush(wl_data.display);
    }
    return true;
}

static bool wayland_SetWindowSize(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    /* clients can only suggest sizes on Wayland; update buffer and let the
     * compositor confirm via configure */
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    int pw, ph;
    SDLOP_Wayland_GetWindowPixelSize(window, &pw, &ph);
    window_resize_buffers(window, pw, ph);
    apply_size_limits(window);
    window_commit(window);
    wl_display_flush(wl_data.display);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
    if (wd) {
        wd->pixel_w = pw;
        wd->pixel_h = ph;
    }
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, pw, ph);
    /* stock pairs SAFE_AREA_CHANGED with every size change, including this
     * client-initiated one (measured on 3.2.10 with the behaviour probe) */
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_SAFE_AREA_CHANGED, 0, 0);
    return true;
}

static bool wayland_SetWindowPosition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (SDLOP_WINDOW_IS_POPUP(window) && wd) {
        /* xdg_popup positions are fixed at creation (reposition needs
         * xdg_wm_base v3): remap with the new offset, as SDL3 does */
        if (wd->popup) {
            destroy_role_objects(wd);
            wl_surface_attach(wd->surface, NULL, 0, 0);
            wl_surface_commit(wd->surface);
            wd->configured = false;
            wd->mapped = false;
            if (!wayland_create_role_objects(window)) {
                return false;
            }
            wl_display_flush(wl_data.display);
        }
        return true;
    }
    return SDL_SetError("Wayland does not allow absolute window positioning");
}

static void wayland_MinimizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (wd && wd->toplevel) {
        xdg_toplevel_set_minimized(wd->toplevel);
        wl_display_flush(wl_data.display);
        window->flags |= SDL_WINDOW_MINIMIZED;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MINIMIZED, 0, 0);
    }
}

static void wayland_MaximizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (wd && wd->toplevel) {
        xdg_toplevel_set_maximized(wd->toplevel);
        wl_display_flush(wl_data.display);
    }
}

static void wayland_RestoreWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (wd && wd->toplevel) {
        xdg_toplevel_unset_maximized(wd->toplevel);
        xdg_toplevel_unset_fullscreen(wd->toplevel);
        wl_display_flush(wl_data.display);
    }
}

static bool wayland_SetWindowFullscreen(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || !wd->toplevel) {
        return SDL_SetError("Invalid window");
    }
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        xdg_toplevel_set_fullscreen(wd->toplevel, NULL);
    } else {
        xdg_toplevel_unset_fullscreen(wd->toplevel);
    }
    wl_display_flush(wl_data.display);
    return true;
}

/* xdg-activation: ask the compositor to activate (raise / focus) a
 * surface; whether it complies is its policy (SDL3 does the same) */
static void activation_done(void *data, struct xdg_activation_token_v1 *token, const char *token_str)
{
    SDL_Window *window = (SDL_Window *)data;
    WaylandWindowData *wd = SDLOP_WindowValid(window) ? (WaylandWindowData *)window->driverdata : NULL;
    if (wd && wd->surface && wl_data.activation) {
        xdg_activation_v1_activate(wl_data.activation, token_str, wd->surface);
    }
    xdg_activation_token_v1_destroy(token);
}

static const struct xdg_activation_token_v1_listener activation_token_listener = {
    activation_done,
};

static void request_activation(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || !wl_data.activation) {
        return;
    }
    struct xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token(wl_data.activation);
    xdg_activation_token_v1_add_listener(token, &activation_token_listener, window);
    if (wl_data.seat && wl_data.pointer_serial) {
        xdg_activation_token_v1_set_serial(token, wl_data.pointer_serial, wl_data.seat);
    }
    xdg_activation_token_v1_set_surface(token, wd->surface);
    xdg_activation_token_v1_commit(token);
    wl_display_flush(wl_data.display);
}

static void wayland_RaiseWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    request_activation(window);
}

static void wayland_SetWindowClearColor(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (wd) {
        wd->clear_explicit = true;
    }
    if (!wd || !wd->configured || (wd->gl_active && !wd->gl_offload_active)) {
        return;
    }
    if (wd->shadow) {
        /* as on X11: the software surface is cleared and presented */
        fill_xrgb(wd->shadow, (size_t)wd->shadow_w * (size_t)wd->shadow_h, window_clear_xrgb(window));
        wayland_UpdateWindowFramebuffer(device, window, NULL, 0);
    } else if (!wd->front && !(window->flags & SDL_WINDOW_HIDDEN)) {
        /* still showing the clear colour: show the new one */
        window_commit(window);
    }
    wl_display_flush(wl_data.display);
}

/* ------------------------------------------------------------------ */
/* SDL 3.4 window management                                           */
/* ------------------------------------------------------------------ */

static void commit_if_configured(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (wd && wd->configured) {
        wl_surface_commit(wd->surface);
    }
    wl_display_flush(wl_data.display);
}

static void wayland_UpdateSizeLimits(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    apply_size_limits(window);
    commit_if_configured(window);
}

static void wayland_SetWindowResizable(SDLop_VideoDevice *device, SDL_Window *window, bool resizable)
{
    (void)resizable; /* window->flags already updated */
    wayland_UpdateSizeLimits(device, window);
}

static bool wayland_SetWindowParent(SDLop_VideoDevice *device, SDL_Window *window, SDL_Window *parent)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    WaylandWindowData *pd = parent ? (WaylandWindowData *)parent->driverdata : NULL;
    if (wd && wd->toplevel) {
        xdg_toplevel_set_parent(wd->toplevel, pd ? pd->toplevel : NULL);
        wl_display_flush(wl_data.display);
    }
    return true;
}

static void wayland_SetWindowBordered(SDLop_VideoDevice *device, SDL_Window *window, bool bordered)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (wd && wd->decoration) {
        /* SDLop draws no client-side decorations: client-side = none */
        zxdg_toplevel_decoration_v1_set_mode(wd->decoration, bordered ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                                                      : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
        commit_if_configured(window);
    }
}

static void update_confinement(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (wl_data.confine_window == window) {
        zwp_confined_pointer_v1_destroy(wl_data.confined_pointer);
        wl_data.confined_pointer = NULL;
        wl_data.confine_window = NULL;
    }
    const bool want = (window->flags & SDL_WINDOW_INPUT_FOCUS) &&
                      ((window->flags & SDL_WINDOW_MOUSE_GRABBED) || !SDL_RectEmpty(&window->mouse_rect));
    if (!want || !wd || !wl_data.pointer || !wl_data.pointer_constraints || wl_data.relative_lock_window == window) {
        return;
    }
    if (wl_data.confined_pointer) {
        /* another window held it */
        zwp_confined_pointer_v1_destroy(wl_data.confined_pointer);
        wl_data.confined_pointer = NULL;
    }
    struct wl_region *region = NULL;
    if (!SDL_RectEmpty(&window->mouse_rect)) {
        region = wl_compositor_create_region(wl_data.compositor);
        wl_region_add(region, window->mouse_rect.x, window->mouse_rect.y, window->mouse_rect.w, window->mouse_rect.h);
    }
    wl_data.confined_pointer = zwp_pointer_constraints_v1_confine_pointer(
        wl_data.pointer_constraints, wd->surface, wl_data.pointer, region, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    wl_data.confine_window = window;
    if (region) {
        wl_region_destroy(region);
    }
    wl_display_flush(wl_data.display);
}

/* Pointer warps (stock): wp_pointer_warp_v1, else the pointer-constraints
 * trick - a oneshot lock with a cursor position hint, released at once
 * (only one constraint per surface: a confinement is lifted meanwhile).
 * Either way only the window with the pointer focus can warp, and the
 * compositor sends no motion for it. */
static bool warp_pointer(SDL_Window *window, float x, float y)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || !wl_data.pointer || wl_data.pointer_focus != window) {
        return false;
    }
    if (wl_data.pointer_warp) {
        /* a position outside the surface is a protocol error */
        const double cx = SDL_clamp((double)x, 0.0, (double)window->w);
        const double cy = SDL_clamp((double)y, 0.0, (double)window->h);
        wp_pointer_warp_v1_warp_pointer(wl_data.pointer_warp, wd->surface, wl_data.pointer, wl_fixed_from_double(cx),
                                        wl_fixed_from_double(cy), wl_data.pointer_serial);
    } else if (wl_data.pointer_constraints && !wl_data.locked_pointer) {
        const bool reconfine = wl_data.confined_pointer != NULL;
        if (reconfine) {
            zwp_confined_pointer_v1_destroy(wl_data.confined_pointer);
            wl_data.confined_pointer = NULL;
        }
        SDL_Window *confined = wl_data.confine_window;
        wl_data.confine_window = NULL;
        struct zwp_locked_pointer_v1 *lock = zwp_pointer_constraints_v1_lock_pointer(
            wl_data.pointer_constraints, wd->surface, wl_data.pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
        zwp_locked_pointer_v1_set_cursor_position_hint(lock, wl_fixed_from_double(x), wl_fixed_from_double(y));
        wl_surface_commit(wd->surface); /* the hint is double-buffered */
        zwp_locked_pointer_v1_destroy(lock);
        if (reconfine && confined) {
            update_confinement(confined);
        }
    } else {
        return false;
    }
    wl_display_flush(wl_data.display);
    return true;
}

static bool wayland_WarpMouse(SDLop_VideoDevice *device, SDL_Window *window, float x, float y)
{
    (void)device;
    warp_pointer(window, x, y);
    return false; /* no motion from the compositor: the core synthesises it (stock) */
}

static bool wayland_WarpMouseGlobal(SDLop_VideoDevice *device, float x, float y)
{
    (void)device;
    /* only within the window that has the pointer (stock) */
    SDL_Window *window = wl_data.pointer_focus;
    if (!window) {
        return true;
    }
    SDL_Rect bounds = { 0, 0, 0, 0 };
    SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &bounds);
    const float lx = x - (float)(bounds.x + window->x), ly = y - (float)(bounds.y + window->y);
    if (lx >= 0.0f && ly >= 0.0f && lx < (float)window->w && ly < (float)window->h && warp_pointer(window, lx, ly)) {
        SDLOP_SendMouseMotion(lx, ly, 0.0f, 0.0f, 0);
    }
    return true;
}

static bool wayland_SetWindowHitTest(SDLop_VideoDevice *device, SDL_Window *window, bool enabled)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (wd) {
        wd->hit_test_result = SDL_HITTEST_NORMAL;
        if (enabled && wl_data.pointer_focus == window) {
            update_hit_test(window, sdlop.mouse_x, sdlop.mouse_y);
        }
        if (wl_data.pointer_focus == window) {
            apply_cursor(wl_data.pointer_serial);
            wl_display_flush(wl_data.display);
        }
    }
    return true;
}

/* the compositor's window menu, for the latest button press (stock) */
static void wayland_ShowWindowSystemMenu(SDLop_VideoDevice *device, SDL_Window *window, int x, int y)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (wd && wd->toplevel && wl_data.seat && wl_data.input_serial) {
        xdg_toplevel_show_window_menu(wd->toplevel, wl_data.seat, wl_data.input_serial, x, y);
        wl_display_flush(wl_data.display);
    }
}

static bool wayland_SetWindowMouseGrab(SDLop_VideoDevice *device, SDL_Window *window, bool grabbed)
{
    (void)device;
    (void)grabbed; /* derived from the window flags, like the mouse rect */
    if (!wl_data.pointer_constraints) {
        return SDL_SetError("Wayland compositor lacks pointer-constraints support");
    }
    update_confinement(window);
    return true;
}

static bool wayland_SetWindowMouseRect(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (!wl_data.pointer_constraints) {
        return SDL_SetError("Wayland compositor lacks pointer-constraints support");
    }
    update_confinement(window);
    return true;
}

static bool wayland_SetWindowKeyboardGrab(SDLop_VideoDevice *device, SDL_Window *window, bool grabbed)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wl_data.shortcuts_inhibit_manager || !wl_data.seat) {
        return SDL_SetError("Wayland compositor lacks keyboard-shortcuts-inhibit support");
    }
    if (grabbed && !wd->shortcuts_inhibitor) {
        wd->shortcuts_inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
            wl_data.shortcuts_inhibit_manager, wd->surface, wl_data.seat);
    } else if (!grabbed && wd->shortcuts_inhibitor) {
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(wd->shortcuts_inhibitor);
        wd->shortcuts_inhibitor = NULL;
    }
    wl_display_flush(wl_data.display);
    return true;
}

static bool wayland_SuspendScreenSaver(SDLop_VideoDevice *device)
{
    (void)device;
    if (!wl_data.idle_inhibit_manager) {
        return SDL_Unsupported();
    }
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        WaylandWindowData *wd = (WaylandWindowData *)w->driverdata;
        if (!wd) {
            continue;
        }
        if (sdlop.suspend_screensaver && !wd->idle_inhibitor) {
            wd->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(wl_data.idle_inhibit_manager, wd->surface);
        } else if (!sdlop.suspend_screensaver && wd->idle_inhibitor) {
            zwp_idle_inhibitor_v1_destroy(wd->idle_inhibitor);
            wd->idle_inhibitor = NULL;
        }
    }
    wl_display_flush(wl_data.display);
    return true;
}

static bool wayland_FlashWindow(SDLop_VideoDevice *device, SDL_Window *window, SDL_FlashOperation operation)
{
    (void)device;
    /* SDL3: xdg-activation is the attention request on Wayland; there is
     * nothing to cancel */
    if (operation != SDL_FLASH_CANCEL) {
        request_activation(window);
    }
    return true;
}

static bool wayland_SyncWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    if (wl_data.display) {
        wl_display_roundtrip(wl_data.display);
        wl_display_dispatch_pending(wl_data.display);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* relative mouse mode via zwp_pointer_constraints + relative_pointer  */
/* ------------------------------------------------------------------ */

static void relative_pointer_motion(void *data, struct zwp_relative_pointer_v1 *rp,
                                    uint32_t utime_hi, uint32_t utime_lo,
                                    wl_fixed_t dx, wl_fixed_t dy,
                                    wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
    (void)data;
    (void)rp;
    (void)utime_hi;
    (void)utime_lo;
    (void)dx;
    (void)dy;
    /* accumulated and flushed on the wl_pointer frame; unaccelerated
     * deltas match SDL3's wayland behavior */
    wl_data.rel_x_acc += (float)wl_fixed_to_double(dx_unaccel);
    wl_data.rel_y_acc += (float)wl_fixed_to_double(dy_unaccel);
    if (!pointer_has_frames(&wl_data)) {
        flush_pointer_frame(&wl_data); /* no frame will follow */
    }
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    relative_pointer_motion,
};

static void locked_pointer_locked(void *data, struct zwp_locked_pointer_v1 *lp)
{
    (void)data;
    (void)lp;
    wl_pointer_set_cursor(wl_data.pointer, 0, NULL, 0, 0); /* hide */
}

static void locked_pointer_unlocked(void *data, struct zwp_locked_pointer_v1 *lp)
{
    (void)data;
    (void)lp;
    /* compositor released the lock (e.g. policy); relative motion stops.
     * The lock is re-armed on the next pointer enter while mode is on. */
    if (wl_data.locked_pointer) {
        zwp_locked_pointer_v1_destroy(wl_data.locked_pointer);
        wl_data.locked_pointer = NULL;
    }
}

static const struct zwp_locked_pointer_v1_listener locked_pointer_listener = {
    locked_pointer_locked,
    locked_pointer_unlocked,
};

static void wayland_release_pointer_lock(void)
{
    if (wl_data.locked_pointer) {
        zwp_locked_pointer_v1_destroy(wl_data.locked_pointer);
        wl_data.locked_pointer = NULL;
    }
    if (wl_data.relative_pointer) {
        zwp_relative_pointer_v1_destroy(wl_data.relative_pointer);
        wl_data.relative_pointer = NULL;
    }
    wl_data.rel_x_acc = wl_data.rel_y_acc = 0.0f;
}

static bool wayland_arm_pointer_lock(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    if (!wl_data.pointer) {
        return SDL_SetError("No wl_pointer (compositor seat has no pointer capability)");
    }
    if (!wl_data.relative_manager || !wl_data.pointer_constraints) {
        return SDL_SetError("Wayland compositor lacks pointer-constraints/relative-pointer support");
    }
    wayland_release_pointer_lock();
    if (wl_data.confined_pointer) {
        /* one constraint per surface: the lock replaces the confinement
         * (SDLOP_UpdateWindowGrab re-confines once relative mode ends) */
        zwp_confined_pointer_v1_destroy(wl_data.confined_pointer);
        wl_data.confined_pointer = NULL;
        wl_data.confine_window = NULL;
    }

    wl_data.relative_pointer =
        zwp_relative_pointer_manager_v1_get_relative_pointer(wl_data.relative_manager, wl_data.pointer);
    zwp_relative_pointer_v1_add_listener(wl_data.relative_pointer, &relative_pointer_listener, &wl_data);

    wl_data.locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
        wl_data.pointer_constraints, wd->surface, wl_data.pointer, NULL,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_locked_pointer_v1_add_listener(wl_data.locked_pointer, &locked_pointer_listener, &wl_data);
    wl_display_flush(wl_data.display);
    wl_data.relative_lock_window = window;
    return true;
}

static bool wayland_SetWindowRelativeMouseMode(SDLop_VideoDevice *device, SDL_Window *window, bool enabled)
{
    (void)device;
    if (enabled) {
        return wayland_arm_pointer_lock(window);
    }
    if (wl_data.relative_lock_window == window) {
        wayland_release_pointer_lock();
        wl_data.relative_lock_window = NULL;
        /* restore the visible cursor */
        if (wl_data.pointer && wl_data.pointer_focus == window) {
            apply_cursor(wl_data.pointer_serial);
        }
    }
    return true;
}

static void wayland_PumpEvents(SDLop_VideoDevice *device, int timeout_ms)
{
    (void)device;
    struct wl_display *display = wl_data.display;
    if (!display) {
        return;
    }

    while (wl_display_prepare_read(display) != 0) {
        wl_display_dispatch_pending(display);
    }
    wl_display_flush(display);

    struct pollfd pfd = { wl_display_get_fd(display), POLLIN, 0 };
    int ret = poll(&pfd, 1, timeout_ms < 0 ? 0 : timeout_ms);
    if (ret > 0 && (pfd.revents & POLLIN)) {
        wl_display_read_events(display);
    } else {
        wl_display_cancel_read(display);
    }
    wl_display_dispatch_pending(display);

    SDLOP_KeyboardProcessRepeats();
}

static int wayland_GetEventFD(SDLop_VideoDevice *device)
{
    (void)device;
    return wl_data.display ? wl_display_get_fd(wl_data.display) : -1;
}

SDLop_VideoDevice SDLop_wayland_device = {
    "wayland",
    wayland_Init,
    wayland_Quit,
    wayland_CreateWindow,
    wayland_DestroyWindow,
    wayland_ShowWindow,
    wayland_HideWindow,
    wayland_SetWindowTitle,
    wayland_SetWindowSize,
    wayland_SetWindowPosition,
    wayland_MinimizeWindow,
    wayland_MaximizeWindow,
    wayland_RestoreWindow,
    wayland_SetWindowFullscreen,
    wayland_RaiseWindow,
    wayland_SetWindowClearColor,
    wayland_SetWindowRelativeMouseMode,
    wayland_PumpEvents,
    wayland_GetEventFD,

    /* software surface */
    wayland_CreateWindowFramebuffer,
    wayland_UpdateWindowFramebuffer,
    wayland_DestroyWindowFramebuffer,

    /* OpenGL (EGL, llvmpipe) - implemented in sdlop_wayland_gl.c */
    SDLOP_Wayland_GL_CreateContext,
    SDLOP_Wayland_GL_MakeCurrent,
    SDLOP_Wayland_GL_SwapBuffers,
    SDLOP_Wayland_GL_DeleteContext,
    SDLOP_Wayland_GL_GetProcAddressThunk,
    SDLOP_Wayland_GL_SetSwapInterval,
    SDLOP_Wayland_GL_GetSwapInterval,

    /* Vulkan (lavapipe) - implemented in sdlop_wayland_vulkan.c */
    SDLOP_Wayland_Vulkan_CreateSurface,
    NULL, /* Vulkan_GetInstanceExtensions: default (VK_KHR_surface + VK_KHR_wayland_surface) */

    /* optional hooks */
    .Vulkan_GetPresentationSupport = SDLOP_Wayland_Vulkan_GetPresentationSupport,
    .supports_popups = true,
    .window_system_input = true,
    .EGL_GetDisplay = SDLOP_Wayland_EGL_GetDisplay,
    .EGL_GetConfig = SDLOP_Wayland_EGL_GetConfig,
    .EGL_GetWindowSurface = SDLOP_Wayland_EGL_GetWindowSurface,
    .SetWindowMinimumSize = wayland_UpdateSizeLimits,
    .SetWindowMaximumSize = wayland_UpdateSizeLimits,
    .SetWindowParent = wayland_SetWindowParent,
    .SetWindowModal = wayland_SetWindowModal,
    .WarpMouse = wayland_WarpMouse,
    .SetWindowHitTest = wayland_SetWindowHitTest,
    .ShowWindowSystemMenu = wayland_ShowWindowSystemMenu,
    .WarpMouseGlobal = wayland_WarpMouseGlobal,
    .SetWindowBordered = wayland_SetWindowBordered,
    .SetWindowResizable = wayland_SetWindowResizable,
    .SetWindowMouseGrab = wayland_SetWindowMouseGrab,
    .SetWindowKeyboardGrab = wayland_SetWindowKeyboardGrab,
    .SetWindowMouseRect = wayland_SetWindowMouseRect,
    .FlashWindow = wayland_FlashWindow,
    .SyncWindow = wayland_SyncWindow,
    .SuspendScreenSaver = wayland_SuspendScreenSaver,
    .CreateCursor = wayland_CreateCursor,
    .FreeCursor = wayland_FreeCursor,
    .ShowCursor = wayland_ShowCursor,
};
