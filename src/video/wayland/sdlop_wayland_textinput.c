/*
  SDLop - Wayland text input / IME (zwp_text_input_v3).

  Port of stock SDL3's SDL_waylandkeyboard.c / text_input listener:
    - one zwp_text_input_v3 per seat; enabled while the keyboard-focused
      window has SDL text input active (enable resets the IME state, so it
      is not repeated), disabled otherwise
    - content type from SDL_PROP_TEXTINPUT_* (type, capitalization,
      autocorrect, multiline), cursor rectangle from SDL_SetTextInputArea
      (x clamped to the area as in stock)
    - preedit_string -> SDL_EVENT_TEXT_EDITING (cursor bytes converted to
      characters), commit_string -> SDL_EVENT_TEXT_INPUT, done without a
      preedit -> empty SDL_EVENT_TEXT_EDITING
  Keys the IME does not consume still arrive as wl_keyboard keys and produce
  text locally (stock). Beyond stock, SDL_ClearComposition re-enables the
  text input, which resets the IME's composition.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "sdlop_wayland_internal.h"
#include "sdlop_wayland_dyn.h"
#include "text-input-unstable-v3-client-protocol.h"

#include <string.h>

static struct
{
    struct zwp_text_input_manager_v3 *manager;
    struct wl_seat *seat;
    struct zwp_text_input_v3 *ti;
    bool enabled;
    bool has_preedit;
    SDL_Window *enabled_window;
    SDL_Rect rect; /* last cursor rectangle sent */
    int cursor;
} ti;

static void ti_enter(void *data, struct zwp_text_input_v3 *t, struct wl_surface *surface)
{
    (void)data;
    (void)t;
    (void)surface;
}

static void ti_leave(void *data, struct zwp_text_input_v3 *t, struct wl_surface *surface)
{
    (void)data;
    (void)t;
    (void)surface;
}

static void ti_preedit_string(void *data, struct zwp_text_input_v3 *t, const char *text, int32_t cursor_begin,
                              int32_t cursor_end)
{
    (void)data;
    (void)t;
    ti.has_preedit = true;
    if (!text) {
        SDLOP_SendEditingText("", 0, 0);
        return;
    }
    const size_t len = strlen(text);
    const int begin = cursor_begin >= 0 ? (int)SDL_utf8strnlen(text, (size_t)cursor_begin < len ? (size_t)cursor_begin : len) : -1;
    const int end = cursor_end >= 0 ? (int)SDL_utf8strnlen(text, (size_t)cursor_end < len ? (size_t)cursor_end : len) : -1;
    int size;
    if (end >= 0) {
        size = begin >= 0 ? end - begin : end;
    } else {
        size = -1;
    }
    SDLOP_SendEditingText(text, begin, size);
}

static void ti_commit_string(void *data, struct zwp_text_input_v3 *t, const char *text)
{
    (void)data;
    (void)t;
    if (text) {
        SDLOP_SendKeyboardText(text, 0);
    }
}

static void ti_delete_surrounding_text(void *data, struct zwp_text_input_v3 *t, uint32_t before, uint32_t after)
{
    /* SDL has no surrounding text (stock ignores it too) */
    (void)data;
    (void)t;
    (void)before;
    (void)after;
}

static void ti_done(void *data, struct zwp_text_input_v3 *t, uint32_t serial)
{
    (void)data;
    (void)t;
    (void)serial;
    if (!ti.has_preedit) {
        SDLOP_SendEditingText("", 0, 0);
    }
    ti.has_preedit = false;
}

static const struct zwp_text_input_v3_listener ti_listener = {
    ti_enter, ti_leave, ti_preedit_string, ti_commit_string, ti_delete_surrounding_text, ti_done,
};

static void ensure_text_input(void)
{
    if (!ti.ti && ti.manager && ti.seat) {
        ti.ti = zwp_text_input_manager_v3_get_text_input(ti.manager, ti.seat);
        if (ti.ti) {
            zwp_text_input_v3_add_listener(ti.ti, &ti_listener, NULL);
        }
    }
}

bool SDLOP_Wayland_TextInput_Global(struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    (void)version;
    if (strcmp(interface, zwp_text_input_manager_v3_interface.name) != 0 || ti.manager ||
        SDLOP_HintFlag(SDLOP_HINT_WAYLAND_NO_TEXT_INPUT, false)) {
        return false;
    }
    ti.manager = wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1);
    ensure_text_input();
    return true;
}

void SDLOP_Wayland_TextInput_SeatAdded(struct wl_seat *seat)
{
    if (!ti.seat) {
        ti.seat = seat;
        ensure_text_input();
    }
}

bool SDLOP_Wayland_TextInput_Available(void)
{
    return ti.manager != NULL;
}

static void send_cursor_rect(SDL_Window *window)
{
    const SDL_Rect *r = &window->text_input_rect;
    if (r->w <= 0 || r->h <= 0) {
        return;
    }
    /* window coordinates are surface coordinates: no scaling */
    const int x = r->x + window->text_input_cursor < r->x + r->w ? r->x + window->text_input_cursor : r->x + r->w;
    zwp_text_input_v3_set_cursor_rectangle(ti.ti, x, r->y, 1, r->h);
    ti.rect = *r;
    ti.cursor = window->text_input_cursor;
}

static void content_type(SDL_PropertiesID props, uint32_t *hint_out, uint32_t *purpose_out)
{
    uint32_t hint = ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE;
    uint32_t purpose;
    switch (SDLOP_GetTextInputType(props)) {
    default:
    case SDL_TEXTINPUT_TYPE_TEXT:
        purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
        break;
    case SDL_TEXTINPUT_TYPE_TEXT_NAME:
        purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NAME;
        break;
    case SDL_TEXTINPUT_TYPE_TEXT_EMAIL:
        purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_EMAIL;
        break;
    case SDL_TEXTINPUT_TYPE_TEXT_USERNAME:
        purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
        hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA;
        break;
    case SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN:
        purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
        hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT | ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA;
        break;
    case SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_VISIBLE:
        purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
        hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA;
        break;
    case SDL_TEXTINPUT_TYPE_NUMBER:
        purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NUMBER;
        break;
    case SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN:
        purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PIN;
        hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT | ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA;
        break;
    case SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_VISIBLE:
        purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PIN;
        hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA;
        break;
    }
    switch (SDLOP_GetTextInputCapitalization(props)) {
    default:
    case SDL_CAPITALIZE_NONE:
        break;
    case SDL_CAPITALIZE_LETTERS:
        hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_UPPERCASE;
        break;
    case SDL_CAPITALIZE_WORDS:
        hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_TITLECASE;
        break;
    case SDL_CAPITALIZE_SENTENCES:
        hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_AUTO_CAPITALIZATION;
        break;
    }
    if (SDLOP_GetTextInputAutocorrect(props)) {
        hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_COMPLETION | ZWP_TEXT_INPUT_V3_CONTENT_HINT_SPELLCHECK;
    }
    if (SDLOP_GetTextInputMultiline(props)) {
        hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE;
    }
    *hint_out = hint;
    *purpose_out = purpose;
}

static void do_enable(SDL_Window *window)
{
    uint32_t hint, purpose;
    content_type(window->text_input_props, &hint, &purpose);
    zwp_text_input_v3_enable(ti.ti);
    zwp_text_input_v3_set_content_type(ti.ti, hint, purpose);
    SDL_zero(ti.rect);
    ti.cursor = 0;
    send_cursor_rect(window);
    zwp_text_input_v3_commit(ti.ti);
    ti.enabled = true;
    ti.enabled_window = window;
}

static void do_disable(void)
{
    zwp_text_input_v3_disable(ti.ti);
    zwp_text_input_v3_commit(ti.ti);
    ti.enabled = false;
    ti.enabled_window = NULL;
    ti.has_preedit = false;
    SDL_zero(ti.rect);
    ti.cursor = 0;
}

/* (re)evaluate after keyboard focus or text-input state changes */
void SDLOP_Wayland_TextInput_Update(void)
{
    if (!ti.ti) {
        return;
    }
    SDL_Window *focus = sdlop.keyboard_focus;
    const bool want = focus && focus->text_input_active;
    if (want && (!ti.enabled || ti.enabled_window != focus)) {
        do_enable(focus); /* enable resets all state: only when needed */
    } else if (!want && ti.enabled) {
        do_disable();
    }
    struct wl_display *display = SDLOP_Wayland_GetDisplay();
    if (display) {
        wl_display_flush(display);
    }
}

bool SDLOP_Wayland_TextInput_Start(SDL_Window *window)
{
    if (ti.ti && ti.enabled && ti.enabled_window == window) {
        /* new properties: re-enable so they take effect (stock re-applies
         * them on the next enable) */
        do_enable(window);
        wl_display_flush(SDLOP_Wayland_GetDisplay());
        return true;
    }
    SDLOP_Wayland_TextInput_Update();
    return true;
}

bool SDLOP_Wayland_TextInput_Stop(SDL_Window *window)
{
    (void)window;
    SDLOP_Wayland_TextInput_Update();
    return true;
}

bool SDLOP_Wayland_TextInput_UpdateArea(SDL_Window *window)
{
    if (ti.ti && ti.enabled && ti.enabled_window == window) {
        const SDL_Rect *r = &window->text_input_rect;
        if (r->x != ti.rect.x || r->y != ti.rect.y || r->w != ti.rect.w || r->h != ti.rect.h ||
            window->text_input_cursor != ti.cursor) {
            send_cursor_rect(window);
            zwp_text_input_v3_commit(ti.ti);
            wl_display_flush(SDLOP_Wayland_GetDisplay());
        }
    }
    return true;
}

bool SDLOP_Wayland_TextInput_ClearComposition(SDL_Window *window)
{
    if (ti.ti && ti.enabled && ti.enabled_window == window) {
        /* enable resets the input method state, including its preedit */
        do_enable(window);
        wl_display_flush(SDLOP_Wayland_GetDisplay());
        SDLOP_SendEditingText("", 0, 0);
    }
    return true;
}

void SDLOP_Wayland_TextInput_WindowDestroyed(SDL_Window *window)
{
    if (ti.enabled_window == window) {
        if (ti.ti) {
            do_disable();
        }
        ti.enabled_window = NULL;
    }
}

void SDLOP_Wayland_TextInput_Quit(void)
{
    if (ti.ti) {
        zwp_text_input_v3_destroy(ti.ti);
    }
    if (ti.manager) {
        zwp_text_input_manager_v3_destroy(ti.manager);
    }
    memset(&ti, 0, sizeof(ti));
}
