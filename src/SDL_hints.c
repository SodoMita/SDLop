/*
  SDLop - hints (SDL_hints.h).

  Same rules as SDL3's src/SDL_hints.c:
    - an environment variable of the same name wins over SDL_HINT_DEFAULT /
      SDL_HINT_NORMAL hints; only SDL_HINT_OVERRIDE beats it
    - a hint can't be replaced by a lower priority value
    - callbacks fire on add (with the current value) and on every change
    - SDL_Quit() drops all hints and callbacks

  Returned strings stay valid until SDL_Quit() / SDL_ResetHints(): replaced
  values are retired instead of freed, so a pointer obtained from
  SDL_GetHint() on one thread can't dangle while another thread sets the
  hint (SDL3 achieves the same with per-thread persistent strings).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include <strings.h>

typedef struct HintWatch
{
    SDL_HintCallback callback;
    void *userdata;
    struct HintWatch *next;
} HintWatch;

typedef struct Hint
{
    char *name;
    char *value;           /* NULL = unset (only callbacks registered) */
    SDL_HintPriority priority;
    HintWatch *callbacks;
    struct Hint *next;
} Hint;

typedef struct Retired
{
    char *value;
    struct Retired *next;
} Retired;


/* SDL3: fall back to old (SDL2) names of environment variables that are
 * important to users (e.g. SDL_VIDEODRIVER=wayland) */
static const char *hint_env(const char *name)
{
    const char *result = SDL_getenv(name);
    if (!result && name && *name) {
        if (SDL_strcmp(name, SDL_HINT_VIDEO_DRIVER) == 0) {
            result = SDL_getenv("SDL_VIDEODRIVER");
        } else if (SDL_strcmp(name, SDL_HINT_AUDIO_DRIVER) == 0) {
            result = SDL_getenv("SDL_AUDIODRIVER");
        }
    }
    return result;
}

static SDLOP_STATIC_MUTEX(hint_lock);
static Hint *hints;
static Retired *retired;

static Hint *find_hint(const char *name)
{
    for (Hint *h = hints; h; h = h->next) {
        if (strcmp(h->name, name) == 0) {
            return h;
        }
    }
    return NULL;
}

static Hint *get_or_create(const char *name)
{
    Hint *h = find_hint(name);
    if (h) {
        return h;
    }
    h = (Hint *)SDL_calloc(1, sizeof(*h));
    if (!h) {
        return NULL;
    }
    h->name = SDL_strdup(name);
    if (!h->name) {
        SDL_free(h);
        return NULL;
    }
    h->priority = SDL_HINT_DEFAULT;
    h->next = hints;
    hints = h;
    return h;
}

static void retire(char *value)
{
    if (!value) {
        return;
    }
    Retired *r = (Retired *)malloc(sizeof(*r));
    if (!r) {
        return; /* leak rather than risk a dangling pointer */
    }
    r->value = value;
    r->next = retired;
    retired = r;
}

static bool values_differ(const char *a, const char *b)
{
    if (!a || !b) {
        return a != b;
    }
    return strcmp(a, b) != 0;
}

/* Snapshot the callbacks so they run without holding the lock (a callback
 * may set or query hints itself). */
typedef struct
{
    int count;
    HintWatch *items;
} WatchList;

static WatchList snapshot_callbacks(Hint *h)
{
    WatchList wl = { 0, NULL };
    for (HintWatch *w = h->callbacks; w; w = w->next) {
        wl.count++;
    }
    if (wl.count) {
        wl.items = (HintWatch *)malloc(sizeof(HintWatch) * (size_t)wl.count);
        if (!wl.items) {
            wl.count = 0;
            return wl;
        }
        int i = 0;
        for (HintWatch *w = h->callbacks; w; w = w->next) {
            wl.items[i++] = *w;
        }
    }
    return wl;
}

static void run_callbacks(WatchList *wl, const char *name, const char *old_value, const char *new_value)
{
    for (int i = 0; i < wl->count; i++) {
        wl->items[i].callback(wl->items[i].userdata, name, old_value, new_value);
    }
    free(wl->items);
    wl->items = NULL;
    wl->count = 0;
}

bool SDL_SetHintWithPriority(const char *name, const char *value, SDL_HintPriority priority)
{
    if (!name || !*name) {
        return SDL_InvalidParamError("name");
    }
    const char *env = hint_env(name);
    if (env && priority < SDL_HINT_OVERRIDE) {
        return SDL_SetError("An environment variable is taking priority");
    }

    SDLOP_MUTEX_LOCK(SDLOP_SM(&hint_lock));
    Hint *h = get_or_create(name);
    if (!h) {
        SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));
        return false;
    }
    if (h->value && priority < h->priority) {
        SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));
        return false;
    }
    const char *old_effective = (env && h->priority != SDL_HINT_OVERRIDE) ? env : h->value;
    char *old_value = h->value;
    char *new_value = value ? SDL_strdup(value) : NULL;
    if (value && !new_value) {
        SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));
        return false;
    }
    h->value = new_value;
    h->priority = priority;
    const char *new_effective = (env && priority != SDL_HINT_OVERRIDE) ? env : new_value;
    WatchList wl = { 0, NULL };
    if (values_differ(old_effective, new_effective)) {
        wl = snapshot_callbacks(h);
    }
    retire(old_value); /* old_effective may point at it until the callbacks ran */
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));

    run_callbacks(&wl, name, old_effective, new_effective);
    return true;
}

bool SDL_SetHint(const char *name, const char *value)
{
    return SDL_SetHintWithPriority(name, value, SDL_HINT_NORMAL);
}

bool SDL_ResetHint(const char *name)
{
    if (!name || !*name) {
        return SDL_InvalidParamError("name");
    }
    const char *env = hint_env(name);
    SDLOP_MUTEX_LOCK(SDLOP_SM(&hint_lock));
    Hint *h = find_hint(name);
    if (!h) {
        SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));
        return false;
    }
    const char *old_effective = (env && h->priority != SDL_HINT_OVERRIDE) ? env : h->value;
    WatchList wl = { 0, NULL };
    if (values_differ(old_effective, env)) {
        wl = snapshot_callbacks(h);
    }
    retire(h->value);
    h->value = NULL;
    h->priority = SDL_HINT_DEFAULT;
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));

    run_callbacks(&wl, name, old_effective, env);
    return true;
}

void SDL_ResetHints(void)
{
    /* collect names first: callbacks may modify the list */
    SDLOP_MUTEX_LOCK(SDLOP_SM(&hint_lock));
    int n = 0;
    for (Hint *h = hints; h; h = h->next) {
        n++;
    }
    char **names = n ? (char **)malloc(sizeof(char *) * (size_t)n) : NULL;
    int count = 0;
    if (names) {
        for (Hint *h = hints; h; h = h->next) {
            if (h->value) {
                names[count] = strdup(h->name);
                if (names[count]) {
                    count++;
                }
            }
        }
    }
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));
    for (int i = 0; i < count; i++) {
        SDL_ResetHint(names[i]);
        free(names[i]);
    }
    free(names);
}

const char *SDL_GetHint(const char *name)
{
    if (!name) {
        return NULL;
    }
    const char *result = hint_env(name);
    SDLOP_MUTEX_LOCK(SDLOP_SM(&hint_lock));
    Hint *h = find_hint(name);
    if (h && h->value && (!result || h->priority == SDL_HINT_OVERRIDE)) {
        result = h->value;
    }
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));
    return result;
}

bool SDLOP_GetStringBoolean(const char *value, bool default_value)
{
    if (!value || !*value) {
        return default_value;
    }
    if (*value == '0' || strcasecmp(value, "false") == 0) {
        return false;
    }
    return true;
}

bool SDLOP_HintFlag(const char *name, bool default_value)
{
    const char *value = SDL_GetHint(name);
    if (!value || !*value) {
        return default_value;
    }
    if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "off") == 0 || strcasecmp(value, "no") == 0) {
        return false;
    }
    return true;
}

const char *SDLOP_HintFirst(const char *const *names)
{
    for (; names && *names; names++) {
        const char *value = SDL_GetHint(*names);
        if (value && *value) {
            return value;
        }
    }
    return NULL;
}

bool SDL_GetHintBoolean(const char *name, bool default_value)
{
    return SDLOP_GetStringBoolean(SDL_GetHint(name), default_value);
}

bool SDL_AddHintCallback(const char *name, SDL_HintCallback callback, void *userdata)
{
    if (!name || !*name) {
        return SDL_InvalidParamError("name");
    }
    if (!callback) {
        return SDL_InvalidParamError("callback");
    }
    SDL_RemoveHintCallback(name, callback, userdata);

    HintWatch *w = (HintWatch *)SDL_malloc(sizeof(*w));
    if (!w) {
        return false;
    }
    w->callback = callback;
    w->userdata = userdata;

    SDLOP_MUTEX_LOCK(SDLOP_SM(&hint_lock));
    Hint *h = get_or_create(name);
    if (!h) {
        SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));
        SDL_free(w);
        return false;
    }
    w->next = h->callbacks;
    h->callbacks = w;
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));

    /* SDL3: the callback is invoked right away with the current value */
    const char *value = SDL_GetHint(name);
    callback(userdata, name, value, value);
    return true;
}

void SDL_RemoveHintCallback(const char *name, SDL_HintCallback callback, void *userdata)
{
    if (!name || !*name) {
        return;
    }
    SDLOP_MUTEX_LOCK(SDLOP_SM(&hint_lock));
    Hint *h = find_hint(name);
    if (h) {
        for (HintWatch **pw = &h->callbacks; *pw; pw = &(*pw)->next) {
            HintWatch *w = *pw;
            if (w->callback == callback && w->userdata == userdata) {
                *pw = w->next;
                SDL_free(w);
                break;
            }
        }
    }
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));
}

void SDLOP_QuitHints(void)
{
    SDLOP_MUTEX_LOCK(SDLOP_SM(&hint_lock));
    while (hints) {
        Hint *h = hints;
        hints = h->next;
        while (h->callbacks) {
            HintWatch *w = h->callbacks;
            h->callbacks = w->next;
            SDL_free(w);
        }
        SDL_free(h->name);
        SDL_free(h->value);
        SDL_free(h);
    }
    while (retired) {
        Retired *r = retired;
        retired = r->next;
        SDL_free(r->value);
        free(r);
    }
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&hint_lock));
}
