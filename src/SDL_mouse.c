/*
  SDLop - mouse handling.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

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

void SDLOP_SendMouseMotion(float x, float y, float xrel, float yrel, Uint64 timestamp_ns)
{
    if (x != SDLOP_NO_POS) {
        sdlop.mouse_x = x;
        sdlop.mouse_y = y;
    }
    if (xrel != 0.0f || yrel != 0.0f) {
        sdlop.mouse_xrel_acc += xrel;
        sdlop.mouse_yrel_acc += yrel;
    }
    if (x == SDLOP_NO_POS && xrel == 0.0f && yrel == 0.0f) {
        return;
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
        sdlop.mouse_x = x;
        sdlop.mouse_y = y;
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
