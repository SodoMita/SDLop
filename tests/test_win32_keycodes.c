/*
  SDLop test: the Windows keyboard path, headless against the dummy driver.

  Windows has no xkb keymap. Key events carry virtual-key codes packed into
  the rawcode (SDLOP_WIN32_RAWCODE(vk, scancode)) and text arrives separately
  as WM_CHAR/WM_UNICHAR through SDLOP_SendKeyboardText, so this checks:

  - SDLOP_Win32KeyFromVK maps VKs to the SDL3 keycodes SDL3 uses
  - key events carry the *unmodified* key, as on every other backend
  - key text is whatever the driver delivered, not derived from modifiers
  - SDL_GetScancodeFromKey/SDL_GetKeyFromScancode round-trip on Windows

  Run under Wine by ctest; only builds on Windows.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include "internal/sdlop_internal.h"
#include "internal/sdlop_win32_keymap.h"
#include <stdio.h>
#include <string.h>

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

/* the VKs the driver actually sends (winuser.h) */
enum { VK_A_ = 0x41, VK_0_ = 0x30, VK_1_ = 0x31, VK_2_ = 0x32, VK_SPACE_ = 0x20 };

typedef struct
{
    SDL_Keycode key;
    SDL_Keymod mod;
    char text[16];
    bool got_key, got_text;
} key_result;

/* press + release one key the way the Win32 driver does, then collect */
static key_result press_vk(Uint32 vk, SDL_Scancode sc, const char *text)
{
    key_result r;
    memset(&r, 0, sizeof(r));
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    if (text) {
        SDLOP_SendKeyboardText(text, 0);
    }
    SDLOP_SendKeyboardKey(true, false, sc, (Uint16)SDLOP_WIN32_RAWCODE(vk, 0x1e), 0);
    SDLOP_SendKeyboardKey(false, false, sc, (Uint16)SDLOP_WIN32_RAWCODE(vk, 0x1e), 0);

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == sc && !r.got_key) {
            r.key = e.key.key;
            r.mod = e.key.mod;
            r.got_key = true;
        } else if (e.type == SDL_EVENT_TEXT_INPUT && !r.got_text) {
            SDL_strlcpy(r.text, e.text.text, sizeof(r.text));
            r.got_text = true;
        }
    }
    return r;
}

int main(void)
{
    /* --- VK -> keycode table ------------------------------------------- */
    CHECK(SDLOP_Win32KeyFromVK('A', SDL_KMOD_NONE) == SDLK_A, "VK 'A' -> SDLK_A");
    CHECK(SDLOP_Win32KeyFromVK('0', SDL_KMOD_NONE) == SDLK_0, "VK '0' -> SDLK_0");
    CHECK(SDLOP_Win32KeyFromVK(VK_SPACE_, SDL_KMOD_NONE) == SDLK_SPACE, "VK_SPACE");
    CHECK(SDLOP_Win32KeyFromVK(0x0D, SDL_KMOD_NONE) == SDLK_RETURN, "VK_RETURN");
    CHECK(SDLOP_Win32KeyFromVK(0x1B, SDL_KMOD_NONE) == SDLK_ESCAPE, "VK_ESCAPE");
    CHECK(SDLOP_Win32KeyFromVK(0x08, SDL_KMOD_NONE) == SDLK_BACKSPACE, "VK_BACK");
    CHECK(SDLOP_Win32KeyFromVK(0x09, SDL_KMOD_NONE) == SDLK_TAB, "VK_TAB");
    CHECK(SDLOP_Win32KeyFromVK(0x2E, SDL_KMOD_NONE) == SDLK_DELETE, "VK_DELETE");
    CHECK(SDLOP_Win32KeyFromVK(0x25, SDL_KMOD_NONE) == SDLK_LEFT, "VK_LEFT");
    CHECK(SDLOP_Win32KeyFromVK(0x26, SDL_KMOD_NONE) == SDLK_UP, "VK_UP");
    CHECK(SDLOP_Win32KeyFromVK(0x27, SDL_KMOD_NONE) == SDLK_RIGHT, "VK_RIGHT");
    CHECK(SDLOP_Win32KeyFromVK(0x28, SDL_KMOD_NONE) == SDLK_DOWN, "VK_DOWN");
    CHECK(SDLOP_Win32KeyFromVK(0x70, SDL_KMOD_NONE) == SDLK_F1, "VK_F1");
    CHECK(SDLOP_Win32KeyFromVK(0x7B, SDL_KMOD_NONE) == SDLK_F12, "VK_F12");
    CHECK(SDLOP_Win32KeyFromVK(0x10, SDL_KMOD_NONE) == SDLK_LSHIFT, "VK_SHIFT");
    CHECK(SDLOP_Win32KeyFromVK(0x11, SDL_KMOD_NONE) == SDLK_LCTRL, "VK_CONTROL");
    CHECK(SDLOP_Win32KeyFromVK(0x12, SDL_KMOD_NONE) == SDLK_LALT, "VK_MENU");
    CHECK(SDLOP_Win32KeyFromVK(0x5B, SDL_KMOD_NONE) == SDLK_LGUI, "VK_LWIN");
    CHECK(SDLOP_Win32KeyFromVK(0x60, SDL_KMOD_NONE) == SDLK_KP_0, "VK_NUMPAD0");
    CHECK(SDLOP_Win32KeyFromVK(0x6A, SDL_KMOD_NONE) == SDLK_KP_MULTIPLY, "VK_MULTIPLY");
    /* IME keys have no SDL3 keycode of their own: SDL3 reports the scancode */
    CHECK(SDLOP_Win32KeyFromVK(0x15, SDL_KMOD_NONE) == SDLK_UNKNOWN, "VK_KANA -> UNKNOWN");
    CHECK(SDLOP_Win32KeyFromVK(0x19, SDL_KMOD_NONE) == SDLK_UNKNOWN, "VK_HANJA -> UNKNOWN");

    /* rawcode packing: high byte VK, low byte scancode */
    CHECK(SDLOP_WIN32_VK(SDLOP_WIN32_RAWCODE(0x41, 0x1e)) == 0x41, "rawcode VK round-trip");
    CHECK(SDLOP_WIN32_RAWCODE(0x41, 0x1e) == ((0x41u << 8) | 0x1eu), "rawcode layout");

    /* --- the core keyboard path ---------------------------------------- */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"); /* headless, no desktop needed */
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS), "SDL_Init: %s", SDL_GetError());
    CHECK(strcmp(SDL_GetCurrentVideoDriver(), "dummy") == 0, "dummy driver");
    SDL_Window *w = SDL_CreateWindow("kbd", 64, 64, 0);
    CHECK(w != NULL, "window: %s", SDL_GetError());
    if (!w) {
        SDL_Quit();
        return 1;
    }
    SDL_StartTextInput(w);

    key_result r = press_vk(VK_A_, SDL_SCANCODE_A, "a");
    CHECK(r.got_key && r.key == SDLK_A, "VK 'A' -> key A (0x%x)", (unsigned)r.key);
    CHECK(r.got_text && strcmp(r.text, "a") == 0, "text 'a' (got '%s')", r.text);

    /* the event key stays unmodified; the text is the driver's business */
    SDLOP_SendKeyboardKey(true, false, SDL_SCANCODE_LSHIFT, (Uint16)SDLOP_WIN32_RAWCODE(0x10, 0x2a), 0);
    r = press_vk(VK_A_, SDL_SCANCODE_A, "A");
    CHECK(r.got_key && r.key == SDLK_A, "shift+'A' -> key A, unmodified (0x%x)", (unsigned)r.key);
    CHECK(r.mod & SDL_KMOD_LSHIFT, "shift carried in mod");
    CHECK(r.got_text && strcmp(r.text, "A") == 0, "text 'A' (got '%s')", r.text);
    SDLOP_SendKeyboardKey(false, false, SDL_SCANCODE_LSHIFT, (Uint16)SDLOP_WIN32_RAWCODE(0x10, 0x2a), 0);

    r = press_vk(VK_2_, SDL_SCANCODE_2, "@");
    CHECK(r.got_key && r.key == SDLK_2, "VK '2' -> SDLK_2");
    CHECK(r.got_text && strcmp(r.text, "@") == 0, "shifted text is the driver's (got '%s')", r.text);

    r = press_vk(VK_SPACE_, SDL_SCANCODE_SPACE, " ");
    CHECK(r.got_key && r.key == SDLK_SPACE, "VK_SPACE -> SDLK_SPACE");
    CHECK(r.got_text && strcmp(r.text, " ") == 0, "space text");

    /* keydown state tracks the press */
    CHECK(SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A] == false, "A released");

    /* --- scancode <-> keycode ------------------------------------------ */
    SDL_Keymod m = SDL_KMOD_NONE;
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_SPACE, SDL_KMOD_NONE, true) == SDLK_SPACE, "space keycode");
    CHECK(SDL_GetScancodeFromKey(SDLK_SPACE, &m) == SDL_SCANCODE_SPACE, "space scancode");
    /* key_event=false applies the modifiers: shift+a is the capital 'A'
     * (SDLK_A itself is the lowercase 'a', SDL3 semantics) */
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_SHIFT, false) == 'A', "shift+a -> 'A'");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_2, SDL_KMOD_LSHIFT, false) == SDLK_AT, "shift+2 -> '@'");
    CHECK(SDL_GetScancodeFromKey(SDLK_UP, NULL) == SDL_SCANCODE_UP, "SDLK_UP -> UP");
    CHECK(SDL_GetKeyboardFocus() == w, "focus follows the created window (dummy)");

    /* --- the active keyboard layout ------------------------------------ *
     * SDL_GetKeyFromScancode is documented as following the *current layout*,
     * and the layout is a character table built with ToUnicodeEx. Wine only
     * ships the US layout, so the interesting property to pin here is not a
     * specific translation but the split: keys that type a character take it,
     * everything else still resolves through the scancode table. If the table
     * were empty or failed to build, the first group would still pass (both
     * paths agree on US) -- so also insist the table itself is populated. */
    {
        static const struct {
            SDL_Scancode sc;
            SDL_Keycode want;
            const char *what;
        } typed[] = {
            { SDL_SCANCODE_A, 'a', "A" },
            { SDL_SCANCODE_Z, 'z', "Z" },
            { SDL_SCANCODE_Y, 'y', "Y" },
            { SDL_SCANCODE_1, '1', "1" },
            { SDL_SCANCODE_SEMICOLON, ';', "semicolon" },
            { SDL_SCANCODE_COMMA, ',', "comma" },
            { SDL_SCANCODE_PERIOD, '.', "period" },
            { SDL_SCANCODE_SLASH, '/', "slash" },
            { SDL_SCANCODE_LEFTBRACKET, '[', "left bracket" },
            { SDL_SCANCODE_GRAVE, '`', "grave" },
            { SDL_SCANCODE_MINUS, '-', "minus" },
            { SDL_SCANCODE_EQUALS, '=', "equals" },
            { SDL_SCANCODE_APOSTROPHE, '\'', "apostrophe" },
        };
        for (size_t i = 0; i < SDL_arraysize(typed); ++i) {
            const SDL_Keycode got = SDL_GetKeyFromScancode(typed[i].sc, SDL_KMOD_NONE, true);
            CHECK(got == typed[i].want, "%s: got 0x%x, expected '%c'",
                  typed[i].what, (unsigned)got, (char)typed[i].want);
        }
        /* This test runs on the dummy driver, so win32_Init has not built the
         * table; build it here, which is what the WM_INPUTLANGCHANGE path
         * does. Without this the checks below failed while the SDL-level ones
         * passed -- the scancode fallback hides an empty table on a US
         * layout, and that is precisely the bug this pins. */
        SDLOP_Win32UpdateKeyboardLayout();
        /* the layout table itself: characters present, non-keys absent */
        CHECK(SDLOP_Win32LayoutCharForScancode(SDL_SCANCODE_Y, false) == 'y',
              "layout table has no character for Y");
        CHECK(SDLOP_Win32LayoutCharForScancode(SDL_SCANCODE_Y, true) == 'Y',
              "layout table has no shifted Y");
        CHECK(SDLOP_Win32LayoutCharForScancode(SDL_SCANCODE_F1, false) == 0,
              "F1 must not have a layout character");
        CHECK(SDLOP_Win32LayoutCharForScancode(SDL_SCANCODE_LEFT, false) == 0,
              "LEFT must not have a layout character");
        /* keys with no character keep their scancode-derived keycode */
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_F1, SDL_KMOD_NONE, true) == SDLK_F1, "F1 keycode");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_F12, SDL_KMOD_NONE, true) == SDLK_F12, "F12 keycode");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_LEFT, SDL_KMOD_NONE, true) == SDLK_LEFT, "LEFT keycode");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, true) == SDLK_ESCAPE, "ESC keycode");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_KP_5, SDL_KMOD_NONE, true) == SDLK_KP_5, "KP_5 keycode");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_TAB, SDL_KMOD_NONE, true) == SDLK_TAB, "TAB keycode");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_RETURN, SDL_KMOD_NONE, true) == SDLK_RETURN, "RETURN keycode");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_BACKSPACE, SDL_KMOD_NONE, true) == SDLK_BACKSPACE, "BACKSPACE keycode");
        /* rebuilding is idempotent and must not lose the table */
        SDLOP_Win32UpdateKeyboardLayout();
        CHECK(SDLOP_Win32LayoutCharForScancode(SDL_SCANCODE_Y, false) == 'y',
              "the layout table was lost on rebuild");
    }

    SDL_StopTextInput(w);
    SDL_DestroyWindow(w);
    SDL_Quit();

    if (failures) {
        printf("test_win32_keycodes: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_win32_keycodes: PASS\n");
    return 0;
}
