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

#include "pointer-constraints-client-protocol.h"
#include "relative-pointer-client-protocol.h"

#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

typedef struct WaylandWindowData
{
    struct wl_surface *surface;
    struct xdg_surface *xsurface;
    struct xdg_toplevel *toplevel;

    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    int shm_fd;
    size_t shm_size;
    void *shm_map;
    int buffer_w, buffer_h;

    bool configured;
    bool mapped;
    uint32_t toplevel_states;
    int pending_w, pending_h;

    /* GL presentation state (managed by sdlop_wayland_gl.c) */
    void *gl_surface; /* EGLSurface */
    void *gl_egl_window; /* wl_egl_window backing the EGLSurface */
    bool gl_active;
} WaylandWindowData;

typedef struct WaylandDeviceData
{
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct xdg_wm_base *wm_base;
    uint32_t compositor_version;
    uint32_t seat_version;

    SDL_Window *pointer_focus;
    float axis_x_acc, axis_y_acc;

    /* relative mouse mode (pointer lock) */
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_relative_pointer_manager_v1 *relative_manager;
    struct zwp_locked_pointer_v1 *locked_pointer;
    struct zwp_relative_pointer_v1 *relative_pointer;
    SDL_Window *relative_lock_window;
    float rel_x_acc, rel_y_acc;
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
    if (wd->gl_active) {
        return; /* EGL owns buffer management for this window now */
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

    bool first = !wd->configured;
    wd->configured = true;

    if (wd->pending_w > 0 && wd->pending_h > 0 &&
        (wd->pending_w != window->w || wd->pending_h != window->h)) {
        window->w = wd->pending_w;
        window->h = wd->pending_h;
        window_create_buffer(window, window->w, window->h);
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, window->w, window->h);
    }
    if (!wd->buffer) {
        window_create_buffer(window, window->w, window->h);
    }
    if (!(window->flags & SDL_WINDOW_HIDDEN)) {
        window_commit(window);
    }
    (void)first;
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
    (void)serial;
    (void)time;

    /* evdev worker covers the keyboard: ignore seat events to avoid
     * double-delivery (this is the "wayland fallback" policy) */
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
    (void)group;
    SDLOP_KeyboardUpdateXkbModifiers(mods_depressed, mods_latched, mods_locked);
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
static struct wl_buffer *cursor_buffer;

static void ensure_cursor(void)
{
    if (cursor_buffer || !wl_data.compositor || !wl_data.shm) {
        return;
    }
    const size_t size = CURSOR_W * CURSOR_H * 4;
    int fd = memfd_create("sdlop-cursor", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        if (fd >= 0) {
            close(fd);
        }
        return;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    uint32_t *pix = (uint32_t *)map;
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            char c = arrow_rows[y][x];
            if (c == 'X') {
                pix[y * CURSOR_W + x] = 0xFF000000;
            } else if (c == 'x') {
                pix[y * CURSOR_W + x] = 0xFFFFFFFF;
            } else {
                pix[y * CURSOR_W + x] = 0x00000000;
            }
        }
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_data.shm, fd, (int32_t)size);
    cursor_buffer = wl_shm_pool_create_buffer(pool, 0, CURSOR_W, CURSOR_H, CURSOR_W * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(map, size);
    close(fd);
    cursor_surface = wl_compositor_create_surface(wl_data.compositor);
}

static void set_default_cursor(struct wl_pointer *pointer, uint32_t serial)
{
    ensure_cursor();
    if (!cursor_surface || !cursor_buffer) {
        return;
    }
    wl_surface_attach(cursor_surface, cursor_buffer, 0, 0);
    wl_surface_damage_buffer(cursor_surface, 0, 0, CURSOR_W, CURSOR_H);
    wl_surface_commit(cursor_surface);
    wl_pointer_set_cursor(pointer, serial, cursor_surface, 0, 0);
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
    (void)serial;
    SDL_Window *window = window_from_surface(surface);
    if (!window) {
        return;
    }
    d->pointer_focus = window;
    sdlop.mouse_focus = window;
    window->flags |= SDL_WINDOW_MOUSE_FOCUS;
    sdlop.mouse_x = (float)wl_fixed_to_double(surface_x);
    sdlop.mouse_y = (float)wl_fixed_to_double(surface_y);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_ENTER, 0, 0);

    if (sdlop.relative_mode_window == window) {
        if (!wl_data.locked_pointer) {
            wayland_arm_pointer_lock(window); /* re-arm after policy unlock */
        } else {
            wl_pointer_set_cursor(pointer, serial, NULL, 0, 0); /* hide */
        }
    } else {
        set_default_cursor(pointer, serial);
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
    SDLOP_SendMouseMotion(x, y, x - sdlop.mouse_x, y - sdlop.mouse_y, SDLOP_MonotonicNS());
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
    (void)serial;
    (void)time;
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
/* wl_seat                                                             */
/* ------------------------------------------------------------------ */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    WaylandDeviceData *d = (WaylandDeviceData *)data;

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
        d->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
        wl_keyboard_destroy(d->keyboard);
        d->keyboard = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
        d->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(d->pointer, &pointer_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
        wl_pointer_destroy(d->pointer);
        d->pointer = NULL;
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
    } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0 && !d->pointer_constraints) {
        d->pointer_constraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0 && !d->relative_manager) {
        d->relative_manager = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
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
    if (!display_name && !getenv("SDL_VIDEODRIVER")) {
        /* don't fail hard when probing: no wayland session */
        return SDL_SetError("WAYLAND_DISPLAY not set");
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
    wayland_release_pointer_lock();
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
    if (wl_data.seat) {
        wl_seat_destroy(wl_data.seat);
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
    wd->xsurface = xdg_wm_base_get_xdg_surface(wl_data.wm_base, wd->surface);
    if (!wd->xsurface) {
        wl_surface_destroy(wd->surface);
        free(wd);
        return SDL_SetError("xdg_wm_base_get_xdg_surface failed");
    }
    xdg_surface_add_listener(wd->xsurface, &xsurface_listener, window);
    wd->toplevel = xdg_surface_get_toplevel(wd->xsurface);
    if (!wd->toplevel) {
        xdg_surface_destroy(wd->xsurface);
        wl_surface_destroy(wd->surface);
        free(wd);
        return SDL_SetError("xdg_surface_get_toplevel failed");
    }
    xdg_toplevel_add_listener(wd->toplevel, &toplevel_listener, window);
    xdg_toplevel_set_title(wd->toplevel, window->title ? window->title : "SDLop");
    xdg_toplevel_set_app_id(wd->toplevel, "SDLop");
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        xdg_toplevel_set_fullscreen(wd->toplevel, NULL);
    }
    if (window->flags & SDL_WINDOW_MAXIMIZED) {
        xdg_toplevel_set_maximized(wd->toplevel);
    }

    window->driverdata = wd;

    /* kick off the configure sequence */
    wl_surface_commit(wd->surface);
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
    SDLOP_Wayland_GL_WindowDestroyed(window);
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
    if (wd->toplevel) {
        xdg_toplevel_destroy(wd->toplevel);
    }
    if (wd->xsurface) {
        xdg_surface_destroy(wd->xsurface);
    }
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
    if (!wd || !wd->configured) {
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
    /* unmap by detaching the buffer */
    wl_surface_attach(wd->surface, NULL, 0, 0);
    wl_surface_commit(wd->surface);
    wl_display_flush(wl_data.display);
    wd->mapped = false;
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
    window_commit(window);
    wl_display_flush(wl_data.display);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, window->w, window->h);
    return true;
}

static bool wayland_SetWindowPosition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
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

static void wayland_RaiseWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    /* Wayland has no raise; focus follows the compositor's policy */
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
            set_default_cursor(wl_data.pointer, 0);
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
};
