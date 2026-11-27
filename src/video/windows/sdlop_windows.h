/*
  SDLop - private definitions for the Win32 video driver (and its WGL and
  Vulkan WSI halves).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_windows_h_
#define sdlop_windows_h_

#include "internal/sdlop_internal.h"

#ifdef _WIN32

#include <windows.h>
#include <stdint.h>

/* Per-window Win32 state (SDL_Window::driverdata). */
typedef struct SDLop_Win32WindowData
{
    HWND hwnd;
    HDC hdc;            /* the window's own DC (CS_OWNDC), stable for its life */
    HDC dc;             /* memory DC holding the software DIB */
    HBITMAP bitmap;     /* the DIB itself */
    void *pixels;       /* its bits, as an SDL surface's pixel pointer */
    SDL_Surface *surface; /* SDL_CreateSurfaceFrom over `pixels` */
    bool surface_dirty;   /* resized under the caller: rebuild before blit */

    SDLOP_MUTEX_T surfacelock; /* UpdateWindowFramebuffer vs WM_SIZE */

    Uint32 dpi;
    bool mouse_leave_armed; /* TrackMouseEvent(TME_LEAVE) is outstanding */

    /* fullscreen bookkeeping */
    bool is_fullscreen;   /* windowed_* below only describe a windowed window */
    RECT windowed_rect;
    DWORD windowed_style;
    DWORD windowed_exstyle;

    /* WM_GETMINMAXINFO */
    bool has_minimum_size;
    bool has_maximum_size;
    int minimum_w, minimum_h;
    int maximum_w, maximum_h;

    HICON icon;

    /* The clip rect we set, so we only release our own (SDL3 keeps the same
     * bookkeeping: cursor_clipped_rect). Zero means "not clipping". */
    RECT cursor_clipped_rect;

    /* WGL (sdlop_windows_gl.c) */
    HGLRC gl_context;      /* the implicit context used for wglGetProcAddress */
    bool gl_context_owned; /* created here, must be deleted here */

    struct SDLop_Win32WindowData *next;
} SDLop_Win32WindowData;

typedef struct SDLop_Win32Monitor
{
    wchar_t device[CCHDEVICENAME];
    RECT rect;           /* desktop coordinates */
    int refresh_hz;
    SDL_DisplayID displayID;
} SDLop_Win32Monitor;

typedef struct SDLop_Win32State
{
    bool class_registered;
    int window_count;
    bool want_relative_mouse;
    SDLop_Win32Monitor *monitors;
    int monitor_count;

    /* raw input (SDL_HINT_WINDOWS_RAW_KEYBOARD and friends). The registration
     * is process-global even though it is triggered per window, so the state
     * lives here rather than on the window. */
    bool raw_keyboard_enabled;
    bool raw_keyboard_nohotkeys;
    bool raw_keyboard_inputsink;
    bool raw_keyboard_registered;
    DWORD raw_keyboard_flags; /* the flags last successfully registered */
    HWND raw_keyboard_target; /* what the registration's hwndTarget is */
    HWND raw_input_hwnd;      /* message-only window, only for RIDEV_INPUTSINK */
    bool prev_keyboard_registered; /* what the keyboard registration was
                                    * before the raw input worker took it over */
    bool pending_e1_sequence; /* first key of a Ctrl+{key} pair */
} SDLop_Win32State;

extern SDLop_Win32State sdlop_win32;
extern SDLop_VideoDevice SDLop_windows_device;

/* sdlop_windows.c */
Uint64 SDLOP_Win32Timestamp(void);
bool SDLOP_Win32GetClientRectInPixels(SDL_Window *window, RECT *rc);
void SDLOP_Win32AdjustWindowRect(SDL_Window *window, RECT *rc);

/* sdlop_windows_gl.c (WGL) */
void *SDLOP_Win32GLCreateContext(SDLop_VideoDevice *device, SDL_Window *window);
bool SDLOP_Win32GLMakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context);
bool SDLOP_Win32GLSwapBuffers(SDLop_VideoDevice *device, SDL_Window *window);
void SDLOP_Win32GLDeleteContext(SDLop_VideoDevice *device, void *context);
SDL_FunctionPointer SDLOP_Win32GLGetProcAddress(SDLop_VideoDevice *device, const char *proc);
bool SDLOP_Win32GLSetSwapInterval(SDLop_VideoDevice *device, int interval);
bool SDLOP_Win32GLGetSwapInterval(SDLop_VideoDevice *device, int *interval);
/* Releases the process-lifetime bootstrap context that wglGetProcAddress
 * needs. Called from win32_Quit, before the window class is unregistered. */
void SDLOP_Win32GLQuit(void);

/* sdlop_windows_ime.c: composition events. Returns true when the message was
 * an IME message and has been handled (the window procedure should stop). */
bool SDLOP_Win32IMEHandleMessage(SDL_Window *window, UINT msg, WPARAM wparam, LPARAM lparam);

/* sdlop_windows_vulkan.c */
bool SDLOP_Win32VulkanCreateSurface(SDLop_VideoDevice *device, SDL_Window *window,
                                   void *instance, const void *allocator, Uint64 *surface);
const char *const *SDLOP_Win32VulkanGetInstanceExtensions(SDLop_VideoDevice *device, Uint32 *count);
bool SDLOP_Win32VulkanGetPresentationSupport(SDLop_VideoDevice *device, void *instance,
                                             void *physical_device, Uint32 queue_family);

#endif /* _WIN32 */

#endif /* sdlop_windows_h_ */
