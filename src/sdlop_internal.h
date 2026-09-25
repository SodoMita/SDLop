/*
  SDLop -- internal interfaces shared by the library's own modules.

  Nothing in this file is installed and nothing here is part of the SDL3 API.
  Application code only ever sees the installed SDL3 headers.
*/

#ifndef SDLOP_INTERNAL_H
#define SDLOP_INTERNAL_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Small utilities                                                           */
/* ------------------------------------------------------------------------- */

#define SDLOP_ARRAYLEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

#if defined(__GNUC__)
#define SDLOP_HOT __attribute__((hot))
#define SDLOP_LIKELY(x) __builtin_expect(!!(x), 1)
#define SDLOP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define SDLOP_HOT
#define SDLOP_LIKELY(x) (x)
#define SDLOP_UNLIKELY(x) (x)
#endif

void *SDLOP_Alloc(size_t size);
void *SDLOP_Calloc(size_t count, size_t size);
void *SDLOP_Realloc(void *mem, size_t size);
char *SDLOP_Strdup(const char *str);
void SDLOP_Free(void *mem);

/* ------------------------------------------------------------------------- */
/* Error / logging plumbing                                                  */
/* ------------------------------------------------------------------------- */

/* Set the error string and return false, like SDL's internal SDL_SetError+false. */
bool SDLOP_SetError(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);

/* SDL_InvalidParamError()/SDL_Unsupported() are macros in SDL_error.h (upstream
   defines them that way), so they need no definitions here - but note they only
   accept a parameter name, unquoted:
       return SDL_InvalidParamError(window);
   and they return whatever SDL_SetError() returns, which is false. */

static inline bool SDL_OutOfMemoryInternal(void)
{
    SDL_OutOfMemory();
    return false;
}

#define SDLOP_OutOfMemory() SDL_OutOfMemoryInternal()

/* Internal logging: thin wrappers over SDL_Log, so a SDLop build can be debugged
   with the same messages SDL3 prints. */
#define SDLOP_LogVerbose(...) SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define SDLOP_LogDebug(...)   SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define SDLOP_LogInfo(...)    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define SDLOP_LogWarn(...)    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define SDLOP_LogError(...)   SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Event queue                                                               */
/* ------------------------------------------------------------------------- */

/* The queue holds this many events; SDL3 uses the same order of magnitude. */
#define SDLOP_EVENT_QUEUE_SIZE 512

/* Push one event, honouring event filters and watches (the public SDL_PushEvent
   does exactly this). Safe to call from any thread. */
bool SDLOP_PushEvent(const SDL_Event *event);

/* Non-blocking peek/pump used by SDL_PumpEvents(). */
void SDLOP_PumpEventsInternal(void);

/* Wake SDL_WaitEvent()/SDL_WaitEventTimeout() waiters. Called by drivers after
   they pushed events from another thread. */
void SDLOP_SignalEvents(void);

/* Number of events currently queued (for tests and for back-pressure). */
int SDLOP_PeekQueuedEvents(void);

/* Public event API internals implemented in src/events/SDL_events.c */
int SDLOP_PeepEvents(SDL_Event *events, int numevents, SDL_EventAction action,
                     Uint32 minType, Uint32 maxType, bool is_internal);
bool SDLOP_WaitEventTimeoutNS(SDL_Event *event, Sint64 timeoutNS);
void SDLOP_FlushEventsInternal(Uint32 minType, Uint32 maxType);

/* Called once from SDL_Init. */
void SDLOP_InitEvents(void);
void SDLOP_QuitEvents(void);

/* ------------------------------------------------------------------------- */
/* Keyboard / mouse state (fed by whichever input source is active)           */
/* ------------------------------------------------------------------------- */

/* The key *layout* the platform says is in effect - on Wayland, the XKB keymap
   the compositor sent us. A scancode is a physical key position and never needs
   a layout; a keycode and the text a key produces do.

   The platform registers one of these when it has a layout to offer (Wayland
   does, and so will every future port with an input-method-aware session); the
   keyboard layer then asks it instead of deriving keycode and text from the
   built-in "us" tables. Without one - offscreen, KMSDRM, a test harness - the
   generated tables are used, which is exactly what SDL3 does when xkbcommon is
   unavailable. */
typedef struct SDLOP_KeyLayout
{
    /* Told about every key press/release, so the layout can track its own
       modifier state. Input usually arrives from the async evdev worker rather
       than from the platform's own event stream, so the layout cannot assume it
       is being kept up to date for us. */
    void (*update_key)(Uint32 evdev_code, bool down);
    /* SDL keycode produced by this evdev keycode in the current modifier state,
       or SDLK_UNKNOWN if the layout has nothing to say about the key. */
    SDL_Keycode (*keycode_from_evdev)(Uint32 evdev_code);
    /* UTF-8 text the key produces, or 0 for none. `buffer` gets a NUL-terminated
       string. */
    int (*text_from_evdev)(Uint32 evdev_code, char *buffer, size_t size);
} SDLOP_KeyLayout;

void SDLOP_SetKeyLayout(const SDLOP_KeyLayout *layout);
const SDLOP_KeyLayout *SDLOP_GetKeyLayout(void);

/* ------------------------------------------------------------------------- */
/* XKB layout (src/input/SDL_xkb.c)                                          */
/*                                                                            */
/* Shared by the backends that can ask the platform what layout is in effect   */
/* (Wayland: the compositor's wl_keyboard keymap; X11: the server's keymap,    */
/* read through libxkbcommon-x11). All of it is compiled out when the build has */
/* no xkbcommon, in which case the keyboard layer uses its generated tables.   */
/* ------------------------------------------------------------------------- */

#ifdef SDLOP_HAVE_XKBCOMMON
/* The layout state keycode/text questions are answered from; NULL when no
   keymap has been installed. */
struct xkb_keymap *SDLOP_XKBKeymap(void);
struct xkb_state *SDLOP_XKBState(void);
SDL_Keycode SDLOP_KeycodeFromKeysym(Uint32 keysym);
/* Does this key repeat while it is held? (xkbcommon knows: it is a property of
   the key in the current layout.) */
bool SDLOP_XKBKeyRepeats(Uint32 evdev_code);

bool SDLOP_XKBCompileFromString(const char *text, size_t size);
/* Compile a keymap from XKB rule names; NULL fields fall back to the
   XKB_DEFAULT_* environment and then to xkbcommon's own defaults. */
bool SDLOP_XKBCompileFromRules(const char *rules, const char *model, const char *layout,
                               const char *variant, const char *options);
/* mmap a keymap file descriptor (wl_keyboard.keymap() gives one), compile it and
   close it. */
bool SDLOP_XKBLoadKeymapFD(int fd);
/* SDLOP_XKB_KEYMAP=<file> was set: the platform's own keymap must be ignored. */
bool SDLOP_XKBOverrideActive(void);
/* Install the best layout available before the first key arrives: the override
   file if there is one, else the local XKB configuration. */
void SDLOP_XKBInit(void);
void SDLOP_XKBQuit(void);
#ifdef SDLOP_HAVE_XKBCOMMON_X11
/* Read the X server's core keyboard keymap. `xdisplay` is a Display*. */
bool SDLOP_XKBLoadFromX11(void *xdisplay, int device_id);
int SDLOP_XKBX11DeviceID(void *xdisplay);
#endif
#endif /* SDLOP_HAVE_XKBCOMMON */

/* Deliver a key press/release. `timestamp` is in nanoseconds of SDL_GetTicksNS()
   time. Produces SDL_EVENT_KEY_DOWN/KEY_UP and, when text input is active, the
   matching SDL_EVENT_TEXT_INPUT/TEXT_EDITING events.

   `evdev_code` is the Linux keycode the event came from (0 when unknown); it is
   only used to consult the platform key layout. `keycode` may be SDLK_UNKNOWN,
   in which case it is looked up from the layout or the scancode tables. */
void SDLOP_SendKeyEvent(SDL_Scancode scancode, SDL_Keycode keycode, SDL_Keymod modstate,
                        bool down, bool repeat, Uint64 timestamp, Uint32 evdev_code);
void SDLOP_SendKeymapChanged(Uint64 timestamp);

void SDLOP_SendMouseMotionAbsolute(SDL_WindowID windowID, float x, float y, Uint64 timestamp);
void SDLOP_SendMouseMotionRelative(SDL_WindowID windowID, float dx, float dy, Uint64 timestamp);
void SDLOP_SendMouseButton(SDL_MouseID mouseID, Uint8 button, bool down, float x, float y,
                           Uint64 timestamp);
void SDLOP_SendMouseWheel(SDL_WindowID windowID, float dx, float dy, SDL_MouseWheelDirection dir,
                          Uint64 timestamp);
void SDLOP_SendWindowFocusEvents(SDL_WindowID id, bool keyboard, bool mouse, Uint64 timestamp);
void SDLOP_ResetKeyboardState(void);

/* Keyboard/mouse state accessors used by the public API. */
void SDLOP_SetKeyboardFocus(SDL_Window *window);
SDL_Window *SDLOP_GetKeyboardFocus(void);
bool SDLOP_KeyboardStateGet(SDL_Scancode scancode);
void SDLOP_SetModState(SDL_Keymod mod);
void SDLOP_ClearMouseButtons(void);

/* ------------------------------------------------------------------------- */
/* Timer callbacks (implemented in src/timer/SDL_timer.c)                     */
/* ------------------------------------------------------------------------- */

void SDLOP_InitTimers(void);
void SDLOP_QuitTimers(void);
void SDLOP_RunTimerCallbacks(void);
/* Milliseconds until the next timer callback is due, or -1 for "none". */
Sint64 SDLOP_NextTimerTimeoutNS(void);

/* ------------------------------------------------------------------------- */
/* Main-thread callback queue (SDL_RunOnMainThread)                          */
/* ------------------------------------------------------------------------- */

void SDLOP_InitMainThreadQueue(void);
void SDLOP_QuitMainThreadQueue(void);
/* Runs queued callbacks. Called by SDL_PumpEvents(). */
void SDLOP_RunMainThreadCallbacks(void);

/* ------------------------------------------------------------------------- */
/* Pending input queue: where async input sources hand events over            */
/*                                                                            */
/* src/input/SDL_evdev.c runs a worker thread that reads /dev/input/event*     */
/* and appends raw records here. The main thread drains the queue in           */
/* SDL_PumpEvents() and turns the records into SDL events. Doing the encoding  */
/* on the main thread keeps the worker free of locks and allocations.          */
/* ------------------------------------------------------------------------- */

typedef struct SDLOP_RawInputRecord
{
    Uint64 timestamp_ns;   /* kernel timestamp, converted to SDL_GetTicksNS() domain */
    Uint32 device_id;
    Uint16 type;           /* evdev EV_* */
    Uint16 code;           /* evdev code */
    Sint32 value;
    Uint8  device_class;   /* SDLOP_DEVICE_KEYBOARD / MOUSE / OTHER */
    Uint8  from_evdev;     /* false: synthesised record */
} SDLOP_RawInputRecord;

typedef enum SDLOP_InputDeviceClass
{
    SDLOP_DEVICE_OTHER = 0,
    SDLOP_DEVICE_KEYBOARD,
    SDLOP_DEVICE_MOUSE
} SDLOP_InputDeviceClass;

#define SDLOP_RAW_QUEUE_SIZE 1024

/* Called by the input worker (any thread); lock-free. Returns false if full. */
bool SDLOP_PushRawInput(const SDLOP_RawInputRecord *record);
/* Called on the main thread; returns how many records were copied out. */
int SDLOP_DrainRawInput(SDLOP_RawInputRecord *out, int max_records);
/* True when the async evdev source is running and owns mouse/keyboard input. */
bool SDLOP_AsyncInputActive(void);

/* Translate everything the input sources produced into SDL events; called on the
   main thread from SDL_PumpEvents(). */
void SDLOP_PumpRawInput(void);
/* Allocate the ring (first use of any input source). */
void SDLOP_MaybeInitRawInput(void);
/* How many records were dropped because the ring was full. */
Uint64 SDLOP_RawInputOverruns(void);
void SDLOP_QuitTestInput(void);

/* evdev keycode (as delivered by /dev/input and by X11 keycodes minus 8) to SDL
   scancode. src/input/SDL_input.c owns the table. */
SDL_Scancode SDLOP_ScancodeFromEvdevKeycode(Uint32 evdev_code);
/* Text input for a key press: pushes SDL_EVENT_TEXT_INPUT when input is active.
   `evdev_code` (or 0) lets the platform key layout supply the text. */
void SDLOP_HandleTextInputForKey(SDL_Scancode scancode, SDL_Keycode key, Uint32 evdev_code,
                                 bool down, Uint64 timestamp);
/* Toggle one SDL_Keymod bit (used for num/caps/scroll lock events). */
void SDLOP_SetModStateBit(SDL_Keymod bit, bool down);

bool SDLOP_InitAsyncInput(void);
void SDLOP_QuitAsyncInput(void);

/* Named pipe test hook: SDLOP_TEST_INPUT=<path> replays text-format records
   through the very same path as real evdev events. Used by the test suite on
   machines (like CI containers) that have no /dev/input. */
bool SDLOP_InitTestInput(const char *path);

/* ------------------------------------------------------------------------- */
/* Video driver interface                                                    */
/* ------------------------------------------------------------------------- */

struct SDLOP_GLDriver;

typedef struct SDLOP_VideoDriver
{
    const char *name;

    /* Where platform mouse/keyboard events go: true when the platform backend
       delivers input events itself (no async evdev source running). */
    bool (*init)(void);
    void (*quit)(void);

    /* Displays */
    int  (*get_displays)(SDL_DisplayID *ids, int max_displays);
    bool (*get_display_bounds)(SDL_DisplayID display, SDL_Rect *bounds);
    bool (*get_display_usable_bounds)(SDL_DisplayID display, SDL_Rect *bounds);
    bool (*get_display_modes)(SDL_DisplayID display, SDL_DisplayMode **modes, int *count);
    bool (*get_display_mode)(SDL_DisplayID display, SDL_DisplayMode *mode);
    bool (*get_display_scale)(SDL_DisplayID display, float *scale);

    /* Windows */
    bool (*create_window)(SDL_Window *window);
    void (*destroy_window)(SDL_Window *window);
    bool (*set_window_title)(SDL_Window *window, const char *title);
    bool (*set_window_position)(SDL_Window *window, int x, int y);
    bool (*get_window_position)(SDL_Window *window, int *x, int *y);
    bool (*set_window_size)(SDL_Window *window, int w, int h);
    bool (*get_window_size)(SDL_Window *window, int *w, int *h);
    bool (*set_window_bordered)(SDL_Window *window, bool bordered);
    bool (*set_window_resizable)(SDL_Window *window, bool resizable);
    bool (*set_window_always_on_top)(SDL_Window *window, bool on_top);
    bool (*set_window_fullscreen)(SDL_Window *window, bool fullscreen);
    bool (*show_window)(SDL_Window *window);
    bool (*hide_window)(SDL_Window *window);
    bool (*raise_window)(SDL_Window *window);
    bool (*maximize_window)(SDL_Window *window);
    bool (*minimize_window)(SDL_Window *window);
    bool (*restore_window)(SDL_Window *window);
    bool (*set_window_opacity)(SDL_Window *window, float opacity);
    bool (*set_window_mouse_rect)(SDL_Window *window, const SDL_Rect *rect);
    bool (*set_window_grab)(SDL_Window *window, bool keyboard, bool mouse);
    bool (*set_window_modal)(SDL_Window *window, SDL_Window *parent);
    bool (*set_window_parent)(SDL_Window *window, SDL_Window *parent);
    bool (*set_window_focusable)(SDL_Window *window, bool focusable);
    bool (*flash_window)(SDL_Window *window, SDL_FlashOperation operation);
    bool (*set_window_icon)(SDL_Window *window, SDL_Surface *icon);
    bool (*set_window_hit_test)(SDL_Window *window, SDL_HitTest callback, void *callback_data);
    bool (*sync_window)(SDL_Window *window);

    /* Software presentation (SDL_GetWindowSurface / SDL_UpdateWindowSurface) */
    bool (*create_window_surface)(SDL_Window *window, SDL_Surface **surface);
    bool (*present_surface)(SDL_Window *window, const SDL_Rect *rects, int numrects);
    bool (*destroy_window_surface)(SDL_Window *window);

    /* Process platform events and translate them into SDL events. */
    void (*pump_events)(void);

    /* A file descriptor that becomes readable when the platform has work for
       SDL_WaitEvent(); polled together with the async-input wakeup fd. -1 when
       the backend has no such fd. To keep it simple the backend may return -1
       and rely on the caller's timeout. */
    int (*get_event_fd)(void);
    /* Called after poll() says the fd is readable: consume nothing, just let the
       next pump_events() pick the data up (Wayland needs this to re-arm). */
    void (*prepare_read)(void);
    /* Some backends have work to do at a time of their own choosing rather than
       when the compositor sends something - synthesizing key repeats is the
       example. Returns nanoseconds until that work is due, or -1 for "nothing
       scheduled". SDL_WaitEvent() then wakes up early enough to do it. */
    Sint64 (*get_event_timeout_ns)(void);

    /* Mouse and cursor plumbing */
    bool (*set_cursor)(SDL_Window *window, SDL_Cursor *cursor);
    bool (*show_cursor)(SDL_Window *window, bool show);
    bool (*warp_mouse)(SDL_Window *window, float x, float y);
    bool (*capture_mouse)(bool enabled);
    bool (*set_relative_mouse_mode)(SDL_Window *window, bool enabled);

    /* Vulkan */
    bool (*create_vulkan_surface)(SDL_Window *window, VkInstance instance,
                                  const struct VkAllocationCallbacks *allocator,
                                  VkSurfaceKHR *surface);
    const char *const *(*get_vulkan_instance_extensions)(Uint32 *count);

    /* Can this queue family present to the platform's display? (Wayland: the
       wl_display the connection is on; X11: the X display and its visual.) */
    bool (*vulkan_presentation_support)(VkInstance instance, VkPhysicalDevice physical_device,
                                        Uint32 queue_family_index);
    const struct SDLOP_GLDriver *gl;
} SDLOP_VideoDriver;

/* GL implementation for one windowing backend (GLX on X11, EGL on Wayland). */
typedef struct SDLOP_GLDriver
{
    const char *name;
    bool (*load_library)(const char *path);
    SDL_FunctionPointer (*get_proc_address)(const char *proc);
    void (*unload_library)(void);
    bool (*extension_supported)(const char *extension);
    bool (*set_attribute)(SDL_GLAttr attr, int value);
    bool (*get_attribute)(SDL_GLAttr attr, int *value);
    void (*reset_attributes)(void);
    SDL_GLContext (*create_context)(SDL_Window *window, SDL_GLContext shared);
    bool (*make_current)(SDL_Window *window, SDL_GLContext context);
    bool (*set_swap_interval)(int interval);
    int  (*get_swap_interval)(void);
    bool (*swap_window)(SDL_Window *window);
    void (*destroy_context)(SDL_GLContext context);
} SDLOP_GLDriver;

/* Driver registry (src/video/SDL_video.c) */
void SDLOP_RegisterVideoDriver(const SDLOP_VideoDriver *driver);
const SDLOP_VideoDriver *SDLOP_GetVideoDriver(void);
extern const SDLOP_VideoDriver SDLOP_WaylandVideoDriver;
extern const SDLOP_VideoDriver SDLOP_X11VideoDriver;
extern const SDLOP_VideoDriver SDLOP_OffscreenVideoDriver;
extern const SDLOP_GLDriver SDLOP_EGLDriver;

bool SDLOP_VideoInit(const char *driver_name);
void SDLOP_VideoQuit(void);
bool SDLOP_VideoIsReady(void);
void SDLOP_VideoPumpEvents(void);

/* Window bookkeeping (src/video/SDL_video.c) */
bool SDLOP_AddWindow(SDL_Window *window);
void SDLOP_RemoveWindow(SDL_Window *window);
SDL_Window *SDLOP_GetWindowFromIDInternal(SDL_WindowID id);
SDL_Window *SDLOP_GetKeyboardFocusWindow(void);
SDL_Window *SDLOP_GetMouseFocusWindow(void);
void SDLOP_SetMouseFocusWindow(SDL_Window *window);
void SDLOP_UpdateWindowDisplay(SDL_Window *window, SDL_DisplayID display);
/* Report a window as moved onto `display` at (x, y): used by the Wayland
   backend, where the compositor decides the position and the outputs a surface
   is shown on (wl_surface.enter/leave) are the only authority on the display. */
void SDLOP_OnWindowMovedOnDisplay(SDL_Window *window, SDL_DisplayID display, int x, int y);
void SDLOP_WindowIterate(void (*fn)(SDL_Window *window, void *userdata), void *userdata);
void SDLOP_OnWindowResized(SDL_Window *window, int w, int h);
void SDLOP_OnWindowPixelSizeChanged(SDL_Window *window, int w, int h);
void SDLOP_OnWindowShown(SDL_Window *window, bool shown);
void SDLOP_OnWindowClosed(SDL_Window *window);
void SDLOP_OnWindowFocusGained(SDL_Window *window);
void SDLOP_OnWindowFocusLost(SDL_Window *window);
void SDLOP_OnWindowMoved(SDL_Window *window, int x, int y);
void SDLOP_OnWindowDisplayScaleChanged(SDL_Window *window, float scale);
void SDLOP_OnWindowOccluded(SDL_Window *window, bool occluded);
void SDLOP_OnWindowMouseEnter(SDL_Window *window);
void SDLOP_OnWindowMouseLeave(SDL_Window *window);
void SDLOP_OnWindowMaximized(SDL_Window *window, bool maximized);
void SDLOP_OnWindowMinimized(SDL_Window *window, bool minimized);
void SDLOP_OnWindowFullscreenChanged(SDL_Window *window, bool fullscreen);
void SDLOP_OnWindowExposed(SDL_Window *window);
bool SDLOP_WindowHasFocus(SDL_Window *window);
/* The window an input event with no explicit target belongs to (mouse focus,
   then keyboard focus, then the only window if there is exactly one). */
SDL_Window *SDLOP_InputTargetWindow(void);
bool SDLOP_RelativeMouseModeActive(void);
void SDLOP_SetRelativeMouseMode(bool enabled);
bool SDLOP_MouseCaptureActive(void);
void SDLOP_SetMouseCapture(bool enabled);
const char *SDLOP_CurrentVideoDriverName(void);

/* Displays (src/video/SDL_video.c) */
struct SDLOP_Display;
struct SDLOP_Display *SDLOP_AddDisplay(SDL_DisplayID id);
/* Drop a display the backend has lost (a Wayland output unplugged, an X11 screen
   disconnected). Windows that were on it are re-homed to another display, and
   SDL_EVENT_DISPLAY_REMOVED is queued once the initial enumeration is over. */
bool SDLOP_RemoveDisplay(SDL_DisplayID id);
/* Called when the backend has finished its initial enumeration: displays found
   from here on are hotplug and produce SDL_EVENT_DISPLAY_ADDED. */
void SDLOP_MarkDisplayEnumerationDone(void);
struct SDLOP_Display *SDLOP_GetDisplay(SDL_DisplayID id);
struct SDLOP_Display *SDLOP_GetDisplayForPoint(const SDL_Point *point);
struct SDLOP_Display *SDLOP_GetPrimaryDisplayInternal(void);
void SDLOP_ResetDisplays(void);
void SDLOP_SetDisplayBounds(struct SDLOP_Display *display, const SDL_Rect *bounds);
void SDLOP_SetDisplayName(struct SDLOP_Display *display, const char *name);
void SDLOP_SetDisplayScale(struct SDLOP_Display *display, float scale);
void SDLOP_SetDisplayContentScale(struct SDLOP_Display *display, float scale);
/* The scale a window renders at: like SDL3, a window stays at 1x unless it asked
   for SDL_WINDOW_HIGH_PIXEL_DENSITY or the backend was told to scale to the
   display (SDL_HINT_VIDEO_WAYLAND_SCALE_TO_DISPLAY). */
void SDLOP_UpdateWindowScaleFactor(SDL_Window *window);
bool SDLOP_ScaleToDisplayEnabled(void);
void SDLOP_SetScaleToDisplay(bool enabled);
void SDLOP_AddDisplayMode(struct SDLOP_Display *display, const SDL_DisplayMode *mode);
void SDLOP_ClearDisplayModes(struct SDLOP_Display *display);
void SDLOP_SetDisplayCurrentMode(struct SDLOP_Display *display, const SDL_DisplayMode *mode);

/* Window surfaces (src/video/SDL_surface_window.c) */
bool SDLOP_CreateWindowSurface(SDL_Window *window, int w, int h, SDL_PixelFormat format);
void SDLOP_DestroyWindowSurface(SDL_Window *window);
/* Keep the window surface in step with the window's pixel size: SDL3 hands the
   same surface back, resized, after the window changed size or scale. */
void SDLOP_ResizeWindowSurface(SDL_Window *window, int w, int h);

/* Vulkan (src/video/SDL_vulkan.c): the loader is dlopen()ed on demand. */
bool SDLOP_VulkanLoad(const char *path);
void SDLOP_VulkanUnload(void);
void SDLOP_VulkanCleanup(void);
/* vkGetInstanceProcAddr of the loaded loader, and the loader's result codes as
   text (used by the backends, which own their platform's entry points). */
SDL_FunctionPointer SDLOP_VulkanGetInstanceProc(VkInstance instance, const char *name);
const char *SDLOP_VulkanResultString(int result);

/* Native handles the GL driver needs (implemented by the video backends). */
void *sdlop_wl_display_handle(void);
void *sdlop_wl_egl_window_create(SDL_Window *window);
unsigned long sdlop_x11_display_handle(void);
unsigned long sdlop_x11_window_handle(SDL_Window *window);

#ifdef SDLOP_HAVE_EGL
#ifdef SDLOP_HAVE_X11
/* The X visual an SDL_WINDOW_OPENGL window must be created with, so the X window
   matches the EGL config (implemented in src/gl/SDL_egl.c). */
bool sdlop_egl_x11_visual(unsigned long *visual_id, int *depth);
#endif
#endif

/* Wakeup descriptor: the async input worker rings this so a thread blocked in
   SDL_WaitEvent() notices new input immediately instead of on a timer. */
bool SDLOP_InitWakeup(void);
void SDLOP_QuitWakeup(void);
int SDLOP_GetWakeupFD(void);
/* SIGINT/SIGTERM -> SDL_EVENT_QUIT (SDL_HINT_NO_SIGNAL_HANDLERS turns it off). */
void SDLOP_InstallSignalHandlers(void);
void SDLOP_QuitSignalHandlers(void);
void SDLOP_QueueSignalQuit(void);
void SDLOP_SignalWakeup(void);

/* Block until the platform has events, the wakeup fd is rung, or timeoutNS
   elapses (-1 blocks until something happens). Returns true if something is
   ready. */
bool SDLOP_WaitPlatformEvents(Sint64 timeoutNS);

/* Deliver events for one key/text-input session: implemented in SDL_keyboard.c */
bool SDLOP_TextInputActiveForWindow(SDL_Window *window);

/* Cursor helpers (src/video/SDL_video.c) */
const char *SDLOP_VideoDriverName(void);
struct SDL_Cursor *SDLOP_CreateCursorFromData(const Uint8 *data, const Uint8 *mask, int w, int h,
                                              int hot_x, int hot_y);
SDL_Cursor *SDLOP_GetActiveCursor(void);

/* Async-input-aware helpers used by the input module */
void SDLOP_OnDeviceAdded(Uint32 device_id, SDLOP_InputDeviceClass cls, const char *name);
void SDLOP_OnDeviceRemoved(Uint32 device_id);

/* ------------------------------------------------------------------------- */
/* SDL_Window internals                                                      */
/* ------------------------------------------------------------------------- */

/* Wayland shares a double buffering scheme between the driver and the window;
   the fields are void* here so this header needs no Wayland includes. */
#define SDLOP_WAYLAND_NUM_BUFFERS 2

/* Opaque Wayland objects, declared (not defined) so this header needs no Wayland
   includes and stays usable by the other backends. */
struct wl_surface;
struct wl_buffer;
struct wl_callback;
struct wl_egl_window;
struct wp_viewport;
struct zwp_locked_pointer_v1;
struct zwp_confined_pointer_v1;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_popup;

typedef struct SDLOP_WaylandBufferSlot
{
    struct wl_buffer *buffer;
    void *pixels;
    size_t size;
    bool busy;
    int w, h;
} SDLOP_WaylandBufferSlot;

/* SDLop defines the opaque SDL_Window itself (upstream leaves it opaque to
   applications, which is what makes this possible). */
struct SDL_Window
{
    SDL_WindowID id;
    SDL_WindowFlags flags;
    SDL_PropertiesID props;

    char *title;
    int x, y;                 /* window position, in desktop coordinates */
    int w, h;                 /* window size, in desktop (logical) units */
    int pixel_w, pixel_h;     /* drawable size in pixels */
    float scale_factor;       /* native scale factor of the display it is on */
    float display_scale;      /* scale the window renders at (see SDLOP_UpdateWindowScaleFactor) */
    SDL_DisplayID display;
    float opacity;
    SDL_Rect mouse_rect;
    bool mouse_rect_set;
    int min_w, min_h, max_w, max_h;
    float min_aspect, max_aspect;
    SDL_DisplayMode fullscreen_mode;
    bool fullscreen_mode_set;

    SDL_Window *parent;
    bool modal;

    /* software presentation */
    SDL_Surface *surface;
    bool surface_dirty;

    /* hit test */
    SDL_HitTest hit_test;
    void *hit_test_data;

    /* backend specific */
    union
    {
        struct
        {
            unsigned long window;      /* X11 Window */
            unsigned long colormap;    /* Colormap we created for a GL visual */
            void *gc;                  /* Xlib GC used for presenting */
            void *image;               /* XImage the window is presented from */
            void *shm;                 /* XShmSegmentInfo when MIT-SHM is used */
            void *buffer;              /* our own pixel buffer */
            int buffer_pitch;
            bool is_popup;
        } x11;
        struct
        {
            struct wl_surface *wl_surface;
            struct xdg_surface *xdg_surface;
            struct xdg_toplevel *xdg_toplevel;
            struct xdg_popup *xdg_popup;
            struct wl_egl_window *egl_window;   /* when GL is used */
            struct wp_viewport *viewport;       /* HiDPI scaling */
            SDLOP_WaylandBufferSlot buffers[SDLOP_WAYLAND_NUM_BUFFERS];
            int num_buffers;
            struct wl_callback *frame_callback;   /* re-armed after every present */
            struct SDLOP_WaylandOutput *outputs[8];  /* wl_surface.enter(), oldest first */
            int num_outputs;
            struct zwp_locked_pointer_v1 *locked_pointer;      /* relative mouse mode */
            struct zwp_confined_pointer_v1 *confined_pointer;  /* mouse grab / rect */
            bool configured;
            bool pending_resize;
            int pending_w, pending_h;
        } wayland;
    } driver;

    struct SDL_Window *next;
    struct SDL_Window *prev;
};

/* ------------------------------------------------------------------------- */
/* Cursor / display internals                                                */
/* ------------------------------------------------------------------------- */

struct SDL_Cursor
{
    SDL_SystemCursor system;      /* SDL_SYSTEM_CURSOR_DEFAULT when custom */
    Uint8 *data;                  /* 32-bit ARGB, w*h, NULL for system cursors */
    int w, h, hot_x, hot_y;
    void *backend;                /* X11 Cursor / wl_cursor* */
    struct SDL_Cursor *next;
};

struct SDL_DisplayModeData
{
    int unused;
};

struct SDLOP_Display
{
    SDL_DisplayID id;
    char *name;
    SDL_Rect bounds;
    SDL_Rect usable;
    float scale;                  /* native scale factor of the output (wl_output.scale) */
    float content_scale;          /* what SDL_GetDisplayContentScale() reports */
    SDL_PropertiesID props;
    SDL_DisplayOrientation orientation;
    SDL_DisplayMode current;
    SDL_DisplayMode *modes;
    int num_modes;
    SDL_DisplayModeData *modedata;
    void *driver_data;            /* wl_output* or X11 screen number */
    struct SDLOP_Display *next;   /* sorted by id */
    bool primary;
};

/* ------------------------------------------------------------------------- */
/* Platform helpers (implemented per backend)                                 */
/* ------------------------------------------------------------------------- */

/* The input module needs to know whether the platform's own pointer is being
   used (so that relative evdev motion and absolute platform motion don't fight). */
void SDLOP_SetPlatformInputActive(bool active);

#ifdef __cplusplus
}
#endif

#endif /* SDLOP_INTERNAL_H */
