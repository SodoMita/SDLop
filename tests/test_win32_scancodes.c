/*
  SDLop test: Windows scan code -> SDL_Scancode mapping.

  The Windows driver translates WM_KEYDOWN scan codes through a table,
  stock SDL3's (src/events/scancodes_windows.h). SDL_Scancode is the USB HID
  usage numbering and Windows scan codes are a different numbering, so this
  can never be a cast: SDL_SCANCODE_A is 4 while the Windows 'A' scan code is
  0x1E. Getting it wrong shifts every SDL_GetKeyboardState() index, silently,
  which is the bug this pins down.

  Every "want" below is the value stock SDL3 reports for that key, so this
  test fails if the table drifts from SDL3's.

  Headless: it calls the mapping directly and opens no window.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <windows.h>
#include "internal/sdlop_win32_keymap.h"
#include <stdio.h>

static int failures = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            failures++;                                          \
        }                                                        \
    } while (0)

struct keycase
{
    const char *name;
    UINT vk;
    UINT scan;      /* hardware scan code, low byte of lParam bits 16-23 */
    int extended;   /* set the E0 prefix bit */
    int want;       /* the SDL_Scancode stock SDL3 produces */
};

/* values below are stock SDL3's (release-3.4.20) for these keys */
static const struct keycase cases[] = {
    /* the row that used to be off by one in every letter */
    { "A",        'A',        0x1E, 0, SDL_SCANCODE_A },          /* 4,  not 30 */
    { "L",        'L',        0x26, 0, SDL_SCANCODE_L },          /* 15 */
    { "M",        'M',        0x32, 0, SDL_SCANCODE_M },          /* 16 */
    { "Q",        'Q',        0x10, 0, SDL_SCANCODE_Q },          /* 20, not 16 */
    { "Z",        'Z',        0x2C, 0, SDL_SCANCODE_Z },          /* 29 */
    /* top row and punctuation */
    { "Escape",   VK_ESCAPE,  0x01, 0, SDL_SCANCODE_ESCAPE },     /* 41, not 1 */
    { "1",        '1',        0x02, 0, SDL_SCANCODE_1 },          /* 30, not 2 */
    { "0",        '0',        0x0B, 0, SDL_SCANCODE_0 },          /* 39 */
    { "Minus",    VK_OEM_MINUS, 0x0C, 0, SDL_SCANCODE_MINUS },    /* 45 */
    { "Equals",   VK_OEM_PLUS,  0x0D, 0, SDL_SCANCODE_EQUALS },   /* 46 */
    { "Backspace", VK_BACK,   0x0E, 0, SDL_SCANCODE_BACKSPACE },  /* 42 */
    { "Tab",      VK_TAB,     0x0F, 0, SDL_SCANCODE_TAB },        /* 43 */
    { "Space",    VK_SPACE,   0x39, 0, SDL_SCANCODE_SPACE },      /* 44 */
    { "Semicolon", VK_OEM_1,  0x27, 0, SDL_SCANCODE_SEMICOLON },  /* 51 */
    { "Grave",    VK_OEM_3,   0x29, 0, SDL_SCANCODE_GRAVE },      /* 53 */
    { "Backslash", VK_OEM_5,  0x2B, 0, SDL_SCANCODE_BACKSLASH },  /* 49 */
    /* function keys: F1..F10 are 58..67, F11 is 68 (not 87) */
    { "F1",       VK_F1,      0x3B, 0, SDL_SCANCODE_F1 },         /* 58, not 59 */
    { "F10",      VK_F10,     0x44, 0, SDL_SCANCODE_F10 },        /* 67 */
    { "F11",      VK_F11,     0x57, 0, SDL_SCANCODE_F11 },        /* 68, not 87 */
    { "F12",      VK_F12,     0x58, 0, SDL_SCANCODE_F12 },        /* 69 */
    /* modifiers and locks */
    { "Return",   VK_RETURN,  0x1C, 0, SDL_SCANCODE_RETURN },     /* 40 */
    { "LCtrl",    VK_LCONTROL, 0x1D, 0, SDL_SCANCODE_LCTRL },     /* 224 */
    { "LShift",   VK_LSHIFT,  0x2A, 0, SDL_SCANCODE_LSHIFT },     /* 225 */
    { "LAlt",     VK_LMENU,   0x38, 0, SDL_SCANCODE_LALT },       /* 226 */
    { "CapsLock", VK_CAPITAL, 0x3A, 0, SDL_SCANCODE_CAPSLOCK },   /* 57 */
    /* the extended block: same scan code as the keypad, different key */
    { "RCtrl",    VK_RCONTROL, 0x1D, 1, SDL_SCANCODE_RCTRL },     /* 228 */
    { "RAlt",     VK_RMENU,    0x38, 1, SDL_SCANCODE_RALT },      /* 230 */
    { "LWin",     VK_LWIN,     0x5B, 1, SDL_SCANCODE_LGUI },      /* 227 */
    { "Menu",     VK_APPS,     0x5D, 1, SDL_SCANCODE_APPLICATION }, /* 101 */
    { "Left",     VK_LEFT,     0x4B, 1, SDL_SCANCODE_LEFT },      /* 80 */
    { "Right",    VK_RIGHT,    0x4D, 1, SDL_SCANCODE_RIGHT },     /* 79 */
    { "Up",       VK_UP,       0x48, 1, SDL_SCANCODE_UP },        /* 82 */
    { "Down",     VK_DOWN,     0x50, 1, SDL_SCANCODE_DOWN },      /* 81 */
    { "Home",     VK_HOME,     0x47, 1, SDL_SCANCODE_HOME },      /* 74 */
    { "End",      VK_END,      0x4F, 1, SDL_SCANCODE_END },       /* 77 */
    { "PageUp",   VK_PRIOR,    0x49, 1, SDL_SCANCODE_PAGEUP },    /* 75 */
    { "PageDown", VK_NEXT,     0x51, 1, SDL_SCANCODE_PAGEDOWN },  /* 78 */
    { "Insert",   VK_INSERT,   0x52, 1, SDL_SCANCODE_INSERT },    /* 73 */
    { "Delete",   VK_DELETE,   0x53, 1, SDL_SCANCODE_DELETE },    /* 76 */
    { "NumLock",  VK_NUMLOCK,  0x45, 1, SDL_SCANCODE_NUMLOCKCLEAR }, /* 83 */
    { "KP/",      VK_DIVIDE,   0x35, 1, SDL_SCANCODE_KP_DIVIDE }, /* 84 */
    { "KPEnter",  VK_RETURN,   0x1C, 1, SDL_SCANCODE_KP_ENTER },  /* 88 */
    /* the non-extended half of the same codes is the keypad */
    { "KP4",      VK_NUMPAD4,  0x4B, 0, SDL_SCANCODE_KP_4 },      /* 92 */
    { "KP5",      VK_NUMPAD5,  0x4C, 0, SDL_SCANCODE_KP_5 },      /* 93 */
    { "KP0",      VK_NUMPAD0,  0x52, 0, SDL_SCANCODE_KP_0 },      /* 98 */
    { "KP1",      VK_NUMPAD1,  0x4F, 0, SDL_SCANCODE_KP_1 },      /* 89 */
};

int main(void)
{
    size_t i;

    for (i = 0; i < SDL_arraysize(cases); i++) {
        const struct keycase *kc = &cases[i];
        /* 0xE0xx carries the E0 prefix, exactly as the driver packs it */
        const Uint16 scan = kc->extended ? (Uint16)(0xE000 | kc->scan) : (Uint16)kc->scan;
        const SDL_Scancode got = SDLOP_Win32ScancodeFromScan(scan);
        CHECK((int)got == kc->want, "%s: scan 0x%02X%s -> SDL_Scancode %d, want %d",
              kc->name, kc->scan, kc->extended ? " (E0)" : "", (int)got, kc->want);
    }

    /* the table is a translation, not an identity: guard the property that
     * makes this worth testing at all */
    CHECK(SDL_SCANCODE_A == 4, "SDL_SCANCODE_A should be 4");
    CHECK(SDLOP_Win32ScancodeFromScan(0x1E) != (SDL_Scancode)0x1E,
          "the scan code must not be cast to a scancode");

    if (failures) {
        fprintf(stderr, "test_win32_scancodes: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_scancodes: %d scan codes match stock SDL3\n",
           (int)SDL_arraysize(cases));
    return 0;
}
