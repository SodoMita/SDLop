/*
  SDLop - clipboard (SDL_clipboard.h).

  Port of SDL3's generic clipboard layer (src/video/SDL_clipboard.c,
  release-3.4.20): the application's offer (callback + mime types) is kept
  here and handed to the video driver; drivers without clipboard support
  get a working in-process clipboard, as in SDL3.

  Copyright (C) 1997-2026 Sam Lantinga (SDL3 parts), zlib license.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

static Uint32 clipboard_sequence;
static SDL_ClipboardDataCallback clipboard_callback;
static SDL_ClipboardCleanupCallback clipboard_cleanup;
static void *clipboard_userdata;
static char **clipboard_mime_types;
static size_t num_clipboard_mime_types;
static char *primary_selection_text;

static bool uninitialized_video(void)
{
    return SDL_SetError("Video subsystem has not been initialized");
}

static void free_mime_types(void)
{
    if (clipboard_mime_types) {
        for (size_t i = 0; i < num_clipboard_mime_types; ++i) {
            SDL_free(clipboard_mime_types[i]);
        }
        SDL_free(clipboard_mime_types);
        clipboard_mime_types = NULL;
        num_clipboard_mime_types = 0;
    }
}

static void cancel_clipboard_data(Uint32 sequence)
{
    if (sequence && sequence != clipboard_sequence) {
        return; /* already canceled */
    }
    if (clipboard_cleanup) {
        clipboard_cleanup(clipboard_userdata);
    }
    free_mime_types();
    clipboard_callback = NULL;
    clipboard_cleanup = NULL;
    clipboard_userdata = NULL;
}

static bool save_mime_types(const char *const *mime_types, size_t num_mime_types)
{
    free_mime_types();
    if (mime_types && num_mime_types > 0) {
        size_t num_allocated = 0;
        clipboard_mime_types = (char **)SDL_malloc(num_mime_types * sizeof(char *));
        if (clipboard_mime_types) {
            for (size_t i = 0; i < num_mime_types; ++i) {
                clipboard_mime_types[i] = SDL_strdup(mime_types[i]);
                if (clipboard_mime_types[i]) {
                    ++num_allocated;
                }
            }
        }
        if (num_allocated < num_mime_types) {
            num_clipboard_mime_types = num_allocated;
            free_mime_types();
            return false;
        }
        num_clipboard_mime_types = num_mime_types;
    }
    return true;
}

/* one allocation: pointer array followed by the strings */
static char **copy_mime_types(const char *const *mime_types, size_t num_mime_types, bool temporary)
{
    size_t alloc_size = sizeof(char *);
    for (size_t i = 0; i < num_mime_types; i++) {
        alloc_size += sizeof(char *) + SDL_strlen(mime_types[i]) + 1;
    }
    char *ret = temporary ? (char *)SDLOP_AllocateTemporaryMemory(alloc_size) : (char *)SDL_malloc(alloc_size);
    if (!ret) {
        return NULL;
    }
    char **result = (char **)ret;
    ret += sizeof(char *) * (num_mime_types + 1);
    for (size_t i = 0; i < num_mime_types; i++) {
        size_t len = SDL_strlen(mime_types[i]) + 1;
        result[i] = ret;
        SDL_memcpy(ret, mime_types[i], len);
        ret += len;
    }
    result[num_mime_types] = NULL;
    return result;
}

void SDLOP_SendClipboardUpdate(bool owner, char **mime_types, size_t num_mime_types)
{
    if (!owner) {
        cancel_clipboard_data(0);
        save_mime_types((const char *const *)mime_types, num_mime_types);
    }
    if (SDL_EventEnabled(SDL_EVENT_CLIPBOARD_UPDATE)) {
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_EVENT_CLIPBOARD_UPDATE;
        event.clipboard.owner = owner;
        event.clipboard.mime_types = (const char **)mime_types;
        event.clipboard.num_mime_types = (Sint32)num_mime_types;
        SDL_PushEvent(&event);
    }
}

const char *const *SDLOP_GetClipboardOfferMimeTypes(size_t *count)
{
    *count = num_clipboard_mime_types;
    return (const char *const *)clipboard_mime_types;
}

bool SDLOP_HasInternalClipboardData(const char *mime_type)
{
    for (size_t i = 0; i < num_clipboard_mime_types; ++i) {
        if (SDL_strcmp(mime_type, clipboard_mime_types[i]) == 0) {
            return true;
        }
    }
    return false;
}

void *SDLOP_GetInternalClipboardData(const char *mime_type, size_t *size)
{
    void *data = NULL;
    *size = 0;
    if (clipboard_callback) {
        if (!SDLOP_HasInternalClipboardData(mime_type)) {
            return NULL; /* the callback didn't advertise this type */
        }
        const void *provided = clipboard_callback(clipboard_userdata, mime_type, size);
        if (provided) {
            data = SDL_malloc(*size + sizeof(Uint32));
            if (data) {
                SDL_memcpy(data, provided, *size);
                SDL_memset((Uint8 *)data + *size, 0, sizeof(Uint32));
            } else {
                *size = 0;
            }
        } else {
            *size = 0;
        }
    }
    return data;
}

const void *SDLCALL SDLOP_ClipboardTextCallback(void *userdata, const char *mime_type, size_t *size)
{
    (void)mime_type;
    char *text = (char *)userdata;
    *size = text ? SDL_strlen(text) : 0;
    return text;
}

static const char *const *get_text_mime_types(size_t *num_mime_types)
{
    if (sdlop.video && sdlop.video->GetTextMimeTypes) {
        return sdlop.video->GetTextMimeTypes(sdlop.video, num_mime_types);
    }
    static const char *const text_mime_types[] = { "text/plain;charset=utf-8" };
    *num_mime_types = SDL_arraysize(text_mime_types);
    return text_mime_types;
}

/* SDL_Quit() */
void SDLOP_QuitClipboard(void)
{
    cancel_clipboard_data(0);
    clipboard_sequence = 0;
    SDL_free(primary_selection_text);
    primary_selection_text = NULL;
}

bool SDL_SetClipboardData(SDL_ClipboardDataCallback callback, SDL_ClipboardCleanupCallback cleanup, void *userdata,
                          const char *const *mime_types, size_t num_mime_types)
{
    if (!sdlop.video) {
        return uninitialized_video();
    }
    if (!((callback && mime_types && num_mime_types > 0) ||
          (!callback && !mime_types && num_mime_types == 0))) {
        return SDL_SetError("Invalid parameters");
    }

    cancel_clipboard_data(0);

    ++clipboard_sequence;
    if (!clipboard_sequence) {
        clipboard_sequence = 1;
    }
    clipboard_callback = callback;
    clipboard_cleanup = cleanup;
    clipboard_userdata = userdata;

    if (!save_mime_types(mime_types, num_mime_types)) {
        SDL_ClearClipboardData();
        return false;
    }

    if (sdlop.video->SetClipboardData) {
        if (!sdlop.video->SetClipboardData(sdlop.video)) {
            return false;
        }
    }

    char **copy = copy_mime_types(mime_types, num_mime_types, true);
    if (!copy) {
        return SDL_SetError("unable to copy current mime types");
    }
    SDLOP_SendClipboardUpdate(true, copy, num_mime_types);
    return true;
}

bool SDL_ClearClipboardData(void)
{
    return SDL_SetClipboardData(NULL, NULL, NULL, NULL, 0);
}

void *SDL_GetClipboardData(const char *mime_type, size_t *size)
{
    size_t unused;
    if (!sdlop.video) {
        uninitialized_video();
        return NULL;
    }
    if (!mime_type) {
        SDL_InvalidParamError("mime_type");
        return NULL;
    }
    if (!size) {
        size = &unused;
    }
    *size = 0;
    if (sdlop.video->GetClipboardData) {
        return sdlop.video->GetClipboardData(sdlop.video, mime_type, size);
    }
    return SDLOP_GetInternalClipboardData(mime_type, size);
}

bool SDL_HasClipboardData(const char *mime_type)
{
    if (!sdlop.video) {
        return uninitialized_video();
    }
    if (!mime_type) {
        return SDL_InvalidParamError("mime_type");
    }
    if (sdlop.video->HasClipboardData) {
        return sdlop.video->HasClipboardData(sdlop.video, mime_type);
    }
    return SDLOP_HasInternalClipboardData(mime_type);
}

char **SDL_GetClipboardMimeTypes(size_t *num_mime_types)
{
    if (num_mime_types) {
        *num_mime_types = 0;
    }
    if (!sdlop.video) {
        uninitialized_video();
        return NULL;
    }
    if (num_mime_types) {
        *num_mime_types = num_clipboard_mime_types;
    }
    return copy_mime_types((const char *const *)clipboard_mime_types, num_clipboard_mime_types, false);
}

bool SDL_SetClipboardText(const char *text)
{
    if (!sdlop.video) {
        return uninitialized_video();
    }
    if (text && *text) {
        size_t n;
        const char *const *types = get_text_mime_types(&n);
        char *copy = SDL_strdup(text);
        if (!copy) {
            return false;
        }
        return SDL_SetClipboardData(SDLOP_ClipboardTextCallback, SDL_free, copy, types, n);
    }
    return SDL_ClearClipboardData();
}

char *SDL_GetClipboardText(void)
{
    if (!sdlop.video) {
        uninitialized_video();
        return SDL_strdup("");
    }
    size_t n, length;
    const char *const *types = get_text_mime_types(&n);
    for (size_t i = 0; i < n; ++i) {
        void *data = SDL_GetClipboardData(types[i], &length);
        if (data) {
            return (char *)data;
        }
    }
    return SDL_strdup("");
}

bool SDL_HasClipboardText(void)
{
    if (!sdlop.video) {
        return uninitialized_video();
    }
    size_t n;
    const char *const *types = get_text_mime_types(&n);
    for (size_t i = 0; i < n; ++i) {
        if (SDL_HasClipboardData(types[i])) {
            return true;
        }
    }
    return false;
}

bool SDL_SetPrimarySelectionText(const char *text)
{
    if (!sdlop.video) {
        return uninitialized_video();
    }
    if (!text) {
        text = "";
    }
    if (sdlop.video->SetPrimarySelectionText) {
        if (!sdlop.video->SetPrimarySelectionText(sdlop.video, text)) {
            return false;
        }
    } else {
        SDL_free(primary_selection_text);
        primary_selection_text = SDL_strdup(text);
    }
    char **copy = copy_mime_types((const char *const *)clipboard_mime_types, num_clipboard_mime_types, true);
    if (!copy) {
        return SDL_SetError("unable to copy current mime types");
    }
    SDLOP_SendClipboardUpdate(true, copy, num_clipboard_mime_types);
    return true;
}

char *SDL_GetPrimarySelectionText(void)
{
    if (!sdlop.video) {
        uninitialized_video();
        return SDL_strdup("");
    }
    if (sdlop.video->GetPrimarySelectionText) {
        return sdlop.video->GetPrimarySelectionText(sdlop.video);
    }
    return SDL_strdup(primary_selection_text ? primary_selection_text : "");
}

bool SDL_HasPrimarySelectionText(void)
{
    if (!sdlop.video) {
        return uninitialized_video();
    }
    if (sdlop.video->HasPrimarySelectionText) {
        return sdlop.video->HasPrimarySelectionText(sdlop.video);
    }
    return primary_selection_text && primary_selection_text[0] != '\0';
}
