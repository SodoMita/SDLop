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
    bool text_input_active; /* SDL_StartTextInput() gate (stock: text events only while active) */
    SDL_Rect text_input_rect; /* SDL_SetTextInputArea */
    int text_input_cursor;
    SDL_PropertiesID text_input_props; /* SDL_StartTextInputWithProperties copy */
    Uint8 clear_r, clear_g, clear_b;
    float display_scale;    /* output scale the window currently sits on */
    SDL_Surface *surface;   /* software window surface (owned by window) */
    void *driverdata;
    struct SDL_Window *next;
    SDL_DisplayID displayID;          /* display the window is currently on */
    bool has_fullscreen_mode;         /* an exclusive fullscreen mode was requested */
    SDL_DisplayMode fullscreen_mode;  /* requested exclusive mode (displayID set) */

    /* SDL 3.4 window state (SDL_video.c; drivers read these in their hooks) */
    bool undefined_x, undefined_y;    /* created with SDL_WINDOWPOS_UNDEFINED */
    int min_w, min_h, max_w, max_h;   /* 0 = no limit */
    float min_aspect, max_aspect;     /* 0 = no limit */
    float opacity;
    SDL_Surface *icon;                /* ARGB8888 copy */
    struct SDL_Window *parent;
    SDL_Rect mouse_rect;              /* empty = none */
    SDL_Rect safe_rect;               /* empty = whole window */
    SDL_PropertiesID props;
    SDL_HitTest hit_test;
    void *hit_test_data;
    SDL_ProgressState progress_state;
    float progress_value;
    SDL_WindowFlags pending_flags;    /* grabs requested while hidden */
    int popup_offset_x, popup_offset_y; /* tooltip / popup menu: offset from parent */
    bool is_destroying;
    bool is_dropping;                 /* drag-and-drop in progress (SDL_dropevents.c) */
};

#define SDLOP_WINDOW_IS_POPUP(w) ((w) && ((w)->flags & (SDL_WINDOW_TOOLTIP | SDL_WINDOW_POPUP_MENU)) != 0)

/* ------------------------------------------------------------------ */
/* Video displays                                                      */
/* ------------------------------------------------------------------ */

#define SDLOP_MAX_DISPLAYS 16

typedef struct SDLop_Display
{
    SDL_DisplayID id;
    char *name;
    SDL_Rect bounds;                /* absolute desktop rectangle (left-to-right) */
    float content_scale;
    SDL_DisplayOrientation natural_orientation;
    SDL_DisplayOrientation current_orientation;
    SDL_DisplayMode desktop_mode;   /* the mode outside exclusive fullscreen */
    SDL_DisplayMode current_mode;   /* set by an exclusive fullscreen mode switch; w == 0: desktop mode */
    SDL_Window *fullscreen_window;  /* window whose exclusive mode the display is in */
    SDL_DisplayMode *fullscreen_modes;   /* sorted largest first; desktop mode always present */
    int num_fullscreen_modes;
    SDL_PropertiesID props;              /* display properties group (0 until first access) */
    void *internal;                 /* driver data (KMSDRM_DisplayData*, ...) */
} SDLop_Display;

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

    /* software rendering: window framebuffer (window surface) */
    bool (*CreateWindowFramebuffer)(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface **surface);
    bool (*UpdateWindowFramebuffer)(SDLop_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects);
    void (*DestroyWindowFramebuffer)(SDLop_VideoDevice *device, SDL_Window *window);

    /* OpenGL (EGL) */
    void *(*GL_CreateContext)(SDLop_VideoDevice *device, SDL_Window *window);
    bool (*GL_MakeCurrent)(SDLop_VideoDevice *device, SDL_Window *window, void *context);
    bool (*GL_SwapBuffers)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*GL_DeleteContext)(SDLop_VideoDevice *device, void *context);
    SDL_FunctionPointer (*GL_GetProcAddress)(SDLop_VideoDevice *device, const char *proc);
    bool (*GL_SetSwapInterval)(SDLop_VideoDevice *device, int interval);
    bool (*GL_GetSwapInterval)(SDLop_VideoDevice *device, int *interval);

    /* Vulkan WSI */
    bool (*Vulkan_CreateSurface)(SDLop_VideoDevice *device, SDL_Window *window, void *instance, const void *allocator, Uint64 *surface);
    /* Vulkan instance extensions to advertise to apps (NULL-terminated).
     * Optional: when NULL, SDL_Vulkan_GetInstanceExtensions() returns the
     * default (VK_KHR_surface + VK_KHR_wayland_surface). */
    const char *const *(*Vulkan_GetInstanceExtensions)(SDLop_VideoDevice *device, Uint32 *count);

    /* ---- everything below is optional (NULL = generic behaviour) ---- */

    /* vkGetPhysicalDevice*PresentationSupportKHR; NULL = always supported */
    bool (*Vulkan_GetPresentationSupport)(SDLop_VideoDevice *device, void *instance, void *physical_device, Uint32 queue_family);

    /* Clipboard (SDL3 SDL_VideoDevice layout). The app's offer lives in
     * SDL_clipboard.c; drivers read it via SDLOP_GetInternalClipboardData().
     * NULL = in-process clipboard only. */
    bool (*SetClipboardData)(SDLop_VideoDevice *device);
    void *(*GetClipboardData)(SDLop_VideoDevice *device, const char *mime_type, size_t *size);
    bool (*HasClipboardData)(SDLop_VideoDevice *device, const char *mime_type);
    const char *const *(*GetTextMimeTypes)(SDLop_VideoDevice *device, size_t *num_mime_types);
    bool (*SetPrimarySelectionText)(SDLop_VideoDevice *device, const char *text);
    char *(*GetPrimarySelectionText)(SDLop_VideoDevice *device);
    bool (*HasPrimarySelectionText)(SDLop_VideoDevice *device);

    /* drag-and-drop target on/off (SDL_EVENT_DROP_FILE/TEXT enabled) */
    void (*AcceptDragAndDrop)(SDL_Window *window, bool accept);

    /* SDL 3.4 window management (same contracts as SDL_sysvideo.h; NULL =
     * SDL_Unsupported() where stock SDL3 reports that) */
    bool supports_popups;            /* SDL_WINDOW_TOOLTIP / SDL_WINDOW_POPUP_MENU */
    bool window_system_input;        /* the driver has its own keyboard/mouse stream (X11, Wayland) */
    void (*SetWindowMinimumSize)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*SetWindowMaximumSize)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*SetWindowAspectRatio)(SDLop_VideoDevice *device, SDL_Window *window);
    bool (*GetWindowBordersSize)(SDLop_VideoDevice *device, SDL_Window *window, int *top, int *left, int *bottom, int *right);
    bool (*SetWindowOpacity)(SDLop_VideoDevice *device, SDL_Window *window, float opacity);
    bool (*SetWindowParent)(SDLop_VideoDevice *device, SDL_Window *window, SDL_Window *parent);
    bool (*SetWindowModal)(SDLop_VideoDevice *device, SDL_Window *window, bool modal);
    void (*SetWindowBordered)(SDLop_VideoDevice *device, SDL_Window *window, bool bordered);
    void (*SetWindowResizable)(SDLop_VideoDevice *device, SDL_Window *window, bool resizable);
    void (*SetWindowAlwaysOnTop)(SDLop_VideoDevice *device, SDL_Window *window, bool on_top);
    bool (*SetWindowIcon)(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface *icon);
    bool (*SetWindowMouseGrab)(SDLop_VideoDevice *device, SDL_Window *window, bool grabbed);
    bool (*SetWindowKeyboardGrab)(SDLop_VideoDevice *device, SDL_Window *window, bool grabbed);
    bool (*SetWindowMouseRect)(SDLop_VideoDevice *device, SDL_Window *window);
    bool (*FlashWindow)(SDLop_VideoDevice *device, SDL_Window *window, SDL_FlashOperation operation);
    bool (*SyncWindow)(SDLop_VideoDevice *device, SDL_Window *window);
    void (*ShowWindowSystemMenu)(SDLop_VideoDevice *device, SDL_Window *window, int x, int y);
    bool (*SetWindowFocusable)(SDLop_VideoDevice *device, SDL_Window *window, bool focusable);
    bool (*SetWindowHitTest)(SDLop_VideoDevice *device, SDL_Window *window, bool enabled);
    bool (*UpdateWindowShape)(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface *shape);
    /* real mode switch for exclusive fullscreen (stock SetDisplayMode); `mode` is one of the
     * display's fullscreen modes, or its desktop mode to restore. Optional: without it
     * exclusive modes are emulated (the window just covers the display). */
    bool (*SetDisplayMode)(SDLop_VideoDevice *device, SDLop_Display *display, const SDL_DisplayMode *mode);
    bool (*SuspendScreenSaver)(SDLop_VideoDevice *device);
    void *(*GetWindowICCProfile)(SDLop_VideoDevice *device, SDL_Window *window, size_t *size);
    bool (*SetWindowFramebufferVSync)(SDLop_VideoDevice *device, SDL_Window *window, int vsync);
    bool (*GetWindowFramebufferVSync)(SDLop_VideoDevice *device, SDL_Window *window, int *vsync);
    SDL_SystemTheme (*GetSystemTheme)(SDLop_VideoDevice *device);
    /* drawable size (HiDPI); NULL = window size times the display mode's
     * pixel density */
    void (*GetWindowSizeInPixels)(SDLop_VideoDevice *device, SDL_Window *window, int *w, int *h);

    /* EGL handles of the current context (SDL_EGL_*); NULL = not EGL */
    void *(*EGL_GetDisplay)(SDLop_VideoDevice *device);
    void *(*EGL_GetConfig)(SDLop_VideoDevice *device);
    void *(*EGL_GetWindowSurface)(SDLop_VideoDevice *device, SDL_Window *window);

    /* Cursors (SDL_mouse.c). CreateCursor fills cursor->internal from
     * cursor->surface / cursor->system_id; ShowCursor(NULL) hides. */
    bool (*CreateCursor)(SDLop_VideoDevice *device, SDL_Cursor *cursor);
    void (*FreeCursor)(SDLop_VideoDevice *device, SDL_Cursor *cursor);
    bool (*ShowCursor)(SDLop_VideoDevice *device, SDL_Cursor *cursor);
    bool (*WarpMouse)(SDLop_VideoDevice *device, SDL_Window *window, float x, float y);
    bool (*WarpMouseGlobal)(SDLop_VideoDevice *device, float x, float y);
    bool (*CaptureMouse)(SDLop_VideoDevice *device, SDL_Window *window); /* NULL window = release */
    SDL_MouseButtonFlags (*GetGlobalMouseState)(SDLop_VideoDevice *device, float *x, float *y);

    /* Text input / IME (stock contracts; NULL = nothing to do) */
    bool (*UpdateTextInputArea)(SDLop_VideoDevice *device, SDL_Window *window);
    bool (*StartTextInput)(SDLop_VideoDevice *device, SDL_Window *window, SDL_PropertiesID props);
    bool (*StopTextInput)(SDLop_VideoDevice *device, SDL_Window *window);
    bool (*ClearComposition)(SDLop_VideoDevice *device, SDL_Window *window);
};

/* Cursor object (SDL_mouse.c) */
struct SDL_Cursor
{
    SDL_SystemCursor system_id;    /* SDL_SYSTEM_CURSOR_COUNT for image cursors */
    SDL_Surface *surface;          /* ARGB8888 image (first frame), NULL for system cursors */
    int hot_x, hot_y;
    struct SDL_Cursor **frames;    /* animated: one image cursor per frame */
    Uint32 *durations;             /* animated: ms per frame (0 = stop) */
    int num_frames, current_frame;
    Uint64 last_update;
    void *internal;                /* driver data */
    struct SDL_Cursor *next;
};
void SDLOP_MouseQuit(void);
SDL_Cursor *SDLOP_GetCurrentCursor(void); /* NULL when hidden */
bool SDLOP_CursorVisible(void);
void SDLOP_RedrawCursor(void);         /* re-apply SDLOP_GetCurrentCursor() via ShowCursor */
void SDLOP_UpdateCursorAnimation(void); /* SDL_PumpEvents */
void SDLOP_UpdateWindowGrab(SDL_Window *window);
void SDLOP_OnWindowMoved(SDL_Window *window); /* popup offsets; moves child popups */
/* freedesktop / CSS cursor name for a system cursor (SDL_GetCSSCursorName) */
const char *SDLOP_GetCSSCursorName(SDL_SystemCursor id, const char **fallback_name);
void SDLOP_ConstrainMouse(SDL_Window *window, float *x, float *y);

/* Clipboard state shared with drivers (SDL_clipboard.c) */
void *SDLOP_GetInternalClipboardData(const char *mime_type, size_t *size); /* malloc'd, NUL-padded copy */
bool SDLOP_HasInternalClipboardData(const char *mime_type);
const char *const *SDLOP_GetClipboardOfferMimeTypes(size_t *count);
/* a foreign owner took the selection (owner=false) or our own offer changed */
void SDLOP_SendClipboardUpdate(bool owner, char **mime_types, size_t num_mime_types);
/* the system took the clipboard from us: drop the app's offer (cleanup runs) */
void SDLOP_CancelClipboardData(void);

/* SDL_dropevents.c */
bool SDLOP_SendDropFile(SDL_Window *window, const char *source, const char *file);
bool SDLOP_SendDropPosition(SDL_Window *window, float x, float y);
bool SDLOP_SendDropText(SDL_Window *window, const char *text);
bool SDLOP_SendDropComplete(SDL_Window *window);
/* text/uri-list (local files -> DROP_FILE) or text (DROP_TEXT per line),
 * then DROP_COMPLETE; tokenizes `data` in place */
void SDLOP_SendDropPayload(SDL_Window *window, bool uri_list, char *data);
bool SDLOP_IsAcceptingDragAndDrop(void);
void SDLOP_ToggleDragAndDropSupport(void);
int SDLOP_URIToLocal(const char *src, char *dst); /* file: URI -> path (in place ok); -1 = not local */
const void *SDLCALL SDLOP_ClipboardTextCallback(void *userdata, const char *mime_type, size_t *size);

/* Provided by backends */
extern SDLop_VideoDevice SDLop_emscripten_device;
extern SDLop_VideoDevice SDLop_wayland_device;
extern SDLop_VideoDevice SDLop_x11_device;
extern SDLop_VideoDevice SDLop_kmsdrm_device;
extern SDLop_VideoDevice SDLop_fbdev_device;
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
    SDLop_Display displays[SDLOP_MAX_DISPLAYS];
    int num_displays;
    SDL_DisplayID next_display_id;

    /* keyboard state (SDL_GetKeyboardState) */
    bool keystate[SDL_SCANCODE_COUNT];
    SDL_Keymod modstate;

    /* focus */
    SDL_Window *keyboard_focus;
    SDL_Window *mouse_focus;
    /* another client holds an active keyboard / pointer grab while our
       window keeps nominal focus (X11 grab-mode Focus/Crossing events): the
       window system delivers nothing to us, so raw evdev must not either */
    bool keyboard_grabbed_elsewhere;
    bool pointer_grabbed_elsewhere;

    /* mouse state */
    float mouse_x, mouse_y;         /* window-relative position */
    float mouse_last_x, mouse_last_y; /* last reported position (for deltas) */
    bool mouse_has_position;        /* false until first motion after focus change */
    float mouse_xrel_acc, mouse_yrel_acc; /* accumulators for GetRelativeMouseState */
    SDL_MouseButtonFlags mouse_buttons;
    SDL_Window *relative_mode_window;

    /* window management */
    SDL_Window *grabbed_window;
    bool suspend_screensaver;

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

    /* KMSDRM VT-switch signals owned by the driver's signalfd; helper
     * threads (raw evdev worker) block these so the signals are always
     * consumed via the fd, never via a default disposition. 0 = unset. */
    int vt_signals[2];
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
/* IME composition (stock SDL_SendEditingText / ...Candidates): only while
 * the focused window has text input active; start/length in characters */
void SDLOP_SendEditingText(const char *utf8, int start, int length);
void SDLOP_SendEditingTextCandidates(char **candidates, int num_candidates, int selected_candidate, bool horizontal);
/* SDL_PROP_TEXTINPUT_* with stock defaults (props 0 = all defaults) */
SDL_TextInputType SDLOP_GetTextInputType(SDL_PropertiesID props);
SDL_Capitalization SDLOP_GetTextInputCapitalization(SDL_PropertiesID props);
bool SDLOP_GetTextInputAutocorrect(SDL_PropertiesID props);
bool SDLOP_GetTextInputMultiline(SDL_PropertiesID props);

/* Mouse. Absolute x/y are window-relative; pass SDLOP_NO_POS to leave
 * the stored position untouched (relative-only motion). */
#define SDLOP_NO_POS (-1e30f)
void SDLOP_SendMouseMotion(float x, float y, float xrel, float yrel, Uint64 timestamp_ns);
void SDLOP_SendMouseButton(bool down, Uint8 button, float x, float y, Uint64 timestamp_ns);
void SDLOP_SendTouch(Uint32 type, Uint64 touchID, Uint64 fingerID, float x, float y,
                     float dx, float dy, float pressure, SDL_WindowID windowID,
                     Uint64 timestamp_ns);
void SDLOP_ResetTouchMouse(void); /* SDL_Init / SDL_Quit */
/* a mouse event made up by touch/pen emulation; fx/fy are window pixels */
void SDLOP_SendSyntheticMouse(Uint32 type, SDL_Window *window, float fx, float fy, Uint8 button,
                              SDL_MouseID which, Uint64 timestamp_ns);

/* Pens (SDL_pen.c; stock SDL_pen_c.h). Timestamps are monotonic ns, 0 = now. */
#define SDLOP_PEN_CAPABILITY_PRESSURE (1u << 0)
#define SDLOP_PEN_CAPABILITY_XTILT (1u << 1)
#define SDLOP_PEN_CAPABILITY_YTILT (1u << 2)
#define SDLOP_PEN_CAPABILITY_DISTANCE (1u << 3)
#define SDLOP_PEN_CAPABILITY_ROTATION (1u << 4)
#define SDLOP_PEN_CAPABILITY_SLIDER (1u << 5)
#define SDLOP_PEN_CAPABILITY_TANGENTIAL_PRESSURE (1u << 6)
#define SDLOP_PEN_CAPABILITY_ERASER (1u << 7)
typedef enum SDLop_PenSubtype
{
    SDLOP_PEN_TYPE_UNKNOWN,
    SDLOP_PEN_TYPE_ERASER,
    SDLOP_PEN_TYPE_PEN,
    SDLOP_PEN_TYPE_PENCIL,
    SDLOP_PEN_TYPE_BRUSH,
    SDLOP_PEN_TYPE_AIRBRUSH
} SDLop_PenSubtype;
typedef struct SDLop_PenInfo
{
    Uint32 capabilities; /* SDLOP_PEN_CAPABILITY_* */
    float max_tilt;      /* degrees, -1 unknown */
    Uint32 wacom_id;
    int num_buttons; /* -1 unknown */
    SDLop_PenSubtype subtype;
    SDL_PenDeviceType device_type;
} SDLop_PenInfo;
SDL_PenID SDLOP_AddPen(const char *name, const SDLop_PenInfo *info, void *handle, SDL_Window *window,
                       bool in_proximity, Uint64 timestamp_ns);
void SDLOP_RemovePen(SDL_PenID id, SDL_Window *window, Uint64 timestamp_ns);
void SDLOP_RemoveAllPens(void); /* video quit: no events */
SDL_PenID SDLOP_FindPenByHandle(void *handle);
void *SDLOP_GetPenHandle(SDL_PenID id);
SDL_PenInputFlags SDLOP_GetPenStatus(SDL_PenID id, float *axes, int num_axes);
void SDLOP_SendPenProximity(Uint64 timestamp_ns, SDL_PenID id, SDL_Window *window, bool in);
void SDLOP_SendPenTouch(Uint64 timestamp_ns, SDL_PenID id, SDL_Window *window, bool eraser, bool down);
void SDLOP_SendPenMotion(Uint64 timestamp_ns, SDL_PenID id, SDL_Window *window, float x, float y);
void SDLOP_SendPenAxis(Uint64 timestamp_ns, SDL_PenID id, SDL_Window *window, SDL_PenAxis axis, float value);
void SDLOP_SendPenButton(Uint64 timestamp_ns, SDL_PenID id, SDL_Window *window, Uint8 button, bool down);

/* Input device registry (SDL_devices.c): sends *_ADDED / *_REMOVED */
void SDLOP_AddMouse(SDL_MouseID id, const char *name);
void SDLOP_RemoveMouse(SDL_MouseID id, bool send_event);
void SDLOP_AddKeyboard(SDL_KeyboardID id, const char *name);
void SDLOP_RemoveKeyboard(SDL_KeyboardID id, bool send_event);
void SDLOP_AddTouch(SDL_TouchID id, SDL_TouchDeviceType type, const char *name);
void SDLOP_RemoveTouch(SDL_TouchID id);
void SDLOP_TrackTouch(Uint32 type, SDL_TouchID touchID, SDL_FingerID fingerID, float x, float y, float pressure);
void SDLOP_DevicesQuit(void);
void SDLOP_SendMouseWheel(float x, float y, Uint64 timestamp_ns);

/* Timestamp helpers */
Uint64 SDLOP_MonotonicNS(void);
static inline Uint64 SDLOP_MonoToSDLTicks(Uint64 mono_ns)
{
    return mono_ns > sdlop.init_monotonic_ns ? mono_ns - sdlop.init_monotonic_ns : 0;
}

/* Window helpers */
void SDLOP_GetEGLAttributeCallbacks(SDL_EGLAttribArrayCallback *platform, SDL_EGLIntArrayCallback *surface,
                                    SDL_EGLIntArrayCallback *context, void **userdata); /* SDL_gl.c */
void SDLOP_AssertionsQuit(void);       /* SDL_misc.c, from SDL_Quit */
bool SDLOP_WindowValid(SDL_Window *window);   /* in the window list */
SDL_Window *SDLOP_GetFocusedKeyboardWindow(void);
SDL_WindowID SDLOP_FocusWindowID(void);   /* focused window id or 0 */

/* Keymap (xkb) management, implemented in SDL_keyboard.c */
bool SDLOP_KeyboardSetKeymapString(const char *keymap_string, size_t length); /* wayland */
bool SDLOP_KeyboardSetDefaultKeymap(void);
void SDLOP_KeyboardUpdateXkbModifiers(Uint32 depressed, Uint32 latched, Uint32 locked);
void SDLOP_KeyboardUpdateXkbState(Uint32 depressed, Uint32 latched, Uint32 locked,
                                  Uint32 base_group, Uint32 latched_group, Uint32 locked_group); /* x11/wayland */
bool SDLOP_KeyboardKeyRepeats(SDL_Scancode sc);
void SDLOP_KeyboardProcessRepeats(void);
void SDLOP_KeyboardQuit(void);
/* Resolve scancode (evdev raw code) -> SDL_Keycode + text using xkb.
 * Returns the keycode; fills text_utf8 (may write "") when non-NULL. */
/* SDL_GetKeyName helper: the printed (capital) form of a keycode */
SDL_Keycode SDLOP_KeyboardKeyNameCapital(SDL_Keycode key);
SDL_Keycode SDLOP_KeyboardTranslateKey(Uint16 rawcode, bool key_event, char *text_utf8, size_t text_size, bool *repeats_out);

/* Raw input worker (evdev), implemented in sdlop_evdev.c */
bool SDLOP_RawInputInit(void);      /* starts worker thread if possible */
void SDLOP_RawInputQuit(void);
bool SDLOP_RawInputHasKeyboard(void);    /* ensure a working raw keyboard before K_OFF */
/* Input source policy: SDL keyboard *and* mouse events come from exactly one
   source. On a window-system driver this is true only when raw input was
   requested (SDLOP_RAW_INPUT=1) and the worker reads both a keyboard and a
   pointer; the driver then drops its own key/button/motion/wheel events.
   On the console drivers raw input is the only source. */
bool SDLOP_RawInputIsSource(void);
/* Console keyboard ownership: call AttachConsole once the console keyboard is
   in K_OFF (raw key events then reach SDL, and Ctrl+Alt+Fn is forwarded),
   DetachConsole as soon as it is given back. */
void SDLOP_RawInputAttachConsole(int console_fd);
void SDLOP_RawInputDetachConsole(void);
void SDLOP_RawInputPump(void);      /* drain raw ring -> SDL events (main thread) */
void SDLOP_RawInputSetRepeatInfo(Uint32 delay_ms, Uint32 interval_ms);

/* Video core helpers (SDL_video.c) */
bool SDLOP_VideoInit(void);
void SDLOP_VideoQuit(void);
void SDLOP_AddWindow(SDL_Window *window);
void SDLOP_RemoveWindow(SDL_Window *window);
void SDLOP_SendQuitEvent(void);

/* Display list management (used by backends). Bounds are laid out
 * left-to-right on every add/remove, like SDL3. */
SDL_DisplayID SDLOP_AddDisplay(const char *name, int w, int h, int refresh_mhz,
                               SDL_DisplayOrientation natural_orientation,
                               void *internal, bool send_event);
void SDLOP_DelDisplay(SDL_DisplayID displayID, bool send_event);
void SDLOP_SendDisplayEvent(SDL_DisplayID displayID, SDL_EventType type, Sint32 data1, Sint32 data2);
bool SDLOP_GetDisplayBoundsById(SDL_DisplayID displayID, SDL_Rect *rect);
int SDLOP_GetDisplayIndexById(SDL_DisplayID displayID); /* -1 when absent */
SDL_DisplayID SDLOP_GetDisplayIdAtIndex(int index);      /* 0 when absent */
bool SDLOP_AddFullscreenDisplayMode(SDL_DisplayID displayID, const SDL_DisplayMode *mode);
/* drivers: the display is now in `mode` (NULL = its desktop mode); sends
 * SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED on a change */
void SDLOP_SetCurrentDisplayMode(SDL_DisplayID displayID, const SDL_DisplayMode *mode);
/* exclusive fullscreen: apply (enter) or give back (leave) the window's
 * fullscreen mode on its display (stock SDL_UpdateFullscreenMode) */
void SDLOP_UpdateFullscreenMode(SDL_Window *window, bool enter);
/* true shortly after a real mode switch, while focus churn from it is expected */
bool SDLOP_DisplayModeSwitchPending(void);
void SDLOP_UpdateWindowDisplay(SDL_Window *window);      /* re-resolves window->displayID */

/* GL attributes (SDL_gl.c) */
typedef struct SDLOP_GLAttributes
{
    int red_size, green_size, blue_size, alpha_size, buffer_size;
    int doublebuffer;
    int depth_size, stencil_size;
    int accum_red, accum_green, accum_blue, accum_alpha;
    int stereo;
    int multisamplebuffers, multisamplesamples;
    int major_version, minor_version;
    int flags;
    int profile_mask;
    int share_with_current_context;
    int framebuffer_srgb_capable;
    int floatbuffers;
    int release_behavior;   /* SDL_GL_CONTEXT_RELEASE_BEHAVIOR, default FLUSH */
    int reset_notification; /* SDL_GL_CONTEXT_RESET_NOTIFICATION, default NO_NOTIFICATION */
    int no_error;           /* SDL_GL_CONTEXT_NO_ERROR */
    int accelerated_visual; /* -1 = unset/auto, 0 = software, 1 = hardware */
} SDLOP_GLAttributes;

extern SDLOP_GLAttributes sdlop_glattrs;
SDL_GLContext SDLOP_GL_CurrentContext(void);
void SDLOP_GL_NotifyWindowResized(SDL_Window *window);
void SDLOP_GL_NotifyWindowDestroyed(SDL_Window *window);
void SDLOP_GL_Quit(void);
bool SDLOP_GL_CanCreateWindow(void);

/* Vulkan offload cleanup hooks (SDL_vulkan.c) */
void SDLOP_Vulkan_NotifyWindowDestroyed(SDL_Window *window);
void SDLOP_Vulkan_Quit(void);
bool SDLOP_Vulkan_CanCreateWindow(void);

/* Surface helpers (SDL_surface.c) */
int SDLOP_BytesPerPixel(SDL_PixelFormat format);

/* Threads (SDL_thread.c) */
void SDLOP_ThreadsQuit(void);                       /* SDL_Quit() hook: TLS teardown */
const char *SDLOP_GetPersistentString(const char *string); /* per-thread intern pool */

/* Environment (SDL_stdlib.c) */
void SDLOP_EnvironmentQuit(void);                   /* SDL_Quit() hook */

/* Logging of subsystem state */
extern bool sdlop_quitting;

/* Core services (SDLop_core.c, SDL_hints.c, ...) */
bool SDLOP_IsMainThread(void);
void SDLOP_InitMainThread(void);
bool SDLOP_GetStringBoolean(const char *value, bool default_value); /* SDL3's SDL_GetStringBoolean */
/* SDLop option lookup: every SDLOP_* knob is a hint, so SDL_SetHint() sets
   it from code and an environment variable of the same name overrides that
   (SDL_HINT_OVERRIDE beats the environment). Unset/empty -> default_value;
   0/false/off/no -> false; anything else -> true. */
bool SDLOP_HintFlag(const char *name, bool default_value);
/* first non-empty SDL_GetHint() of a NULL-terminated name list (each name:
 * override hint > environment > hint), e.g. a per-API option before the
 * global one */
const char *SDLOP_HintFirst(const char *const *names);
void SDLOP_SendSimpleEvent(SDL_EventType type);  /* type + timestamp only */
const char *SDLOP_GetExeName(void);  /* basename of /proc/self/exe or NULL */
const char *SDLOP_GetAppID(void);    /* SDL3 SDL_GetAppID() for WM_CLASS / xdg app_id */

/* SDL3 "temporary memory": freed at the next SDL_PumpEvents() on this
 * thread, or handed over with the event that references it */
void *SDLOP_AllocateTemporaryMemory(size_t size);
void SDLOP_FreeTemporaryMemory(void);

/* SDL_Quit() teardown, in call order */
void SDLOP_QuitTimers(void);
void SDLOP_QuitEvents(void);
void SDLOP_QuitClipboard(void);
void SDLOP_QuitFilesystem(void); /* SDL_filesystem.c: drop path caches */
void SDLOP_QuitLog(void);
void SDLOP_QuitHints(void);
void SDLOP_QuitProperties(void);

#endif /* sdlop_internal_h_ */
