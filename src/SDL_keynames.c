/*
  SDLop - scancode/key names, key-name lookups, text-input gating.

  Shared by the native (xkb) and web (DOM) keyboard backends. Semantics
  follow stock SDL3 (3.2.10 - 3.4.20): names resolve through the scancode table,
  the extended-key table and UCS4->UTF-8 with stable interned pointers,
  and TEXT_INPUT is only delivered while SDL_StartTextInput() is active.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <string.h>
#include <strings.h>

#define CHECK_WINDOW(window, retval)                                  \
    if (!sdlop.video) {                                               \
        SDL_SetError("Video subsystem has not been initialized");     \
        return retval;                                                \
    }                                                                 \
    if (!SDLOP_WindowValid(window)) {                                 \
        SDL_SetError("Invalid window");                               \
        return retval;                                                \
    }

bool SDL_StartTextInputWithProperties(SDL_Window *window, SDL_PropertiesID props)
{
    CHECK_WINDOW(window, false);
    /* kept for SDL_PROP_TEXTINPUT_* queries; drivers with an IME read them
     * in StartTextInput */
    if (window->text_input_props) {
        SDL_DestroyProperties(window->text_input_props);
        window->text_input_props = 0;
    }
    if (props) {
        window->text_input_props = SDL_CreateProperties();
        if (!window->text_input_props) {
            return false;
        }
        if (!SDL_CopyProperties(props, window->text_input_props)) {
            return false;
        }
    }
    const bool was_active = window->text_input_active;
    window->text_input_active = true;
    /* stock: (re)starting applies the new properties */
    if (sdlop.video && sdlop.video->StartTextInput &&
        !sdlop.video->StartTextInput(sdlop.video, window, window->text_input_props)) {
        window->text_input_active = was_active;
        return false;
    }
    return true;
}

SDL_TextInputType SDLOP_GetTextInputType(SDL_PropertiesID props)
{
    return (SDL_TextInputType)SDL_GetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT);
}

SDL_Capitalization SDLOP_GetTextInputCapitalization(SDL_PropertiesID props)
{
    if (SDL_HasProperty(props, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER)) {
        return (SDL_Capitalization)SDL_GetNumberProperty(props, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER,
                                                         SDL_CAPITALIZE_NONE);
    }
    switch (SDLOP_GetTextInputType(props)) {
    case SDL_TEXTINPUT_TYPE_TEXT:
        return SDL_CAPITALIZE_SENTENCES;
    case SDL_TEXTINPUT_TYPE_TEXT_NAME:
        return SDL_CAPITALIZE_WORDS;
    default:
        return SDL_CAPITALIZE_NONE;
    }
}

bool SDLOP_GetTextInputAutocorrect(SDL_PropertiesID props)
{
    return SDL_GetBooleanProperty(props, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, true);
}

bool SDLOP_GetTextInputMultiline(SDL_PropertiesID props)
{
    if (SDL_HasProperty(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN)) {
        return SDL_GetBooleanProperty(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);
    }
    return !SDL_GetHintBoolean(SDL_HINT_RETURN_KEY_HIDES_IME, false);
}

bool SDL_StartTextInput(SDL_Window *window)
{
    return SDL_StartTextInputWithProperties(window, 0);
}

bool SDL_StopTextInput(SDL_Window *window)
{
    CHECK_WINDOW(window, false);
    if (window->text_input_active) {
        window->text_input_active = false;
        if (sdlop.video && sdlop.video->StopTextInput) {
            sdlop.video->StopTextInput(sdlop.video, window);
        }
    }
    return true;
}

bool SDL_TextInputActive(SDL_Window *window)
{
    CHECK_WINDOW(window, false);
    return window->text_input_active;
}

bool SDL_SetTextInputArea(SDL_Window *window, const SDL_Rect *rect, int cursor)
{
    CHECK_WINDOW(window, false);
    if (rect) {
        window->text_input_rect = *rect;
        window->text_input_cursor = cursor;
    } else {
        SDL_zero(window->text_input_rect);
        window->text_input_cursor = 0;
    }
    if (sdlop.video->UpdateTextInputArea && !sdlop.video->UpdateTextInputArea(sdlop.video, window)) {
        return false;
    }
    return true;
}

bool SDL_GetTextInputArea(SDL_Window *window, SDL_Rect *rect, int *cursor)
{
    CHECK_WINDOW(window, false);
    if (rect) {
        *rect = window->text_input_rect;
    }
    if (cursor) {
        *cursor = window->text_input_cursor;
    }
    return true;
}

bool SDL_ClearComposition(SDL_Window *window)
{
    CHECK_WINDOW(window, false);
    /* compose sequences are resolved per key press; an IME may hold a
     * composition */
    if (sdlop.video && sdlop.video->ClearComposition) {
        return sdlop.video->ClearComposition(sdlop.video, window);
    }
    return true;
}

bool SDL_HasScreenKeyboardSupport(void)
{
    return false; /* desktop/web backends: no on-screen keyboard */
}

bool SDL_ScreenKeyboardShown(SDL_Window *window)
{
    CHECK_WINDOW(window, false);
    return false;
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
    /* the queue stores up to 63 bytes per event: longer text (IME commits)
     * goes out in several events, split at character boundaries */
    const size_t max_chunk = 63;
    while (*utf8) {
        char chunk[64];
        size_t n = strlen(utf8);
        if (n > max_chunk) {
            n = max_chunk;
            while (n > 0 && (((unsigned char)utf8[n]) & 0xC0) == 0x80) {
                n--; /* don't split a UTF-8 sequence */
            }
            if (n == 0) {
                n = max_chunk; /* malformed: avoid looping forever */
            }
        }
        memcpy(chunk, utf8, n);
        chunk[n] = '\0';
        utf8 += n;
        SDL_Event event;
        SDL_zero(event);
        event.text.type = SDL_EVENT_TEXT_INPUT;
        event.text.timestamp = timestamp_ns ? SDLOP_MonoToSDLTicks(timestamp_ns) : SDL_GetTicksNS();
        event.text.windowID = SDLOP_FocusWindowID();
        event.text.text = chunk; /* copied into slot storage by the queue */
        SDLOP_PushEventInternal(&event);
    }
}

void SDLOP_SendEditingText(const char *utf8, int start, int length)
{
    if (!utf8 || !sdlop.keyboard_focus || !sdlop.keyboard_focus->text_input_active ||
        !SDL_EventEnabled(SDL_EVENT_TEXT_EDITING)) {
        return;
    }
    const size_t n = strlen(utf8) + 1;
    char *copy = (char *)SDLOP_AllocateTemporaryMemory(n); /* travels with the event */
    if (!copy) {
        return;
    }
    memcpy(copy, utf8, n);
    SDL_Event event;
    SDL_zero(event);
    event.edit.type = SDL_EVENT_TEXT_EDITING;
    event.edit.timestamp = SDL_GetTicksNS();
    event.edit.windowID = SDLOP_FocusWindowID();
    event.edit.text = copy;
    event.edit.start = start;
    event.edit.length = length;
    SDLOP_PushEventInternal(&event);
}

void SDLOP_SendEditingTextCandidates(char **candidates, int num_candidates, int selected_candidate, bool horizontal)
{
    if (!sdlop.keyboard_focus || !sdlop.keyboard_focus->text_input_active ||
        !SDL_EventEnabled(SDL_EVENT_TEXT_EDITING_CANDIDATES)) {
        return;
    }
    /* one block: the pointer array (NULL-terminated) followed by the strings */
    size_t total = (size_t)(num_candidates + 1) * sizeof(char *);
    for (int i = 0; i < num_candidates; i++) {
        total += strlen(candidates[i]) + 1;
    }
    const char **list = (const char **)SDLOP_AllocateTemporaryMemory(total);
    if (!list) {
        return;
    }
    char *strings = (char *)(list + num_candidates + 1);
    for (int i = 0; i < num_candidates; i++) {
        const size_t n = strlen(candidates[i]) + 1;
        memcpy(strings, candidates[i], n);
        list[i] = strings;
        strings += n;
    }
    list[num_candidates] = NULL;
    SDL_Event event;
    SDL_zero(event);
    event.edit_candidates.type = SDL_EVENT_TEXT_EDITING_CANDIDATES;
    event.edit_candidates.timestamp = SDL_GetTicksNS();
    event.edit_candidates.windowID = SDLOP_FocusWindowID();
    event.edit_candidates.candidates = num_candidates > 0 ? list : NULL;
    event.edit_candidates.num_candidates = num_candidates;
    event.edit_candidates.selected_candidate = selected_candidate;
    event.edit_candidates.horizontal = horizontal;
    SDLOP_PushEventInternal(&event);
}


/* ------------------------------------------------------------------ */
/* Scancode / key names (generated, SDL3-style)                        */
/* ------------------------------------------------------------------ */

#include "scancode_names.h"

/* SDL_SetScancodeName overrides (application-owned strings) */
static const char *sdlop_custom_scancode_names[SDL_SCANCODE_COUNT];
static bool sdlop_custom_scancode_set[SDL_SCANCODE_COUNT];

static const char *scancode_name_raw(int scancode)
{
    return sdlop_custom_scancode_set[scancode] ? sdlop_custom_scancode_names[scancode]
                                               : sdlop_scancode_names[scancode];
}

bool SDL_SetScancodeName(SDL_Scancode scancode, const char *name)
{
    if ((int)scancode < SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
        return SDL_SetError("Parameter '%s' is invalid", "scancode");
    }
    sdlop_custom_scancode_names[scancode] = name;
    sdlop_custom_scancode_set[scancode] = true;
    return true;
}

const char *SDL_GetScancodeName(SDL_Scancode scancode)
{
    if ((int)scancode < SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
        SDL_SetError("Parameter '%s' is invalid", "scancode");
        return "";
    }
    const char *name = scancode_name_raw(scancode);
    return name ? name : "";
}

SDL_Scancode SDL_GetScancodeFromName(const char *name)
{
    if (!name || !*name) {
        SDL_SetError("Parameter '%s' is invalid", "name");
        return SDL_SCANCODE_UNKNOWN;
    }
    for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) {
        const char *sc_name = scancode_name_raw(i);
        if (sc_name && strcasecmp(name, sc_name) == 0) {
            return (SDL_Scancode)i;
        }
    }
    SDL_SetError("Parameter '%s' is invalid", "name");
    return SDL_SCANCODE_UNKNOWN;
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
       the shifted capital, looked up on the active keymap (stock) */
    key = SDLOP_KeyboardKeyNameCapital(key);
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
        /* SDL3: a keycode is the unshifted key, but a name is the symbol
         * printed on the key - usually the shifted one ("A" names 'a'). So a
         * single character that needs shift/caps on the current layout maps
         * back to the unmodified keycode of that key. */
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)key, &mod);
        if (sc != SDL_SCANCODE_UNKNOWN && (mod & (SDL_KMOD_SHIFT | SDL_KMOD_CAPS))) {
            return SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
        }
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
            /* SDL3: the key that scancode produces ("Space" -> ' ',
             * "Return" -> '\r'); non-character keys stay scancode-masked */
            return SDL_GetKeyFromScancode((SDL_Scancode)i, SDL_KMOD_NONE, false);
        }
    }

    return SDLK_UNKNOWN;
}

