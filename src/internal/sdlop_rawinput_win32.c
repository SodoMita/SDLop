/*
  SDLop - raw input (evdev) stubs for the Windows build.

  SDLop's low-latency raw input reads /dev/input/event* directly and owns the
  console VT, which is a Linux feature. Windows delivers keyboard and mouse
  input through the window message queue instead (see src/video/windows/),
  so the SDLop_RawInput* hooks and the SDLop_* raw-event API report
  "unavailable" here and every high-level entry point keeps working.

  Same role as SDL_keyboard_web.c has for the Emscripten build.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#ifdef _WIN32

#include <SDL3/SDLop.h>

bool SDLOP_RawInputInit(void)
{
    return SDL_SetError("raw input is not available on Windows");
}

void SDLOP_RawInputQuit(void)
{
}

void SDLOP_RawInputPump(void)
{
}

bool SDLOP_RawInputIsSource(void)
{
    return false;
}

bool SDLOP_RawInputHasKeyboard(void)
{
    return false;
}

void SDLOP_RawInputAttachConsole(int console_fd)
{
    (void)console_fd;
}

void SDLOP_RawInputDetachConsole(void)
{
}

void SDLOP_RawInputSetRepeatInfo(Uint32 delay_ms, Uint32 interval_ms)
{
    (void)delay_ms;
    (void)interval_ms;
}

int SDLop_PollRawEvents(SDLop_RawEvent *events, int max_events)
{
    (void)events;
    (void)max_events;
    return 0;
}

bool SDLop_RegisterRawEventCallback(SDLop_RawEventCallback cb, void *userdata)
{
    (void)cb;
    (void)userdata;
    SDL_SetError("Unsupported");
    return false;
}

void SDLop_UnregisterRawEventCallback(SDLop_RawEventCallback cb)
{
    (void)cb;
}

bool SDLop_RawInputAvailable(void)
{
    return false;
}

Uint64 SDLop_GetRawEventCount(void)
{
    return 0;
}

#endif /* _WIN32 */
