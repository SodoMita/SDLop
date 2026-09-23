/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Keyboard handling (subset of <SDL3/SDL_keyboard.h>, identical
  names/values/signatures).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_keyboard_h_
#define SDL_keyboard_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_scancode.h>

/**
 * Get a snapshot of the current keyboard state.
 *
 * The returned array is indexed by SDL_Scancode values and is valid until
 * the next call to SDL_GetKeyboardState(). Values are true while the key
 * is pressed.
 *
 * \param numkeys if non-NULL, receives the length of the returned array.
 * \returns a pointer to an array of key states.
 */
extern const bool *SDL_GetKeyboardState(int *numkeys);

/**
 * Get the current state of a key on the keyboard.
 */
extern bool SDL_GetKeyState(SDL_Scancode scancode);

/**
 * Release all keys (useful when losing/regaining focus).
 */
extern void SDL_ResetKeyboard(void);

/**
 * Get the current key modifier state for the keyboard.
 */
extern SDL_Keymod SDL_GetModState(void);

/**
 * Set the current key modifier state for the keyboard.
 */
extern void SDL_SetModState(SDL_Keymod modstate);

/**
 * Get the window which currently has keyboard focus.
 */
extern SDL_Window *SDL_GetKeyboardFocus(void);

/**
 * Get the key code corresponding to the given scancode (layout-aware when a
 * keymap is available, e.g. from Wayland/xkbcommon).
 */
extern SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode, SDL_Keymod modstate, bool key_event);

/**
 * Get the scancode corresponding to the given key code.
 */
extern SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod *modstate);

/**
 * Get a human-readable name for a scancode.
 */
extern const char *SDL_GetScancodeName(SDL_Scancode scancode);

/**
 * Get a human-readable name for a key.
 */
extern const char *SDL_GetKeyName(SDL_Keycode key);

#endif /* SDL_keyboard_h_ */
