/*
  Helper for Win32 tests that depend on an interactive desktop.

  An SSH/Service session has a non-visible "Service-0x..." window station.
  SendInput cannot deliver mouse/keyboard input to a window there, so tests
  that inject input must skip rather than report a library failure. Wine's
  Xvfb-backed window station is visible, so CI keeps running them.
*/
#ifndef SDLOP_WIN32_TESTENV_H
#define SDLOP_WIN32_TESTENV_H

#include <stdbool.h>
#include <windows.h>

/* USEROBJECTFLAGS has dwFlags on MSVC and Wine as well as MinGW. */
#define SDLOP_USEROBJECT_FLAGS(f) ((f).dwFlags)

static bool win32_test_interactive_desktop(void)
{
    HWINSTA station = GetProcessWindowStation();
    USEROBJECTFLAGS flags;
    DWORD needed = 0;
    if (!station || !GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &needed)) {
        return false;
    }
    return (SDLOP_USEROBJECT_FLAGS(flags) & WSF_VISIBLE) != 0;
}

#endif /* SDLOP_WIN32_TESTENV_H */
