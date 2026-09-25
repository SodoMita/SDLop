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
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_rect.h>

/**
 * The type used to identify a window (opaque window handle).
 */
typedef struct SDL_Window SDL_Window;

/* Forward declaration for the software-rendering surface API. */
typedef struct SDL_Surface SDL_Surface;

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

/* ------------------------------------------------------------------ */
/* Software rendering (window surface)                                 */
/* ------------------------------------------------------------------ */

/**
 * Get the SDL surface associated with the window (creates it on first
 * call). On Wayland this maps directly onto the window's shared-memory
 * buffer - zero copy.
 *
 * The surface is owned by the window; do not SDL_DestroySurface() it. It
 * follows the window size (use SDL_SetWindowSize / handle
 * SDL_EVENT_WINDOW_RESIZED and re-query pitch/pixels).
 *
 * \returns the surface or NULL on failure.
 */
extern SDL_Surface *SDL_GetWindowSurface(SDL_Window *window);

/**
 * Copy the window surface to the screen (commits the buffer).
 */
extern bool SDL_UpdateWindowSurface(SDL_Window *window);

/**
 * Copy areas of the window surface to the screen.
 */
extern bool SDL_UpdateWindowSurfaceRects(SDL_Window *window, const SDL_Rect *rects, int numrects);

/**
 * Return whether the window has a surface associated with it.
 */
extern bool SDL_WindowHasSurface(SDL_Window *window);

/**
 * Destroy the surface associated with the window.
 */
extern bool SDL_DestroyWindowSurface(SDL_Window *window);

/* ------------------------------------------------------------------ */
/* OpenGL (EGL on Wayland; llvmpipe provides the software rasterizer)  */
/* ------------------------------------------------------------------ */

/** Opaque type for an OpenGL context. */
typedef struct SDL_GLContext_t *SDL_GLContext;

/** Attributes for SDL_GL_SetAttribute()/SDL_GL_GetAttribute()
 * (identical order/values to SDL3). */
typedef enum SDL_GLAttr
{
    SDL_GL_RED_SIZE,
    SDL_GL_GREEN_SIZE,
    SDL_GL_BLUE_SIZE,
    SDL_GL_ALPHA_SIZE,
    SDL_GL_BUFFER_SIZE,
    SDL_GL_DOUBLEBUFFER,
    SDL_GL_DEPTH_SIZE,
    SDL_GL_STENCIL_SIZE,
    SDL_GL_ACCUM_RED_SIZE,
    SDL_GL_ACCUM_GREEN_SIZE,
    SDL_GL_ACCUM_BLUE_SIZE,
    SDL_GL_ACCUM_ALPHA_SIZE,
    SDL_GL_STEREO,
    SDL_GL_MULTISAMPLEBUFFERS,
    SDL_GL_MULTISAMPLESAMPLES,
    SDL_GL_ACCELERATED_VISUAL,
    SDL_GL_RETAINED_BACKING,
    SDL_GL_CONTEXT_MAJOR_VERSION,
    SDL_GL_CONTEXT_MINOR_VERSION,
    SDL_GL_CONTEXT_FLAGS,
    SDL_GL_CONTEXT_PROFILE_MASK,
    SDL_GL_SHARE_WITH_CURRENT_CONTEXT,
    SDL_GL_FRAMEBUFFER_SRGB_CAPABLE,
    SDL_GL_CONTEXT_RELEASE_BEHAVIOR,
    SDL_GL_CONTEXT_RESET_NOTIFICATION,
    SDL_GL_CONTEXT_NO_ERROR,
    SDL_GL_FLOATBUFFERS,
    SDL_GL_EGL_PLATFORM
} SDL_GLAttr;

#define SDL_GL_CONTEXT_PROFILE_CORE           0x0001
#define SDL_GL_CONTEXT_PROFILE_COMPATIBILITY  0x0002
#define SDL_GL_CONTEXT_PROFILE_ES             0x0004 /**< GLX_CONTEXT_ES2_PROFILE_BIT_EXT */

#define SDL_GL_CONTEXT_DEBUG_FLAG              0x0001
#define SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG 0x0002
#define SDL_GL_CONTEXT_ROBUST_ACCESS_FLAG      0x0004
#define SDL_GL_CONTEXT_RESET_ISOLATION_FLAG    0x0008

/**
 * Select the OpenGL library. SDLop loads EGL dynamically when the video
 * driver creates its first context; path is currently accepted for API
 * compatibility.
 */
extern bool SDL_GL_LoadLibrary(const char *path);

/**
 * Get an OpenGL function by name (EGL/GL driver entry points).
 */
extern SDL_FunctionPointer SDL_GL_GetProcAddress(const char *proc);

/**
 * Unload the OpenGL library. SDLop releases its dynamically loaded EGL
 * symbols when the video subsystem shuts down.
 */
extern void SDL_GL_UnloadLibrary(void);

/**
 * Reset all OpenGL attributes to their default values.
 */
extern void SDL_GL_ResetAttributes(void);

/**
 * Set an OpenGL window attribute before context creation.
 */
extern bool SDL_GL_SetAttribute(SDL_GLAttr attr, int value);

/**
 * Get the actual value for an attribute from the current context.
 */
extern bool SDL_GL_GetAttribute(SDL_GLAttr attr, int *value);

/**
 * Create an OpenGL context for the given window and make it current.
 */
extern SDL_GLContext SDL_GL_CreateContext(SDL_Window *window);

/**
 * Set up an OpenGL context for rendering into an OpenGL window.
 */
extern bool SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context);

/**
 * Get the currently active OpenGL window.
 */
extern SDL_Window *SDL_GL_GetCurrentWindow(void);

/**
 * Get the currently active OpenGL context.
 */
extern SDL_GLContext SDL_GL_GetCurrentContext(void);

/**
 * Set the swap interval for the current OpenGL context (0 = no vsync).
 */
extern bool SDL_GL_SetSwapInterval(int interval);

/**
 * Get the swap interval for the current OpenGL context.
 */
extern bool SDL_GL_GetSwapInterval(int *interval);

/**
 * Update a window with OpenGL rendering (presents to Wayland).
 */
extern bool SDL_GL_SwapWindow(SDL_Window *window);

/**
 * Delete an OpenGL context.
 */
extern void SDL_GL_DestroyContext(SDL_GLContext context);

/**
 * Get the content display scale relative to a window's pixel size.
 *
 * \\since This function is available since SDL 3.2.0.
 */
extern SDL_DECLSPEC float SDLCALL SDL_GetWindowDisplayScale(SDL_Window *window);


/* ---- API-surface parity types (SDL3 3.2.10 layouts; declarations only,
 * the lean core does not implement display enumeration) ---- */

typedef Uint32 SDL_DisplayID;

typedef enum SDL_SystemTheme
{
    SDL_SYSTEM_THEME_UNKNOWN,   /**< Unknown system theme */
    SDL_SYSTEM_THEME_LIGHT,     /**< Light colored system theme */
    SDL_SYSTEM_THEME_DARK       /**< Dark colored system theme */
} SDL_SystemTheme;

typedef enum SDL_DisplayOrientation
{
    SDL_ORIENTATION_UNKNOWN,            /**< The display orientation can't be determined */
    SDL_ORIENTATION_LANDSCAPE,          /**< The display is in landscape mode, with the right side up, relative to portrait mode */
    SDL_ORIENTATION_LANDSCAPE_FLIPPED,  /**< The display is in landscape mode, with the left side up, relative to portrait mode */
    SDL_ORIENTATION_PORTRAIT,           /**< The display is in portrait mode */
    SDL_ORIENTATION_PORTRAIT_FLIPPED    /**< The display is in portrait mode, upside down */
} SDL_DisplayOrientation;

typedef enum SDL_HitTestResult
{
    SDL_HITTEST_NORMAL,             /**< Region is normal. No special properties. */
    SDL_HITTEST_DRAGGABLE,          /**< Region can drag entire window. */
    SDL_HITTEST_RESIZE_TOPLEFT,     /**< Region is the resizable top-left corner border. */
    SDL_HITTEST_RESIZE_TOP,         /**< Region is the resizable top border. */
    SDL_HITTEST_RESIZE_TOPRIGHT,    /**< Region is the resizable top-right corner border. */
    SDL_HITTEST_RESIZE_RIGHT,       /**< Region is the resizable right border. */
    SDL_HITTEST_RESIZE_BOTTOMRIGHT, /**< Region is the resizable bottom-right corner border. */
    SDL_HITTEST_RESIZE_BOTTOM,      /**< Region is the resizable bottom border. */
    SDL_HITTEST_RESIZE_BOTTOMLEFT,  /**< Region is the resizable bottom-left corner border. */
    SDL_HITTEST_RESIZE_LEFT         /**< Region is the resizable left border. */
} SDL_HitTestResult;

typedef struct SDL_DisplayModeData SDL_DisplayModeData;

typedef struct SDL_DisplayMode
{
    SDL_DisplayID displayID;        /**< the display this mode is associated with */
    SDL_PixelFormat format;         /**< pixel format */
    int w;                          /**< width */
    int h;                          /**< height */
    float pixel_density;            /**< scale converting size to pixels */
    float refresh_rate;             /**< refresh rate (or 0.0f for unspecified) */
    int refresh_rate_numerator;     /**< precise refresh rate numerator (or 0 for unspecified) */
    int refresh_rate_denominator;   /**< precise refresh rate denominator */

    SDL_DisplayModeData *internal;  /**< Private */

} SDL_DisplayMode;

#endif /* SDL_video_h_ */
