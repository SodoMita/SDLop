/*
  SDLop - Win32 virtual-key code -> SDL keycode (see sdlop_win32_keymap.h).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#ifdef _WIN32

#include "internal/sdlop_win32_keymap.h"

#define VK_A 0x41
#define VK_Z 0x5A
#define VK_0 0x30
#define VK_9 0x39
#define VK_F1_ 0x70

Uint32 SDLOP_Win32CharFromVK(Uint32 vk, bool shifted)
{
    if (vk >= VK_A && vk <= VK_Z) {
        return shifted ? (Uint32)vk : (Uint32)(vk + ('a' - 'A'));
    }
    if (vk >= VK_0 && vk <= VK_9) {
        static const char shifted_digits[] = ")!@#$%^&*(";
        return shifted ? (Uint32)(unsigned char)shifted_digits[vk - VK_0] : vk;
    }
    return 0;
}

SDL_Keycode SDLOP_Win32KeyFromVK(Uint32 vk, SDL_Keymod modstate)
{
    const bool shifted = (modstate & (SDL_KMOD_SHIFT | SDL_KMOD_CAPS)) == SDL_KMOD_SHIFT ||
                         (modstate & (SDL_KMOD_SHIFT | SDL_KMOD_CAPS)) == SDL_KMOD_CAPS;
    switch (vk) {
    case 0: return SDLK_UNKNOWN;
    case VK_CANCEL: return SDLK_CANCEL;
    case VK_BACK: return SDLK_BACKSPACE;
    case VK_TAB: return SDLK_TAB;
    case VK_CLEAR: return SDLK_CLEAR;
    case VK_RETURN: return SDLK_RETURN;
    case VK_SHIFT: return SDLK_LSHIFT; /* the driver refines with the scan code */
    case VK_CONTROL: return SDLK_LCTRL;
    case VK_MENU: return SDLK_LALT;
    case VK_PAUSE: return SDLK_PAUSE;
    case VK_CAPITAL: return SDLK_CAPSLOCK;
    case VK_ESCAPE: return SDLK_ESCAPE;
    case VK_SPACE: return SDLK_SPACE;
    case VK_PRIOR: return SDLK_PAGEUP;
    case VK_NEXT: return SDLK_PAGEDOWN;
    case VK_END: return SDLK_END;
    case VK_HOME: return SDLK_HOME;
    case VK_LEFT: return SDLK_LEFT;
    case VK_UP: return SDLK_UP;
    case VK_RIGHT: return SDLK_RIGHT;
    case VK_DOWN: return SDLK_DOWN;
    case VK_SELECT: return SDLK_SELECT;
    case VK_PRINT: return SDLK_PRINTSCREEN;
    case VK_EXECUTE: return SDLK_EXECUTE;
    case VK_SNAPSHOT: return SDLK_PRINTSCREEN;
    case VK_INSERT: return SDLK_INSERT;
    case VK_DELETE: return SDLK_DELETE;
    case VK_HELP: return SDLK_HELP;
    case VK_LWIN: return SDLK_LGUI;
    case VK_RWIN: return SDLK_RGUI;
    case VK_APPS: return SDLK_APPLICATION;
    case VK_SLEEP: return SDLK_SLEEP;
    case VK_NUMPAD0: return SDLK_KP_0;
    case VK_NUMPAD1: return SDLK_KP_1;
    case VK_NUMPAD2: return SDLK_KP_2;
    case VK_NUMPAD3: return SDLK_KP_3;
    case VK_NUMPAD4: return SDLK_KP_4;
    case VK_NUMPAD5: return SDLK_KP_5;
    case VK_NUMPAD6: return SDLK_KP_6;
    case VK_NUMPAD7: return SDLK_KP_7;
    case VK_NUMPAD8: return SDLK_KP_8;
    case VK_NUMPAD9: return SDLK_KP_9;
    case VK_MULTIPLY: return SDLK_KP_MULTIPLY;
    case VK_ADD: return SDLK_KP_PLUS;
    case VK_SEPARATOR: return SDLK_SEPARATOR;
    case VK_SUBTRACT: return SDLK_KP_MINUS;
    case VK_DECIMAL: return SDLK_KP_PERIOD;
    case VK_DIVIDE: return SDLK_KP_DIVIDE;
    case VK_NUMLOCK: return SDLK_NUMLOCKCLEAR;
    case VK_SCROLL: return SDLK_SCROLLLOCK;
    case VK_OEM_NEC_EQUAL: return SDLK_KP_EQUALS;

    /* the modifier keys, left and right told apart by the extended flag -
     * the driver sends L/R itself; these are the fallbacks */
    case VK_LSHIFT: return SDLK_LSHIFT;
    case VK_RSHIFT: return SDLK_RSHIFT;
    case VK_LCONTROL: return SDLK_LCTRL;
    case VK_RCONTROL: return SDLK_RCTRL;
    case VK_LMENU: return SDLK_LALT;
    case VK_RMENU: return SDLK_RALT;

    /* OEM punctuation: the keycode is the US character, as SDL3 reports it
     * for a physical key regardless of layout */
    case VK_OEM_1: return SDLK_SEMICOLON;
    case VK_OEM_PLUS: return SDLK_EQUALS;
    case VK_OEM_COMMA: return SDLK_COMMA;
    case VK_OEM_MINUS: return SDLK_MINUS;
    case VK_OEM_PERIOD: return SDLK_PERIOD;
    case VK_OEM_2: return SDLK_SLASH;
    case VK_OEM_3: return SDLK_GRAVE;
    case VK_OEM_4: return SDLK_LEFTBRACKET;
    case VK_OEM_5: return SDLK_BACKSLASH;
    case VK_OEM_6: return SDLK_RIGHTBRACKET;
    case VK_OEM_7: return SDLK_APOSTROPHE;
    case VK_OEM_8: return SDLK_UNKNOWN;
    case VK_OEM_102: return SDLK_BACKSLASH;
    case VK_OEM_AX: return SDLK_UNKNOWN;

    /* volume and media */
    case VK_VOLUME_MUTE: return SDLK_MUTE;
    case VK_VOLUME_DOWN: return SDLK_VOLUMEDOWN;
    case VK_VOLUME_UP: return SDLK_VOLUMEUP;
    case VK_MEDIA_NEXT_TRACK: return SDLK_MEDIA_NEXT_TRACK;
    case VK_MEDIA_PREV_TRACK: return SDLK_MEDIA_PREVIOUS_TRACK;
    case VK_MEDIA_STOP: return SDLK_MEDIA_STOP;
    case VK_MEDIA_PLAY_PAUSE: return SDLK_MEDIA_PLAY_PAUSE;
    case VK_BROWSER_BACK: return SDLK_AC_BACK;
    case VK_BROWSER_FORWARD: return SDLK_AC_FORWARD;
    case VK_BROWSER_REFRESH: return SDLK_AC_REFRESH;
    case VK_BROWSER_STOP: return SDLK_AC_STOP;
    case VK_BROWSER_SEARCH: return SDLK_AC_SEARCH;
    case VK_BROWSER_FAVORITES: return SDLK_AC_BOOKMARKS;
    case VK_BROWSER_HOME: return SDLK_AC_HOME;

    /* the IME keys: SDL3 has no keycode of its own for these, and the
     * scancode-based keycode is what SDL reports for them anyway */
    case VK_KANA:
    case VK_JUNJA:
    case VK_FINAL:
    case VK_HANJA:
    case VK_CONVERT:
    case VK_NONCONVERT:
    case VK_ACCEPT:
    case VK_MODECHANGE:
        return SDLK_UNKNOWN;

    default:
        break;
    }

    if (vk >= VK_A && vk <= VK_Z) {
        return (SDL_Keycode)(shifted ? vk : vk + ('a' - 'A'));
    }
    if (vk >= VK_0 && vk <= VK_9) {
        /* SDL3 reports digit keycodes as the character, unshifted */
        return (SDL_Keycode)vk;
    }
    if (vk >= VK_F1_ && vk <= VK_F1_ + 23) {
        return (SDL_Keycode)(SDLK_F1 + (vk - VK_F1_));
    }
    return SDLK_UNKNOWN;
}

#endif /* _WIN32 */
