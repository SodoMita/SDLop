/*
  SDLop test: SDL3 keycode semantics on the native (xkbcommon) keyboard path,
  headless against the dummy driver with the default US keymap.

  - SDLK_A..SDLK_Z are the lowercase characters (SDL3: SDLK_A == 'a')
  - key events carry the *unmodified* key: shift+a -> key 'a', text "A"
  - SDL_GetKeyFromScancode(key_event=true) ignores modifiers,
    key_event=false applies them
  - SDL_GetKeyFromName/SDL_GetKeyName/SDL_GetScancodeFromKey round-trips

  Run a second time with SDLOP_LIB_XKB=none (ctest: test_keycodes_noxkb):
  the built-in US table must give the same answers without libxkbcommon.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include "internal/sdlop_internal.h"
#include <stdio.h>
#include <stdlib.h>
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

/* evdev codes (linux/input-event-codes.h) */
enum { KEY_1 = 2, KEY_2 = 3, KEY_A = 30, KEY_LEFTSHIFT = 42, KEY_SPACE = 57 };

typedef struct
{
    SDL_Keycode key;
    SDL_Keymod mod;
    char text[16];
    bool got_key, got_text;
} key_result;

/* press + release one key through the core keyboard path, like a driver */
static key_result press(SDL_Scancode sc, Uint16 evdev)
{
    key_result r;
    memset(&r, 0, sizeof(r));
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    SDLOP_SendKeyboardKey(true, false, sc, evdev, 0);
    SDLOP_SendKeyboardKey(false, false, sc, evdev, 0);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == sc && !r.got_key) {
            r.key = e.key.key;
            r.mod = e.key.mod;
            r.got_key = true;
        } else if (e.type == SDL_EVENT_TEXT_INPUT && !r.got_text) {
            snprintf(r.text, sizeof(r.text), "%s", e.text.text);
            r.got_text = true;
        }
    }
    return r;
}

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    const char *xkb_env = getenv("SDLOP_LIB_XKB");
    const bool no_xkb = xkb_env && strcmp(xkb_env, "none") == 0;

    /* compile-time ABI: identical to SDL3's SDL_keycode.h */
    CHECK(SDLK_A == 0x61u && SDLK_M == 0x6du && SDLK_Z == 0x7au, "SDLK_A..Z == 'a'..'z'");
    CHECK(SDLK_0 == '0' && SDLK_SPACE == ' ' && SDLK_AT == '@' && SDLK_RETURN == '\r', "ASCII keycodes");
    CHECK(SDLK_F1 == (SDL_SCANCODE_F1 | SDLK_SCANCODE_MASK), "SDLK_F1 is scancode-masked");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("keycodes", 64, 64, 0);
    CHECK(w != NULL, "window");
    printf("test_keycodes: keymap %s\n", no_xkb ? "built-in US table (no libxkbcommon)" : "xkbcommon default");

    /* --- SDL_GetKeyFromScancode ------------------------------------------ */
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_NONE, true) == SDLK_A, "A -> SDLK_A");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_LSHIFT, true) == SDLK_A,
          "key_event=true ignores shift (0x%x)", (unsigned)SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_LSHIFT, true));
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_NONE, false) == SDLK_A, "A unmodified");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_RSHIFT, false) == 'A', "shift applied -> 'A' (0x%x)",
          (unsigned)SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_RSHIFT, false));
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_CAPS, false) == 'A', "caps applied -> 'A'");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_2, SDL_KMOD_LSHIFT, false) == SDLK_AT, "shift+2 -> '@'");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_2, SDL_KMOD_LSHIFT, true) == SDLK_2, "key event shift+2 -> '2'");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_SPACE, SDL_KMOD_NONE, true) == SDLK_SPACE, "space");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_RETURN, SDL_KMOD_NONE, true) == SDLK_RETURN, "return");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, true) == SDLK_ESCAPE, "escape");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_F5, SDL_KMOD_NONE, true) == SDLK_F5, "F5");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_LEFT, SDL_KMOD_LSHIFT, false) == SDLK_LEFT, "shift+left -> left");

    /* --- SDL_GetScancodeFromKey ------------------------------------------ */
    SDL_Keymod m = SDL_KMOD_NONE;
    CHECK(SDL_GetScancodeFromKey(SDLK_A, &m) == SDL_SCANCODE_A && m == SDL_KMOD_NONE, "SDLK_A -> A");
    m = SDL_KMOD_NONE;
    CHECK(SDL_GetScancodeFromKey('A', &m) == SDL_SCANCODE_A && (m & SDL_KMOD_SHIFT), "'A' -> A + shift");
    m = SDL_KMOD_NONE;
    CHECK(SDL_GetScancodeFromKey(SDLK_AT, &m) == SDL_SCANCODE_2 && (m & SDL_KMOD_SHIFT), "'@' -> 2 + shift");
    CHECK(SDL_GetScancodeFromKey(SDLK_UP, NULL) == SDL_SCANCODE_UP, "SDLK_UP -> UP");

    /* --- names ------------------------------------------------------------ */
    CHECK(strcmp(SDL_GetKeyName(SDLK_A), "A") == 0, "name of SDLK_A is '%s'", SDL_GetKeyName(SDLK_A));
    CHECK(strcmp(SDL_GetKeyName(SDLK_ESCAPE), "Escape") == 0, "name of SDLK_ESCAPE");
    CHECK(SDL_GetKeyFromName("A") == SDLK_A, "key from 'A' (0x%x)", (unsigned)SDL_GetKeyFromName("A"));
    CHECK(SDL_GetKeyFromName("a") == SDLK_A, "key from 'a'");
    CHECK(SDL_GetKeyFromName("Space") == SDLK_SPACE, "key from 'Space' (0x%x)", (unsigned)SDL_GetKeyFromName("Space"));
    CHECK(SDL_GetKeyFromName("Return") == SDLK_RETURN, "key from 'Return'");
    CHECK(SDL_GetKeyFromName("Escape") == SDLK_ESCAPE, "key from 'Escape'");
    CHECK(SDL_GetKeyFromName("F5") == SDLK_F5, "key from 'F5'");
    CHECK(SDL_GetKeyFromName(SDL_GetKeyName(SDLK_Q)) == SDLK_Q, "name round-trip for SDLK_Q");

    /* --- key events carry the unmodified key; text follows modifiers ------ */
    SDL_StartTextInput(w);
    key_result r = press(SDL_SCANCODE_A, KEY_A);
    CHECK(r.got_key && r.key == SDLK_A, "KEY_DOWN a -> SDLK_A (0x%x)", (unsigned)r.key);
    CHECK(r.got_text && strcmp(r.text, "a") == 0, "text 'a' (got '%s')", r.text);

    SDLOP_SendKeyboardKey(true, false, SDL_SCANCODE_LSHIFT, KEY_LEFTSHIFT, 0);
    r = press(SDL_SCANCODE_A, KEY_A);
    CHECK(r.got_key && r.key == SDLK_A, "KEY_DOWN shift+a -> SDLK_A, unmodified like SDL3 (0x%x)", (unsigned)r.key);
    CHECK(r.mod & SDL_KMOD_LSHIFT, "shift+a carries LSHIFT");
    CHECK(r.got_text && strcmp(r.text, "A") == 0, "shift+a types 'A' (got '%s')", r.text);
    r = press(SDL_SCANCODE_2, KEY_2);
    CHECK(r.got_key && r.key == SDLK_2, "KEY_DOWN shift+2 -> SDLK_2 (0x%x)", (unsigned)r.key);
    CHECK(r.got_text && strcmp(r.text, "@") == 0, "shift+2 types '@' (got '%s')", r.text);
    SDLOP_SendKeyboardKey(false, false, SDL_SCANCODE_LSHIFT, KEY_LEFTSHIFT, 0);

    r = press(SDL_SCANCODE_SPACE, KEY_SPACE);
    CHECK(r.got_key && r.key == SDLK_SPACE, "KEY_DOWN space -> SDLK_SPACE (0x%x)", (unsigned)r.key);
    r = press(SDL_SCANCODE_1, KEY_1);
    CHECK(r.got_key && r.key == SDLK_1, "KEY_DOWN 1 -> SDLK_1");

    /* --- held keys: repeats carry text (core path, then the raw evdev
     * window-system path, where kernel repeats are dropped in favour of
     * SDL's own); then with repeat turned off by the window system
     * (wl_keyboard.repeat_info rate 0, xset r off) ----------------------- */
    SDLOP_RawInputSetRepeatInfo(100, 20);
    for (int pass = 0; pass < 3; pass++) {
        const bool raw_path = pass == 1;
        const bool disabled = pass == 2;
        sdlop.repeat_disabled = disabled;
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        SDLop_RawEvent re;
        memset(&re, 0, sizeof(re));
        re.type = 1; /* EV_KEY */
        re.code = KEY_A;
        re.value = 1;
        if (raw_path) {
            re.timestamp_ns = SDLOP_MonotonicNS();
            SDLOP_RawInputTestFeed(&re, true);
        } else {
            SDLOP_SendKeyboardKey(true, false, SDL_SCANCODE_A, KEY_A, 0);
        }
        int downs = 0, repeats = 0, texts = 0, kernel = 0;
        const Uint64 end = SDL_GetTicks() + 400;
        while (SDL_GetTicks() < end) {
            if (raw_path && kernel < 10) { /* kernel autorepeat (value 2) arrives too */
                re.value = 2;
                re.timestamp_ns = SDLOP_MonotonicNS();
                SDLOP_RawInputTestFeed(&re, true);
                kernel++;
            }
            SDL_PumpEvents();
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_A) {
                    downs++;
                    repeats += e.key.repeat;
                } else if (e.type == SDL_EVENT_TEXT_INPUT && strcmp(e.text.text, "a") == 0) {
                    texts++;
                }
            }
            SDL_Delay(5);
        }
        re.value = 0;
        if (raw_path) {
            SDLOP_RawInputTestFeed(&re, true);
        } else {
            SDLOP_SendKeyboardKey(false, false, SDL_SCANCODE_A, KEY_A, 0);
        }
        const char *what = raw_path ? "raw evdev path" : "core path";
        if (disabled) {
            CHECK(downs == 1 && repeats == 0 && texts == 1, "repeat off: one key down, no repeats (%d downs, %d texts)",
                  downs, texts);
            sdlop.repeat_disabled = false;
            continue;
        }
        /* 400 ms held, 100 ms delay, 20 ms interval: ~15 repeats */
        CHECK(repeats >= 8 && repeats <= 20 && downs == repeats + 1, "%s: held key repeats (%d downs, %d repeats)", what,
              downs, repeats);
        CHECK(texts == downs, "%s: one TEXT_INPUT per key down incl. repeats (%d texts, %d downs)", what, texts,
              downs);
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    }
    SDL_StopTextInput(w);

    SDL_DestroyWindow(w);
    SDL_Quit();
    if (failures) {
        printf("test_keycodes: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_keycodes: PASS\n");
    return 0;
}
