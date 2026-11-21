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
#include "../kmsdrm/sdlop_xcursor.h"

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

typedef struct WaylandWindowData
{
    struct wl_surface *surface;
    struct xdg_surface *xsurface;
    struct xdg_toplevel *toplevel;
    struct xdg_popup *popup;      /* SDL_WINDOW_TOOLTIP / SDL_WINDOW_POPUP_MENU */

    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    int shm_fd;
    size_t shm_size;
    void *shm_map;
    int buffer_w, buffer_h;

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
    float axis_x_acc, axis_y_acc;

    /* relative mouse mode (pointer lock) */
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_relative_pointer_manager_v1 *relative_manager;
    struct zwp_locked_pointer_v1 *locked_pointer;
    struct zwp_relative_pointer_v1 *relative_pointer;
    SDL_Window *relative_lock_window;
    float rel_x_acc, rel_y_acc;

    /* SDL 3.4 window management / cursors */
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct zwp_idle_inhibit_manager_v1 *idle_inhibit_manager;
    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *shortcuts_inhibit_manager;
    struct xdg_activation_v1 *activation;
    struct zwp_confined_pointer_v1 *confined_pointer;
    SDL_Window *confine_window;
    uint32_t pointer_serial;      /* last wl_pointer.enter serial (set_cursor) */
    uint32_t input_serial;        /* last button/key serial (xdg_popup grab) */
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

static void window_destroy_buffer(WaylandWindowData *wd)
{
    if (wd->buffer) {
        wl_buffer_destroy(wd->buffer);
        wd->buffer = NULL;
    }
    if (wd->pool) {
        wl_shm_pool_destroy(wd->pool);
        wd->pool = NULL;
    }
    if (wd->shm_map && wd->shm_map != MAP_FAILED) {
        munmap(wd->shm_map, wd->shm_size);
        wd->shm_map = NULL;
    }
    if (wd->shm_fd >= 0) {
        close(wd->shm_fd);
        wd->shm_fd = -1;
    }
    wd->shm_size = 0;
}

static bool window_create_buffer(SDL_Window *window, int w, int h)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd) {
        return false;
    }
    window_destroy_buffer(wd);

    size_t stride = (size_t)w * 4;
    size_t size = stride * (size_t)h;

    int fd = memfd_create("sdlop-shm", MFD_CLOEXEC);
    if (fd < 0) {
        return SDL_SetError("memfd_create failed: %s", strerror(errno));
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return SDL_SetError("ftruncate failed: %s", strerror(errno));
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return SDL_SetError("mmap failed: %s", strerror(errno));
    }

    /* clear color (XRGB8888) */
    uint32_t color = ((uint32_t)window->clear_r << 16) | ((uint32_t)window->clear_g << 8) | window->clear_b;
    uint32_t *pix = (uint32_t *)map;
    size_t npix = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npix; i++) {
        pix[i] = color;
    }

    wd->pool = wl_shm_create_pool(wl_data.shm, fd, (int32_t)size);
    wd->buffer = wl_shm_pool_create_buffer(wd->pool, 0, w, h, (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
    wd->shm_fd = fd;
    wd->shm_map = map;
    wd->shm_size = size;
    wd->buffer_w = w;
    wd->buffer_h = h;

    /* keep an existing window surface in sync with the new buffer
     * (SDL3 semantics: the surface survives resizes) */
    SDLOP_Wayland_GL_WindowResized(window);
    if (window->surface) {
        window->surface->w = w;
        window->surface->h = h;
        window->surface->pitch = (int)stride;
        window->surface->pixels = map;
    }
    return true;
}

static void window_commit(SDL_Window *window)
{
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd || !wd->configured || !wd->buffer) {
        return;
    }
    if (wd->gl_active && !wd->gl_offload_active) {
        return; /* native Wayland EGL owns buffer management for this window */
    }
    wl_surface_attach(wd->surface, wd->buffer, 0, 0);
    wl_surface_damage_buffer(wd->surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(wd->surface);
    wd->mapped = true;
}

/* ------------------------------------------------------------------ */
/* software window surface (zero copy: pixels ARE the shm buffer)      */
/* ------------------------------------------------------------------ */

static bool wayland_CreateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface **surface)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    if (!wd->buffer) {
        if (!window_create_buffer(window, window->w, window->h)) {
            return false;
        }
    }
    SDL_Surface *s = SDL_CreateSurfaceFrom(window->w, window->h, SDL_PIXELFORMAT_XRGB8888, wd->shm_map, window->w * 4);
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
    wl_surface_attach(wd->surface, wd->buffer, 0, 0);
    if (rects && numrects > 0) {
        for (int i = 0; i < numrects; i++) {
            wl_surface_damage_buffer(wd->surface, rects[i].x, rects[i].y, rects[i].w, rects[i].h);
        }
    } else {
        wl_surface_damage_buffer(wd->surface, 0, 0, wd->buffer_w, wd->buffer_h);
    }
    wl_surface_commit(wd->surface);
    wl_display_flush(wl_data.display);
    wd->mapped = true;
    return true;
}

static void wayland_DestroyWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    /* the surface wraps the shm buffer; free the SDL_Surface struct only */
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* internals shared with the GL/Vulkan modules                         */
/* ------------------------------------------------------------------ */

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

bool SDLOP_Wayland_AcquireOffloadShm(SDL_Window *window, void **pixels_out, int *pitch_out, int *w_out, int *h_out)
{
    WaylandWindowData *wd = window ? (WaylandWindowData *)window->driverdata : NULL;
    if (!wd) {
        return SDL_SetError("Invalid window");
    }
    int w = window->w > 0 ? window->w : 1;
    int h = window->h > 0 ? window->h : 1;
    if (!wd->buffer || wd->buffer_w != w || wd->buffer_h != h) {
        if (!window_create_buffer(window, w, h)) {
            return false;
        }
    }
    if (pixels_out) {
        *pixels_out = wd->shm_map;
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
    if (!wd || !wd->buffer) {
        return SDL_SetError("Window has no shm buffer");
    }
    if (!wd->configured) {
        /* xsurface_configure will commit once the configure handshake arrives */
        return true;
    }
    wl_surface_attach(wd->surface, wd->buffer, 0, 0);
    wl_surface_damage_buffer(wd->surface, 0, 0, wd->buffer_w, wd->buffer_h);
    wl_surface_commit(wd->surface);
    wl_display_flush(wl_data.display);
    wd->mapped = true;
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
        window_create_buffer(window, window->w, window->h);
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, window->w, window->h);
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
    if (!wd->buffer) {
        window_create_buffer(window, window->w, window->h);
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
    (void)keys;
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)d;
    SDL_Window *window = NULL;
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        WaylandWindowData *wd = (WaylandWindowData *)w->driverdata;
        if (wd && wd->surface == surface) {
            window = w;
            break;
        }
    }
    (void)serial;
    if (window) {
        if (sdlop.keyboard_focus && sdlop.keyboard_focus != window) {
            sdlop.keyboard_focus->flags &= ~SDL_WINDOW_INPUT_FOCUS;
            SDLOP_SendWindowEvent(sdlop.keyboard_focus, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
        }
        sdlop.keyboard_focus = window;
        window->flags |= SDL_WINDOW_INPUT_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_FOCUS_GAINED, 0, 0);
    }
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
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)data;
    (void)keyboard;
    (void)time;
    wl_data.input_serial = serial;

    /* Desktop keyboards use the focus-aware seat stream even with a readable
     * evdev device. Only console keyboard ownership enables raw SDL keys. */
    if (SDLOP_RawInputKeyboardActive()) {
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
    if (rate > 0 && delay > 0) {
        SDLOP_RawInputSetRepeatInfo((Uint32)delay, (Uint32)(1000 / rate));
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
} WaylandCursor;

/* premultiplied ARGB8888 wl_shm buffer (the pool is released right away;
 * the buffer keeps the memory alive) */
static struct wl_buffer *create_shm_cursor_buffer(const uint32_t *argb, int w, int h, int pitch_px)
{
    if (!wl_data.shm) {
        return NULL;
    }
    const size_t size = (size_t)w * (size_t)h * 4;
    int fd = memfd_create("sdlop-cursor", MFD_CLOEXEC);
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
    uint32_t *pix = (uint32_t *)map;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint32_t p = argb[(size_t)y * (size_t)pitch_px + (size_t)x];
            const uint32_t a = p >> 24;
            pix[y * w + x] = (a << 24) | ((((p >> 16) & 0xff) * a / 255) << 16) |
                             ((((p >> 8) & 0xff) * a / 255) << 8) | ((p & 0xff) * a / 255);
        }
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_data.shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(map, size);
    close(fd);
    return buffer;
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

static bool wayland_CreateCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    WaylandCursor *wc = (WaylandCursor *)calloc(1, sizeof(*wc));
    if (!wc) {
        return SDL_OutOfMemory();
    }
    if (cursor->surface) {
        SDL_Surface *s = cursor->surface; /* ARGB8888 */
        wc->buffer = create_shm_cursor_buffer((const uint32_t *)s->pixels, s->w, s->h, s->pitch / 4);
        wc->w = s->w;
        wc->h = s->h;
        wc->hot_x = cursor->hot_x;
        wc->hot_y = cursor->hot_y;
    } else {
        /* themed (CSS name, fallback, legacy X name - like SDL3's
         * wayland-cursor path); the built-in arrow when nothing matches */
        struct sdlop_xcursor_theme *theme = wayland_cursor_theme();
        if (theme) {
            const char *fallback = NULL;
            const char *names[3] = { SDLOP_GetCSSCursorName(cursor->system_id, &fallback), NULL, NULL };
            names[1] = fallback;
            names[2] = cursor->system_id == SDL_SYSTEM_CURSOR_DEFAULT ? "left_ptr" : NULL;
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
    if (!wc->buffer) {
        free(wc);
        return SDL_SetError("Couldn't create Wayland cursor buffer");
    }
    cursor->internal = wc;
    return true;
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
    if (!wc || !wc->buffer || (wl_data.locked_pointer && wl_data.relative_lock_window == wl_data.pointer_focus)) {
        wl_pointer_set_cursor(wl_data.pointer, serial, NULL, 0, 0);
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

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    SDL_Window *window = window_from_surface(surface);
    if (!window) {
        return;
    }
    d->pointer_serial = serial;
    d->pointer_focus = window;
    sdlop.mouse_focus = window;
    window->flags |= SDL_WINDOW_MOUSE_FOCUS;
    sdlop.mouse_has_position = false; /* stock: the enter's own motion carries no delta */
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_ENTER, 0, 0);
    /* stock delivers the enter position through the motion path (a movement
       event with zero delta); a confine warp may arrive with no following
       motion, so the enter coordinates are the position. */
    SDLOP_SendMouseMotion((float)wl_fixed_to_double(surface_x),
                          (float)wl_fixed_to_double(surface_y), 0.0f, 0.0f,
                          SDLOP_MonotonicNS());

    if (sdlop.relative_mode_window == window) {
        if (!wl_data.locked_pointer) {
            wayland_arm_pointer_lock(window); /* re-arm after policy unlock */
        } else {
            wl_pointer_set_cursor(pointer, serial, NULL, 0, 0); /* hide */
        }
    } else {
        apply_cursor(serial);
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

    if (SDLOP_RawInputMouseActive()) {
        if (sdlop.relative_mode_window == window) {
            return; /* relative mode: evdev owns motion entirely */
        }
        /* re-anchor accumulated position to compositor truth (no event:
         * evdev already emitted the low-latency motion event) */
        sdlop.mouse_x = x;
        sdlop.mouse_y = y;
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
    wl_data.input_serial = serial;
    if (SDLOP_RawInputMouseActive()) {
        return; /* evdev owns buttons */
    }
    Uint8 btn = wl_button_to_sdl(button);
    if (btn) {
        SDLOP_SendMouseButton(state == WL_POINTER_BUTTON_STATE_PRESSED, btn, SDLOP_NO_POS, SDLOP_NO_POS, SDLOP_MonotonicNS());
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)pointer;
    (void)time;
    if (SDLOP_RawInputMouseActive()) {
        return;
    }
    float v = (float)wl_fixed_to_double(value) / 10.0f; /* weston: 10 units per detent */
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        d->axis_y_acc -= v;
    } else {
        d->axis_x_acc += v;
    }
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)pointer;
    if (d->axis_x_acc != 0.0f || d->axis_y_acc != 0.0f) {
        SDLOP_SendMouseWheel(d->axis_x_acc, d->axis_y_acc, SDLOP_MonotonicNS());
        d->axis_x_acc = d->axis_y_acc = 0.0f;
    }
    /* relative mouse mode: flush accumulated relative-pointer motion
     * (skipped when the raw evdev worker owns the mouse) */
    if (d->relative_pointer && (d->rel_x_acc != 0.0f || d->rel_y_acc != 0.0f)) {
        if (!SDLOP_RawInputMouseActive()) {
            SDLOP_SendMouseMotion(SDLOP_NO_POS, SDLOP_NO_POS, d->rel_x_acc, d->rel_y_acc, SDLOP_MonotonicNS());
        }
        d->rel_x_acc = d->rel_y_acc = 0.0f;
    }
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
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;
    (void)pointer;
    if (SDLOP_RawInputMouseActive()) {
        return;
    }
    float v = (float)value120 / 120.0f;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        d->axis_y_acc += v;
    } else {
        d->axis_x_acc += v;
    }
}

static void pointer_axis_relative_direction(void *data, struct wl_pointer *pointer, uint32_t axis, uint32_t direction)
{
    (void)data;
    (void)pointer;
    (void)axis;
    (void)direction;
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
    float s = wayland_output_scale(output);
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        WaylandWindowData *wd = (WaylandWindowData *)w->driverdata;
        if (wd && wd->output == output && w->display_scale != s) {
            w->display_scale = s;
            SDLOP_SendWindowEvent(w, SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED, 0, 0);
        }
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
    float s = wayland_output_scale(output);
    if (window->display_scale != s) {
        window->display_scale = s;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED, 0, 0);
    }
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    SDL_Window *window = (SDL_Window *)data;
    WaylandWindowData *wd = (WaylandWindowData *)window->driverdata;
    (void)surface;
    (void)output;
    wd->output = NULL; /* scale stays until the next enter (SDL3 behavior) */
}

static void surface_preferred_buffer_scale(void *data, struct wl_surface *surface, int32_t factor)
{
    (void)data; (void)surface; (void)factor;
}

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
        d->pointer_id = sdlop_device_next_id();
        send_mouse_device_event(true, d->pointer_id);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
        wl_pointer_destroy(d->pointer);
        d->pointer = NULL;
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
        d->compositor_version = version < 4 ? version : 4;
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
            WaylandWindowData *wd = (WaylandWindowData *)w->driverdata;
            if (wd && wd->output == d->outputs[i].output) {
                wd->output = NULL; /* keeps its last scale until re-enter */
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
        wl_display_disconnect(wl_data.display);
        wl_data.display = NULL;
        return SDL_SetError("Wayland registry roundtrip failed");
    }
    if (!wl_data.compositor || !wl_data.wm_base || !wl_data.shm) {
        wl_display_disconnect(wl_data.display);
        wl_data.display = NULL;
        return SDL_SetError("Wayland compositor missing required globals (wl_compositor/xdg_wm_base/wl_shm)");
    }
    /* second roundtrip so seat capabilities (keyboard/pointer) arrive */
    wl_display_roundtrip(wl_data.display);
    return true;
}

static void wayland_Quit(SDLop_VideoDevice *device)
{
    (void)device;
    SDLOP_Wayland_GL_Deinit();
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

static void destroy_role_objects(WaylandWindowData *wd)
{
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

static bool wayland_create_role_objects(SDL_Window *window)
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
    if (wl_data.decoration_manager) {
        wd->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(wl_data.decoration_manager, wd->toplevel);
        zxdg_toplevel_decoration_v1_set_mode(wd->decoration, (window->flags & SDL_WINDOW_BORDERLESS)
                                                                 ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                                                                 : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    apply_size_limits(window);
    if (window->parent && window->parent->driverdata &&
        ((WaylandWindowData *)window->parent->driverdata)->toplevel) {
        xdg_toplevel_set_parent(wd->toplevel, ((WaylandWindowData *)window->parent->driverdata)->toplevel);
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

static bool wayland_CreateWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    WaylandWindowData *wd = (WaylandWindowData *)calloc(1, sizeof(*wd));
    if (!wd) {
        return SDL_OutOfMemory();
    }
    wd->shm_fd = -1;

    wd->surface = wl_compositor_create_surface(wl_data.compositor);
    if (!wd->surface) {
        free(wd);
        return SDL_SetError("wl_compositor_create_surface failed");
    }
    wl_surface_add_listener(wd->surface, &surface_listener, window);
    window->driverdata = wd;

    if (!wayland_create_role_objects(window)) {
        wl_surface_destroy(wd->surface);
        window->driverdata = NULL;
        free(wd);
        return false;
    }
    if (sdlop.suspend_screensaver && wl_data.idle_inhibit_manager) {
        wd->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(wl_data.idle_inhibit_manager, wd->surface);
    }
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
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
    destroy_role_objects(wd);
    if (wd->surface) {
        wl_surface_destroy(wd->surface);
    }
    window_destroy_buffer(wd);
    free(wd);
    window->driverdata = NULL;
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
    window_create_buffer(window, window->w, window->h);
    apply_size_limits(window);
    window_commit(window);
    wl_display_flush(wl_data.display);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, window->w, window->h);
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
    if (!wd || !wd->shm_map) {
        return;
    }
    uint32_t color = ((uint32_t)window->clear_r << 16) | ((uint32_t)window->clear_g << 8) | window->clear_b;
    uint32_t *pix = (uint32_t *)wd->shm_map;
    size_t npix = (size_t)wd->buffer_w * (size_t)wd->buffer_h;
    for (size_t i = 0; i < npix; i++) {
        pix[i] = color;
    }
    window_commit(window);
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
    .EGL_GetDisplay = SDLOP_Wayland_EGL_GetDisplay,
    .EGL_GetConfig = SDLOP_Wayland_EGL_GetConfig,
    .EGL_GetWindowSurface = SDLOP_Wayland_EGL_GetWindowSurface,
    .SetWindowMinimumSize = wayland_UpdateSizeLimits,
    .SetWindowMaximumSize = wayland_UpdateSizeLimits,
    .SetWindowParent = wayland_SetWindowParent,
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
