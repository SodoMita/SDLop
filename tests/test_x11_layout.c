/*
  SDLop test: keyboard layouts end-to-end through the X11 driver.

  Switches the X server keymap with setxkbmap while SDLop runs and drives
  keys through XTest:
  - the driver reloads the keymap on XKB NewKeyboard/MapNotify
    (SDL_EVENT_KEYMAP_CHANGED) and keycodes/text follow it
  - de: umlaut keycodes, dead keys composed into text (´ + e -> "é")
  - ru: key events keep the QWERTY keycodes, text is Cyrillic
  - us,ru: locking the second group (XkbLatchLockState) switches layouts

  The original server layout is restored at the end. Skips without DISPLAY,
  setxkbmap, or XKB.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xkb.h>
#include <xcb/xtest.h>

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

/* X keycodes (= evdev + 8) */
enum { XK_EQUAL = 21, XK_Q = 24, XK_E = 26, XK_LEFTBRACE = 34, XK_A = 38 };

static xcb_connection_t *obs;
static char saved_layout[128] = "us";
static char saved_variant[128] = "";
static char saved_options[256] = "";

static bool run(const char *fmt, ...)
{
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    return system(cmd) == 0;
}

static void save_layout(void)
{
    FILE *f = popen("setxkbmap -query 2>/dev/null", "r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, ':');
        if (!v) {
            continue;
        }
        v++;
        while (*v == ' ') {
            v++;
        }
        v[strcspn(v, "\n")] = '\0';
        if (strncmp(line, "layout:", 7) == 0) {
            snprintf(saved_layout, sizeof(saved_layout), "%s", v);
        } else if (strncmp(line, "variant:", 8) == 0) {
            snprintf(saved_variant, sizeof(saved_variant), "%s", v);
        } else if (strncmp(line, "options:", 8) == 0) {
            snprintf(saved_options, sizeof(saved_options), "%s", v);
        }
    }
    pclose(f);
}

static void restore_layout(void)
{
    run("setxkbmap -option '' -layout '%s' -variant '%s' %s%s%s", saved_layout, saved_variant,
        saved_options[0] ? "-option '" : "", saved_options, saved_options[0] ? "'" : "");
}

/* first keysym of an X keycode in the server's core mapping */
static xcb_keysym_t server_keysym(uint8_t keycode)
{
    xcb_get_keyboard_mapping_reply_t *r =
        xcb_get_keyboard_mapping_reply(obs, xcb_get_keyboard_mapping(obs, keycode, 1), NULL);
    xcb_keysym_t sym = 0;
    if (r && r->keysyms_per_keycode > 0) {
        sym = xcb_get_keyboard_mapping_keysyms(r)[0];
    }
    free(r);
    return sym;
}

static void fake_key(uint8_t keycode, bool down)
{
    xcb_test_fake_input(obs, down ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
    xcb_flush(obs);
}

static void tap(uint8_t keycode)
{
    fake_key(keycode, true);
    fake_key(keycode, false);
}

/* every injected request processed, SDLop has read the results */
static void sync_input(void)
{
    free(xcb_get_input_focus_reply(obs, xcb_get_input_focus(obs), NULL));
    Uint64 end = SDL_GetTicks() + 40;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
}

typedef struct
{
    SDL_Keycode key; /* last KEY_DOWN */
    int keys;
    char text[64];
    int keymap_changed;
} result;

static result collect(int settle_ms)
{
    result r;
    memset(&r, 0, sizeof(r));
    Uint64 end = SDL_GetTicks() + (Uint64)settle_ms;
    sync_input();
    SDL_Event e;
    do {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_KEY_DOWN) {
                r.key = e.key.key;
                r.keys++;
            } else if (e.type == SDL_EVENT_TEXT_INPUT) {
                strncat(r.text, e.text.text, sizeof(r.text) - strlen(r.text) - 1);
            } else if (e.type == SDL_EVENT_KEYMAP_CHANGED) {
                r.keymap_changed++;
            }
        }
        SDL_PumpEvents();
        SDL_Delay(5);
    } while (SDL_GetTicks() < end);
    return r;
}

static void drain(void)
{
    sync_input();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
}

/* switch the server layout; wait for SDLop to report the new keymap */
static bool switch_layout(const char *layout, xcb_keysym_t expect_sym, uint8_t at_keycode, int *keymap_events)
{
    drain();
    if (!run("setxkbmap -option '' -variant '' -layout '%s'", layout)) {
        return false;
    }
    Uint64 end = SDL_GetTicks() + 2000;
    while (server_keysym(at_keycode) != expect_sym && SDL_GetTicks() < end) {
        SDL_Delay(10);
    }
    if (server_keysym(at_keycode) != expect_sym) {
        return false;
    }
    result r = collect(150);
    *keymap_events = r.keymap_changed;
    return true;
}

static bool lock_group(int group)
{
    xcb_void_cookie_t c = xcb_xkb_latch_lock_state_checked(obs, XCB_XKB_ID_USE_CORE_KBD, 0, 0, 1, (uint8_t)group,
                                                           0, 0, 0);
    xcb_generic_error_t *err = xcb_request_check(obs, c);
    free(err);
    return err == NULL;
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_x11_layout: SKIP (DISPLAY not set)\n");
        return 0;
    }
    if (system("command -v setxkbmap >/dev/null 2>&1") != 0) {
        printf("test_x11_layout: SKIP (setxkbmap not installed)\n");
        return getenv("SDLOP_TEST_STRICT") ? 1 : 0;
    }
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDL_VIDEODRIVER", "x11", 1);

    /* the observer connection also keeps the server from resetting (and
     * dropping the keymap) between setxkbmap runs */
    obs = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(obs)) {
        printf("test_x11_layout: SKIP (cannot connect)\n");
        return 0;
    }
    xcb_xkb_use_extension_reply_t *ue =
        xcb_xkb_use_extension_reply(obs, xcb_xkb_use_extension(obs, 1, 0), NULL);
    bool have_xkb = ue && ue->supported;
    free(ue);
    if (!have_xkb) {
        printf("test_x11_layout: SKIP (no XKB)\n");
        return 0;
    }
    save_layout();
    printf("test_x11_layout: %s, server layout '%s'\n", getenv("DISPLAY"), saved_layout);

    if (!run("setxkbmap -option '' -variant '' -layout us") || server_keysym(XK_Q) != 'q') {
        printf("test_x11_layout: SKIP (cannot set the server keymap)\n");
        restore_layout();
        return getenv("SDLOP_TEST_STRICT") ? 1 : 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        restore_layout();
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("SDLop x11 layout test", 200, 120, 0);
    CHECK(w != NULL, "window");
    SDL_ShowWindow(w);
    SDL_RaiseWindow(w);
    {
        Uint64 end = SDL_GetTicks() + 2000;
        while (SDL_GetKeyboardFocus() != w && SDL_GetTicks() < end) {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
    }
    CHECK(SDL_GetKeyboardFocus() == w, "keyboard focus");
    SDL_StartTextInput(w);

    drain();
    tap(XK_Q);
    result r = collect(50);
    CHECK(r.key == SDLK_Q && strcmp(r.text, "q") == 0, "us: q (0x%x '%s')", (unsigned)r.key, r.text);

    /* ---- de ---------------------------------------------------------- */
    int kc = 0;
    CHECK(switch_layout("de", 0xfc /* udiaeresis */, XK_LEFTBRACE, &kc), "server layout de");
    CHECK(kc >= 1, "de: KEYMAP_CHANGED (%d)", kc);
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_LEFTBRACKET, SDL_KMOD_NONE, true) == 0xfc, "de: [ key -> 'ü'");
    drain();
    tap(XK_LEFTBRACE);
    r = collect(50);
    CHECK(r.key == 0xfc && strcmp(r.text, "\xc3\xbc") == 0, "de: ü key (0x%x '%s')", (unsigned)r.key, r.text);
    drain();
    tap(XK_EQUAL); /* dead_acute */
    tap(XK_E);
    r = collect(50);
    CHECK(r.keys == 2 && strcmp(r.text, "\xc3\xa9") == 0, "de: dead acute + e -> 'é' (%d keys, '%s')", r.keys, r.text);

    /* ---- ru ---------------------------------------------------------- */
    CHECK(switch_layout("ru", 0x6ca /* Cyrillic_shorti */, XK_Q, &kc), "server layout ru");
    CHECK(kc >= 1, "ru: KEYMAP_CHANGED (%d)", kc);
    drain();
    tap(XK_Q);
    r = collect(50);
    CHECK(r.key == SDLK_Q && strcmp(r.text, "\xd0\xb9") == 0, "ru: Q key -> SDLK_Q, types 'й' (0x%x '%s')",
          (unsigned)r.key, r.text);
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, false) == 0x439, "ru: layout Q -> 'й'");

    /* ---- us,ru: group switching -------------------------------------- */
    CHECK(switch_layout("us,ru", 'q', XK_Q, &kc), "server layout us,ru");
    lock_group(0);
    drain();
    tap(XK_Q);
    r = collect(50);
    CHECK(strcmp(r.text, "q") == 0, "us,ru group 1 types 'q' ('%s')", r.text);
    drain();
    CHECK(lock_group(1), "lock group 2");
    r = collect(150);
    CHECK(r.keymap_changed == 1, "group switch sends KEYMAP_CHANGED (%d)", r.keymap_changed);
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, false) == 0x439, "group 2: Q -> 'й'");
    tap(XK_Q);
    r = collect(50);
    CHECK(r.key == SDLK_Q && strcmp(r.text, "\xd0\xb9") == 0, "group 2 types 'й' (0x%x '%s')", (unsigned)r.key,
          r.text);
    lock_group(0);
    r = collect(150);
    CHECK(r.keymap_changed == 1 && SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, false) == SDLK_Q,
          "back to group 1 (%d)", r.keymap_changed);

    SDL_StopTextInput(w);
    SDL_DestroyWindow(w);
    restore_layout();
    SDL_Quit();
    xcb_disconnect(obs);

    if (failures) {
        printf("test_x11_layout: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_x11_layout: PASS\n");
    return 0;
}
