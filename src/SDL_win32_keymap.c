/*
  SDLop - Win32 virtual-key code -> SDL keycode (see sdlop_win32_keymap.h).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#ifdef _WIN32

#include "internal/sdlop_win32_keymap.h"

#include <windows.h>

#define VK_A 0x41
#define VK_Z 0x5A
#define VK_0 0x30
#define VK_9 0x39
#define VK_F1_ 0x70

/* Windows scan code (a byte, or 0xE0xx for the extended set) to SDL scancode.
 *
 * This is stock SDL3's canonical table (src/events/scancodes_windows.h,
 * release-3.4.20). SDL_Scancode uses the USB HID usage numbering while
 * Windows uses its own, so the mapping must be a table and can never be a
 * cast: SDL_SCANCODE_A is 4, the Windows 'A' scan code is 0x1E.
 *
 * Index it with the low byte of the scan code, bit 7 set for the E0 prefix --
 * see SDLOP_Win32ScancodeFromScan(). */
static const SDL_Scancode windows_scancode_table[] = {
    /*0x00*/ SDL_SCANCODE_UNKNOWN,
    /*0x01*/ SDL_SCANCODE_ESCAPE,
    /*0x02*/ SDL_SCANCODE_1,
    /*0x03*/ SDL_SCANCODE_2,
    /*0x04*/ SDL_SCANCODE_3,
    /*0x05*/ SDL_SCANCODE_4,
    /*0x06*/ SDL_SCANCODE_5,
    /*0x07*/ SDL_SCANCODE_6,
    /*0x08*/ SDL_SCANCODE_7,
    /*0x09*/ SDL_SCANCODE_8,
    /*0x0a*/ SDL_SCANCODE_9,
    /*0x0b*/ SDL_SCANCODE_0,
    /*0x0c*/ SDL_SCANCODE_MINUS,
    /*0x0d*/ SDL_SCANCODE_EQUALS,
    /*0x0e*/ SDL_SCANCODE_BACKSPACE,
    /*0x0f*/ SDL_SCANCODE_TAB,
    /*0x10*/ SDL_SCANCODE_Q,
    /*0x11*/ SDL_SCANCODE_W,
    /*0x12*/ SDL_SCANCODE_E,
    /*0x13*/ SDL_SCANCODE_R,
    /*0x14*/ SDL_SCANCODE_T,
    /*0x15*/ SDL_SCANCODE_Y,
    /*0x16*/ SDL_SCANCODE_U,
    /*0x17*/ SDL_SCANCODE_I,
    /*0x18*/ SDL_SCANCODE_O,
    /*0x19*/ SDL_SCANCODE_P,
    /*0x1a*/ SDL_SCANCODE_LEFTBRACKET,
    /*0x1b*/ SDL_SCANCODE_RIGHTBRACKET,
    /*0x1c*/ SDL_SCANCODE_RETURN,
    /*0x1d*/ SDL_SCANCODE_LCTRL,
    /*0x1e*/ SDL_SCANCODE_A,
    /*0x1f*/ SDL_SCANCODE_S,
    /*0x20*/ SDL_SCANCODE_D,
    /*0x21*/ SDL_SCANCODE_F,
    /*0x22*/ SDL_SCANCODE_G,
    /*0x23*/ SDL_SCANCODE_H,
    /*0x24*/ SDL_SCANCODE_J,
    /*0x25*/ SDL_SCANCODE_K,
    /*0x26*/ SDL_SCANCODE_L,
    /*0x27*/ SDL_SCANCODE_SEMICOLON,
    /*0x28*/ SDL_SCANCODE_APOSTROPHE,
    /*0x29*/ SDL_SCANCODE_GRAVE,
    /*0x2a*/ SDL_SCANCODE_LSHIFT,
    /*0x2b*/ SDL_SCANCODE_BACKSLASH,
    /*0x2c*/ SDL_SCANCODE_Z,
    /*0x2d*/ SDL_SCANCODE_X,
    /*0x2e*/ SDL_SCANCODE_C,
    /*0x2f*/ SDL_SCANCODE_V,
    /*0x30*/ SDL_SCANCODE_B,
    /*0x31*/ SDL_SCANCODE_N,
    /*0x32*/ SDL_SCANCODE_M,
    /*0x33*/ SDL_SCANCODE_COMMA,
    /*0x34*/ SDL_SCANCODE_PERIOD,
    /*0x35*/ SDL_SCANCODE_SLASH,
    /*0x36*/ SDL_SCANCODE_RSHIFT,
    /*0x37*/ SDL_SCANCODE_KP_MULTIPLY,
    /*0x38*/ SDL_SCANCODE_LALT,
    /*0x39*/ SDL_SCANCODE_SPACE,
    /*0x3a*/ SDL_SCANCODE_CAPSLOCK,
    /*0x3b*/ SDL_SCANCODE_F1,
    /*0x3c*/ SDL_SCANCODE_F2,
    /*0x3d*/ SDL_SCANCODE_F3,
    /*0x3e*/ SDL_SCANCODE_F4,
    /*0x3f*/ SDL_SCANCODE_F5,
    /*0x40*/ SDL_SCANCODE_F6,
    /*0x41*/ SDL_SCANCODE_F7,
    /*0x42*/ SDL_SCANCODE_F8,
    /*0x43*/ SDL_SCANCODE_F9,
    /*0x44*/ SDL_SCANCODE_F10,
    /*0x45*/ SDL_SCANCODE_NUMLOCKCLEAR,
    /*0x46*/ SDL_SCANCODE_SCROLLLOCK,
    /*0x47*/ SDL_SCANCODE_KP_7,
    /*0x48*/ SDL_SCANCODE_KP_8,
    /*0x49*/ SDL_SCANCODE_KP_9,
    /*0x4a*/ SDL_SCANCODE_KP_MINUS,
    /*0x4b*/ SDL_SCANCODE_KP_4,
    /*0x4c*/ SDL_SCANCODE_KP_5,
    /*0x4d*/ SDL_SCANCODE_KP_6,
    /*0x4e*/ SDL_SCANCODE_KP_PLUS,
    /*0x4f*/ SDL_SCANCODE_KP_1,
    /*0x50*/ SDL_SCANCODE_KP_2,
    /*0x51*/ SDL_SCANCODE_KP_3,
    /*0x52*/ SDL_SCANCODE_KP_0,
    /*0x53*/ SDL_SCANCODE_KP_PERIOD,
    /*0x54*/ SDL_SCANCODE_UNKNOWN,
    /*0x55*/ SDL_SCANCODE_UNKNOWN,
    /*0x56*/ SDL_SCANCODE_NONUSBACKSLASH,
    /*0x57*/ SDL_SCANCODE_F11,
    /*0x58*/ SDL_SCANCODE_F12,
    /*0x59*/ SDL_SCANCODE_KP_EQUALS,
    /*0x5a*/ SDL_SCANCODE_UNKNOWN,
    /*0x5b*/ SDL_SCANCODE_UNKNOWN,
    /*0x5c*/ SDL_SCANCODE_INTERNATIONAL6,
    /*0x5d*/ SDL_SCANCODE_UNKNOWN,
    /*0x5e*/ SDL_SCANCODE_UNKNOWN,
    /*0x5f*/ SDL_SCANCODE_UNKNOWN,
    /*0x60*/ SDL_SCANCODE_UNKNOWN,
    /*0x61*/ SDL_SCANCODE_UNKNOWN,
    /*0x62*/ SDL_SCANCODE_UNKNOWN,
    /*0x63*/ SDL_SCANCODE_UNKNOWN,
    /*0x64*/ SDL_SCANCODE_F13,
    /*0x65*/ SDL_SCANCODE_F14,
    /*0x66*/ SDL_SCANCODE_F15,
    /*0x67*/ SDL_SCANCODE_F16,
    /*0x68*/ SDL_SCANCODE_F17,
    /*0x69*/ SDL_SCANCODE_F18,
    /*0x6a*/ SDL_SCANCODE_F19,
    /*0x6b*/ SDL_SCANCODE_F20,
    /*0x6c*/ SDL_SCANCODE_F21,
    /*0x6d*/ SDL_SCANCODE_F22,
    /*0x6e*/ SDL_SCANCODE_F23,
    /*0x6f*/ SDL_SCANCODE_UNKNOWN,
    /*0x70*/ SDL_SCANCODE_INTERNATIONAL2,
    /*0x71*/ SDL_SCANCODE_LANG2,
    /*0x72*/ SDL_SCANCODE_LANG1,
    /*0x73*/ SDL_SCANCODE_INTERNATIONAL1,
    /*0x74*/ SDL_SCANCODE_UNKNOWN,
    /*0x75*/ SDL_SCANCODE_UNKNOWN,
    /*0x76*/ SDL_SCANCODE_F24,
    /*0x77*/ SDL_SCANCODE_LANG4,
    /*0x78*/ SDL_SCANCODE_LANG3,
    /*0x79*/ SDL_SCANCODE_INTERNATIONAL4,
    /*0x7a*/ SDL_SCANCODE_UNKNOWN,
    /*0x7b*/ SDL_SCANCODE_INTERNATIONAL5,
    /*0x7c*/ SDL_SCANCODE_UNKNOWN,
    /*0x7d*/ SDL_SCANCODE_INTERNATIONAL3,
    /*0x7e*/ SDL_SCANCODE_KP_COMMA,
    /*0x7f*/ SDL_SCANCODE_UNKNOWN,
    /*0xe000*/ SDL_SCANCODE_UNKNOWN,
    /*0xe001*/ SDL_SCANCODE_UNKNOWN, // Fn+Esc on HP 250 G7 Notebook PC and HP Laptop 14-cm0xxx keyboards
    /*0xe002*/ SDL_SCANCODE_UNKNOWN,
    /*0xe003*/ SDL_SCANCODE_UNKNOWN,
    /*0xe004*/ SDL_SCANCODE_UNKNOWN,
    /*0xe005*/ SDL_SCANCODE_UNKNOWN, // "Messenger" on Natural Multimedia Keyboard
    /*0xe006*/ SDL_SCANCODE_UNKNOWN,
    /*0xe007*/ SDL_SCANCODE_AGAIN, // "Redo" on Natural Multimedia Keyboard
    /*0xe008*/ SDL_SCANCODE_UNDO, // "Undo" on Natural Multimedia Keyboard
    /*0xe009*/ SDL_SCANCODE_UNKNOWN,
    /*0xe00a*/ SDL_SCANCODE_PASTE,
    /*0xe00b*/ SDL_SCANCODE_UNKNOWN,
    /*0xe00c*/ SDL_SCANCODE_UNKNOWN,
    /*0xe00d*/ SDL_SCANCODE_UNKNOWN,
    /*0xe00e*/ SDL_SCANCODE_UNKNOWN,
    /*0xe00f*/ SDL_SCANCODE_UNKNOWN,
    /*0xe010*/ SDL_SCANCODE_MEDIA_PREVIOUS_TRACK,
    /*0xe011*/ SDL_SCANCODE_UNKNOWN,
    /*0xe012*/ SDL_SCANCODE_UNKNOWN,
    /*0xe013*/ SDL_SCANCODE_UNKNOWN,
    /*0xe014*/ SDL_SCANCODE_UNKNOWN,
    /*0xe015*/ SDL_SCANCODE_UNKNOWN,
    /*0xe016*/ SDL_SCANCODE_UNKNOWN, // "Log Off" on Natural Multimedia Keyboard
    /*0xe017*/ SDL_SCANCODE_CUT,
    /*0xe018*/ SDL_SCANCODE_COPY,
    /*0xe019*/ SDL_SCANCODE_MEDIA_NEXT_TRACK,
    /*0xe01a*/ SDL_SCANCODE_UNKNOWN,
    /*0xe01b*/ SDL_SCANCODE_UNKNOWN,
    /*0xe01c*/ SDL_SCANCODE_KP_ENTER,
    /*0xe01d*/ SDL_SCANCODE_RCTRL,
    /*0xe01e*/ SDL_SCANCODE_UNKNOWN,
    /*0xe01f*/ SDL_SCANCODE_UNKNOWN,
    /*0xe020*/ SDL_SCANCODE_MUTE,
    /*0xe021*/ SDL_SCANCODE_UNKNOWN, // LaunchApp2; "Calculator" on Natural Multimedia Keyboard and on Turbo multimedia keyboards
    /*0xe022*/ SDL_SCANCODE_MEDIA_PLAY_PAUSE,
    /*0xe023*/ SDL_SCANCODE_UNKNOWN, // "Spell" on Natural Multimedia Keyboard
    /*0xe024*/ SDL_SCANCODE_MEDIA_STOP,
    /*0xe025*/ SDL_SCANCODE_UNKNOWN,
    /*0xe026*/ SDL_SCANCODE_UNKNOWN,
    /*0xe027*/ SDL_SCANCODE_UNKNOWN,
    /*0xe028*/ SDL_SCANCODE_UNKNOWN,
    /*0xe029*/ SDL_SCANCODE_UNKNOWN,
    /*0xe02a*/ SDL_SCANCODE_UNKNOWN,
    /*0xe02b*/ SDL_SCANCODE_UNKNOWN, // Brightness up & brightness down on HP 250 G7 Notebook PC and HP Laptop 14-cm0xxx keyboards
    /*0xe02c*/ SDL_SCANCODE_MEDIA_EJECT,
    /*0xe02d*/ SDL_SCANCODE_UNKNOWN,
    /*0xe02e*/ SDL_SCANCODE_VOLUMEDOWN,
    /*0xe02f*/ SDL_SCANCODE_UNKNOWN,
    /*0xe030*/ SDL_SCANCODE_VOLUMEUP,
    /*0xe031*/ SDL_SCANCODE_UNKNOWN,
    /*0xe032*/ SDL_SCANCODE_AC_HOME,
    /*0xe033*/ SDL_SCANCODE_UNKNOWN,
    /*0xe034*/ SDL_SCANCODE_UNKNOWN,
    /*0xe035*/ SDL_SCANCODE_KP_DIVIDE,
    /*0xe036*/ SDL_SCANCODE_RSHIFT, // see https://mojira.dev/MC-311424
    /*0xe037*/ SDL_SCANCODE_PRINTSCREEN,
    /*0xe038*/ SDL_SCANCODE_RALT,
    /*0xe039*/ SDL_SCANCODE_UNKNOWN,
    /*0xe03a*/ SDL_SCANCODE_UNKNOWN,
    /*0xe03b*/ SDL_SCANCODE_HELP,
    /*0xe03c*/ SDL_SCANCODE_UNKNOWN, // "My Music" on Natural Multimedia Keyboard
    /*0xe03d*/ SDL_SCANCODE_UNKNOWN,
    /*0xe03e*/ SDL_SCANCODE_AC_NEW, // "New" on Natural Multimedia Keyboard
    /*0xe03f*/ SDL_SCANCODE_AC_OPEN, // "Open" on Natural Multimedia Keyboard
    /*0xe040*/ SDL_SCANCODE_AC_CLOSE, // "Close" on Natural Multimedia Keyboard
    /*0xe041*/ SDL_SCANCODE_UNKNOWN, // "Reply" on Natural Multimedia Keyboard
    /*0xe042*/ SDL_SCANCODE_UNKNOWN, // "Fwd" on Natural Multimedia Keyboard
    /*0xe043*/ SDL_SCANCODE_UNKNOWN, // "Send" on Natural Multimedia Keyboard
    /*0xe044*/ SDL_SCANCODE_UNKNOWN,
    /*0xe045*/ SDL_SCANCODE_NUMLOCKCLEAR,
    /*0xe046*/ SDL_SCANCODE_PAUSE,
    /*0xe047*/ SDL_SCANCODE_HOME,
    /*0xe048*/ SDL_SCANCODE_UP,
    /*0xe049*/ SDL_SCANCODE_PAGEUP,
    /*0xe04a*/ SDL_SCANCODE_UNKNOWN,
    /*0xe04b*/ SDL_SCANCODE_LEFT,
    /*0xe04c*/ SDL_SCANCODE_UNKNOWN, // "My Documents" on Natural Multimedia Keyboard
    /*0xe04d*/ SDL_SCANCODE_RIGHT,
    /*0xe04e*/ SDL_SCANCODE_UNKNOWN,
    /*0xe04f*/ SDL_SCANCODE_END,
    /*0xe050*/ SDL_SCANCODE_DOWN,
    /*0xe051*/ SDL_SCANCODE_PAGEDOWN,
    /*0xe052*/ SDL_SCANCODE_INSERT,
    /*0xe053*/ SDL_SCANCODE_DELETE,
    /*0xe054*/ SDL_SCANCODE_UNKNOWN,
    /*0xe055*/ SDL_SCANCODE_UNKNOWN,
    /*0xe056*/ SDL_SCANCODE_UNKNOWN,
    /*0xe057*/ SDL_SCANCODE_AC_SAVE, // "Save" on Natural Multimedia Keyboard
    /*0xe058*/ SDL_SCANCODE_AC_PRINT, // "Print" on Natural Multimedia Keyboard
    /*0xe059*/ SDL_SCANCODE_UNKNOWN,
    /*0xe05a*/ SDL_SCANCODE_UNKNOWN,
    /*0xe05b*/ SDL_SCANCODE_LGUI,
    /*0xe05c*/ SDL_SCANCODE_RGUI,
    /*0xe05d*/ SDL_SCANCODE_APPLICATION,
    /*0xe05e*/ SDL_SCANCODE_POWER,
    /*0xe05f*/ SDL_SCANCODE_SLEEP,
    /*0xe060*/ SDL_SCANCODE_UNKNOWN,
    /*0xe061*/ SDL_SCANCODE_UNKNOWN,
    /*0xe062*/ SDL_SCANCODE_UNKNOWN,
    /*0xe063*/ SDL_SCANCODE_WAKE,  // "Wake Up" on Turbo multimedia keyboards
    /*0xe064*/ SDL_SCANCODE_UNKNOWN, // "My Pictures" on Natural Multimedia Keyboard
    /*0xe065*/ SDL_SCANCODE_AC_SEARCH,
    /*0xe066*/ SDL_SCANCODE_AC_BOOKMARKS,
    /*0xe067*/ SDL_SCANCODE_AC_REFRESH,
    /*0xe068*/ SDL_SCANCODE_AC_STOP,
    /*0xe069*/ SDL_SCANCODE_AC_FORWARD,
    /*0xe06a*/ SDL_SCANCODE_AC_BACK,
    /*0xe06b*/ SDL_SCANCODE_UNKNOWN,    // LaunchApp1; "My computer" on Turbo multimedia keyboards
    /*0xe06c*/ SDL_SCANCODE_UNKNOWN,    // LaunchMail; "Mail" on Natural Multimedia Keyboard and on Turbo multimedia keyboards
    /*0xe06d*/ SDL_SCANCODE_MEDIA_SELECT,
    /*0xe06e*/ SDL_SCANCODE_UNKNOWN,
    /*0xe06f*/ SDL_SCANCODE_UNKNOWN,
    /*0xe070*/ SDL_SCANCODE_UNKNOWN,
    /*0xe071*/ SDL_SCANCODE_UNKNOWN,
    /*0xe072*/ SDL_SCANCODE_UNKNOWN,
    /*0xe073*/ SDL_SCANCODE_UNKNOWN,
    /*0xe074*/ SDL_SCANCODE_UNKNOWN,
    /*0xe075*/ SDL_SCANCODE_UNKNOWN,
    /*0xe076*/ SDL_SCANCODE_UNKNOWN,
    /*0xe077*/ SDL_SCANCODE_UNKNOWN,
    /*0xe078*/ SDL_SCANCODE_UNKNOWN,
    /*0xe079*/ SDL_SCANCODE_UNKNOWN,
    /*0xe07a*/ SDL_SCANCODE_UNKNOWN,
    /*0xe07b*/ SDL_SCANCODE_UNKNOWN,
    /*0xe07c*/ SDL_SCANCODE_UNKNOWN,
    /*0xe07d*/ SDL_SCANCODE_UNKNOWN,
    /*0xe07e*/ SDL_SCANCODE_UNKNOWN,
    /*0xe07f*/ SDL_SCANCODE_UNKNOWN
};

SDL_Scancode SDLOP_Win32ScancodeFromScan(Uint16 scan_code)
{
    /* Bit 7 of the index is the E0 prefix, so the table is two 128-entry
     * banks: 0x00-0x7F plain, 0x80-0xFF extended. */
    const Uint8 index = (Uint8)(LOBYTE(scan_code) | (HIBYTE(scan_code) ? 0x80 : 0x00));
    return windows_scancode_table[index];
}

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
