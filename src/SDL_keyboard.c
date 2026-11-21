/*
  SDLop - keyboard handling + xkbcommon integration.

  Keys arrive as SDL scancodes (converted from evdev raw codes or Wayland
  key events). This file maintains the key state array, modifier state,
  key repeat emulation, and xkb-based layout translation (keycode + text).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "internal/scancode_evdev.h"
#ifndef _WIN32
#include "internal/sdlop_xkb_dyn.h"
#else
#include "internal/sdlop_win32_keymap.h"
#endif

/* ------------------------------------------------------------------ */
/* scancode -> evdev reverse table (built once)                        */
/* ------------------------------------------------------------------ */

#ifndef _WIN32
/* Windows has no evdev codes: the driver resolves VKs through the Win32
 * keymap, so the reverse table and the text filter below are POSIX-only. */
static Uint16 scancode_to_evdev[SDL_SCANCODE_COUNT];
static bool tables_ready;

static void build_tables(void)
{
    for (int i = 0; i <= SDLOP_EVDEV_KEY_MAX; i++) {
        Uint16 sc = sdlop_evdev_to_scancode[i];
        if (sc > 0 && sc < SDL_SCANCODE_COUNT && scancode_to_evdev[sc] == 0) {
            scancode_to_evdev[sc] = (Uint16)i;
        }
    }
    tables_ready = true;
}

static Uint16 evdev_from_scancode(SDL_Scancode sc)
{
    if (!tables_ready) {
        build_tables();
    }
    if (sc >= 0 && sc < SDL_SCANCODE_COUNT) {
        return scancode_to_evdev[sc];
    }
    return 0;
}
#endif /* !_WIN32 */

/* ------------------------------------------------------------------ */
/* xkb state                                                           */
/* ------------------------------------------------------------------ */

/* shared by both platforms (see below) */
static SDL_Keycode default_key_from_scancode(SDL_Scancode sc, SDL_Keymod mod);

#ifdef _WIN32
/* No compose table on Windows: the system IME hands over finished text. */
static void compose_reset(void) { }
#endif

#ifndef _WIN32
static struct xkb_context *xkb_ctx;
static struct xkb_keymap *keymap;
static struct xkb_state *xkb_st;
static struct xkb_state *xkb_st_clean; /* no modifiers, for key resolution */
static xkb_mod_index_t mod_shift, mod_ctrl, mod_alt, mod_super, mod_caps, mod_num, mod_altgr, mod_level5;

/* Active layout (xkb group) and the SDL3 keymap-layout flags derived from
 * it (SDL_DetermineKeymapLayout): with the default SDL3 keycode options
 * ("french_numbers,latin_letters"), key events on a non-Latin layout carry
 * the US QWERTY keycodes, and a French number row reports digits. */
static xkb_layout_index_t cur_layout;
static bool suppress_layout_event;
static Uint32 layout_flags;
#define LAYOUT_FRENCH_NUMBERS 0x01
#define LAYOUT_LATIN_LETTERS  0x02
#define LAYOUT_THAI           0x04

/* dead-key / Compose-key composition (libxkbcommon compose, locale table) */
static struct xkb_compose_table *compose_table;
static struct xkb_compose_state *compose_st;
static bool compose_tried;

/* keycode -> (scancode, modstate) for SDL_GetScancodeFromKey, filled from
 * the active layout level by level (none, shift, AltGr, AltGr+shift) so the
 * first, least-modified match wins - like SDL3's keymap (open addressing) */
#define REVTAB_SIZE 2048
static struct { SDL_Keycode key; Uint16 sc; Uint16 mod; } revtab[REVTAB_SIZE];

static void revtab_put(SDL_Keycode key, SDL_Scancode sc, SDL_Keymod mod)
{
    uint32_t h = ((uint32_t)key * 2654435761u) & (REVTAB_SIZE - 1);
    for (int n = 0; n < REVTAB_SIZE && revtab[h].key != 0; n++) {
        if (revtab[h].key == key) {
            return; /* first (least modified) entry wins */
        }
        h = (h + 1) & (REVTAB_SIZE - 1);
    }
    if (revtab[h].key == 0) {
        revtab[h].key = key;
        revtab[h].sc = (Uint16)sc;
        revtab[h].mod = (Uint16)mod;
    }
}

static bool revtab_get(SDL_Keycode key, SDL_Scancode *sc, SDL_Keymod *mod)
{
    uint32_t h = ((uint32_t)key * 2654435761u) & (REVTAB_SIZE - 1);
    for (int n = 0; n < REVTAB_SIZE && revtab[h].key != 0; n++) {
        if (revtab[h].key == key) {
            *sc = (SDL_Scancode)revtab[h].sc;
            *mod = (SDL_Keymod)revtab[h].mod;
            return true;
        }
        h = (h + 1) & (REVTAB_SIZE - 1);
    }
    return false;
}

#ifndef _WIN32

static SDL_Keycode keysym_to_keycode(xkb_keysym_t sym);


static void find_mod_indices(void)
{
    mod_shift  = SDLOP_XKB_xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);   /* "Shift" */
    mod_ctrl   = SDLOP_XKB_xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);    /* "Control" */
    mod_alt    = SDLOP_XKB_xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);     /* "Mod1" */
    mod_super  = SDLOP_XKB_xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_LOGO);    /* "Mod4" */
    mod_caps   = SDLOP_XKB_xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CAPS);    /* "Lock" */
    mod_num    = SDLOP_XKB_xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_NUM);     /* "Mod2" */
    mod_altgr  = SDLOP_XKB_xkb_keymap_mod_get_index(keymap, "Mod5");   /* "Mod5" */
    mod_level5 = SDLOP_XKB_xkb_keymap_mod_get_index(keymap, "Mod3");               /* ISO_Level5_Shift */
}

static xkb_mod_mask_t xkb_mask_for(SDL_Keymod modstate)
{
    xkb_mod_mask_t mask = 0;
    if ((modstate & SDL_KMOD_SHIFT) && mod_shift != XKB_MOD_INVALID) {
        mask |= (xkb_mod_mask_t)1 << mod_shift;
    }
    if ((modstate & SDL_KMOD_CAPS) && mod_caps != XKB_MOD_INVALID) {
        mask |= (xkb_mod_mask_t)1 << mod_caps;
    }
    if ((modstate & SDL_KMOD_MODE) && mod_altgr != XKB_MOD_INVALID) {
        mask |= (xkb_mod_mask_t)1 << mod_altgr;
    }
    if ((modstate & SDL_KMOD_LEVEL5) && mod_level5 != XKB_MOD_INVALID) {
        mask |= (xkb_mod_mask_t)1 << mod_level5;
    }
    return mask;
}

/* the clean state carries no modifiers, only the active layout */
static void clean_state_set(SDL_Keymod modstate)
{
    SDLOP_XKB_xkb_state_update_mask(xkb_st_clean, xkb_mask_for(modstate), 0, 0, 0, 0, cur_layout);
}

/* keysym of a key on the active layout at the given shift level */
static xkb_keysym_t layout_keysym(Uint16 evdev, SDL_Keymod modstate)
{
    if (modstate) {
        clean_state_set(modstate);
    }
    xkb_keysym_t sym = SDLOP_XKB_xkb_state_key_get_one_sym(xkb_st_clean, (xkb_keycode_t)evdev + 8);
    if (modstate) {
        clean_state_set(SDL_KMOD_NONE);
    }
    return sym;
}

static SDL_Keycode keycode_from_sym(xkb_keysym_t sym, SDL_Scancode sc)
{
    SDL_Keycode key = keysym_to_keycode(sym);
    if (key == SDLK_UNKNOWN && sc != SDL_SCANCODE_UNKNOWN) {
        /* no symbol / no unicode (XF86 keys ...): the scancode keycode */
        key = SDL_SCANCODE_TO_KEYCODE(sc);
    }
    return key;
}

/* SDL3 SDL_GetKeymapKeycode(keyboard->keymap, ...): the active layout */
static SDL_Keycode layout_keycode(SDL_Scancode sc, SDL_Keymod modstate)
{
    Uint16 evdev = evdev_from_scancode(sc);
    if (!evdev) {
        return SDL_SCANCODE_TO_KEYCODE(sc);
    }
    if (!keymap) {
        return default_key_from_scancode(sc, modstate);
    }
    return keycode_from_sym(layout_keysym(evdev, modstate), sc);
}

/* SDL3 key_event semantics: SDL_GetCurrentKeymap(false) is the default US
 * keymap for non-Latin / Thai layouts; modifiers are ignored except that a
 * French number row is looked up shifted (digits) */
static SDL_Keycode event_keycode(SDL_Scancode sc)
{
    if (!keymap || !(layout_flags & LAYOUT_LATIN_LETTERS) || (layout_flags & LAYOUT_THAI)) {
        return default_key_from_scancode(sc, SDL_KMOD_NONE);
    }
    SDL_Keymod mod = SDL_KMOD_NONE;
    if ((layout_flags & LAYOUT_FRENCH_NUMBERS) && sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) {
        mod = SDL_KMOD_SHIFT;
    }
    return layout_keycode(sc, mod);
}

static bool is_ascii_digit(SDL_Keycode k)
{
    return k >= '0' && k <= '9';
}

/* port of SDL_DetermineKeymapLayout + the keycode->scancode table */
static void build_layout_tables(void)
{
    clean_state_set(SDL_KMOD_NONE);

    layout_flags = LAYOUT_FRENCH_NUMBERS;
    for (int i = SDL_SCANCODE_1; i <= SDL_SCANCODE_0; i++) {
        if (is_ascii_digit(layout_keycode((SDL_Scancode)i, SDL_KMOD_NONE)) ||
            !is_ascii_digit(layout_keycode((SDL_Scancode)i, SDL_KMOD_SHIFT))) {
            layout_flags &= ~LAYOUT_FRENCH_NUMBERS;
            break;
        }
    }
    for (int i = SDL_SCANCODE_A; i <= SDL_SCANCODE_D; i++) {
        const SDL_Keycode key = layout_keycode((SDL_Scancode)i, SDL_KMOD_NONE);
        if (key <= 0xFF) {
            layout_flags |= LAYOUT_LATIN_LETTERS;
            break;
        }
        if (key >= 0x0E00 && key <= 0x0E7F) {
            layout_flags |= LAYOUT_THAI;
            break;
        }
    }

    static const SDL_Keymod levels[] = {
        SDL_KMOD_NONE, SDL_KMOD_SHIFT, SDL_KMOD_MODE, SDL_KMOD_MODE | SDL_KMOD_SHIFT
    };
    memset(revtab, 0, sizeof(revtab));
    for (size_t l = 0; l < SDL_arraysize(levels); l++) {
        clean_state_set(levels[l]);
        for (uint16_t evdev = 1; evdev <= SDLOP_EVDEV_KEY_MAX; evdev++) {
            SDL_Scancode sc = (SDL_Scancode)sdlop_evdev_to_scancode[evdev];
            if (sc == SDL_SCANCODE_UNKNOWN || evdev_from_scancode(sc) != evdev) {
                continue;
            }
            xkb_keysym_t sym = SDLOP_XKB_xkb_state_key_get_one_sym(xkb_st_clean, (xkb_keycode_t)evdev + 8);
            if (sym == XKB_KEY_NoSymbol) {
                continue;
            }
            SDL_Keycode key = keysym_to_keycode(sym);
            if (key != SDLK_UNKNOWN && !(key & SDLK_SCANCODE_MASK)) {
                revtab_put(key, sc, levels[l]);
            }
        }
    }
    clean_state_set(SDL_KMOD_NONE);
}

/* follow the live state's layout group (multi-layout keymaps: us,ru ...);
 * a switch rebuilds the tables and is reported like SDL3 (KEYMAP_CHANGED) */
static void check_layout_change(void)
{
    if (!xkb_st) {
        return;
    }
    xkb_layout_index_t layout = SDLOP_XKB_xkb_state_serialize_layout(xkb_st, XKB_STATE_LAYOUT_EFFECTIVE);
    bool suppress = suppress_layout_event;
    suppress_layout_event = false;
    if (layout == cur_layout) {
        return;
    }
    cur_layout = layout;
    build_layout_tables();
    if (!suppress) {
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_EVENT_KEYMAP_CHANGED;
        event.common.timestamp = SDL_GetTicksNS();
        SDLOP_PushEventInternal(&event);
    }
}

static void compose_init(void)
{
    if (compose_tried || !xkb_ctx || !SDLOP_Xkb_HaveCompose()) {
        return;
    }
    compose_tried = true;
    /* SDL3 order; xkbcommon maps C/POSIX to en_US.UTF-8 and honours
     * XCOMPOSEFILE / ~/.XCompose */
    const char *locale = NULL;
    static const char *const vars[] = { "LC_ALL", "LC_CTYPE", "LANG" };
    for (size_t i = 0; i < SDL_arraysize(vars) && !locale; i++) {
        const char *v = getenv(vars[i]);
        if (v && *v) {
            locale = v;
        }
    }
    if (!locale) {
        locale = "C";
    }
    compose_table = SDLOP_XKB_xkb_compose_table_new_from_locale(xkb_ctx, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (compose_table) {
        compose_st = SDLOP_XKB_xkb_compose_state_new(compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
    }
}

static void compose_reset(void)
{
    if (compose_st) {
        SDLOP_XKB_xkb_compose_state_reset(compose_st);
    }
}

static bool set_keymap(struct xkb_keymap *new_keymap)
{
    bool replacing = (keymap != NULL); /* stock: KEYMAP_CHANGED fires for a replaced keymap, not the first one */
    struct xkb_state *new_state = SDLOP_XKB_xkb_state_new(new_keymap);
    struct xkb_state *new_clean = SDLOP_XKB_xkb_state_new(new_keymap);
    if (!new_state || !new_clean) {
        SDLOP_XKB_xkb_state_unref(new_state);
        SDLOP_XKB_xkb_state_unref(new_clean);
        SDLOP_XKB_xkb_keymap_unref(new_keymap);
        return SDL_SetError("SDLOP_XKB_xkb_state_new() failed");
    }
    if (xkb_st) {
        SDLOP_XKB_xkb_state_unref(xkb_st);
    }
    if (xkb_st_clean) {
        SDLOP_XKB_xkb_state_unref(xkb_st_clean);
    }
    if (keymap) {
        SDLOP_XKB_xkb_keymap_unref(keymap);
    }
    keymap = new_keymap;
    xkb_st = new_state;
    xkb_st_clean = new_clean;
    cur_layout = 0;
    find_mod_indices();
    build_layout_tables();
    /* the state sync that follows a new keymap is part of setting it */
    suppress_layout_event = true;
    compose_init();
    compose_reset();
    if (replacing) {
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_EVENT_KEYMAP_CHANGED; /* app event: common layout */
        event.common.timestamp = SDL_GetTicksNS();
        SDLOP_PushEventInternal(&event);
    }
    return true;
}

bool SDLOP_KeyboardSetKeymapString(const char *keymap_string, size_t length)
{
    if (!SDLOP_Xkb_LoadSymbols()) {
        return false;
    }
    if (!xkb_ctx) {
        xkb_ctx = SDLOP_XKB_xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!xkb_ctx) {
            return SDL_SetError("SDLOP_XKB_xkb_context_new() failed");
        }
    }
    /* The compositor sends `length` bytes over an mmap'd fd; they are NOT
     * readable past that point (and may not be NUL-terminated within it),
     * but xkb requires a C string. Copy and terminate defensively. */
    char *terminated = (char *)malloc(length + 1);
    if (!terminated) {
        return SDL_SetError("Out of memory parsing keymap");
    }
    memcpy(terminated, keymap_string, length);
    terminated[length] = '\0';
    struct xkb_keymap *km = SDLOP_XKB_xkb_keymap_new_from_string(xkb_ctx, terminated, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    free(terminated);
    if (!km) {
        return SDL_SetError("Could not parse Wayland xkb keymap");
    }
    return set_keymap(km);
}

bool SDLOP_KeyboardSetDefaultKeymap(void)
{
    if (!SDLOP_Xkb_LoadSymbols()) {
        return false;
    }
    if (!xkb_ctx) {
        xkb_ctx = SDLOP_XKB_xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!xkb_ctx) {
            return SDL_SetError("SDLOP_XKB_xkb_context_new() failed");
        }
    }
    if (keymap) {
        return true; /* already have one */
    }
    struct xkb_rule_names names = { 0 };
    struct xkb_keymap *km = SDLOP_XKB_xkb_keymap_new_from_names(xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) {
        return SDL_SetError("Could not create default xkb keymap");
    }
    return set_keymap(km);
}

/* rebuild the caps/num/level5/mode bits (and cross-check the L/R bits)
 * from xkb's effective modifier state; shared by the mask-based wayland
 * path and the key-event-based raw path */
static void sdlop_sync_mods_from_xkb(void)
{
    if (!xkb_st) {
        return;
    }
    SDL_Keymod mod = sdlop.modstate & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT | SDL_KMOD_LCTRL | SDL_KMOD_RCTRL | SDL_KMOD_LALT | SDL_KMOD_RALT | SDL_KMOD_LGUI | SDL_KMOD_RGUI);

    /* toggle locks come from xkb; left/right bits are tracked from key events */
    if (SDLOP_XKB_xkb_state_mod_index_is_active(xkb_st, mod_caps, XKB_STATE_MODS_EFFECTIVE) == 1) {
        mod |= SDL_KMOD_CAPS;
    }
    if (SDLOP_XKB_xkb_state_mod_index_is_active(xkb_st, mod_num, XKB_STATE_MODS_EFFECTIVE) == 1) {
        mod |= SDL_KMOD_NUM;
    }
    if (SDLOP_XKB_xkb_state_mod_index_is_active(xkb_st, mod_level5, XKB_STATE_MODS_EFFECTIVE) == 1) {
        mod |= SDL_KMOD_LEVEL5;
    }
    if (SDLOP_XKB_xkb_state_mod_index_is_active(xkb_st, mod_altgr, XKB_STATE_MODS_EFFECTIVE) == 1) {
        mod |= SDL_KMOD_MODE;
    }
    /* keep L/R modifier bits in sync with xkb effective state */
    if (SDLOP_XKB_xkb_state_mod_index_is_active(xkb_st, mod_shift, XKB_STATE_MODS_EFFECTIVE) != 1) {
        mod &= ~(SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT);
    }
    if (SDLOP_XKB_xkb_state_mod_index_is_active(xkb_st, mod_ctrl, XKB_STATE_MODS_EFFECTIVE) != 1) {
        mod &= ~(SDL_KMOD_LCTRL | SDL_KMOD_RCTRL);
    }
    if (SDLOP_XKB_xkb_state_mod_index_is_active(xkb_st, mod_super, XKB_STATE_MODS_EFFECTIVE) != 1) {
        mod &= ~(SDL_KMOD_LGUI | SDL_KMOD_RGUI);
    }
    sdlop.modstate = mod;
}

void SDLOP_KeyboardUpdateXkbState(Uint32 depressed, Uint32 latched, Uint32 locked,
                                  Uint32 base_group, Uint32 latched_group, Uint32 locked_group)
{
    if (!xkb_st) {
        return;
    }
    /* the layout group matters for multi-layout setups (us,ru ...): key
     * text and keycodes follow the active group */
    SDLOP_XKB_xkb_state_update_mask(xkb_st, depressed, latched, locked,
                                    base_group, latched_group, locked_group);
    sdlop_sync_mods_from_xkb();
    check_layout_change();
}

void SDLOP_KeyboardUpdateXkbModifiers(Uint32 depressed, Uint32 latched, Uint32 locked)
{
    SDLOP_KeyboardUpdateXkbState(depressed, latched, locked, 0, 0, 0);
}

#endif /* !_WIN32 (xkb keymap layer) */

/* ------------------------------------------------------------------ */
/* keysym -> SDL_Keycode                                               */
/* ------------------------------------------------------------------ */

/* SDL3 SDL_GetKeyCodeFromKeySym: unicode for printable keysyms, the
 * spacing character for dead keys, a few extended keycodes, and
 * scancode-mask keycodes for everything else. Returns SDLK_UNKNOWN when the
 * keysym has no keycode of its own (the caller uses the scancode). */
static const Uint16 dead_key_symbols_fe50_fe6f[] = {
    0x0060, 0x00b4, 0x005e, 0x1fc0, 0x00af, 0x02d8, 0x02d9, 0x00a8, /* grave .. diaeresis */
    0x02da, 0x02dd, 0x02c7, 0x00b8, 0x02db, 0x037a, 0x309b, 0x309c, /* abovering .. semivoiced */
    0x0323, 0x0309, 0x031b, 0x002f, 0x02bc, 0x02bd, 0x02f5, 0x02f3, /* belowdot .. belowring */
    0x005f, 0x005e, 0x02f7, 0x032e, 0x0324, 0x0311, 0x0326, 0x00a4, /* belowmacron .. currency */
};
static const Uint16 dead_key_symbols_fe80_fe93[] = {
    0x0061, 0x0041, 0x0065, 0x0045, 0x0069, 0x0049, 0x006f, 0x004f, /* dead_a .. dead_O */
    0x0075, 0x0055, 0x0259, 0x018f, 0x0000, 0x0621, 0x0000, 0x0000, /* dead_u .. */
    0x005f, 0x02c8, 0x02cc, 0x002f,                                 /* lowline .. longsolidus */
};

static SDL_Keycode keysym_to_keycode(xkb_keysym_t sym)
{
    if (sym == XKB_KEY_NoSymbol) {
        return SDLK_UNKNOWN;
    }
    /* Latin-1 range maps directly to the unicode value (SDL3 semantics:
     * base-layout letters are lowercase: SDLK_A == 'a') */
    if (sym >= 0x20 && sym < 0x100 && sym != 0x7f) {
        return (SDL_Keycode)sym; /* space (0x20) == SDLK_SPACE */
    }

    /* SDL3 gives these ASCII values rather than scancode-mask codes */
    if (sym == XKB_KEY_Return) {
        return SDLK_RETURN;
    }
    if (sym == XKB_KEY_Escape) {
        return SDLK_ESCAPE;
    }
    if (sym == XKB_KEY_BackSpace) {
        return SDLK_BACKSPACE;
    }
    if (sym == XKB_KEY_Tab || sym == XKB_KEY_KP_Tab) {
        return SDLK_TAB;
    }
    if (sym == XKB_KEY_Delete) {
        return SDLK_DELETE;
    }

    /* non-printable keys: scancode | SDLK_SCANCODE_MASK (SDL3 semantics) */
    static const struct { xkb_keysym_t sym; SDL_Scancode sc; } special[] = {
        { XKB_KEY_Escape, SDL_SCANCODE_ESCAPE },
        { XKB_KEY_Return, SDL_SCANCODE_RETURN },
        { XKB_KEY_KP_Enter, SDL_SCANCODE_KP_ENTER },
        { XKB_KEY_Tab, SDL_SCANCODE_TAB },
        { XKB_KEY_BackSpace, SDL_SCANCODE_BACKSPACE },
        { XKB_KEY_Delete, SDL_SCANCODE_DELETE },
        { XKB_KEY_Insert, SDL_SCANCODE_INSERT },
        { XKB_KEY_Home, SDL_SCANCODE_HOME },
        { XKB_KEY_End, SDL_SCANCODE_END },
        { XKB_KEY_Page_Up, SDL_SCANCODE_PAGEUP },
        { XKB_KEY_Page_Down, SDL_SCANCODE_PAGEDOWN },
        { XKB_KEY_Left, SDL_SCANCODE_LEFT },
        { XKB_KEY_Right, SDL_SCANCODE_RIGHT },
        { XKB_KEY_Up, SDL_SCANCODE_UP },
        { XKB_KEY_Down, SDL_SCANCODE_DOWN },
        { XKB_KEY_Caps_Lock, SDL_SCANCODE_CAPSLOCK },
        { XKB_KEY_Num_Lock, SDL_SCANCODE_NUMLOCKCLEAR },
        { XKB_KEY_Scroll_Lock, SDL_SCANCODE_SCROLLLOCK },
        { XKB_KEY_Print, SDL_SCANCODE_PRINTSCREEN },
        { XKB_KEY_Pause, SDL_SCANCODE_PAUSE },
        { XKB_KEY_Menu, SDL_SCANCODE_APPLICATION },
        { XKB_KEY_Shift_L, SDL_SCANCODE_LSHIFT },
        { XKB_KEY_Shift_R, SDL_SCANCODE_RSHIFT },
        { XKB_KEY_Control_L, SDL_SCANCODE_LCTRL },
        { XKB_KEY_Control_R, SDL_SCANCODE_RCTRL },
        { XKB_KEY_Alt_L, SDL_SCANCODE_LALT },
        { XKB_KEY_Alt_R, SDL_SCANCODE_RALT },
        { XKB_KEY_Super_L, SDL_SCANCODE_LGUI },
        { XKB_KEY_Super_R, SDL_SCANCODE_RGUI },
        { XKB_KEY_KP_Space, SDL_SCANCODE_SPACE },
        { XKB_KEY_KP_Tab, SDL_SCANCODE_TAB },
        { XKB_KEY_KP_Multiply, SDL_SCANCODE_KP_MULTIPLY },
        { XKB_KEY_KP_Add, SDL_SCANCODE_KP_PLUS },
        { XKB_KEY_KP_Subtract, SDL_SCANCODE_KP_MINUS },
        { XKB_KEY_KP_Decimal, SDL_SCANCODE_KP_PERIOD },
        { XKB_KEY_KP_Divide, SDL_SCANCODE_KP_DIVIDE },
        { XKB_KEY_KP_Equal, SDL_SCANCODE_KP_EQUALS },
        { XKB_KEY_KP_Separator, SDL_SCANCODE_KP_COMMA },
        { XKB_KEY_KP_0, SDL_SCANCODE_KP_0 },
        { XKB_KEY_KP_1, SDL_SCANCODE_KP_1 },
        { XKB_KEY_KP_2, SDL_SCANCODE_KP_2 },
        { XKB_KEY_KP_3, SDL_SCANCODE_KP_3 },
        { XKB_KEY_KP_4, SDL_SCANCODE_KP_4 },
        { XKB_KEY_KP_5, SDL_SCANCODE_KP_5 },
        { XKB_KEY_KP_6, SDL_SCANCODE_KP_6 },
        { XKB_KEY_KP_7, SDL_SCANCODE_KP_7 },
        { XKB_KEY_KP_8, SDL_SCANCODE_KP_8 },
        { XKB_KEY_KP_9, SDL_SCANCODE_KP_9 },
    };
    for (size_t i = 0; i < SDL_arraysize(special); i++) {
        if (special[i].sym == sym) {
            return SDL_SCANCODE_TO_KEYCODE(special[i].sc);
        }
    }
    /* F1..F24 are sequential in both xkb and SDL */
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F24) {
        return SDL_SCANCODE_TO_KEYCODE((SDL_Scancode)(SDL_SCANCODE_F1 + (sym - XKB_KEY_F1)));
    }
    /* remaining keypad keysyms keep their scancode keycode (SDL3's
     * keysym->UCS table has no keypad range) */
    if (sym >= XKB_KEY_KP_Space && sym <= XKB_KEY_KP_Equal) {
        return SDLK_UNKNOWN;
    }
    /* any other printable keysym: its unicode value (Cyrillic, Greek, ...) */
    uint32_t cp = SDLOP_XKB_xkb_keysym_to_utf32(sym);
    if (cp >= 0x20 && cp != 0x7f && !(cp >= 0x80 && cp < 0xa0)) {
        return (SDL_Keycode)cp;
    }
    if (sym >= 0xfe50 && sym <= 0xfe6f) {
        return dead_key_symbols_fe50_fe6f[sym - 0xfe50];
    }
    if (sym >= 0xfe80 && sym <= 0xfe93 && dead_key_symbols_fe80_fe93[sym - 0xfe80]) {
        return dead_key_symbols_fe80_fe93[sym - 0xfe80];
    }
    switch (sym) {
    case XKB_KEY_ISO_Level3_Shift: return SDLK_MODE;
    case XKB_KEY_ISO_Level5_Shift: return SDLK_LEVEL5_SHIFT;
    case XKB_KEY_ISO_Left_Tab:     return SDLK_LEFT_TAB;
    case XKB_KEY_Multi_key:        return SDLK_MULTI_KEY_COMPOSE;
    case XKB_KEY_Meta_L:           return SDLK_LMETA;
    case XKB_KEY_Meta_R:           return SDLK_RMETA;
    case XKB_KEY_Hyper_L:          return SDLK_LHYPER;
    case XKB_KEY_Hyper_R:          return SDLK_RHYPER;
    default: break;
    }
    return SDLK_UNKNOWN;
}

#endif /* !_WIN32 (keysym mapping) */

#ifdef _WIN32

/* Windows has no xkb keymap and no compositor-supplied one: keys arrive as
 * virtual-key codes (translated in src/SDL_win32_keymap.c) and text as
 * WM_CHAR/WM_UNICHAR from the driver, so everything above compiles away.
 * The stubs keep the internal contract (see sdlop_internal.h) intact. */

bool SDLOP_KeyboardSetKeymapString(const char *keymap_string, size_t length)
{
    (void)keymap_string; (void)length;
    return true; /* the system provides key translation; nothing to install */
}

bool SDLOP_KeyboardSetDefaultKeymap(void)
{
    return true;
}

void SDLOP_KeyboardUpdateXkbState(Uint32 depressed, Uint32 latched, Uint32 locked,
                                  Uint32 group, Uint32 keycode, Uint32 mods)
{
    (void)depressed; (void)latched; (void)locked;
    (void)group; (void)keycode; (void)mods;
}

void SDLOP_KeyboardUpdateXkbModifiers(Uint32 depressed, Uint32 latched, Uint32 locked)
{
    (void)depressed; (void)latched; (void)locked;
}

#endif /* _WIN32 */


/* SDL3's built-in US layout (SDL_GetDefaultKeyFromScancode): used when no
 * xkb keymap is available (no libxkbcommon), and as the Windows mapping, so
 * keycodes stay meaningful everywhere */
static SDL_Keycode default_key_from_scancode(SDL_Scancode sc, SDL_Keymod mod)
{
    const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
    const bool caps = (mod & SDL_KMOD_CAPS) != 0;
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return (SDL_Keycode)((shift != caps ? 'A' : 'a') + (sc - SDL_SCANCODE_A));
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) {
        static const char base[] = "1234567890", shifted[] = "!@#$%^&*()";
        return (SDL_Keycode)(shift ? shifted : base)[sc - SDL_SCANCODE_1];
    }
    static const struct { SDL_Scancode sc; char base, shifted; } punct[] = {
        { SDL_SCANCODE_SPACE, ' ', ' ' }, { SDL_SCANCODE_MINUS, '-', '_' },
        { SDL_SCANCODE_EQUALS, '=', '+' }, { SDL_SCANCODE_LEFTBRACKET, '[', '{' },
        { SDL_SCANCODE_RIGHTBRACKET, ']', '}' }, { SDL_SCANCODE_BACKSLASH, '\\', '|' },
        { SDL_SCANCODE_NONUSHASH, '#', '~' }, { SDL_SCANCODE_SEMICOLON, ';', ':' },
        { SDL_SCANCODE_APOSTROPHE, '\'', '"' }, { SDL_SCANCODE_GRAVE, '`', '~' },
        { SDL_SCANCODE_COMMA, ',', '<' }, { SDL_SCANCODE_PERIOD, '.', '>' },
        { SDL_SCANCODE_SLASH, '/', '?' }, { SDL_SCANCODE_NONUSBACKSLASH, '<', '>' },
    };
    for (size_t i = 0; i < SDL_arraysize(punct); i++) {
        if (punct[i].sc == sc) {
            return (SDL_Keycode)(unsigned char)(shift ? punct[i].shifted : punct[i].base);
        }
    }
    switch (sc) {
    case SDL_SCANCODE_RETURN:    return SDLK_RETURN;
    case SDL_SCANCODE_ESCAPE:    return SDLK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return SDLK_BACKSPACE;
    case SDL_SCANCODE_TAB:       return SDLK_TAB;
    case SDL_SCANCODE_DELETE:    return SDLK_DELETE;
    default: break;
    }
    return SDL_SCANCODE_TO_KEYCODE(sc);
}

#ifndef _WIN32
/* SDL convention: no text for ctrl/alt combos (AltGr = MODE is fine), and
 * no control characters. POSIX-only: on Windows the text arrives finished
 * from WM_CHAR, where Win32 has already applied these rules. */
static void filter_text(char *text_utf8)
{
    SDL_Keymod mod = sdlop.modstate;
    if (mod & SDL_KMOD_MODE) {
        mod &= ~(SDL_KMOD_RALT | SDL_KMOD_LALT);
    }
    if (mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) {
        text_utf8[0] = '\0';
    }
    if (text_utf8[0] && (unsigned char)text_utf8[0] < 0x20 && text_utf8[0] != '\t') {
        text_utf8[0] = '\0';
    }
}
#endif /* !_WIN32 */

/* Feed a key press to the compose state. Returns true when composition
 * consumed the key (text_utf8 then holds the composed text, or nothing
 * while a sequence is in progress / was cancelled). */
#ifndef _WIN32

static bool compose_key(xkb_keycode_t xk, char *text_utf8, size_t text_size)
{
    if (!compose_st) {
        return false;
    }
    xkb_keysym_t sym = SDLOP_XKB_xkb_state_key_get_one_sym(xkb_st, xk);
    if (sym == XKB_KEY_NoSymbol ||
        SDLOP_XKB_xkb_compose_state_feed(compose_st, sym) != XKB_COMPOSE_FEED_ACCEPTED) {
        return false; /* modifiers are ignored by compose */
    }
    switch (SDLOP_XKB_xkb_compose_state_get_status(compose_st)) {
    case XKB_COMPOSE_COMPOSING:
        return true; /* dead key / Multi_key: no text yet */
    case XKB_COMPOSE_COMPOSED: {
        int len = SDLOP_XKB_xkb_compose_state_get_utf8(compose_st, text_utf8, text_size);
        if (len <= 0 || len >= (int)text_size) {
            text_utf8[0] = '\0';
        }
        SDLOP_XKB_xkb_compose_state_reset(compose_st);
        filter_text(text_utf8);
        return true;
    }
    case XKB_COMPOSE_CANCELLED:
        SDLOP_XKB_xkb_compose_state_reset(compose_st);
        return true; /* the cancelling key produces no text (SDL3, XIM) */
    case XKB_COMPOSE_NOTHING:
    default:
        return false;
    }
}

#endif /* !_WIN32 */

SDL_Keycode SDLOP_KeyboardTranslateKey(Uint16 rawcode, bool key_event, char *text_utf8, size_t text_size, bool *repeats_out)
{
#ifdef _WIN32
    /* `rawcode` is the driver's (VK << 8) | scan pair; the keycode comes from
     * the VK and text is sent by the driver from WM_CHAR. */
    if (text_utf8 && text_size) {
        text_utf8[0] = '\0';
    }
    if (repeats_out) {
        *repeats_out = true; /* Win32 auto-repeats keys itself */
    }
    const Uint32 vk = SDLOP_WIN32_VK(rawcode);
    if (!vk) {
        return SDLK_UNKNOWN;
    }
    return SDLOP_Win32KeyFromVK(vk, key_event ? SDL_GetModState() : SDL_KMOD_NONE);
#else
    if (text_utf8 && text_size) {
        text_utf8[0] = '\0';
    }
    if (repeats_out) {
        *repeats_out = false;
    }
    if (rawcode == 0 || rawcode > SDLOP_EVDEV_KEY_MAX) {
        return SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_UNKNOWN);
    }
    SDL_Scancode sc = (SDL_Scancode)sdlop_evdev_to_scancode[rawcode];
    if (!xkb_st) {
        return default_key_from_scancode(sc, SDL_KMOD_NONE);
    }
    xkb_keycode_t xk = (xkb_keycode_t)rawcode + 8; /* evdev -> xkb offset */

    /* resolved once for both text + key repeat bookkeeping */
    bool repeats = SDLOP_XKB_xkb_keymap_key_repeats(keymap, xk) == 1;
    if (repeats_out) {
        *repeats_out = repeats;
    }

    /* keycode: SDL3 key-event semantics (see event_keycode); for raw codes
     * without an SDL scancode, the base-layout keysym */
    SDL_Keycode key;
    if (sc != SDL_SCANCODE_UNKNOWN) {
        key = key_event ? event_keycode(sc) : layout_keycode(sc, SDL_KMOD_NONE);
    } else {
        key = keysym_to_keycode(SDLOP_XKB_xkb_state_key_get_one_sym(xkb_st_clean, xk));
    }

    /* text: from the live state (modifiers, active layout), through
     * compose for dead keys / the Compose key */
    if (key_event && text_utf8 && text_size) {
        if (compose_key(xk, text_utf8, text_size)) {
            return key;
        }
        if (repeats) {
            int len = SDLOP_XKB_xkb_state_key_get_utf8(xkb_st, xk, text_utf8, text_size);
            if (len <= 0 || len >= (int)text_size) {
                text_utf8[0] = '\0';
            } else {
                filter_text(text_utf8);
            }
        }
    }
    return key;
#endif /* _WIN32 */
}

bool SDLOP_KeyboardKeyRepeats(SDL_Scancode sc)
{
#ifdef _WIN32
    (void)sc;
    return true; /* every key auto-repeats on Windows */
#else
    if (!keymap) {
        return false;
    }
    Uint16 evdev = evdev_from_scancode(sc);
    if (!evdev) {
        return false;
    }
    return SDLOP_XKB_xkb_keymap_key_repeats(keymap, (xkb_keycode_t)evdev + 8);
#endif
}

/* ------------------------------------------------------------------ */
/* Modifier L/R tracking                                               */
/* ------------------------------------------------------------------ */

static SDL_Keymod modifier_bit_for(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_LSHIFT: return SDL_KMOD_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return SDL_KMOD_RSHIFT;
    case SDL_SCANCODE_LCTRL:  return SDL_KMOD_LCTRL;
    case SDL_SCANCODE_RCTRL:  return SDL_KMOD_RCTRL;
    case SDL_SCANCODE_LALT:   return SDL_KMOD_LALT;
    case SDL_SCANCODE_RALT:   return SDL_KMOD_RALT;
    case SDL_SCANCODE_LGUI:   return SDL_KMOD_LGUI;
    case SDL_SCANCODE_RGUI:   return SDL_KMOD_RGUI;
    default: return SDL_KMOD_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* Key events                                                          */
/* ------------------------------------------------------------------ */

void SDLOP_SendKeyboardKey(bool down, bool repeat, SDL_Scancode scancode, Uint16 rawcode, Uint64 timestamp_ns)
{
    if (scancode == SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
        return;
    }

#ifdef _WIN32
    Uint16 xkb_raw = rawcode; /* (VK << 8) | scan, from the driver */
#else
    Uint16 xkb_raw = rawcode ? rawcode : evdev_from_scancode(scancode);
#endif
    SDL_Keymod mod = sdlop.modstate;
    SDL_Keymod modbit = modifier_bit_for(scancode);

    if (down) {
        sdlop.keystate[scancode] = true;
        if (modbit) {
            mod |= modbit;
        }
    } else {
        sdlop.keystate[scancode] = false;
        if (modbit) {
            mod &= ~modbit;
        }
    }
    sdlop.modstate = mod;

    /* keep the live xkb state in sync (raw evdev path has no other way to
     * track modifiers/locks; wayland additionally receives absolute masks
     * from the compositor, which simply re-assert this state). Repeats are
     * already reflected in the state from the initial press. */
#ifndef _WIN32
    if (xkb_st && xkb_raw && !repeat) {
        SDLOP_XKB_xkb_state_update_key(xkb_st, (xkb_keycode_t)xkb_raw + 8,
                                       down ? XKB_KEY_DOWN : XKB_KEY_UP);
        sdlop_sync_mods_from_xkb();
        check_layout_change();
        mod = sdlop.modstate;
    }
#else
    (void)xkb_raw;
#endif

    /* resolve keycode (+ text, only needed for key presses) */
    bool key_repeats = false;
    char text[64];
    SDL_Keycode key = SDLOP_KeyboardTranslateKey(xkb_raw, down, text, sizeof(text), &key_repeats);

    SDL_Event event;
    SDL_zero(event);
    event.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.timestamp = timestamp_ns ? SDLOP_MonoToSDLTicks(timestamp_ns) : SDL_GetTicksNS();
    event.key.windowID = SDLOP_FocusWindowID();
    event.key.which = 0;
    event.key.scancode = scancode;
    event.key.key = key;
    event.key.mod = mod;
    event.key.raw = rawcode;
    event.key.down = down;
    event.key.repeat = repeat;
    SDLOP_PushEventInternal(&event);

    if (down && text[0]) {
        SDLOP_SendKeyboardText(text, timestamp_ns);
    }

    /* key repeat management (raw path has no server-generated repeats) */
    if (down && !repeat) {
        if (key_repeats) {
            sdlop.repeat_scancode = scancode;
            sdlop.repeat_rawcode = rawcode;
            sdlop.repeat_active = true;
            sdlop.repeat_next_ns = SDLOP_MonotonicNS() + (Uint64)sdlop.repeat_delay_ms * 1000000ull;
        }
    } else if (!down && scancode == sdlop.repeat_scancode) {
        sdlop.repeat_active = false;
    }
}

/* Generate due repeats. Called from PumpEvents. */
void SDLOP_KeyboardProcessRepeats(void)
{
    if (!sdlop.repeat_active) {
        return;
    }
    Uint64 now = SDLOP_MonotonicNS();
    while (sdlop.repeat_active && (Sint64)(now - sdlop.repeat_next_ns) >= 0) {
        SDLOP_SendKeyboardKey(true, true, sdlop.repeat_scancode, sdlop.repeat_rawcode, sdlop.repeat_next_ns);
        sdlop.repeat_next_ns += (Uint64)sdlop.repeat_interval_ms * 1000000ull;
    }
}

/* ------------------------------------------------------------------ */
/* Public keyboard API                                                 */
/* ------------------------------------------------------------------ */

const bool *SDL_GetKeyboardState(int *numkeys)
{
    if (numkeys) {
        *numkeys = SDL_SCANCODE_COUNT;
    }
    return sdlop.keystate;
}

void SDL_ResetKeyboard(void)
{
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++) {
        if (sdlop.keystate[i]) {
#ifdef _WIN32
            SDLOP_SendKeyboardKey(false, false, (SDL_Scancode)i, 0, 0);
#else
            SDLOP_SendKeyboardKey(false, false, (SDL_Scancode)i, evdev_from_scancode((SDL_Scancode)i), 0);
#endif
        }
    }
    sdlop.repeat_active = false;
    compose_reset(); /* focus loss abandons a pending dead key */
}

SDL_Keymod SDL_GetModState(void)
{
    return sdlop.modstate;
}

void SDL_SetModState(SDL_Keymod modstate)
{
    sdlop.modstate = modstate;
}

SDL_Window *SDL_GetKeyboardFocus(void)
{
    return sdlop.keyboard_focus;
}

SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode, SDL_Keymod modstate, bool key_event)
{
    /* SDL3 semantics:
     *  - key_event = true: the keycode a key event would carry - modifiers
     *    are ignored (letters stay lowercase, shift+2 is '2');
     *  - key_event = false: the passed modifiers select the shift level
     *    (shift/caps, AltGr = SDL_KMOD_MODE, SDL_KMOD_LEVEL5). */
    if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
        return SDLK_UNKNOWN;
    }
#ifdef _WIN32
    /* No layout keymap to consult: SDL3's built-in table is the Windows
     * mapping for a US layout, and key events ignore modifiers. */
    return default_key_from_scancode(scancode, key_event ? SDL_KMOD_NONE : modstate);
#else
    if (key_event) {
        return event_keycode(scancode);
    }
    return layout_keycode(scancode, modstate);
#endif
}

/* SDL_GetKeyName: a keycode is the unshifted key, its name the letter
 * printed on it - usually the shifted capital (SDL3: shift lookup on the
 * current keymap, accepted only if it case-folds back to the key; Latin-1
 * symbols 0x80-0xBF, x and / are never shifted) */
SDL_Keycode SDLOP_KeyboardKeyNameCapital(SDL_Keycode key)
{
#ifdef _WIN32
    return (key >= 'a' && key <= 'z') ? key - ('a' - 'A') : key;
#else
    if (!((key >= 'a' && key <= 'z') || (key > 0xBF && key != 0xD7 && key != 0xF7))) {
        return key;
    }
    SDL_Scancode sc;
    SDL_Keymod mod;
    const bool layout_keymap = keymap && (layout_flags & LAYOUT_LATIN_LETTERS) && !(layout_flags & LAYOUT_THAI);
    if (!layout_keymap || !revtab_get(key, &sc, &mod)) {
        /* default (US) keymap */
        return (key >= 'a' && key <= 'z') ? key - 'a' + 'A' : key;
    }
    if (mod & (SDL_KMOD_SHIFT | SDL_KMOD_LEVEL5)) {
        return key;
    }
    Uint16 evdev = evdev_from_scancode(sc);
    xkb_keysym_t base = layout_keysym(evdev, mod & SDL_KMOD_MODE);
    xkb_keysym_t shifted = layout_keysym(evdev, SDL_KMOD_SHIFT | (mod & SDL_KMOD_MODE));
    SDL_Keycode capital = keycode_from_sym(shifted, sc);
    if (capital == key || capital == SDLK_UNKNOWN || (capital & (SDLK_SCANCODE_MASK | SDLK_EXTENDED_MASK))) {
        return key;
    }
    if (SDLOP_XKB_xkb_keysym_to_lower) {
        return SDLOP_XKB_xkb_keysym_to_lower(shifted) == base ? capital : key;
    }
    return (key >= 'a' && key <= 'z' && capital == key - 'a' + 'A') ? capital : key;
#endif
}

SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod *modstate)
{
    if (modstate) {
        *modstate = SDL_KMOD_NONE;
    }
    if (key & SDLK_SCANCODE_MASK) {
        return (SDL_Scancode)(key & ~SDLK_SCANCODE_MASK);
    }
    switch (key) {
    case SDLK_RETURN:    return SDL_SCANCODE_RETURN;
    case SDLK_ESCAPE:    return SDL_SCANCODE_ESCAPE;
    case SDLK_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
    case SDLK_TAB:       return SDL_SCANCODE_TAB;
    case SDLK_DELETE:    return SDL_SCANCODE_DELETE;
    default: break;
    }
    /* printable: the active layout, least-modified level first (SDL3
     * searches keyboard->keymap, then the default keymap) */
    SDL_Scancode sc;
    SDL_Keymod found_mod;
#ifndef _WIN32
    if (keymap && revtab_get(key, &sc, &found_mod)) {
        if (modstate) {
            *modstate = found_mod;
        }
        return sc;
    }
#else
    (void)sc;
    (void)found_mod;
#endif
    /* not on the active layout (or no keymap): SDL3's default US table,
     * unshifted first, then shifted ('A' -> A + shift, '@' -> 2 + shift).
     * The ISO-only keys are skipped so '~'/'<' resolve to the US keys. */
    for (int pass = 0; pass < 2; pass++) {
        const SDL_Keymod m = pass ? SDL_KMOD_SHIFT : SDL_KMOD_NONE;
        for (int i = SDL_SCANCODE_A; i < SDL_SCANCODE_CAPSLOCK; i++) {
            if (i == SDL_SCANCODE_NONUSHASH) {
                continue;
            }
            if (default_key_from_scancode((SDL_Scancode)i, m) == key) {
                if (modstate) {
                    *modstate = m;
                }
                return (SDL_Scancode)i;
            }
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

void SDLOP_KeyboardQuit(void)
{
#ifndef _WIN32
    if (compose_st) {
        SDLOP_XKB_xkb_compose_state_unref(compose_st);
        compose_st = NULL;
    }
    if (compose_table) {
        SDLOP_XKB_xkb_compose_table_unref(compose_table);
        compose_table = NULL;
    }
    compose_tried = false;
    cur_layout = 0;
    layout_flags = 0;
    memset(revtab, 0, sizeof(revtab));
    if (xkb_st) {
        SDLOP_XKB_xkb_state_unref(xkb_st);
        xkb_st = NULL;
    }
    if (xkb_st_clean) {
        SDLOP_XKB_xkb_state_unref(xkb_st_clean);
        xkb_st_clean = NULL;
    }
    if (keymap) {
        SDLOP_XKB_xkb_keymap_unref(keymap);
        keymap = NULL;
    }
    if (xkb_ctx) {
        SDLOP_XKB_xkb_context_unref(xkb_ctx);
        xkb_ctx = NULL;
    }
#endif /* !_WIN32 */
    sdlop.repeat_active = false;
}
