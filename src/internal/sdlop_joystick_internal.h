/*
  SDLop - internal joystick object model shared by the public API and the
  Linux evdev backend.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#ifndef sdloP_joystick_internal_h_
#define sdloP_joystick_internal_h_

#include <SDL3/SDL_events.h> /* SDL_EventType (SendDeviceEvent) */
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#define SDLOP_JOYSTICK_MAGIC 0x53444C4A /* "SDLJ" */
#define SDLOP_GAMEPAD_MAGIC  0x53444C47 /* "SDLG" */

typedef struct SDLop_JoystickAxisInfo
{
    Uint16 code;    /* evdev ABS_* code */
    int min, max;   /* from EVIOCGABS */
} SDLop_JoystickAxisInfo;

typedef struct SDLop_JoystickHatInfo
{
    Uint16 xcode;   /* evdev ABS_HATnX */
    Uint16 ycode;   /* evdev ABS_HATnY */
} SDLop_JoystickHatInfo;

typedef struct SDLop_JoystickDeviceDesc
{
    const char *name;
    const char *path;
    Uint16 bus;
    Uint16 vendor;
    Uint16 product;
    Uint16 version;
    Uint16 firmware;
    SDL_JoystickType type;
    bool is_gamepad;
    SDL_GamepadType gamepad_type;
    const SDLop_JoystickAxisInfo *axes;
    int num_axes;
    const Sint16 *axes_initial;
    const Uint16 *buttons;   /* evdev BTN_* codes */
    int num_buttons;
    const bool *buttons_initial;
    const SDLop_JoystickHatInfo *hats;
    int num_hats;
    const Uint8 *hats_initial;
} SDLop_JoystickDeviceDesc;

/* The real SDL_Joystick object (opaque in the public headers). */
struct SDL_Joystick
{
    Uint32 magic;
    SDL_JoystickID instance_id;
    char *name;
    char *path;
    SDL_GUID guid;
    Uint16 vendor, product, version, firmware;
    Uint16 bus;
    SDL_JoystickType type;
    bool is_gamepad;
    SDL_GamepadType gamepad_type;
    int player_index;
    bool connected;
    bool virtual_device;
    int ref_count;       /* SDL_OpenJoystick() + one per open gamepad */
    int gamepad_refs;    /* open SDL_Gamepad handles using this joystick */

    int num_axes, num_buttons, num_hats, num_balls;
    Sint16 *axes;
    Sint16 *axes_initial;
    Uint16 *axis_codes;
    int *axis_min, *axis_max;
    bool *buttons;
    Uint16 *button_codes;
    Uint8 *hats;
    SDLop_JoystickHatInfo *hats_info;

    /* Generic evdev -> gamepad mapping (0xff = unmapped). */
    Uint8 gpad_axis[SDL_GAMEPAD_AXIS_COUNT];
    Uint8 gpad_button[SDL_GAMEPAD_BUTTON_COUNT];
    int gpad_hat;

    void *driverdata;                 /* evdev device (SDLOP_EvdevJoystick *) */
    struct SDL_Joystick *next;
};

struct SDL_Gamepad
{
    Uint32 magic;
    SDL_Joystick *joystick;
    int ref_count;
    struct SDL_Gamepad *next;
};

/* Maximum fds exposed to SDL_WaitEventTimeout(): evdev devices plus one
 * inotify descriptor used for hotplug notification. */
#define SDLOP_JOYSTICK_MAX_WAIT_FDS 65

/* Lifecycle (called from SDLop_core.c / SDL_PumpEvents()). */
bool SDLOP_InitJoysticks(void);
void SDLOP_QuitJoysticks(void);
bool SDLOP_JoysticksInitialized(void);
void SDLOP_JoystickPump(void);        /* called from SDL_PumpEvents() */
int SDLOP_JoystickGetEventFDs(int *fds, int max_fds); /* devices + hotplug fd */

/* Backend -> core. */
SDL_Joystick *SDLOP_JoystickAddDevice(const SDLop_JoystickDeviceDesc *desc, void *driverdata);
void SDLOP_JoystickRemoveDevice(SDL_Joystick *joystick);
void SDLOP_JoystickQuitAllDevices(void);
bool SDLOP_JoystickValid(SDL_Joystick *joystick);
bool SDLOP_GamepadValid(SDL_Gamepad *gamepad);
SDL_Joystick *SDLOP_JoystickFromInstanceID(SDL_JoystickID instance_id);
void SDLOP_JoystickSendAxis(SDL_Joystick *joystick, Uint8 axis, Sint16 value);
void SDLOP_JoystickSendButton(SDL_Joystick *joystick, Uint8 button, bool down);
void SDLOP_JoystickSendHat(SDL_Joystick *joystick, Uint8 hat, Uint8 value);
void SDLOP_JoystickSendDeviceEvent(SDL_Joystick *joystick, SDL_EventType type);

/* Find the SDL index of an evdev code (-1 when unsupported). */
int SDLOP_JoystickAxisIndex(SDL_Joystick *joystick, Uint16 code);
int SDLOP_JoystickButtonIndex(SDL_Joystick *joystick, Uint16 code);
int SDLOP_JoystickHatIndex(SDL_Joystick *joystick, Uint16 code, bool *is_x);

/* GUID helper used by the evdev backend. */
SDL_GUID SDLOP_CreateJoystickGUID(Uint16 bus, Uint16 vendor, Uint16 product,
                                  Uint16 version, const char *name);

#endif /* sdloP_joystick_internal_h_ */
