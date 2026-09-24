/*
  SDLop -- SDL_hints.h.

  Hints are a small table of name/value strings with priorities, an environment
  variable fallback (SDL3 reads an environment variable with the hint's own name)
  and change callbacks. Only a handful of hints change SDLop's behaviour; see
  docs/HINTS.md for the list.
*/

#include "../sdlop_internal.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef struct SDLOP_HintCallback
{
    SDL_HintCallback callback;
    void *userdata;
    struct SDLOP_HintCallback *next;
} SDLOP_HintCallback;

typedef struct SDLOP_Hint
{
    char *name;
    char *value;
    SDL_HintPriority priority;
    SDLOP_HintCallback *callbacks;
} SDLOP_Hint;

static pthread_mutex_t sdlop_hints_lock = PTHREAD_MUTEX_INITIALIZER;
static SDLOP_Hint *sdlop_hints;
static int sdlop_num_hints;
static int sdlop_hints_capacity;

static SDLOP_Hint *sdlop_hint_for(const char *name, bool create)
{
    int i;
    if (!name || !name[0]) {
        return NULL;
    }
    for (i = 0; i < sdlop_num_hints; i++) {
        if (strcmp(sdlop_hints[i].name, name) == 0) {
            return &sdlop_hints[i];
        }
    }
    if (!create) {
        return NULL;
    }
    if (sdlop_num_hints == sdlop_hints_capacity) {
        int newcap = sdlop_hints_capacity ? sdlop_hints_capacity * 2 : 16;
        SDLOP_Hint *newhints = (SDLOP_Hint *)SDLOP_Realloc(sdlop_hints, (size_t)newcap * sizeof(*newhints));
        if (!newhints) {
            SDLOP_OutOfMemory();
            return NULL;
        }
        sdlop_hints = newhints;
        sdlop_hints_capacity = newcap;
    }
    memset(&sdlop_hints[sdlop_num_hints], 0, sizeof(sdlop_hints[0]));
    sdlop_hints[sdlop_num_hints].name = SDL_strdup(name);
    if (!sdlop_hints[sdlop_num_hints].name) {
        SDLOP_OutOfMemory();
        return NULL;
    }
    return &sdlop_hints[sdlop_num_hints++];
}

static void sdlop_notify_hint(const char *name, const char *old_value, const char *new_value)
{
    SDLOP_Hint *hint = sdlop_hint_for(name, false);
    SDLOP_HintCallback *node;
    if (!hint) {
        return;
    }
    node = hint->callbacks;
    while (node) {
        SDLOP_HintCallback *next = node->next;
        node->callback(node->userdata, name, old_value, new_value);
        node = next;
    }
}

bool SDL_SetHintWithPriority(const char *name, const char *value, SDL_HintPriority priority)
{
    SDLOP_Hint *hint;
    char *old_value = NULL;
    bool changed = false;

    if (!name || !name[0]) {
        return SDL_InvalidParamError("name");
    }
    pthread_mutex_lock(&sdlop_hints_lock);
    hint = sdlop_hint_for(name, true);
    if (!hint) {
        pthread_mutex_unlock(&sdlop_hints_lock);
        return false;
    }
    if (hint->value && priority < hint->priority) {
        pthread_mutex_unlock(&sdlop_hints_lock);
        return false;
    }
    if (hint->value) {
        old_value = SDL_strdup(hint->value);
    }
    SDLOP_Free(hint->value);
    hint->value = value ? SDL_strdup(value) : NULL;
    hint->priority = value ? priority : SDL_HINT_DEFAULT;
    changed = ((old_value == NULL) != (value == NULL)) ||
              (old_value && value && strcmp(old_value, value) != 0);
    pthread_mutex_unlock(&sdlop_hints_lock);

    if (changed) {
        sdlop_notify_hint(name, old_value, value);
    }
    SDLOP_Free(old_value);
    return true;
}

bool SDL_SetHint(const char *name, const char *value)
{
    return SDL_SetHintWithPriority(name, value, SDL_HINT_NORMAL);
}

static bool sdlop_remove_hint(const char *name)
{
    SDLOP_Hint *hint;
    char *old_value = NULL;
    bool existed;

    pthread_mutex_lock(&sdlop_hints_lock);
    hint = sdlop_hint_for(name, false);
    existed = (hint && hint->value != NULL);
    if (existed) {
        old_value = SDL_strdup(hint->value);
        SDLOP_Free(hint->value);
        hint->value = NULL;
        hint->priority = SDL_HINT_DEFAULT;
    }
    pthread_mutex_unlock(&sdlop_hints_lock);
    if (existed) {
        sdlop_notify_hint(name, old_value, NULL);
    }
    SDLOP_Free(old_value);
    return existed;
}

bool SDL_ResetHint(const char *name)
{
    return sdlop_remove_hint(name);
}

void SDL_ResetHints(void)
{
    int i;
    for (i = 0; i < sdlop_num_hints; i++) {
        sdlop_remove_hint(sdlop_hints[i].name);
    }
}

const char *SDL_GetHint(const char *name)
{
    SDLOP_Hint *hint;
    const char *value = NULL;

    if (!name || !name[0]) {
        return NULL;
    }
    pthread_mutex_lock(&sdlop_hints_lock);
    hint = sdlop_hint_for(name, false);
    if (hint && hint->value) {
        value = hint->value;
    }
    pthread_mutex_unlock(&sdlop_hints_lock);
    if (!value) {
        /* SDL3 falls back to an environment variable with the hint's own name. */
        value = getenv(name);
    }
    return value;
}

bool SDL_GetHintBoolean(const char *name, bool default_value)
{
    const char *value = SDL_GetHint(name);
    if (!value || !value[0]) {
        return default_value;
    }
    if (SDL_strcasecmp(value, "0") == 0 || SDL_strcasecmp(value, "false") == 0 ||
        SDL_strcasecmp(value, "no") == 0) {
        return false;
    }
    return true;
}

bool SDL_AddHintCallback(const char *name, SDL_HintCallback callback, void *userdata)
{
    SDLOP_HintCallback *node;
    const char *value;

    if (!name || !name[0]) {
        return SDL_InvalidParamError("name");
    }
    if (!callback) {
        return SDL_InvalidParamError("callback");
    }

    node = (SDLOP_HintCallback *)SDLOP_Calloc(1, sizeof(*node));
    if (!node) {
        return SDLOP_OutOfMemory();
    }
    node->callback = callback;
    node->userdata = userdata;

    pthread_mutex_lock(&sdlop_hints_lock);
    {
        SDLOP_Hint *hint = sdlop_hint_for(name, true);
        if (!hint) {
            pthread_mutex_unlock(&sdlop_hints_lock);
            SDLOP_Free(node);
            return false;
        }
        node->next = hint->callbacks;
        hint->callbacks = node;
    }
    pthread_mutex_unlock(&sdlop_hints_lock);

    /* SDL3 calls the callback immediately with the current value. */
    value = SDL_GetHint(name);
    callback(userdata, name, NULL, value);
    return true;
}

void SDL_RemoveHintCallback(const char *name, SDL_HintCallback callback, void *userdata)
{
    SDLOP_Hint *hint;
    SDLOP_HintCallback **link;

    if (!name || !callback) {
        return;
    }
    pthread_mutex_lock(&sdlop_hints_lock);
    hint = sdlop_hint_for(name, false);
    if (hint) {
        link = &hint->callbacks;
        while (*link) {
            if ((*link)->callback == callback && (*link)->userdata == userdata) {
                SDLOP_HintCallback *dead = *link;
                *link = dead->next;
                SDLOP_Free(dead);
                break;
            }
            link = &(*link)->next;
        }
    }
    pthread_mutex_unlock(&sdlop_hints_lock);
}
