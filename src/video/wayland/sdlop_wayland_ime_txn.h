/*
  SDLop - text-input-v3 transaction state (no Wayland dependency).

  zwp_text_input_v3 double-buffers preedit_string, commit_string and
  delete_surrounding_text until `done`, which applies them in a fixed order
  regardless of their order on the wire:
    1. replace the existing preedit with nothing
    2. (delete surrounding text: SDL has none)
    3. insert the commit string
    4. insert the new preedit, with its cursor
  The listeners only record; SDLOP_IMETxnDone() emits the SDL events, so an
  application never sees half a transaction (e.g. when `done` arrives in a
  later socket read), and a new preedit is never followed by the commit that
  logically precedes it.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#ifndef SDLOP_WAYLAND_IME_TXN_H
#define SDLOP_WAYLAND_IME_TXN_H

#include <SDL3/SDL_stdinc.h>

typedef struct SDLOP_IMETxn
{
    char *preedit; /* pending; NULL = empty */
    int begin, size; /* pending preedit cursor, in characters (SDL) */
    char *commit; /* pending; NULL = none */
    bool visible; /* the application currently shows a non-empty preedit */
} SDLOP_IMETxn;

typedef void (*SDLOP_IMEEditFn)(const char *text, int start, int length);
typedef void (*SDLOP_IMETextFn)(const char *text);

static inline void SDLOP_IMETxnSetPreedit(SDLOP_IMETxn *t, const char *text, int cursor_begin, int cursor_end)
{
    SDL_free(t->preedit);
    t->preedit = NULL;
    t->begin = 0;
    t->size = 0;
    if (!text || !*text) {
        return;
    }
    t->preedit = SDL_strdup(text);
    /* byte offsets -> characters; -1 (hidden cursor) stays -1 */
    const size_t len = SDL_strlen(text);
    const int begin = cursor_begin >= 0 ? (int)SDL_utf8strnlen(text, (size_t)cursor_begin < len ? (size_t)cursor_begin : len) : -1;
    const int end = cursor_end >= 0 ? (int)SDL_utf8strnlen(text, (size_t)cursor_end < len ? (size_t)cursor_end : len) : -1;
    t->begin = begin;
    t->size = end >= 0 ? (begin >= 0 ? end - begin : end) : -1;
}

static inline void SDLOP_IMETxnSetCommit(SDLOP_IMETxn *t, const char *text)
{
    SDL_free(t->commit);
    t->commit = text && *text ? SDL_strdup(text) : NULL;
}

/* drop pending state (enable/disable reset the input method) */
static inline void SDLOP_IMETxnReset(SDLOP_IMETxn *t)
{
    SDL_free(t->preedit);
    SDL_free(t->commit);
    t->preedit = t->commit = NULL;
    t->begin = t->size = 0;
    t->visible = false;
}

static inline void SDLOP_IMETxnDone(SDLOP_IMETxn *t, SDLOP_IMEEditFn edit, SDLOP_IMETextFn text)
{
    if (t->commit) {
        if (t->visible) {
            edit("", 0, 0); /* 1: the old preedit goes before the commit lands */
            t->visible = false;
        }
        text(t->commit); /* 3 */
    }
    if (t->preedit) {
        edit(t->preedit, t->begin, t->size); /* 4 */
        t->visible = true;
    } else {
        edit("", 0, 0); /* no preedit in this transaction: none (stock) */
        t->visible = false;
    }
    /* the pending state returns to its initial values after done */
    SDL_free(t->preedit);
    SDL_free(t->commit);
    t->preedit = t->commit = NULL;
    t->begin = t->size = 0;
}

#endif
