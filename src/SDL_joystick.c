/*
  SDLop - joystick and gamepad API without devices.

  SDLop covers windowing and input from keyboards, mice, touch and pens;
  game controllers are out of scope. Applications built against stock SDL3
  still call the joystick / gamepad API unconditionally (Dear ImGui's SDL3
  backend polls SDL_GetGamepads() every frame), so the whole API is exported
  and behaves exactly like stock SDL3 on a machine with no controller
  attached: enumeration returns empty lists, opening fails with an error,
  per-device queries reject the (necessarily invalid) handle, and the pure
  string/enum helpers work.

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

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

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

static SDL_JoystickID *empty_id_list(int *count)
{
    if (count) {
        *count = 0;
    }
    SDL_JoystickID *ids = (SDL_JoystickID *)SDL_calloc(1, sizeof(SDL_JoystickID));
    if (!ids) {
        SDL_OutOfMemory();
    }
    return ids;
}

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

/* No SDL_Joystick / SDL_Gamepad object can exist, so every handle is
 * invalid; stock SDL reports this through its magic check. */
#define BAD_JOYSTICK() SDL_InvalidParamError("joystick")
#define BAD_GAMEPAD()  SDL_InvalidParamError("gamepad")

static SDL_GUID zero_guid(void)
{
    SDL_GUID guid;
    memset(&guid, 0, sizeof(guid));
    return guid;
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
    return false;
}

SDL_JoystickID *SDL_GetJoysticks(int *count)
{
    return empty_id_list(count);
}

const char *SDL_GetJoystickNameForID(SDL_JoystickID instance_id)
{
    no_joystick(instance_id);
    return NULL;
}

const char *SDL_GetJoystickPathForID(SDL_JoystickID instance_id)
{
    no_joystick(instance_id);
    return NULL;
}

int SDL_GetJoystickPlayerIndexForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return -1;
}

SDL_GUID SDL_GetJoystickGUIDForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return zero_guid();
}

Uint16 SDL_GetJoystickVendorForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return 0;
}

Uint16 SDL_GetJoystickProductForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return 0;
}

Uint16 SDL_GetJoystickProductVersionForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return 0;
}

SDL_JoystickType SDL_GetJoystickTypeForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return SDL_JOYSTICK_TYPE_UNKNOWN;
}

SDL_Joystick *SDL_OpenJoystick(SDL_JoystickID instance_id)
{
    no_joystick(instance_id);
    return NULL;
}

SDL_Joystick *SDL_GetJoystickFromID(SDL_JoystickID instance_id)
{
    no_joystick(instance_id);
    return NULL;
}

SDL_Joystick *SDL_GetJoystickFromPlayerIndex(int player_index)
{
    SDL_SetError("Couldn't find joystick with player index %d", player_index);
    return NULL;
}

SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc *desc)
{
    if (!desc) {
        SDL_InvalidParamError("desc");
        return 0;
    }
    SDL_SetError("SDLop: virtual joysticks are not supported");
    return 0;
}

bool SDL_DetachVirtualJoystick(SDL_JoystickID instance_id)
{
    return SDL_SetError("Virtual joystick %" SDL_PRIu32 " not found", instance_id);
}

bool SDL_IsJoystickVirtual(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return false;
}

bool SDL_SetJoystickVirtualAxis(SDL_Joystick *joystick, int axis, Sint16 value)
{
    (void)joystick, (void)axis, (void)value;
    return BAD_JOYSTICK();
}

bool SDL_SetJoystickVirtualBall(SDL_Joystick *joystick, int ball, Sint16 xrel, Sint16 yrel)
{
    (void)joystick, (void)ball, (void)xrel, (void)yrel;
    return BAD_JOYSTICK();
}

bool SDL_SetJoystickVirtualButton(SDL_Joystick *joystick, int button, bool down)
{
    (void)joystick, (void)button, (void)down;
    return BAD_JOYSTICK();
}

bool SDL_SetJoystickVirtualHat(SDL_Joystick *joystick, int hat, Uint8 value)
{
    (void)joystick, (void)hat, (void)value;
    return BAD_JOYSTICK();
}

bool SDL_SetJoystickVirtualTouchpad(SDL_Joystick *joystick, int touchpad, int finger, bool down, float x, float y, float pressure)
{
    (void)joystick, (void)touchpad, (void)finger, (void)down, (void)x, (void)y, (void)pressure;
    return BAD_JOYSTICK();
}

bool SDL_SendJoystickVirtualSensorData(SDL_Joystick *joystick, SDL_SensorType type, Uint64 sensor_timestamp, const float *data, int num_values)
{
    (void)joystick, (void)type, (void)sensor_timestamp, (void)data, (void)num_values;
    return BAD_JOYSTICK();
}

SDL_PropertiesID SDL_GetJoystickProperties(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return 0;
}

const char *SDL_GetJoystickName(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return NULL;
}

const char *SDL_GetJoystickPath(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return NULL;
}

int SDL_GetJoystickPlayerIndex(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return -1;
}

bool SDL_SetJoystickPlayerIndex(SDL_Joystick *joystick, int player_index)
{
    (void)joystick, (void)player_index;
    return BAD_JOYSTICK();
}

SDL_GUID SDL_GetJoystickGUID(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return zero_guid();
}

Uint16 SDL_GetJoystickVendor(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return 0;
}

Uint16 SDL_GetJoystickProduct(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return 0;
}

Uint16 SDL_GetJoystickProductVersion(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return 0;
}

Uint16 SDL_GetJoystickFirmwareVersion(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return 0;
}

const char *SDL_GetJoystickSerial(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return NULL;
}

SDL_JoystickType SDL_GetJoystickType(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return SDL_JOYSTICK_TYPE_UNKNOWN;
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
    (void)joystick;
    BAD_JOYSTICK();
    return false;
}

SDL_JoystickID SDL_GetJoystickID(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return 0;
}

int SDL_GetNumJoystickAxes(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return -1;
}

int SDL_GetNumJoystickBalls(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return -1;
}

int SDL_GetNumJoystickHats(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return -1;
}

int SDL_GetNumJoystickButtons(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return -1;
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
}

Sint16 SDL_GetJoystickAxis(SDL_Joystick *joystick, int axis)
{
    (void)joystick, (void)axis;
    BAD_JOYSTICK();
    return 0;
}

bool SDL_GetJoystickAxisInitialState(SDL_Joystick *joystick, int axis, Sint16 *state)
{
    (void)axis;
    (void)joystick;
    if (state) {
        *state = 0;
    }
    return BAD_JOYSTICK();
}

bool SDL_GetJoystickBall(SDL_Joystick *joystick, int ball, int *dx, int *dy)
{
    (void)joystick, (void)ball;
    if (dx) {
        *dx = 0;
    }
    if (dy) {
        *dy = 0;
    }
    return BAD_JOYSTICK();
}

Uint8 SDL_GetJoystickHat(SDL_Joystick *joystick, int hat)
{
    (void)joystick, (void)hat;
    BAD_JOYSTICK();
    return SDL_HAT_CENTERED;
}

bool SDL_GetJoystickButton(SDL_Joystick *joystick, int button)
{
    (void)joystick, (void)button;
    BAD_JOYSTICK();
    return false;
}

bool SDL_RumbleJoystick(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms)
{
    (void)joystick, (void)low_frequency_rumble, (void)high_frequency_rumble, (void)duration_ms;
    return BAD_JOYSTICK();
}

bool SDL_RumbleJoystickTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble, Uint32 duration_ms)
{
    (void)joystick, (void)left_rumble, (void)right_rumble, (void)duration_ms;
    return BAD_JOYSTICK();
}

bool SDL_SetJoystickLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)joystick, (void)red, (void)green, (void)blue;
    return BAD_JOYSTICK();
}

bool SDL_SendJoystickEffect(SDL_Joystick *joystick, const void *data, int size)
{
    (void)joystick, (void)data, (void)size;
    return BAD_JOYSTICK();
}

void SDL_CloseJoystick(SDL_Joystick *joystick)
{
    (void)joystick; /* stock SDL ignores NULL / invalid handles silently */
}

SDL_JoystickConnectionState SDL_GetJoystickConnectionState(SDL_Joystick *joystick)
{
    (void)joystick;
    BAD_JOYSTICK();
    return SDL_JOYSTICK_CONNECTION_INVALID;
}

SDL_PowerState SDL_GetJoystickPowerInfo(SDL_Joystick *joystick, int *percent)
{
    (void)joystick;
    if (percent) {
        *percent = -1;
    }
    BAD_JOYSTICK();
    return SDL_POWERSTATE_ERROR;
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
    (void)guid;
    SDL_SetError("Mapping not available");
    return NULL;
}

char *SDL_GetGamepadMapping(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return NULL;
}

bool SDL_SetGamepadMapping(SDL_JoystickID instance_id, const char *mapping)
{
    (void)mapping;
    return no_joystick(instance_id);
}

/* ------------------------------------------------------------------ */
/* Gamepad: enumeration and devices                                    */
/* ------------------------------------------------------------------ */

bool SDL_HasGamepad(void)
{
    return false;
}

SDL_JoystickID *SDL_GetGamepads(int *count)
{
    return empty_id_list(count);
}

bool SDL_IsGamepad(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return false;
}

const char *SDL_GetGamepadNameForID(SDL_JoystickID instance_id)
{
    no_gamepad(instance_id);
    return NULL;
}

const char *SDL_GetGamepadPathForID(SDL_JoystickID instance_id)
{
    no_gamepad(instance_id);
    return NULL;
}

int SDL_GetGamepadPlayerIndexForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return -1;
}

SDL_GUID SDL_GetGamepadGUIDForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return zero_guid();
}

Uint16 SDL_GetGamepadVendorForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return 0;
}

Uint16 SDL_GetGamepadProductForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return 0;
}

Uint16 SDL_GetGamepadProductVersionForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return 0;
}

SDL_GamepadType SDL_GetGamepadTypeForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return SDL_GAMEPAD_TYPE_UNKNOWN;
}

SDL_GamepadType SDL_GetRealGamepadTypeForID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return SDL_GAMEPAD_TYPE_UNKNOWN;
}

char *SDL_GetGamepadMappingForID(SDL_JoystickID instance_id)
{
    no_gamepad(instance_id);
    return NULL;
}

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance_id)
{
    no_gamepad(instance_id);
    return NULL;
}

SDL_Gamepad *SDL_GetGamepadFromID(SDL_JoystickID instance_id)
{
    no_gamepad(instance_id);
    return NULL;
}

SDL_Gamepad *SDL_GetGamepadFromPlayerIndex(int player_index)
{
    SDL_SetError("Couldn't find gamepad with player index %d", player_index);
    return NULL;
}

SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return 0;
}

SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return 0;
}

const char *SDL_GetGamepadName(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return NULL;
}

const char *SDL_GetGamepadPath(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return NULL;
}

SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return SDL_GAMEPAD_TYPE_UNKNOWN;
}

SDL_GamepadType SDL_GetRealGamepadType(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return SDL_GAMEPAD_TYPE_UNKNOWN;
}

int SDL_GetGamepadPlayerIndex(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return -1;
}

bool SDL_SetGamepadPlayerIndex(SDL_Gamepad *gamepad, int player_index)
{
    (void)gamepad, (void)player_index;
    return BAD_GAMEPAD();
}

Uint16 SDL_GetGamepadVendor(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return 0;
}

Uint16 SDL_GetGamepadProduct(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return 0;
}

Uint16 SDL_GetGamepadProductVersion(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return 0;
}

Uint16 SDL_GetGamepadFirmwareVersion(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return 0;
}

const char *SDL_GetGamepadSerial(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return NULL;
}

Uint64 SDL_GetGamepadSteamHandle(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return 0;
}

SDL_JoystickConnectionState SDL_GetGamepadConnectionState(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return SDL_JOYSTICK_CONNECTION_INVALID;
}

SDL_PowerState SDL_GetGamepadPowerInfo(SDL_Gamepad *gamepad, int *percent)
{
    (void)gamepad;
    if (percent) {
        *percent = -1;
    }
    BAD_GAMEPAD();
    return SDL_POWERSTATE_ERROR;
}

bool SDL_GamepadConnected(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return false;
}

SDL_Joystick *SDL_GetGamepadJoystick(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    BAD_GAMEPAD();
    return NULL;
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
    (void)gamepad;
    if (count) {
        *count = 0;
    }
    BAD_GAMEPAD();
    return NULL;
}

void SDL_UpdateGamepads(void)
{
}

bool SDL_GamepadHasAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
    (void)gamepad, (void)axis;
    return false;
}

Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
    (void)gamepad, (void)axis;
    BAD_GAMEPAD();
    return 0;
}

bool SDL_GamepadHasButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
    (void)gamepad, (void)button;
    return false;
}

bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
    (void)gamepad, (void)button;
    BAD_GAMEPAD();
    return false;
}

SDL_GamepadButtonLabel SDL_GetGamepadButtonLabel(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
    (void)gamepad, (void)button;
    BAD_GAMEPAD();
    return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
}

int SDL_GetNumGamepadTouchpads(SDL_Gamepad *gamepad)
{
    (void)gamepad;
    return 0;
}

int SDL_GetNumGamepadTouchpadFingers(SDL_Gamepad *gamepad, int touchpad)
{
    (void)gamepad, (void)touchpad;
    return 0;
}

bool SDL_GetGamepadTouchpadFinger(SDL_Gamepad *gamepad, int touchpad, int finger, bool *down, float *x, float *y, float *pressure)
{
    (void)gamepad, (void)touchpad, (void)finger;
    if (down) {
        *down = false;
    }
    if (x) {
        *x = 0.0f;
    }
    if (y) {
        *y = 0.0f;
    }
    if (pressure) {
        *pressure = 0.0f;
    }
    return BAD_GAMEPAD();
}

bool SDL_GamepadHasSensor(SDL_Gamepad *gamepad, SDL_SensorType type)
{
    (void)gamepad, (void)type;
    return false;
}

bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *gamepad, SDL_SensorType type, bool enabled)
{
    (void)gamepad, (void)type, (void)enabled;
    return BAD_GAMEPAD();
}

bool SDL_GamepadSensorEnabled(SDL_Gamepad *gamepad, SDL_SensorType type)
{
    (void)gamepad, (void)type;
    return false;
}

float SDL_GetGamepadSensorDataRate(SDL_Gamepad *gamepad, SDL_SensorType type)
{
    (void)gamepad, (void)type;
    return 0.0f;
}

bool SDL_GetGamepadSensorData(SDL_Gamepad *gamepad, SDL_SensorType type, float *data, int num_values)
{
    (void)gamepad, (void)type;
    if (data && num_values > 0) {
        memset(data, 0, sizeof(float) * (size_t)num_values);
    }
    return BAD_GAMEPAD();
}

bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms)
{
    (void)gamepad, (void)low_frequency_rumble, (void)high_frequency_rumble, (void)duration_ms;
    return BAD_GAMEPAD();
}

bool SDL_RumbleGamepadTriggers(SDL_Gamepad *gamepad, Uint16 left_rumble, Uint16 right_rumble, Uint32 duration_ms)
{
    (void)gamepad, (void)left_rumble, (void)right_rumble, (void)duration_ms;
    return BAD_GAMEPAD();
}

bool SDL_SetGamepadLED(SDL_Gamepad *gamepad, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)gamepad, (void)red, (void)green, (void)blue;
    return BAD_GAMEPAD();
}

bool SDL_SendGamepadEffect(SDL_Gamepad *gamepad, const void *data, int size)
{
    (void)gamepad, (void)data, (void)size;
    return BAD_GAMEPAD();
}

void SDL_CloseGamepad(SDL_Gamepad *gamepad)
{
    (void)gamepad; /* NULL / invalid handles are ignored, as in stock SDL */
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
