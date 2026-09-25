/*
  SDLop - keyboard handling for the web (Emscripten) build.

  Replaces SDL_keyboard.c when xkbcommon is unavailable. The browser
  delivers layout-independent DOM codes; we keep a static US-layout
  table (SDL3's default-keymap semantics) for scancode -> keycode/text.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* static US layout table                                              */
/* ------------------------------------------------------------------ */

struct keyent
{
    SDL_Scancode sc;
    char base;      /* unshifted printable (0 = none) */
    char shifted;   /* shifted printable (0 = none) */
    bool repeats;
};

/* clang-format off */
static const struct keyent key_table[] = {
    { SDL_SCANCODE_A, 'a', 'A', true }, { SDL_SCANCODE_B, 'b', 'B', true },
    { SDL_SCANCODE_C, 'c', 'C', true }, { SDL_SCANCODE_D, 'd', 'D', true },
    { SDL_SCANCODE_E, 'e', 'E', true }, { SDL_SCANCODE_F, 'f', 'F', true },
    { SDL_SCANCODE_G, 'g', 'G', true }, { SDL_SCANCODE_H, 'h', 'H', true },
    { SDL_SCANCODE_I, 'i', 'I', true }, { SDL_SCANCODE_J, 'j', 'J', true },
    { SDL_SCANCODE_K, 'k', 'K', true }, { SDL_SCANCODE_L, 'l', 'L', true },
    { SDL_SCANCODE_M, 'm', 'M', true }, { SDL_SCANCODE_N, 'n', 'N', true },
    { SDL_SCANCODE_O, 'o', 'O', true }, { SDL_SCANCODE_P, 'p', 'P', true },
    { SDL_SCANCODE_Q, 'q', 'Q', true }, { SDL_SCANCODE_R, 'r', 'R', true },
    { SDL_SCANCODE_S, 's', 'S', true }, { SDL_SCANCODE_T, 't', 'T', true },
    { SDL_SCANCODE_U, 'u', 'U', true }, { SDL_SCANCODE_V, 'v', 'V', true },
    { SDL_SCANCODE_W, 'w', 'W', true }, { SDL_SCANCODE_X, 'x', 'X', true },
    { SDL_SCANCODE_Y, 'y', 'Y', true }, { SDL_SCANCODE_Z, 'z', 'Z', true },
    { SDL_SCANCODE_1, '1', '!', true }, { SDL_SCANCODE_2, '2', '@', true },
    { SDL_SCANCODE_3, '3', '#', true }, { SDL_SCANCODE_4, '4', '$', true },
    { SDL_SCANCODE_5, '5', '%', true }, { SDL_SCANCODE_6, '6', '^', true },
    { SDL_SCANCODE_7, '7', '&', true }, { SDL_SCANCODE_8, '8', '*', true },
    { SDL_SCANCODE_9, '9', '(', true }, { SDL_SCANCODE_0, '0', ')', true },
    { SDL_SCANCODE_MINUS, '-', '_', true }, { SDL_SCANCODE_EQUALS, '=', '+', true },
    { SDL_SCANCODE_LEFTBRACKET, '[', '{', true }, { SDL_SCANCODE_RIGHTBRACKET, ']', '}', true },
    { SDL_SCANCODE_BACKSLASH, '\\', '|', true }, { SDL_SCANCODE_NONUSBACKSLASH, '\\', '|', true },
    { SDL_SCANCODE_SEMICOLON, ';', ':', true }, { SDL_SCANCODE_APOSTROPHE, '\'', '"', true },
    { SDL_SCANCODE_GRAVE, '`', '~', true }, { SDL_SCANCODE_COMMA, ',', '<', true },
    { SDL_SCANCODE_PERIOD, '.', '>', true }, { SDL_SCANCODE_SLASH, '/', '?', true },
    { SDL_SCANCODE_SPACE, ' ', ' ', true },
    { SDL_SCANCODE_RETURN, 0, 0, true }, { SDL_SCANCODE_TAB, 0, 0, true },
    { SDL_SCANCODE_BACKSPACE, 0, 0, true }, { SDL_SCANCODE_DELETE, 0, 0, true },
    { SDL_SCANCODE_INSERT, 0, 0, true },
    { SDL_SCANCODE_UP, 0, 0, true }, { SDL_SCANCODE_DOWN, 0, 0, true },
    { SDL_SCANCODE_LEFT, 0, 0, true }, { SDL_SCANCODE_RIGHT, 0, 0, true },
    { SDL_SCANCODE_HOME, 0, 0, true }, { SDL_SCANCODE_END, 0, 0, true },
    { SDL_SCANCODE_PAGEUP, 0, 0, true }, { SDL_SCANCODE_PAGEDOWN, 0, 0, true },
    { SDL_SCANCODE_ESCAPE, 0, 0, false }, { SDL_SCANCODE_CAPSLOCK, 0, 0, false },
    { SDL_SCANCODE_NUMLOCKCLEAR, 0, 0, false }, { SDL_SCANCODE_SCROLLLOCK, 0, 0, false },
    { SDL_SCANCODE_LSHIFT, 0, 0, false }, { SDL_SCANCODE_RSHIFT, 0, 0, false },
    { SDL_SCANCODE_LCTRL, 0, 0, false }, { SDL_SCANCODE_RCTRL, 0, 0, false },
    { SDL_SCANCODE_LALT, 0, 0, false }, { SDL_SCANCODE_RALT, 0, 0, false },
    { SDL_SCANCODE_LGUI, 0, 0, false }, { SDL_SCANCODE_RGUI, 0, 0, false },
    /* keypad: text when NumLock is on */
    { SDL_SCANCODE_KP_0, '0', '0', true }, { SDL_SCANCODE_KP_1, '1', '1', true },
    { SDL_SCANCODE_KP_2, '2', '2', true }, { SDL_SCANCODE_KP_3, '3', '3', true },
    { SDL_SCANCODE_KP_4, '4', '4', true }, { SDL_SCANCODE_KP_5, '5', '5', true },
    { SDL_SCANCODE_KP_6, '6', '6', true }, { SDL_SCANCODE_KP_7, '7', '7', true },
    { SDL_SCANCODE_KP_8, '8', '8', true }, { SDL_SCANCODE_KP_9, '9', '9', true },
    { SDL_SCANCODE_KP_DIVIDE, '/', '/', true }, { SDL_SCANCODE_KP_MULTIPLY, '*', '*', true },
    { SDL_SCANCODE_KP_MINUS, '-', '-', true }, { SDL_SCANCODE_KP_PLUS, '+', '+', true },
    { SDL_SCANCODE_KP_PERIOD, '.', '.', true }, { SDL_SCANCODE_KP_ENTER, 0, 0, true },
    { SDL_SCANCODE_KP_EQUALS, '=', '=', true },
};
/* clang-format on */

static const struct keyent *entry_for(SDL_Scancode sc)
{
    for (size_t i = 0; i < SDL_arraysize(key_table); i++) {
        if (key_table[i].sc == sc) {
            return &key_table[i];
        }
    }
    return NULL;
}

/* Resolve key + text for a scancode. `key_event` mirrors SDL3's
 * SDL_GetKeyFromScancode semantics (false = polled base key). */
static SDL_Keycode resolve(SDL_Scancode sc, SDL_Keymod mod, bool key_event, char *text, size_t text_size)
{
    if (text && text_size) {
        text[0] = '\0';
    }
    const struct keyent *e = entry_for(sc);
    if (!e) {
        return sc == SDL_SCANCODE_UNKNOWN ? SDLK_UNKNOWN : (SDL_Keycode)(SDLK_SCANCODE_MASK | sc);
    }

    bool shifted = key_event && (mod & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT)) != 0;
    bool caps = (mod & SDL_KMOD_CAPS) != 0;
    char ch = shifted ? e->shifted : e->base;
    if (ch && caps && e->base >= 'a' && e->base <= 'z') {
        ch = shifted ? e->base : (char)(e->base - 32); /* shift XOR caps for letters */
    }

    if (text && text_size && ch) {
        bool printable_keypad = (sc >= SDL_SCANCODE_KP_DIVIDE && sc <= SDL_SCANCODE_KP_9);
        if (!printable_keypad || (mod & SDL_KMOD_NUM)) {
            /* ctrl/alt/gui/meta combos produce no text (xkb semantics) */
            if (!(mod & (SDL_KMOD_LCTRL | SDL_KMOD_RCTRL | SDL_KMOD_LALT | SDL_KMOD_RALT | SDL_KMOD_LGUI | SDL_KMOD_RGUI | SDL_KMOD_MODE))) {
                text[0] = ch;
                text[1] = '\0';
            }
        }
    }

    if (!ch) {
        /* SDL3 assigns ASCII values to these; everything else is
         * scancode | SDLK_SCANCODE_MASK */
        switch (sc) {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return SDLK_RETURN;
        case SDL_SCANCODE_ESCAPE:   return SDLK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return SDLK_BACKSPACE;
        case SDL_SCANCODE_TAB:      return SDLK_TAB;
        case SDL_SCANCODE_DELETE:   return SDLK_DELETE;
        default: break;
        }
        return (SDL_Keycode)(SDLK_SCANCODE_MASK | sc);
    }
    return (SDL_Keycode)(unsigned char)ch;
}

SDL_Keycode SDLOP_KeyboardTranslateKey(Uint16 rawcode, bool key_event, char *text_utf8, size_t text_size)
{
    /* the raw evdev path does not exist on the web */
    (void)rawcode;
    (void)key_event;
    if (text_utf8 && text_size) {
        text_utf8[0] = '\0';
    }
    return SDLK_UNKNOWN;
}

bool SDLOP_KeyboardKeyRepeats(SDL_Scancode sc)
{
    const struct keyent *e = entry_for(sc);
    return e && e->repeats;
}

/* ------------------------------------------------------------------ */
/* keymaps (no-ops on the web; the table is built in)                  */
/* ------------------------------------------------------------------ */

bool SDLOP_KeyboardSetKeymapString(const char *keymap_string, size_t length)
{
    (void)keymap_string;
    (void)length;
    return SDL_SetError("Unsupported");
}

bool SDLOP_KeyboardSetDefaultKeymap(void)
{
    return true; /* static table always available */
}

void SDLOP_KeyboardUpdateXkbModifiers(Uint32 depressed, Uint32 latched, Uint32 locked)
{
    (void)depressed;
    (void)latched;
    (void)locked;
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

    char text[8] = { 0 };
    SDL_Keycode key = resolve(scancode, mod, true, text, sizeof(text));

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
    event.text.text = utf8;
    SDLOP_PushEventInternal(&event);
}

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
            SDLOP_SendKeyboardKey(false, false, (SDL_Scancode)i, 0, 0);
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
    return resolve(scancode, modstate, key_event, NULL, 0);
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
    if (key >= 0x20 && key < 0x7f) {
        char ch = (char)key;
        for (size_t i = 0; i < SDL_arraysize(key_table); i++) {
            const struct keyent *e = &key_table[i];
            if (e->base && (e->base == ch || e->shifted == ch)) {
                if (modstate && e->shifted == ch && e->base != ch) {
                    *modstate = SDL_KMOD_LSHIFT;
                }
                return e->sc;
            }
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Scancode / key names (shared with the native build)                 */
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
    SDL_ResetKeyboard();
    sdlop.modstate = SDL_KMOD_NONE;
}

/* ------------------------------------------------------------------ */
/* raw-input stubs (no evdev on the web)                               */
/* ------------------------------------------------------------------ */

bool SDLOP_RawInputKeyboardActive(void)
{
    return false;
}

bool SDLOP_RawInputInit(void)
{
    return SDL_SetError("raw input is not available on the web");
}

void SDLOP_RawInputQuit(void)
{
}

void SDLOP_RawInputPump(void)
{
}

int SDLop_PollRawEvents(SDLop_RawEvent *events, int max_events)
{
    (void)events;
    (void)max_events;
    return 0;
}

bool SDLop_RegisterRawEventCallback(SDLop_RawEventCallback cb, void *userdata)
{
    (void)cb;
    (void)userdata;
    SDL_SetError("Unsupported");
    return false;
}

void SDLop_UnregisterRawEventCallback(SDLop_RawEventCallback cb)
{
    (void)cb;
}

bool SDLop_RawInputAvailable(void)
{
    return false;
}

Uint64 SDLop_GetRawEventCount(void)
{
    return 0;
}
