/*
  SDLop - keyboard handling + xkbcommon integration.

  Keys arrive as SDL scancodes (converted from evdev raw codes or Wayland
  key events). This file maintains the key state array, modifier state,
  key repeat emulation, and xkb-based layout translation (keycode + text).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "internal/scancode_evdev.h"
#include <xkbcommon/xkbcommon.h>

/* ------------------------------------------------------------------ */
/* scancode -> evdev reverse table (built once)                        */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* xkb state                                                           */
/* ------------------------------------------------------------------ */

static struct xkb_context *xkb_ctx;
static struct xkb_keymap *keymap;
static struct xkb_state *xkb_st;
static struct xkb_state *xkb_st_clean; /* no modifiers, for key resolution */
static xkb_mod_index_t mod_shift, mod_ctrl, mod_alt, mod_super, mod_caps, mod_num, mod_altgr, mod_level5;

/* sym -> scancode lookup for SDL_GetScancodeFromKey (open addressing) */
#define SYMTAB_SIZE 1024
static struct { xkb_keysym_t sym; SDL_Scancode sc; } symtab[SYMTAB_SIZE];

static void symtab_put(xkb_keysym_t sym, SDL_Scancode sc)
{
    uint32_t h = (uint32_t)sym & (SYMTAB_SIZE - 1);
    while (symtab[h].sym != 0 && symtab[h].sym != sym) {
        h = (h + 1) & (SYMTAB_SIZE - 1);
    }
    symtab[h].sym = sym;
    symtab[h].sc = sc;
}

static SDL_Scancode symtab_get(xkb_keysym_t sym)
{
    uint32_t h = (uint32_t)sym & (SYMTAB_SIZE - 1);
    while (symtab[h].sym != 0) {
        if (symtab[h].sym == sym) {
            return symtab[h].sc;
        }
        h = (h + 1) & (SYMTAB_SIZE - 1);
    }
    return SDL_SCANCODE_UNKNOWN;
}

static void find_mod_indices(void)
{
    mod_shift  = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);   /* "Shift" */
    mod_ctrl   = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);    /* "Control" */
    mod_alt    = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);     /* "Mod1" */
    mod_super  = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_LOGO);    /* "Mod4" */
    mod_caps   = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CAPS);    /* "Lock" */
    mod_num    = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_NUM);     /* "Mod2" */
    mod_altgr  = xkb_keymap_mod_get_index(keymap, "Mod5");   /* "Mod5" */
    mod_level5 = xkb_keymap_mod_get_index(keymap, "Mod3");               /* ISO_Level5_Shift */
}

/* Build sym->scancode map and remember key names from the base layout */
static void build_symtab(void)
{
    memset(symtab, 0, sizeof(symtab));
    for (uint16_t evdev = 1; evdev <= SDLOP_EVDEV_KEY_MAX; evdev++) {
        SDL_Scancode sc = (SDL_Scancode)sdlop_evdev_to_scancode[evdev];
        if (sc == SDL_SCANCODE_UNKNOWN) {
            continue;
        }
        xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_st_clean, (xkb_keycode_t)evdev + 8);
        if (sym != XKB_KEY_NoSymbol) {
            symtab_put(sym, sc);
            /* also index unshifted letters (a-z) so SDLK_a finds A key */
            if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) {
                symtab_put(sym - XKB_KEY_A + XKB_KEY_a, sc);
            }
        }
    }
}

static bool set_keymap(struct xkb_keymap *new_keymap)
{
    struct xkb_state *new_state = xkb_state_new(new_keymap);
    struct xkb_state *new_clean = xkb_state_new(new_keymap);
    if (!new_state || !new_clean) {
        xkb_state_unref(new_state);
        xkb_state_unref(new_clean);
        xkb_keymap_unref(new_keymap);
        return SDL_SetError("xkb_state_new() failed");
    }
    if (xkb_st) {
        xkb_state_unref(xkb_st);
    }
    if (xkb_st_clean) {
        xkb_state_unref(xkb_st_clean);
    }
    if (keymap) {
        xkb_keymap_unref(keymap);
    }
    keymap = new_keymap;
    xkb_st = new_state;
    xkb_st_clean = new_clean;
    find_mod_indices();
    build_symtab();
    return true;
}

bool SDLOP_KeyboardSetKeymapString(const char *keymap_string, size_t length)
{
    if (!xkb_ctx) {
        xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!xkb_ctx) {
            return SDL_SetError("xkb_context_new() failed");
        }
    }
    struct xkb_keymap *km = xkb_keymap_new_from_string(xkb_ctx, keymap_string, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) {
        return SDL_SetError("Could not parse Wayland xkb keymap");
    }
    (void)length;
    return set_keymap(km);
}

bool SDLOP_KeyboardSetDefaultKeymap(void)
{
    if (!xkb_ctx) {
        xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!xkb_ctx) {
            return SDL_SetError("xkb_context_new() failed");
        }
    }
    if (keymap) {
        return true; /* already have one */
    }
    struct xkb_rule_names names = { 0 };
    struct xkb_keymap *km = xkb_keymap_new_from_names(xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) {
        return SDL_SetError("Could not create default xkb keymap");
    }
    return set_keymap(km);
}

void SDLOP_KeyboardUpdateXkbModifiers(Uint32 depressed, Uint32 latched, Uint32 locked)
{
    if (!xkb_st) {
        return;
    }
    xkb_state_update_mask(xkb_st, depressed, latched, locked, 0, 0, 0);

    SDL_Keymod mod = sdlop.modstate & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT | SDL_KMOD_LCTRL | SDL_KMOD_RCTRL | SDL_KMOD_LALT | SDL_KMOD_RALT | SDL_KMOD_LGUI | SDL_KMOD_RGUI);

    /* toggle locks come from xkb; left/right bits are tracked from key events */
    if (xkb_state_mod_index_is_active(xkb_st, mod_caps, XKB_STATE_MODS_EFFECTIVE) == 1) {
        mod |= SDL_KMOD_CAPS;
    }
    if (xkb_state_mod_index_is_active(xkb_st, mod_num, XKB_STATE_MODS_EFFECTIVE) == 1) {
        mod |= SDL_KMOD_NUM;
    }
    if (xkb_state_mod_index_is_active(xkb_st, mod_level5, XKB_STATE_MODS_EFFECTIVE) == 1) {
        mod |= SDL_KMOD_LEVEL5;
    }
    if (xkb_state_mod_index_is_active(xkb_st, mod_altgr, XKB_STATE_MODS_EFFECTIVE) == 1) {
        mod |= SDL_KMOD_MODE;
    }
    /* keep L/R modifier bits in sync with xkb effective state */
    if (xkb_state_mod_index_is_active(xkb_st, mod_shift, XKB_STATE_MODS_EFFECTIVE) != 1) {
        mod &= ~(SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT);
    }
    if (xkb_state_mod_index_is_active(xkb_st, mod_ctrl, XKB_STATE_MODS_EFFECTIVE) != 1) {
        mod &= ~(SDL_KMOD_LCTRL | SDL_KMOD_RCTRL);
    }
    if (xkb_state_mod_index_is_active(xkb_st, mod_super, XKB_STATE_MODS_EFFECTIVE) != 1) {
        mod &= ~(SDL_KMOD_LGUI | SDL_KMOD_RGUI);
    }
    sdlop.modstate = mod;
}

/* ------------------------------------------------------------------ */
/* keysym -> SDL_Keycode                                               */
/* ------------------------------------------------------------------ */

static SDL_Keycode keysym_to_keycode(xkb_keysym_t sym)
{
    /* Latin-1 range maps directly to the unicode value (SDL3 semantics:
     * base-layout letters are lowercase: SDLK_a == 'a') */
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
        return (SDL_Keycode)sym;
    }
    if (sym > 0x20 && sym < 0x100 && sym != 0x7f) {
        return (SDL_Keycode)sym;
    }

    /* SDL3 gives these ASCII values rather than scancode-mask codes */
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        return SDLK_RETURN;
    }
    if (sym == XKB_KEY_Escape) {
        return SDLK_ESCAPE;
    }
    if (sym == XKB_KEY_BackSpace) {
        return SDLK_BACKSPACE;
    }
    if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab || sym == XKB_KEY_KP_Tab) {
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
        { XKB_KEY_ISO_Left_Tab, SDL_SCANCODE_TAB },
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
        { XKB_KEY_space, SDL_SCANCODE_SPACE },
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
    return SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_UNKNOWN);
}

SDL_Keycode SDLOP_KeyboardTranslateKey(Uint16 rawcode, bool key_event, char *text_utf8, size_t text_size)
{
    if (text_utf8 && text_size) {
        text_utf8[0] = '\0';
    }
    if (!xkb_st || rawcode == 0) {
        return SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_UNKNOWN);
    }
    xkb_keycode_t xk = (xkb_keycode_t)rawcode + 8; /* evdev -> xkb offset */

    /* key code: resolved on the base layout (clean state) like SDL3 */
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_st_clean, xk);
    SDL_Keycode key = keysym_to_keycode(sym);

    /* text: from the live state, respecting modifiers */
    if (key_event && text_utf8) {
        if (xkb_keymap_key_repeats(keymap, xk)) {
            int len = xkb_state_key_get_utf8(xkb_st, xk, text_utf8, text_size);
            if (len <= 0 || len >= (int)text_size) {
                text_utf8[0] = '\0';
            } else {
                /* don't emit text for ctrl/alt combos (SDL convention),
                 * but allow AltGr (MODE) */
                SDL_Keymod mod = sdlop.modstate;
                if (mod & SDL_KMOD_MODE) {
                    mod &= ~(SDL_KMOD_RALT | SDL_KMOD_LALT);
                }
                if (mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) {
                    text_utf8[0] = '\0';
                }
                /* filter control characters */
                if (text_utf8[0] && (unsigned char)text_utf8[0] < 0x20 && text_utf8[0] != '\t') {
                    text_utf8[0] = '\0';
                }
            }
        }
    }
    return key;
}

bool SDLOP_KeyboardKeyRepeats(SDL_Scancode sc)
{
    if (!keymap) {
        return false;
    }
    Uint16 evdev = evdev_from_scancode(sc);
    if (!evdev) {
        return false;
    }
    return xkb_keymap_key_repeats(keymap, (xkb_keycode_t)evdev + 8);
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
    case SDL_SCANCODE_RALT:   return (sdlop.modstate & SDL_KMOD_MODE) ? (SDL_KMOD_RALT) : SDL_KMOD_RALT;
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

    /* resolve keycode + text */
    char text[64];
    SDL_Keycode key = SDLOP_KeyboardTranslateKey(rawcode ? rawcode : evdev_from_scancode(scancode), true, text, sizeof(text));

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
        if (SDLOP_KeyboardKeyRepeats(scancode)) {
            sdlop.repeat_scancode = scancode;
            sdlop.repeat_rawcode = rawcode;
            sdlop.repeat_active = true;
            sdlop.repeat_next_ns = SDLOP_MonotonicNS() + (Uint64)sdlop.repeat_delay_ms * 1000000ull;
        }
    } else if (!down && scancode == sdlop.repeat_scancode) {
        sdlop.repeat_active = false;
    }
}

void SDLOP_SendKeyboardText(const char *utf8, Uint64 timestamp_ns)
{
    if (!utf8 || !utf8[0]) {
        return;
    }
    SDL_Event event;
    SDL_zero(event);
    event.text.type = SDL_EVENT_TEXT_INPUT;
    event.text.timestamp = timestamp_ns ? SDLOP_MonoToSDLTicks(timestamp_ns) : SDL_GetTicksNS();
    event.text.windowID = SDLOP_FocusWindowID();
    event.text.text = utf8; /* copied into slot storage by the queue */
    SDLOP_PushEventInternal(&event);
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

bool SDL_GetKeyState(SDL_Scancode scancode)
{
    if (scancode >= 0 && scancode < SDL_SCANCODE_COUNT) {
        return sdlop.keystate[scancode];
    }
    SDL_SetError("Invalid scancode: %d", (int)scancode);
    return false;
}

void SDL_ResetKeyboard(void)
{
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++) {
        if (sdlop.keystate[i]) {
            SDLOP_SendKeyboardKey(false, false, (SDL_Scancode)i, evdev_from_scancode((SDL_Scancode)i), 0);
        }
    }
    sdlop.repeat_active = false;
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
    Uint16 raw = evdev_from_scancode(scancode);
    /* SDL3 applies the *passed* modifiers when key_event is set */
    if (key_event && keymap &&
        (modstate & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT | SDL_KMOD_CAPS))) {
        xkb_mod_mask_t mask = 0;
        if (modstate & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT)) {
            mask |= (xkb_mod_mask_t)1 << mod_shift;
        }
        if (modstate & SDL_KMOD_CAPS) {
            mask |= (xkb_mod_mask_t)1 << mod_caps;
        }
        xkb_state_update_mask(xkb_st_clean, mask, 0, 0, 0, 0, 0);
        xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_st_clean, (xkb_keycode_t)raw + 8);
        xkb_state_update_mask(xkb_st_clean, 0, 0, 0, 0, 0, 0);
        return keysym_to_keycode(sym);
    }
    return SDLOP_KeyboardTranslateKey(raw, key_event, NULL, 0);
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
    /* printable: look up the base-layout keysym */
    SDL_Scancode sc = symtab_get((xkb_keysym_t)key);
    if (sc != SDL_SCANCODE_UNKNOWN) {
        return sc;
    }
    /* shifted printables (SDL3: SDLK_W resolves to SDL_SCANCODE_W) */
    static const struct { char ch; SDL_Scancode sc; } shifted_us[] = {
        { '!', SDL_SCANCODE_1 }, { '@', SDL_SCANCODE_2 }, { '#', SDL_SCANCODE_3 },
        { '$', SDL_SCANCODE_4 }, { '%', SDL_SCANCODE_5 }, { '^', SDL_SCANCODE_6 },
        { '&', SDL_SCANCODE_7 }, { '*', SDL_SCANCODE_8 }, { '(', SDL_SCANCODE_9 },
        { ')', SDL_SCANCODE_0 }, { '_', SDL_SCANCODE_MINUS }, { '+', SDL_SCANCODE_EQUALS },
        { '{', SDL_SCANCODE_LEFTBRACKET }, { '}', SDL_SCANCODE_RIGHTBRACKET },
        { '|', SDL_SCANCODE_BACKSLASH }, { ':', SDL_SCANCODE_SEMICOLON },
        { '"', SDL_SCANCODE_APOSTROPHE }, { '~', SDL_SCANCODE_GRAVE },
        { '<', SDL_SCANCODE_COMMA }, { '>', SDL_SCANCODE_PERIOD },
        { '?', SDL_SCANCODE_SLASH },
    };
    if (key >= 'A' && key <= 'Z') {
        if (modstate) {
            *modstate = SDL_KMOD_LSHIFT;
        }
        return (SDL_Scancode)(SDL_SCANCODE_A + (key - 'A'));
    }
    for (size_t i = 0; i < SDL_arraysize(shifted_us); i++) {
        if (shifted_us[i].ch == (char)key) {
            if (modstate) {
                *modstate = SDL_KMOD_LSHIFT;
            }
            return shifted_us[i].sc;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Scancode / key names (generated, SDL3-style)                        */
/* ------------------------------------------------------------------ */

#include "scancode_names.h"

const char *SDL_GetScancodeName(SDL_Scancode scancode)
{
    if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT) {
        const char *name = sdlop_scancode_names[scancode];
        if (name) {
            return name;
        }
    }
    return "";
}

const char *SDL_GetKeyName(SDL_Keycode key)
{
    if (key & SDLK_SCANCODE_MASK) {
        return SDL_GetScancodeName((SDL_Scancode)(key & ~SDLK_SCANCODE_MASK));
    }
    switch (key) {
    case SDLK_RETURN:    return "Return";
    case SDLK_ESCAPE:    return "Escape";
    case SDLK_BACKSPACE: return "Backspace";
    case SDLK_TAB:       return "Tab";
    case SDLK_DELETE:    return "Delete";
    default: break;
    }
    if (key >= 0x20 && key < 0x7f) {
        static _Thread_local char namebuf[2] = { 0, 0 };
        namebuf[0] = (char)key;
        return namebuf;
    }
    return "";
}

void SDLOP_KeyboardQuit(void)
{
    if (xkb_st) {
        xkb_state_unref(xkb_st);
        xkb_st = NULL;
    }
    if (xkb_st_clean) {
        xkb_state_unref(xkb_st_clean);
        xkb_st_clean = NULL;
    }
    if (keymap) {
        xkb_keymap_unref(keymap);
        keymap = NULL;
    }
    if (xkb_ctx) {
        xkb_context_unref(xkb_ctx);
        xkb_ctx = NULL;
    }
    sdlop.repeat_active = false;
}
