/*
  SDLop - library init/quit and subsystem wiring.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "internal/sdlop_joystick_internal.h"
#include "internal/sdlop_portable.h"
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

SDLop_Globals sdlop = {
    .wake_fd = -1,
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && (!defined(SDLOP_HAVE_EVENTFD) || !SDLOP_HAVE_EVENTFD)
    .wake_write_fd = -1,
#endif
};
bool sdlop_quitting;

/* ------------------------------------------------------------------ */
/* Main thread (SDL3: the thread that called SDL_Init first)           */
/* ------------------------------------------------------------------ */

static SDL_ThreadID main_thread;
static _Atomic bool main_thread_known;

bool SDLOP_IsMainThread(void)
{
    if (!main_thread_known) {
        return true; /* SDL3: before SDL_Init every thread may act as main */
    }
    return SDL_GetCurrentThreadID() == main_thread;
}

/* SDL_SetMainReady() / first SDL_Init(): this thread is the main thread */
void SDLOP_InitMainThread(void)
{
    if (!main_thread_known) {
        main_thread = SDL_GetCurrentThreadID();
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
#if defined(SDL_PLATFORM_AIX)
    return "AIX";
#elif defined(SDL_PLATFORM_ANDROID)
    return "Android";
#elif defined(SDL_PLATFORM_BSDI)
    return "BSDI";
#elif defined(SDL_PLATFORM_EMSCRIPTEN)
    return "Emscripten";
#elif defined(SDL_PLATFORM_FREEBSD)
    return "FreeBSD";
#elif defined(SDL_PLATFORM_HAIKU)
    return "Haiku";
#elif defined(SDL_PLATFORM_LINUX)
    return "Linux";
#elif defined(SDL_PLATFORM_MACOS)
    return "macOS";
#elif defined(SDL_PLATFORM_NETBSD)
    return "NetBSD";
#elif defined(SDL_PLATFORM_OPENBSD)
    return "OpenBSD";
#elif defined(SDL_PLATFORM_QNXNTO)
    return "QNX Neutrino";
#elif defined(SDL_PLATFORM_SOLARIS)
    return "Solaris";
#elif defined(SDL_PLATFORM_WIN32)
    return "Windows";
#elif defined(SDL_PLATFORM_VISIONOS)
    return "visionOS";
#elif defined(SDL_PLATFORM_IOS)
    return "iOS";
#elif defined(SDL_PLATFORM_TVOS)
    return "tvOS";
#elif defined(SDL_PLATFORM_HURD)
    return "GNU/Hurd";
#elif defined(__DragonFly__)
    return "DragonFly";
#else
    return "Unknown (see SDL_platform.h)";
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
#if !defined(__EMSCRIPTEN__)
        char *path = SDLOP_ReadExePath();
        if (path) {
            const char *base = strrchr(path, '/');
#ifdef _WIN32
            const char *bslash = strrchr(path, '\\');
            if (bslash && (!base || bslash > base)) {
                base = bslash;
            }
#endif
            SDL_strlcpy(name, base ? base + 1 : path, sizeof(name));
            SDL_free(path);
        } else {
            SDL_ClearError();
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
    return (Uint64)SDLOP_MonotonicNowNS();
}

static bool subsystem_init(SDL_InitFlags flags)
{
    if (flags & SDL_INIT_HAPTIC) {
        if (!SDLOP_InitHaptics()) {
            return false;
        }
    }
    if (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        if (!SDLOP_InitJoysticks()) {
            return false;
        }
    }
    if (flags & SDL_INIT_VIDEO) {
#ifndef _WIN32
        if (!SDLOP_VideoInit()) {
            SDLOP_RawInputQuit(); /* a console driver may have started it */
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
#else
        /* Windows gets its keys through the window messages by default; the
         * raw input worker is opt-in (SDLOP_RAW_INPUT=1) and, when on, feeds
         * the same rings the evdev worker does. There is no xkb keymap. */
        if (!SDLOP_VideoInit()) {
            return false;
        }
        if (!SDLOP_RawInputInit()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "SDLop raw input unavailable: %s", SDL_GetError());
            SDL_ClearError();
        }
#endif
    }
    return true;
}

static void subsystem_quit(SDL_InitFlags flags)
{
    if (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        SDLOP_QuitJoysticks();
    }
    if (flags & SDL_INIT_HAPTIC) {
        SDLOP_QuitHaptics();
    }
    if (flags & SDL_INIT_VIDEO) {
        /* the evdev worker on Linux, the Raw Input worker on Windows; a no-op
         * when neither was started */
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
#if defined(__EMSCRIPTEN__) || defined(_WIN32)
        /* No fd to wake on the web; Windows waits on an auto-reset event
         * instead, so SDL_WaitEvent can block rather than poll. */
        sdlop.wake_fd = -1;
#if defined(_WIN32)
        sdlop.wake_handle = CreateEventW(NULL, FALSE, FALSE, NULL);
#endif
#else
#if defined(SDLOP_HAVE_EVENTFD) && SDLOP_HAVE_EVENTFD
        SDLOP_WakeFdCreate(&sdlop.wake_fd, NULL);
#else
        SDLOP_WakeFdCreate(&sdlop.wake_fd, &sdlop.wake_write_fd);
#endif
#endif
        sdlop.init_done = true;
    }
    /* subsystem dependencies, as in stock SDL_InitSubSystem */
    if (flags & SDL_INIT_GAMEPAD) {
        flags |= SDL_INIT_JOYSTICK;
    }
    if (flags & (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR | SDL_INIT_CAMERA)) {
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
    SDLOP_QuitFilesystem();
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#if defined(SDLOP_HAVE_EVENTFD) && SDLOP_HAVE_EVENTFD
    SDLOP_WakeFdClose(&sdlop.wake_fd, NULL);
#else
    SDLOP_WakeFdClose(&sdlop.wake_fd, &sdlop.wake_write_fd);
#endif
#endif
#if defined(_WIN32)
    if (sdlop.wake_handle) {
        CloseHandle((HANDLE)sdlop.wake_handle);
        sdlop.wake_handle = NULL;
    }
#endif
    SDLOP_EnvironmentQuit();
    SDLOP_ThreadsQuit();
    SDLOP_ReleaseTimerHandles();
    memset(&sdlop, 0, sizeof(sdlop));
    sdlop.wake_fd = -1;
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && (!defined(SDLOP_HAVE_EVENTFD) || !SDLOP_HAVE_EVENTFD)
    sdlop.wake_write_fd = -1;
#endif
    SDLOP_ResetTouchMouse();
    SDLOP_AssertionsQuit(); /* report uses the log */
    SDLOP_QuitLog();
    SDLOP_QuitHints();
    SDLOP_QuitProperties();
    main_thread_known = false;
    sdlop_quitting = false;
}
