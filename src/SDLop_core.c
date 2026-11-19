/*
  SDLop - library init/quit and subsystem wiring.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#ifndef __EMSCRIPTEN__
#include <sys/eventfd.h>
#endif
#include <time.h>
#include <unistd.h>

SDLop_Globals sdlop;
bool sdlop_quitting;

/* ------------------------------------------------------------------ */
/* Main thread (SDL3: the thread that called SDL_Init first)           */
/* ------------------------------------------------------------------ */

static pthread_t main_thread;
static _Atomic bool main_thread_known;

bool SDLOP_IsMainThread(void)
{
    if (!main_thread_known) {
        return true; /* SDL3: before SDL_Init every thread may act as main */
    }
    return pthread_equal(pthread_self(), main_thread) != 0;
}

/* SDL_SetMainReady() / first SDL_Init(): this thread is the main thread */
void SDLOP_InitMainThread(void)
{
    if (!main_thread_known) {
        main_thread = pthread_self();
        main_thread_known = true;
    }
}

bool SDL_IsMainThread(void)
{
    return SDLOP_IsMainThread();
}

/* ------------------------------------------------------------------ */
/* Version / platform                                                  */
/* ------------------------------------------------------------------ */

int SDL_GetVersion(void)
{
    /* the SDL API level SDLop implements (the headers' SDL_VERSION) */
    return SDL_VERSION;
}

const char *SDL_GetRevision(void)
{
#define SDLOP_STR2(x) #x
#define SDLOP_STR(x) SDLOP_STR2(x)
    return "SDLop-" SDLOP_STR(SDLOP_MAJOR_VERSION) "." SDLOP_STR(SDLOP_MINOR_VERSION) "." SDLOP_STR(SDLOP_PATCHLEVEL)
           " (SDL " SDLOP_STR(SDL_MAJOR_VERSION) "." SDLOP_STR(SDL_MINOR_VERSION) "." SDLOP_STR(SDL_MICRO_VERSION) " API)";
#undef SDLOP_STR
#undef SDLOP_STR2
}

const char *SDL_GetPlatform(void)
{
#ifdef __EMSCRIPTEN__
    return "Emscripten";
#else
    return "Linux";
#endif
}

/* ------------------------------------------------------------------ */
/* App metadata (stored in the global properties, like SDL3)           */
/* ------------------------------------------------------------------ */

static bool valid_metadata_property(const char *name)
{
    if (!name || !*name) {
        return false;
    }
    return strcmp(name, SDL_PROP_APP_METADATA_NAME_STRING) == 0 ||
           strcmp(name, SDL_PROP_APP_METADATA_VERSION_STRING) == 0 ||
           strcmp(name, SDL_PROP_APP_METADATA_IDENTIFIER_STRING) == 0 ||
           strcmp(name, SDL_PROP_APP_METADATA_CREATOR_STRING) == 0 ||
           strcmp(name, SDL_PROP_APP_METADATA_COPYRIGHT_STRING) == 0 ||
           strcmp(name, SDL_PROP_APP_METADATA_URL_STRING) == 0 ||
           strcmp(name, SDL_PROP_APP_METADATA_TYPE_STRING) == 0;
}

bool SDL_SetAppMetadataProperty(const char *name, const char *value)
{
    if (!valid_metadata_property(name)) {
        return SDL_InvalidParamError("name");
    }
    return SDL_SetStringProperty(SDL_GetGlobalProperties(), name, value);
}

bool SDL_SetAppMetadata(const char *appname, const char *appversion, const char *appidentifier)
{
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, appname);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING, appversion);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING, appidentifier);
    return true;
}

const char *SDLOP_GetExeName(void)
{
    static char name[256];
    if (!name[0]) {
#ifndef __EMSCRIPTEN__
        char path[1024];
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (n > 0) {
            path[n] = '\0';
            const char *base = strrchr(path, '/');
            SDL_strlcpy(name, base ? base + 1 : path, sizeof(name));
        }
#endif
    }
    return name[0] ? name : NULL;
}

/* SDL3 core/unix SDL_GetAppID(): metadata identifier, exe name, "SDL_App" */
const char *SDLOP_GetAppID(void)
{
    const char *id = SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING);
    if (!id || !*id) {
        id = SDLOP_GetExeName();
    }
    return (id && *id) ? id : "SDL_App";
}

const char *SDL_GetAppMetadataProperty(const char *name)
{
    if (!valid_metadata_property(name)) {
        SDL_InvalidParamError("name");
        return NULL;
    }
    const char *value = NULL;
    if (strcmp(name, SDL_PROP_APP_METADATA_NAME_STRING) == 0) {
        value = SDL_GetHint(SDL_HINT_APP_NAME);
    } else if (strcmp(name, SDL_PROP_APP_METADATA_IDENTIFIER_STRING) == 0) {
        value = SDL_GetHint(SDL_HINT_APP_ID);
    }
    if (!value || !*value) {
        value = SDL_GetStringProperty(SDL_GetGlobalProperties(), name, NULL);
    }
    if (!value || !*value) {
        if (strcmp(name, SDL_PROP_APP_METADATA_NAME_STRING) == 0) {
            value = SDLOP_GetExeName();
            if (!value) {
                value = "SDL Application";
            }
        } else if (strcmp(name, SDL_PROP_APP_METADATA_TYPE_STRING) == 0) {
            value = "application";
        }
    }
    return value;
}

Uint64 SDLOP_MonotonicNS(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
}

static bool subsystem_init(SDL_InitFlags flags)
{
    if (flags & SDL_INIT_VIDEO) {
        if (!SDLOP_VideoInit()) {
            return false;
        }
        /* default xkb layout; replaced by the Wayland keymap when a
         * compositor provides one */
        if (!SDLOP_KeyboardSetDefaultKeymap()) {
            SDL_ClearError(); /* keymap is optional: keycodes still resolve */
        }
        /* asyncinput-style low-latency raw input; optional */
        if (!SDLOP_RawInputInit()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "SDLop raw input unavailable: %s", SDL_GetError());
            SDL_ClearError();
        }
    }
    return true;
}

static void subsystem_quit(SDL_InitFlags flags)
{
    if (flags & SDL_INIT_VIDEO) {
        SDLOP_RawInputQuit();
        SDLOP_VideoQuit();
        SDLOP_KeyboardQuit();
        SDLOP_DevicesQuit();
    }
}

bool SDL_Init(SDL_InitFlags flags)
{
    SDLOP_InitMainThread();
    if (!sdlop.init_done) {
        sdlop.init_monotonic_ns = SDLOP_MonotonicNS();
        sdlop.next_window_id = 0;
        sdlop.next_user_event = SDL_EVENT_USER;
        sdlop.repeat_delay_ms = 500;
        sdlop.repeat_interval_ms = 33;
#ifndef __EMSCRIPTEN__ /* no fds to wake on the web */
        sdlop.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (sdlop.wake_fd < 0) {
            sdlop.wake_fd = -1;
        }
#else
        sdlop.wake_fd = -1;
#endif
        sdlop.init_done = true;
    }
    if (flags & SDL_INIT_VIDEO) {
        flags |= SDL_INIT_EVENTS;
    }
    SDL_InitFlags new_flags = flags & ~sdlop.init_flags;
    if (new_flags) {
        if (!subsystem_init(new_flags)) {
            return false;
        }
        sdlop.init_flags |= new_flags;
    }
    return true;
}

bool SDL_InitSubSystem(SDL_InitFlags flags)
{
    return SDL_Init(flags);
}

void SDL_QuitSubSystem(SDL_InitFlags flags)
{
    SDL_InitFlags active = flags & sdlop.init_flags;
    if (active) {
        subsystem_quit(active);
        sdlop.init_flags &= ~active;
    }
}

SDL_InitFlags SDL_WasInit(SDL_InitFlags flags)
{
    if (flags == 0) {
        return sdlop.init_flags;
    }
    return sdlop.init_flags & flags;
}

void SDL_Quit(void)
{
    if (!sdlop.init_done) {
        return;
    }
    sdlop_quitting = true;
    if (sdlop.init_flags) {
        subsystem_quit(sdlop.init_flags);
        sdlop.init_flags = 0;
    }
    SDLOP_QuitTimers();
    SDLOP_QuitEvents();
    SDLOP_QuitClipboard();
    if (sdlop.wake_fd >= 0) {
        close(sdlop.wake_fd);
        sdlop.wake_fd = -1;
    }
    SDLOP_EnvironmentQuit();
    SDLOP_ThreadsQuit();
    memset(&sdlop, 0, sizeof(sdlop));
    sdlop.wake_fd = -1;
    SDLOP_ResetTouchMouse();
    SDLOP_AssertionsQuit(); /* report uses the log */
    SDLOP_QuitLog();
    SDLOP_QuitHints();
    SDLOP_QuitProperties();
    main_thread_known = false;
    sdlop_quitting = false;
}
