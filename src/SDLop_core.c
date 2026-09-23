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
    }
}

bool SDL_Init(SDL_InitFlags flags)
{
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
        flags |= SDL_INIT_EVENTS | SDL_INIT_TIMER;
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
    if (sdlop.wake_fd >= 0) {
        close(sdlop.wake_fd);
        sdlop.wake_fd = -1;
    }
    memset(&sdlop, 0, sizeof(sdlop));
    sdlop.wake_fd = -1;
    sdlop_quitting = false;
}
