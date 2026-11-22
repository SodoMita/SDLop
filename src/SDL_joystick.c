/*
  SDLop - joystick and gamepad API backed by Linux evdev devices.

  Applications built against stock SDL3 call the joystick / gamepad API
  unconditionally (Dear ImGui's SDL3 backend polls SDL_GetGamepads() every
  frame), so the whole API is exported. On Linux SDLop enumerates physical
  joysticks through evdev, reports axis/button/hat state and device events,
  and exposes standard gamepads through a generic evdev mapping. On other
  platforms it keeps the same "no device attached" behaviour as before.

  The string tables and face-style labels are from SDL 3.4.20
  (src/joystick/SDL_gamepad.c), (C) Sam Lantinga, zlib license.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define SDL_MAIN_NOIMPL

#include "internal/sdlop_internal.h"
#include "internal/sdlop_joystick_internal.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifdef __linux__
#include <linux/input.h>
#endif

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

/* recursive like stock's joystick lock; set up once (the static
 * PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP is glibc-only, not musl/emscripten) */
static pthread_mutex_t joystick_lock;
static pthread_once_t joystick_lock_once = PTHREAD_ONCE_INIT;

static void joystick_lock_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&joystick_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
static bool joystick_events_enabled = true;
static bool gamepad_events_enabled = true;

/* With no device there is no valid instance ID. Lookups and per-ID queries
 * (*ForID, *FromID, *FromPlayerIndex) just return the neutral value and
 * leave the error state alone, so an app that checks SDL_GetError() after a
 * "no controllers" query sees nothing. Only real operations (open, set
 * mapping) fail with an error. */
static bool no_joystick(SDL_JoystickID instance_id)
{
    if (instance_id == 0) {
        return SDL_InvalidParamError("instance_id");
    }
    return SDL_SetError("Joystick %" SDL_PRIu32 " not found", instance_id);
}

static bool no_gamepad(SDL_JoystickID instance_id)
{
    if (instance_id == 0) {
        return SDL_InvalidParamError("instance_id");
    }
    return SDL_SetError("Gamepad %" SDL_PRIu32 " not found", instance_id);
}

static SDL_GUID zero_guid(void)
{
    SDL_GUID guid;
    memset(&guid, 0, sizeof(guid));
    return guid;
}


/* ------------------------------------------------------------------ */
/* Real joystick / gamepad objects                                     */
/* ------------------------------------------------------------------ */

static SDL_Joystick *joysticks;
static SDL_Gamepad *gamepads;
static SDL_JoystickID next_instance_id = 1;
static int next_player_index;

static void sdloP_destroy_gamepad(SDL_Gamepad *gamepad)
{
    if (!gamepad) {
        return;
    }
    SDL_Gamepad **link = &gamepads;
    while (*link && *link != gamepad) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = gamepad->next;
    }
    gamepad->magic = 0;
    SDL_free(gamepad);
}

static void sdloP_destroy_joystick(SDL_Joystick *joystick)
{
    if (!joystick) {
        return;
    }
    SDL_Joystick **link = &joysticks;
    while (*link && *link != joystick) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = joystick->next;
    }
    joystick->magic = 0;
    SDL_free(joystick->name);
    SDL_free(joystick->path);
    SDL_free(joystick->axes);
    SDL_free(joystick->axes_initial);
    SDL_free(joystick->axis_codes);
    SDL_free(joystick->axis_min);
    SDL_free(joystick->axis_max);
    SDL_free(joystick->buttons);
    SDL_free(joystick->button_codes);
    SDL_free(joystick->hats);
    SDL_free(joystick->hats_info);
    SDL_free(joystick);
}

static void sdloP_release_joystick(SDL_Joystick *joystick)
{
    if (!joystick) {
        return;
    }
    if (joystick->ref_count > 0) {
        --joystick->ref_count;
    }
    if (joystick->ref_count == 0 && joystick->gamepad_refs == 0 && !joystick->connected) {
        sdloP_destroy_joystick(joystick);
    }
}

bool SDLOP_JoystickValid(SDL_Joystick *joystick)
{
    return joystick && joystick->magic == SDLOP_JOYSTICK_MAGIC;
}

bool SDLOP_GamepadValid(SDL_Gamepad *gamepad)
{
    return gamepad && gamepad->magic == SDLOP_GAMEPAD_MAGIC && SDLOP_JoystickValid(gamepad->joystick);
}

SDL_Joystick *SDLOP_JoystickFromInstanceID(SDL_JoystickID instance_id)
{
    if (!instance_id) {
        return NULL;
    }
    for (SDL_Joystick *j = joysticks; j; j = j->next) {
        if (j->instance_id == instance_id) {
            return j;
        }
    }
    return NULL;
}

static SDL_Gamepad *sdloP_gamepad_from_joystick(SDL_Joystick *joystick)
{
    for (SDL_Gamepad *g = gamepads; g; g = g->next) {
        if (g->joystick == joystick) {
            return g;
        }
    }
    return NULL;
}

SDL_GUID SDLOP_CreateJoystickGUID(Uint16 bus, Uint16 vendor, Uint16 product,
                                  Uint16 version, const char *name)
{
    /* Same layout as SDL3's SDL_CreateJoystickGUID() for standard devices. */
    SDL_GUID guid;
    SDL_zero(guid);
    Uint16 crc = 0;
    if (name && *name) {
        crc = SDL_crc16(crc, name, SDL_strlen(name));
    }
    Uint16 *w = (Uint16 *)guid.data;
    *w++ = SDL_Swap16LE(bus);
    *w++ = SDL_Swap16LE(crc);
    *w++ = SDL_Swap16LE(vendor);
    *w++ = 0;
    *w++ = SDL_Swap16LE(product);
    *w++ = 0;
    *w++ = SDL_Swap16LE(version);
    return guid;
}

static void sdloP_copy_arrays(SDL_Joystick *j, const SDLop_JoystickDeviceDesc *desc)
{
    if (desc->num_axes > 0 && desc->axes) {
        j->axis_codes = (Uint16 *)SDL_malloc((size_t)desc->num_axes * sizeof(Uint16));
        j->axis_min = (int *)SDL_malloc((size_t)desc->num_axes * sizeof(int));
        j->axis_max = (int *)SDL_malloc((size_t)desc->num_axes * sizeof(int));
        for (int i = 0; i < desc->num_axes; ++i) {
            j->axis_codes[i] = desc->axes[i].code;
            j->axis_min[i] = desc->axes[i].min;
            j->axis_max[i] = desc->axes[i].max;
        }
    }
    if (desc->num_buttons > 0 && desc->buttons) {
        j->button_codes = (Uint16 *)SDL_malloc((size_t)desc->num_buttons * sizeof(Uint16));
        for (int i = 0; i < desc->num_buttons; ++i) {
            j->button_codes[i] = desc->buttons[i];
        }
    }
    if (desc->num_hats > 0 && desc->hats) {
        j->hats_info = (SDLop_JoystickHatInfo *)SDL_malloc((size_t)desc->num_hats * sizeof(*j->hats_info));
        for (int i = 0; i < desc->num_hats; ++i) {
            j->hats_info[i] = desc->hats[i];
        }
    }
}

static void sdloP_setup_gamepad(SDL_Joystick *j)
{
    memset(j->gpad_axis, 0xff, sizeof(j->gpad_axis));
    memset(j->gpad_button, 0xff, sizeof(j->gpad_button));
    j->gpad_hat = -1;
    if (!j->is_gamepad) {
        return;
    }
#ifdef __linux__
    static const struct { Uint16 code; SDL_GamepadAxis axis; } axis_map[] = {
        { ABS_X, SDL_GAMEPAD_AXIS_LEFTX }, { ABS_Y, SDL_GAMEPAD_AXIS_LEFTY },
        { ABS_RX, SDL_GAMEPAD_AXIS_RIGHTX }, { ABS_RY, SDL_GAMEPAD_AXIS_RIGHTY },
        { ABS_Z, SDL_GAMEPAD_AXIS_LEFT_TRIGGER }, { ABS_RZ, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER },
    };
    static const struct { Uint16 code; SDL_GamepadButton button; } button_map[] = {
        { BTN_SOUTH, SDL_GAMEPAD_BUTTON_SOUTH }, { BTN_EAST, SDL_GAMEPAD_BUTTON_EAST },
        { BTN_WEST, SDL_GAMEPAD_BUTTON_WEST }, { BTN_NORTH, SDL_GAMEPAD_BUTTON_NORTH },
        { BTN_TL, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER }, { BTN_TR, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
        { BTN_SELECT, SDL_GAMEPAD_BUTTON_BACK }, { BTN_START, SDL_GAMEPAD_BUTTON_START },
        { BTN_MODE, SDL_GAMEPAD_BUTTON_GUIDE }, { BTN_THUMBL, SDL_GAMEPAD_BUTTON_LEFT_STICK },
        { BTN_THUMBR, SDL_GAMEPAD_BUTTON_RIGHT_STICK },
    };
    for (int i = 0; i < j->num_axes; ++i) {
        for (size_t k = 0; k < SDL_arraysize(axis_map); ++k) {
            if (j->axis_codes[i] == axis_map[k].code) {
                j->gpad_axis[axis_map[k].axis] = (Uint8)i;
            }
        }
    }
    for (int i = 0; i < j->num_buttons; ++i) {
        for (size_t k = 0; k < SDL_arraysize(button_map); ++k) {
            if (j->button_codes[i] == button_map[k].code) {
                j->gpad_button[button_map[k].button] = (Uint8)i;
            }
        }
    }
    if (j->num_hats > 0) {
        j->gpad_hat = 0;
    }
#endif
}

SDL_Joystick *SDLOP_JoystickAddDevice(const SDLop_JoystickDeviceDesc *desc, void *driverdata)
{
    if (!desc || !desc->name) {
        return NULL;
    }
    SDL_Joystick *j = (SDL_Joystick *)SDL_calloc(1, sizeof(*j));
    if (!j) {
        return NULL;
    }
    j->magic = SDLOP_JOYSTICK_MAGIC;
    j->instance_id = next_instance_id++;
    j->name = SDL_strdup(desc->name);
    j->path = desc->path ? SDL_strdup(desc->path) : NULL;
    j->bus = desc->bus;
    j->vendor = desc->vendor;
    j->product = desc->product;
    j->version = desc->version;
    j->firmware = desc->firmware;
    j->type = desc->type;
    j->is_gamepad = desc->is_gamepad;
    j->gamepad_type = desc->is_gamepad ? desc->gamepad_type : SDL_GAMEPAD_TYPE_UNKNOWN;
    j->player_index = desc->is_gamepad ? next_player_index++ : -1;
    j->connected = true;
    j->num_axes = desc->num_axes;
    j->num_buttons = desc->num_buttons;
    j->num_hats = desc->num_hats;
    j->axes = (Sint16 *)SDL_calloc((size_t)(j->num_axes ? j->num_axes : 1), sizeof(Sint16));
    j->axes_initial = (Sint16 *)SDL_calloc((size_t)(j->num_axes ? j->num_axes : 1), sizeof(Sint16));
    j->buttons = (bool *)SDL_calloc((size_t)(j->num_buttons ? j->num_buttons : 1), sizeof(bool));
    j->hats = (Uint8 *)SDL_calloc((size_t)(j->num_hats ? j->num_hats : 1), sizeof(Uint8));
    if (desc->axes_initial) {
        SDL_memcpy(j->axes, desc->axes_initial, (size_t)j->num_axes * sizeof(Sint16));
        SDL_memcpy(j->axes_initial, desc->axes_initial, (size_t)j->num_axes * sizeof(Sint16));
    }
    if (desc->buttons_initial) {
        SDL_memcpy(j->buttons, desc->buttons_initial, (size_t)j->num_buttons * sizeof(bool));
    }
    if (desc->hats_initial) {
        SDL_memcpy(j->hats, desc->hats_initial, (size_t)j->num_hats * sizeof(Uint8));
    }
    sdloP_copy_arrays(j, desc);
    j->guid = SDLOP_CreateJoystickGUID(desc->bus, desc->vendor, desc->product, desc->version, desc->name);
    sdloP_setup_gamepad(j);
    j->driverdata = driverdata;

    SDL_LockJoysticks();
    j->next = joysticks;
    joysticks = j;
    SDL_UnlockJoysticks();
    SDLOP_JoystickSendDeviceEvent(j, SDL_EVENT_JOYSTICK_ADDED);
    if (j->is_gamepad) {
        SDLOP_JoystickSendDeviceEvent(j, SDL_EVENT_GAMEPAD_ADDED);
    }
    return j;
}

void SDLOP_JoystickRemoveDevice(SDL_Joystick *joystick)
{
    if (!SDLOP_JoystickValid(joystick)) {
        return;
    }
    SDL_LockJoysticks();
    joystick->connected = false;
    SDL_Joystick **link = &joysticks;
    while (*link && *link != joystick) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = joystick->next;
        joystick->next = NULL;
    }
    SDL_UnlockJoysticks();
    SDLOP_JoystickSendDeviceEvent(joystick, SDL_EVENT_JOYSTICK_REMOVED);
    if (joystick->is_gamepad) {
        SDLOP_JoystickSendDeviceEvent(joystick, SDL_EVENT_GAMEPAD_REMOVED);
    }
    if (joystick->ref_count == 0 && joystick->gamepad_refs == 0) {
        sdloP_destroy_joystick(joystick);
    }
}

void SDLOP_JoystickQuitAllDevices(void)
{
    SDL_LockJoysticks();
    while (gamepads) {
        sdloP_destroy_gamepad(gamepads);
    }
    while (joysticks) {
        SDL_Joystick *j = joysticks;
        joysticks = j->next;
        j->connected = false;
        sdloP_destroy_joystick(j);
    }
    next_instance_id = 1;
    next_player_index = 0;
    SDL_UnlockJoysticks();
}

static void sdloP_push_input(SDL_Event *event)
{
    event->common.timestamp = SDL_GetTicksNS();
    SDLOP_PushInputEvent(event);
}

static Sint16 sdloP_gamepad_axis_value(SDL_Joystick *joystick, int axis, Sint16 value)
{
    /* evdev triggers are unsigned; map their centred -32768..32767 value
       onto SDL's 0..32767 gamepad trigger range. */
    Uint16 code = axis >= 0 && axis < joystick->num_axes ? joystick->axis_codes[axis] : 0;
#ifdef __linux__
    if (code == ABS_Z || code == ABS_RZ) {
        return (Sint16)((value + 32768) / 2);
    }
#else
    (void)code;
#endif
    return value;
}

void SDLOP_JoystickSendAxis(SDL_Joystick *joystick, Uint8 axis, Sint16 value)
{
    if (!SDLOP_JoystickValid(joystick) || !joystick->connected || axis >= joystick->num_axes) {
        return;
    }
    if (joystick->axes[axis] == value) {
        return;
    }
    joystick->axes[axis] = value;
    if (joystick_events_enabled) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.jaxis.type = SDL_EVENT_JOYSTICK_AXIS_MOTION;
        ev.jaxis.which = joystick->instance_id;
        ev.jaxis.axis = axis;
        ev.jaxis.value = value;
        sdloP_push_input(&ev);
    }
    if (joystick->is_gamepad && gamepad_events_enabled) {
        for (int g = 0; g < SDL_GAMEPAD_AXIS_COUNT; ++g) {
            if (joystick->gpad_axis[g] == axis) {
                SDL_Event ev;
                SDL_zero(ev);
                ev.gaxis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                ev.gaxis.which = joystick->instance_id;
                ev.gaxis.axis = (Uint8)g;
                ev.gaxis.value = sdloP_gamepad_axis_value(joystick, axis, value);
                sdloP_push_input(&ev);
            }
        }
    }
}

void SDLOP_JoystickSendButton(SDL_Joystick *joystick, Uint8 button, bool down)
{
    if (!SDLOP_JoystickValid(joystick) || !joystick->connected || button >= joystick->num_buttons) {
        return;
    }
    if (joystick->buttons[button] == down) {
        return;
    }
    joystick->buttons[button] = down;
    if (joystick_events_enabled) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.jbutton.type = down ? SDL_EVENT_JOYSTICK_BUTTON_DOWN : SDL_EVENT_JOYSTICK_BUTTON_UP;
        ev.jbutton.which = joystick->instance_id;
        ev.jbutton.button = button;
        ev.jbutton.down = down;
        sdloP_push_input(&ev);
    }
    if (joystick->is_gamepad && gamepad_events_enabled) {
        for (int g = 0; g < SDL_GAMEPAD_BUTTON_COUNT; ++g) {
            if (joystick->gpad_button[g] == button) {
                SDL_Event ev;
                SDL_zero(ev);
                ev.gbutton.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
                ev.gbutton.which = joystick->instance_id;
                ev.gbutton.button = (Uint8)g;
                ev.gbutton.down = down;
                sdloP_push_input(&ev);
            }
        }
    }
}

void SDLOP_JoystickSendHat(SDL_Joystick *joystick, Uint8 hat, Uint8 value)
{
    if (!SDLOP_JoystickValid(joystick) || !joystick->connected || hat >= joystick->num_hats) {
        return;
    }
    Uint8 old = joystick->hats[hat];
    if (old == value) {
        return;
    }
    joystick->hats[hat] = value;
    if (joystick_events_enabled) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.jhat.type = SDL_EVENT_JOYSTICK_HAT_MOTION;
        ev.jhat.which = joystick->instance_id;
        ev.jhat.hat = hat;
        ev.jhat.value = value;
        sdloP_push_input(&ev);
    }
    if (joystick->is_gamepad && gamepad_events_enabled && hat == (Uint8)joystick->gpad_hat) {
        static const struct { Uint8 bit; SDL_GamepadButton button; } dpad[] = {
            { SDL_HAT_UP, SDL_GAMEPAD_BUTTON_DPAD_UP },
            { SDL_HAT_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN },
            { SDL_HAT_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT },
            { SDL_HAT_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
        };
        for (size_t i = 0; i < SDL_arraysize(dpad); ++i) {
            if ((old ^ value) & dpad[i].bit) {
                bool down = (value & dpad[i].bit) != 0;
                SDL_Event ev;
                SDL_zero(ev);
                ev.gbutton.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
                ev.gbutton.which = joystick->instance_id;
                ev.gbutton.button = dpad[i].button;
                ev.gbutton.down = down;
                sdloP_push_input(&ev);
            }
        }
    }
}

void SDLOP_JoystickSendDeviceEvent(SDL_Joystick *joystick, SDL_EventType type)
{
    if (!SDLOP_JoystickValid(joystick)) {
        return;
    }
    SDL_Event ev;
    SDL_zero(ev);
    if (type == SDL_EVENT_JOYSTICK_ADDED || type == SDL_EVENT_JOYSTICK_REMOVED ||
        type == SDL_EVENT_JOYSTICK_UPDATE_COMPLETE) {
        ev.jdevice.type = type;
        ev.jdevice.which = joystick->instance_id;
    } else {
        ev.gdevice.type = type;
        ev.gdevice.which = joystick->instance_id;
    }
    sdloP_push_input(&ev);
}

int SDLOP_JoystickAxisIndex(SDL_Joystick *joystick, Uint16 code)
{
    if (!SDLOP_JoystickValid(joystick)) {
        return -1;
    }
    for (int i = 0; i < joystick->num_axes; ++i) {
        if (joystick->axis_codes[i] == code) {
            return i;
        }
    }
    return -1;
}

int SDLOP_JoystickButtonIndex(SDL_Joystick *joystick, Uint16 code)
{
    if (!SDLOP_JoystickValid(joystick)) {
        return -1;
    }
    for (int i = 0; i < joystick->num_buttons; ++i) {
        if (joystick->button_codes[i] == code) {
            return i;
        }
    }
    return -1;
}

int SDLOP_JoystickHatIndex(SDL_Joystick *joystick, Uint16 code, bool *is_x)
{
    if (!SDLOP_JoystickValid(joystick)) {
        return -1;
    }
    for (int i = 0; i < joystick->num_hats; ++i) {
        if (joystick->hats_info[i].xcode == code) {
            if (is_x) *is_x = true;
            return i;
        }
        if (joystick->hats_info[i].ycode == code) {
            if (is_x) *is_x = false;
            return i;
        }
    }
    return -1;
}

static bool sdloP_bad_joystick(SDL_Joystick *joystick)
{
    if (!SDLOP_JoystickValid(joystick)) {
        SDL_InvalidParamError("joystick");
        return true;
    }
    return false;
}

static bool sdloP_bad_gamepad(SDL_Gamepad *gamepad)
{
    if (!SDLOP_GamepadValid(gamepad)) {
        SDL_InvalidParamError("gamepad");
        return true;
    }
    return false;
}

static SDL_Joystick *sdloP_gamepad_joystick(SDL_JoystickID instance_id);

static char *sdloP_gamepad_mapping(SDL_Joystick *joystick)
{
    static const char *axis_names[SDL_GAMEPAD_AXIS_COUNT] = {
        "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"
    };
    static const char *button_names[SDL_GAMEPAD_BUTTON_COUNT] = {
        "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
        "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright",
        "misc1", "rightpaddle1", "leftpaddle1", "rightpaddle2", "leftpaddle2",
        "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6"
    };
    char guid[64];
    SDL_GUIDToString(joystick->guid, guid, sizeof(guid));

    char mapping[1024];
    size_t pos = (size_t)SDL_snprintf(mapping, sizeof(mapping), "%s,SDLop Generic Gamepad", guid);
    int n;
    for (int g = 0; g < SDL_GAMEPAD_AXIS_COUNT && pos + 1 < sizeof(mapping); ++g) {
        int index = joystick->gpad_axis[g];
        if (index == 0xff) continue;
        n = SDL_snprintf(mapping + pos, sizeof(mapping) - pos, ",%s:a%d", axis_names[g], index);
        if (n < 0 || (size_t)n >= sizeof(mapping) - pos) break;
        pos += (size_t)n;
    }
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && pos + 1 < sizeof(mapping); ++b) {
        int index = joystick->gpad_button[b];
        if (index == 0xff || b >= SDL_GAMEPAD_BUTTON_DPAD_UP) continue; /* hats below */
        n = SDL_snprintf(mapping + pos, sizeof(mapping) - pos, ",%s:b%d", button_names[b], index);
        if (n < 0 || (size_t)n >= sizeof(mapping) - pos) break;
        pos += (size_t)n;
    }
    if (joystick->gpad_hat >= 0 && pos + 1 < sizeof(mapping)) {
        static const struct { const char *name; Uint8 bit; } dpad[] = {
            { "dpup", SDL_HAT_UP }, { "dpdown", SDL_HAT_DOWN },
            { "dpleft", SDL_HAT_LEFT }, { "dpright", SDL_HAT_RIGHT }
        };
        for (size_t i = 0; i < SDL_arraysize(dpad) && pos + 1 < sizeof(mapping); ++i) {
            n = SDL_snprintf(mapping + pos, sizeof(mapping) - pos, ",%s:h%d.%u",
                             dpad[i].name, joystick->gpad_hat, (unsigned)dpad[i].bit);
            if (n < 0 || (size_t)n >= sizeof(mapping) - pos) break;
            pos += (size_t)n;
        }
    }
    if (pos + 1 < sizeof(mapping)) {
        SDL_snprintf(mapping + pos, sizeof(mapping) - pos, ",platform:Linux");
    }
    return SDL_strdup(mapping);
}

/* ------------------------------------------------------------------ */
/* Joystick                                                            */
/* ------------------------------------------------------------------ */

void SDL_LockJoysticks(void)
{
    pthread_once(&joystick_lock_once, joystick_lock_init);
    pthread_mutex_lock(&joystick_lock);
}

void SDL_UnlockJoysticks(void)
{
    pthread_once(&joystick_lock_once, joystick_lock_init);
    pthread_mutex_unlock(&joystick_lock);
}

bool SDL_HasJoystick(void)
{
    SDL_LockJoysticks();
    bool found = false;
    for (SDL_Joystick *j = joysticks; j; j = j->next) {
        if (j->connected) {
            found = true;
            break;
        }
    }
    SDL_UnlockJoysticks();
    return found;
}

SDL_JoystickID *SDL_GetJoysticks(int *count)
{
    SDL_LockJoysticks();
    int n = 0;
    for (SDL_Joystick *j = joysticks; j; j = j->next) {
        if (j->connected) ++n;
    }
    SDL_JoystickID *ids = (SDL_JoystickID *)SDL_calloc((size_t)n + 1, sizeof(SDL_JoystickID));
    if (!ids) {
        SDL_OutOfMemory();
        if (count) *count = 0;
    } else {
        int i = 0;
        for (SDL_Joystick *j = joysticks; j; j = j->next) {
            if (j->connected) ids[i++] = j->instance_id;
        }
        if (count) *count = n;
    }
    SDL_UnlockJoysticks();
    return ids;
}

const char *SDL_GetJoystickNameForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    return j ? j->name : NULL;
}

const char *SDL_GetJoystickPathForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    return j ? j->path : NULL;
}

int SDL_GetJoystickPlayerIndexForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    return j ? j->player_index : -1;
}

SDL_GUID SDL_GetJoystickGUIDForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    return j ? j->guid : zero_guid();
}

Uint16 SDL_GetJoystickVendorForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    return j ? j->vendor : 0;
}

Uint16 SDL_GetJoystickProductForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    return j ? j->product : 0;
}

Uint16 SDL_GetJoystickProductVersionForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    return j ? j->version : 0;
}

SDL_JoystickType SDL_GetJoystickTypeForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    return j ? j->type : SDL_JOYSTICK_TYPE_UNKNOWN;
}

SDL_Joystick *SDL_OpenJoystick(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    if (!j || !j->connected) {
        no_joystick(instance_id);
        return NULL;
    }
    ++j->ref_count;
    return j;
}

SDL_Joystick *SDL_GetJoystickFromID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    return (j && j->connected) ? j : NULL;
}

SDL_Joystick *SDL_GetJoystickFromPlayerIndex(int player_index)
{
    if (player_index < 0) return NULL;
    for (SDL_Joystick *j = joysticks; j; j = j->next) {
        if (j->connected && j->player_index == player_index) return j;
    }
    return NULL;
}

SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc *desc)
{
    if (!desc) {
        return SDL_InvalidParamError("desc"), 0;
    }
    return SDL_SetError("SDLop: virtual joysticks are not supported"), 0;
}

bool SDL_DetachVirtualJoystick(SDL_JoystickID instance_id)
{
    return SDL_SetError("Virtual joystick %" SDL_PRIu32 " not found", instance_id);
}

bool SDL_IsJoystickVirtual(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    return j && j->virtual_device;
}

bool SDL_SetJoystickVirtualAxis(SDL_Joystick *joystick, int axis, Sint16 value)
{
    (void)axis;
    (void)value;
    if (sdloP_bad_joystick(joystick)) return false;
    return SDL_SetError("Joystick is not virtual");
}

bool SDL_SetJoystickVirtualBall(SDL_Joystick *joystick, int ball, Sint16 xrel, Sint16 yrel)
{
    (void)ball;
    (void)xrel;
    (void)yrel;
    if (sdloP_bad_joystick(joystick)) return false;
    return SDL_SetError("Joystick is not virtual");
}

bool SDL_SetJoystickVirtualButton(SDL_Joystick *joystick, int button, bool down)
{
    (void)button;
    (void)down;
    if (sdloP_bad_joystick(joystick)) return false;
    return SDL_SetError("Joystick is not virtual");
}

bool SDL_SetJoystickVirtualHat(SDL_Joystick *joystick, int hat, Uint8 value)
{
    (void)hat;
    (void)value;
    if (sdloP_bad_joystick(joystick)) return false;
    return SDL_SetError("Joystick is not virtual");
}

bool SDL_SetJoystickVirtualTouchpad(SDL_Joystick *joystick, int touchpad, int finger, bool down, float x, float y, float pressure)
{
    (void)touchpad;
    (void)finger;
    (void)down;
    (void)x;
    (void)y;
    (void)pressure;
    if (sdloP_bad_joystick(joystick)) return false;
    return SDL_SetError("Joystick is not virtual");
}

bool SDL_SendJoystickVirtualSensorData(SDL_Joystick *joystick, SDL_SensorType type, Uint64 sensor_timestamp, const float *data, int num_values)
{
    (void)type;
    (void)sensor_timestamp;
    (void)data;
    (void)num_values;
    if (sdloP_bad_joystick(joystick)) return false;
    return SDL_SetError("Joystick has no sensors");
}

SDL_PropertiesID SDL_GetJoystickProperties(SDL_Joystick *joystick)
{
    if (sdloP_bad_joystick(joystick)) return 0;
    return SDL_SetError("Joystick properties are not supported"), 0;
}

const char *SDL_GetJoystickName(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->name : (sdloP_bad_joystick(joystick), NULL);
}

const char *SDL_GetJoystickPath(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->path : (sdloP_bad_joystick(joystick), NULL);
}

int SDL_GetJoystickPlayerIndex(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->player_index : (sdloP_bad_joystick(joystick), -1);
}

bool SDL_SetJoystickPlayerIndex(SDL_Joystick *joystick, int player_index)
{
    if (sdloP_bad_joystick(joystick)) return false;
    joystick->player_index = player_index;
    return true;
}

SDL_GUID SDL_GetJoystickGUID(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->guid : (sdloP_bad_joystick(joystick), zero_guid());
}

Uint16 SDL_GetJoystickVendor(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->vendor : (sdloP_bad_joystick(joystick), 0);
}

Uint16 SDL_GetJoystickProduct(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->product : (sdloP_bad_joystick(joystick), 0);
}

Uint16 SDL_GetJoystickProductVersion(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->version : (sdloP_bad_joystick(joystick), 0);
}

Uint16 SDL_GetJoystickFirmwareVersion(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->firmware : (sdloP_bad_joystick(joystick), 0);
}

const char *SDL_GetJoystickSerial(SDL_Joystick *joystick)
{
    if (sdloP_bad_joystick(joystick)) return NULL;
    return NULL;
}

SDL_JoystickType SDL_GetJoystickType(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->type : (sdloP_bad_joystick(joystick), SDL_JOYSTICK_TYPE_UNKNOWN);
}

void SDL_GetJoystickGUIDInfo(SDL_GUID guid, Uint16 *vendor, Uint16 *product, Uint16 *version, Uint16 *crc16)
{
    /* Port of SDL_joystick.c: standard form is bus, crc16, vendor, 0,
     * product, 0, version, driver (16-bit little-endian words). */
    const Uint8 *d = guid.data;
    Uint16 w[8];
    for (int i = 0; i < 8; i++) {
        w[i] = (Uint16)(d[i * 2] | (d[i * 2 + 1] << 8));
    }
    const Uint16 bus = w[0];
    const bool known_bus = (bus < ' ' || bus == 0xFF /* virtual */);
    const bool standard = known_bus && w[3] == 0 && w[5] == 0;
    if (vendor) {
        *vendor = standard ? w[2] : 0;
    }
    if (product) {
        *product = standard ? w[4] : 0;
    }
    if (version) {
        *version = standard ? w[6] : 0;
    }
    if (crc16) {
        *crc16 = known_bus ? w[1] : 0;
    }
}

bool SDL_JoystickConnected(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) && joystick->connected;
}

SDL_JoystickID SDL_GetJoystickID(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->instance_id : (sdloP_bad_joystick(joystick), 0);
}

int SDL_GetNumJoystickAxes(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->num_axes : (sdloP_bad_joystick(joystick), -1);
}

int SDL_GetNumJoystickBalls(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->num_balls : (sdloP_bad_joystick(joystick), -1);
}

int SDL_GetNumJoystickHats(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->num_hats : (sdloP_bad_joystick(joystick), -1);
}

int SDL_GetNumJoystickButtons(SDL_Joystick *joystick)
{
    return SDLOP_JoystickValid(joystick) ? joystick->num_buttons : (sdloP_bad_joystick(joystick), -1);
}

void SDL_SetJoystickEventsEnabled(bool enabled)
{
    joystick_events_enabled = enabled;
}

bool SDL_JoystickEventsEnabled(void)
{
    return joystick_events_enabled;
}

void SDL_UpdateJoysticks(void)
{
    SDLOP_JoystickPump();
}

Sint16 SDL_GetJoystickAxis(SDL_Joystick *joystick, int axis)
{
    if (sdloP_bad_joystick(joystick)) return 0;
    if (axis < 0 || axis >= joystick->num_axes) {
        SDL_InvalidParamError("axis");
        return 0;
    }
    return joystick->axes[axis];
}

bool SDL_GetJoystickAxisInitialState(SDL_Joystick *joystick, int axis, Sint16 *state)
{
    if (sdloP_bad_joystick(joystick)) return false;
    if (axis < 0 || axis >= joystick->num_axes) {
        return SDL_InvalidParamError("axis");
    }
    if (state) *state = joystick->axes_initial[axis];
    return true;
}

bool SDL_GetJoystickBall(SDL_Joystick *joystick, int ball, int *dx, int *dy)
{
    (void)ball;
    if (sdloP_bad_joystick(joystick)) return false;
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    return SDL_SetError("Joystick has no trackballs");
}

Uint8 SDL_GetJoystickHat(SDL_Joystick *joystick, int hat)
{
    if (sdloP_bad_joystick(joystick)) return SDL_HAT_CENTERED;
    if (hat < 0 || hat >= joystick->num_hats) {
        SDL_InvalidParamError("hat");
        return SDL_HAT_CENTERED;
    }
    return joystick->hats[hat];
}

bool SDL_GetJoystickButton(SDL_Joystick *joystick, int button)
{
    if (sdloP_bad_joystick(joystick)) return false;
    if (button < 0 || button >= joystick->num_buttons) {
        SDL_InvalidParamError("button");
        return false;
    }
    return joystick->buttons[button];
}

bool SDL_RumbleJoystick(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms)
{
    (void)low_frequency_rumble;
    (void)high_frequency_rumble;
    (void)duration_ms;
    if (sdloP_bad_joystick(joystick)) return false;
    return SDL_SetError("Rumble is not supported");
}

bool SDL_RumbleJoystickTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble, Uint32 duration_ms)
{
    (void)left_rumble;
    (void)right_rumble;
    (void)duration_ms;
    if (sdloP_bad_joystick(joystick)) return false;
    return SDL_SetError("Rumble is not supported");
}

bool SDL_SetJoystickLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)red;
    (void)green;
    (void)blue;
    if (sdloP_bad_joystick(joystick)) return false;
    return SDL_SetError("LED control is not supported");
}

bool SDL_SendJoystickEffect(SDL_Joystick *joystick, const void *data, int size)
{
    (void)data;
    (void)size;
    if (sdloP_bad_joystick(joystick)) return false;
    return SDL_SetError("Joystick effects are not supported");
}

void SDL_CloseJoystick(SDL_Joystick *joystick)
{
    if (SDLOP_JoystickValid(joystick)) {
        sdloP_release_joystick(joystick);
    }
}

SDL_JoystickConnectionState SDL_GetJoystickConnectionState(SDL_Joystick *joystick)
{
    if (!SDLOP_JoystickValid(joystick)) return SDL_JOYSTICK_CONNECTION_INVALID;
    return joystick->connected ? SDL_JOYSTICK_CONNECTION_WIRED : SDL_JOYSTICK_CONNECTION_UNKNOWN;
}

SDL_PowerState SDL_GetJoystickPowerInfo(SDL_Joystick *joystick, int *percent)
{
    if (percent) *percent = -1;
    if (!SDLOP_JoystickValid(joystick)) return SDL_POWERSTATE_ERROR;
    return SDL_POWERSTATE_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Gamepad: string / enum helpers (pure, same as stock)                */
/* ------------------------------------------------------------------ */

static const char *map_StringForGamepadType[] = {
    "unknown", "standard", "xbox360", "xboxone", "ps3", "ps4", "ps5",
    "switchpro", "joyconleft", "joyconright", "joyconpair", "gamecube", "steam"
};
SDL_COMPILE_TIME_ASSERT(map_StringForGamepadType, SDL_arraysize(map_StringForGamepadType) == SDL_GAMEPAD_TYPE_COUNT);

static const char *map_StringForGamepadAxis[] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"
};
SDL_COMPILE_TIME_ASSERT(map_StringForGamepadAxis, SDL_arraysize(map_StringForGamepadAxis) == SDL_GAMEPAD_AXIS_COUNT);

static const char *map_StringForGamepadButton[] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright",
    "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad",
    "misc2", "misc3", "misc4", "misc5", "misc6"
};
SDL_COMPILE_TIME_ASSERT(map_StringForGamepadButton, SDL_arraysize(map_StringForGamepadButton) == SDL_GAMEPAD_BUTTON_COUNT);

SDL_GamepadType SDL_GetGamepadTypeFromString(const char *str)
{
    if (!str || !*str) {
        return SDL_GAMEPAD_TYPE_UNKNOWN;
    }
    if (*str == '+' || *str == '-') {
        ++str;
    }
    for (int i = 0; i < (int)SDL_arraysize(map_StringForGamepadType); ++i) {
        if (strcasecmp(str, map_StringForGamepadType[i]) == 0) {
            return (SDL_GamepadType)i;
        }
    }
    return SDL_GAMEPAD_TYPE_UNKNOWN;
}

const char *SDL_GetGamepadStringForType(SDL_GamepadType type)
{
    if (type >= SDL_GAMEPAD_TYPE_STANDARD && type < SDL_GAMEPAD_TYPE_COUNT) {
        return map_StringForGamepadType[type];
    }
    return NULL;
}

SDL_GamepadAxis SDL_GetGamepadAxisFromString(const char *str)
{
    if (!str || !*str) {
        return SDL_GAMEPAD_AXIS_INVALID;
    }
    if (*str == '+' || *str == '-') {
        ++str;
    }
    for (int i = 0; i < (int)SDL_arraysize(map_StringForGamepadAxis); ++i) {
        if (strcasecmp(str, map_StringForGamepadAxis[i]) == 0) {
            return (SDL_GamepadAxis)i;
        }
    }
    return SDL_GAMEPAD_AXIS_INVALID;
}

const char *SDL_GetGamepadStringForAxis(SDL_GamepadAxis axis)
{
    if (axis > SDL_GAMEPAD_AXIS_INVALID && axis < SDL_GAMEPAD_AXIS_COUNT) {
        return map_StringForGamepadAxis[axis];
    }
    return NULL;
}

SDL_GamepadButton SDL_GetGamepadButtonFromString(const char *str)
{
    if (!str || !*str) {
        return SDL_GAMEPAD_BUTTON_INVALID;
    }
    for (int i = 0; i < (int)SDL_arraysize(map_StringForGamepadButton); ++i) {
        if (strcasecmp(str, map_StringForGamepadButton[i]) == 0) {
            return (SDL_GamepadButton)i;
        }
    }
    return SDL_GAMEPAD_BUTTON_INVALID;
}

const char *SDL_GetGamepadStringForButton(SDL_GamepadButton button)
{
    if (button > SDL_GAMEPAD_BUTTON_INVALID && button < SDL_GAMEPAD_BUTTON_COUNT) {
        return map_StringForGamepadButton[button];
    }
    return NULL;
}

SDL_GamepadButtonLabel SDL_GetGamepadButtonLabelForType(SDL_GamepadType type, SDL_GamepadButton button)
{
    /* face style per type, then label per face button (SDL_gamepad.c) */
    static const SDL_GamepadButtonLabel abxy[4] = {
        SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_B,
        SDL_GAMEPAD_BUTTON_LABEL_X, SDL_GAMEPAD_BUTTON_LABEL_Y
    };
    static const SDL_GamepadButtonLabel axby[4] = {
        SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_X,
        SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_LABEL_Y
    };
    static const SDL_GamepadButtonLabel bayx[4] = {
        SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_LABEL_A,
        SDL_GAMEPAD_BUTTON_LABEL_Y, SDL_GAMEPAD_BUTTON_LABEL_X
    };
    static const SDL_GamepadButtonLabel sony[4] = {
        SDL_GAMEPAD_BUTTON_LABEL_CROSS, SDL_GAMEPAD_BUTTON_LABEL_CIRCLE,
        SDL_GAMEPAD_BUTTON_LABEL_SQUARE, SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE
    };
    /* SOUTH, EAST, WEST, NORTH are 0..3 */
    if (button < SDL_GAMEPAD_BUTTON_SOUTH || button > SDL_GAMEPAD_BUTTON_NORTH) {
        return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    }
    switch (type) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return sony[button];
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return bayx[button];
    case SDL_GAMEPAD_TYPE_GAMECUBE:
        return axby[button];
    default:
        return abxy[button];
    }
}

/* ------------------------------------------------------------------ */
/* Gamepad: mappings                                                   */
/* ------------------------------------------------------------------ */

/* Mappings are accepted (an app loading gamecontrollerdb.txt must not
 * fail) but there is no device to apply them to, so none are kept. */
static bool looks_like_mapping(const char *line)
{
    /* "GUID,name,bindings..." : at least two commas, not a comment */
    if (!line || *line == '#' || *line == '\0') {
        return false;
    }
    const char *c = strchr(line, ',');
    return c && strchr(c + 1, ',');
}

int SDL_AddGamepadMapping(const char *mapping)
{
    if (!mapping) {
        SDL_InvalidParamError("mapping");
        return -1;
    }
    if (!looks_like_mapping(mapping)) {
        SDL_SetError("Invalid mapping string");
        return -1;
    }
    return 1;
}

int SDL_AddGamepadMappingsFromIO(SDL_IOStream *src, bool closeio)
{
    (void)closeio;
    /* SDLop implements no SDL_IOStream, so no valid stream can reach here */
    SDL_InvalidParamError("src");
    (void)src;
    return -1;
}

int SDL_AddGamepadMappingsFromFile(const char *file)
{
    if (!file) {
        SDL_InvalidParamError("file");
        return -1;
    }
    FILE *f = fopen(file, "r");
    if (!f) {
        SDL_SetError("Couldn't open %s", file);
        return -1;
    }
    int n = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (looks_like_mapping(line)) {
            n++;
        }
    }
    fclose(f);
    return n;
}

bool SDL_ReloadGamepadMappings(void)
{
    return true;
}

char **SDL_GetGamepadMappings(int *count)
{
    if (count) {
        *count = 0;
    }
    char **list = (char **)SDL_calloc(1, sizeof(char *));
    if (!list) {
        SDL_OutOfMemory();
    }
    return list;
}

char *SDL_GetGamepadMappingForGUID(SDL_GUID guid)
{
    for (SDL_Joystick *j = joysticks; j; j = j->next) {
        if (j->is_gamepad && SDL_memcmp(&j->guid, &guid, sizeof(guid)) == 0) {
            return sdloP_gamepad_mapping(j);
        }
    }
    SDL_SetError("Mapping not available");
    return NULL;
}

char *SDL_GetGamepadMapping(SDL_Gamepad *gamepad)
{
    if (!SDLOP_GamepadValid(gamepad)) return (sdloP_bad_gamepad(gamepad), NULL);
    return sdloP_gamepad_mapping(gamepad->joystick);
}

bool SDL_SetGamepadMapping(SDL_JoystickID instance_id, const char *mapping)
{
    (void)mapping;
    if (!sdloP_gamepad_joystick(instance_id)) return no_gamepad(instance_id);
    return SDL_SetError("Changing gamepad mappings is not supported");
}


/* ------------------------------------------------------------------ */
/* Gamepad helpers                                                     */
/* ------------------------------------------------------------------ */

static SDL_Joystick *sdloP_gamepad_joystick(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = SDLOP_JoystickFromInstanceID(instance_id);
    return (j && j->is_gamepad) ? j : NULL;
}

static SDL_Gamepad *sdloP_open_gamepad(SDL_Joystick *joystick)
{
    SDL_Gamepad *g = sdloP_gamepad_from_joystick(joystick);
    if (!g) {
        g = (SDL_Gamepad *)SDL_calloc(1, sizeof(*g));
        if (!g) {
            SDL_OutOfMemory();
            return NULL;
        }
        g->magic = SDLOP_GAMEPAD_MAGIC;
        g->joystick = joystick;
        g->next = gamepads;
        gamepads = g;
        ++joystick->gamepad_refs;
        ++joystick->ref_count; /* opening a gamepad opens its joystick */
    }
    ++g->ref_count;
    return g;
}

/* ------------------------------------------------------------------ */
/* Gamepad: enumeration and devices                                    */
/* ------------------------------------------------------------------ */

bool SDL_HasGamepad(void)
{
    for (SDL_Joystick *j = joysticks; j; j = j->next) {
        if (j->connected && j->is_gamepad) return true;
    }
    return false;
}

SDL_JoystickID *SDL_GetGamepads(int *count)
{
    int n = 0;
    for (SDL_Joystick *j = joysticks; j; j = j->next) {
        if (j->connected && j->is_gamepad) ++n;
    }
    SDL_JoystickID *ids = (SDL_JoystickID *)SDL_calloc((size_t)n + 1, sizeof(SDL_JoystickID));
    if (!ids) {
        SDL_OutOfMemory();
        if (count) *count = 0;
        return NULL;
    }
    int i = 0;
    for (SDL_Joystick *j = joysticks; j; j = j->next) {
        if (j->connected && j->is_gamepad) ids[i++] = j->instance_id;
    }
    if (count) *count = n;
    return ids;
}

bool SDL_IsGamepad(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = sdloP_gamepad_joystick(instance_id);
    return j && j->connected;
}

const char *SDL_GetGamepadNameForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = sdloP_gamepad_joystick(instance_id);
    return j ? j->name : NULL;
}

const char *SDL_GetGamepadPathForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = sdloP_gamepad_joystick(instance_id);
    return j ? j->path : NULL;
}

int SDL_GetGamepadPlayerIndexForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = sdloP_gamepad_joystick(instance_id);
    return j ? j->player_index : -1;
}

SDL_GUID SDL_GetGamepadGUIDForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = sdloP_gamepad_joystick(instance_id);
    return j ? j->guid : zero_guid();
}

Uint16 SDL_GetGamepadVendorForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = sdloP_gamepad_joystick(instance_id);
    return j ? j->vendor : 0;
}

Uint16 SDL_GetGamepadProductForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = sdloP_gamepad_joystick(instance_id);
    return j ? j->product : 0;
}

Uint16 SDL_GetGamepadProductVersionForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = sdloP_gamepad_joystick(instance_id);
    return j ? j->version : 0;
}

SDL_GamepadType SDL_GetGamepadTypeForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = sdloP_gamepad_joystick(instance_id);
    return j ? j->gamepad_type : SDL_GAMEPAD_TYPE_UNKNOWN;
}

SDL_GamepadType SDL_GetRealGamepadTypeForID(SDL_JoystickID instance_id)
{
    return SDL_GetGamepadTypeForID(instance_id);
}

char *SDL_GetGamepadMappingForID(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = sdloP_gamepad_joystick(instance_id);
    return j ? sdloP_gamepad_mapping(j) : NULL;
}

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance_id)
{
    SDL_Joystick *j = sdloP_gamepad_joystick(instance_id);
    if (!j || !j->connected) {
        no_gamepad(instance_id);
        return NULL;
    }
    return sdloP_open_gamepad(j);
}

SDL_Gamepad *SDL_GetGamepadFromID(SDL_JoystickID instance_id)
{
    for (SDL_Gamepad *g = gamepads; g; g = g->next) {
        if (SDLOP_GamepadValid(g) && g->joystick->instance_id == instance_id) return g;
    }
    return NULL;
}

SDL_Gamepad *SDL_GetGamepadFromPlayerIndex(int player_index)
{
    if (player_index < 0) return NULL;
    for (SDL_Gamepad *g = gamepads; g; g = g->next) {
        if (SDLOP_GamepadValid(g) && g->joystick->player_index == player_index) return g;
    }
    return NULL;
}

SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad *gamepad)
{
    if (sdloP_bad_gamepad(gamepad)) return 0;
    return SDL_SetError("Gamepad properties are not supported"), 0;
}

SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *gamepad)
{
    return SDLOP_GamepadValid(gamepad) ? gamepad->joystick->instance_id : (sdloP_bad_gamepad(gamepad), 0);
}

const char *SDL_GetGamepadName(SDL_Gamepad *gamepad)
{
    return SDLOP_GamepadValid(gamepad) ? gamepad->joystick->name : (sdloP_bad_gamepad(gamepad), NULL);
}

const char *SDL_GetGamepadPath(SDL_Gamepad *gamepad)
{
    return SDLOP_GamepadValid(gamepad) ? gamepad->joystick->path : (sdloP_bad_gamepad(gamepad), NULL);
}

SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad)
{
    return SDLOP_GamepadValid(gamepad) ? gamepad->joystick->gamepad_type : (sdloP_bad_gamepad(gamepad), SDL_GAMEPAD_TYPE_UNKNOWN);
}

SDL_GamepadType SDL_GetRealGamepadType(SDL_Gamepad *gamepad)
{
    return SDL_GetGamepadType(gamepad);
}

int SDL_GetGamepadPlayerIndex(SDL_Gamepad *gamepad)
{
    return SDLOP_GamepadValid(gamepad) ? gamepad->joystick->player_index : (sdloP_bad_gamepad(gamepad), -1);
}

bool SDL_SetGamepadPlayerIndex(SDL_Gamepad *gamepad, int player_index)
{
    if (sdloP_bad_gamepad(gamepad)) return false;
    gamepad->joystick->player_index = player_index;
    return true;
}

Uint16 SDL_GetGamepadVendor(SDL_Gamepad *gamepad)
{
    return SDLOP_GamepadValid(gamepad) ? gamepad->joystick->vendor : (sdloP_bad_gamepad(gamepad), 0);
}

Uint16 SDL_GetGamepadProduct(SDL_Gamepad *gamepad)
{
    return SDLOP_GamepadValid(gamepad) ? gamepad->joystick->product : (sdloP_bad_gamepad(gamepad), 0);
}

Uint16 SDL_GetGamepadProductVersion(SDL_Gamepad *gamepad)
{
    return SDLOP_GamepadValid(gamepad) ? gamepad->joystick->version : (sdloP_bad_gamepad(gamepad), 0);
}

Uint16 SDL_GetGamepadFirmwareVersion(SDL_Gamepad *gamepad)
{
    return SDLOP_GamepadValid(gamepad) ? gamepad->joystick->firmware : (sdloP_bad_gamepad(gamepad), 0);
}

const char *SDL_GetGamepadSerial(SDL_Gamepad *gamepad)
{
    if (sdloP_bad_gamepad(gamepad)) return NULL;
    return NULL;
}

Uint64 SDL_GetGamepadSteamHandle(SDL_Gamepad *gamepad)
{
    return sdloP_bad_gamepad(gamepad), 0;
}

SDL_JoystickConnectionState SDL_GetGamepadConnectionState(SDL_Gamepad *gamepad)
{
    if (!SDLOP_GamepadValid(gamepad)) return SDL_JOYSTICK_CONNECTION_INVALID;
    return gamepad->joystick->connected ? SDL_JOYSTICK_CONNECTION_WIRED : SDL_JOYSTICK_CONNECTION_UNKNOWN;
}

SDL_PowerState SDL_GetGamepadPowerInfo(SDL_Gamepad *gamepad, int *percent)
{
    if (percent) *percent = -1;
    if (!SDLOP_GamepadValid(gamepad)) return SDL_POWERSTATE_ERROR;
    return SDL_POWERSTATE_UNKNOWN;
}

bool SDL_GamepadConnected(SDL_Gamepad *gamepad)
{
    return SDLOP_GamepadValid(gamepad) && gamepad->joystick->connected;
}

SDL_Joystick *SDL_GetGamepadJoystick(SDL_Gamepad *gamepad)
{
    return SDLOP_GamepadValid(gamepad) ? gamepad->joystick : (sdloP_bad_gamepad(gamepad), NULL);
}

void SDL_SetGamepadEventsEnabled(bool enabled)
{
    gamepad_events_enabled = enabled;
}

bool SDL_GamepadEventsEnabled(void)
{
    return gamepad_events_enabled;
}

SDL_GamepadBinding **SDL_GetGamepadBindings(SDL_Gamepad *gamepad, int *count)
{
    if (count) *count = 0;
    if (sdloP_bad_gamepad(gamepad)) return NULL;
    return (SDL_GamepadBinding **)SDL_calloc(1, sizeof(SDL_GamepadBinding *));
}

void SDL_UpdateGamepads(void)
{
    SDLOP_JoystickPump();
}

bool SDL_GamepadHasAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
    if (!SDLOP_GamepadValid(gamepad) || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) return false;
    return gamepad->joystick->gpad_axis[axis] != 0xff;
}

Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
    if (sdloP_bad_gamepad(gamepad)) return 0;
    if (axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) return SDL_InvalidParamError("axis"), 0;
    int index = gamepad->joystick->gpad_axis[axis];
    if (index == 0xff) return 0;
    return sdloP_gamepad_axis_value(gamepad->joystick, index, gamepad->joystick->axes[index]);
}

bool SDL_GamepadHasButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
    if (!SDLOP_GamepadValid(gamepad) || button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) return false;
    SDL_Joystick *j = gamepad->joystick;
    if (button == SDL_GAMEPAD_BUTTON_DPAD_UP || button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ||
        button == SDL_GAMEPAD_BUTTON_DPAD_LEFT || button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
        return j->gpad_hat >= 0;
    }
    return j->gpad_button[button] != 0xff;
}

bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
    if (sdloP_bad_gamepad(gamepad)) return false;
    if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) return SDL_InvalidParamError("button"), false;
    SDL_Joystick *j = gamepad->joystick;
    static const struct { SDL_GamepadButton button; Uint8 bit; } dpad[] = {
        { SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_HAT_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_HAT_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_HAT_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_HAT_RIGHT },
    };
    for (size_t i = 0; i < SDL_arraysize(dpad); ++i) {
        if (button == dpad[i].button) {
            return j->gpad_hat >= 0 && (j->hats[j->gpad_hat] & dpad[i].bit) != 0;
        }
    }
    int index = j->gpad_button[button];
    return index != 0xff && index < j->num_buttons && j->buttons[index];
}

SDL_GamepadButtonLabel SDL_GetGamepadButtonLabel(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
    if (!SDLOP_GamepadValid(gamepad)) return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    return SDL_GetGamepadButtonLabelForType(gamepad->joystick->gamepad_type, button);
}

int SDL_GetNumGamepadTouchpads(SDL_Gamepad *gamepad)
{
    return sdloP_bad_gamepad(gamepad) ? 0 : 0;
}

int SDL_GetNumGamepadTouchpadFingers(SDL_Gamepad *gamepad, int touchpad)
{
    (void)touchpad;
    return sdloP_bad_gamepad(gamepad) ? 0 : 0;
}

bool SDL_GetGamepadTouchpadFinger(SDL_Gamepad *gamepad, int touchpad, int finger, bool *down, float *x, float *y, float *pressure)
{
    (void)touchpad;
    (void)finger;
    if (down) *down = false;
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    if (pressure) *pressure = 0.0f;
    return sdloP_bad_gamepad(gamepad) ? false : SDL_SetError("Gamepad has no touchpad");
}

bool SDL_GamepadHasSensor(SDL_Gamepad *gamepad, SDL_SensorType type)
{
    (void)type;
    return sdloP_bad_gamepad(gamepad) ? false : false;
}

bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *gamepad, SDL_SensorType type, bool enabled)
{
    (void)type;
    (void)enabled;
    return sdloP_bad_gamepad(gamepad) ? false : SDL_SetError("Gamepad has no sensors");
}

bool SDL_GamepadSensorEnabled(SDL_Gamepad *gamepad, SDL_SensorType type)
{
    (void)type;
    return sdloP_bad_gamepad(gamepad) ? false : false;
}

float SDL_GetGamepadSensorDataRate(SDL_Gamepad *gamepad, SDL_SensorType type)
{
    (void)gamepad;
    (void)type;
    return 0.0f;
}

bool SDL_GetGamepadSensorData(SDL_Gamepad *gamepad, SDL_SensorType type, float *data, int num_values)
{
    (void)type;
    if (data && num_values > 0) SDL_memset(data, 0, sizeof(float) * (size_t)num_values);
    return sdloP_bad_gamepad(gamepad) ? false : SDL_SetError("Gamepad has no sensors");
}

bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms)
{
    (void)low_frequency_rumble;
    (void)high_frequency_rumble;
    (void)duration_ms;
    return sdloP_bad_gamepad(gamepad) ? false : SDL_SetError("Rumble is not supported");
}

bool SDL_RumbleGamepadTriggers(SDL_Gamepad *gamepad, Uint16 left_rumble, Uint16 right_rumble, Uint32 duration_ms)
{
    (void)left_rumble;
    (void)right_rumble;
    (void)duration_ms;
    return sdloP_bad_gamepad(gamepad) ? false : SDL_SetError("Rumble is not supported");
}

bool SDL_SetGamepadLED(SDL_Gamepad *gamepad, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)red;
    (void)green;
    (void)blue;
    return sdloP_bad_gamepad(gamepad) ? false : SDL_SetError("LED control is not supported");
}

bool SDL_SendGamepadEffect(SDL_Gamepad *gamepad, const void *data, int size)
{
    (void)data;
    (void)size;
    return sdloP_bad_gamepad(gamepad) ? false : SDL_SetError("Gamepad effects are not supported");
}

void SDL_CloseGamepad(SDL_Gamepad *gamepad)
{
    if (!SDLOP_GamepadValid(gamepad)) return;
    SDL_Joystick *j = gamepad->joystick;
    if (gamepad->ref_count > 0) --gamepad->ref_count;
    if (j) {
        if (j->ref_count > 0) --j->ref_count;
        if (j->gamepad_refs > 0) --j->gamepad_refs;
    }
    if (gamepad->ref_count == 0) {
        SDL_Gamepad **link = &gamepads;
        while (*link && *link != gamepad) link = &(*link)->next;
        if (*link) *link = gamepad->next;
        gamepad->magic = 0;
        SDL_free(gamepad);
        if (j && j->ref_count == 0 && j->gamepad_refs == 0 && !j->connected) {
            sdloP_destroy_joystick(j);
        }
    }
}

const char *SDL_GetGamepadAppleSFSymbolsNameForButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
    (void)gamepad, (void)button;
    return NULL; /* Apple-only; stock Linux builds return NULL too */
}

const char *SDL_GetGamepadAppleSFSymbolsNameForAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
    (void)gamepad, (void)axis;
    return NULL;
}
