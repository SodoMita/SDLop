/*
  SDLop - scancode/key names, key-name lookups, text-input gating.

  Shared by the native (xkb) and web (DOM) keyboard backends. Semantics
  follow stock SDL3 3.2.10: names resolve through the scancode table,
  the extended-key table and UCS4->UTF-8 with stable interned pointers,
  and TEXT_INPUT is only delivered while SDL_StartTextInput() is active.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <string.h>
#include <strings.h>

bool SDL_StartTextInput(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    window->text_input_active = true;
    return true;
}

bool SDL_StopTextInput(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    window->text_input_active = false;
    return true;
}

bool SDL_TextInputActive(SDL_Window *window)
{
    return window && window->text_input_active;
}

void SDLOP_SendKeyboardText(const char *utf8, Uint64 timestamp_ns)
{
    if (!utf8 || !utf8[0]) {
        return;
    }
    /* stock SDL3: TEXT_INPUT is never delivered unless text input was
     * enabled via SDL_StartTextInput() - it is disabled by default */
    if (!sdlop.keyboard_focus || !sdlop.keyboard_focus->text_input_active) {
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

/* SDL3 keeps these names in their own table (src/events/SDL_keymap.c). */
static const char *const sdlop_extended_key_names[] = {
    "LeftTab",          /* SDLK_LEFT_TAB */
    "Level5Shift",      /* SDLK_LEVEL5_SHIFT */
    "MultiKeyCompose",  /* SDLK_MULTI_KEY_COMPOSE */
    "Left Meta",        /* SDLK_LMETA */
    "Right Meta",       /* SDLK_RMETA */
    "Left Hyper",       /* SDLK_LHYPER */
    "Right Hyper",      /* SDLK_RHYPER */
};

/* stable pointers for computed names, like stock's SDL_GetPersistentString */
#define SDLOP_NAME_POOL 64
static char sdlop_name_pool[SDLOP_NAME_POOL][8];
static int sdlop_name_pool_n = 0;

static const char *intern_key_name(const char *s)
{
    for (int i = 0; i < sdlop_name_pool_n; i++) {
        if (strcmp(sdlop_name_pool[i], s) == 0) {
            return sdlop_name_pool[i];
        }
    }
    if (sdlop_name_pool_n < SDLOP_NAME_POOL) {
        char *slot = sdlop_name_pool[sdlop_name_pool_n++];
        strncpy(slot, s, sizeof(sdlop_name_pool[0]) - 1);
        slot[sizeof(sdlop_name_pool[0]) - 1] = '\0';
        return slot;
    }
    return sdlop_name_pool[0];
}

static char *ucs4_to_utf8(Uint32 cp, char *out)
{
    if (cp <= 0x7F) {
        *out++ = (char)cp;
    } else if (cp <= 0x7FF) {
        *out++ = (char)(0xC0 | (cp >> 6));
        *out++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        *out++ = (char)(0xE0 | (cp >> 12));
        *out++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *out++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *out++ = (char)(0xF0 | (cp >> 18));
        *out++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *out++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *out++ = (char)(0x80 | (cp & 0x3F));
    }
    *out = '\0';
    return out;
}

const char *SDL_GetKeyName(SDL_Keycode key)
{
    if (key & SDLK_SCANCODE_MASK) {
        return SDL_GetScancodeName((SDL_Scancode)(key & ~SDLK_SCANCODE_MASK));
    }

    if (key & SDLK_EXTENDED_MASK) {
        SDL_Keycode idx = (SDL_Keycode)(key & ~SDLK_EXTENDED_MASK);
        if (idx > 0 && (size_t)(idx - 1) < SDL_arraysize(sdlop_extended_key_names)) {
            return sdlop_extended_key_names[idx - 1];
        }
        SDL_SetError("Parameter '%s' is invalid", "key");
        return "";
    }

    switch (key) {
    case SDLK_RETURN:    return SDL_GetScancodeName(SDL_SCANCODE_RETURN);
    case SDLK_ESCAPE:    return SDL_GetScancodeName(SDL_SCANCODE_ESCAPE);
    case SDLK_BACKSPACE: return SDL_GetScancodeName(SDL_SCANCODE_BACKSPACE);
    case SDLK_TAB:       return SDL_GetScancodeName(SDL_SCANCODE_TAB);
    case SDLK_SPACE:     return SDL_GetScancodeName(SDL_SCANCODE_SPACE);
    case SDLK_DELETE:    return SDL_GetScancodeName(SDL_SCANCODE_DELETE);
    default: break;
    }

    /* a keycode is the unshifted key; the NAME is the printed letter, usually
       the shifted capital (stock SDL_GetKeyName). ASCII case here; non-ASCII
       capitalization would need the active keymap, which stock consults. */
    if (key >= 'a' && key <= 'z') {
        key += 'A' - 'a';
    }
    if (key > 0 && key < 0x110000) {
        char name[8];
        ucs4_to_utf8((Uint32)key, name);
        return intern_key_name(name);
    }
    return "";
}

SDL_Keycode SDL_GetKeyFromName(const char *name)
{
    if (!name) {
        return SDLK_UNKNOWN;
    }

    /* a single UTF-8 character is the keycode itself (stock decoding) */
    Uint32 key = (Uint8)*name;
    size_t len = strlen(name);
    if (key >= 0xF0) {
        if (len == 4) {
            key = (key & 0x07) << 18;
            key |= ((Uint32)(Uint8)name[1] & 0x3F) << 12;
            key |= ((Uint32)(Uint8)name[2] & 0x3F) << 6;
            key |= (Uint32)(Uint8)name[3] & 0x3F;
        } else {
            key = 0;
        }
    } else if (key >= 0xE0) {
        if (len == 3) {
            key = (key & 0x0F) << 12;
            key |= ((Uint32)(Uint8)name[1] & 0x3F) << 6;
            key |= (Uint32)(Uint8)name[2] & 0x3F;
        } else {
            key = 0;
        }
    } else if (key >= 0xC0) {
        if (len == 2) {
            key = (key & 0x1F) << 6;
            key |= (Uint32)(Uint8)name[1] & 0x3F;
        } else {
            key = 0;
        }
    } else if (len != 1) {
        key = 0;
    }
    if (key) {
        return (SDL_Keycode)key;
    }

    for (size_t i = 0; i < SDL_arraysize(sdlop_extended_key_names); i++) {
        if (strcasecmp(name, sdlop_extended_key_names[i]) == 0) {
            return (SDL_Keycode)(SDLK_EXTENDED_MASK | (SDL_Keycode)(i + 1));
        }
    }

    for (int i = 0; i < SDL_SCANCODE_COUNT; i++) {
        const char *sc_name = SDL_GetScancodeName((SDL_Scancode)i);
        if (*sc_name && *sc_name == *name && strcasecmp(name, sc_name) == 0) {
            return (SDL_Keycode)i | SDLK_SCANCODE_MASK;
        }
    }

    return SDLK_UNKNOWN;
}

