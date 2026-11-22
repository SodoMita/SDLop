/*
  SDLop - pen / drawing-tablet input core (stock SDL_pen.c semantics).

  Backends register a pen when its tool shows up (Wayland tablet-v2 tool,
  XI2 stylus/eraser device) and report touch, motion, axes, buttons and
  proximity; this turns them into SDL_EVENT_PEN_* plus the stock
  emulations:
    - SDL_HINT_PEN_MOUSE_EVENTS (default on): the first touching pen drives
      a left-button mouse (which = SDL_PEN_MOUSEID), hovering pens move it,
      barrel buttons map to right/middle/X1/X2
    - SDL_HINT_PEN_TOUCH_EVENTS (default on): the touching pen is a finger
      on the "pen_input" touch device (SDL_PEN_TOUCHID)
  Single-threaded like the rest of SDLop's event delivery, so no lock.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

typedef struct SDLop_Pen
{
    SDL_PenID id;
    char *name;
    SDLop_PenInfo info;
    float axes[SDL_PEN_AXIS_COUNT];
    float x, y;
    SDL_PenInputFlags state;
    void *handle;
} SDLop_Pen;

static SDLop_Pen *pens;
static int num_pens;
static SDL_PenID pen_touching; /* the pen driving the emulated mouse/finger */
static SDL_PenID next_pen_id;
static bool pen_touch_device;

static SDLop_Pen *find_pen(SDL_PenID id)
{
    for (int i = 0; i < num_pens; i++) {
        if (pens[i].id == id) {
            return &pens[i];
        }
    }
    return NULL;
}

static bool pen_mouse_events(void)
{
    return SDLOP_GetStringBoolean(SDL_GetHint(SDL_HINT_PEN_MOUSE_EVENTS), true);
}

static bool pen_touch_events(void)
{
    return SDLOP_GetStringBoolean(SDL_GetHint(SDL_HINT_PEN_TOUCH_EVENTS), true);
}

/* the emulated touch device exists while there are pens to drive it */
static void update_touch_device(void)
{
    const bool want = num_pens > 0 && pen_touch_events();
    if (want && !pen_touch_device) {
        SDLOP_AddTouch(SDL_PEN_TOUCHID, SDL_TOUCH_DEVICE_DIRECT, "pen_input");
        pen_touch_device = true;
    } else if (!want && pen_touch_device) {
        SDLOP_RemoveTouch(SDL_PEN_TOUCHID);
        pen_touch_device = false;
    }
}

static Uint64 pen_ts(Uint64 timestamp_ns)
{
    return timestamp_ns ? SDLOP_MonoToSDLTicks(timestamp_ns) : SDL_GetTicksNS();
}

static void pen_finger(Uint32 type, SDL_Window *window, const SDLop_Pen *pen, Uint64 timestamp_ns)
{
    if (!window || window->w <= 0 || window->h <= 0 || !pen_touch_events()) {
        return;
    }
    update_touch_device();
    SDLOP_SendTouch(type, SDL_PEN_TOUCHID, SDL_BUTTON_LEFT, pen->x / (float)window->w, pen->y / (float)window->h,
                    0.0f, 0.0f, pen->axes[SDL_PEN_AXIS_PRESSURE], window->id, timestamp_ns);
}

SDL_PenID SDLOP_AddPen(const char *name, const SDLop_PenInfo *info, void *handle, SDL_Window *window,
                       bool in_proximity, Uint64 timestamp_ns)
{
    SDLop_Pen *grown = (SDLop_Pen *)SDL_realloc(pens, sizeof(*pens) * (size_t)(num_pens + 1));
    if (!grown) {
        return 0;
    }
    pens = grown;
    SDLop_Pen *pen = &pens[num_pens++];
    SDL_zerop(pen);
    pen->id = ++next_pen_id;
    if (pen->id == 0) {
        pen->id = ++next_pen_id;
    }
    pen->name = SDL_strdup(name ? name : "Unnamed pen");
    if (info) {
        pen->info = *info;
    }
    pen->handle = handle;
    const SDL_PenID id = pen->id;
    update_touch_device();
    if (in_proximity) {
        SDLOP_SendPenProximity(timestamp_ns, id, window, true);
    }
    return id;
}

void SDLOP_RemovePen(SDL_PenID id, SDL_Window *window, Uint64 timestamp_ns)
{
    if (!find_pen(id)) {
        return;
    }
    SDLOP_SendPenProximity(timestamp_ns, id, window, false); /* bye bye */
    SDLop_Pen *pen = find_pen(id);
    if (pen_touching == id) {
        pen_touching = 0;
    }
    SDL_free(pen->name);
    const int idx = (int)(pen - pens);
    SDL_memmove(&pens[idx], &pens[idx + 1], sizeof(*pens) * (size_t)(num_pens - idx - 1));
    if (--num_pens == 0) {
        SDL_free(pens);
        pens = NULL;
    }
    update_touch_device();
}

void SDLOP_RemoveAllPens(void)
{
    for (int i = 0; i < num_pens; i++) {
        SDL_free(pens[i].name);
    }
    SDL_free(pens);
    pens = NULL;
    num_pens = 0;
    pen_touching = 0;
    if (pen_touch_device) {
        SDLOP_RemoveTouch(SDL_PEN_TOUCHID);
        pen_touch_device = false;
    }
}

SDL_PenID SDLOP_FindPenByHandle(void *handle)
{
    for (int i = 0; i < num_pens; i++) {
        if (pens[i].handle == handle) {
            return pens[i].id;
        }
    }
    return 0;
}

void *SDLOP_GetPenHandle(SDL_PenID id)
{
    SDLop_Pen *pen = find_pen(id);
    return pen ? pen->handle : NULL;
}

SDL_PenInputFlags SDLOP_GetPenStatus(SDL_PenID id, float *axes, int num_axes)
{
    SDLop_Pen *pen = find_pen(id);
    if (!pen) {
        return 0;
    }
    if (axes && num_axes > 0) {
        SDL_memcpy(axes, pen->axes, sizeof(float) * (size_t)SDL_min(num_axes, SDL_PEN_AXIS_COUNT));
    }
    return pen->state;
}

void SDLOP_SendPenProximity(Uint64 timestamp_ns, SDL_PenID id, SDL_Window *window, bool in)
{
    SDLop_Pen *pen = find_pen(id);
    if (!pen || ((pen->state & SDL_PEN_INPUT_IN_PROXIMITY) != 0) == in) {
        return;
    }
    if (in) {
        pen->state |= SDL_PEN_INPUT_IN_PROXIMITY;
    } else {
        pen->state &= ~SDL_PEN_INPUT_IN_PROXIMITY;
    }
    const Uint32 type = in ? SDL_EVENT_PEN_PROXIMITY_IN : SDL_EVENT_PEN_PROXIMITY_OUT;
    if (SDL_EventEnabled(type)) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.pproximity.type = type;
        ev.pproximity.timestamp = pen_ts(timestamp_ns);
        ev.pproximity.windowID = window ? window->id : 0;
        ev.pproximity.which = id;
        ev.pproximity.pen_state = pen->state;
        ev.pproximity.device_type = pen->info.device_type;
        SDLOP_PushEventInternal(&ev);
    }
}

static void ensure_proximity(Uint64 timestamp_ns, SDLop_Pen *pen, SDL_Window *window)
{
    if (!(pen->state & SDL_PEN_INPUT_IN_PROXIMITY)) {
        SDLOP_SendPenProximity(timestamp_ns, pen->id, window, true);
    }
}

void SDLOP_SendPenTouch(Uint64 timestamp_ns, SDL_PenID id, SDL_Window *window, bool eraser, bool down)
{
    SDLop_Pen *pen = find_pen(id);
    if (!pen) {
        return;
    }
    SDL_PenInputFlags state = pen->state;
    if (down) {
        state |= SDL_PEN_INPUT_DOWN;
    } else {
        state &= ~SDL_PEN_INPUT_DOWN;
    }
    if (eraser) {
        state |= SDL_PEN_INPUT_ERASER_TIP;
    } else {
        state &= ~SDL_PEN_INPUT_ERASER_TIP;
    }
    if (state == pen->state) {
        return;
    }
    pen->state = state;
    const Uint32 type = down ? SDL_EVENT_PEN_DOWN : SDL_EVENT_PEN_UP;
    if (SDL_EventEnabled(type)) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.ptouch.type = type;
        ev.ptouch.timestamp = pen_ts(timestamp_ns);
        ev.ptouch.windowID = window ? window->id : 0;
        ev.ptouch.which = id;
        ev.ptouch.pen_state = state;
        ev.ptouch.x = pen->x;
        ev.ptouch.y = pen->y;
        ev.ptouch.eraser = eraser;
        ev.ptouch.down = down;
        ev.ptouch.device_type = pen->info.device_type;
        SDLOP_PushEventInternal(&ev);
    }
    if (window) {
        if (pen_mouse_events()) {
            if (down && !pen_touching) {
                SDLOP_SendSyntheticMouse(SDL_EVENT_MOUSE_BUTTON_DOWN, window, pen->x, pen->y, SDL_BUTTON_LEFT,
                                         SDL_PEN_MOUSEID, timestamp_ns);
            } else if (!down && pen_touching == id) {
                SDLOP_SendSyntheticMouse(SDL_EVENT_MOUSE_BUTTON_UP, window, pen->x, pen->y, SDL_BUTTON_LEFT,
                                         SDL_PEN_MOUSEID, timestamp_ns);
            }
        }
        if (!pen_touching || pen_touching == id) {
            pen_finger(down ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_FINGER_UP, window, pen, timestamp_ns);
        }
    }
    if (down && !pen_touching) {
        pen_touching = id;
    } else if (!down && pen_touching == id) {
        pen_touching = 0;
    }
}

void SDLOP_SendPenMotion(Uint64 timestamp_ns, SDL_PenID id, SDL_Window *window, float x, float y)
{
    SDLop_Pen *pen = find_pen(id);
    if (!pen) {
        return;
    }
    ensure_proximity(timestamp_ns, pen, window);
    if (pen->x == x && pen->y == y) {
        return;
    }
    pen->x = x;
    pen->y = y;
    if (SDL_EventEnabled(SDL_EVENT_PEN_MOTION)) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.pmotion.type = SDL_EVENT_PEN_MOTION;
        ev.pmotion.timestamp = pen_ts(timestamp_ns);
        ev.pmotion.windowID = window ? window->id : 0;
        ev.pmotion.which = id;
        ev.pmotion.pen_state = pen->state;
        ev.pmotion.x = x;
        ev.pmotion.y = y;
        ev.pmotion.device_type = pen->info.device_type;
        SDLOP_PushEventInternal(&ev);
    }
    if (!window) {
        return;
    }
    /* hovering pens move the mouse too, as long as none is touching */
    if ((pen_touching == id || pen_touching == 0) && pen_mouse_events()) {
        SDLOP_SendSyntheticMouse(SDL_EVENT_MOUSE_MOTION, window, x, y, SDL_BUTTON_LEFT, SDL_PEN_MOUSEID,
                                 timestamp_ns);
    }
    if (pen_touching == id) {
        pen_finger(SDL_EVENT_FINGER_MOTION, window, pen, timestamp_ns);
    }
}

void SDLOP_SendPenAxis(Uint64 timestamp_ns, SDL_PenID id, SDL_Window *window, SDL_PenAxis axis, float value)
{
    SDLop_Pen *pen = find_pen(id);
    if (!pen || axis < 0 || axis >= SDL_PEN_AXIS_COUNT || pen->axes[axis] == value) {
        return;
    }
    pen->axes[axis] = value;
    if (SDL_EventEnabled(SDL_EVENT_PEN_AXIS)) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.paxis.type = SDL_EVENT_PEN_AXIS;
        ev.paxis.timestamp = pen_ts(timestamp_ns);
        ev.paxis.windowID = window ? window->id : 0;
        ev.paxis.which = id;
        ev.paxis.pen_state = pen->state;
        ev.paxis.x = pen->x;
        ev.paxis.y = pen->y;
        ev.paxis.axis = axis;
        ev.paxis.value = value;
        ev.paxis.device_type = pen->info.device_type;
        SDLOP_PushEventInternal(&ev);
    }
    if (window && axis == SDL_PEN_AXIS_PRESSURE && pen_touching == id) {
        pen_finger(SDL_EVENT_FINGER_MOTION, window, pen, timestamp_ns);
    }
}

void SDLOP_SendPenButton(Uint64 timestamp_ns, SDL_PenID id, SDL_Window *window, Uint8 button, bool down)
{
    SDLop_Pen *pen = find_pen(id);
    if (!pen || button < 1 || button > 5) {
        return;
    }
    ensure_proximity(timestamp_ns, pen, window);
    const SDL_PenInputFlags flag = (SDL_PenInputFlags)(1u << button);
    if (((pen->state & flag) != 0) == down) {
        return;
    }
    if (down) {
        pen->state |= flag;
    } else {
        pen->state &= ~flag;
    }
    const Uint32 type = down ? SDL_EVENT_PEN_BUTTON_DOWN : SDL_EVENT_PEN_BUTTON_UP;
    if (!SDL_EventEnabled(type)) {
        return;
    }
    SDL_Event ev;
    SDL_zero(ev);
    ev.pbutton.type = type;
    ev.pbutton.timestamp = pen_ts(timestamp_ns);
    ev.pbutton.windowID = window ? window->id : 0;
    ev.pbutton.which = id;
    ev.pbutton.pen_state = pen->state;
    ev.pbutton.x = pen->x;
    ev.pbutton.y = pen->y;
    ev.pbutton.button = button;
    ev.pbutton.down = down;
    ev.pbutton.device_type = pen->info.device_type;
    SDLOP_PushEventInternal(&ev);
    if (window && (!pen_touching || pen_touching == id) && pen_mouse_events()) {
        /* stock table, indexed by pen button number */
        static const Uint8 mouse_buttons[] = { SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1,
                                               SDL_BUTTON_X2 };
        if (button < SDL_arraysize(mouse_buttons)) {
            SDLOP_SendSyntheticMouse(down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP, window, pen->x,
                                     pen->y, mouse_buttons[button], SDL_PEN_MOUSEID, timestamp_ns);
        }
    }
}

SDL_PenDeviceType SDL_GetPenDeviceType(SDL_PenID instance_id)
{
    SDLop_Pen *pen = find_pen(instance_id);
    return pen ? pen->info.device_type : SDL_PEN_DEVICE_TYPE_INVALID;
}
