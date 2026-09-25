/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Event queue management (subset of <SDL3/SDL_events.h>; struct layouts,
  event type values and signatures are identical to SDL3).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_events_h_
#define SDL_events_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>

/**
 * The types of events that can be delivered (values identical to SDL3).
 */
typedef enum SDL_EventType
{
    SDL_EVENT_FIRST     = 0,
    SDL_EVENT_QUIT           = 0x100,
    SDL_EVENT_TERMINATING,
    SDL_EVENT_LOW_MEMORY,
    SDL_EVENT_WILL_ENTER_BACKGROUND,
    SDL_EVENT_DID_ENTER_BACKGROUND,
    SDL_EVENT_WILL_ENTER_FOREGROUND,
    SDL_EVENT_DID_ENTER_FOREGROUND,
    SDL_EVENT_LOCALE_CHANGED,
    SDL_EVENT_SYSTEM_THEME_CHANGED,

    /* Display events */
    /* 0x150 was SDL_DISPLAYEVENT, reserve the number for sdl2-compat */
    SDL_EVENT_DISPLAY_ORIENTATION = 0x151,   /**< Display orientation has changed to data1 */
    SDL_EVENT_DISPLAY_ADDED,                 /**< Display has been added to the system */
    SDL_EVENT_DISPLAY_REMOVED,               /**< Display has been removed from the system */
    SDL_EVENT_DISPLAY_MOVED,                 /**< Display has changed position */
    SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED,  /**< Display has changed desktop mode */
    SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED,  /**< Display has changed current mode */
    SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED, /**< Display has changed content scale */
    SDL_EVENT_DISPLAY_FIRST = SDL_EVENT_DISPLAY_ORIENTATION,
    SDL_EVENT_DISPLAY_LAST = SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED,

    /* Window events */
    SDL_EVENT_WINDOW_SHOWN = 0x202,
    SDL_EVENT_WINDOW_HIDDEN,
    SDL_EVENT_WINDOW_EXPOSED,
    SDL_EVENT_WINDOW_MOVED,
    SDL_EVENT_WINDOW_RESIZED,
    SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED,
    SDL_EVENT_WINDOW_METAL_VIEW_RESIZED,
    SDL_EVENT_WINDOW_MINIMIZED,
    SDL_EVENT_WINDOW_MAXIMIZED,
    SDL_EVENT_WINDOW_RESTORED,
    SDL_EVENT_WINDOW_MOUSE_ENTER,
    SDL_EVENT_WINDOW_MOUSE_LEAVE,
    SDL_EVENT_WINDOW_FOCUS_GAINED,
    SDL_EVENT_WINDOW_FOCUS_LOST,
    SDL_EVENT_WINDOW_CLOSE_REQUESTED,
    SDL_EVENT_WINDOW_HIT_TEST,
    SDL_EVENT_WINDOW_ICCPROF_CHANGED,
    SDL_EVENT_WINDOW_DISPLAY_CHANGED,
    SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED,
    SDL_EVENT_WINDOW_SAFE_AREA_CHANGED,
    SDL_EVENT_WINDOW_OCCLUDED,
    SDL_EVENT_WINDOW_ENTER_FULLSCREEN,
    SDL_EVENT_WINDOW_LEAVE_FULLSCREEN,
    SDL_EVENT_WINDOW_DESTROYED,
    SDL_EVENT_WINDOW_HDR_STATE_CHANGED,
    SDL_EVENT_WINDOW_FIRST = SDL_EVENT_WINDOW_SHOWN,
    SDL_EVENT_WINDOW_LAST = SDL_EVENT_WINDOW_HDR_STATE_CHANGED,

    /* Keyboard events */
    SDL_EVENT_KEY_DOWN        = 0x300,
    SDL_EVENT_KEY_UP,
    SDL_EVENT_TEXT_EDITING,
    SDL_EVENT_TEXT_INPUT,
    SDL_EVENT_TEXT_EDITING_CANDIDATES,
    SDL_EVENT_KEYMAP_CHANGED,
    SDL_EVENT_KEYBOARD_ADDED,
    SDL_EVENT_KEYBOARD_REMOVED,

    /* Mouse events */
    SDL_EVENT_MOUSE_MOTION    = 0x400,
    SDL_EVENT_MOUSE_BUTTON_DOWN,
    SDL_EVENT_MOUSE_BUTTON_UP,
    SDL_EVENT_MOUSE_WHEEL,
    SDL_EVENT_MOUSE_ADDED,
    SDL_EVENT_MOUSE_REMOVED,

    /* Touch events */
    SDL_EVENT_FINGER_DOWN      = 0x700,
    SDL_EVENT_FINGER_UP,
    SDL_EVENT_FINGER_MOTION,
    SDL_EVENT_FINGER_CANCELED,

    /* User events */
    SDL_EVENT_USER    = 0x8000,

    /* Clipboard events */
    SDL_EVENT_CLIPBOARD_UPDATE = 0x900,

    SDL_EVENT_LAST    = 0xFFFF
} SDL_EventType;

/** Fields shared by every event. */
typedef struct SDL_CommonEvent
{
    Uint32 type;
    Uint32 reserved;
    Uint64 timestamp;   /**< In nanoseconds, populated using SDL_GetTicksNS() */
} SDL_CommonEvent;

/** Window state change event data (event.window.*). */
typedef struct SDL_DisplayEvent
{
    SDL_EventType type; /**< SDL_DISPLAYEVENT_* */
    Uint32 reserved;
    Uint64 timestamp;   /**< In nanoseconds, populated using SDL_GetTicksNS() */
    SDL_DisplayID displayID;/**< The associated display */
    Sint32 data1;       /**< event dependent data */
    Sint32 data2;       /**< event dependent data */
} SDL_DisplayEvent;

typedef struct SDL_WindowEvent
{
    SDL_EventType type; /**< SDL_EVENT_WINDOW_* */
    Uint32 reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;
    Sint32 data1;       /**< event dependent data */
    Sint32 data2;       /**< event dependent data */
} SDL_WindowEvent;

/** Keyboard button event data (event.key.*). */
typedef struct SDL_KeyboardDeviceEvent
{
    SDL_EventType type; /**< SDL_EVENT_KEYBOARD_ADDED or SDL_EVENT_KEYBOARD_REMOVED */
    Uint32 reserved;
    Uint64 timestamp;   /**< In nanoseconds, populated using SDL_GetTicksNS() */
    SDL_KeyboardID which;   /**< The keyboard instance id */
} SDL_KeyboardDeviceEvent;

typedef struct SDL_KeyboardEvent
{
    SDL_EventType type;     /**< SDL_EVENT_KEY_DOWN or SDL_EVENT_KEY_UP */
    Uint32 reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;  /**< The window with keyboard focus, if any */
    SDL_KeyboardID which;   /**< The keyboard instance id, or 0 */
    SDL_Scancode scancode;  /**< SDL physical key code */
    SDL_Keycode key;        /**< SDL virtual key code */
    SDL_Keymod mod;         /**< current key modifiers */
    Uint16 raw;             /**< The platform dependent scancode (evdev on Linux) */
    bool down;              /**< true if the key is pressed */
    bool repeat;            /**< true if this is a key repeat */
} SDL_KeyboardEvent;

/** Keyboard text input event data (event.text.*). */
typedef struct SDL_TextEditingEvent
{
    SDL_EventType type;         /**< SDL_EVENT_TEXT_EDITING */
    Uint32 reserved;
    Uint64 timestamp;           /**< In nanoseconds, populated using SDL_GetTicksNS() */
    SDL_WindowID windowID;      /**< The window with keyboard focus, if any */
    const char *text;           /**< The editing text */
    Sint32 start;               /**< The start cursor of selected editing text, or -1 if not set */
    Sint32 length;              /**< The length of selected editing text, or -1 if not set */
} SDL_TextEditingEvent;

typedef struct SDL_TextEditingCandidatesEvent
{
    SDL_EventType type;         /**< SDL_EVENT_TEXT_EDITING_CANDIDATES */
    Uint32 reserved;
    Uint64 timestamp;           /**< In nanoseconds, populated using SDL_GetTicksNS() */
    SDL_WindowID windowID;      /**< The window with keyboard focus, if any */
    const char * const *candidates;    /**< The list of candidates, or NULL if there are no candidates available */
    Sint32 num_candidates;      /**< The number of strings in `candidates` */
    Sint32 selected_candidate;  /**< The index of the selected candidate, or -1 if no candidate is selected */
    bool horizontal;          /**< true if the list is horizontal, false if it's vertical */
    Uint8 padding1;
    Uint8 padding2;
    Uint8 padding3;
} SDL_TextEditingCandidatesEvent;

typedef struct SDL_TextInputEvent
{
    SDL_EventType type; /**< SDL_EVENT_TEXT_INPUT */
    Uint32 reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;
    const char *text;   /**< The input text, UTF-8 encoded (transient!) */
} SDL_TextInputEvent;

/** Mouse motion event data (event.motion.*). */
typedef struct SDL_MouseDeviceEvent
{
    SDL_EventType type; /**< SDL_EVENT_MOUSE_ADDED or SDL_EVENT_MOUSE_REMOVED */
    Uint32 reserved;
    Uint64 timestamp;   /**< In nanoseconds, populated using SDL_GetTicksNS() */
    SDL_MouseID which;  /**< The mouse instance id */
} SDL_MouseDeviceEvent;

typedef struct SDL_MouseMotionEvent
{
    SDL_EventType type; /**< SDL_EVENT_MOUSE_MOTION */
    Uint32 reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;
    SDL_MouseID which;
    SDL_MouseButtonFlags state;   /**< The current button state */
    float x;        /**< X coordinate, relative to window */
    float y;        /**< Y coordinate, relative to window */
    float xrel;     /**< The relative motion in the X direction */
    float yrel;     /**< The relative motion in the Y direction */
} SDL_MouseMotionEvent;

/** Mouse button event data (event.button.*). */
typedef struct SDL_MouseButtonEvent
{
    SDL_EventType type; /**< SDL_EVENT_MOUSE_BUTTON_DOWN or SDL_EVENT_MOUSE_BUTTON_UP */
    Uint32 reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;
    SDL_MouseID which;
    Uint8 button;   /**< The mouse button index (SDL_BUTTON_LEFT, ...) */
    bool down;      /**< true if the button is pressed */
    Uint8 clicks;   /**< 1 for single-click, 2 for double-click, etc. */
    Uint8 padding;
    float x;        /**< X coordinate, relative to window */
    float y;        /**< Y coordinate, relative to window */
} SDL_MouseButtonEvent;

/** Mouse wheel event data (event.wheel.*). */
typedef struct SDL_MouseWheelEvent
{
    SDL_EventType type; /**< SDL_EVENT_MOUSE_WHEEL */
    Uint32 reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;
    SDL_MouseID which;
    float x;    /**< The amount scrolled horizontally (positive = right) */
    float y;    /**< The amount scrolled vertically (positive = away) */
    SDL_MouseWheelDirection direction;
    float mouse_x;  /**< X coordinate, relative to window */
    float mouse_y;  /**< Y coordinate, relative to window */
} SDL_MouseWheelEvent;

/** The "quit requested" event (event.quit.*). */
typedef struct SDL_QuitEvent
{
    SDL_EventType type; /**< SDL_EVENT_QUIT */
    Uint32 reserved;
    Uint64 timestamp;
} SDL_QuitEvent;

/** A user-defined event type (event.user.*). */
typedef struct SDL_UserEvent
{
    Uint32 type;        /**< SDL_EVENT_USER through SDL_EVENT_LAST-1 */
    Uint32 reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;
    Sint32 code;        /**< User defined event code */
    void *data1;        /**< User defined data pointer */
    void *data2;        /**< User defined data pointer */
} SDL_UserEvent;

/**
 * General event structure. Layout and size identical to SDL3.
 */
/**
 * Touch finger event structure (event.tfinger) - field-compatible with SDL3.
 */
typedef struct SDL_TouchFingerEvent
{
    SDL_EventType type; /**< SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_UP, SDL_EVENT_FINGER_MOTION, or SDL_EVENT_FINGER_CANCELED */
    Uint32 reserved;
    Uint64 timestamp;   /**< In nanoseconds, populated using SDL_GetTicksNS() */
    SDL_TouchID touchID; /**< The touch device id */
    SDL_FingerID fingerID;
    float x;            /**< Normalized in the range 0...1 */
    float y;            /**< Normalized in the range 0...1 */
    float dx;           /**< Normalized in the range -1...1 */
    float dy;           /**< Normalized in the range -1...1 */
    float pressure;     /**< Normalized in the range 0...1 */
    SDL_WindowID windowID; /**< The window underneath the finger, if any */
} SDL_TouchFingerEvent;

typedef union SDL_Event
{
    Uint32 type;                    /**< Event type, shared with all events */
    SDL_CommonEvent common;         /**< Common event data */
    SDL_DisplayEvent display;       /**< Display state change event data */
    SDL_WindowEvent window;         /**< Window event data */
    SDL_KeyboardDeviceEvent kdevice; /**< Keyboard device change event data */
    SDL_KeyboardEvent key;          /**< Keyboard event data */
    SDL_TextEditingEvent edit;      /**< Text editing event data */
    SDL_TextEditingCandidatesEvent edit_candidates; /**< Text editing candidates event data */
    SDL_TextInputEvent text;        /**< Text input event data */
    SDL_MouseDeviceEvent mdevice;   /**< Mouse device change event data */
    SDL_MouseMotionEvent motion;    /**< Mouse motion event data */
    SDL_MouseButtonEvent button;    /**< Mouse button event data */
    SDL_MouseWheelEvent wheel;      /**< Mouse wheel event data */
    SDL_QuitEvent quit;             /**< Quit request event data */
    SDL_UserEvent user;             /**< Custom event data */
    SDL_TouchFingerEvent tfinger;   /**< Touch finger event data */

    /* Pad to the same size as SDL3 (128 bytes) */
    Uint8 padding[128];
} SDL_Event;

/* Make sure the size stays identical to SDL3 */
typedef char SDL_static_assert_event_size[(sizeof(SDL_Event) == 128) ? 1 : -1];

/**
 * Pump the event loop, gathering events from the input devices.
 *
 * Drains the low-latency raw input ring and the window system connection
 * (non-blocking), translating everything into SDL events.
 */
extern void SDL_PumpEvents(void);

/**
 * Check for the existence of a certain event type in the event queue.
 */
extern bool SDL_HasEvent(Uint32 type);

/**
 * Check for the existence of certain event types in the event queue.
 */
extern bool SDL_HasEvents(Uint32 minType, Uint32 maxType);

/**
 * Clear events of a specific type from the event queue.
 */
extern void SDL_FlushEvent(Uint32 type);

/**
 * Clear events of a range of types from the event queue.
 */
extern void SDL_FlushEvents(Uint32 minType, Uint32 maxType);

/**
 * Poll for currently pending events.
 *
 * \param event the SDL_Event structure to be filled with the next event,
 *              or NULL.
 * \returns true if there is a pending event or false if none are available.
 */
extern bool SDL_PollEvent(SDL_Event *event);

/**
 * Wait indefinitely for the next available event.
 */
extern bool SDL_WaitEvent(SDL_Event *event);

/**
 * Wait until the specified timeout for the next available event.
 *
 * \param timeoutMS the maximum number of milliseconds to wait (-1 = wait
 *                  indefinitely).
 * \returns true on success or false if there was an error while waiting.
 */
extern bool SDL_WaitEventTimeout(SDL_Event *event, Sint32 timeoutMS);

/**
 * Add an event to the event queue.
 */
extern bool SDL_PushEvent(SDL_Event *event);

/**
 * Allocate a set of user-defined events.
 *
 * \returns the beginning event number, or 0 if there are not enough
 *          user-defined events left.
 */
extern Uint32 SDL_RegisterEvents(int numevents);

/**
 * Pump the event loop and return whether an SDL_EVENT_QUIT event is queued.
 */

/**
 * Get the window associated with an event.
 */
extern SDL_Window *SDL_GetWindowFromEvent(const SDL_Event *event);

#endif /* SDL_events_h_ */
