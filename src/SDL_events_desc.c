/*
  SDLop - SDL_GetEventDescription() (SDL 3.4).

  Taken verbatim from SDL3 (src/events/SDL_events.c, release-3.4.20) so the
  descriptions are byte-identical to stock SDL3's - tools and tests compare
  these strings. Copyright (C) 1997-2026 Sam Lantinga, zlib license.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

int SDL_GetEventDescription(const SDL_Event *event, char *buf, int buflen)
{
    if (!event) {
        return SDL_snprintf(buf, buflen, "(null)");
    }

    static const char *pen_axisnames[] = { "PRESSURE", "XTILT", "YTILT", "DISTANCE", "ROTATION", "SLIDER", "TANGENTIAL_PRESSURE" };
    SDL_COMPILE_TIME_ASSERT(pen_axisnames_array_matches, SDL_arraysize(pen_axisnames) == SDL_PEN_AXIS_COUNT);

    char name[64];
    char details[128];

// this is to make (void)SDL_snprintf() calls cleaner.
#define uint unsigned int

    name[0] = '\0';
    details[0] = '\0';

    // !!! FIXME: This code is kinda ugly, sorry.

    if ((event->type >= SDL_EVENT_USER) && (event->type <= SDL_EVENT_LAST)) {
        char plusstr[16];
        SDL_strlcpy(name, "SDL_EVENT_USER", sizeof(name));
        if (event->type > SDL_EVENT_USER) {
            (void)SDL_snprintf(plusstr, sizeof(plusstr), "+%u", ((uint)event->type) - SDL_EVENT_USER);
        } else {
            plusstr[0] = '\0';
        }
        (void)SDL_snprintf(details, sizeof(details), "%s (timestamp=%" SDL_PRIu64 " windowid=%u code=%d data1=%p data2=%p)",
                           plusstr, event->user.timestamp, (uint)event->user.windowID,
                           (int)event->user.code, event->user.data1, event->user.data2);
    }

    switch (event->type) {
#define SDL_EVENT_CASE(x) \
    case x:               \
        SDL_strlcpy(name, #x, sizeof(name));
        SDL_EVENT_CASE(SDL_EVENT_FIRST)
        SDL_strlcpy(details, " (THIS IS PROBABLY A BUG!)", sizeof(details));
        break;
        SDL_EVENT_CASE(SDL_EVENT_QUIT)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 ")", event->quit.timestamp);
        break;
        SDL_EVENT_CASE(SDL_EVENT_TERMINATING)
        break;
        SDL_EVENT_CASE(SDL_EVENT_LOW_MEMORY)
        break;
        SDL_EVENT_CASE(SDL_EVENT_WILL_ENTER_BACKGROUND)
        break;
        SDL_EVENT_CASE(SDL_EVENT_DID_ENTER_BACKGROUND)
        break;
        SDL_EVENT_CASE(SDL_EVENT_WILL_ENTER_FOREGROUND)
        break;
        SDL_EVENT_CASE(SDL_EVENT_DID_ENTER_FOREGROUND)
        break;
        SDL_EVENT_CASE(SDL_EVENT_LOCALE_CHANGED)
        break;
        SDL_EVENT_CASE(SDL_EVENT_SYSTEM_THEME_CHANGED)
        break;
        SDL_EVENT_CASE(SDL_EVENT_KEYMAP_CHANGED)
        break;
        SDL_EVENT_CASE(SDL_EVENT_CLIPBOARD_UPDATE)
        break;

#define SDL_RENDEREVENT_CASE(x)                \
    case x:                                    \
        SDL_strlcpy(name, #x, sizeof(name));   \
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " event=%s windowid=%u)", \
                           event->display.timestamp, name, (uint)event->render.windowID); \
        break
        SDL_RENDEREVENT_CASE(SDL_EVENT_RENDER_TARGETS_RESET);
        SDL_RENDEREVENT_CASE(SDL_EVENT_RENDER_DEVICE_RESET);
        SDL_RENDEREVENT_CASE(SDL_EVENT_RENDER_DEVICE_LOST);

#define SDL_DISPLAYEVENT_CASE(x)               \
    case x:                                    \
        SDL_strlcpy(name, #x, sizeof(name));   \
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " display=%u event=%s data1=%d, data2=%d)", \
                           event->display.timestamp, (uint)event->display.displayID, name, (int)event->display.data1, (int)event->display.data2); \
        break
        SDL_DISPLAYEVENT_CASE(SDL_EVENT_DISPLAY_ORIENTATION);
        SDL_DISPLAYEVENT_CASE(SDL_EVENT_DISPLAY_ADDED);
        SDL_DISPLAYEVENT_CASE(SDL_EVENT_DISPLAY_REMOVED);
        SDL_DISPLAYEVENT_CASE(SDL_EVENT_DISPLAY_MOVED);
        SDL_DISPLAYEVENT_CASE(SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED);
        SDL_DISPLAYEVENT_CASE(SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED);
        SDL_DISPLAYEVENT_CASE(SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED);
        SDL_DISPLAYEVENT_CASE(SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED);
#undef SDL_DISPLAYEVENT_CASE

#define SDL_WINDOWEVENT_CASE(x)                \
    case x:                                    \
        SDL_strlcpy(name, #x, sizeof(name)); \
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u event=%s data1=%d data2=%d)", \
                           event->window.timestamp, (uint)event->window.windowID, name, (int)event->window.data1, (int)event->window.data2); \
        break
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_SHOWN);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_HIDDEN);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_EXPOSED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_MOVED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_RESIZED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_METAL_VIEW_RESIZED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_MINIMIZED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_MAXIMIZED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_RESTORED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_MOUSE_ENTER);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_MOUSE_LEAVE);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_FOCUS_GAINED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_FOCUS_LOST);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_HIT_TEST);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_ICCPROF_CHANGED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_DISPLAY_CHANGED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_SAFE_AREA_CHANGED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_OCCLUDED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_ENTER_FULLSCREEN);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_LEAVE_FULLSCREEN);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_DESTROYED);
        SDL_WINDOWEVENT_CASE(SDL_EVENT_WINDOW_HDR_STATE_CHANGED);
#undef SDL_WINDOWEVENT_CASE

#define PRINT_KEYDEV_EVENT(event) (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%u)", event->kdevice.timestamp, (uint)event->kdevice.which)
        SDL_EVENT_CASE(SDL_EVENT_KEYBOARD_ADDED)
        PRINT_KEYDEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_KEYBOARD_REMOVED)
        PRINT_KEYDEV_EVENT(event);
        break;
#undef PRINT_KEYDEV_EVENT

#define PRINT_KEY_EVENT(event)                                                                                                              \
    (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u which=%u state=%s repeat=%s scancode=%u keycode=%u mod=0x%x)", \
                       event->key.timestamp, (uint)event->key.windowID, (uint)event->key.which,                                       \
                       event->key.down ? "pressed" : "released",                                                            \
                       event->key.repeat ? "true" : "false",                                                                                \
                       (uint)event->key.scancode,                                                                                           \
                       (uint)event->key.key,                                                                                                \
                       (uint)event->key.mod)
        SDL_EVENT_CASE(SDL_EVENT_KEY_DOWN)
        PRINT_KEY_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_KEY_UP)
        PRINT_KEY_EVENT(event);
        break;
#undef PRINT_KEY_EVENT

        SDL_EVENT_CASE(SDL_EVENT_TEXT_EDITING)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u text='%s' start=%d length=%d)",
                           event->edit.timestamp, (uint)event->edit.windowID,
                           event->edit.text, (int)event->edit.start, (int)event->edit.length);
        break;

        SDL_EVENT_CASE(SDL_EVENT_TEXT_EDITING_CANDIDATES)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u num_candidates=%d selected_candidate=%d)",
                           event->edit_candidates.timestamp, (uint)event->edit_candidates.windowID,
                           (int)event->edit_candidates.num_candidates, (int)event->edit_candidates.selected_candidate);
        break;

        SDL_EVENT_CASE(SDL_EVENT_TEXT_INPUT)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u text='%s')", event->text.timestamp, (uint)event->text.windowID, event->text.text);
        break;
        SDL_EVENT_CASE(SDL_EVENT_SCREEN_KEYBOARD_SHOWN)
        break;
        SDL_EVENT_CASE(SDL_EVENT_SCREEN_KEYBOARD_HIDDEN)
        break;

#define PRINT_MOUSEDEV_EVENT(event) (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%u)", event->mdevice.timestamp, (uint)event->mdevice.which)
        SDL_EVENT_CASE(SDL_EVENT_MOUSE_ADDED)
        PRINT_MOUSEDEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_MOUSE_REMOVED)
        PRINT_MOUSEDEV_EVENT(event);
        break;
#undef PRINT_MOUSEDEV_EVENT

        SDL_EVENT_CASE(SDL_EVENT_MOUSE_MOTION)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u which=%u state=%u x=%g y=%g xrel=%g yrel=%g)",
                           event->motion.timestamp, (uint)event->motion.windowID,
                           (uint)event->motion.which, (uint)event->motion.state,
                           event->motion.x, event->motion.y,
                           event->motion.xrel, event->motion.yrel);
        break;

#define PRINT_MBUTTON_EVENT(event)                                                                                              \
    (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u which=%u button=%u state=%s clicks=%u x=%g y=%g)", \
                       event->button.timestamp, (uint)event->button.windowID,                                             \
                       (uint)event->button.which, (uint)event->button.button,                                                   \
                       event->button.down ? "pressed" : "released",                                                             \
                       (uint)event->button.clicks, event->button.x, event->button.y)
        SDL_EVENT_CASE(SDL_EVENT_MOUSE_BUTTON_DOWN)
        PRINT_MBUTTON_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_MOUSE_BUTTON_UP)
        PRINT_MBUTTON_EVENT(event);
        break;
#undef PRINT_MBUTTON_EVENT

        SDL_EVENT_CASE(SDL_EVENT_MOUSE_WHEEL)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u which=%u x=%g y=%g integer_x=%d integer_y=%d direction=%s)",
                           event->wheel.timestamp, (uint)event->wheel.windowID,
                           (uint)event->wheel.which, event->wheel.x, event->wheel.y,
                           (int)event->wheel.integer_x, (int)event->wheel.integer_y,
                           event->wheel.direction == SDL_MOUSEWHEEL_NORMAL ? "normal" : "flipped");
        break;

        SDL_EVENT_CASE(SDL_EVENT_JOYSTICK_AXIS_MOTION)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d axis=%u value=%d)",
                           event->jaxis.timestamp, (int)event->jaxis.which,
                           (uint)event->jaxis.axis, (int)event->jaxis.value);
        break;

        SDL_EVENT_CASE(SDL_EVENT_JOYSTICK_BALL_MOTION)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d ball=%u xrel=%d yrel=%d)",
                           event->jball.timestamp, (int)event->jball.which,
                           (uint)event->jball.ball, (int)event->jball.xrel, (int)event->jball.yrel);
        break;

        SDL_EVENT_CASE(SDL_EVENT_JOYSTICK_HAT_MOTION)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d hat=%u value=%u)",
                           event->jhat.timestamp, (int)event->jhat.which,
                           (uint)event->jhat.hat, (uint)event->jhat.value);
        break;

#define PRINT_JBUTTON_EVENT(event)                                                              \
    (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d button=%u state=%s)", \
                       event->jbutton.timestamp, (int)event->jbutton.which,               \
                       (uint)event->jbutton.button, event->jbutton.down ? "pressed" : "released")
        SDL_EVENT_CASE(SDL_EVENT_JOYSTICK_BUTTON_DOWN)
        PRINT_JBUTTON_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_JOYSTICK_BUTTON_UP)
        PRINT_JBUTTON_EVENT(event);
        break;
#undef PRINT_JBUTTON_EVENT

        SDL_EVENT_CASE(SDL_EVENT_JOYSTICK_BATTERY_UPDATED)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d state=%u percent=%d)",
                           event->jbattery.timestamp, (int)event->jbattery.which,
                           event->jbattery.state, event->jbattery.percent);
        break;

#define PRINT_JOYDEV_EVENT(event) (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d)", event->jdevice.timestamp, (int)event->jdevice.which)
        SDL_EVENT_CASE(SDL_EVENT_JOYSTICK_ADDED)
        PRINT_JOYDEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_JOYSTICK_REMOVED)
        PRINT_JOYDEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_JOYSTICK_UPDATE_COMPLETE)
        PRINT_JOYDEV_EVENT(event);
        break;
#undef PRINT_JOYDEV_EVENT

        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_AXIS_MOTION)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d axis=%u value=%d)",
                           event->gaxis.timestamp, (int)event->gaxis.which,
                           (uint)event->gaxis.axis, (int)event->gaxis.value);
        break;

#define PRINT_CBUTTON_EVENT(event)                                                              \
    (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d button=%u state=%s)", \
                       event->gbutton.timestamp, (int)event->gbutton.which,               \
                       (uint)event->gbutton.button, event->gbutton.down ? "pressed" : "released")
        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        PRINT_CBUTTON_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_BUTTON_UP)
        PRINT_CBUTTON_EVENT(event);
        break;
#undef PRINT_CBUTTON_EVENT

#define PRINT_GAMEPADDEV_EVENT(event) (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d)", event->gdevice.timestamp, (int)event->gdevice.which)
        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_ADDED)
        PRINT_GAMEPADDEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_REMOVED)
        PRINT_GAMEPADDEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_REMAPPED)
        PRINT_GAMEPADDEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_UPDATE_COMPLETE)
        PRINT_GAMEPADDEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED)
        PRINT_GAMEPADDEV_EVENT(event);
        break;
#undef PRINT_GAMEPADDEV_EVENT

#define PRINT_CTOUCHPAD_EVENT(event)                                                                                     \
    (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d touchpad=%d finger=%d x=%f y=%f pressure=%f)", \
                       event->gtouchpad.timestamp, (int)event->gtouchpad.which,                                    \
                       (int)event->gtouchpad.touchpad, (int)event->gtouchpad.finger,                                     \
                       event->gtouchpad.x, event->gtouchpad.y, event->gtouchpad.pressure)
        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN)
        PRINT_CTOUCHPAD_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_TOUCHPAD_UP)
        PRINT_CTOUCHPAD_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION)
        PRINT_CTOUCHPAD_EVENT(event);
        break;
#undef PRINT_CTOUCHPAD_EVENT

        SDL_EVENT_CASE(SDL_EVENT_GAMEPAD_SENSOR_UPDATE)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d sensor=%d data[0]=%f data[1]=%f data[2]=%f)",
                           event->gsensor.timestamp, (int)event->gsensor.which, (int)event->gsensor.sensor,
                           event->gsensor.data[0], event->gsensor.data[1], event->gsensor.data[2]);
        break;

#define PRINT_FINGER_EVENT(event)                                                                                                                      \
    (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " touchid=%" SDL_PRIu64 " fingerid=%" SDL_PRIu64 " x=%f y=%f dx=%f dy=%f pressure=%f)", \
                       event->tfinger.timestamp, event->tfinger.touchID,                                                              \
                       event->tfinger.fingerID, event->tfinger.x, event->tfinger.y,                                                         \
                       event->tfinger.dx, event->tfinger.dy, event->tfinger.pressure)
        SDL_EVENT_CASE(SDL_EVENT_FINGER_DOWN)
        PRINT_FINGER_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_FINGER_UP)
        PRINT_FINGER_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_FINGER_CANCELED)
        PRINT_FINGER_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_FINGER_MOTION)
        PRINT_FINGER_EVENT(event);
        break;
#undef PRINT_FINGER_EVENT

#define PRINT_PINCH_EVENT(event)                                                                                                                      \
    (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " scale=%f)", \
                       event->pinch.timestamp, event->pinch.scale)
        SDL_EVENT_CASE(SDL_EVENT_PINCH_BEGIN)
        PRINT_PINCH_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_PINCH_UPDATE)
        PRINT_PINCH_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_PINCH_END)
        PRINT_PINCH_EVENT(event);
        break;
#undef PRINT_PINCH_EVENT

#define PRINT_PTOUCH_EVENT(event)                                                                             \
    (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u which=%u pen_state=%u x=%g y=%g eraser=%s state=%s)", \
                       event->ptouch.timestamp, (uint)event->ptouch.windowID, (uint)event->ptouch.which, (uint)event->ptouch.pen_state, event->ptouch.x, event->ptouch.y, \
                       event->ptouch.eraser ? "yes" : "no", event->ptouch.down ? "down" : "up");
        SDL_EVENT_CASE(SDL_EVENT_PEN_DOWN)
        PRINT_PTOUCH_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_PEN_UP)
        PRINT_PTOUCH_EVENT(event);
        break;
#undef PRINT_PTOUCH_EVENT

#define PRINT_PPROXIMITY_EVENT(event)                                                                             \
    (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u which=%u)", \
                       event->pproximity.timestamp, (uint)event->pproximity.windowID, (uint)event->pproximity.which);
        SDL_EVENT_CASE(SDL_EVENT_PEN_PROXIMITY_IN)
        PRINT_PPROXIMITY_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_PEN_PROXIMITY_OUT)
        PRINT_PPROXIMITY_EVENT(event);
        break;
#undef PRINT_PPROXIMITY_EVENT

        SDL_EVENT_CASE(SDL_EVENT_PEN_AXIS)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u which=%u pen_state=%u x=%g y=%g axis=%s value=%g)",
                           event->paxis.timestamp, (uint)event->paxis.windowID, (uint)event->paxis.which, (uint)event->paxis.pen_state, event->paxis.x, event->paxis.y,
                           ((((int) event->paxis.axis) >= 0) && (event->paxis.axis < SDL_arraysize(pen_axisnames))) ? pen_axisnames[event->paxis.axis] : "[UNKNOWN]", event->paxis.value);
        break;

        SDL_EVENT_CASE(SDL_EVENT_PEN_MOTION)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u which=%u pen_state=%u x=%g y=%g)",
                           event->pmotion.timestamp, (uint)event->pmotion.windowID, (uint)event->pmotion.which, (uint)event->pmotion.pen_state, event->pmotion.x, event->pmotion.y);
        break;

#define PRINT_PBUTTON_EVENT(event)                                                                                                               \
    (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " windowid=%u which=%u pen_state=%u x=%g y=%g button=%u state=%s)", \
                       event->pbutton.timestamp, (uint)event->pbutton.windowID, (uint)event->pbutton.which, (uint)event->pbutton.pen_state, event->pbutton.x, event->pbutton.y, \
                       (uint)event->pbutton.button, event->pbutton.down ? "down" : "up");
        SDL_EVENT_CASE(SDL_EVENT_PEN_BUTTON_DOWN)
        PRINT_PBUTTON_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_PEN_BUTTON_UP)
        PRINT_PBUTTON_EVENT(event);
        break;
#undef PRINT_PBUTTON_EVENT

#define PRINT_DROP_EVENT(event) (void)SDL_snprintf(details, sizeof(details), " (data='%s' timestamp=%" SDL_PRIu64 " windowid=%u x=%f y=%f)", event->drop.data, event->drop.timestamp, (uint)event->drop.windowID, event->drop.x, event->drop.y)
        SDL_EVENT_CASE(SDL_EVENT_DROP_FILE)
        PRINT_DROP_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_DROP_TEXT)
        PRINT_DROP_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_DROP_BEGIN)
        PRINT_DROP_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_DROP_COMPLETE)
        PRINT_DROP_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_DROP_POSITION)
        PRINT_DROP_EVENT(event);
        break;
#undef PRINT_DROP_EVENT

#define PRINT_AUDIODEV_EVENT(event) (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%u recording=%s)", event->adevice.timestamp, (uint)event->adevice.which, event->adevice.recording ? "true" : "false")
        SDL_EVENT_CASE(SDL_EVENT_AUDIO_DEVICE_ADDED)
        PRINT_AUDIODEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_AUDIO_DEVICE_REMOVED)
        PRINT_AUDIODEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED)
        PRINT_AUDIODEV_EVENT(event);
        break;
#undef PRINT_AUDIODEV_EVENT

#define PRINT_CAMERADEV_EVENT(event) (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%u)", event->cdevice.timestamp, (uint)event->cdevice.which)
        SDL_EVENT_CASE(SDL_EVENT_CAMERA_DEVICE_ADDED)
        PRINT_CAMERADEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_CAMERA_DEVICE_REMOVED)
        PRINT_CAMERADEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_CAMERA_DEVICE_APPROVED)
        PRINT_CAMERADEV_EVENT(event);
        break;
        SDL_EVENT_CASE(SDL_EVENT_CAMERA_DEVICE_DENIED)
        PRINT_CAMERADEV_EVENT(event);
        break;
#undef PRINT_CAMERADEV_EVENT

        SDL_EVENT_CASE(SDL_EVENT_SENSOR_UPDATE)
        (void)SDL_snprintf(details, sizeof(details), " (timestamp=%" SDL_PRIu64 " which=%d data[0]=%f data[1]=%f data[2]=%f data[3]=%f data[4]=%f data[5]=%f)",
                           event->sensor.timestamp, (int)event->sensor.which,
                           event->sensor.data[0], event->sensor.data[1], event->sensor.data[2],
                           event->sensor.data[3], event->sensor.data[4], event->sensor.data[5]);
        break;

#undef SDL_EVENT_CASE

    case SDL_EVENT_POLL_SENTINEL:
        // No logging necessary for this one
        break;

    default:
        if (!name[0]) {
            if (event->type >= SDL_EVENT_USER) {
                SDL_strlcpy(name, "USER", sizeof(name));
            } else {
                SDL_strlcpy(name, "UNKNOWN", sizeof(name));
            }
            (void)SDL_snprintf(details, sizeof(details), " 0x%x", (uint)event->type);
        }
        break;
    }
#undef uint

    int retval = 0;
    if (name[0]) {
        retval = SDL_snprintf(buf, buflen, "%s%s", name, details);
    } else if (buf && (buflen > 0)) {
        *buf = '\0';
    }
    return retval;
}
