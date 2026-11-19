/*
  SDLop - event categories and SDL_GetWindowFromEvent().

  Taken from SDL3 (src/events/SDL_categories.c + SDL_categories_c.h,
  release-3.4.20), so every event type that carries a windowID resolves
  exactly like stock SDL3. Copyright (C) 1997-2026 Sam Lantinga, zlib.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

typedef enum SDL_EventCategory
{
    SDL_EVENTCATEGORY_UNKNOWN,
    SDL_EVENTCATEGORY_SYSTEM,
    SDL_EVENTCATEGORY_DISPLAY,
    SDL_EVENTCATEGORY_WINDOW,
    SDL_EVENTCATEGORY_KDEVICE,
    SDL_EVENTCATEGORY_KEY,
    SDL_EVENTCATEGORY_EDIT,
    SDL_EVENTCATEGORY_EDIT_CANDIDATES,
    SDL_EVENTCATEGORY_TEXT,
    SDL_EVENTCATEGORY_MDEVICE,
    SDL_EVENTCATEGORY_MOTION,
    SDL_EVENTCATEGORY_BUTTON,
    SDL_EVENTCATEGORY_WHEEL,
    SDL_EVENTCATEGORY_JDEVICE,
    SDL_EVENTCATEGORY_JAXIS,
    SDL_EVENTCATEGORY_JBALL,
    SDL_EVENTCATEGORY_JHAT,
    SDL_EVENTCATEGORY_JBUTTON,
    SDL_EVENTCATEGORY_JBATTERY,
    SDL_EVENTCATEGORY_GDEVICE,
    SDL_EVENTCATEGORY_GAXIS,
    SDL_EVENTCATEGORY_GBUTTON,
    SDL_EVENTCATEGORY_GTOUCHPAD,
    SDL_EVENTCATEGORY_GSENSOR,
    SDL_EVENTCATEGORY_ADEVICE,
    SDL_EVENTCATEGORY_CDEVICE,
    SDL_EVENTCATEGORY_SENSOR,
    SDL_EVENTCATEGORY_QUIT,
    SDL_EVENTCATEGORY_USER,
    SDL_EVENTCATEGORY_TFINGER,
    SDL_EVENTCATEGORY_PPROXIMITY,
    SDL_EVENTCATEGORY_PTOUCH,
    SDL_EVENTCATEGORY_PMOTION,
    SDL_EVENTCATEGORY_PBUTTON,
    SDL_EVENTCATEGORY_PAXIS,
    SDL_EVENTCATEGORY_DROP,
    SDL_EVENTCATEGORY_CLIPBOARD,
    SDL_EVENTCATEGORY_RENDER,
    SDL_EVENTCATEGORY_PINCH,
} SDL_EventCategory;

static SDL_EventCategory SDL_GetEventCategory(Uint32 type)
{
    if (type >= SDL_EVENT_USER && type <= SDL_EVENT_LAST) {
        return SDL_EVENTCATEGORY_USER;
    }
    else if (type >= SDL_EVENT_DISPLAY_FIRST && type <= SDL_EVENT_DISPLAY_LAST) {
        return SDL_EVENTCATEGORY_DISPLAY;
    }
    else if (type >= SDL_EVENT_WINDOW_FIRST && type <= SDL_EVENT_WINDOW_LAST) {
        return SDL_EVENTCATEGORY_WINDOW;
    }
    switch (type) {
    default:
        SDL_SetError("Unknown event type");
        return SDL_EVENTCATEGORY_UNKNOWN;

    case SDL_EVENT_KEYMAP_CHANGED:
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_LOW_MEMORY:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
    case SDL_EVENT_LOCALE_CHANGED:
    case SDL_EVENT_SYSTEM_THEME_CHANGED:
        return SDL_EVENTCATEGORY_SYSTEM;

    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_RENDER_DEVICE_RESET:
    case SDL_EVENT_RENDER_DEVICE_LOST:
        return SDL_EVENTCATEGORY_RENDER;

    case SDL_EVENT_QUIT:
        return SDL_EVENTCATEGORY_QUIT;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return SDL_EVENTCATEGORY_KEY;

    case SDL_EVENT_TEXT_EDITING:
        return SDL_EVENTCATEGORY_EDIT;

    case SDL_EVENT_TEXT_INPUT:
        return SDL_EVENTCATEGORY_TEXT;

    case SDL_EVENT_KEYBOARD_ADDED:
    case SDL_EVENT_KEYBOARD_REMOVED:
        return SDL_EVENTCATEGORY_KDEVICE;

    case SDL_EVENT_TEXT_EDITING_CANDIDATES:
        return SDL_EVENTCATEGORY_EDIT_CANDIDATES;

    case SDL_EVENT_MOUSE_MOTION:
        return SDL_EVENTCATEGORY_MOTION;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return SDL_EVENTCATEGORY_BUTTON;

    case SDL_EVENT_MOUSE_WHEEL:
        return SDL_EVENTCATEGORY_WHEEL;

    case SDL_EVENT_MOUSE_ADDED:
    case SDL_EVENT_MOUSE_REMOVED:
        return SDL_EVENTCATEGORY_MDEVICE;

    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        return SDL_EVENTCATEGORY_JAXIS;

    case SDL_EVENT_JOYSTICK_BALL_MOTION:
        return SDL_EVENTCATEGORY_JBALL;

    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        return SDL_EVENTCATEGORY_JHAT;

    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return SDL_EVENTCATEGORY_JBUTTON;

    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_REMOVED:
    case SDL_EVENT_JOYSTICK_UPDATE_COMPLETE:
        return SDL_EVENTCATEGORY_JDEVICE;

    case SDL_EVENT_JOYSTICK_BATTERY_UPDATED:
        return SDL_EVENTCATEGORY_JBATTERY;

    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return SDL_EVENTCATEGORY_GAXIS;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return SDL_EVENTCATEGORY_GBUTTON;

    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
    case SDL_EVENT_GAMEPAD_UPDATE_COMPLETE:
    case SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED:
        return SDL_EVENTCATEGORY_GDEVICE;

    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        return SDL_EVENTCATEGORY_GTOUCHPAD;

    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        return SDL_EVENTCATEGORY_GSENSOR;

    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_FINGER_MOTION:
        return SDL_EVENTCATEGORY_TFINGER;

    case SDL_EVENT_PINCH_BEGIN:
    case SDL_EVENT_PINCH_UPDATE:
    case SDL_EVENT_PINCH_END:
        return SDL_EVENTCATEGORY_PINCH;

    case SDL_EVENT_CLIPBOARD_UPDATE:
        return SDL_EVENTCATEGORY_CLIPBOARD;

    case SDL_EVENT_DROP_FILE:
    case SDL_EVENT_DROP_TEXT:
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_COMPLETE:
    case SDL_EVENT_DROP_POSITION:
        return SDL_EVENTCATEGORY_DROP;

    case SDL_EVENT_AUDIO_DEVICE_ADDED:
    case SDL_EVENT_AUDIO_DEVICE_REMOVED:
    case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
        return SDL_EVENTCATEGORY_ADEVICE;

    case SDL_EVENT_SENSOR_UPDATE:
        return SDL_EVENTCATEGORY_SENSOR;

    case SDL_EVENT_PEN_PROXIMITY_IN:
    case SDL_EVENT_PEN_PROXIMITY_OUT:
        return SDL_EVENTCATEGORY_PPROXIMITY;

    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
        return SDL_EVENTCATEGORY_PTOUCH;

    case SDL_EVENT_PEN_BUTTON_DOWN:
    case SDL_EVENT_PEN_BUTTON_UP:
        return SDL_EVENTCATEGORY_PBUTTON;

    case SDL_EVENT_PEN_MOTION:
        return SDL_EVENTCATEGORY_PMOTION;

    case SDL_EVENT_PEN_AXIS:
        return SDL_EVENTCATEGORY_PAXIS;

    case SDL_EVENT_CAMERA_DEVICE_ADDED:
    case SDL_EVENT_CAMERA_DEVICE_REMOVED:
    case SDL_EVENT_CAMERA_DEVICE_APPROVED:
    case SDL_EVENT_CAMERA_DEVICE_DENIED:
        return SDL_EVENTCATEGORY_CDEVICE;
    }
}

SDL_Window *SDL_GetWindowFromEvent(const SDL_Event *event)
{
    SDL_WindowID windowID;

    switch (SDL_GetEventCategory(event->type)) {
    case SDL_EVENTCATEGORY_USER:
        windowID = event->user.windowID;
        break;
    case SDL_EVENTCATEGORY_WINDOW:
        windowID = event->window.windowID;
        break;
    case SDL_EVENTCATEGORY_KEY:
        windowID = event->key.windowID;
        break;
    case SDL_EVENTCATEGORY_EDIT:
        windowID = event->edit.windowID;
        break;
    case SDL_EVENTCATEGORY_TEXT:
        windowID = event->text.windowID;
        break;
    case SDL_EVENTCATEGORY_EDIT_CANDIDATES:
        windowID = event->edit_candidates.windowID;
        break;
    case SDL_EVENTCATEGORY_MOTION:
        windowID = event->motion.windowID;
        break;
    case SDL_EVENTCATEGORY_BUTTON:
        windowID = event->button.windowID;
        break;
    case SDL_EVENTCATEGORY_WHEEL:
        windowID = event->wheel.windowID;
        break;
    case SDL_EVENTCATEGORY_TFINGER:
        windowID = event->tfinger.windowID;
        break;
    case SDL_EVENTCATEGORY_PPROXIMITY:
        windowID = event->pproximity.windowID;
        break;
    case SDL_EVENTCATEGORY_PTOUCH:
        windowID = event->ptouch.windowID;
        break;
    case SDL_EVENTCATEGORY_PBUTTON:
        windowID = event->pbutton.windowID;
        break;
    case SDL_EVENTCATEGORY_PMOTION:
        windowID = event->pmotion.windowID;
        break;
    case SDL_EVENTCATEGORY_PAXIS:
        windowID = event->paxis.windowID;
        break;
    case SDL_EVENTCATEGORY_DROP:
        windowID = event->drop.windowID;
        break;
    case SDL_EVENTCATEGORY_RENDER:
        windowID = event->render.windowID;
        break;
    case SDL_EVENTCATEGORY_PINCH:
        windowID = event->pinch.windowID;
        break;
    default:
        // < 0  -> invalid event type (error is set by SDL_GetEventCategory)
        // else -> event has no associated window (not an error)
        return NULL;
    }
    return SDL_GetWindowFromID(windowID);
}
