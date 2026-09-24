/*
  SDLop -- SDL_init.h: subsystems, app metadata and the main-thread callback
  queue (SDL_RunOnMainThread).

  SDLop implements SDL_INIT_VIDEO (which implies SDL_INIT_EVENTS). The other
  SDL_INIT_* flags are accepted, reported by SDL_WasInit() and allocate nothing -
  there is no audio, joystick, haptic, sensor or camera code to initialise.
*/

#include "../sdlop_internal.h"

#include <pthread.h>
#include <string.h>

static SDL_InitFlags sdlop_initialized;
static pthread_t sdlop_main_thread;
static bool sdlop_main_thread_known;
static pthread_mutex_t sdlop_init_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------------- */
/* Main-thread callback queue                                                */
/* ------------------------------------------------------------------------- */

typedef struct SDLOP_MainThreadItem
{
    SDL_MainThreadCallback callback;
    void *userdata;
    bool *done;                    /* points at the waiter's flag, or NULL */
} SDLOP_MainThreadItem;

static pthread_mutex_t sdlop_mt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sdlop_mt_cond = PTHREAD_COND_INITIALIZER;
static SDLOP_MainThreadItem **sdlop_mt_queue;
static int sdlop_mt_count;
/* Atomic mirror of sdlop_mt_count, maintained so that SDL_PumpEvents() - which
   every frame calls, usually with nothing queued - does not have to take the
   lock just to find the queue empty. */
static int sdlop_mt_pending;
static int sdlop_mt_capacity;

void SDLOP_InitMainThreadQueue(void)
{
    sdlop_mt_count = 0;
    __atomic_store_n(&sdlop_mt_pending, 0, __ATOMIC_RELEASE);
    sdlop_mt_capacity = 0;
    sdlop_mt_queue = NULL;
}

void SDLOP_QuitMainThreadQueue(void)
{
    pthread_mutex_lock(&sdlop_mt_lock);
    /* Drop anything that was queued but never ran: those items were allocated by
       callers that are not waiting for them. */
    while (sdlop_mt_count > 0) {
        SDLOP_MainThreadItem *item = sdlop_mt_queue[--sdlop_mt_count];
        __atomic_store_n(&sdlop_mt_pending, sdlop_mt_count, __ATOMIC_RELEASE);
        if (item->done) {
            *item->done = true;
            pthread_cond_broadcast(&sdlop_mt_cond);
        } else {
            SDLOP_Free(item);
        }
    }
    pthread_mutex_unlock(&sdlop_mt_lock);
}

bool SDL_IsMainThread(void)
{
    return sdlop_main_thread_known && pthread_equal(pthread_self(), sdlop_main_thread);
}

bool SDL_RunOnMainThread(SDL_MainThreadCallback callback, void *userdata, bool wait_complete)
{
    SDLOP_MainThreadItem *item;
    bool done = false;

    if (!callback) {
        return SDL_InvalidParamError("callback");
    }
    if (SDL_IsMainThread()) {
        callback(userdata);
        return true;
    }
    if (!wait_complete) {
        item = (SDLOP_MainThreadItem *)SDLOP_Calloc(1, sizeof(*item));
        if (!item) {
            return SDLOP_OutOfMemory();
        }
    } else {
        item = (SDLOP_MainThreadItem *)alloca(sizeof(*item));
        memset(item, 0, sizeof(*item));
        item->done = &done;
    }
    item->callback = callback;
    item->userdata = userdata;

    pthread_mutex_lock(&sdlop_mt_lock);
    if (sdlop_mt_count == sdlop_mt_capacity) {
        int newcap = sdlop_mt_capacity ? sdlop_mt_capacity * 2 : 8;
        SDLOP_MainThreadItem **newqueue =
            (SDLOP_MainThreadItem **)SDLOP_Realloc(sdlop_mt_queue, (size_t)newcap * sizeof(*newqueue));
        if (!newqueue) {
            pthread_mutex_unlock(&sdlop_mt_lock);
            if (!wait_complete) {
                SDLOP_Free(item);
            }
            return SDLOP_OutOfMemory();
        }
        sdlop_mt_queue = newqueue;
        sdlop_mt_capacity = newcap;
    }
    sdlop_mt_queue[sdlop_mt_count++] = item;
    __atomic_store_n(&sdlop_mt_pending, sdlop_mt_count, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&sdlop_mt_lock);

    if (wait_complete) {
        pthread_mutex_lock(&sdlop_mt_lock);
        while (!done) {
            pthread_cond_wait(&sdlop_mt_cond, &sdlop_mt_lock);
        }
        pthread_mutex_unlock(&sdlop_mt_lock);
    }
    return true;
}

void SDLOP_RunMainThreadCallbacks(void)
{
    SDLOP_MainThreadItem *item = NULL;

    if (__atomic_load_n(&sdlop_mt_pending, __ATOMIC_ACQUIRE) == 0) {
        return;                        /* nothing queued: the common case */
    }
    for (;;) {
        pthread_mutex_lock(&sdlop_mt_lock);
        if (sdlop_mt_count > 0) {
            item = sdlop_mt_queue[0];
            memmove(&sdlop_mt_queue[0], &sdlop_mt_queue[1],
                    (size_t)(sdlop_mt_count - 1) * sizeof(sdlop_mt_queue[0]));
            sdlop_mt_count--;
            __atomic_store_n(&sdlop_mt_pending, sdlop_mt_count, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&sdlop_mt_lock);
        if (!item) {
            return;
        }
        item->callback(item->userdata);
        if (item->done) {
            pthread_mutex_lock(&sdlop_mt_lock);
            *item->done = true;
            pthread_cond_broadcast(&sdlop_mt_cond);
            pthread_mutex_unlock(&sdlop_mt_lock);
        } else {
            SDLOP_Free(item);
        }
        item = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* Subsystems                                                                */
/* ------------------------------------------------------------------------- */

bool SDL_InitSubSystem(SDL_InitFlags flags)
{
    pthread_mutex_lock(&sdlop_init_lock);
    if (!sdlop_main_thread_known) {
        sdlop_main_thread = pthread_self();
        sdlop_main_thread_known = true;
    }

    flags &= ~sdlop_initialized;
    if (flags == 0) {
        pthread_mutex_unlock(&sdlop_init_lock);
        return true;
    }
    /* Video cannot work without the event machinery. */
    if (flags & SDL_INIT_VIDEO) {
        flags |= SDL_INIT_EVENTS;
    }

    if (flags & SDL_INIT_EVENTS) {
        SDLOP_InitEvents();
        SDLOP_InitMainThreadQueue();
        SDLOP_InitTimers();
    }
    if (flags & SDL_INIT_VIDEO) {
        if (!SDLOP_VideoInit(NULL)) {
            pthread_mutex_unlock(&sdlop_init_lock);
            return false;
        }
        SDLOP_InitAsyncInput();
    }
    sdlop_initialized |= flags;
    pthread_mutex_unlock(&sdlop_init_lock);
    return true;
}

bool SDL_Init(SDL_InitFlags flags)
{
    return SDL_InitSubSystem(flags);
}

void SDL_QuitSubSystem(SDL_InitFlags flags)
{
    pthread_mutex_lock(&sdlop_init_lock);
    flags &= sdlop_initialized;
    if (flags == 0) {
        pthread_mutex_unlock(&sdlop_init_lock);
        return;
    }
    if (flags & SDL_INIT_VIDEO) {
        SDLOP_QuitAsyncInput();
        SDLOP_VideoQuit();
        SDLOP_VulkanCleanup();
    }
    if (flags & SDL_INIT_EVENTS) {
        SDLOP_QuitTimers();
        SDLOP_QuitMainThreadQueue();
        SDLOP_QuitEvents();
    }
    sdlop_initialized &= ~flags;
    pthread_mutex_unlock(&sdlop_init_lock);
}

SDL_InitFlags SDL_WasInit(SDL_InitFlags flags)
{
    SDL_InitFlags initialized;
    pthread_mutex_lock(&sdlop_init_lock);
    initialized = sdlop_initialized;
    pthread_mutex_unlock(&sdlop_init_lock);
    if (flags == 0) {
        return initialized;
    }
    return initialized & flags;
}

void SDL_Quit(void)
{
    SDL_QuitSubSystem(SDL_WasInit(0));
}

/* ------------------------------------------------------------------------- */
/* App metadata (stored in the global property bag, like SDL3)                */
/* ------------------------------------------------------------------------- */

bool SDL_SetAppMetadataProperty(const char *name, const char *value)
{
    if (!name || !name[0]) {
        return SDL_InvalidParamError("name");
    }
    return SDL_SetStringProperty(SDL_GetGlobalProperties(), name, value);
}

const char *SDL_GetAppMetadataProperty(const char *name)
{
    if (!name || !name[0]) {
        return NULL;
    }
    return SDL_GetStringProperty(SDL_GetGlobalProperties(), name, NULL);
}

bool SDL_SetAppMetadata(const char *appname, const char *appversion, const char *appidentifier)
{
    if (appname) {
        if (!SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, appname)) {
            return false;
        }
    }
    if (appversion) {
        if (!SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING, appversion)) {
            return false;
        }
    }
    if (appidentifier) {
        if (!SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING, appidentifier)) {
            return false;
        }
    }
    return true;
}
