/*
  SDLop test: SDL3 keyboard-layout semantics on the native (xkbcommon) path,
  headless (dummy driver), with real xkeyboard-config layouts selected via
  XKB_DEFAULT_LAYOUT before each SDL_Init.

  - non-Latin layouts (ru): key events carry the US QWERTY keycodes
    (SDL3 default option "latin_letters"), key_event=false gives the layout's
    own characters, text follows the layout
  - French number row: key events report digits ("french_numbers")
  - unicode keycodes beyond Latin-1, dead keys -> spacing characters,
    AltGr -> SDLK_MODE, SDL_GetScancodeFromKey for layout characters
  - SDL_GetKeyName capitalizes through the layout ("Ü")
  - multi-layout keymaps follow the active group (+ KEYMAP_CHANGED)
  - dead keys / Compose via libxkbcommon compose: ´ + e -> "é"

  Skips (exit 0) when libxkbcommon or the layout data is unavailable.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include "internal/sdlop_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
enum {
    KEY_GRAVE_ = 41, KEY_1_ = 2, KEY_2_ = 3, KEY_EQUAL_ = 13, KEY_Q_ = 16, KEY_E_ = 18,
    KEY_LEFTBRACE_ = 26, KEY_A_ = 30, KEY_LEFTSHIFT_ = 42, KEY_RIGHTALT_ = 100
};

static SDL_Window *window;

static bool start(const char *layout)
{
    setenv("XKB_DEFAULT_LAYOUT", layout, 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    window = SDL_CreateWindow("layouts", 64, 64, 0);
    SDL_StartTextInput(window);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    return window != NULL;
}

static void stop(void)
{
    SDL_DestroyWindow(window);
    window = NULL;
    SDL_Quit();
}

static void key(SDL_Scancode sc, Uint16 evdev, bool down)
{
    SDLOP_SendKeyboardKey(down, false, sc, evdev, 0);
}

typedef struct
{
    SDL_Keycode key;   /* keycode of the last KEY_DOWN */
    char text[64];     /* concatenated TEXT_INPUT */
    int keymap_changed;
} result;

static result collect(void)
{
    result r;
    memset(&r, 0, sizeof(r));
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_KEY_DOWN) {
            r.key = e.key.key;
        } else if (e.type == SDL_EVENT_TEXT_INPUT) {
            strncat(r.text, e.text.text, sizeof(r.text) - strlen(r.text) - 1);
        } else if (e.type == SDL_EVENT_KEYMAP_CHANGED) {
            r.keymap_changed++;
        }
    }
    return r;
}

/* press + release (optionally with a modifier held), return the events */
static result tap_mod(SDL_Scancode sc, Uint16 evdev, SDL_Scancode msc, Uint16 mevdev)
{
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    if (mevdev) {
        key(msc, mevdev, true);
    }
    key(sc, evdev, true);
    key(sc, evdev, false);
    if (mevdev) {
        key(msc, mevdev, false);
    }
    return collect();
}

static result tap(SDL_Scancode sc, Uint16 evdev)
{
    return tap_mod(sc, evdev, SDL_SCANCODE_UNKNOWN, 0);
}

static bool have_compose_data(void)
{
    const char *dirs[] = { "/usr/share/X11/locale/en_US.UTF-8/Compose",
                           "/usr/lib/X11/locale/en_US.UTF-8/Compose" };
    for (size_t i = 0; i < SDL_arraysize(dirs); i++) {
        if (access(dirs[i], R_OK) == 0) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    unsetenv("XKB_DEFAULT_VARIANT");
    unsetenv("XKB_DEFAULT_OPTIONS");
    const char *xkb_env = getenv("SDLOP_LIB_XKB");
    if (xkb_env && strcmp(xkb_env, "none") == 0) {
        printf("test_layouts: SKIP (libxkbcommon disabled)\n");
        return 0;
    }

    /* ---- ru: non-Latin layout ------------------------------------------ */
    if (!start("ru")) {
        return 1;
    }
    {
        /* layout data present? (the typed text only needs xkbcommon) */
        result probe = tap(SDL_SCANCODE_Q, KEY_Q_);
        if (strcmp(probe.text, "\xd0\xb9") != 0) {
            printf("test_layouts: SKIP (no xkb layout data for 'ru': Q types '%s')\n", probe.text);
            stop();
            return getenv("SDLOP_TEST_STRICT") ? 1 : 0; /* CI: data must be there */
        }
    }
    {
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, false) == 0x439, "ru: layout Q -> 'й'");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, true) == SDLK_Q,
              "ru: key event Q -> SDLK_Q (latin_letters), got 0x%x",
              (unsigned)SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, true));
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_SHIFT, false) == 0x419, "ru: shift+Q -> 'Й'");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_LEFTBRACKET, SDL_KMOD_NONE, true) == SDLK_LEFTBRACKET,
              "ru: key event [ -> '[' (whole default keymap)");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_2, SDL_KMOD_SHIFT, false) == '"', "ru: shift+2 -> '\"'");
        SDL_Keymod m = SDL_KMOD_CTRL;
        CHECK(SDL_GetScancodeFromKey(0x439, &m) == SDL_SCANCODE_Q && m == SDL_KMOD_NONE, "ru: 'й' -> Q");
        CHECK(SDL_GetScancodeFromKey(0x419, &m) == SDL_SCANCODE_Q && (m & SDL_KMOD_SHIFT), "ru: 'Й' -> Q + shift");
        CHECK(SDL_GetScancodeFromKey(SDLK_Q, &m) == SDL_SCANCODE_Q, "ru: SDLK_Q -> Q (default keymap fallback)");
        CHECK(strcmp(SDL_GetKeyName(SDLK_Q), "Q") == 0, "ru: name of SDLK_Q");
        result r = tap(SDL_SCANCODE_Q, KEY_Q_);
        CHECK(r.key == SDLK_Q, "ru: KEY_DOWN Q carries SDLK_Q (0x%x)", (unsigned)r.key);
        CHECK(strcmp(r.text, "\xd0\xb9") == 0, "ru: Q types 'й' (got '%s')", r.text);
        r = tap_mod(SDL_SCANCODE_Q, KEY_Q_, SDL_SCANCODE_LSHIFT, KEY_LEFTSHIFT_);
        CHECK(r.key == SDLK_Q && strcmp(r.text, "\xd0\x99") == 0, "ru: shift+Q -> SDLK_Q, 'Й' (got '%s')", r.text);
        stop();
    }

    /* ---- fr: AZERTY, French number row --------------------------------- */
    if (!start("fr")) {
        return 1;
    }
    {
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, true) == SDLK_A, "fr: Q key -> SDLK_A");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_1, SDL_KMOD_NONE, true) == SDLK_1, "fr: key event 1 -> '1' (french_numbers)");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_1, SDL_KMOD_NONE, false) == '&', "fr: unshifted 1 key is '&'");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_2, SDL_KMOD_NONE, false) == 0xe9, "fr: unshifted 2 key is 'é'");
        SDL_Keymod m = SDL_KMOD_NONE;
        CHECK(SDL_GetScancodeFromKey(SDLK_A, &m) == SDL_SCANCODE_Q && m == SDL_KMOD_NONE, "fr: SDLK_A -> Q");
        CHECK(SDL_GetScancodeFromKey(SDLK_2, &m) == SDL_SCANCODE_2 && (m & SDL_KMOD_SHIFT), "fr: '2' -> 2 + shift");
        CHECK(SDL_GetScancodeFromKey(0xe9, &m) == SDL_SCANCODE_2 && m == SDL_KMOD_NONE, "fr: 'é' -> 2");
        result r = tap(SDL_SCANCODE_2, KEY_2_);
        CHECK(r.key == SDLK_2 && strcmp(r.text, "\xc3\xa9") == 0, "fr: 2 key -> SDLK_2, types 'é' (0x%x '%s')",
              (unsigned)r.key, r.text);
        stop();
    }

    /* ---- de: umlauts, dead keys, AltGr, compose ------------------------- */
    if (!start("de")) {
        return 1;
    }
    {
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_LEFTBRACKET, SDL_KMOD_NONE, true) == 0xfc, "de: [ key -> 'ü'");
        CHECK(strcmp(SDL_GetKeyName(0xfc), "\xc3\x9c") == 0, "de: name of 'ü' is 'Ü' (got '%s')", SDL_GetKeyName(0xfc));
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_EQUALS, SDL_KMOD_NONE, true) == 0xb4,
              "de: dead_acute -> '´' (0x%x)", (unsigned)SDL_GetKeyFromScancode(SDL_SCANCODE_EQUALS, SDL_KMOD_NONE, true));
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_GRAVE, SDL_KMOD_NONE, true) == '^', "de: dead_circumflex -> '^'");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_RALT, SDL_KMOD_NONE, true) == SDLK_MODE, "de: AltGr -> SDLK_MODE");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_MODE, false) == '@', "de: AltGr+Q -> '@'");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Y, SDL_KMOD_NONE, true) == SDLK_Z, "de: QWERTZ Y key -> SDLK_Z");
        SDL_Keymod m = SDL_KMOD_NONE;
        CHECK(SDL_GetScancodeFromKey('@', &m) == SDL_SCANCODE_Q && (m & SDL_KMOD_MODE), "de: '@' -> Q + AltGr");

        result r = tap_mod(SDL_SCANCODE_Q, KEY_Q_, SDL_SCANCODE_RALT, KEY_RIGHTALT_);
        CHECK(r.key == SDLK_Q && strcmp(r.text, "@") == 0, "de: AltGr+Q types '@' (got '%s')", r.text);

        if (have_compose_data()) {
            r = tap(SDL_SCANCODE_EQUALS, KEY_EQUAL_);
            CHECK(r.key == 0xb4 && r.text[0] == '\0', "compose: dead key alone types nothing (got '%s')", r.text);
            r = tap(SDL_SCANCODE_E, KEY_E_);
            CHECK(r.key == SDLK_E && strcmp(r.text, "\xc3\xa9") == 0, "compose: ´ + e -> 'é' (got '%s')", r.text);
            tap(SDL_SCANCODE_GRAVE, KEY_GRAVE_);
            r = tap_mod(SDL_SCANCODE_A, KEY_A_, SDL_SCANCODE_LSHIFT, KEY_LEFTSHIFT_);
            CHECK(strcmp(r.text, "\xc3\x82") == 0, "compose: ^ + shift+a -> 'Â' (got '%s')", r.text);
            tap(SDL_SCANCODE_EQUALS, KEY_EQUAL_);
            r = tap(SDL_SCANCODE_EQUALS, KEY_EQUAL_);
            CHECK(strcmp(r.text, "\xc2\xb4") == 0, "compose: ´ + ´ -> '´' (got '%s')", r.text);
            tap(SDL_SCANCODE_EQUALS, KEY_EQUAL_);
            r = tap(SDL_SCANCODE_Q, KEY_Q_);
            CHECK(r.text[0] == '\0', "compose: ´ + q cancels without text (got '%s')", r.text);
            r = tap(SDL_SCANCODE_E, KEY_E_);
            CHECK(strcmp(r.text, "e") == 0, "compose: plain text after a cancelled sequence (got '%s')", r.text);
            /* focus loss (SDL_ResetKeyboard) abandons a pending dead key */
            tap(SDL_SCANCODE_EQUALS, KEY_EQUAL_);
            SDL_ResetKeyboard();
            r = tap(SDL_SCANCODE_E, KEY_E_);
            CHECK(strcmp(r.text, "e") == 0, "compose: reset on focus loss (got '%s')", r.text);
        } else {
            printf("test_layouts: compose checks skipped (no X11 Compose data)\n");
            CHECK(!getenv("SDLOP_TEST_STRICT"), "SDLOP_TEST_STRICT: compose data missing");
        }
        stop();
    }

    /* ---- us,ru: group switching ----------------------------------------- */
    if (!start("us,ru")) {
        return 1;
    }
    {
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, false) == SDLK_Q, "us,ru: group 1 is us");
        result r = tap(SDL_SCANCODE_Q, KEY_Q_);
        CHECK(r.keymap_changed == 0 && strcmp(r.text, "q") == 0, "us,ru: types 'q', no keymap change");

        /* the window system reports the locked group (Wayland modifiers /
         * XKB state notify) */
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        SDLOP_KeyboardUpdateXkbState(0, 0, 0, 0, 0, 1);
        r = collect();
        CHECK(r.keymap_changed == 1, "us,ru: switching to ru sends KEYMAP_CHANGED (%d)", r.keymap_changed);
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, false) == 0x439, "us,ru: group 2 Q -> 'й'");
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, true) == SDLK_Q, "us,ru: group 2 key event -> SDLK_Q");
        SDL_Keymod m;
        CHECK(SDL_GetScancodeFromKey(0x439, &m) == SDL_SCANCODE_Q, "us,ru: 'й' -> Q in group 2");
        r = tap(SDL_SCANCODE_Q, KEY_Q_);
        CHECK(r.key == SDLK_Q && strcmp(r.text, "\xd0\xb9") == 0, "us,ru: group 2 types 'й' (got '%s')", r.text);

        SDLOP_KeyboardUpdateXkbState(0, 0, 0, 0, 0, 1); /* same group again: no event */
        SDLOP_KeyboardUpdateXkbState(0, 0, 0, 0, 0, 0);
        r = collect();
        CHECK(r.keymap_changed == 1, "us,ru: back to us sends exactly one KEYMAP_CHANGED (%d)", r.keymap_changed);
        CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, false) == SDLK_Q, "us,ru: back to 'q'");
        stop();
    }

    if (failures) {
        printf("test_layouts: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_layouts: PASS\n");
    return 0;
}
