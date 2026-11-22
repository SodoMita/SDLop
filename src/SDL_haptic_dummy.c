/*
  SDLop - dummy haptic backend (SDL3 3.4.20 src/haptic/dummy/SDL_syshaptic.c).

  SDLop exposes no haptic devices, like its joystick backend. Kept close to
  upstream so the core can later select a real backend with the same SDL_SYS_*
  hooks. (C) Sam Lantinga, zlib license.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "internal/sdlop_internal.h"

/* SDLop always uses SDL3's dummy haptic backend (no devices). */
#define SDL_HAPTIC_DUMMY 1

#if defined(SDL_HAPTIC_DUMMY) || defined(SDL_HAPTIC_DISABLED)

#include "internal/sdlop_haptic_internal.h"

static bool SDL_SYS_LogicError(void)
{
    return SDL_SetError("Logic error: No haptic devices available.");
}

bool SDL_SYS_HapticInit(void)
{
    return true;
}

int SDL_SYS_NumHaptics(void)
{
    return 0;
}

SDL_HapticID SDL_SYS_HapticInstanceID(int index)
{
    (void)index;
    SDL_SYS_LogicError();
    return 0;
}

const char *SDL_SYS_HapticName(int index)
{
    (void)index;
    SDL_SYS_LogicError();
    return NULL;
}

bool SDL_SYS_HapticOpen(SDL_Haptic *haptic)
{
    (void)haptic;
    return SDL_SYS_LogicError();
}

int SDL_SYS_HapticMouse(void)
{
    return -1;
}

bool SDL_SYS_JoystickIsHaptic(SDL_Joystick *joystick)
{
    (void)joystick;
    return false;
}

bool SDL_SYS_HapticOpenFromJoystick(SDL_Haptic *haptic, SDL_Joystick *joystick)
{
    (void)haptic;
    (void)joystick;
    return SDL_SYS_LogicError();
}

bool SDL_SYS_JoystickSameHaptic(SDL_Haptic *haptic, SDL_Joystick *joystick)
{
    (void)haptic;
    (void)joystick;
    return false;
}

void SDL_SYS_HapticClose(SDL_Haptic *haptic)
{
    (void)haptic;
    return;
}

void SDL_SYS_HapticQuit(void)
{
    return;
}

bool SDL_SYS_HapticNewEffect(SDL_Haptic *haptic,
                            struct haptic_effect *effect, const SDL_HapticEffect *base)
{
    (void)haptic;
    (void)effect;
    (void)base;
    return SDL_SYS_LogicError();
}

bool SDL_SYS_HapticUpdateEffect(SDL_Haptic *haptic,
                               struct haptic_effect *effect,
                               const SDL_HapticEffect *data)
{
    (void)haptic;
    (void)effect;
    (void)data;
    return SDL_SYS_LogicError();
}

bool SDL_SYS_HapticRunEffect(SDL_Haptic *haptic, struct haptic_effect *effect,
                            Uint32 iterations)
{
    (void)haptic;
    (void)effect;
    (void)iterations;
    return SDL_SYS_LogicError();
}

bool SDL_SYS_HapticStopEffect(SDL_Haptic *haptic, struct haptic_effect *effect)
{
    (void)haptic;
    (void)effect;
    return SDL_SYS_LogicError();
}

void SDL_SYS_HapticDestroyEffect(SDL_Haptic *haptic, struct haptic_effect *effect)
{
    (void)haptic;
    (void)effect;
    SDL_SYS_LogicError();
    return;
}

int SDL_SYS_HapticGetEffectStatus(SDL_Haptic *haptic,
                                  struct haptic_effect *effect)
{
    (void)haptic;
    (void)effect;
    SDL_SYS_LogicError();
    return -1;
}

bool SDL_SYS_HapticSetGain(SDL_Haptic *haptic, int gain)
{
    (void)haptic;
    (void)gain;
    return SDL_SYS_LogicError();
}

bool SDL_SYS_HapticSetAutocenter(SDL_Haptic *haptic, int autocenter)
{
    (void)haptic;
    (void)autocenter;
    return SDL_SYS_LogicError();
}

bool SDL_SYS_HapticPause(SDL_Haptic *haptic)
{
    (void)haptic;
    return SDL_SYS_LogicError();
}

bool SDL_SYS_HapticResume(SDL_Haptic *haptic)
{
    (void)haptic;
    return SDL_SYS_LogicError();
}

bool SDL_SYS_HapticStopAll(SDL_Haptic *haptic)
{
    (void)haptic;
    return SDL_SYS_LogicError();
}

#endif // SDL_HAPTIC_DUMMY || SDL_HAPTIC_DISABLED
