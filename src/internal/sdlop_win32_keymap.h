/*
  SDLop - translation of Win32 virtual-key codes to SDL keycodes.

  POSIX builds resolve keys through libxkbcommon (scancode -> keysym ->
  keycode, with the active layout). Windows has no such layer to consult: the
  WM_KEYDOWN message already carries the virtual-key code the layout produced,
  and WM_CHAR/WM_UNICHAR carry the text. So on Windows this table *is* the
  keymap, and the driver sends text itself rather than synthesising it.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_win32_keymap_h_
#define sdlop_win32_keymap_h_

#include <SDL3/SDL.h>

#ifdef _WIN32

/* SDLOP_SendKeyboardKey() takes a Uint16 "rawcode": the POSIX drivers put an
 * evdev keycode there. Windows needs both the VK (for the keycode) and the
 * scan code (for SDL_GetKeyFromScancode round-trips), so pack them:
 * high byte = VK (all the ones we care about fit in 8 bits), low byte = scan. */
#define SDLOP_WIN32_RAWCODE(vk, scan) ((Uint16)((((Uint32)(vk) & 0xFFu) << 8) | ((Uint32)(scan) & 0xFFu)))
#define SDLOP_WIN32_VK(raw)           ((Uint32)((raw) >> 8))

/* VK -> SDL_Keycode. `modstate` decides the case of printable keys, matching
 * SDL3's "the keycode reports what the key would type" rule. */
SDL_Keycode SDLOP_Win32KeyFromVK(Uint32 vk, SDL_Keymod modstate);

/* Windows scan code -> SDL_Scancode, for the WM_KEYDOWN path. `scan_code` is
 * the hardware code from lParam bits 16-23, widened to 0xE0xx for an extended
 * key (KF_EXTENDED); 0xE046 is Pause. The mapping is stock SDL3's table, and
 * SDL_Scancode uses USB HID numbering, so it is never the identity. */
SDL_Scancode SDLOP_Win32ScancodeFromScan(Uint16 scan_code);

/* Character a key produces with the given modifiers, for SDL_GetKeyFromName
 * style helpers; 0 when the key has no character. */
Uint32 SDLOP_Win32CharFromVK(Uint32 vk, bool shifted);

#endif /* _WIN32 */

#endif /* sdlop_win32_keymap_h_ */
