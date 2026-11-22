/*
  SDLop - X11 pens (graphics tablets) through XInput 2, after stock SDL3's
  SDL_x11pen.c / SDL_x11xinput2.c:

  - a pen is an enabled slave (or floating) pointer with an "Abs Pressure"
    valuator; "Abs Tilt X/Y" become the tilt axes. It is an eraser when the
    Wacom "Wacom Tool Type" property says ERASER, else when its name
    contains "eraser".
  - XI_Motion / XI_ButtonPress / XI_ButtonRelease are selected for each pen
    device (not the masters) on every SDL window, so the core events the
    rest of the driver uses keep flowing. Button 1 is the tip (pen touch);
    buttons 2.. are pen buttons 1.. .
  - the master pointer turns the same pen input into core pointer events
    right after the slave event, with the same server time; those are
    dropped (SDLOP_X11_Pen_FilterCore) - the pen core synthesizes the
    SDL_PEN_MOUSEID mouse instead, exactly like stock, which drops pen
    events coming from the master device.
  - XI_HierarchyChanged re-enumerates the devices (tablet hotplug);
    "Wacom Serial IDs" changes report proximity in/out.

  SDLOP_X11_NO_PEN=1 turns it off (pens stay plain mice).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_PENS 16
#define AXIS_MISSING (-1)

typedef struct X11Pen
{
    uint16_t deviceid;
    SDL_PenID id;
    bool eraser;
    bool seen; /* enumeration mark */
    int valuator[SDL_PEN_AXIS_COUNT];
    double min[SDL_PEN_AXIS_COUNT];
    double max[SDL_PEN_AXIS_COUNT];
} X11Pen;

static struct
{
    bool active;
    uint8_t opcode;
    xcb_atom_t abs_pressure, abs_tilt_x, abs_tilt_y;
    xcb_atom_t wacom_serial_ids, wacom_tool_type, eraser;
    X11Pen *pens[MAX_PENS];
    int num_pens;
    /* server time of the last pen device event: core pointer events with
     * this time are the master's copy of it */
    xcb_timestamp_t core_time;
    bool core_time_valid;
} xp;

static xcb_atom_t intern(const char *name, bool only_if_exists)
{
    xcb_connection_t *c = SDLOP_X11_GetConnection();
    xcb_intern_atom_reply_t *r =
        X11_xcb_intern_atom_reply(c, X11_xcb_intern_atom(c, only_if_exists, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static double fp3232(xcb_input_fp3232_t v)
{
    return (double)v.integral + (double)v.frac / 4294967296.0;
}

static X11Pen *find_pen(uint16_t deviceid)
{
    for (int i = 0; i < xp.num_pens; i++) {
        if (xp.pens[i]->deviceid == deviceid) {
            return xp.pens[i];
        }
    }
    return NULL;
}

/* 32-bit integer device property into dest; number of items read */
static int get_int_property(uint16_t deviceid, xcb_atom_t prop, int32_t *dest, int max_items)
{
    if (prop == XCB_ATOM_NONE) {
        return 0;
    }
    xcb_connection_t *c = SDLOP_X11_GetConnection();
    xcb_input_xi_get_property_reply_t *r = X11_xcb_input_xi_get_property_reply(
        c, X11_xcb_input_xi_get_property(c, deviceid, 0, prop, XCB_ATOM_ANY, 0, (uint32_t)max_items), NULL);
    int n = 0;
    if (r && r->type == XCB_ATOM_INTEGER && r->num_items > 0) {
        const uint8_t *items = (const uint8_t *)(r + 1);
        const size_t avail = (size_t)r->length * 4;
        for (; n < max_items && n < (int)r->num_items; n++) {
            if (r->format == 32 && (size_t)(n + 1) * 4 <= avail) {
                memcpy(&dest[n], items + n * 4, 4);
            } else if (r->format == 16 && (size_t)(n + 1) * 2 <= avail) {
                int16_t v;
                memcpy(&v, items + n * 2, 2);
                dest[n] = v;
            } else if (r->format == 8 && (size_t)(n + 1) <= avail) {
                dest[n] = (int8_t)items[n];
            } else {
                break;
            }
        }
    }
    free(r);
    return n;
}

/* stock X11_XInput2PenIsInProximity: Wacom serial ids of the current tool */
static bool query_proximity(uint16_t deviceid, bool *in_proximity)
{
    int32_t ids[5];
    if (get_int_property(deviceid, xp.wacom_serial_ids, ids, 5) == 5) {
        *in_proximity = ids[3] != 0 || ids[4] != 0;
        return true;
    }
    return false;
}

static bool query_wacom_id(uint16_t deviceid, Uint32 *wacom_id)
{
    int32_t ids[5];
    if (get_int_property(deviceid, xp.wacom_serial_ids, ids, 5) >= 3) {
        *wacom_id = (Uint32)ids[2];
        return true;
    }
    return false;
}

static bool is_eraser(uint16_t deviceid, const char *name)
{
    if (xp.wacom_tool_type != XCB_ATOM_NONE) {
        xcb_connection_t *c = SDLOP_X11_GetConnection();
        xcb_input_xi_get_property_reply_t *r = X11_xcb_input_xi_get_property_reply(
            c, X11_xcb_input_xi_get_property(c, deviceid, 0, xp.wacom_tool_type, XCB_ATOM_ANY, 0, 32), NULL);
        if (r && r->num_items > 0) {
            const uint8_t *items = (const uint8_t *)(r + 1);
            const size_t avail = (size_t)r->length * 4;
            bool known = false, result = false;
            if (r->type == XCB_ATOM_ATOM && r->format == 32 && avail >= 4) {
                xcb_atom_t a;
                memcpy(&a, items, 4);
                known = a != XCB_ATOM_NONE;
                result = known && a == xp.eraser;
            } else if (r->type == XCB_ATOM_STRING && r->format == 8) {
                const size_t n = r->num_items < avail ? r->num_items : avail;
                known = true;
                result = n == 6 && strncasecmp((const char *)items, "eraser", 6) == 0;
            }
            free(r);
            if (known) {
                return result;
            }
        } else {
            free(r);
        }
    }
    return name && strcasestr(name, "eraser") != NULL;
}

static void select_pen_on(xcb_window_t xid, uint16_t deviceid, bool on)
{
    struct
    {
        xcb_input_event_mask_t head;
        uint32_t mask;
    } em;
    em.head.deviceid = deviceid;
    em.head.mask_len = 1;
    em.mask = on ? (XCB_INPUT_XI_EVENT_MASK_MOTION | XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS |
                    XCB_INPUT_XI_EVENT_MASK_BUTTON_RELEASE)
                 : 0;
    xcb_connection_t *c = SDLOP_X11_GetConnection();
    free(X11_xcb_request_check(c, X11_xcb_input_xi_select_events_checked(c, xid, 1, &em.head)));
}

static void select_pen_everywhere(uint16_t deviceid, bool on)
{
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        const xcb_window_t xid = SDLOP_X11_GetWindowXID(w);
        if (xid) {
            select_pen_on(xid, deviceid, on);
        }
    }
}

/* one xcb_input_xi_device_info_t (and its trailing name + classes, which
 * end at `end`): add it if it's a pen not known yet. Marks known pens. */
static void consider_device(const xcb_input_xi_device_info_t *info, const uint8_t *end)
{
    if ((info->type != XCB_INPUT_DEVICE_TYPE_SLAVE_POINTER && info->type != XCB_INPUT_DEVICE_TYPE_FLOATING_SLAVE) ||
        !info->enabled) {
        return;
    }
    const uint8_t *p = (const uint8_t *)(info + 1);
    if (p + info->name_len > end) {
        return;
    }
    char name[256];
    const size_t nlen = info->name_len < sizeof(name) - 1 ? info->name_len : sizeof(name) - 1;
    memcpy(name, p, nlen);
    name[nlen] = '\0';
    p += (info->name_len + 3u) & ~3u;

    X11Pen pen;
    memset(&pen, 0, sizeof(pen));
    for (int a = 0; a < SDL_PEN_AXIS_COUNT; a++) {
        pen.valuator[a] = AXIS_MISSING;
    }
    int buttons = 0;
    Uint32 caps = 0;
    for (int i = 0; i < info->num_classes; i++) {
        if (p + sizeof(xcb_input_device_class_t) > end) {
            return;
        }
        const xcb_input_device_class_t *cls = (const xcb_input_device_class_t *)p;
        const size_t clen = (size_t)cls->len * 4;
        if (clen < sizeof(*cls) || p + clen > end) {
            return;
        }
        if (cls->type == XCB_INPUT_DEVICE_CLASS_TYPE_BUTTON && clen >= 8) {
            uint16_t nb;
            memcpy(&nb, p + 6, 2);
            buttons += nb;
        } else if (cls->type == XCB_INPUT_DEVICE_CLASS_TYPE_VALUATOR && clen >= sizeof(xcb_input_valuator_class_t)) {
            xcb_input_valuator_class_t v;
            memcpy(&v, p, sizeof(v));
            int axis = -1;
            if (v.label != XCB_ATOM_NONE && v.label == xp.abs_pressure) {
                axis = SDL_PEN_AXIS_PRESSURE;
                caps |= SDLOP_PEN_CAPABILITY_PRESSURE;
            } else if (v.label != XCB_ATOM_NONE && v.label == xp.abs_tilt_x) {
                axis = SDL_PEN_AXIS_XTILT;
                caps |= SDLOP_PEN_CAPABILITY_XTILT;
            } else if (v.label != XCB_ATOM_NONE && v.label == xp.abs_tilt_y) {
                axis = SDL_PEN_AXIS_YTILT;
                caps |= SDLOP_PEN_CAPABILITY_YTILT;
            }
            if (axis >= 0) {
                pen.valuator[axis] = v.number;
                pen.min[axis] = fp3232(v.min);
                pen.max[axis] = fp3232(v.max);
            }
        }
        p += clen;
    }
    if (!(caps & SDLOP_PEN_CAPABILITY_PRESSURE)) {
        return; /* a pen if and only if it measures pressure (stock) */
    }
    X11Pen *known = find_pen(info->deviceid);
    if (known) {
        known->seen = true;
        return;
    }
    if (xp.num_pens >= MAX_PENS) {
        return;
    }
    X11Pen *h = (X11Pen *)malloc(sizeof(*h));
    if (!h) {
        return;
    }
    *h = pen;
    h->deviceid = info->deviceid;
    h->eraser = is_eraser(info->deviceid, name);
    h->seen = true;

    SDLop_PenInfo pi;
    memset(&pi, 0, sizeof(pi));
    pi.capabilities = caps | (h->eraser ? SDLOP_PEN_CAPABILITY_ERASER : 0);
    pi.max_tilt = -1.0f;
    pi.num_buttons = buttons;
    pi.subtype = h->eraser ? SDLOP_PEN_TYPE_ERASER : SDLOP_PEN_TYPE_PEN;
    pi.device_type = SDL_PEN_DEVICE_TYPE_UNKNOWN;
    query_wacom_id(info->deviceid, &pi.wacom_id);
    bool in_proximity;
    if (!query_proximity(info->deviceid, &in_proximity)) {
        in_proximity = true; /* can't tell: stock says it's in proximity */
    }
    h->id = SDLOP_AddPen(name, &pi, h, NULL, in_proximity, 0);
    if (!h->id) {
        free(h);
        return;
    }
    xp.pens[xp.num_pens++] = h;
    select_pen_everywhere(h->deviceid, true);
}

static void remove_pen_at(int i, bool deselect)
{
    X11Pen *h = xp.pens[i];
    if (deselect) {
        select_pen_everywhere(h->deviceid, false);
    }
    SDLOP_RemovePen(h->id, NULL, 0);
    free(h);
    xp.pens[i] = xp.pens[--xp.num_pens];
}

/* (re)scan all devices: add new pens, drop the ones that went away */
static void enumerate(void)
{
    xcb_connection_t *c = SDLOP_X11_GetConnection();
    xcb_generic_error_t *err = NULL;
    xcb_input_xi_query_device_reply_t *r =
        X11_xcb_input_xi_query_device_reply(c, X11_xcb_input_xi_query_device(c, XCB_INPUT_DEVICE_ALL), &err);
    if (!r) {
        SDL_SetError("x11 pen enumerate: no reply (error %d)", err ? err->error_code : -1);
        free(err);
        return;
    }
    int parsed = 0;
    for (int i = 0; i < xp.num_pens; i++) {
        xp.pens[i]->seen = false;
    }
    const uint8_t *p = (const uint8_t *)(r + 1);
    const uint8_t *end = (const uint8_t *)r + 32 + (size_t)r->length * 4;
    for (int i = 0; i < r->num_infos; i++) {
        if (p + sizeof(xcb_input_xi_device_info_t) > end) {
            break;
        }
        const xcb_input_xi_device_info_t *info = (const xcb_input_xi_device_info_t *)p;
        /* size of this entry: header, padded name, classes */
        const uint8_t *q = p + sizeof(*info) + ((info->name_len + 3u) & ~3u);
        for (int k = 0; k < info->num_classes && q + 4 <= end; k++) {
            uint16_t len;
            memcpy(&len, q + 2, 2);
            if (len == 0) {
                q = end;
                break;
            }
            q += (size_t)len * 4;
        }
        if (q > end) {
            break;
        }
        consider_device(info, q);
        parsed++;
        p = q;
    }
    const int num_infos = r->num_infos;
    free(r);
    int removed = 0;
    for (int i = 0; i < xp.num_pens; i++) {
        removed += !xp.pens[i]->seen;
    }
    SDL_SetError("x11 pen enumerate: %d/%d infos, %d pens, %d gone", parsed, num_infos, xp.num_pens, removed);
    for (int i = xp.num_pens - 1; i >= 0; i--) {
        if (!xp.pens[i]->seen) {
            remove_pen_at(i, false); /* the device is gone: nothing to deselect */
        }
    }
}

void SDLOP_X11_Pen_Init(uint8_t xi_opcode)
{
    memset(&xp, 0, sizeof(xp));
    if (SDLOP_HintFlag("SDLOP_X11_NO_PEN", false)) {
        return;
    }
    xp.active = true;
    xp.opcode = xi_opcode;
    /* the labels only exist once a driver created them: only_if_exists
     * keeps a pen-less server's atom table clean */
    xp.abs_pressure = intern("Abs Pressure", true);
    xp.abs_tilt_x = intern("Abs Tilt X", true);
    xp.abs_tilt_y = intern("Abs Tilt Y", true);
    xp.wacom_serial_ids = intern("Wacom Serial IDs", true);
    xp.wacom_tool_type = intern("Wacom Tool Type", true);
    xp.eraser = intern("ERASER", true);

    /* hotplug + Wacom proximity, for every device */
    struct
    {
        xcb_input_event_mask_t head;
        uint32_t mask;
    } em;
    em.head.deviceid = XCB_INPUT_DEVICE_ALL;
    em.head.mask_len = 1;
    em.mask = XCB_INPUT_XI_EVENT_MASK_HIERARCHY | XCB_INPUT_XI_EVENT_MASK_PROPERTY;
    xcb_connection_t *c = SDLOP_X11_GetConnection();
    free(X11_xcb_request_check(c, X11_xcb_input_xi_select_events_checked(c, SDLOP_X11_GetRoot(), 1, &em.head)));
    enumerate();
}

void SDLOP_X11_Pen_Quit(void)
{
    for (int i = 0; i < xp.num_pens; i++) {
        free(xp.pens[i]); /* SDLOP_RemoveAllPens drops them at video quit */
    }
    memset(&xp, 0, sizeof(xp));
}

void SDLOP_X11_Pen_WindowCreated(xcb_window_t xid)
{
    for (int i = 0; xp.active && i < xp.num_pens; i++) {
        select_pen_on(xid, xp.pens[i]->deviceid, true);
    }
}

bool SDLOP_X11_Pen_IsPenDevice(uint16_t deviceid)
{
    return xp.active && find_pen(deviceid) != NULL;
}

bool SDLOP_X11_Pen_FilterCore(xcb_timestamp_t time)
{
    return xp.active && xp.core_time_valid && time == xp.core_time;
}

/* stock X11_XInput2NormalizePenAxes for pressure and tilt */
static float normalize(const X11Pen *pen, int axis, double value)
{
    const double min = pen->min[axis], max = pen->max[axis];
    double v;
    if (min < 0.0) { /* min .. 0 .. max: 0 stays 0 */
        v = value < 0.0 ? value / -min : (max == 0.0 ? 0.0 : value / max);
    } else {
        v = max == 0.0 ? 0.0 : (value - min) / max;
    }
    return (float)v;
}

static void handle_device_event(const xcb_input_button_press_event_t *ev, uint16_t evtype, size_t size)
{
    if (ev->deviceid != ev->sourceid) {
        return; /* a master's copy (not selected, but be sure) */
    }
    X11Pen *pen = find_pen(ev->sourceid);
    if (!pen) {
        return;
    }
    xp.core_time = ev->time;
    xp.core_time_valid = true;
    SDL_Window *window = SDLOP_X11_WindowFromXID(ev->event);
    if (evtype == XCB_INPUT_MOTION) {
        SDLOP_SendPenMotion(0, pen->id, window, (float)ev->event_x / 65536.0f, (float)ev->event_y / 65536.0f);
        /* button mask, valuator mask, then one FP3232 per set valuator */
        const uint8_t *base = (const uint8_t *)(ev + 1);
        const size_t masks = ((size_t)ev->buttons_len + ev->valuators_len) * 4;
        if (sizeof(*ev) + masks > size) {
            return;
        }
        const uint8_t *vmask = base + (size_t)ev->buttons_len * 4;
        const uint8_t *values = base + masks;
        const int nbits = ev->valuators_len * 32;
        int vi = 0;
        double vals[64];
        bool have[64];
        for (int b = 0; b < nbits && b < 64; b++) {
            have[b] = (vmask[b / 8] >> (b % 8)) & 1;
            if (have[b]) {
                if (sizeof(*ev) + masks + (size_t)(vi + 1) * 8 > size) {
                    have[b] = false;
                    continue;
                }
                xcb_input_fp3232_t f;
                memcpy(&f, values + (size_t)vi * 8, 8);
                vals[b] = fp3232(f);
                vi++;
            }
        }
        for (int a = 0; a < SDL_PEN_AXIS_COUNT; a++) {
            const int v = pen->valuator[a];
            if (v != AXIS_MISSING && v < nbits && v < 64 && have[v]) {
                SDLOP_SendPenAxis(0, pen->id, window, (SDL_PenAxis)a, normalize(pen, a, vals[v]));
            }
        }
        return;
    }
    const bool down = evtype == XCB_INPUT_BUTTON_PRESS;
    if (ev->detail == 1) {
        SDLOP_SendPenTouch(0, pen->id, window, pen->eraser, down);
    } else if (ev->detail >= 2 && ev->detail <= 6) {
        SDLOP_SendPenButton(0, pen->id, window, (Uint8)(ev->detail - 1), down);
    }
}

bool SDLOP_X11_Pen_HandleEvent(const xcb_ge_generic_event_t *ge)
{
    if (!xp.active) {
        return false;
    }
    const size_t size = 32 + 4 + (size_t)ge->length * 4; /* + xcb's full_sequence */
    switch (ge->event_type) {
    case XCB_INPUT_MOTION:
    case XCB_INPUT_BUTTON_PRESS:
    case XCB_INPUT_BUTTON_RELEASE: {
        const xcb_input_button_press_event_t *ev = (const xcb_input_button_press_event_t *)ge;
        if (size < sizeof(*ev) || !find_pen(ev->sourceid)) {
            return false;
        }
        handle_device_event(ev, ge->event_type, size);
        return true;
    }
    case XCB_INPUT_HIERARCHY: {
        const xcb_input_hierarchy_event_t *he = (const xcb_input_hierarchy_event_t *)ge;
        if (he->flags & (XCB_INPUT_HIERARCHY_MASK_SLAVE_ADDED | XCB_INPUT_HIERARCHY_MASK_SLAVE_REMOVED |
                         XCB_INPUT_HIERARCHY_MASK_DEVICE_ENABLED | XCB_INPUT_HIERARCHY_MASK_DEVICE_DISABLED)) {
            enumerate();
        }
        return true;
    }
    case XCB_INPUT_PROPERTY: {
        const xcb_input_property_event_t *pe = (const xcb_input_property_event_t *)ge;
        X11Pen *pen = find_pen(pe->deviceid);
        bool in;
        if (pen && pe->property == xp.wacom_serial_ids && pe->what == XCB_INPUT_PROPERTY_FLAG_MODIFIED &&
            query_proximity(pe->deviceid, &in)) {
            SDLOP_SendPenProximity(0, pen->id, sdlop.mouse_focus, in);
        }
        return true;
    }
    default:
        return false;
    }
}
