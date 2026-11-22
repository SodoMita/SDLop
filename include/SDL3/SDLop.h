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

  On the console drivers (kmsdrm, fbdev) the standard SDL3 event queue
  (SDL_PollEvent etc.) is fed from the same ring, so both models can be
  mixed freely. On X11/Wayland the worker is off unless SDLOP_RAW_INPUT=1
  (see "Options" below); SDLop never mixes evdev and window-system input:
  either evdev supplies all SDL keyboard and mouse events or none.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDLop_h_
#define SDLop_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

/**
 * \name Options
 *
 * Every SDLOP_* environment variable is also an SDL hint of the same name,
 * so code can choose it with SDL_SetHint() / SDL_SetHintWithPriority():
 *
 * \code
 * SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1");          // default, env may override
 * SDL_SetHintWithPriority(SDLOP_HINT_RAW_INPUT, "0",
 *                         SDL_HINT_OVERRIDE);      // wins over the environment
 * SDL_Init(SDL_INIT_VIDEO);
 * \endcode
 *
 * Precedence: an SDL_HINT_OVERRIDE hint, then the environment, then a
 * hint set with a lower priority, then the built-in default. Options are
 * read when the subsystem or driver that uses them initialises (mostly
 * SDL_Init), so set them before that. SDL_ResetHint() returns an option
 * to the environment/default; SDL_Quit() drops all hints, as in SDL3. Boolean options treat unset or empty as the
 * default, "0"/"false"/"off"/"no" as false and anything else as true.
 *
 * Library options (SDLOP_LIB_*) name the shared object to dlopen; for the
 * optional X11 extension libraries "none" treats that library as missing.
 * @{
 */
/* input */
#define SDLOP_HINT_RAW_INPUT             "SDLOP_RAW_INPUT"         /**< bool: evdev input. Default: on for kmsdrm/fbdev, off for X11/Wayland. On X11/Wayland "1" makes evdev the only keyboard and mouse source (needs a readable keyboard and pointer, otherwise window-system input is used for both). */
#define SDLOP_HINT_DISABLE_RAW_INPUT     "SDLOP_DISABLE_RAW_INPUT" /**< bool: never start evdev input (legacy; same as SDLOP_RAW_INPUT=0, and wins over it) */
#define SDLOP_HINT_LOG_PRIORITY          "SDLOP_LOG_PRIORITY"      /**< default log priority (verbose..critical) when SDL_HINT_LOGGING is unset */
#define SDLOP_HINT_POWER_SUPPLY_PATH     "SDLOP_POWER_SUPPLY_PATH" /**< sysfs power_supply directory (default /sys/class/power_supply) */
#define SDLOP_HINT_TTYNO                 "TTYNO"                   /**< console drivers: VT number to own (implies a VT switch) */
/* X11 */
#define SDLOP_HINT_X11_EGL_PLATFORM      "SDLOP_X11_EGL_PLATFORM"  /**< "x11" to skip the EGL xcb platform */
#define SDLOP_HINT_X11_NO_RENDER         "SDLOP_X11_NO_RENDER"     /**< bool: do not use XRender (cursors) */
#define SDLOP_HINT_X11_NO_SHM            "SDLOP_X11_NO_SHM"        /**< bool: do not use MIT-SHM */
#define SDLOP_HINT_X11_NO_XIM           "SDLOP_X11_NO_XIM"        /**< bool: no input method (XIM via libX11) for text input */
#define SDLOP_HINT_X11_NO_TOUCH          "SDLOP_X11_NO_TOUCH"      /**< bool: no XInput2 touch */
#define SDLOP_HINT_X11_NO_GESTURES       "SDLOP_X11_NO_GESTURES"   /**< bool: do not select XInput 2.4 touchpad pinch events */
#define SDLOP_HINT_NO_DBUS               "SDLOP_NO_DBUS"           /**< bool: never connect to the D-Bus session bus (system theme, window progress, portal file dialogs) */
#define SDLOP_HINT_X11_NO_PEN            "SDLOP_X11_NO_PEN"        /**< bool: no XInput2 pens (tablets stay plain mice); read at video init */
#define SDLOP_HINT_X11_NO_XSETTINGS     "SDLOP_X11_NO_XSETTINGS"  /**< bool: do not read XSETTINGS (content scale from Gdk/WindowScalingFactor, Xft/DPI); read at video init */
#define SDLOP_HINT_X11_RELATIVE_WARP     "SDLOP_X11_RELATIVE_WARP" /**< bool: relative mode by warping instead of XInput2 raw motion */
/* GPU offload (headless GPU rendering presented through the window framebuffer) */
#define SDLOP_HINT_OFFLOAD               "SDLOP_OFFLOAD"           /**< default offload policy for GL and Vulkan: "auto" (default), "0"/"off"/"false"/"no", "1"/"on"/"true"/"yes"/"force" */
#define SDLOP_HINT_GL_OFFLOAD            "SDLOP_GL_OFFLOAD"        /**< OpenGL offload policy (same values); wins over SDLOP_OFFLOAD. Read when a GL context/window is set up */
#define SDLOP_HINT_VULKAN_OFFLOAD        "SDLOP_VULKAN_OFFLOAD"    /**< Vulkan offload policy (same values); wins over SDLOP_VK_OFFLOAD and SDLOP_OFFLOAD. Read when Vulkan is loaded / queried */
#define SDLOP_HINT_VK_OFFLOAD            "SDLOP_VK_OFFLOAD"        /**< alias of SDLOP_VULKAN_OFFLOAD (lower precedence) */
#define SDLOP_HINT_GL_OFFLOAD_PRIME      "SDLOP_GL_OFFLOAD_PRIME"  /**< bool: kmsdrm: scan out offloaded GL frames via PRIME import (default on; "0" copies through dumb buffers) */
/* Wayland */
#define SDLOP_HINT_WAYLAND_NO_VIEWPORTER "SDLOP_WAYLAND_NO_VIEWPORTER" /**< bool: do not use wp_viewporter (the clear colour is then a filled full-size buffer); read at video init */
#define SDLOP_HINT_WAYLAND_NO_CURSOR_SHAPE "SDLOP_WAYLAND_NO_CURSOR_SHAPE" /**< bool: draw system cursors from the theme instead of wp_cursor_shape_v1; read at video init */
#define SDLOP_HINT_WAYLAND_NO_TEXT_INPUT "SDLOP_WAYLAND_NO_TEXT_INPUT" /**< bool: do not use zwp_text_input_v3 (IME); read at video init */
#define SDLOP_HINT_WAYLAND_NO_POINTER_GESTURES "SDLOP_WAYLAND_NO_POINTER_GESTURES" /**< bool: do not bind zwp_pointer_gestures_v1 (no SDL_EVENT_PINCH_*); read at video init */
#define SDLOP_HINT_WAYLAND_NO_TABLET "SDLOP_WAYLAND_NO_TABLET" /**< bool: do not use zwp_tablet_manager_v2 (pens); read at video init */
#define SDLOP_HINT_WAYLAND_NO_FRACTIONAL_SCALE "SDLOP_WAYLAND_NO_FRACTIONAL_SCALE" /**< bool: do not use wp_fractional_scale_v1 (integer preferred buffer / output scales only); read at video init */
/* kmsdrm */
#define SDLOP_HINT_KMSDRM_DEVICE         "SDLOP_KMSDRM_DEVICE"     /**< DRM device: card number or /dev/dri/cardN path (default: probe) */
#define SDLOP_HINT_KMSDRM_CONNECTOR      "SDLOP_KMSDRM_CONNECTOR"  /**< use only outputs whose name contains this (e.g. HDMI) */
#define SDLOP_HINT_KMSDRM_ATOMIC         "SDLOP_KMSDRM_ATOMIC"     /**< bool: try atomic modesetting (default on) */
#define SDLOP_HINT_KMSDRM_ATOMIC_CURSOR  "SDLOP_KMSDRM_ATOMIC_CURSOR" /**< bool: cursor plane through atomic commits (default off) */
#define SDLOP_HINT_KMSDRM_DOUBLE_BUFFER  "SDLOP_KMSDRM_DOUBLE_BUFFER" /**< bool: GL path waits for the flip right after queueing it (SDL_HINT_VIDEO_DOUBLE_BUFFER) */
#define SDLOP_HINT_KMSDRM_FLIP_TIMEOUT_MS "SDLOP_KMSDRM_FLIP_TIMEOUT_MS" /**< page-flip wait limit in ms (default 1500) */
#define SDLOP_HINT_KMSDRM_HOTPLUG        "SDLOP_KMSDRM_HOTPLUG"    /**< bool: watch connector hotplug (default on) */
#define SDLOP_HINT_KMSDRM_KEEP_MASTER    "SDLOP_KMSDRM_KEEP_MASTER" /**< bool: keep DRM master when a Vulkan window asks for VK_KHR_display */
#define SDLOP_HINT_KMSDRM_NOFLIP         "SDLOP_KMSDRM_NOFLIP"     /**< bool: never page flip; always drmModeSetCrtc */
#define SDLOP_HINT_KMSDRM_REQUIRE_MASTER "SDLOP_KMSDRM_REQUIRE_MASTER" /**< bool: fail without DRM master (default on) */
#define SDLOP_HINT_KMSDRM_VT_SWITCH      "SDLOP_KMSDRM_VT_SWITCH"  /**< bool: activate a VT at startup */
/* fbdev */
#define SDLOP_HINT_FBDEV_PATH            "SDLOP_FBDEV_PATH"        /**< framebuffer device (default /dev/fb0) */
#define SDLOP_HINT_FBDEV_SIZE            "SDLOP_FBDEV_SIZE"        /**< WxH: emulate a framebuffer of that size (testing) */
#define SDLOP_HINT_FBDEV_DUMP            "SDLOP_FBDEV_DUMP"        /**< write presented frames to this PPM file (testing) */
#define SDLOP_HINT_FBDEV_CURSOR          "SDLOP_FBDEV_CURSOR"      /**< bool: software cursor (default on) */
#define SDLOP_HINT_FBDEV_TAKEOVER        "SDLOP_FBDEV_TAKEOVER"    /**< bool: own the console (graphics mode, VT_PROCESS; default on) */
#define SDLOP_HINT_FBDEV_VT_SWITCH       "SDLOP_FBDEV_VT_SWITCH"   /**< bool: activate a VT at startup */
/* libraries (dlopen names, or "none") */
#define SDLOP_HINT_LIB_X11               "SDLOP_LIB_X11"   /**< libX11: GLX, and EGL without EGL_EXT_platform_xcb */
#define SDLOP_HINT_LIB_XCB               "SDLOP_LIB_XCB"
#define SDLOP_HINT_LIB_XCB_RANDR         "SDLOP_LIB_XCB_RANDR"
#define SDLOP_HINT_LIB_XCB_RENDER        "SDLOP_LIB_XCB_RENDER"
#define SDLOP_HINT_LIB_XCB_SHM           "SDLOP_LIB_XCB_SHM"
#define SDLOP_HINT_LIB_XCB_XINPUT        "SDLOP_LIB_XCB_XINPUT"
#define SDLOP_HINT_LIB_XCB_XKB           "SDLOP_LIB_XCB_XKB"
#define SDLOP_HINT_LIB_XKB               "SDLOP_LIB_XKB"
#define SDLOP_HINT_LIB_XKBCOMMON_X11     "SDLOP_LIB_XKBCOMMON_X11"
#define SDLOP_HINT_LIB_WAYLAND           "SDLOP_LIB_WAYLAND"
#define SDLOP_HINT_LIB_EGL               "SDLOP_LIB_EGL"
#define SDLOP_HINT_LIB_DRM               "SDLOP_LIB_DRM"
#define SDLOP_HINT_LIB_GBM               "SDLOP_LIB_GBM"
/** @} */

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
 * ring buffer. This API is global and not window/VT-focus filtered.
 * The SDL event queue is filtered: on the console drivers raw keys become
 * SDL key events only while the owned console is in the foreground; on
 * X11/Wayland with SDLOP_RAW_INPUT=1 keys need one of our windows to have
 * keyboard focus and pointer input needs the pointer over one of them (or
 * relative mode). Polling this ring does not consume SDL events.
 *
 * \returns the number of events written into events, or -1 on error.
 */
extern int SDLop_PollRawEvents(SDLop_RawEvent *events, int max_events);

/**
 * Query whether the low-latency evdev worker thread is running.
 *
 * When false (e.g. /dev/input is not readable, or on X11/Wayland without
 * SDLOP_RAW_INPUT=1), SDLop transparently uses window-system input instead.
 * On X11/Wayland a true result means evdev is the only SDL keyboard and
 * mouse source.
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
