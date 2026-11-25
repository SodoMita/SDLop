/*
  SDLop - mouse handling.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <strings.h>

static SDL_WindowID mouse_window_id(void)
{
    if (sdlop.mouse_focus) {
        return sdlop.mouse_focus->id;
    }
    if (sdlop.keyboard_focus) {
        return sdlop.keyboard_focus->id;
    }
    return sdlop.windows ? sdlop.windows->id : 0;
}

/* Mirrors stock SDL3's ConstrainMousePosition: the reported position lives
   inside the window (or its SDL_SetWindowMouseRect confine), and going past
   the far edge keeps the larger of the edge and the previous position so
   deltas stay continuous. */
void SDLOP_ConstrainMouse(SDL_Window *w, float *x, float *y)
{
    if (!w || (w->flags & SDL_WINDOW_MOUSE_CAPTURE)) {
        return;
    }
    int x_min = 0, x_max = w->w - 1;
    int y_min = 0, y_max = w->h - 1;
    if (!SDL_RectEmpty(&w->mouse_rect)) {
        SDL_Rect window_rect = { 0, 0, x_max + 1, y_max + 1 };
        SDL_Rect r;
        if (SDL_GetRectIntersection(&w->mouse_rect, &window_rect, &r)) {
            x_min = r.x;
            y_min = r.y;
            x_max = x_min + r.w - 1;
            y_max = y_min + r.h - 1;
        }
    }
    if (*x >= (float)(x_max + 1)) {
        *x = SDL_max((float)x_max, sdlop.mouse_last_x);
    }
    if (*x < (float)x_min) {
        *x = (float)x_min;
    }
    if (*y >= (float)(y_max + 1)) {
        *y = SDL_max((float)y_max, sdlop.mouse_last_y);
    }
    if (*y < (float)y_min) {
        *y = (float)y_min;
    }
}

static void constrain_mouse_pos(float *x, float *y)
{
    SDLOP_ConstrainMouse(sdlop.mouse_focus, x, y);
}

static SDL_MouseMotionTransformCallback relative_transform;
static void *relative_transform_data;

void SDLOP_SendMouseMotion(float x, float y, float xrel, float yrel, Uint64 timestamp_ns)
{
    if (x == SDLOP_NO_POS) {
        /* relative-only motion (relative-pointer / evdev / locked pointer) */
        if (xrel == 0.0f && yrel == 0.0f) {
            return;
        }
        if (relative_transform && sdlop.relative_mode_window) {
            relative_transform(relative_transform_data,
                               timestamp_ns ? SDLOP_MonoToSDLTicks(timestamp_ns) : SDL_GetTicksNS(),
                               sdlop.mouse_focus ? sdlop.mouse_focus : sdlop.relative_mode_window, 0, &xrel, &yrel);
        }
        if (sdlop.mouse_has_position) {
            float fx = sdlop.mouse_x + xrel;
            float fy = sdlop.mouse_y + yrel;
            constrain_mouse_pos(&fx, &fy);
            sdlop.mouse_x = fx;
            sdlop.mouse_y = fy;
            sdlop.mouse_last_x = fx;
            sdlop.mouse_last_y = fy;
        }
    } else {
        /* absolute motion: clamp first, then derive deltas (stock order);
           explicit xrel/yrel from injection paths are honored as-is */
        float fx = x;
        float fy = y;
        constrain_mouse_pos(&fx, &fy);
        if (xrel == 0.0f && yrel == 0.0f) {
            if (sdlop.mouse_has_position) {
                xrel = fx - sdlop.mouse_last_x;
                yrel = fy - sdlop.mouse_last_y;
            }
            if (sdlop.mouse_has_position && xrel == 0.0f && yrel == 0.0f) {
                return; /* drop events that don't change state (stock) */
            }
        }
        sdlop.mouse_x = fx;
        sdlop.mouse_y = fy;
        sdlop.mouse_last_x = fx;
        sdlop.mouse_last_y = fy;
        sdlop.mouse_has_position = true;
    }
    if (xrel != 0.0f || yrel != 0.0f) {
        sdlop.mouse_xrel_acc += xrel;
        sdlop.mouse_yrel_acc += yrel;
    }

    SDL_Event event;
    SDL_zero(event);
    event.motion.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.timestamp = timestamp_ns ? SDLOP_MonoToSDLTicks(timestamp_ns) : SDL_GetTicksNS();
    event.motion.windowID = mouse_window_id();
    event.motion.which = 0;
    event.motion.state = sdlop.mouse_buttons;
    event.motion.x = sdlop.mouse_x;
    event.motion.y = sdlop.mouse_y;
    event.motion.xrel = xrel;
    event.motion.yrel = yrel;
    SDLOP_PushInputEvent(&event);
}

void SDLOP_SendMouseButton(bool down, Uint8 button, float x, float y, Uint64 timestamp_ns)
{
    if (button < 1 || button > 31) {
        return;
    }
    if (x != SDLOP_NO_POS) {
        float fx = x;
        float fy = y;
        constrain_mouse_pos(&fx, &fy);
        sdlop.mouse_x = fx;
        sdlop.mouse_y = fy;
        sdlop.mouse_last_x = fx;
        sdlop.mouse_last_y = fy;
        sdlop.mouse_has_position = true;
    }

    SDL_MouseButtonFlags mask = SDL_BUTTON_MASK(button);
    if (down) {
        sdlop.mouse_buttons |= mask;
    } else {
        sdlop.mouse_buttons &= ~mask;
    }

    SDL_Event event;
    SDL_zero(event);
    event.button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.timestamp = timestamp_ns ? SDLOP_MonoToSDLTicks(timestamp_ns) : SDL_GetTicksNS();
    event.button.windowID = mouse_window_id();
    event.button.which = 0;
    event.button.button = button;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = sdlop.mouse_x;
    event.button.y = sdlop.mouse_y;
    SDLOP_PushInputEvent(&event);
}

/* ---- synthetic mouse events from touch (SDL3 SDL_HINT_TOUCH_MOUSE_EVENTS,
 * default on): the first finger down drives a left-button mouse with
 * which = SDL_TOUCH_MOUSEID; further fingers only produce FINGER_* events.
 * Window systems do not emulate the pointer once a client takes touch
 * events (XI2.2 touch selection, wl_touch), so this is what keeps
 * mouse-only apps usable on touchscreens. */
static struct
{
    bool touching;
    SDL_TouchID touch;
    SDL_FingerID finger;
    SDL_WindowID window;
} touch_mouse;

static bool touch_mouse_enabled(void)
{
    return SDLOP_GetStringBoolean(SDL_GetHint(SDL_HINT_TOUCH_MOUSE_EVENTS), true);
}

void SDLOP_SendSyntheticMouse(Uint32 type, SDL_Window *window, float fx, float fy, Uint8 button,
                              SDL_MouseID which, Uint64 timestamp_ns)
{
    if (sdlop.mouse_focus != window && type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        /* SDL_UpdateMouseFocus: the touched window gets the mouse focus */
        if (sdlop.mouse_focus) {
            sdlop.mouse_focus->flags &= ~SDL_WINDOW_MOUSE_FOCUS;
            SDLOP_SendWindowEvent(sdlop.mouse_focus, SDL_EVENT_WINDOW_MOUSE_LEAVE, 0, 0);
        }
        sdlop.mouse_focus = window;
        window->flags |= SDL_WINDOW_MOUSE_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_ENTER, 0, 0);
        sdlop.mouse_has_position = false;
    }

    float xrel = sdlop.mouse_has_position ? fx - sdlop.mouse_last_x : 0.0f;
    float yrel = sdlop.mouse_has_position ? fy - sdlop.mouse_last_y : 0.0f;
    bool moved = !sdlop.mouse_has_position || xrel != 0.0f || yrel != 0.0f;
    sdlop.mouse_x = sdlop.mouse_last_x = fx;
    sdlop.mouse_y = sdlop.mouse_last_y = fy;
    sdlop.mouse_has_position = true;

    SDL_Event event;
    Uint64 ts = timestamp_ns ? SDLOP_MonoToSDLTicks(timestamp_ns) : SDL_GetTicksNS();
    if (moved) {
        sdlop.mouse_xrel_acc += xrel;
        sdlop.mouse_yrel_acc += yrel;
        SDL_zero(event);
        event.motion.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.timestamp = ts;
        event.motion.windowID = window->id;
        event.motion.which = which;
        event.motion.state = sdlop.mouse_buttons;
        event.motion.x = fx;
        event.motion.y = fy;
        event.motion.xrel = xrel;
        event.motion.yrel = yrel;
        SDLOP_PushInputEvent(&event);
    }
    if (type == SDL_EVENT_MOUSE_MOTION) {
        return;
    }
    bool down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (down) {
        sdlop.mouse_buttons |= SDL_BUTTON_MASK(button);
    } else {
        sdlop.mouse_buttons &= ~SDL_BUTTON_MASK(button);
    }
    SDL_zero(event);
    event.button.type = type;
    event.button.timestamp = ts;
    event.button.windowID = window->id;
    event.button.which = which;
    event.button.button = button;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = fx;
    event.button.y = fy;
    SDLOP_PushInputEvent(&event);
}

static void touch_mouse_event(Uint32 type, SDL_Window *window, float x, float y, Uint64 timestamp_ns)
{
    /* SDL3: position = normalized * window size, clamped into the window */
    float fx = x * (float)window->w;
    float fy = y * (float)window->h;
    fx = SDL_max(0.0f, SDL_min(fx, (float)(window->w - 1)));
    fy = SDL_max(0.0f, SDL_min(fy, (float)(window->h - 1)));
    SDLOP_SendSyntheticMouse(type, window, fx, fy, SDL_BUTTON_LEFT, SDL_TOUCH_MOUSEID, timestamp_ns);
}

static void touch_to_mouse(Uint32 type, SDL_TouchID touchID, SDL_FingerID fingerID, float x, float y,
                           SDL_WindowID windowID, Uint64 timestamp_ns)
{
    bool tracked = touch_mouse.touching && touch_mouse.touch == touchID && touch_mouse.finger == fingerID;
    if (type == SDL_EVENT_FINGER_DOWN) {
        if (touch_mouse.touching || !touch_mouse_enabled()) {
            return;
        }
        SDL_Window *window = SDL_GetWindowFromID(windowID);
        if (!window || window->w <= 0 || window->h <= 0) {
            return;
        }
        touch_mouse.touching = true;
        touch_mouse.touch = touchID;
        touch_mouse.finger = fingerID;
        touch_mouse.window = windowID;
        touch_mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, window, x, y, timestamp_ns);
        return;
    }
    if (!tracked) {
        return;
    }
    /* the sequence stays with the window it started on */
    SDL_Window *window = SDL_GetWindowFromID(touch_mouse.window);
    if (type == SDL_EVENT_FINGER_MOTION) {
        if (window && window->w > 0 && window->h > 0) {
            touch_mouse_event(SDL_EVENT_MOUSE_MOTION, window, x, y, timestamp_ns);
        }
        return;
    }
    /* FINGER_UP / FINGER_CANCELED: always release (cancel carries no window) */
    touch_mouse.touching = false;
    if (window && window->w > 0 && window->h > 0) {
        if (type == SDL_EVENT_FINGER_CANCELED) {
            x = window->w > 1 ? sdlop.mouse_x / (float)window->w : 0.0f;
            y = window->h > 1 ? sdlop.mouse_y / (float)window->h : 0.0f;
        }
        touch_mouse_event(SDL_EVENT_MOUSE_BUTTON_UP, window, x, y, timestamp_ns);
    } else {
        sdlop.mouse_buttons &= ~SDL_BUTTON_LMASK;
    }
}

void SDLOP_ResetTouchMouse(void)
{
    SDL_zero(touch_mouse);
}

void SDLOP_SendTouch(Uint32 type, Uint64 touchID, Uint64 fingerID, float x, float y,
                     float dx, float dy, float pressure, SDL_WindowID windowID,
                     Uint64 timestamp_ns)
{
    SDLOP_TrackTouch(type, touchID, fingerID, x, y, pressure);
    /* SDL3 order: the synthetic mouse events precede the finger event; pens
     * make their own mouse events (SDL_HINT_PEN_MOUSE_EVENTS) */
    if (touchID != SDL_PEN_TOUCHID) {
        touch_to_mouse(type, touchID, fingerID, x, y, windowID, timestamp_ns);
    }

    SDL_Event event;
    SDL_zero(event);
    event.tfinger.type = type;
    event.tfinger.timestamp = timestamp_ns ? SDLOP_MonoToSDLTicks(timestamp_ns) : SDL_GetTicksNS();
    event.tfinger.touchID = touchID;
    event.tfinger.fingerID = fingerID;
    event.tfinger.x = x;
    event.tfinger.y = y;
    event.tfinger.dx = dx;
    event.tfinger.dy = dy;
    event.tfinger.pressure = pressure;
    event.tfinger.windowID = windowID;
    SDLOP_PushInputEvent(&event);
}

/* stock SDL_SendPinch: the backends pass the protocol's scale through
 * (Wayland and XI 2.4 both report it relative to the gesture start) */
void SDLOP_SendPinch(Uint32 type, Uint64 timestamp_ns, SDL_Window *window, float scale)
{
    if (!SDL_EventEnabled(type)) {
        return;
    }
    SDL_Event event;
    SDL_zero(event);
    event.pinch.type = type;
    event.pinch.timestamp = timestamp_ns ? SDLOP_MonoToSDLTicks(timestamp_ns) : SDL_GetTicksNS();
    event.pinch.scale = scale;
    event.pinch.windowID = window ? window->id : 0;
    SDLOP_PushEventInternal(&event);
}


void SDLOP_SendMouseWheel(float x, float y, Uint64 timestamp_ns)
{
    SDLOP_SendMouseWheelDirection(x, y, SDL_MOUSEWHEEL_NORMAL, timestamp_ns);
}

/* FLIPPED: natural scrolling; like stock the values are not negated, the
 * application multiplies by -1 if it wants the physical direction */
void SDLOP_SendMouseWheelDirection(float x, float y, SDL_MouseWheelDirection direction, Uint64 timestamp_ns)
{
    if (x == 0.0f && y == 0.0f) {
        return;
    }
    SDL_Event event;
    SDL_zero(event);
    event.wheel.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.timestamp = timestamp_ns ? SDLOP_MonoToSDLTicks(timestamp_ns) : SDL_GetTicksNS();
    event.wheel.windowID = mouse_window_id();
    event.wheel.which = 0;
    event.wheel.x = x;
    event.wheel.y = y;
    event.wheel.direction = direction;
    event.wheel.mouse_x = sdlop.mouse_x;
    event.wheel.mouse_y = sdlop.mouse_y;
    SDLOP_PushInputEvent(&event);
}

SDL_Window *SDL_GetMouseFocus(void)
{
    return sdlop.mouse_focus;
}

SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y)
{
    if (x) {
        *x = sdlop.mouse_x;
    }
    if (y) {
        *y = sdlop.mouse_y;
    }
    return sdlop.mouse_buttons;
}

SDL_MouseButtonFlags SDL_GetRelativeMouseState(float *x, float *y)
{
    if (x) {
        *x = sdlop.mouse_xrel_acc;
        sdlop.mouse_xrel_acc = 0.0f;
    }
    if (y) {
        *y = sdlop.mouse_yrel_acc;
        sdlop.mouse_yrel_acc = 0.0f;
    }
    return sdlop.mouse_buttons;
}

bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    SDL_Window *prev_mode_window = sdlop.relative_mode_window;
    Uint64 prev_flags = window->flags;

    if (enabled) {
        sdlop.relative_mode_window = window;
        window->flags |= SDL_WINDOW_MOUSE_RELATIVE_MODE;
    } else {
        if (sdlop.relative_mode_window == window) {
            sdlop.relative_mode_window = NULL;
        }
        window->flags &= ~SDL_WINDOW_MOUSE_RELATIVE_MODE;
    }
    if (sdlop.video && sdlop.video->SetWindowRelativeMouseMode &&
        !sdlop.video->SetWindowRelativeMouseMode(sdlop.video, window, enabled)) {
        /* driver refused (e.g. compositor lacks pointer-constraints);
         * restore the previous state like SDL3 does */
        sdlop.relative_mode_window = prev_mode_window;
        window->flags = prev_flags;
        return false;
    }
    if (window->flags != prev_flags) {
        /* relative mode implies a mouse grab and a hidden cursor */
        SDLOP_UpdateWindowGrab(window);
        SDLOP_RedrawCursor();
    }
    return true;
}

bool SDL_GetWindowRelativeMouseMode(SDL_Window *window)
{
    if (!window) {
        SDL_SetError("Invalid window");
        return false;
    }
    return (window->flags & SDL_WINDOW_MOUSE_RELATIVE_MODE) != 0;
}

bool SDL_SetRelativeMouseTransform(SDL_MouseMotionTransformCallback transform, void *userdata)
{
    if (sdlop.relative_mode_window) {
        return SDL_SetError("Can't set mouse transform while relative mode is active");
    }
    relative_transform = transform;
    relative_transform_data = userdata;
    return true;
}

SDL_MouseButtonFlags SDL_GetGlobalMouseState(float *x, float *y)
{
    if (sdlop.video && sdlop.video->GetGlobalMouseState) {
        float tmpx, tmpy;
        if (!x) {
            x = &tmpx;
        }
        if (!y) {
            y = &tmpy;
        }
        *x = *y = 0.0f;
        return sdlop.video->GetGlobalMouseState(sdlop.video, x, y);
    }
    return SDL_GetMouseState(x, y);
}

void SDL_WarpMouseInWindow(SDL_Window *window, float x, float y)
{
    if (!window) {
        window = sdlop.mouse_focus;
    }
    if (!window || (window->flags & SDL_WINDOW_MINIMIZED)) {
        return;
    }
    /* ignore the previous position when we warp */
    sdlop.mouse_last_x = x;
    sdlop.mouse_last_y = y;
    sdlop.mouse_has_position = false;

    if (sdlop.relative_mode_window &&
        !SDL_GetHintBoolean(SDL_HINT_MOUSE_RELATIVE_WARP_MOTION, false)) {
        /* relative mode: adjust the absolute position, no motion event */
        sdlop.mouse_x = x;
        sdlop.mouse_y = y;
        sdlop.mouse_has_position = true;
        return;
    }
    if (sdlop.video && sdlop.video->WarpMouse && !sdlop.relative_mode_window &&
        sdlop.video->WarpMouse(sdlop.video, window, x, y)) {
        return; /* the window system reports the motion */
    }
    /* synthetic motion (SDL_PrivateSendMouseMotion(window, ...)): the
     * event is reported against the warped window even when the pointer
     * focus is elsewhere; focus itself is left to the window system */
    SDL_Window *old_focus = sdlop.mouse_focus;
    sdlop.mouse_focus = window;
    SDLOP_SendMouseMotion(x, y, 0.0f, 0.0f, 0);
    sdlop.mouse_focus = old_focus;
}

bool SDL_WarpMouseGlobal(float x, float y)
{
    if (sdlop.video && sdlop.video->WarpMouseGlobal) {
        return sdlop.video->WarpMouseGlobal(sdlop.video, x, y);
    }
    return SDL_Unsupported();
}

/* ---- mouse capture (SDL_CaptureMouse) ---- */
static bool capture_desired;
static SDL_Window *capture_window;

static bool update_mouse_capture(bool force_release)
{
    if (!sdlop.video || !sdlop.video->CaptureMouse) {
        return true;
    }
    SDL_Window *want = NULL;
    if (!force_release && capture_desired && !sdlop.relative_mode_window) {
        want = sdlop.mouse_focus;
    }
    if (want != capture_window) {
        SDL_Window *previous = capture_window;
        if (previous) {
            previous->flags &= ~SDL_WINDOW_MOUSE_CAPTURE;
        }
        if (want) {
            want->flags |= SDL_WINDOW_MOUSE_CAPTURE;
        }
        capture_window = want;
        if (!sdlop.video->CaptureMouse(sdlop.video, want)) {
            if (previous) {
                previous->flags |= SDL_WINDOW_MOUSE_CAPTURE;
            }
            if (want) {
                want->flags &= ~SDL_WINDOW_MOUSE_CAPTURE;
            }
            capture_window = previous;
            return false;
        }
    }
    return true;
}

bool SDL_CaptureMouse(bool enabled)
{
    if (!sdlop.video || !sdlop.video->CaptureMouse) {
        return SDL_Unsupported();
    }
    if (enabled && SDL_GetKeyboardFocus() == NULL) {
        return SDL_SetError("No window has focus");
    }
    capture_desired = enabled;
    return update_mouse_capture(false);
}

/* ------------------------------------------------------------------ */
/* cursors                                                             */
/* ------------------------------------------------------------------ */

static SDL_Cursor *cursors;      /* every live cursor (incl. animation frames) */
static SDL_Cursor *def_cursor;
static SDL_Cursor *cur_cursor;
static bool cursor_hidden;       /* SDL_HideCursor */
static bool cursors_initialized;

static SDL_Cursor *alloc_cursor(SDL_SystemCursor system_id, SDL_Surface *argb, int hot_x, int hot_y)
{
    SDL_Cursor *cursor = (SDL_Cursor *)SDL_calloc(1, sizeof(*cursor));
    if (!cursor) {
        return NULL;
    }
    cursor->system_id = system_id;
    cursor->hot_x = hot_x;
    cursor->hot_y = hot_y;
    if (argb) {
        cursor->surface = SDL_DuplicateSurface(argb);
        if (!cursor->surface) {
            SDL_free(cursor);
            return NULL;
        }
    }
    if (sdlop.video && sdlop.video->CreateCursor && !sdlop.video->CreateCursor(sdlop.video, cursor)) {
        SDL_DestroySurface(cursor->surface);
        SDL_free(cursor);
        return NULL;
    }
    return cursor;
}

static void free_cursor(SDL_Cursor *cursor)
{
    if (sdlop.video && sdlop.video->FreeCursor) {
        sdlop.video->FreeCursor(sdlop.video, cursor);
    }
    for (int i = 0; i < cursor->num_frames; ++i) {
        free_cursor(cursor->frames[i]);
    }
    SDL_free(cursor->frames);
    SDL_free(cursor->durations);
    SDL_DestroySurface(cursor->surface);
    SDL_free(cursor);
}

static void link_cursor(SDL_Cursor *cursor)
{
    cursor->next = cursors;
    cursors = cursor;
}

static void ensure_cursors(void)
{
    if (cursors_initialized || !sdlop.video) {
        return;
    }
    cursors_initialized = true;
    cursor_hidden = false;
    SDL_Cursor *c = NULL;
    if (sdlop.video->CreateCursor) {
        c = alloc_cursor(SDL_SYSTEM_CURSOR_DEFAULT, NULL, 0, 0);
    }
    if (!c) {
        /* stock SDL_PostInitMouse: a blank 1x1 cursor keeps the API sane */
        SDL_Surface *surface = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_ARGB8888);
        if (surface) {
            SDL_memset(surface->pixels, 0, (size_t)surface->h * (size_t)surface->pitch);
            c = alloc_cursor(SDL_SYSTEM_CURSOR_COUNT, surface, 0, 0);
            SDL_DestroySurface(surface);
        }
    }
    def_cursor = cur_cursor = c;
}

SDL_Cursor *SDLOP_GetCurrentCursor(void)
{
    ensure_cursors();
    SDL_Cursor *cursor = sdlop.mouse_focus ? cur_cursor : def_cursor;
    if (sdlop.mouse_focus && (cursor_hidden || sdlop.relative_mode_window)) {
        cursor = NULL;
    }
    if (cursor && cursor->num_frames > 0) {
        cursor = cursor->frames[cursor->current_frame];
    }
    return cursor;
}

bool SDLOP_CursorVisible(void)
{
    return !cursor_hidden;
}

void SDLOP_RedrawCursor(void)
{
    if (sdlop.video && sdlop.video->ShowCursor) {
        sdlop.video->ShowCursor(sdlop.video, SDLOP_GetCurrentCursor());
    }
}

void SDLOP_UpdateCursorAnimation(void)
{
    SDL_Cursor *cursor = cur_cursor;
    if (!cursor || cursor->num_frames <= 0 || !sdlop.mouse_focus) {
        return;
    }
    Uint32 duration = cursor->durations[cursor->current_frame];
    if (!duration) {
        return; /* stop frame */
    }
    Uint64 now = SDL_GetTicks();
    if (now < cursor->last_update + duration) {
        return;
    }
    cursor->current_frame = (cursor->current_frame + 1) % cursor->num_frames;
    cursor->last_update = now;
    SDLOP_RedrawCursor();
}

void SDLOP_MouseQuit(void)
{
    update_mouse_capture(true);
    capture_desired = false;
    capture_window = NULL;
    if (sdlop.video && sdlop.video->ShowCursor && cursors_initialized) {
        sdlop.video->ShowCursor(sdlop.video, NULL);
    }
    while (cursors) {
        SDL_Cursor *next = cursors->next;
        free_cursor(cursors);
        cursors = next;
    }
    if (def_cursor) {
        free_cursor(def_cursor);
    }
    def_cursor = cur_cursor = NULL;
    cursors_initialized = false;
    cursor_hidden = false;
    relative_transform = NULL;
    relative_transform_data = NULL;
}

SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *surface, int hot_x, int hot_y)
{
    if (!surface) {
        SDL_InvalidParamError("surface");
        return NULL;
    }
    SDL_PropertiesID props = SDL_GetSurfaceProperties(surface);
    hot_x = (int)SDL_GetNumberProperty(props, SDL_PROP_SURFACE_HOTSPOT_X_NUMBER, hot_x);
    hot_y = (int)SDL_GetNumberProperty(props, SDL_PROP_SURFACE_HOTSPOT_Y_NUMBER, hot_y);
    if (hot_x < 0 || hot_y < 0 || hot_x >= surface->w || hot_y >= surface->h) {
        SDL_SetError("Cursor hot spot doesn't lie within cursor");
        return NULL;
    }
    ensure_cursors();
    SDL_Surface *temp = NULL;
    if (surface->format != SDL_PIXELFORMAT_ARGB8888) {
        temp = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
        if (!temp) {
            return NULL;
        }
        surface = temp;
    }
    SDL_Cursor *cursor = alloc_cursor(SDL_SYSTEM_CURSOR_COUNT, surface, hot_x, hot_y);
    if (cursor) {
        link_cursor(cursor);
    }
    SDL_DestroySurface(temp);
    return cursor;
}

SDL_Cursor *SDL_CreateCursor(const Uint8 *data, const Uint8 *mask, int w, int h, int hot_x, int hot_y)
{
    const Uint32 black = 0xFF000000;
    const Uint32 white = 0xFFFFFFFF;
    const Uint32 transparent = 0x00000000;
    const Uint32 inverted = 0xFF000000; /* only Win32 can invert */
    Uint8 datab = 0, maskb = 0;

    w = ((w + 7) & ~7);
    SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        return NULL;
    }
    for (int y = 0; y < h; ++y) {
        Uint32 *pixels = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
        for (int x = 0; x < w; ++x) {
            if ((x % 8) == 0) {
                datab = *data++;
                maskb = *mask++;
            }
            if (maskb & 0x80) {
                *pixels++ = (datab & 0x80) ? black : white;
            } else {
                *pixels++ = (datab & 0x80) ? inverted : transparent;
            }
            datab <<= 1;
            maskb <<= 1;
        }
    }
    SDL_Cursor *cursor = SDL_CreateColorCursor(surface, hot_x, hot_y);
    SDL_DestroySurface(surface);
    return cursor;
}

SDL_Cursor *SDL_CreateAnimatedCursor(SDL_CursorFrameInfo *frames, int frame_count, int hot_x, int hot_y)
{
    if (!frames) {
        SDL_InvalidParamError("frames");
        return NULL;
    }
    if (!frames[0].surface) {
        SDL_SetError("NULL surface in frame 0");
        return NULL;
    }
    if (frame_count <= 0) {
        SDL_InvalidParamError("frame_count");
        return NULL;
    }
    if (frame_count == 1) {
        return SDL_CreateColorCursor(frames[0].surface, hot_x, hot_y);
    }
    SDL_PropertiesID props = SDL_GetSurfaceProperties(frames[0].surface);
    hot_x = (int)SDL_GetNumberProperty(props, SDL_PROP_SURFACE_HOTSPOT_X_NUMBER, hot_x);
    hot_y = (int)SDL_GetNumberProperty(props, SDL_PROP_SURFACE_HOTSPOT_Y_NUMBER, hot_y);
    if (hot_x < 0 || hot_y < 0 || hot_x >= frames[0].surface->w || hot_y >= frames[0].surface->h) {
        SDL_SetError("Cursor hot spot doesn't lie within cursor");
        return NULL;
    }
    const int w = frames[0].surface->w;
    const int h = frames[0].surface->h;
    for (int i = 0; i < frame_count; ++i) {
        if (!frames[i].surface) {
            SDL_SetError("Null surface in frame %i", i);
            return NULL;
        }
        if (frames[i].surface->w != w || frames[i].surface->h != h) {
            SDL_SetError("All frames in an animated sequence must have the same dimensions");
            return NULL;
        }
    }
    ensure_cursors();
    SDL_Cursor *cursor = (SDL_Cursor *)SDL_calloc(1, sizeof(*cursor));
    if (!cursor) {
        return NULL;
    }
    cursor->system_id = SDL_SYSTEM_CURSOR_COUNT;
    cursor->hot_x = hot_x;
    cursor->hot_y = hot_y;
    cursor->frames = (SDL_Cursor **)SDL_calloc((size_t)frame_count, sizeof(*cursor->frames));
    cursor->durations = (Uint32 *)SDL_calloc((size_t)frame_count, sizeof(*cursor->durations));
    if (!cursor->frames || !cursor->durations) {
        free_cursor(cursor);
        return NULL;
    }
    for (int i = 0; i < frame_count; ++i) {
        SDL_Surface *s = frames[i].surface;
        SDL_Surface *temp = NULL;
        if (s->format != SDL_PIXELFORMAT_ARGB8888) {
            s = temp = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
        }
        cursor->frames[i] = s ? alloc_cursor(SDL_SYSTEM_CURSOR_COUNT, s, hot_x, hot_y) : NULL;
        SDL_DestroySurface(temp);
        if (!cursor->frames[i]) {
            cursor->num_frames = i;
            free_cursor(cursor);
            return NULL;
        }
        cursor->durations[i] = frames[i].duration;
        cursor->num_frames = i + 1;
    }
    link_cursor(cursor);
    return cursor;
}

const char *SDLOP_GetCSSCursorName(SDL_SystemCursor id, const char **fallback_name)
{
    /* https://www.w3.org/TR/css-ui-4/#cursor (SDL_GetCSSCursorName) */
    switch (id) {
    case SDL_SYSTEM_CURSOR_DEFAULT: return "default";
    case SDL_SYSTEM_CURSOR_TEXT: return "text";
    case SDL_SYSTEM_CURSOR_WAIT: return "wait";
    case SDL_SYSTEM_CURSOR_CROSSHAIR: return "crosshair";
    case SDL_SYSTEM_CURSOR_PROGRESS: return "progress";
    case SDL_SYSTEM_CURSOR_NWSE_RESIZE:
        if (fallback_name) {
            *fallback_name = "nw-resize"; /* only a single arrow */
        }
        return "nwse-resize";
    case SDL_SYSTEM_CURSOR_NESW_RESIZE:
        if (fallback_name) {
            *fallback_name = "ne-resize";
        }
        return "nesw-resize";
    case SDL_SYSTEM_CURSOR_EW_RESIZE:
        if (fallback_name) {
            *fallback_name = "col-resize";
        }
        return "ew-resize";
    case SDL_SYSTEM_CURSOR_NS_RESIZE:
        if (fallback_name) {
            *fallback_name = "row-resize";
        }
        return "ns-resize";
    case SDL_SYSTEM_CURSOR_MOVE: return "all-scroll";
    case SDL_SYSTEM_CURSOR_NOT_ALLOWED: return "not-allowed";
    case SDL_SYSTEM_CURSOR_POINTER: return "pointer";
    case SDL_SYSTEM_CURSOR_NW_RESIZE: return "nw-resize";
    case SDL_SYSTEM_CURSOR_N_RESIZE: return "n-resize";
    case SDL_SYSTEM_CURSOR_NE_RESIZE: return "ne-resize";
    case SDL_SYSTEM_CURSOR_E_RESIZE: return "e-resize";
    case SDL_SYSTEM_CURSOR_SE_RESIZE: return "se-resize";
    case SDL_SYSTEM_CURSOR_S_RESIZE: return "s-resize";
    case SDL_SYSTEM_CURSOR_SW_RESIZE: return "sw-resize";
    case SDL_SYSTEM_CURSOR_W_RESIZE: return "w-resize";
    default: return "default";
    }
}

SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id)
{
    if (!sdlop.video || !sdlop.video->CreateCursor) {
        SDL_SetError("CreateSystemCursor is not currently supported");
        return NULL;
    }
    if ((int)id < 0 || id >= SDL_SYSTEM_CURSOR_COUNT) {
        SDL_SetError("Unhandled SDL_SystemCursor");
        return NULL;
    }
    ensure_cursors();
    SDL_Cursor *cursor = alloc_cursor(id, NULL, 0, 0);
    if (cursor) {
        link_cursor(cursor);
    }
    return cursor;
}

bool SDL_SetCursor(SDL_Cursor *cursor)
{
    ensure_cursors();
    if (cursor == cur_cursor) {
        return true;
    }
    if (cursor) {
        if (cursor != def_cursor) {
            SDL_Cursor *found;
            for (found = cursors; found; found = found->next) {
                if (found == cursor) {
                    break;
                }
            }
            if (!found) {
                return SDL_SetError("Cursor not associated with the current mouse");
            }
        }
        if (cursor->num_frames > 0) {
            cursor->current_frame = 0;
            cursor->last_update = SDL_GetTicks();
        }
        cur_cursor = cursor;
    }
    SDLOP_RedrawCursor();
    return true;
}

SDL_Cursor *SDL_GetCursor(void)
{
    ensure_cursors();
    return cur_cursor;
}

SDL_Cursor *SDL_GetDefaultCursor(void)
{
    ensure_cursors();
    return def_cursor;
}

void SDL_DestroyCursor(SDL_Cursor *cursor)
{
    if (!cursor || cursor == def_cursor) {
        return;
    }
    if (cursor == cur_cursor) {
        SDL_SetCursor(def_cursor);
    }
    for (SDL_Cursor *prev = NULL, *curr = cursors; curr; prev = curr, curr = curr->next) {
        if (curr == cursor) {
            if (prev) {
                prev->next = curr->next;
            } else {
                cursors = curr->next;
            }
            free_cursor(curr);
            return;
        }
    }
}

bool SDL_ShowCursor(void)
{
    ensure_cursors();
    if (cursor_hidden) {
        cursor_hidden = false;
        SDLOP_RedrawCursor();
    }
    return true;
}

bool SDL_HideCursor(void)
{
    ensure_cursors();
    if (!cursor_hidden) {
        cursor_hidden = true;
        SDLOP_RedrawCursor();
    }
    return true;
}

bool SDL_CursorVisible(void)
{
    /* the pointer counts as visible until SDL_HideCursor() asks otherwise;
       before any cursor API call there is no cursor object, but the driver
       may already be drawing one (stock SDL3 seeds a default cursor at video
       init, SDLop creates it lazily) */
    if (!cursors_initialized) {
        ensure_cursors();
    }
    return !cursor_hidden;
}
