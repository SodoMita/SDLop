/*
  SDLop -- the Wayland backend.

  What this backend is:
    * a Wayland client written against the stable core protocol plus xdg-shell
      (generated at build time by wayland-scanner into src/generated/);
    * software presentation through wl_shm, so a window shows pixels with no GL
      driver, no Mesa and no GPU at all - which is also what makes it testable in
      a container;
    * a real Vulkan surface (VkWaylandSurfaceCreateInfoKHR) and, when EGL is
      available, an EGL/GLES window through wl_egl_window;
    * input forwarding through wl_seat when the async evdev worker is not running
      (XWayland, flatpak, no /dev/input access). When the worker *is* running the
      pointer/keyboard listeners stay quiet, so an application never sees the
      same key twice.

  Deliberate omissions (documented in docs/ROADMAP.md): wl_drm/dmabuf, libdecor
  decorations (windows are therefore "CSD-less": the compositor draws its own
  titlebar or none), and pointer-confinement based relative mode.
*/

#include "../sdlop_internal.h"

#include <wayland-client.h>
#include <wayland-egl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/memfd.h>

#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-output-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "relative-pointer-client-protocol.h"
#include "pointer-constraints-client-protocol.h"

#ifdef SDLOP_HAVE_XKBCOMMON
#include <xkbcommon/xkbcommon.h>
#endif


#define SDLOP_WaylandBuffer SDLOP_WaylandBufferSlot

/* The window struct keeps the Wayland objects as opaque pointers (sdlop
   internal header declares the types), so the fields are used directly. */
/* The highest wl_output version this backend implements (name/description plus
   the v3 release request). */
#define SDLOP_WL_OUTPUT_VERSION 4

#define WL_SURFACE(w)  ((w)->driver.wayland.wl_surface)
#define XDG_SURFACE(w) ((w)->driver.wayland.xdg_surface)
#define XDG_TOPLEVEL(w) ((w)->driver.wayland.xdg_toplevel)
#define XDG_POPUP(w)   ((w)->driver.wayland.xdg_popup)
#define WL_VIEWPORT(w) ((w)->driver.wayland.viewport)
#define WL_LOCKED_POINTER(w)   ((w)->driver.wayland.locked_pointer)
#define WL_CONFINED_POINTER(w) ((w)->driver.wayland.confined_pointer)
#define WL_OUTPUTS(w)          ((w)->driver.wayland.outputs)
#define WL_NUM_OUTPUTS(w)      ((w)->driver.wayland.num_outputs)
#define WL_MAX_OUTPUTS         ((int)SDL_arraysize(((SDL_Window *)0)->driver.wayland.outputs))

typedef struct SDLOP_WaylandOutput
{
    struct wl_output *output;
    struct zxdg_output_v1 *xdg_output;
    uint32_t registry_name;          /* for registry_global_remove */
    SDL_DisplayID id;
    int scale;
    int refresh;                     /* mHz */
    int x, y;                        /* logical position (zxdg_output_v1) */
    int logical_w, logical_h;        /* logical size, which is what SDL reports */
    int pixel_w, pixel_h;            /* physical size, from the current mode */
    uint32_t transform;
    uint32_t version;                /* bound wl_output version */
    int done_count;                  /* wl_output.done events seen since the last apply */
    char model[128];                 /* wl_output.geometry() model, the weakest name */
    char connector[64];              /* wl_output.name(), e.g. "DP-3" */
    int fallback_x;                  /* left-to-right slot, no-xdg_output case */
    char description[128];           /* wl_output.description(): the name SDL3 prefers */
    char xdg_description[128];       /* zxdg_output_v1 description (only used pre-v4) */
    bool named;                      /* the display has its final name already */
    bool have_logical;
    bool have_mode;
} SDLOP_WaylandOutput;

static struct wl_display *sdlop_wl_display;
static struct wl_registry *sdlop_wl_registry;
static struct wl_compositor *sdlop_wl_compositor;
static struct wl_shm *sdlop_wl_shm;
static struct xdg_wm_base *sdlop_wl_wm_base;
static struct wp_viewporter *sdlop_wl_viewporter;
static struct zxdg_output_manager_v1 *sdlop_wl_xdg_output_manager;
static struct wp_cursor_shape_manager_v1 *sdlop_wl_cursor_shape_manager;
static struct wl_seat *sdlop_wl_seat;
static struct wl_pointer *sdlop_wl_pointer;
static struct zwp_relative_pointer_manager_v1 *sdlop_wl_relative_pointer_manager;
static struct zwp_relative_pointer_v1 *sdlop_wl_relative_pointer;
static struct zwp_pointer_constraints_v1 *sdlop_wl_pointer_constraints;
static struct wl_keyboard *sdlop_wl_keyboard;
static struct wp_cursor_shape_device_v1 *sdlop_wl_cursor_shape_device;
static struct wl_surface *sdlop_wl_cursor_surface;

static SDLOP_WaylandOutput sdlop_wl_outputs[8];
static int sdlop_wl_num_outputs;      /* highest slot in use plus one */
static int sdlop_wl_next_output_x;
/* Display IDs are handed out once and never reused: a window that outlives an
   unplugged monitor must not end up pointing at a different display. */
static SDL_DisplayID sdlop_wl_next_display_id = 1;

/* Output slots are never compacted: the compositor holds a pointer to the slot
   as the listener data of its wl_output, so entries have to stay put. */
static SDLOP_WaylandOutput *sdlop_wl_free_output_slot(void)
{
    const int capacity = (int)(sizeof(sdlop_wl_outputs) / sizeof(sdlop_wl_outputs[0]));
    int i;

    for (i = 0; i < sdlop_wl_num_outputs; i++) {
        if (!sdlop_wl_outputs[i].output) {
            return &sdlop_wl_outputs[i];
        }
    }
    if (sdlop_wl_num_outputs < capacity) {
        return &sdlop_wl_outputs[sdlop_wl_num_outputs++];
    }
    return NULL;
}
static bool sdlop_wl_reading;
static bool sdlop_wl_has_keyboard;
static bool sdlop_wl_has_pointer;
static SDL_Window *sdlop_wl_keyboard_focus;
static SDL_Window *sdlop_wl_pointer_focus;
static SDL_Cursor *sdlop_wl_current_cursor;
static bool sdlop_wl_cursor_visible = true;
/* Key repeat: Wayland compositors never send repeats, the client synthesizes
   them from the rate/delay wl_keyboard.repeat_info() announced. */
static int sdlop_wl_repeat_rate;                 /* keys per second, 0 = off */
static int sdlop_wl_repeat_delay;                /* ms before the first repeat */
static Uint32 sdlop_wl_repeat_key;               /* evdev keycode being repeated */
static Uint64 sdlop_wl_repeat_deadline;          /* when the next repeat is due */
static float sdlop_wl_pointer_x, sdlop_wl_pointer_y;

#ifdef SDLOP_HAVE_XKBCOMMON
#endif

/* ------------------------------------------------------------------------- */
/* Displays                                                                  */
/* ------------------------------------------------------------------------- */

static void sdlop_wl_apply_output(SDLOP_WaylandOutput *out);

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                            int32_t physical_width, int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model, int32_t transform)
{
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)data;

    (void)output; (void)physical_width; (void)physical_height; (void)subpixel;
    (void)make; (void)x; (void)y;
    out->transform = (uint32_t)transform;
    /* Only remembered: the display name is chosen once, by
       sdlop_wl_apply_output(), from the description the compositor sent. */
    if (model && model[0]) {
        SDL_strlcpy(out->model, model, sizeof(out->model));
    }
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width,
                        int32_t height, int32_t refresh)
{
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)data;
    struct SDLOP_Display *display = SDLOP_GetDisplay(out->id);

    (void)output;
    out->refresh = refresh;
    if (!display) {
        return;
    }
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        /* The mode size is the *physical* size of the output: a 1024x768@2
           output is 512x384 in the global compositing space, and that is what
           SDL reports. sdlop_wl_apply_output() turns it into a display mode
           once the whole burst (mode + scale + zxdg_output_v1) has arrived. */
        out->pixel_w = width;
        out->pixel_h = height;
        out->have_mode = true;
        if (!out->have_logical) {
            out->logical_w = width;
            out->logical_h = height;
        }
        if (out->version < 2) {
            /* wl_output.done only exists from version 2 on: a v1 compositor
               never signals the end of the burst, so do it here. */
            sdlop_wl_apply_output(out);
        }
    }
    if (flags & WL_OUTPUT_MODE_PREFERRED) {
        /* Every mode a compositor reports for a headless/single-mode output is
           the current one; SDL only needs the current mode plus the native
           fullscreen mode, which sdlop_wl_apply_output() derives. */
    }
}

/* wl_output.done marks the end of a batch of output events. It is *the*
   completion signal: with an zxdg_output_v1 attached the compositor sends two
   of them (one for the wl_output events, one for the xdg_output events), and
   zxdg_output_v1.done itself is deprecated from manager version 3 on. */
static void output_done(void *data, struct wl_output *output)
{
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)data;
    const int event_await_count = 1 + (out->xdg_output != NULL);

    (void)output;
    if (out->done_count < event_await_count + 1) {
        out->done_count++;
    }
    if (out->done_count < event_await_count) {
        return;
    }
    sdlop_wl_apply_output(out);
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)data;
    struct SDLOP_Display *display = SDLOP_GetDisplay(out->id);

    (void)output;
    out->scale = factor > 0 ? factor : 1;
    if (display) {
        SDLOP_SetDisplayScale(display, (float)out->scale);
    }
}

/* wl_output.name() is the connector name ("DP-3"); SDL3 keeps it for output
   matching but does not use it as the display name, so it is only a fallback
   here (for a compositor that offers no description at all). */
static void output_name(void *data, struct wl_output *output, const char *name)
{
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)data;
    (void)output;
    if (name && name[0]) {
        SDL_strlcpy(out->connector, name, sizeof(out->connector));
    }
}

static void output_description(void *data, struct wl_output *output, const char *description)
{
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)data;
    (void)output;
    if (description && description[0]) {
        SDL_strlcpy(out->description, description, sizeof(out->description));
    }
}

/* Turn a completed burst of output events into a display.
 *
 * "Logical" is what SDL reports: a 1024x768 output at scale 2 is a 512x384
 * display at 800,0, and that position only comes from zxdg_output_v1 (a
 * wl_output has no coordinates of its own). The scale factor of the output is
 * the ratio between the two, which is why both sizes are needed before the
 * geometry can be applied - the mode arrives in one batch, the zxdg_output_v1
 * sizes in another, and wl_output.done closes each of them.
 *
 * The rules follow SDL3: without wp_viewporter the compositor can only scale by
 * whole factors, so the native size is a multiple of the logical one. */
static void sdlop_wl_apply_output(SDLOP_WaylandOutput *out)
{
    struct SDLOP_Display *display = SDLOP_GetDisplay(out->id);
    SDL_DisplayMode native_mode, logical_mode;
    int native_w, native_h, logical_w, logical_h;
    const int wl_scale = out->scale > 0 ? out->scale : 1;
    float factor = (float)wl_scale;

    if (!display || !out->have_mode) {
        return;
    }

    if (out->transform & WL_OUTPUT_TRANSFORM_90) {
        native_w = out->pixel_h;
        native_h = out->pixel_w;
    } else {
        native_w = out->pixel_w;
        native_h = out->pixel_h;
    }

    if (out->have_logical) {
        logical_w = out->logical_w;
        logical_h = out->logical_h;
        if (native_w != logical_w || native_h != logical_h) {
            if (sdlop_wl_viewporter) {
                /* The compositor scales the logical viewport: the ratio *is* the
                   scale factor (fractional scale factors included). */
                factor = logical_w > 0 ? (float)native_w / (float)logical_w : (float)wl_scale;
            } else {
                /* The native size is a multiple of the logical one. */
                native_w = logical_w * wl_scale;
                native_h = logical_h * wl_scale;
                factor = (float)wl_scale;
            }
        } else if (wl_scale > 1) {
            /* The output is not scaled in the global compositing space. */
            logical_w /= wl_scale;
            logical_h /= wl_scale;
        }
    } else {
        logical_w = native_w / wl_scale;
        logical_h = native_h / wl_scale;
        out->x = out->fallback_x;   /* no zxdg_output_v1: our own layout */
    }

    /* The name is chosen once, on the first complete burst, and never rewritten:
       a later event must not be able to rename a display the application already
       has an ID for. The order follows SDL3 - the compositor's description beats
       the model string from wl_output.geometry(), which is "Unknown" on a
       headless or virtual output. */
    if (!out->named) {
        char fallback[64];
        const char *best = NULL;

        if (out->description[0]) {
            best = out->description;
        } else if (out->xdg_description[0]) {
            best = out->xdg_description;
        } else if (out->model[0] && strcmp(out->model, "Unknown") != 0) {
            best = out->model;
        } else if (out->connector[0]) {
            best = out->connector;
        } else {
            SDL_snprintf(fallback, sizeof(fallback), "Wayland output %u", (unsigned)out->id);
            best = fallback;
        }
        SDLOP_SetDisplayName(display, best);
        out->named = true;
    }

    /* SDL reports the display in the coordinate space the application draws in:
       logical unless SDL_HINT_VIDEO_WAYLAND_SCALE_TO_DISPLAY asked for physical
       pixels (SDL3 derives the same way from the current mode). */
    if (SDLOP_ScaleToDisplayEnabled()) {
        SDLOP_SetDisplayBounds(display, &(SDL_Rect){ out->x, out->y, native_w, native_h });
    } else {
        SDLOP_SetDisplayBounds(display, &(SDL_Rect){ out->x, out->y, logical_w, logical_h });
    }
    display->usable = display->bounds;
    SDLOP_SetDisplayScale(display, factor);

    memset(&native_mode, 0, sizeof(native_mode));
    native_mode.displayID = out->id;
    native_mode.format = SDL_PIXELFORMAT_XRGB8888;
    native_mode.w = native_w;
    native_mode.h = native_h;
    native_mode.refresh_rate = (float)out->refresh / 1000.0f;
    native_mode.refresh_rate_numerator = out->refresh;
    native_mode.refresh_rate_denominator = 1000;
    native_mode.pixel_density = 1.0f;

    logical_mode = native_mode;
    logical_mode.w = logical_w;
    logical_mode.h = logical_h;
    logical_mode.pixel_density = factor;

    SDLOP_ClearDisplayModes(display);
    if (factor == 1.0f || sdlop_wl_viewporter) {
        /* The application can render at the native resolution... */
        SDLOP_AddDisplayMode(display, &native_mode);
        if (native_w != logical_w || native_h != logical_h) {
            SDLOP_AddDisplayMode(display, &logical_mode);
        }
    } else {
        /* ...or at any whole multiple of the logical resolution. */
        int i;
        for (i = wl_scale; i > 0; --i) {
            SDL_DisplayMode scaled = logical_mode;
            scaled.w = logical_w * i;
            scaled.h = logical_h * i;
            scaled.pixel_density = 1.0f;
            SDLOP_AddDisplayMode(display, &scaled);
        }
    }

    /* Without SDL_HINT_VIDEO_WAYLAND_SCALE_TO_DISPLAY the desktop stays in
       logical coordinates and the compositor upscales the window surface. */
    if (SDLOP_ScaleToDisplayEnabled()) {
        SDLOP_SetDisplayCurrentMode(display, &native_mode);
    } else {
        SDLOP_SetDisplayCurrentMode(display, &logical_mode);
    }
}

static void xdg_output_logical_position(void *data, struct zxdg_output_v1 *xdg_output,
                                        int32_t x, int32_t y)
{
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)data;
    (void)xdg_output;

    out->x = x;
    out->y = y;
    /* The position alone is not enough to place a display: the size follows in
       its own event, and until it arrives the mode's size stands in for it. */
}

static void xdg_output_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
                                    int32_t width, int32_t height)
{
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)data;
    (void)xdg_output;

    if (width > 0 && height > 0) {
        out->logical_w = width;
        out->logical_h = height;
        out->have_logical = true;
    }
}

/* v1/v2 signal the end of a batch with done; v2 also sends name/description.
   From manager version 3 the event is deprecated: the compositor sends an extra
   wl_output.done instead, which output_done() counts. */
static void xdg_output_done(void *data, struct zxdg_output_v1 *xdg_output)
{
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)data;

    if (zxdg_output_v1_get_version(xdg_output) < 3) {
        sdlop_wl_apply_output(out);
    }
}

/* zxdg_output_v1 repeats name and description, but on the output object, not on a
   wl_output: they get their own handlers rather than being squeezed into the
   wl_output ones (whose first argument is a different type). Both are deprecated
   from wl_output v4 on, so they are only remembered as a fallback. */
static void xdg_output_name(void *data, struct zxdg_output_v1 *xdg_output, const char *name)
{
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)data;

    (void)xdg_output;
    if (name && name[0] && !out->connector[0]) {
        SDL_strlcpy(out->connector, name, sizeof(out->connector));
    }
}

static void xdg_output_description(void *data, struct zxdg_output_v1 *xdg_output,
                                   const char *description)
{
    /* The description ("Dell Inc. DELL U2720Q") is friendlier than the connector
       name ("DP-3"), so it is the first choice - but only when the compositor
       speaks wl_output below v4, exactly like SDL3. */
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)data;

    (void)xdg_output;
    if (description && description[0] && out->version < 4) {
        SDL_strlcpy(out->xdg_description, description, sizeof(out->xdg_description));
    }
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    xdg_output_logical_position,
    xdg_output_logical_size,
    xdg_output_done,
    xdg_output_name,
    xdg_output_description,
};

static const struct wl_output_listener output_listener = {
    output_geometry,
    output_mode,
    output_done,
    output_scale,
    output_name,
    output_description,
};

/* ------------------------------------------------------------------------- */
/* Seat: pointer and keyboard                                                */
/* ------------------------------------------------------------------------- */

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    SDL_Window *window = (SDL_Window *)wl_surface_get_user_data(surface);
    (void)data; (void)pointer; (void)serial;

    sdlop_wl_pointer_focus = window;
    sdlop_wl_pointer_x = (float)wl_fixed_to_double(sx);
    sdlop_wl_pointer_y = (float)wl_fixed_to_double(sy);
    if (window) {
        SDLOP_OnWindowMouseEnter(window);
        if (!SDLOP_AsyncInputActive()) {
            SDLOP_SendMouseMotionAbsolute(window->id, sdlop_wl_pointer_x, sdlop_wl_pointer_y,
                                          SDL_GetTicksNS());
        }
    }
    if (sdlop_wl_cursor_shape_manager && sdlop_wl_pointer && sdlop_wl_cursor_visible) {
        if (!sdlop_wl_cursor_shape_device) {
            sdlop_wl_cursor_shape_device =
                wp_cursor_shape_manager_v1_get_pointer(sdlop_wl_cursor_shape_manager, pointer);
        }
        if (sdlop_wl_cursor_shape_device) {
            wp_cursor_shape_device_v1_set_shape(sdlop_wl_cursor_shape_device, serial,
                                                WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
        }
    }
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    (void)data; (void)pointer; (void)serial; (void)surface;
    if (sdlop_wl_pointer_focus) {
        SDLOP_OnWindowMouseLeave(sdlop_wl_pointer_focus);
    }
    sdlop_wl_pointer_focus = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy)
{
    float x = (float)wl_fixed_to_double(sx);
    float y = (float)wl_fixed_to_double(sy);
    SDL_Window *window = sdlop_wl_pointer_focus;

    (void)data; (void)pointer;
    if (!window) {
        return;
    }
    if (window->flags & SDL_WINDOW_MOUSE_RELATIVE_MODE) {
        float dx = x - sdlop_wl_pointer_x;
        float dy = y - sdlop_wl_pointer_y;
        sdlop_wl_pointer_x = x;
        sdlop_wl_pointer_y = y;
        if (!SDLOP_AsyncInputActive()) {
            SDLOP_SendMouseMotionRelative(window->id, dx, dy, SDL_GetTicksNS());
        }
        return;
    }
    sdlop_wl_pointer_x = x;
    sdlop_wl_pointer_y = y;
    if (!SDLOP_AsyncInputActive()) {
        SDLOP_SendMouseMotionAbsolute(window->id, x, y, SDL_GetTicksNS());
    }
    (void)time;
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    Uint8 sdl_button;
    (void)data; (void)pointer; (void)serial; (void)time;

    switch (button) {
        case 0x110: sdl_button = SDL_BUTTON_LEFT; break;
        case 0x111: sdl_button = SDL_BUTTON_RIGHT; break;
        case 0x112: sdl_button = SDL_BUTTON_MIDDLE; break;
        case 0x113: sdl_button = SDL_BUTTON_X1; break;
        case 0x114: sdl_button = SDL_BUTTON_X2; break;
        default: return;
    }
    if (SDLOP_AsyncInputActive()) {
        return;
    }
    SDLOP_SendMouseButton(1, sdl_button, state == WL_POINTER_BUTTON_STATE_PRESSED,
                          sdlop_wl_pointer_x, sdlop_wl_pointer_y, SDL_GetTicksNS());
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                         wl_fixed_t value)
{
    float amount = (float)wl_fixed_to_double(value);
    (void)data; (void)pointer; (void)time;

    if (!sdlop_wl_pointer_focus || SDLOP_AsyncInputActive()) {
        return;
    }
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        /* Wayland's sign is inverted with respect to SDL's */
        SDLOP_SendMouseWheel(sdlop_wl_pointer_focus->id, 0.0f, -amount / 10.0f,
                             SDL_MOUSEWHEEL_NORMAL, SDL_GetTicksNS());
    } else {
        SDLOP_SendMouseWheel(sdlop_wl_pointer_focus->id, amount / 10.0f, 0.0f,
                             SDL_MOUSEWHEEL_NORMAL, SDL_GetTicksNS());
    }
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data; (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    (void)data; (void)pointer; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis,
                                  int32_t value120)
{
    (void)data; (void)pointer; (void)axis; (void)value120;
}

static void pointer_axis_relative_direction(void *data, struct wl_pointer *pointer, uint32_t axis,
                                            uint32_t direction)
{
    (void)data; (void)pointer; (void)axis; (void)direction;
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

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd,
                            uint32_t size)
{
    (void)data; (void)keyboard; (void)size;
#ifdef SDLOP_HAVE_XKBCOMMON
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    if (SDLOP_XKBOverrideActive()) {
        SDLOP_LogInfo("sdlop: ignoring the compositor keymap, SDLOP_XKB_KEYMAP is set");
        close(fd);
        return;
    }
    if (!SDLOP_XKBLoadKeymapFD(fd)) {
        SDLOP_LogWarn("sdlop: could not compile the compositor keymap");
    }
#else
    close(fd);
#endif
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    SDL_Window *window = surface ? (SDL_Window *)wl_surface_get_user_data(surface) : NULL;
    uint32_t *key;
    (void)data; (void)keyboard; (void)serial;

    sdlop_wl_keyboard_focus = window;
    for (key = (uint32_t *)keys->data; (uint32_t *)key < (uint32_t *)((char *)keys->data + keys->size); key++) {
        SDL_Scancode scancode = SDLOP_ScancodeFromEvdevKeycode(*key);
        if (scancode != SDL_SCANCODE_UNKNOWN && !SDLOP_AsyncInputActive()) {
            SDLOP_SendKeyEvent(scancode, SDLK_UNKNOWN, SDL_KMOD_NONE, true, false,
                               SDL_GetTicksNS(), *key);
        }
    }
    if (window) {
        SDLOP_OnWindowFocusGained(window);
    }
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface)
{
    (void)data; (void)keyboard; (void)serial;
    sdlop_wl_repeat_key = 0;
    if (sdlop_wl_keyboard_focus) {
        SDLOP_OnWindowFocusLost(sdlop_wl_keyboard_focus);
    } else if (surface && !sdlop_wl_keyboard_focus) {
        SDLOP_ResetKeyboardState();
    }
    sdlop_wl_keyboard_focus = NULL;
}

static bool sdlop_wl_key_repeats(Uint32 key, SDL_Scancode scancode);
static void sdlop_wl_repeat_handle(void);

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    SDL_Scancode scancode = SDLOP_ScancodeFromEvdevKeycode(key);
    (void)data; (void)keyboard; (void)serial; (void)time;

    if (SDLOP_AsyncInputActive()) {
        return;                    /* the evdev worker is the input source */
    }
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    /* The keycode and the text come from the keymap we registered, via the
       evdev keycode the compositor just gave us. */
    SDLOP_SendKeyEvent(scancode, SDLK_UNKNOWN, SDL_KMOD_NONE,
                       state == WL_KEYBOARD_KEY_STATE_PRESSED, false, SDL_GetTicksNS(), key);

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (sdlop_wl_repeat_rate && sdlop_wl_key_repeats(key, scancode)) {
            sdlop_wl_repeat_key = key;
            sdlop_wl_repeat_deadline =
                SDL_GetTicksNS() + (Uint64)sdlop_wl_repeat_delay * SDL_NS_PER_MS;
        } else {
            sdlop_wl_repeat_key = 0;
        }
    } else if (key == sdlop_wl_repeat_key) {
        sdlop_wl_repeat_key = 0;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    (void)data; (void)keyboard; (void)serial; (void)latched; (void)locked; (void)group;
#ifdef SDLOP_HAVE_XKBCOMMON
    if (SDLOP_XKBState()) {
        xkb_state_update_mask(SDLOP_XKBState(), depressed, latched, locked, 0, 0, group);
    } else
#endif
    {
        SDL_Keymod mods = SDL_KMOD_NONE;
        if (depressed & 1) {
            mods |= SDL_KMOD_LSHIFT;
        }
        if (depressed & 2) {
            mods |= SDL_KMOD_CAPS;
        }
        if (depressed & 4) {
            mods |= SDL_KMOD_LCTRL;
        }
        if (depressed & 8) {
            mods |= SDL_KMOD_LALT;
        }
        if (depressed & 64) {
            mods |= SDL_KMOD_LGUI;
        }
        SDL_SetModState(mods);
    }
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate,
                                 int32_t delay)
{
    (void)data; (void)keyboard;
    /* SDL3 clamps the rate the same way: a value of 0 means "do not repeat". */
    sdlop_wl_repeat_rate = (rate < 0) ? 0 : (rate > 1000 ? 1000 : rate);
    sdlop_wl_repeat_delay = (delay < 0) ? 0 : delay;
}

/* Does this key repeat at all? Modifiers do not, and the keymap knows. */
static bool sdlop_wl_key_repeats(Uint32 key, SDL_Scancode scancode)
{
#ifdef SDLOP_HAVE_XKBCOMMON
    if (SDLOP_XKBKeymap()) {
        return SDLOP_XKBKeyRepeats(key);
    }
#endif
    (void)key;
    switch (scancode) {
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT:
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL:
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT:
    case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI:
    case SDL_SCANCODE_CAPSLOCK: case SDL_SCANCODE_NUMLOCKCLEAR:
    case SDL_SCANCODE_SCROLLLOCK:
        return false;
    default:
        return true;
    }
}

/* Send a repeat for the key that is still held, if one is due. Called from the
   event pump, which is the only place repeats can be delivered from: they need a
   timer and there is no compositor event to hang them on. */
static void sdlop_wl_repeat_handle(void)
{
    SDL_Scancode scancode;
    Uint64 now;

    if (!sdlop_wl_repeat_key || !sdlop_wl_repeat_rate || SDLOP_AsyncInputActive()) {
        return;
    }
    now = SDL_GetTicksNS();
    if (now < sdlop_wl_repeat_deadline) {
        return;
    }
    scancode = SDLOP_ScancodeFromEvdevKeycode(sdlop_wl_repeat_key);
    if (scancode != SDL_SCANCODE_UNKNOWN) {
        SDLOP_SendKeyEvent(scancode, SDLK_UNKNOWN, SDL_KMOD_NONE, true, true, now,
                           sdlop_wl_repeat_key);
    }
    sdlop_wl_repeat_deadline = now + (Uint64)SDL_NS_PER_SECOND / (Uint64)sdlop_wl_repeat_rate;
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_keymap,
    keyboard_enter,
    keyboard_leave,
    keyboard_key,
    keyboard_modifiers,
    keyboard_repeat_info,
};

static bool sdlop_wayland_lock_pointer(SDL_Window *window);
static void sdlop_wayland_unlock_pointer(SDL_Window *window);
static void sdlop_wayland_unconfine_pointer(SDL_Window *window);
static const struct zwp_relative_pointer_v1_listener relative_pointer_listener;

static void sdlop_wayland_setup_relative_pointer(void)
{
    if (sdlop_wl_relative_pointer || !sdlop_wl_relative_pointer_manager || !sdlop_wl_pointer) {
        return;
    }
    sdlop_wl_relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
        sdlop_wl_relative_pointer_manager, sdlop_wl_pointer);
    if (sdlop_wl_relative_pointer) {
        zwp_relative_pointer_v1_add_listener(sdlop_wl_relative_pointer, &relative_pointer_listener,
                                             NULL);
    }
    /* A pointer can appear *after* the application asked for relative mode: a
       seat without a mouse at connect time (a virtual pointer, a hotplugged
       device) grows one later. Lock it on the windows that are already in
       relative mode, or the mode would silently stay in its delta fallback. */
    if (sdlop_wl_pointer_constraints && SDLOP_RelativeMouseModeActive()) {
        int count = 0;
        SDL_Window **windows = SDL_GetWindows(&count);
        if (windows) {
            for (int i = 0; i < count; i++) {
                if (windows[i]->flags & SDL_WINDOW_MOUSE_RELATIVE_MODE) {
                    sdlop_wayland_unconfine_pointer(windows[i]);
                    sdlop_wayland_lock_pointer(windows[i]);
                }
            }
            SDL_free(windows);
        }
    }
}

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    (void)data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !sdlop_wl_pointer) {
        sdlop_wl_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(sdlop_wl_pointer, &pointer_listener, NULL);
        sdlop_wl_has_pointer = true;
        sdlop_wayland_setup_relative_pointer();
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !sdlop_wl_keyboard) {
        sdlop_wl_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(sdlop_wl_keyboard, &keyboard_listener, NULL);
        sdlop_wl_has_keyboard = true;
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && sdlop_wl_keyboard) {
        wl_keyboard_release(sdlop_wl_keyboard);
        sdlop_wl_keyboard = NULL;
        sdlop_wl_has_keyboard = false;
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && sdlop_wl_pointer) {
        if (sdlop_wl_relative_pointer) {
            zwp_relative_pointer_v1_destroy(sdlop_wl_relative_pointer);
            sdlop_wl_relative_pointer = NULL;
        }
        wl_pointer_release(sdlop_wl_pointer);
        sdlop_wl_pointer = NULL;
        sdlop_wl_has_pointer = false;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    seat_capabilities,
    seat_name,
};

/* ------------------------------------------------------------------------- */
/* xdg-shell                                                                 */
/* ------------------------------------------------------------------------- */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    wm_base_ping,
};

/* ------------------------------------------------------------------------- */
/* Which display is this window on?                                          */
/*                                                                           */
/* Wayland has no window position, so the compositor is the only authority on */
/* where a surface landed: wl_surface.enter() lists the outputs a surface is  */
/* shown on, and the last one to arrive is the one the window is on. That is  */
/* what SDL3 reports as the window's display, and it is what makes the display */
/* survive an output being unplugged (the compositor then re-homes the window  */
/* and sends leave/enter for the new one).                                    */
/* ------------------------------------------------------------------------- */

static void sdlop_wl_move_window_to_last_output(SDL_Window *window)
{
    SDLOP_WaylandOutput *out;

    if (WL_NUM_OUTPUTS(window) <= 0) {
        /* The window is on no display at all, e.g. minimized: SDL3 leaves the
           window's display alone in that case. */
        return;
    }
    out = WL_OUTPUTS(window)[WL_NUM_OUTPUTS(window) - 1];
    /* The position SDL3 reports is not the real one - Wayland does not have
       one. It is the top-left of the display the window is on: applications
       (and their mouse math) rely on the window's position being inside the
       display it claims to be on. */
    SDLOP_OnWindowMovedOnDisplay(window, out->id, out->x, out->y);
}

/* The compositor can send an event for an object that was already released on
   our side (the surface or the output is gone): only objects this window and
   this backend still own are acted upon. */
static bool sdlop_wl_owns_output(SDLOP_WaylandOutput *out, struct wl_output *output)
{
    return out && out->output == output;
}

/* Drop `out` from the window's output list, if it is in there. */
static void sdlop_wl_forget_output(SDL_Window *window, SDLOP_WaylandOutput *out)
{
    int i;

    for (i = 0; i < WL_NUM_OUTPUTS(window); i++) {
        if (WL_OUTPUTS(window)[i] == out) {
            memmove(&WL_OUTPUTS(window)[i], &WL_OUTPUTS(window)[i + 1],
                    sizeof(WL_OUTPUTS(window)[0]) * (size_t)(WL_NUM_OUTPUTS(window) - i - 1));
            WL_NUM_OUTPUTS(window)--;
            i--;
        }
    }
}

static void sdlop_wl_surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    SDL_Window *window = (SDL_Window *)data;
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)wl_output_get_user_data(output);
    int i;

    if (!sdlop_wl_owns_output(out, output) || surface != WL_SURFACE(window)) {
        return;
    }
    for (i = 0; i < WL_NUM_OUTPUTS(window); i++) {
        if (WL_OUTPUTS(window)[i] == out) {
            return;   /* already known */
        }
    }
    if (WL_NUM_OUTPUTS(window) >= WL_MAX_OUTPUTS) {
        return;
    }
    WL_OUTPUTS(window)[WL_NUM_OUTPUTS(window)++] = out;
    /* A fullscreen window belongs to the output it went fullscreen on, which is
       the first one the compositor reports; a windowed one follows the last. */
    if (!(window->flags & SDL_WINDOW_FULLSCREEN) || WL_NUM_OUTPUTS(window) == 1) {
        sdlop_wl_move_window_to_last_output(window);
    }
}

static void sdlop_wl_surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    SDL_Window *window = (SDL_Window *)data;
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)wl_output_get_user_data(output);

    if (!sdlop_wl_owns_output(out, output) || surface != WL_SURFACE(window)) {
        return;
    }
    sdlop_wl_forget_output(window, out);
    if (WL_NUM_OUTPUTS(window) > 0) {
        sdlop_wl_move_window_to_last_output(window);
    }
}

static void sdlop_wl_forget_output_in_window(SDL_Window *window, void *userdata)
{
    SDLOP_WaylandOutput *out = (SDLOP_WaylandOutput *)userdata;

    if (WL_NUM_OUTPUTS(window) == 0) {
        return;
    }
    sdlop_wl_forget_output(window, out);
    if (WL_NUM_OUTPUTS(window) > 0) {
        /* Still on one of the outputs that are left: move the window there. */
        sdlop_wl_move_window_to_last_output(window);
    }
    /* Nothing left means the compositor has not re-homed the surface yet; the
       display removal itself then picks a surviving display in the core. */
}

/* An output global went away: forget it everywhere it was being tracked. */
static void sdlop_wl_forget_removed_output(SDLOP_WaylandOutput *out)
{
    SDLOP_WindowIterate(sdlop_wl_forget_output_in_window, out);
}

static const struct wl_surface_listener sdlop_wl_surface_listener = {
    sdlop_wl_surface_enter,
    sdlop_wl_surface_leave,
    NULL,   /* preferred_buffer_scale: the whole-output scale is used instead */
    NULL,   /* preferred_buffer_transform */
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    SDL_Window *window = (SDL_Window *)data;

    xdg_surface_ack_configure(xdg_surface, serial);
    window->driver.wayland.configured = true;
    if (window->driver.wayland.pending_resize) {
        int w = window->driver.wayland.pending_w;
        int h = window->driver.wayland.pending_h;
        window->driver.wayland.pending_resize = false;
        if (w > 0 && h > 0) {
            SDLOP_OnWindowResized(window, w, h);
            SDLOP_OnWindowPixelSizeChanged(window, w * (int)window->display_scale,
                                           h * (int)window->display_scale);
        }
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
                               int32_t height, struct wl_array *states)
{
    SDL_Window *window = (SDL_Window *)data;
    uint32_t *state;
    bool maximized = false, fullscreen = false, activated = false;

    for (state = (uint32_t *)states->data;
         (uint32_t *)state < (uint32_t *)((char *)states->data + states->size); state++) {
        switch (*state) {
            case XDG_TOPLEVEL_STATE_MAXIMIZED: maximized = true; break;
            case XDG_TOPLEVEL_STATE_FULLSCREEN: fullscreen = true; break;
            case XDG_TOPLEVEL_STATE_ACTIVATED: activated = true; break;
            default: break;
        }
    }
    (void)toplevel;
    if (width > 0 && height > 0) {
        window->driver.wayland.pending_w = width;
        window->driver.wayland.pending_h = height;
        window->driver.wayland.pending_resize = true;
    } else {
        window->driver.wayland.pending_resize = false;
    }
    if (maximized != ((window->flags & SDL_WINDOW_MAXIMIZED) != 0)) {
        SDLOP_OnWindowMaximized(window, maximized);
    }
    if (fullscreen != ((window->flags & SDL_WINDOW_FULLSCREEN) != 0)) {
        SDLOP_OnWindowFullscreenChanged(window, fullscreen);
    }
    if (activated) {
        SDLOP_OnWindowFocusGained(window);
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    SDL_Window *window = (SDL_Window *)data;
    (void)toplevel;
    SDLOP_OnWindowClosed(window);
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel, int32_t width,
                                      int32_t height)
{
    (void)data; (void)toplevel; (void)width; (void)height;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel, struct wl_array *capabilities)
{
    (void)data; (void)toplevel; (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    toplevel_configure,
    toplevel_close,
    toplevel_configure_bounds,
    toplevel_wm_capabilities,
};

/* ------------------------------------------------------------------------- */
/* shm buffers                                                               */
/* ------------------------------------------------------------------------- */

static int sdlop_create_shm_file(size_t size)
{
    int fd;
#ifdef __linux__
#ifdef SYS_memfd_create
    fd = (int)syscall(SYS_memfd_create, "sdlop-shm", MFD_CLOEXEC);
#else
    fd = -1;
#endif
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) < 0) {
            close(fd);
            fd = -1;
        } else {
            return fd;
        }
    }
#endif
    {
        char name[64];
        static int counter;
        SDL_snprintf(name, sizeof(name), "/sdlop-%d-%d", (int)getpid(), counter++);
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            return -1;
        }
        shm_unlink(name);
        if (ftruncate(fd, (off_t)size) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    SDLOP_WaylandBuffer *buf = (SDLOP_WaylandBuffer *)data;
    (void)buffer;
    buf->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    buffer_release,
};

/* The compositor calls this when it is ready for the next frame. Keeping one
   armed after every commit tells us when a buffer has really been consumed,
   which is the difference between a healthy client and one that attaches a
   buffer it is still using (a protocol error a compositor is allowed to kill
   the connection over). */
static void frame_callback_done(void *data, struct wl_callback *callback, uint32_t callback_data)
{
    SDL_Window *window = (SDL_Window *)data;
    (void)callback_data;

    if (window && window->driver.wayland.frame_callback == callback) {
        window->driver.wayland.frame_callback = NULL;
    }
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_callback_listener = {
    frame_callback_done,
};

static void sdlop_wayland_arm_frame_callback(SDL_Window *window)
{
    if (!WL_SURFACE(window) || window->driver.wayland.frame_callback) {
        return;
    }
    window->driver.wayland.frame_callback = wl_surface_frame(WL_SURFACE(window));
    wl_callback_add_listener(window->driver.wayland.frame_callback, &frame_callback_listener, window);
}

/* Wait for a buffer to come free, pumping the connection meanwhile. Bounded, so
   a compositor that never releases anything (or is simply gone) cannot hang the
   application: the caller falls back to reusing a buffer after the timeout. */
static void sdlop_wayland_wait_for_buffer(SDL_Window *window, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicksNS() + (Uint64)timeout_ms * SDL_NS_PER_MS;

    for (;;) {
        int i;

        for (i = 0; i < SDLOP_WAYLAND_NUM_BUFFERS; i++) {
            if (!window->driver.wayland.buffers[i].busy) {
                return;
            }
        }
        if (SDL_GetTicksNS() >= deadline || !sdlop_wl_display) {
            return;
        }
        /* Reading events is what delivers wl_buffer.release(). */
        if (!sdlop_wl_reading) {
            if (wl_display_prepare_read(sdlop_wl_display) != 0) {
                wl_display_dispatch_pending(sdlop_wl_display);
                SDL_Delay(1);
                continue;
            }
            sdlop_wl_reading = true;
        }
        {
            struct pollfd pfd;
            pfd.fd = wl_display_get_fd(sdlop_wl_display);
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, 1) > 0 && (pfd.revents & POLLIN)) {
                wl_display_read_events(sdlop_wl_display);
            } else {
                wl_display_cancel_read(sdlop_wl_display);
            }
            sdlop_wl_reading = false;
        }
        wl_display_dispatch_pending(sdlop_wl_display);
        wl_display_flush(sdlop_wl_display);
    }
}

static bool sdlop_wayland_create_buffers(SDL_Window *window, int w, int h)
{
    for (int i = 0; i < SDLOP_WAYLAND_NUM_BUFFERS; i++) {
        SDLOP_WaylandBuffer *buf = &window->driver.wayland.buffers[i];
        size_t size = (size_t)w * (size_t)h * 4;
        int fd;
        struct wl_shm_pool *pool;

        if (buf->buffer) {
            wl_buffer_destroy(buf->buffer);
            buf->buffer = NULL;
        }
        if (buf->pixels) {
            munmap(buf->pixels, buf->size);
            buf->pixels = NULL;
        }
        if (w <= 0 || h <= 0) {
            continue;
        }

        fd = sdlop_create_shm_file(size);
        if (fd < 0) {
            return SDL_SetError("Couldn't create a shared memory buffer");
        }
        buf->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (buf->pixels == MAP_FAILED) {
            close(fd);
            buf->pixels = NULL;
            return SDL_SetError("Couldn't map the shared memory buffer");
        }
        buf->size = size;
        pool = wl_shm_create_pool(sdlop_wl_shm, fd, (int32_t)size);
        buf->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        if (!buf->buffer) {
            return SDL_SetError("Couldn't create a Wayland buffer");
        }
        wl_buffer_add_listener(buf->buffer, &buffer_listener, buf);
        buf->busy = false;
        buf->w = w;
        buf->h = h;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Window management                                                         */
/* ------------------------------------------------------------------------- */

static bool sdlop_wayland_create_window(SDL_Window *window)
{
    struct wl_surface *surface;
    int w = window->w;
    int h = window->h;

    surface = wl_compositor_create_surface(sdlop_wl_compositor);
    if (!surface) {
        return SDL_SetError("Couldn't create a Wayland surface");
    }
    WL_SURFACE(window) = surface;
    wl_surface_set_user_data(surface, window);
    wl_surface_add_listener(surface, &sdlop_wl_surface_listener, window);
    window->driver.wayland.num_buffers = SDLOP_WAYLAND_NUM_BUFFERS;

    if (!sdlop_wl_wm_base) {
        /* no xdg-shell: this is not a usable desktop session */
        return SDL_SetError("Compositor does not support xdg-shell");
    }

    if ((window->flags & SDL_WINDOW_POPUP_MENU) && window->parent) {
        struct xdg_positioner *positioner = xdg_wm_base_create_positioner(sdlop_wl_wm_base);
        XDG_SURFACE(window) = xdg_wm_base_get_xdg_surface(sdlop_wl_wm_base, surface);
        xdg_surface_add_listener(XDG_SURFACE(window), &xdg_surface_listener, window);
        xdg_positioner_set_size(positioner, w, h);
        xdg_positioner_set_anchor_rect(positioner, 0, 0, window->parent->w, window->parent->h);
        XDG_POPUP(window) =
            xdg_surface_get_popup(XDG_SURFACE(window),
                                  XDG_SURFACE(window->parent), positioner);
        xdg_positioner_destroy(positioner);
    } else {
        XDG_SURFACE(window) = xdg_wm_base_get_xdg_surface(sdlop_wl_wm_base, surface);
        xdg_surface_add_listener(XDG_SURFACE(window), &xdg_surface_listener, window);
        XDG_TOPLEVEL(window) =
            xdg_surface_get_toplevel(XDG_SURFACE(window));
        xdg_toplevel_add_listener(XDG_TOPLEVEL(window), &toplevel_listener, window);
        xdg_toplevel_set_title(XDG_TOPLEVEL(window),
                               window->title ? window->title : "SDLop");
        {
            const char *app_id = SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING);
            if (app_id) {
                xdg_toplevel_set_app_id(XDG_TOPLEVEL(window), app_id);
            }
        }
        if (window->flags & SDL_WINDOW_RESIZABLE) {
            xdg_toplevel_set_min_size(XDG_TOPLEVEL(window), 0, 0);
        }
    }

    if (!sdlop_wayland_create_buffers(window, w, h)) {
        return false;
    }
    wl_surface_commit(surface);
    wl_display_roundtrip(sdlop_wl_display);
    window->driver.wayland.configured = true;
    return true;
}

static void sdlop_wayland_destroy_window(SDL_Window *window)
{
    for (int i = 0; i < SDLOP_WAYLAND_NUM_BUFFERS; i++) {
        SDLOP_WaylandBuffer *buf = &window->driver.wayland.buffers[i];
        if (buf->buffer) {
            wl_buffer_destroy(buf->buffer);
        }
        if (buf->pixels) {
            munmap(buf->pixels, buf->size);
        }
        memset(buf, 0, sizeof(*buf));
    }
    if (window->driver.wayland.frame_callback) {
        wl_callback_destroy(window->driver.wayland.frame_callback);
        window->driver.wayland.frame_callback = NULL;
    }
    if (window->driver.wayland.egl_window) {
        wl_egl_window_destroy(window->driver.wayland.egl_window);
    }
    /* Pointer constraints belong to the surface, so they go before it does. */
    sdlop_wayland_unlock_pointer(window);
    sdlop_wayland_unconfine_pointer(window);
    if (XDG_POPUP(window)) {
        xdg_popup_destroy(XDG_POPUP(window));
    }
    if (XDG_TOPLEVEL(window)) {
        xdg_toplevel_destroy(XDG_TOPLEVEL(window));
    }
    if (XDG_SURFACE(window)) {
        xdg_surface_destroy(XDG_SURFACE(window));
    }
    if (WL_SURFACE(window)) {
        wl_surface_destroy(WL_SURFACE(window));
    }
}

static bool sdlop_wayland_present(SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDLOP_WaylandBuffer *buf = NULL;
    int scale = 1;
    int i;

    (void)rects;
    (void)numrects;

    if (!window->surface || !WL_SURFACE(window)) {
        return false;
    }
    /* Prefer a free buffer; if both are still in the compositor's hands, wait a
       little for one instead of overwriting a buffer that is being displayed. */
    for (i = 0; i < SDLOP_WAYLAND_NUM_BUFFERS; i++) {
        if (!window->driver.wayland.buffers[i].busy) {
            buf = &window->driver.wayland.buffers[i];
            break;
        }
    }
    if (!buf) {
        sdlop_wayland_wait_for_buffer(window, 100);
        for (i = 0; i < SDLOP_WAYLAND_NUM_BUFFERS; i++) {
            if (!window->driver.wayland.buffers[i].busy) {
                buf = &window->driver.wayland.buffers[i];
                break;
            }
        }
    }
    if (!buf) {
        /* Still nothing: the compositor is not releasing buffers at all. Reuse
           the first one rather than refusing to draw - a torn frame beats a
           frozen window - but re-arm the frame callback so we notice as soon as
           the compositor comes back for more. */
        if (window->driver.wayland.frame_callback) {
            wl_callback_destroy(window->driver.wayland.frame_callback);
            window->driver.wayland.frame_callback = NULL;
        }
        buf = &window->driver.wayland.buffers[0];
    }
    if (buf->w != window->surface->w || buf->h != window->surface->h) {
        if (!sdlop_wayland_create_buffers(window, window->surface->w, window->surface->h)) {
            return false;
        }
        buf = &window->driver.wayland.buffers[0];
    }

    {
        const Uint8 *src = (const Uint8 *)window->surface->pixels;
        Uint8 *dst = (Uint8 *)buf->pixels;
        int row_bytes = window->surface->w * 4;
        for (int y = 0; y < window->surface->h; y++) {
            memcpy(dst + (size_t)y * row_bytes, src + (size_t)y * window->surface->pitch,
                   (size_t)row_bytes);
        }
    }

    if (window->display_scale > 1.0f) {
        scale = (int)window->display_scale;
    }
    if (sdlop_wl_viewporter && scale > 1) {
        if (!WL_VIEWPORT(window)) {
            WL_VIEWPORT(window) = wp_viewporter_get_viewport(sdlop_wl_viewporter,
                                                                          WL_SURFACE(window));
        }
        if (WL_VIEWPORT(window)) {
            wp_viewport_set_destination(WL_VIEWPORT(window),
                                        buf->w / scale, buf->h / scale);
        }
    }

    /* Arm the next frame notification *before* the commit that will trigger it;
       the compositor takes the callback from the new surface state. */
    sdlop_wayland_arm_frame_callback(window);

    wl_surface_attach(WL_SURFACE(window), buf->buffer, 0, 0);
    wl_surface_damage_buffer(WL_SURFACE(window), 0, 0, buf->w, buf->h);
    wl_surface_set_buffer_scale(WL_SURFACE(window), scale);
    wl_surface_commit(WL_SURFACE(window));
    buf->busy = true;
    return true;
}

static bool sdlop_wayland_set_title(SDL_Window *window, const char *title)
{
    if (XDG_TOPLEVEL(window)) {
        xdg_toplevel_set_title(XDG_TOPLEVEL(window), title);
        wl_surface_commit(WL_SURFACE(window));
    }
    return true;
}

static bool sdlop_wayland_show(SDL_Window *window)
{
    if (WL_SURFACE(window)) {
        wl_surface_commit(WL_SURFACE(window));
    }
    return true;
}

static bool sdlop_wayland_hide(SDL_Window *window)
{
    if (WL_SURFACE(window)) {
        wl_surface_attach(WL_SURFACE(window), NULL, 0, 0);
        wl_surface_commit(WL_SURFACE(window));
    }
    return true;
}

static bool sdlop_wayland_fullscreen(SDL_Window *window, bool fullscreen)
{
    if (!XDG_TOPLEVEL(window)) {
        return SDL_Unsupported();
    }
    if (fullscreen) {
        xdg_toplevel_set_fullscreen(XDG_TOPLEVEL(window), NULL);
    } else {
        xdg_toplevel_unset_fullscreen(XDG_TOPLEVEL(window));
    }
    return true;
}

static bool sdlop_wayland_maximize(SDL_Window *window)
{
    if (!XDG_TOPLEVEL(window)) {
        return SDL_Unsupported();
    }
    xdg_toplevel_set_maximized(XDG_TOPLEVEL(window));
    return true;
}

static bool sdlop_wayland_minimize(SDL_Window *window)
{
    if (!XDG_TOPLEVEL(window)) {
        return SDL_Unsupported();
    }
    xdg_toplevel_set_minimized(XDG_TOPLEVEL(window));
    return true;
}

static bool sdlop_wayland_restore(SDL_Window *window)
{
    if (!XDG_TOPLEVEL(window)) {
        return SDL_Unsupported();
    }
    if (!(window->flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
        return true;                   /* nothing to restore */
    }
    /* Only maximized: a fullscreen window is left fullscreen, which is what
       SDL_RestoreWindow() means (SDL_SetWindowFullscreen() is the way out of
       fullscreen). Note xdg-shell has no way to unset minimize. */
    if (window->flags & SDL_WINDOW_MAXIMIZED) {
        xdg_toplevel_unset_maximized(XDG_TOPLEVEL(window));
    }
    return true;
}

static bool sdlop_wayland_set_size(SDL_Window *window, int w, int h)
{
    const float scale = window->display_scale > 0.0f ? window->display_scale : 1.0f;
    const int pixel_w = (int)(w * scale + 0.5f);
    const int pixel_h = (int)(h * scale + 0.5f);

    /* On Wayland the compositor decides the size; we ask, and the configure
       event tells us what we got (and confirms the pixel size with it). */
    SDLOP_OnWindowResized(window, w, h);
    SDLOP_OnWindowPixelSizeChanged(window, pixel_w, pixel_h);
    sdlop_wayland_create_buffers(window, pixel_w, pixel_h);
    return true;
}

static bool sdlop_wayland_sync(SDL_Window *window)
{
    (void)window;
    wl_display_roundtrip(sdlop_wl_display);
    return true;
}

static bool sdlop_wayland_set_cursor(SDL_Window *window, SDL_Cursor *cursor)
{
    static const Uint32 shape_for_system_cursor[] = {
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING,
    };
    (void)window;

    if (!cursor) {
        return true;
    }
    sdlop_wl_current_cursor = cursor;
    if (!sdlop_wl_cursor_shape_manager || !sdlop_wl_pointer || !sdlop_wl_cursor_visible) {
        return true;
    }
    if (!sdlop_wl_cursor_shape_device) {
        sdlop_wl_cursor_shape_device =
            wp_cursor_shape_manager_v1_get_pointer(sdlop_wl_cursor_shape_manager, sdlop_wl_pointer);
    }
    if (!sdlop_wl_cursor_shape_device) {
        return true;
    }
    if (cursor->system < SDL_SYSTEM_CURSOR_COUNT) {
        wp_cursor_shape_device_v1_set_shape(sdlop_wl_cursor_shape_device, 0,
                                            shape_for_system_cursor[cursor->system]);
    } else {
        /* a custom 32-bit ARGB cursor: upload it through wl_shm */
        size_t size = (size_t)cursor->w * (size_t)cursor->h * 4;
        int fd = sdlop_create_shm_file(size);
        if (fd < 0) {
            return false;
        }
        {
            void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            struct wl_shm_pool *pool;
            struct wl_buffer *buffer;
            if (pixels == MAP_FAILED) {
                close(fd);
                return false;
            }
            memcpy(pixels, cursor->data, size);
            pool = wl_shm_create_pool(sdlop_wl_shm, fd, (int32_t)size);
            buffer = wl_shm_pool_create_buffer(pool, 0, cursor->w, cursor->h, cursor->w * 4,
                                               WL_SHM_FORMAT_ARGB8888);
            wl_shm_pool_destroy(pool);
            munmap(pixels, size);
            close(fd);
            if (!sdlop_wl_cursor_surface) {
                sdlop_wl_cursor_surface = wl_compositor_create_surface(sdlop_wl_compositor);
            }
            wl_surface_attach(sdlop_wl_cursor_surface, buffer, 0, 0);
            wl_surface_damage(sdlop_wl_cursor_surface, 0, 0, cursor->w, cursor->h);
            wl_surface_commit(sdlop_wl_cursor_surface);
            wl_pointer_set_cursor(sdlop_wl_pointer, 0, sdlop_wl_cursor_surface,
                                  cursor->hot_x, cursor->hot_y);
            wl_buffer_destroy(buffer);
        }
    }
    return true;
}

static bool sdlop_wayland_show_cursor(SDL_Window *window, bool show)
{
    (void)window;
    sdlop_wl_cursor_visible = show;
    if (sdlop_wl_pointer) {
        if (!show) {
            wl_pointer_set_cursor(sdlop_wl_pointer, 0, NULL, 0, 0);
        } else if (sdlop_wl_current_cursor) {
            sdlop_wayland_set_cursor(window, sdlop_wl_current_cursor);
        }
    }
    return true;
}

static bool sdlop_wayland_warp_mouse(SDL_Window *window, float x, float y)
{
    /* Wayland deliberately has no way to move the pointer. SDL3 has the same
       restriction; we update our own state so the application sees the position
       it asked for, and the compositor moves the pointer on the next real
       motion. */
    sdlop_wl_pointer_x = x;
    sdlop_wl_pointer_y = y;
    if (window) {
        SDLOP_SendMouseMotionAbsolute(window->id, x, y, SDL_GetTicksNS());
    }
    return true;
}

/* On-demand mode query: the video core asks for it when a display has no mode
   yet, which happens between SDL_EVENT_DISPLAY_ADDED and the first complete
   burst of output events. The answer comes from the raw fields, so it is also
   correct for an output that has not been applied at all. */
static bool sdlop_wayland_get_mode(SDL_DisplayID display_id, SDL_DisplayMode *mode)
{
    struct SDLOP_Display *display = SDLOP_GetDisplay(display_id);
    SDLOP_WaylandOutput *out = display ? (SDLOP_WaylandOutput *)display->driver_data : NULL;
    int w, h;

    if (!out || !out->have_mode) {
        return false;
    }
    if (out->have_logical) {
        w = out->logical_w;
        h = out->logical_h;
    } else {
        w = out->pixel_w;
        h = out->pixel_h;
    }
    memset(mode, 0, sizeof(*mode));
    mode->displayID = display_id;
    mode->format = SDL_PIXELFORMAT_XRGB8888;
    mode->w = w;
    mode->h = h;
    mode->refresh_rate = (float)out->refresh / 1000.0f;
    mode->refresh_rate_numerator = out->refresh;
    mode->refresh_rate_denominator = 1000;
    mode->pixel_density = (float)(out->scale > 0 ? out->scale : 1);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Pointer constraints: relative mouse mode, mouse grab and mouse rect        */
/* ------------------------------------------------------------------------- */

static void locked_pointer_locked(void *data, struct zwp_locked_pointer_v1 *locked_pointer)
{
    (void)data; (void)locked_pointer;
}

static void locked_pointer_unlocked(void *data, struct zwp_locked_pointer_v1 *locked_pointer)
{
    /* The compositor can burn the lock (a shell shortcut, another client taking
       focus). The object stays valid until we destroy it, exactly like SDL3. */
    (void)data; (void)locked_pointer;
}

static const struct zwp_locked_pointer_v1_listener locked_pointer_listener = {
    locked_pointer_locked,
    locked_pointer_unlocked,
};

static void confined_pointer_confined(void *data, struct zwp_confined_pointer_v1 *confined_pointer)
{
    (void)data; (void)confined_pointer;
}

static void confined_pointer_unconfined(void *data, struct zwp_confined_pointer_v1 *confined_pointer)
{
    (void)data; (void)confined_pointer;
}

static const struct zwp_confined_pointer_v1_listener confined_pointer_listener = {
    confined_pointer_confined,
    confined_pointer_unconfined,
};

static bool sdlop_wayland_lock_pointer(SDL_Window *window)
{
    if (!sdlop_wl_pointer_constraints || !sdlop_wl_pointer || !window) {
        return false;
    }
    if (WL_LOCKED_POINTER(window)) {
        return true;
    }
    if (WL_CONFINED_POINTER(window)) {
        /* A surface cannot be confined and locked at the same time. */
        zwp_confined_pointer_v1_destroy(WL_CONFINED_POINTER(window));
        WL_CONFINED_POINTER(window) = NULL;
    }
    WL_LOCKED_POINTER(window) = zwp_pointer_constraints_v1_lock_pointer(
        sdlop_wl_pointer_constraints, WL_SURFACE(window), sdlop_wl_pointer, NULL,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    if (!WL_LOCKED_POINTER(window)) {
        return false;
    }
    zwp_locked_pointer_v1_add_listener(WL_LOCKED_POINTER(window), &locked_pointer_listener, window);
    return true;
}

static void sdlop_wayland_unlock_pointer(SDL_Window *window)
{
    if (window && WL_LOCKED_POINTER(window)) {
        zwp_locked_pointer_v1_destroy(WL_LOCKED_POINTER(window));
        WL_LOCKED_POINTER(window) = NULL;
    }
}

static bool sdlop_wayland_confine_pointer(SDL_Window *window)
{
    struct wl_region *region = NULL;

    if (!sdlop_wl_pointer_constraints || !sdlop_wl_pointer || !window) {
        /* Nothing to confine with (a compositor without
           zwp_pointer_constraints_v1, or a seat with no pointer at all). A
           Wayland grab can never stop another client from taking the pointer,
           so this stays a best-effort no-op instead of an error. */
        return true;
    }
    if (WL_LOCKED_POINTER(window)) {
        return true;   /* a locked pointer already keeps the cursor in place */
    }
    if (WL_CONFINED_POINTER(window)) {
        zwp_confined_pointer_v1_destroy(WL_CONFINED_POINTER(window));
        WL_CONFINED_POINTER(window) = NULL;
    }
    if (window->mouse_rect_set) {
        region = wl_compositor_create_region(sdlop_wl_compositor);
        if (region) {
            wl_region_add(region, window->mouse_rect.x, window->mouse_rect.y,
                          window->mouse_rect.w, window->mouse_rect.h);
        }
    }
    WL_CONFINED_POINTER(window) = zwp_pointer_constraints_v1_confine_pointer(
        sdlop_wl_pointer_constraints, WL_SURFACE(window), sdlop_wl_pointer, region,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    if (region) {
        wl_region_destroy(region);
    }
    if (!WL_CONFINED_POINTER(window)) {
        return false;
    }
    zwp_confined_pointer_v1_add_listener(WL_CONFINED_POINTER(window), &confined_pointer_listener,
                                         window);
    /* The confinement region is double buffered: it only takes effect on commit. */
    wl_surface_commit(WL_SURFACE(window));
    return true;
}

static void sdlop_wayland_unconfine_pointer(SDL_Window *window)
{
    if (window && WL_CONFINED_POINTER(window)) {
        zwp_confined_pointer_v1_destroy(WL_CONFINED_POINTER(window));
        WL_CONFINED_POINTER(window) = NULL;
    }
}

/* SDL_SetWindowMouseGrab / SDL_SetWindowMouseRect both mean "keep the pointer on
   this surface" on Wayland, which is what a pointer confinement does. */
static bool sdlop_wayland_set_window_grab(SDL_Window *window, bool keyboard, bool mouse)
{
    (void)keyboard;   /* Wayland has no keyboard grab: focus belongs to the compositor */
    if (mouse || window->mouse_rect_set) {
        return sdlop_wayland_confine_pointer(window);
    }
    sdlop_wayland_unconfine_pointer(window);
    return true;
}

static bool sdlop_wayland_set_mouse_rect(SDL_Window *window, const SDL_Rect *rect)
{
    if (rect || (window->flags & SDL_WINDOW_MOUSE_GRABBED)) {
        return sdlop_wayland_confine_pointer(window);
    }
    sdlop_wayland_unconfine_pointer(window);
    return true;
}

static bool sdlop_wayland_relative_mode(SDL_Window *window, bool enabled)
{
    SDL_Window **windows;
    int count = 0, i;

    (void)window;
    if (enabled && (!sdlop_wl_relative_pointer_manager || !sdlop_wl_pointer_constraints ||
                    !sdlop_wl_pointer)) {
        /* Without zwp_pointer_constraints_v1 a client cannot lock the pointer.
           The absolute pointer still produces deltas (see pointer_motion), so
           relative mode works in a degraded form rather than failing. */
        SDLOP_LogInfo("sdlop: compositor has no pointer-constraints, relative mode uses deltas");
        return true;
    }
    windows = SDL_GetWindows(&count);
    if (!windows) {
        return false;
    }
    for (i = 0; i < count; i++) {
        if (enabled) {
            /* A lock and a confine on the same surface is a protocol error. */
            sdlop_wayland_unconfine_pointer(windows[i]);
            sdlop_wayland_lock_pointer(windows[i]);
        } else {
            sdlop_wayland_unlock_pointer(windows[i]);
            if ((windows[i]->flags & SDL_WINDOW_MOUSE_GRABBED) || windows[i]->mouse_rect_set) {
                sdlop_wayland_confine_pointer(windows[i]);
            }
        }
    }
    SDL_free(windows);
    return true;
}

/* zwp_relative_pointer_v1: the unaccelerated deltas of a locked pointer. This is
   the low latency path - no pointer acceleration, no compositor cursor warping,
   one event per real device motion. */
static void relative_pointer_motion(void *data, struct zwp_relative_pointer_v1 *pointer,
                                    uint32_t time_hi, uint32_t time_lo, wl_fixed_t dx, wl_fixed_t dy,
                                    wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
    SDL_Window *window = sdlop_wl_pointer_focus ? sdlop_wl_pointer_focus : SDLOP_InputTargetWindow();

    (void)data; (void)pointer; (void)time_hi; (void)time_lo; (void)dx; (void)dy;
    if (!window || !SDLOP_RelativeMouseModeActive() || SDLOP_AsyncInputActive()) {
        return;
    }
    sdlop_wl_pointer_x += (float)wl_fixed_to_double(dx_unaccel);
    sdlop_wl_pointer_y += (float)wl_fixed_to_double(dy_unaccel);
    SDLOP_SendMouseMotionRelative(window->id, (float)wl_fixed_to_double(dx_unaccel),
                                  (float)wl_fixed_to_double(dy_unaccel), SDL_GetTicksNS());
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    relative_pointer_motion,
};

/* ------------------------------------------------------------------------- */
/* Registry / driver entry points                                            */
/* ------------------------------------------------------------------------- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    (void)data;
    (void)version;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        sdlop_wl_compositor = (struct wl_compositor *)wl_registry_bind(registry, name,
                                                                      &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        sdlop_wl_shm = (struct wl_shm *)wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        sdlop_wl_wm_base = (struct xdg_wm_base *)wl_registry_bind(registry, name,
                                                                  &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(sdlop_wl_wm_base, &wm_base_listener, NULL);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        sdlop_wl_seat = (struct wl_seat *)wl_registry_bind(registry, name, &wl_seat_interface, 5);
        wl_seat_add_listener(sdlop_wl_seat, &seat_listener, NULL);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        /* Version 4 is what the SDLop implementation speaks: it has the name and
           description events, and wl_output.release (v3+) which quit uses.
           Binding a lower version would make that release a protocol error. */
        uint32_t output_version = (version < SDLOP_WL_OUTPUT_VERSION) ? version
                                                                    : SDLOP_WL_OUTPUT_VERSION;
        {
            SDLOP_WaylandOutput *out = sdlop_wl_free_output_slot();
            struct SDLOP_Display *display;
            if (!out) {
                return;
            }
            memset(out, 0, sizeof(*out));
            out->scale = 1;
            out->registry_name = name;
            /* Fallback placement for a compositor without zxdg_output_v1: lay
               the displays out left to right, which the logical position then
               corrects. Until a burst arrives the display sits at 0,0 - what
               SDL3 reports as well - rather than at an invented offset. */
            out->x = 0;
            out->fallback_x = sdlop_wl_next_output_x;
            out->version = output_version;
            out->output = (struct wl_output *)wl_registry_bind(registry, name, &wl_output_interface,
                                                               output_version);
            if (!out->output) {
                return;
            }
            display = SDLOP_AddDisplay(sdlop_wl_next_display_id++);
            if (display) {
                out->id = display->id;
                display->driver_data = out;
                display->bounds.x = out->x;
            }
            wl_output_add_listener(out->output, &output_listener, out);
            if (sdlop_wl_xdg_output_manager) {
                out->xdg_output = zxdg_output_manager_v1_get_xdg_output(sdlop_wl_xdg_output_manager,
                                                                        out->output);
                if (out->xdg_output) {
                    zxdg_output_v1_add_listener(out->xdg_output, &xdg_output_listener, out);
                }
            }
            sdlop_wl_next_output_x += 1920;
        }
    } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        sdlop_wl_pointer_constraints = (struct zwp_pointer_constraints_v1 *)wl_registry_bind(
            registry, name, &zwp_pointer_constraints_v1_interface, 1);
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        sdlop_wl_relative_pointer_manager = (struct zwp_relative_pointer_manager_v1 *)wl_registry_bind(
            registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        /* v3 drops the per-output done event in favour of wl_output.done; both
           are handled, so any version works. */
        uint32_t v = (version < 3) ? version : 3;
        sdlop_wl_xdg_output_manager =
            (struct zxdg_output_manager_v1 *)wl_registry_bind(registry, name,
                                                              &zxdg_output_manager_v1_interface, v);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        sdlop_wl_viewporter = (struct wp_viewporter *)wl_registry_bind(registry, name,
                                                                       &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        sdlop_wl_cursor_shape_manager =
            (struct wp_cursor_shape_manager_v1 *)wl_registry_bind(
                registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    }
}

/* Globals can go away: unplugging a monitor makes its wl_output disappear. The
   display it stood for has to disappear with it, or the application sees a
   monitor that is no longer there. */
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    int i;

    (void)data;
    (void)registry;

    for (i = 0; i < sdlop_wl_num_outputs; i++) {
        if (sdlop_wl_outputs[i].registry_name != name) {
            continue;
        }
        if (sdlop_wl_outputs[i].xdg_output) {
            zxdg_output_v1_destroy(sdlop_wl_outputs[i].xdg_output);
        }
        if (sdlop_wl_outputs[i].output) {
            wl_output_release(sdlop_wl_outputs[i].output);
        }
        /* Windows that were showing there lost it: drop it from their output
           list first, then let the display removal re-home whatever is left
           over. The slot itself stays put until the next output claims it. */
        sdlop_wl_forget_removed_output(&sdlop_wl_outputs[i]);
        SDLOP_RemoveDisplay(sdlop_wl_outputs[i].id);
        memset(&sdlop_wl_outputs[i], 0, sizeof(sdlop_wl_outputs[0]));
        {
            bool any_live = false;
            int j;
            for (j = 0; j < sdlop_wl_num_outputs; j++) {
                if (sdlop_wl_outputs[j].output) {
                    any_live = true;
                }
            }
            if (!any_live) {
                sdlop_wl_next_output_x = 0;
            }
        }
        return;
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};

static bool sdlop_wayland_init(void)
{
    const char *display_name = SDL_getenv("WAYLAND_DISPLAY");

    if (!display_name || !display_name[0]) {
        return SDL_SetError("WAYLAND_DISPLAY is not set");
    }
    /* SDL3 keeps displays and windows in logical coordinates by default; the
       hint switches to rendering in the output's physical pixels. */
    SDLOP_SetScaleToDisplay(SDL_GetHintBoolean(SDL_HINT_VIDEO_WAYLAND_SCALE_TO_DISPLAY, false));
    sdlop_wl_display = wl_display_connect(NULL);
    if (!sdlop_wl_display) {
        return SDL_SetError("Couldn't connect to the Wayland display");
    }
    sdlop_wl_registry = wl_display_get_registry(sdlop_wl_display);
    wl_registry_add_listener(sdlop_wl_registry, &registry_listener, NULL);
    wl_display_roundtrip(sdlop_wl_display);

    if (!sdlop_wl_compositor || !sdlop_wl_shm) {
        return SDL_SetError("Wayland compositor is missing wl_compositor/wl_shm");
    }
    if (!sdlop_wl_wm_base) {
        return SDL_SetError("Wayland compositor does not support xdg-shell");
    }
    /* a second roundtrip so wl_output/wl_seat globals are always resolved */
    wl_display_roundtrip(sdlop_wl_display);
    sdlop_wayland_setup_relative_pointer();

#ifdef SDLOP_HAVE_XKBCOMMON
    /* Have a layout ready before the first key can arrive; the compositor's own
       keymap replaces it if and when it is sent. */
    SDLOP_XKBInit();
#endif
    return true;
}

static void sdlop_wayland_quit(void)
{
    /* Every global this connection produced has to be dropped here, not just
       destroyed: the objects belong to the connection that is about to go away,
       and a process that calls SDL_Init() again (the benchmark's cycle loop, and
       any application that re-inits) would otherwise hand libwayland proxies
       from a dead connection - which the compositor sees as an unknown object.
       Weston rejects that with a protocol error; the release/destroy calls then
       dereference freed proxies. */
    if (sdlop_wl_cursor_shape_device) {
        wp_cursor_shape_device_v1_destroy(sdlop_wl_cursor_shape_device);
        sdlop_wl_cursor_shape_device = NULL;
    }
    if (sdlop_wl_cursor_surface) {
        wl_surface_destroy(sdlop_wl_cursor_surface);
        sdlop_wl_cursor_surface = NULL;
    }
    if (sdlop_wl_cursor_shape_manager) {
        wp_cursor_shape_manager_v1_destroy(sdlop_wl_cursor_shape_manager);
        sdlop_wl_cursor_shape_manager = NULL;
    }
    if (sdlop_wl_relative_pointer) {
        zwp_relative_pointer_v1_destroy(sdlop_wl_relative_pointer);
        sdlop_wl_relative_pointer = NULL;
    }
    if (sdlop_wl_relative_pointer_manager) {
        zwp_relative_pointer_manager_v1_destroy(sdlop_wl_relative_pointer_manager);
        sdlop_wl_relative_pointer_manager = NULL;
    }
    if (sdlop_wl_pointer_constraints) {
        zwp_pointer_constraints_v1_destroy(sdlop_wl_pointer_constraints);
        sdlop_wl_pointer_constraints = NULL;
    }
    if (sdlop_wl_viewporter) {
        wp_viewporter_destroy(sdlop_wl_viewporter);
        sdlop_wl_viewporter = NULL;
    }
    for (int i = 0; i < sdlop_wl_num_outputs; i++) {
        if (sdlop_wl_outputs[i].xdg_output) {
            zxdg_output_v1_destroy(sdlop_wl_outputs[i].xdg_output);
            sdlop_wl_outputs[i].xdg_output = NULL;
        }
    }
    if (sdlop_wl_xdg_output_manager) {
        zxdg_output_manager_v1_destroy(sdlop_wl_xdg_output_manager);
        sdlop_wl_xdg_output_manager = NULL;
    }
    if (sdlop_wl_pointer) {
        wl_pointer_release(sdlop_wl_pointer);
        sdlop_wl_pointer = NULL;
        sdlop_wl_has_pointer = false;
    }
    if (sdlop_wl_keyboard) {
        wl_keyboard_release(sdlop_wl_keyboard);
        sdlop_wl_keyboard = NULL;
        sdlop_wl_has_keyboard = false;
    }
    if (sdlop_wl_seat) {
        wl_seat_release(sdlop_wl_seat);
        sdlop_wl_seat = NULL;
    }
    for (int i = 0; i < sdlop_wl_num_outputs; i++) {
        if (sdlop_wl_outputs[i].output) {
            wl_output_release(sdlop_wl_outputs[i].output);
            sdlop_wl_outputs[i].output = NULL;
        }
    }
    sdlop_wl_num_outputs = 0;
    sdlop_wl_next_output_x = 0;
    if (sdlop_wl_wm_base) {
        xdg_wm_base_destroy(sdlop_wl_wm_base);
        sdlop_wl_wm_base = NULL;
    }
    if (sdlop_wl_shm) {
        wl_shm_destroy(sdlop_wl_shm);
        sdlop_wl_shm = NULL;
    }
    if (sdlop_wl_compositor) {
        wl_compositor_destroy(sdlop_wl_compositor);
        sdlop_wl_compositor = NULL;
    }
    if (sdlop_wl_registry) {
        wl_registry_destroy(sdlop_wl_registry);
        sdlop_wl_registry = NULL;
    }
    if (sdlop_wl_display) {
        wl_display_flush(sdlop_wl_display);
        wl_display_disconnect(sdlop_wl_display);
    }
    sdlop_wl_display = NULL;
#ifdef SDLOP_HAVE_XKBCOMMON
    SDLOP_SetKeyLayout(NULL);
    SDLOP_XKBQuit();
#endif
}

static int sdlop_wayland_get_event_fd(void)
{
    return sdlop_wl_display ? wl_display_get_fd(sdlop_wl_display) : -1;
}

/* Called by SDLOP_WaitPlatformEvents() right before poll(): claiming the read
   here is what lets SDL_WaitEvent() block in poll() without racing the
   compositor's data. */
static void sdlop_wayland_prepare_read(void)
{
    if (!sdlop_wl_display || sdlop_wl_reading) {
        return;
    }
    if (wl_display_prepare_read(sdlop_wl_display) == 0) {
        sdlop_wl_reading = true;
    } else {
        wl_display_dispatch_pending(sdlop_wl_display);
    }
    wl_display_flush(sdlop_wl_display);
}

/* How long until the next key repeat, so that a blocking SDL_WaitEvent() wakes up
   in time for it instead of sleeping through it. */
static Sint64 sdlop_wayland_get_event_timeout_ns(void)
{
    Sint64 remaining;

    if (!sdlop_wl_repeat_key || !sdlop_wl_repeat_rate || SDLOP_AsyncInputActive()) {
        return -1;
    }
    remaining = (Sint64)(sdlop_wl_repeat_deadline - SDL_GetTicksNS());
    return (remaining < 0) ? 0 : remaining;
}

static void sdlop_wayland_pump_events(void)
{
    if (!sdlop_wl_display) {
        return;
    }
    sdlop_wl_repeat_handle();

    /* Claim the socket read if nobody has claimed it. An application that only
       ever calls SDL_PollEvent() never reaches SDLOP_WaitPlatformEvents(), and
       without this the client would never read from the compositor at all: its
       outbound queue fills up (a Wayland connection caps it at a few kilobytes)
       and the compositor drops the client. */
    if (!sdlop_wl_reading) {
        if (wl_display_prepare_read(sdlop_wl_display) != 0) {
            /* Somebody queued events for us: dispatch them instead. */
            wl_display_dispatch_pending(sdlop_wl_display);
            wl_display_flush(sdlop_wl_display);
            return;
        }
        sdlop_wl_reading = true;
    }

    {
        /* Only read when the socket actually has data: if the blocking wait in
           SDL_WaitEvent() timed out, the claim has to be released again rather
           than read from (libwayland allows one or the other, not both). */
        struct pollfd pfd;
        pfd.fd = wl_display_get_fd(sdlop_wl_display);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            wl_display_read_events(sdlop_wl_display);
        } else {
            wl_display_cancel_read(sdlop_wl_display);
        }
        sdlop_wl_reading = false;
    }
    wl_display_dispatch_pending(sdlop_wl_display);
    wl_display_flush(sdlop_wl_display);
}

/* ------------------------------------------------------------------------- */
/* Vulkan: the Wayland WSI (VK_KHR_wayland_surface)                          */
/*                                                                           */
/* The ABI is declared here, like the Vulkan module's, so no Vulkan headers  */
/* are needed to build against a runtime-only system.                        */
/* ------------------------------------------------------------------------- */

#define SDLOP_VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR 1000008000

typedef int (*SDLOP_PFN_vkCreateWaylandSurfaceKHR)(VkInstance instance, const void *create_info,
                                                   const struct VkAllocationCallbacks *allocator,
                                                   VkSurfaceKHR *surface);
typedef bool (*SDLOP_PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)(
    VkPhysicalDevice physical_device, Uint32 queue_family_index, struct wl_display *display);

typedef struct SDLOP_VkWaylandSurfaceCreateInfoKHR
{
    Uint32 sType;
    const void *pNext;
    Uint32 flags;
    struct wl_display *display;
    struct wl_surface *surface;
} SDLOP_VkWaylandSurfaceCreateInfoKHR;

static bool sdlop_wayland_create_vulkan_surface(SDL_Window *window, VkInstance instance,
                                                const struct VkAllocationCallbacks *allocator,
                                                VkSurfaceKHR *surface)
{
    SDLOP_PFN_vkCreateWaylandSurfaceKHR create_surface;
    SDLOP_VkWaylandSurfaceCreateInfoKHR info;
    int result;

    if (!sdlop_wl_display || !WL_SURFACE(window)) {
        return SDL_SetError("The window has no Wayland surface");
    }
    create_surface = (SDLOP_PFN_vkCreateWaylandSurfaceKHR)SDLOP_VulkanGetInstanceProc(
        instance, "vkCreateWaylandSurfaceKHR");
    if (!create_surface) {
        return SDL_SetError("VK_KHR_wayland_surface extension is not enabled in the Vulkan instance");
    }
    memset(&info, 0, sizeof(info));
    info.sType = SDLOP_VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    info.display = sdlop_wl_display;
    info.surface = WL_SURFACE(window);
    result = create_surface(instance, &info, allocator, surface);
    if (result != 0) {
        return SDL_SetError("vkCreateWaylandSurfaceKHR failed: %s", SDLOP_VulkanResultString(result));
    }
    return true;
}

static bool sdlop_wayland_vulkan_presentation_support(VkInstance instance,
                                                      VkPhysicalDevice physical_device,
                                                      Uint32 queue_family_index)
{
    SDLOP_PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR get_support =
        (SDLOP_PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)SDLOP_VulkanGetInstanceProc(
            instance, "vkGetPhysicalDeviceWaylandPresentationSupportKHR");

    if (!get_support) {
        return SDL_SetError("VK_KHR_wayland_surface extension is not enabled in the Vulkan instance");
    }
    if (!sdlop_wl_display) {
        return SDL_SetError("The Wayland display is not open");
    }
    return get_support(physical_device, queue_family_index, sdlop_wl_display) ? true : false;
}

static const char *const *sdlop_wayland_vulkan_extensions(Uint32 *count)
{
    static const char *extensions[] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };
    if (count) {
        *count = 2;
    }
    return extensions;
}

extern const SDLOP_GLDriver SDLOP_EGLDriver;

const SDLOP_VideoDriver SDLOP_WaylandVideoDriver = {
    .name = "wayland",
    .init = sdlop_wayland_init,
    .quit = sdlop_wayland_quit,

    /* displays come from the registry, so the core's generic display API is all
       that is needed - no driver hooks */
    .create_window = sdlop_wayland_create_window,
    .destroy_window = sdlop_wayland_destroy_window,
    .set_window_title = sdlop_wayland_set_title,
    .set_window_size = sdlop_wayland_set_size,
    .set_window_fullscreen = sdlop_wayland_fullscreen,
    .show_window = sdlop_wayland_show,
    .hide_window = sdlop_wayland_hide,
    .maximize_window = sdlop_wayland_maximize,
    .minimize_window = sdlop_wayland_minimize,
    .restore_window = sdlop_wayland_restore,
    .sync_window = sdlop_wayland_sync,

    /* software presentation through wl_shm */
    .present_surface = sdlop_wayland_present,

    .pump_events = sdlop_wayland_pump_events,
    .get_event_timeout_ns = sdlop_wayland_get_event_timeout_ns,
    .get_event_fd = sdlop_wayland_get_event_fd,
    .prepare_read = sdlop_wayland_prepare_read,

    .set_cursor = sdlop_wayland_set_cursor,
    .show_cursor = sdlop_wayland_show_cursor,
    .warp_mouse = sdlop_wayland_warp_mouse,
    .set_relative_mouse_mode = sdlop_wayland_relative_mode,
    .get_display_mode = sdlop_wayland_get_mode,
    .set_window_grab = sdlop_wayland_set_window_grab,
    .set_window_mouse_rect = sdlop_wayland_set_mouse_rect,

    .create_vulkan_surface = sdlop_wayland_create_vulkan_surface,
    .get_vulkan_instance_extensions = sdlop_wayland_vulkan_extensions,
    .vulkan_presentation_support = sdlop_wayland_vulkan_presentation_support,

    .gl = &SDLOP_EGLDriver,
};

/* ------------------------------------------------------------------------- */
/* Handles the GL driver needs                                               */
/* ------------------------------------------------------------------------- */

void *sdlop_wl_display_handle(void)
{
    return sdlop_wl_display;
}

void *sdlop_wl_egl_window_create(SDL_Window *window)
{
    return wl_egl_window_create(window->driver.wayland.wl_surface, window->pixel_w, window->pixel_h);
}

void sdlop_wl_egl_window_resize(struct wl_egl_window *egl_window, int w, int h, int dx, int dy)
{
    if (egl_window) {
        wl_egl_window_resize(egl_window, w, h, dx, dy);
    }
}
