/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Display and window management (subset of <SDL3/SDL_video.h>, identical
  names/values/signatures).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_video_h_
#define SDL_video_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>

/**
 * The type used to identify a window (opaque window handle).
 */
typedef struct SDL_Window SDL_Window;

/** Window flags (identical values to SDL3). */
typedef Uint64 SDL_WindowFlags;

#define SDL_WINDOW_FULLSCREEN           SDL_UINT64_C(0x0000000000000001)
#define SDL_WINDOW_OPENGL               SDL_UINT64_C(0x0000000000000002) /**< accepted, ignored */
#define SDL_WINDOW_OCCLUDED             SDL_UINT64_C(0x0000000000000004)
#define SDL_WINDOW_HIDDEN               SDL_UINT64_C(0x0000000000000008)
#define SDL_WINDOW_BORDERLESS           SDL_UINT64_C(0x0000000000000010)
#define SDL_WINDOW_RESIZABLE            SDL_UINT64_C(0x0000000000000020)
#define SDL_WINDOW_MINIMIZED            SDL_UINT64_C(0x0000000000000040)
#define SDL_WINDOW_MAXIMIZED            SDL_UINT64_C(0x0000000000000080)
#define SDL_WINDOW_MOUSE_GRABBED        SDL_UINT64_C(0x0000000000000100)
#define SDL_WINDOW_INPUT_FOCUS          SDL_UINT64_C(0x0000000000000200)
#define SDL_WINDOW_MOUSE_FOCUS          SDL_UINT64_C(0x0000000000000400)
#define SDL_WINDOW_EXTERNAL            SDL_UINT64_C(0x0000000000000800)
#define SDL_WINDOW_MODAL                SDL_UINT64_C(0x0000000000001000)
#define SDL_WINDOW_HIGH_PIXEL_DENSITY   SDL_UINT64_C(0x0000000000002000)
#define SDL_WINDOW_MOUSE_CAPTURE        SDL_UINT64_C(0x0000000000004000)
#define SDL_WINDOW_MOUSE_RELATIVE_MODE  SDL_UINT64_C(0x0000000000008000)
#define SDL_WINDOW_ALWAYS_ON_TOP        SDL_UINT64_C(0x0000000000010000)
#define SDL_WINDOW_UTILITY              SDL_UINT64_C(0x0000000000020000)
#define SDL_WINDOW_TOOLTIP              SDL_UINT64_C(0x0000000000040000)
#define SDL_WINDOW_POPUP_MENU           SDL_UINT64_C(0x0000000000080000)
#define SDL_WINDOW_KEYBOARD_GRABBED     SDL_UINT64_C(0x0000000000100000)
#define SDL_WINDOW_VULKAN               SDL_UINT64_C(0x0000000010000000)
#define SDL_WINDOW_METAL                SDL_UINT64_C(0x0000000020000000)
#define SDL_WINDOW_TRANSPARENT          SDL_UINT64_C(0x0000000040000000)
#define SDL_WINDOW_NOT_FOCUSABLE        SDL_UINT64_C(0x0000000080000000)

/** \name Window position constants (identical values to SDL3) */
/* @{ */
#define SDL_WINDOWPOS_UNDEFINED_MASK    0x1FFF0000u
#define SDL_WINDOWPOS_UNDEFINED_DISPLAY(X)  (SDL_WINDOWPOS_UNDEFINED_MASK|(X))
#define SDL_WINDOWPOS_UNDEFINED         SDL_WINDOWPOS_UNDEFINED_DISPLAY(0)
#define SDL_WINDOWPOS_ISUNDEFINED(X)    (((X)&0xFFFF0000) == SDL_WINDOWPOS_UNDEFINED_MASK)

#define SDL_WINDOWPOS_CENTERED_MASK    0x2FFF0000u
#define SDL_WINDOWPOS_CENTERED_DISPLAY(X)  (SDL_WINDOWPOS_CENTERED_MASK|(X))
#define SDL_WINDOWPOS_CENTERED         SDL_WINDOWPOS_CENTERED_DISPLAY(0)
#define SDL_WINDOWPOS_ISCENTERED(X)    (((X)&0xFFFF0000) == SDL_WINDOWPOS_CENTERED_MASK)
/* @} */

/**
 * Create a window with the specified dimensions and flags.
 *
 * \param title the title of the window (UTF-8, may be NULL).
 * \param w the width of the window.
 * \param h the height of the window.
 * \param flags 0, or one or more SDL_WindowFlags OR'd together.
 * \returns the window that was created or NULL on failure.
 */
extern SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags);

/**
 * Destroy a window.
 */
extern void SDL_DestroyWindow(SDL_Window *window);

/**
 * Get the numeric ID of a window.
 */
extern SDL_WindowID SDL_GetWindowID(SDL_Window *window);

/**
 * Get a window from a stored ID.
 */
extern SDL_Window *SDL_GetWindowFromID(SDL_WindowID id);

/**
 * Get the window flags.
 */
extern SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window);

/**
 * Set the title of a window (UTF-8).
 *
 * \returns true on success or false on failure.
 */
extern bool SDL_SetWindowTitle(SDL_Window *window, const char *title);

/**
 * Get the title of a window (UTF-8). Returns "" if there is no title.
 */
extern const char *SDL_GetWindowTitle(SDL_Window *window);

/**
 * Set the size of a window's client area.
 */
extern bool SDL_SetWindowSize(SDL_Window *window, int w, int h);

/**
 * Get the size of a window's client area.
 */
extern bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h);

/**
 * Request that the size of a window's client area be set (alias of
 * SDL_SetWindowSize for API compatibility).
 */
extern bool SDL_SetWindowPosition(SDL_Window *window, int x, int y);

/**
 * Get the position of a window, if the platform supports it.
 */
extern bool SDL_GetWindowPosition(SDL_Window *window, int *x, int *y);

/**
 * Show a window.
 */
extern bool SDL_ShowWindow(SDL_Window *window);

/**
 * Hide a window.
 */
extern bool SDL_HideWindow(SDL_Window *window);

/**
 * Request that a window be minimized.
 */
extern bool SDL_MinimizeWindow(SDL_Window *window);

/**
 * Request that a window be maximized.
 */
extern bool SDL_MaximizeWindow(SDL_Window *window);

/**
 * Request that a window be restored to normal size/position.
 */
extern bool SDL_RestoreWindow(SDL_Window *window);

/**
 * Request that a window's fullscreen state be changed.
 */
extern bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen);

/**
 * Raise a window above other windows and set the input focus.
 */
extern bool SDL_RaiseWindow(SDL_Window *window);

/**
 * Get the name of the currently initialized video driver
 * ("wayland", "dummy", ...).
 */
extern const char *SDL_GetCurrentVideoDriver(void);

/**
 * Get the number of video drivers compiled into SDLop.
 */
extern int SDL_GetNumVideoDrivers(void);

/**
 * Get the name of a built in video driver.
 */
extern const char *SDL_GetVideoDriver(int index);

#endif /* SDL_video_h_ */
