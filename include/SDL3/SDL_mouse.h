/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Mouse handling (subset of <SDL3/SDL_mouse.h>, identical
  names/values/signatures).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_mouse_h_
#define SDL_mouse_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_video.h>

/** Mouse button constants (identical to SDL3). */
#define SDL_BUTTON_LEFT     1
#define SDL_BUTTON_MIDDLE   2
#define SDL_BUTTON_RIGHT    3
#define SDL_BUTTON_X1       4
#define SDL_BUTTON_X2       5

#define SDL_BUTTON_MASK(X)  (1u << ((X)-1))
#define SDL_BUTTON_LMASK    SDL_BUTTON_MASK(SDL_BUTTON_LEFT)
#define SDL_BUTTON_MMASK    SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)
#define SDL_BUTTON_RMASK    SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)
#define SDL_BUTTON_X1MASK   SDL_BUTTON_MASK(SDL_BUTTON_X1)
#define SDL_BUTTON_X2MASK   SDL_BUTTON_MASK(SDL_BUTTON_X2)

/**
 * Scroll direction types for the Scroll event.
 */
typedef enum SDL_MouseWheelDirection
{
    SDL_MOUSEWHEEL_NORMAL,
    SDL_MOUSEWHEEL_FLIPPED
} SDL_MouseWheelDirection;

/**
 * Get the window which currently has mouse focus.
 */
extern SDL_Window *SDL_GetMouseFocus(void);

/**
 * Retrieve the current state of the mouse.
 *
 * The current button state is returned as a button bitmask, and x/y are set
 * to the mouse coordinates relative to the focus window (window-relative),
 * in pixels. Outside a focused window the coordinates are unchanged.
 */
extern SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y);

/**
 * Retrieve the relative state of the mouse.
 *
 * x/y receive the accumulated mouse motion since the last call to this
 * function. The current button state is returned as a button bitmask.
 */
extern SDL_MouseButtonFlags SDL_GetRelativeMouseState(float *x, float *y);

/**
 * Set relative mouse mode for a window.
 *
 * While enabled the cursor is hidden and mouse motion is reported as
 * relative deltas (SDL_EVENT_MOUSE_MOTION xrel/yrel). SDLop sources these
 * from raw evdev when available, otherwise from the window system.
 */
extern bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled);

/**
 * Query whether relative mouse mode is enabled for a window.
 */
extern bool SDL_GetWindowRelativeMouseMode(SDL_Window *window);

#endif /* SDL_mouse_h_ */
