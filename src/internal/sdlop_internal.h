/*
  SDLop - internal shared definitions.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_internal_h_
#define sdlop_internal_h_

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Window internal                                                     */
/* ------------------------------------------------------------------ */

struct SDL_Window
{
    SDL_WindowID id;
    char *title;
    int x, y;           /* last known position (compositor-provided) */
    int w, h;           /* requested size */
    SDL_WindowFlags flags;
    Uint8 clear_r, clear_g, clear_b;
    void *driverdata;
    struct SDL_Window *next;
};

/* ------------------------------------------------------------------ */
/* Video driver interface                                              */
/* ------------------------------------------------------------------ */

typedef struct SDLop_VideoDevice SDLop_VideoDevice;

struct SDLop_VideoDevice
{
    const char *name;

    bool (*Init)(SDLop_VideoDevice *device);
    void (*Quit)(SDLop_VideoDevice *device);

    bool (*CreateWindow)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*DestroyWindow)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*ShowWindow)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*HideWindow)(SDLop_VideoDevice *device, SDL_Window *window);
    bool (*SetWindowTitle)(SDLop_VideoDevice *device, SDL_Window *window);
    bool (*SetWindowSize)(SDLop_VideoDevice *device, SDL_Window *window);
    bool (*SetWindowPosition)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*MinimizeWindow)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*MaximizeWindow)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*RestoreWindow)(SDLop_VideoDevice *device, SDL_Window *window);
    bool (*SetWindowFullscreen)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*RaiseWindow)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*SetWindowClearColor)(SDLop_VideoDevice *device, SDL_Window *window);
    bool (*SetWindowRelativeMouseMode)(SDLop_VideoDevice *device, SDL_Window *window, bool enabled);

    /* Dispatch pending window-system events. timeout_ms < 0 means
     * non-blocking; >= 0 may block that long (used by SDL_WaitEvent). */
    void (*PumpEvents)(SDLop_VideoDevice *device, int timeout_ms);

    /* fd to poll for window-system activity (-1 if none) */
    int (*GetEventFD)(SDLop_VideoDevice *device);
};

/* Provided by backends */
extern SDLop_VideoDevice SDLop_wayland_device;
extern SDLop_VideoDevice SDLop_dummy_device;

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

#define SDLOP_MAX_WINDOWS 32

typedef struct SDLop_Globals
{
    bool init_done;
    SDL_InitFlags init_flags;
    Uint64 init_monotonic_ns;   /* CLOCK_MONOTONIC at SDL_Init, for timestamps */

    /* video */
    SDLop_VideoDevice *video;
    SDL_Window *windows;        /* linked list */
    int num_windows;
    SDL_WindowID next_window_id;

    /* keyboard state (SDL_GetKeyboardState) */
    bool keystate[SDL_SCANCODE_COUNT];
    SDL_Keymod modstate;

    /* focus */
    SDL_Window *keyboard_focus;
    SDL_Window *mouse_focus;

    /* mouse state */
    float mouse_x, mouse_y;         /* window-relative position */
    float mouse_xrel_acc, mouse_yrel_acc; /* accumulators for GetRelativeMouseState */
    SDL_MouseButtonFlags mouse_buttons;
    SDL_Window *relative_mode_window;

    /* key repeat emulation (for the raw evdev path) */
    SDL_Scancode repeat_scancode;
    Uint16 repeat_rawcode;
    bool repeat_active;
    Uint64 repeat_next_ns;      /* absolute CLOCK_MONOTONIC deadline */
    Uint32 repeat_delay_ms;
    Uint32 repeat_interval_ms;

    /* xkb (set by the wayland driver, else defaults) */
    void *xkb_context;          /* struct xkb_context* */
    void *xkb_keymap;           /* struct xkb_keymap* */
    void *xkb_state;            /* struct xkb_state* */

    /* user events */
    Uint32 next_user_event;

    /* event queue wakeup (raw input worker -> main thread) */
    int wake_fd;                /* eventfd, read to reset */
} SDLop_Globals;

extern SDLop_Globals sdlop;

/* ------------------------------------------------------------------ */
/* Internal event injection (called by input sources, main thread)     */
/* ------------------------------------------------------------------ */

/* Push an event into the SDL event queue (main thread only). */
bool SDLOP_PushEventInternal(SDL_Event *event);

/* Post a window event (RESIZED, FOCUS_*, CLOSE_REQUESTED, ...). */
void SDLOP_SendWindowEvent(SDL_Window *window, SDL_EventType type, Sint32 data1, Sint32 data2);

/* Keyboard: scancode-level (converted by callers from raw or wl codes).
 * timestamp_ns: CLOCK_MONOTONIC ns (0 = now). */
void SDLOP_SendKeyboardKey(bool down, bool repeat, SDL_Scancode scancode, Uint16 rawcode, Uint64 timestamp_ns);
void SDLOP_SendKeyboardText(const char *utf8, Uint64 timestamp_ns);

/* Mouse. Absolute x/y are window-relative; pass SDLOP_NO_POS to leave
 * the stored position untouched (relative-only motion). */
#define SDLOP_NO_POS (-1e30f)
void SDLOP_SendMouseMotion(float x, float y, float xrel, float yrel, Uint64 timestamp_ns);
void SDLOP_SendMouseButton(bool down, Uint8 button, float x, float y, Uint64 timestamp_ns);
void SDLOP_SendMouseWheel(float x, float y, Uint64 timestamp_ns);

/* Timestamp helpers */
Uint64 SDLOP_MonotonicNS(void);
static inline Uint64 SDLOP_MonoToSDLTicks(Uint64 mono_ns)
{
    return mono_ns > sdlop.init_monotonic_ns ? mono_ns - sdlop.init_monotonic_ns : 0;
}

/* Window helpers */
SDL_Window *SDLOP_GetFocusedKeyboardWindow(void);
SDL_WindowID SDLOP_FocusWindowID(void);   /* focused window id or 0 */

/* Keymap (xkb) management, implemented in SDL_keyboard.c */
bool SDLOP_KeyboardSetKeymapString(const char *keymap_string, size_t length); /* wayland */
bool SDLOP_KeyboardSetDefaultKeymap(void);
void SDLOP_KeyboardUpdateXkbModifiers(Uint32 depressed, Uint32 latched, Uint32 locked);
bool SDLOP_KeyboardKeyRepeats(SDL_Scancode sc);
void SDLOP_KeyboardProcessRepeats(void);
void SDLOP_KeyboardQuit(void);
/* Resolve scancode (evdev raw code) -> SDL_Keycode + text using xkb.
 * Returns the keycode; fills text_utf8 (may write "") when non-NULL. */
SDL_Keycode SDLOP_KeyboardTranslateKey(Uint16 rawcode, bool key_event, char *text_utf8, size_t text_size);

/* Raw input worker (evdev), implemented in sdlop_evdev.c */
bool SDLOP_RawInputInit(void);      /* starts worker thread if possible */
void SDLOP_RawInputQuit(void);
bool SDLOP_RawInputKeyboardActive(void); /* a raw keyboard device exists */
bool SDLOP_RawInputMouseActive(void);    /* a raw pointer device exists */
void SDLOP_RawInputPump(void);      /* drain raw ring -> SDL events (main thread) */
void SDLOP_RawInputSetRepeatInfo(Uint32 delay_ms, Uint32 interval_ms);

/* Video core helpers (SDL_video.c) */
bool SDLOP_VideoInit(void);
void SDLOP_VideoQuit(void);
void SDLOP_AddWindow(SDL_Window *window);
void SDLOP_RemoveWindow(SDL_Window *window);
void SDLOP_SendQuitEvent(void);

/* Logging of subsystem state */
extern bool sdlop_quitting;

#endif /* sdlop_internal_h_ */
