/*
  SDLop Wayland backend - drawing tablets (zwp_tablet_manager_v2).

  Stock SDL_waylandevents.c semantics: each tool with a known role (pen,
  eraser, brush, pencil, airbrush) becomes an SDL pen once its description
  is done; proximity/down/up/motion/axes/buttons are collected per frame
  and sent in stock order (motion before a down, after an up). Pressure,
  distance and slider are normalized from 0..65535, tilt and rotation are
  degrees (rotation mapped to -180..180), BTN_STYLUS/2/3 are pen buttons
  1/2/3. Tools get the default cursor shape on proximity-in when
  wp_cursor_shape_manager_v1 is there. Pads are not used and are released
  right away.

  SDLOP_WAYLAND_NO_TABLET=1 leaves the protocol unbound.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "sdlop_wayland_internal.h"
#include "sdlop_wayland_dyn.h"
#include "tablet-v2-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"

#include <string.h>

enum
{
    TOOL_STATE_NONE,
    TOOL_STATE_DOWN,
    TOOL_STATE_UP
};

typedef struct WlTabletTool
{
    struct zwp_tablet_tool_v2 *tool;
    struct wp_cursor_shape_device_v1 *shape;
    SDLop_PenInfo info;
    SDL_PenID pen;
    SDL_Window *focus;
    struct
    {
        bool have_proximity, in_proximity;
        int tool_state;
        bool have_motion;
        float x, y;
        Uint32 axes_set;
        float axes[SDL_PEN_AXIS_COUNT];
        int buttons[3]; /* 0 unchanged, 1 down, 2 up */
    } frame;
    struct WlTabletTool *next;
} WlTabletTool;

typedef struct WlTablet
{
    struct zwp_tablet_v2 *tablet;
    struct WlTablet *next;
} WlTablet;

static struct
{
    struct zwp_tablet_manager_v2 *manager;
    struct wl_seat *seat;
    struct zwp_tablet_seat_v2 *tseat;
    WlTabletTool *tools;
    WlTablet *tablets;
} tb;

/* ---- tablets: only kept to be destroyed ---------------------------- */

static void tablet_name(void *data, struct zwp_tablet_v2 *t, const char *name)
{
    (void)data;
    (void)t;
    (void)name;
}

static void tablet_id(void *data, struct zwp_tablet_v2 *t, uint32_t vid, uint32_t pid)
{
    (void)data;
    (void)t;
    (void)vid;
    (void)pid;
}

static void tablet_path(void *data, struct zwp_tablet_v2 *t, const char *path)
{
    (void)data;
    (void)t;
    (void)path;
}

static void tablet_done(void *data, struct zwp_tablet_v2 *t)
{
    (void)data;
    (void)t;
}

static void tablet_removed(void *data, struct zwp_tablet_v2 *t)
{
    (void)data;
    for (WlTablet **pp = &tb.tablets; *pp; pp = &(*pp)->next) {
        if ((*pp)->tablet == t) {
            WlTablet *dead = *pp;
            *pp = dead->next;
            zwp_tablet_v2_destroy(dead->tablet);
            SDL_free(dead);
            return;
        }
    }
}

static const struct zwp_tablet_v2_listener tablet_listener = {
    tablet_name, tablet_id, tablet_path, tablet_done, tablet_removed,
};

/* ---- tools ---------------------------------------------------------- */

static void tool_type(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t type)
{
    (void)tool;
    WlTabletTool *t = (WlTabletTool *)data;
    switch (type) {
    case ZWP_TABLET_TOOL_V2_TYPE_PEN: t->info.subtype = SDLOP_PEN_TYPE_PEN; break;
    case ZWP_TABLET_TOOL_V2_TYPE_ERASER: t->info.subtype = SDLOP_PEN_TYPE_ERASER; break;
    case ZWP_TABLET_TOOL_V2_TYPE_BRUSH: t->info.subtype = SDLOP_PEN_TYPE_BRUSH; break;
    case ZWP_TABLET_TOOL_V2_TYPE_PENCIL: t->info.subtype = SDLOP_PEN_TYPE_PENCIL; break;
    case ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH: t->info.subtype = SDLOP_PEN_TYPE_AIRBRUSH; break;
    default: t->info.subtype = SDLOP_PEN_TYPE_UNKNOWN; break; /* finger, mouse, lens: not pens */
    }
}

static void tool_hardware_serial(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t hi, uint32_t lo)
{
    (void)data;
    (void)tool;
    (void)hi;
    (void)lo;
}

static void tool_hardware_id_wacom(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t hi, uint32_t lo)
{
    (void)tool;
    (void)hi;
    ((WlTabletTool *)data)->info.wacom_id = lo;
}

static void tool_capability(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t capability)
{
    (void)tool;
    WlTabletTool *t = (WlTabletTool *)data;
    switch (capability) {
    case ZWP_TABLET_TOOL_V2_CAPABILITY_TILT:
        t->info.capabilities |= SDLOP_PEN_CAPABILITY_XTILT | SDLOP_PEN_CAPABILITY_YTILT;
        break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE: t->info.capabilities |= SDLOP_PEN_CAPABILITY_PRESSURE; break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE: t->info.capabilities |= SDLOP_PEN_CAPABILITY_DISTANCE; break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION: t->info.capabilities |= SDLOP_PEN_CAPABILITY_ROTATION; break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER: t->info.capabilities |= SDLOP_PEN_CAPABILITY_SLIDER; break;
    default: break;
    }
}

static void tool_done(void *data, struct zwp_tablet_tool_v2 *tool)
{
    (void)tool;
    WlTabletTool *t = (WlTabletTool *)data;
    if (t->pen || t->info.subtype == SDLOP_PEN_TYPE_UNKNOWN) {
        return;
    }
    if (t->info.subtype == SDLOP_PEN_TYPE_ERASER) {
        t->info.capabilities |= SDLOP_PEN_CAPABILITY_ERASER;
    }
    static const char *const names[] = { "Tablet tool", "Tablet eraser", "Tablet pen", "Tablet pencil",
                                         "Tablet brush", "Tablet airbrush" };
    t->pen = SDLOP_AddPen(names[t->info.subtype], &t->info, t, t->focus, false, SDLOP_MonotonicNS());
}

static void tool_free(WlTabletTool *t, bool events)
{
    if (t->pen) {
        if (events) {
            SDLOP_RemovePen(t->pen, t->focus, SDLOP_MonotonicNS());
        }
        t->pen = 0;
    }
    if (t->shape) {
        wp_cursor_shape_device_v1_destroy(t->shape);
    }
    zwp_tablet_tool_v2_destroy(t->tool);
    SDL_free(t);
}

static void tool_removed(void *data, struct zwp_tablet_tool_v2 *tool)
{
    (void)tool;
    for (WlTabletTool **pp = &tb.tools; *pp; pp = &(*pp)->next) {
        if (*pp == data) {
            WlTabletTool *dead = *pp;
            *pp = dead->next;
            tool_free(dead, true);
            return;
        }
    }
}

static void tool_proximity_in(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial,
                              struct zwp_tablet_v2 *tablet, struct wl_surface *surface)
{
    (void)tool;
    (void)tablet;
    WlTabletTool *t = (WlTabletTool *)data;
    t->focus = surface ? SDLOP_Wayland_WindowFromSurface(surface) : NULL;
    t->frame.have_proximity = true;
    t->frame.in_proximity = true;
    if (t->shape) {
        wp_cursor_shape_device_v1_set_shape(t->shape, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    }
}

static void tool_proximity_out(void *data, struct zwp_tablet_tool_v2 *tool)
{
    (void)tool;
    WlTabletTool *t = (WlTabletTool *)data;
    t->frame.have_proximity = true;
    t->frame.in_proximity = false;
}

static void tool_down(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial)
{
    (void)tool;
    (void)serial;
    ((WlTabletTool *)data)->frame.tool_state = TOOL_STATE_DOWN;
}

static void tool_up(void *data, struct zwp_tablet_tool_v2 *tool)
{
    (void)tool;
    ((WlTabletTool *)data)->frame.tool_state = TOOL_STATE_UP;
}

static void tool_motion(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t x, wl_fixed_t y)
{
    (void)tool;
    WlTabletTool *t = (WlTabletTool *)data;
    /* window coordinates are surface coordinates in SDLop */
    t->frame.x = (float)wl_fixed_to_double(x);
    t->frame.y = (float)wl_fixed_to_double(y);
    t->frame.have_motion = true;
}

static void set_axis(WlTabletTool *t, SDL_PenAxis axis, float value)
{
    t->frame.axes[axis] = value;
    t->frame.axes_set |= 1u << axis;
}

static void tool_pressure(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t pressure)
{
    (void)tool;
    set_axis((WlTabletTool *)data, SDL_PEN_AXIS_PRESSURE, (float)pressure / 65535.0f);
}

static void tool_distance(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t distance)
{
    (void)tool;
    set_axis((WlTabletTool *)data, SDL_PEN_AXIS_DISTANCE, (float)distance / 65535.0f);
}

static void tool_tilt(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t tx, wl_fixed_t ty)
{
    (void)tool;
    set_axis((WlTabletTool *)data, SDL_PEN_AXIS_XTILT, (float)wl_fixed_to_double(tx));
    set_axis((WlTabletTool *)data, SDL_PEN_AXIS_YTILT, (float)wl_fixed_to_double(ty));
}

static void tool_rotation(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t degrees)
{
    (void)tool;
    const float r = (float)wl_fixed_to_double(degrees);
    set_axis((WlTabletTool *)data, SDL_PEN_AXIS_ROTATION, r > 180.0f ? r - 360.0f : r);
}

static void tool_slider(void *data, struct zwp_tablet_tool_v2 *tool, int32_t position)
{
    (void)tool;
    set_axis((WlTabletTool *)data, SDL_PEN_AXIS_SLIDER, (float)position / 65535.0f);
}

static void tool_wheel(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t degrees, int32_t clicks)
{
    (void)data;
    (void)tool;
    (void)degrees;
    (void)clicks; /* no SDL axis for it (stock ignores it too) */
}

static void tool_button(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial, uint32_t button,
                        uint32_t state)
{
    (void)tool;
    (void)serial;
    WlTabletTool *t = (WlTabletTool *)data;
    int b;
    switch (button) {
    case 0x14b: b = 0; break; /* BTN_STYLUS */
    case 0x14c: b = 1; break; /* BTN_STYLUS2 */
    case 0x149: b = 2; break; /* BTN_STYLUS3 */
    default: return;
    }
    t->frame.buttons[b] = state == ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED ? 1 : 2;
}

static void tool_frame(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t time)
{
    (void)tool;
    (void)time;
    WlTabletTool *t = (WlTabletTool *)data;
    const SDL_PenID id = t->pen;
    if (!id) {
        memset(&t->frame, 0, sizeof(t->frame));
        return;
    }
    const Uint64 ts = SDLOP_MonotonicNS();
    SDL_Window *window = t->focus;
    const bool eraser = t->info.subtype == SDLOP_PEN_TYPE_ERASER;

    if (t->frame.have_proximity && t->frame.in_proximity) {
        SDLOP_SendPenProximity(ts, id, window, true);
    }
    /* motion before a down and after an up: no stray strokes */
    if (t->frame.have_motion && t->frame.tool_state == TOOL_STATE_DOWN) {
        SDLOP_SendPenMotion(ts, id, window, t->frame.x, t->frame.y);
        SDLOP_SendPenTouch(ts, id, window, eraser, true);
    } else if (t->frame.have_motion && t->frame.tool_state == TOOL_STATE_UP) {
        SDLOP_SendPenTouch(ts, id, window, eraser, false);
        SDLOP_SendPenMotion(ts, id, window, t->frame.x, t->frame.y);
    } else {
        if (t->frame.tool_state != TOOL_STATE_NONE) {
            SDLOP_SendPenTouch(ts, id, window, eraser, t->frame.tool_state == TOOL_STATE_DOWN);
        }
        if (t->frame.have_motion) {
            SDLOP_SendPenMotion(ts, id, window, t->frame.x, t->frame.y);
        }
    }
    for (int a = 0; a < SDL_PEN_AXIS_COUNT; a++) {
        if (t->frame.axes_set & (1u << a)) {
            SDLOP_SendPenAxis(ts, id, window, (SDL_PenAxis)a, t->frame.axes[a]);
        }
    }
    for (int b = 0; b < 3; b++) {
        if (t->frame.buttons[b]) {
            SDLOP_SendPenButton(ts, id, window, (Uint8)(b + 1), t->frame.buttons[b] == 1);
        }
    }
    if (t->frame.have_proximity && !t->frame.in_proximity) {
        SDLOP_SendPenProximity(ts, id, window, false);
        t->focus = NULL;
    }
    memset(&t->frame, 0, sizeof(t->frame));
}

static const struct zwp_tablet_tool_v2_listener tool_listener = {
    tool_type,         tool_hardware_serial, tool_hardware_id_wacom,
    tool_capability,   tool_done,            tool_removed,
    tool_proximity_in, tool_proximity_out,   tool_down,
    tool_up,           tool_motion,          tool_pressure,
    tool_distance,     tool_tilt,            tool_rotation,
    tool_slider,       tool_wheel,           tool_button,
    tool_frame,
};

/* ---- seat ------------------------------------------------------------ */

static void seat_tablet_added(void *data, struct zwp_tablet_seat_v2 *seat, struct zwp_tablet_v2 *tablet)
{
    (void)data;
    (void)seat;
    WlTablet *t = (WlTablet *)SDL_calloc(1, sizeof(*t));
    if (!t) {
        zwp_tablet_v2_destroy(tablet);
        return;
    }
    t->tablet = tablet;
    zwp_tablet_v2_add_listener(tablet, &tablet_listener, t);
    t->next = tb.tablets;
    tb.tablets = t;
}

static void seat_tool_added(void *data, struct zwp_tablet_seat_v2 *seat, struct zwp_tablet_tool_v2 *tool)
{
    (void)data;
    (void)seat;
    WlTabletTool *t = (WlTabletTool *)SDL_calloc(1, sizeof(*t));
    if (!t) {
        zwp_tablet_tool_v2_destroy(tool);
        return;
    }
    t->tool = tool;
    t->info.max_tilt = -1.0f;
    t->info.num_buttons = -1;
    t->info.device_type = SDL_PEN_DEVICE_TYPE_UNKNOWN; /* the protocol doesn't say */
    struct wp_cursor_shape_manager_v1 *sm = SDLOP_Wayland_GetCursorShapeManager();
    if (sm) {
        t->shape = wp_cursor_shape_manager_v1_get_tablet_tool_v2(sm, tool);
    }
    zwp_tablet_tool_v2_add_listener(tool, &tool_listener, t);
    t->next = tb.tools;
    tb.tools = t;
}

static void seat_pad_added(void *data, struct zwp_tablet_seat_v2 *seat, struct zwp_tablet_pad_v2 *pad)
{
    (void)data;
    (void)seat;
    zwp_tablet_pad_v2_destroy(pad); /* pads (buttons/rings/strips) have no SDL events */
}

static const struct zwp_tablet_seat_v2_listener seat_listener = {
    seat_tablet_added,
    seat_tool_added,
    seat_pad_added,
};

static void ensure_tablet_seat(void)
{
    if (tb.manager && tb.seat && !tb.tseat) {
        tb.tseat = zwp_tablet_manager_v2_get_tablet_seat(tb.manager, tb.seat);
        zwp_tablet_seat_v2_add_listener(tb.tseat, &seat_listener, NULL);
    }
}

bool SDLOP_Wayland_Tablet_Global(struct wl_registry *registry, uint32_t name, const char *interface,
                                 uint32_t version)
{
    (void)version;
    if (strcmp(interface, zwp_tablet_manager_v2_interface.name) != 0) {
        return false;
    }
    if (!tb.manager && !SDLOP_HintFlag(SDLOP_HINT_WAYLAND_NO_TABLET, false)) {
        tb.manager = wl_registry_bind(registry, name, &zwp_tablet_manager_v2_interface, 1);
        ensure_tablet_seat();
    }
    return true;
}

void SDLOP_Wayland_Tablet_SeatAdded(struct wl_seat *seat)
{
    if (!tb.seat) {
        tb.seat = seat;
        ensure_tablet_seat();
    }
}

void SDLOP_Wayland_Tablet_WindowDestroyed(SDL_Window *window)
{
    for (WlTabletTool *t = tb.tools; t; t = t->next) {
        if (t->focus == window) {
            t->focus = NULL;
        }
    }
}

void SDLOP_Wayland_Tablet_Quit(void)
{
    while (tb.tools) {
        WlTabletTool *t = tb.tools;
        tb.tools = t->next;
        tool_free(t, false); /* SDLOP_RemoveAllPens follows at video quit */
    }
    while (tb.tablets) {
        WlTablet *t = tb.tablets;
        tb.tablets = t->next;
        zwp_tablet_v2_destroy(t->tablet);
        SDL_free(t);
    }
    if (tb.tseat) {
        zwp_tablet_seat_v2_destroy(tb.tseat);
    }
    if (tb.manager) {
        zwp_tablet_manager_v2_destroy(tb.manager);
    }
    memset(&tb, 0, sizeof(tb));
}
