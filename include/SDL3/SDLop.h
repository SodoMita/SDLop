/*
  SDLop extensions - asyncinput-style low-latency raw input.

  SDLop runs a background worker thread that reads evdev devices directly
  (/dev/input/event*), like the asyncinput library. Raw events keep native
  platform codes (zero translation cost) and kernel timestamps, and can be
  consumed two ways:

    1. Callback: SDLop_RegisterRawEventCallback() - invoked directly on the
       worker thread, the lowest latency path.
    2. Polling: SDLop_PollRawEvents() - drains a lock-free SPSC ring from
       your thread.

  The standard SDL3 event queue (SDL_PollEvent etc.) is fed from the same
  ring, so both models can be mixed freely.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDLop_h_
#define SDLop_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

/**
 * A raw input event, exactly as read from the kernel (struct input_event
 * semantics). Codes are native platform codes - on Linux these are evdev
 * codes (KEY_*, BTN_*, REL_*). No translation, no allocation.
 */
typedef struct SDLop_RawEvent
{
    Uint32 device;       /**< SDLop device index of the source device */
    Uint16 type;         /**< native event type (SDLop_EV_*) */
    Uint16 code;         /**< native event code (SDLop_KEY_*, SDLop_BTN_*, SDLop_REL_*) */
    Sint32 value;        /**< native value (1/2/0 for keys, delta for REL) */
    Uint64 timestamp_ns; /**< kernel timestamp, CLOCK_MONOTONIC nanoseconds */
} SDLop_RawEvent;

/* ---- Zero-cost native constants (identical to Linux evdev codes) ---- */

/* Event types */
#define SDLop_EV_SYN        0x00
#define SDLop_EV_KEY        0x01
#define SDLop_EV_REL        0x02
#define SDLop_EV_ABS        0x03
#define SDLop_EV_MSC        0x04

/* SYN codes */
#define SDLop_SYN_REPORT    0

/* Common keys (evdev KEY_* values) */
#define SDLop_KEY_ESC       1
#define SDLop_KEY_1         2
#define SDLop_KEY_2         3
#define SDLop_KEY_3         4
#define SDLop_KEY_4         5
#define SDLop_KEY_5         6
#define SDLop_KEY_6         7
#define SDLop_KEY_7         8
#define SDLop_KEY_8         9
#define SDLop_KEY_9         10
#define SDLop_KEY_0         11
#define SDLop_KEY_MINUS     12
#define SDLop_KEY_EQUAL     13
#define SDLop_KEY_BACKSPACE 14
#define SDLop_KEY_TAB       15
#define SDLop_KEY_Q         16
#define SDLop_KEY_W         17
#define SDLop_KEY_E         18
#define SDLop_KEY_R         19
#define SDLop_KEY_T         20
#define SDLop_KEY_Y         21
#define SDLop_KEY_U         22
#define SDLop_KEY_I         23
#define SDLop_KEY_O         24
#define SDLop_KEY_P         25
#define SDLop_KEY_ENTER     28
#define SDLop_KEY_LEFTCTRL  29
#define SDLop_KEY_A         30
#define SDLop_KEY_S         31
#define SDLop_KEY_D         32
#define SDLop_KEY_F         33
#define SDLop_KEY_G         34
#define SDLop_KEY_H         35
#define SDLop_KEY_J         36
#define SDLop_KEY_K         37
#define SDLop_KEY_L         38
#define SDLop_KEY_Z         44
#define SDLop_KEY_X         45
#define SDLop_KEY_C         46
#define SDLop_KEY_V         47
#define SDLop_KEY_B         48
#define SDLop_KEY_N         49
#define SDLop_KEY_M         50
#define SDLop_KEY_COMMA     51
#define SDLop_KEY_DOT       52
#define SDLop_KEY_SLASH     53
#define SDLop_KEY_RIGHTSHIFT 54
#define SDLop_KEY_LEFTALT   56
#define SDLop_KEY_SPACE     57
#define SDLop_KEY_CAPSLOCK  58
#define SDLop_KEY_F1        59
#define SDLop_KEY_F2        60
#define SDLop_KEY_F3        61
#define SDLop_KEY_F4        62
#define SDLop_KEY_F5        63
#define SDLop_KEY_F6        64
#define SDLop_KEY_F7        65
#define SDLop_KEY_F8        66
#define SDLop_KEY_F9        67
#define SDLop_KEY_F10       68
#define SDLop_KEY_F11       87
#define SDLop_KEY_F12       88
#define SDLop_KEY_RIGHTCTRL 97
#define SDLop_KEY_RIGHTALT  100
#define SDLop_KEY_UP        103
#define SDLop_KEY_LEFT      105
#define SDLop_KEY_RIGHT     106
#define SDLop_KEY_DOWN      108
#define SDLop_KEY_LEFTMETA  125
#define SDLop_KEY_RIGHTMETA 126

/* Mouse buttons (evdev BTN_* values) */
#define SDLop_BTN_LEFT      0x110
#define SDLop_BTN_RIGHT     0x111
#define SDLop_BTN_MIDDLE    0x112
#define SDLop_BTN_SIDE      0x113
#define SDLop_BTN_EXTRA     0x114

/* Relative axes (evdev REL_* values) */
#define SDLop_REL_X         0x00
#define SDLop_REL_Y         0x01
#define SDLop_REL_HWHEEL    0x06
#define SDLop_REL_WHEEL     0x08
#define SDLop_REL_WHEEL_HI_RES 0x0b
#define SDLop_REL_HWHEEL_HI_RES 0x0c

/**
 * Raw event callback, invoked on the input worker thread.
 *
 * Keep it fast: it runs on the lowest-latency path. The event pointer is
 * only valid for the duration of the call.
 */
typedef void (*SDLop_RawEventCallback)(const SDLop_RawEvent *event, void *userdata);

/**
 * Register a raw event callback (asyncinput-style).
 *
 * \param cb the callback, invoked on the worker thread for each raw event.
 * \param userdata a pointer passed through to the callback.
 * \returns true on success or false if too many callbacks are registered.
 */
extern bool SDLop_RegisterRawEventCallback(SDLop_RawEventCallback cb, void *userdata);

/**
 * Unregister a previously registered raw event callback.
 */
extern void SDLop_UnregisterRawEventCallback(SDLop_RawEventCallback cb);

/**
 * Poll raw events (asyncinput-style ni_poll equivalent).
 *
 * Non-blocking. Drains up to max_events raw events from the lock-free
 * ring buffer. Raw events are also forwarded to the SDL event queue as
 * SDL3 events; polling raw events does not consume those.
 *
 * \returns the number of events written into events, or -1 on error.
 */
extern int SDLop_PollRawEvents(SDLop_RawEvent *events, int max_events);

/**
 * Query whether the low-latency evdev worker thread is running.
 *
 * When false (e.g. /dev/input is not readable), SDLop transparently uses
 * window-system input instead.
 */
extern bool SDLop_RawInputAvailable(void);

/**
 * Get the number of raw events seen since init (worker thread stats).
 */
extern Uint64 SDLop_GetRawEventCount(void);

/**
 * Set the background color a window is presented with.
 *
 * SDLop is windowing + input only; this lets you tint the surface without
 * a rendering API. Takes effect on the next present.
 */
extern void SDLop_SetWindowClearColor(SDL_Window *window, Uint8 r, Uint8 g, Uint8 b);

#endif /* SDLop_h_ */
