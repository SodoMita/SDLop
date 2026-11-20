/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
 *
 * SDLop: generated from SDL 3.4.20 by tools/gen_headers.py - declarations of
 * functions SDLop does not implement are removed; everything else is
 * upstream's text. Do not edit by hand.
 */

/**
 * # CategoryJoystick
 *
 * SDL joystick support.
 *
 * This is the lower-level joystick handling. If you want the simpler option,
 * where what each button does is well-defined, you should use the gamepad API
 * instead.
 *
 * The term "instance_id" is the current instantiation of a joystick device in
 * the system. If the joystick is removed and then re-inserted then it will
 * get a new instance_id. instance_id's are monotonically increasing
 * identifiers of a joystick plugged in.
 *
 * The term "player_index" is the number assigned to a player on a specific
 * controller. For XInput controllers this returns the XInput user index. Many
 * joysticks will not be able to supply this information.
 *
 * SDL_GUID is used as a stable 128-bit identifier for a joystick device that
 * does not change over time. It identifies class of the device (a X360 wired
 * controller for example). This identifier is platform dependent.
 *
 * In order to use these functions, SDL_Init() must have been called with the
 * SDL_INIT_JOYSTICK flag. This causes SDL to scan the system for joysticks,
 * and load appropriate drivers.
 *
 * If you would like to receive joystick updates while the application is in
 * the background, you should set the following hint before calling
 * SDL_Init(): SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS
 *
 * SDL can provide virtual joysticks as well: the app defines an imaginary
 * controller with SDL_AttachVirtualJoystick(), and then can provide inputs
 * for it via SDL_SetJoystickVirtualAxis(), SDL_SetJoystickVirtualButton(),
 * etc. As this data is supplied, it will look like a normal joystick to SDL,
 * just not backed by a hardware driver. This has been used to make unusual
 * devices, like VR headset controllers, look like normal joysticks, or
 * provide recording/playback of game inputs, etc.
 */

#ifndef SDL_joystick_h_
#define SDL_joystick_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_guid.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_power.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_sensor.h>

#include <SDL3/SDL_begin_code.h>
/* Set up for C function definitions, even when using C++ */
#ifdef __cplusplus
extern "C" {
#endif

#ifdef SDL_THREAD_SAFETY_ANALYSIS
/*
 * This is not an exported symbol from SDL, this is only in the headers to
 * help Clang's thread safety analysis tools to function. Do not attempt
 * to access this symbol from your app, it will not work!
 */
extern SDL_Mutex *SDL_joystick_lock;
#endif

/**
 * The joystick structure used to identify an SDL joystick.
 *
 * This is opaque data.
 *
 * \since This struct is available since SDL 3.2.0.
 */
typedef struct SDL_Joystick SDL_Joystick;

/**
 * This is a unique ID for a joystick for the time it is connected to the
 * system, and is never reused for the lifetime of the application.
 *
 * If the joystick is disconnected and reconnected, it will get a new ID.
 *
 * The value 0 is an invalid ID.
 *
 * \since This datatype is available since SDL 3.2.0.
 */
typedef Uint32 SDL_JoystickID;

/**
 * An enum of some common joystick types.
 *
 * In some cases, SDL can identify a low-level joystick as being a certain
 * type of device, and will report it through SDL_GetJoystickType (or
 * SDL_GetJoystickTypeForID).
 *
 * This is by no means a complete list of everything that can be plugged into
 * a computer.
 *
 * You may refer to
 * [XInput Controller Types](https://learn.microsoft.com/en-us/windows/win32/xinput/xinput-and-controller-subtypes)
 * table for a general understanding of each joystick type.
 *
 * \since This enum is available since SDL 3.2.0.
 */
typedef enum SDL_JoystickType
{
    SDL_JOYSTICK_TYPE_UNKNOWN,
    SDL_JOYSTICK_TYPE_GAMEPAD,
    SDL_JOYSTICK_TYPE_WHEEL,
    SDL_JOYSTICK_TYPE_ARCADE_STICK,
    SDL_JOYSTICK_TYPE_FLIGHT_STICK,
    SDL_JOYSTICK_TYPE_DANCE_PAD,
    SDL_JOYSTICK_TYPE_GUITAR,
    SDL_JOYSTICK_TYPE_DRUM_KIT,
    SDL_JOYSTICK_TYPE_ARCADE_PAD,
    SDL_JOYSTICK_TYPE_THROTTLE,
    SDL_JOYSTICK_TYPE_COUNT
} SDL_JoystickType;

/**
 * Possible connection states for a joystick device.
 *
 * This is used by SDL_GetJoystickConnectionState to report how a device is
 * connected to the system.
 *
 * \since This enum is available since SDL 3.2.0.
 */
typedef enum SDL_JoystickConnectionState
{
    SDL_JOYSTICK_CONNECTION_INVALID = -1,
    SDL_JOYSTICK_CONNECTION_UNKNOWN,
    SDL_JOYSTICK_CONNECTION_WIRED,
    SDL_JOYSTICK_CONNECTION_WIRELESS
} SDL_JoystickConnectionState;

/**
 * The largest value an SDL_Joystick's axis can report.
 *
 * \since This macro is available since SDL 3.2.0.
 *
 * \sa SDL_JOYSTICK_AXIS_MIN
 */
#define SDL_JOYSTICK_AXIS_MAX   32767

/**
 * The smallest value an SDL_Joystick's axis can report.
 *
 * This is a negative number!
 *
 * \since This macro is available since SDL 3.2.0.
 *
 * \sa SDL_JOYSTICK_AXIS_MAX
 */
#define SDL_JOYSTICK_AXIS_MIN   -32768


/* Function prototypes */
















/**
 * The structure that describes a virtual joystick touchpad.
 *
 * \since This struct is available since SDL 3.2.0.
 *
 * \sa SDL_VirtualJoystickDesc
 */
typedef struct SDL_VirtualJoystickTouchpadDesc
{
    Uint16 nfingers;    /**< the number of simultaneous fingers on this touchpad */
    Uint16 padding[3];
} SDL_VirtualJoystickTouchpadDesc;

/**
 * The structure that describes a virtual joystick sensor.
 *
 * \since This struct is available since SDL 3.2.0.
 *
 * \sa SDL_VirtualJoystickDesc
 */
typedef struct SDL_VirtualJoystickSensorDesc
{
    SDL_SensorType type;    /**< the type of this sensor */
    float rate;             /**< the update frequency of this sensor, may be 0.0f */
} SDL_VirtualJoystickSensorDesc;

/**
 * The structure that describes a virtual joystick.
 *
 * This structure should be initialized using SDL_INIT_INTERFACE(). All
 * elements of this structure are optional.
 *
 * \since This struct is available since SDL 3.2.0.
 *
 * \sa SDL_AttachVirtualJoystick
 * \sa SDL_INIT_INTERFACE
 * \sa SDL_VirtualJoystickSensorDesc
 * \sa SDL_VirtualJoystickTouchpadDesc
 */
typedef struct SDL_VirtualJoystickDesc
{
    Uint32 version;     /**< the version of this interface */
    Uint16 type;        /**< `SDL_JoystickType` */
    Uint16 padding;     /**< unused */
    Uint16 vendor_id;   /**< the USB vendor ID of this joystick */
    Uint16 product_id;  /**< the USB product ID of this joystick */
    Uint16 naxes;       /**< the number of axes on this joystick */
    Uint16 nbuttons;    /**< the number of buttons on this joystick */
    Uint16 nballs;      /**< the number of balls on this joystick */
    Uint16 nhats;       /**< the number of hats on this joystick */
    Uint16 ntouchpads;  /**< the number of touchpads on this joystick, requires `touchpads` to point at valid descriptions */
    Uint16 nsensors;    /**< the number of sensors on this joystick, requires `sensors` to point at valid descriptions */
    Uint16 padding2[2]; /**< unused */
    Uint32 button_mask; /**< A mask of which buttons are valid for this controller
                             e.g. (1 << SDL_GAMEPAD_BUTTON_SOUTH) */
    Uint32 axis_mask;   /**< A mask of which axes are valid for this controller
                             e.g. (1 << SDL_GAMEPAD_AXIS_LEFTX) */
    const char *name;   /**< the name of the joystick */
    const SDL_VirtualJoystickTouchpadDesc *touchpads;   /**< A pointer to an array of touchpad descriptions, required if `ntouchpads` is > 0 */
    const SDL_VirtualJoystickSensorDesc *sensors;       /**< A pointer to an array of sensor descriptions, required if `nsensors` is > 0 */

    void *userdata;     /**< User data pointer passed to callbacks */
    void (SDLCALL *Update)(void *userdata); /**< Called when the joystick state should be updated */
    void (SDLCALL *SetPlayerIndex)(void *userdata, int player_index); /**< Called when the player index is set */
    bool (SDLCALL *Rumble)(void *userdata, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble); /**< Implements SDL_RumbleJoystick() */
    bool (SDLCALL *RumbleTriggers)(void *userdata, Uint16 left_rumble, Uint16 right_rumble); /**< Implements SDL_RumbleJoystickTriggers() */
    bool (SDLCALL *SetLED)(void *userdata, Uint8 red, Uint8 green, Uint8 blue); /**< Implements SDL_SetJoystickLED() */
    bool (SDLCALL *SendEffect)(void *userdata, const void *data, int size); /**< Implements SDL_SendJoystickEffect() */
    bool (SDLCALL *SetSensorsEnabled)(void *userdata, bool enabled); /**< Implements SDL_SetGamepadSensorEnabled() */
    void (SDLCALL *Cleanup)(void *userdata); /**< Cleans up the userdata when the joystick is detached */
} SDL_VirtualJoystickDesc;

/* Check the size of SDL_VirtualJoystickDesc
 *
 * If this assert fails, either the compiler is padding to an unexpected size,
 * or the interface has been updated and this should be updated to match and
 * the code using this interface should be updated to handle the old version.
 */
SDL_COMPILE_TIME_ASSERT(SDL_VirtualJoystickDesc_SIZE,
    (sizeof(void *) == 4 && sizeof(SDL_VirtualJoystickDesc) == 84) ||
    (sizeof(void *) == 8 && sizeof(SDL_VirtualJoystickDesc) == 136));











#define SDL_PROP_JOYSTICK_CAP_MONO_LED_BOOLEAN          "SDL.joystick.cap.mono_led"
#define SDL_PROP_JOYSTICK_CAP_RGB_LED_BOOLEAN           "SDL.joystick.cap.rgb_led"
#define SDL_PROP_JOYSTICK_CAP_PLAYER_LED_BOOLEAN        "SDL.joystick.cap.player_led"
#define SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN            "SDL.joystick.cap.rumble"
#define SDL_PROP_JOYSTICK_CAP_TRIGGER_RUMBLE_BOOLEAN    "SDL.joystick.cap.trigger_rumble"


























#define SDL_HAT_CENTERED    0x00u
#define SDL_HAT_UP          0x01u
#define SDL_HAT_RIGHT       0x02u
#define SDL_HAT_DOWN        0x04u
#define SDL_HAT_LEFT        0x08u
#define SDL_HAT_RIGHTUP     (SDL_HAT_RIGHT|SDL_HAT_UP)
#define SDL_HAT_RIGHTDOWN   (SDL_HAT_RIGHT|SDL_HAT_DOWN)
#define SDL_HAT_LEFTUP      (SDL_HAT_LEFT|SDL_HAT_UP)
#define SDL_HAT_LEFTDOWN    (SDL_HAT_LEFT|SDL_HAT_DOWN)









/* Ends C function definitions when using C++ */
#ifdef __cplusplus
}
#endif
#include <SDL3/SDL_close_code.h>

#endif /* SDL_joystick_h_ */
