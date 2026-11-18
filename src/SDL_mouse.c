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
   inside the window (no mouse-rect API in this subset, so the window rect is
   the only confine), and going past the far edge keeps the larger of the
   edge and the previous position so deltas stay continuous. */
static void constrain_mouse_pos(float *x, float *y)
{
    SDL_Window *w = sdlop.mouse_focus;
    if (!w || (w->flags & SDL_WINDOW_MOUSE_CAPTURE)) {
        return;
    }
    int x_max = w->w - 1;
    int y_max = w->h - 1;
    if (*x >= (float)(x_max + 1)) {
        *x = SDL_max((float)x_max, sdlop.mouse_last_x);
    }
    if (*x < 0.0f) {
        *x = 0.0f;
    }
    if (*y >= (float)(y_max + 1)) {
        *y = SDL_max((float)y_max, sdlop.mouse_last_y);
    }
    if (*y < 0.0f) {
        *y = 0.0f;
    }
}

void SDLOP_SendMouseMotion(float x, float y, float xrel, float yrel, Uint64 timestamp_ns)
{
    if (x == SDLOP_NO_POS) {
        /* relative-only motion (relative-pointer / evdev / locked pointer) */
        if (xrel == 0.0f && yrel == 0.0f) {
            return;
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
    SDLOP_PushEventInternal(&event);
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
    SDLOP_PushEventInternal(&event);
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
    const char *v = getenv(SDL_HINT_TOUCH_MOUSE_EVENTS);
    if (!v || !*v) {
        return true;
    }
    return !(strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0);
}

static void touch_mouse_event(Uint32 type, SDL_Window *window, float x, float y, Uint64 timestamp_ns)
{
    /* SDL3: position = normalized * window size, clamped into the window */
    float fx = x * (float)window->w;
    float fy = y * (float)window->h;
    fx = SDL_max(0.0f, SDL_min(fx, (float)(window->w - 1)));
    fy = SDL_max(0.0f, SDL_min(fy, (float)(window->h - 1)));

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
        event.motion.which = SDL_TOUCH_MOUSEID;
        event.motion.state = sdlop.mouse_buttons;
        event.motion.x = fx;
        event.motion.y = fy;
        event.motion.xrel = xrel;
        event.motion.yrel = yrel;
        SDLOP_PushEventInternal(&event);
    }
    if (type == SDL_EVENT_MOUSE_MOTION) {
        return;
    }
    bool down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (down) {
        sdlop.mouse_buttons |= SDL_BUTTON_LMASK;
    } else {
        sdlop.mouse_buttons &= ~SDL_BUTTON_LMASK;
    }
    SDL_zero(event);
    event.button.type = type;
    event.button.timestamp = ts;
    event.button.windowID = window->id;
    event.button.which = SDL_TOUCH_MOUSEID;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = fx;
    event.button.y = fy;
    SDLOP_PushEventInternal(&event);
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
    /* SDL3 order: the synthetic mouse events precede the finger event */
    touch_to_mouse(type, touchID, fingerID, x, y, windowID, timestamp_ns);

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
    SDLOP_PushEventInternal(&event);
}


void SDLOP_SendMouseWheel(float x, float y, Uint64 timestamp_ns)
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
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    event.wheel.mouse_x = sdlop.mouse_x;
    event.wheel.mouse_y = sdlop.mouse_y;
    SDLOP_PushEventInternal(&event);
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
