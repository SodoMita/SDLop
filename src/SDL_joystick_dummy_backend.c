/*
  SDLop - no-device joystick backend (web / non-Linux builds).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_joystick_internal.h"

bool SDLOP_InitJoysticks(void)
{
    return true;
}

void SDLOP_QuitJoysticks(void)
{
}

bool SDLOP_JoysticksInitialized(void)
{
    return true;
}

void SDLOP_JoystickPump(void)
{
}

int SDLOP_JoystickGetEventFDs(int *fds, int max_fds)
{
    (void)fds, (void)max_fds;
    return 0;
}
