/*
  SDLop - internal haptic backend interface.

  Adapted from SDL 3.4.20 src/haptic/SDL_syshaptic.h (C) Sam Lantinga,
  zlib license. SDLop uses SDL3's dummy backend: no haptic devices are
  exposed, but the core keeps SDL3's object/effect/rumble semantics so a
  real backend can be added later without changing the public API.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#ifndef sdloP_haptic_internal_h_
#define sdloP_haptic_internal_h_

#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_joystick.h>

#define SDLOP_HAPTIC_MAGIC 0x53444C4Fu /* "SDLO" */

struct haptic_effect
{
    SDL_HapticEffect effect;
    struct haptic_hweffect *hweffect;
};

/* The real SDL_Haptic struct, from SDL 3.4.20. */
struct SDL_Haptic
{
    SDL_HapticID instance_id;
    char *name;

    struct haptic_effect *effects;
    int neffects;
    int nplaying;
    Uint32 supported;
    int naxes;

    struct haptic_hwdata *hwdata;
    int ref_count;

    SDL_HapticEffectID rumble_id;
    SDL_HapticEffect rumble_effect;
    struct SDL_Haptic *next;

    Uint32 magic; /* SDLop object validation (SDL3 uses a global registry) */
};

/* Core lifecycle, called from SDLop_core.c for SDL_INIT_HAPTIC. */
extern bool SDLOP_InitHaptics(void);
extern void SDLOP_QuitHaptics(void);

/* Object validation, equivalent to SDL3's SDL_ObjectValid()/SetObjectValid(). */
extern bool SDLOP_HapticValid(SDL_Haptic *haptic);
extern void SDLOP_SetHapticValid(SDL_Haptic *haptic, bool valid);

/* Keep backend hooks internal: the shared-library version script exports
   every SDL_* symbol, so the SDL3 SDL_SYS_* names are mapped to SDLOP_SYS_*. */
#define SDL_SYS_HapticInit SDLOP_SYS_HapticInit
#define SDL_SYS_NumHaptics SDLOP_SYS_NumHaptics
#define SDL_SYS_HapticInstanceID SDLOP_SYS_HapticInstanceID
#define SDL_SYS_HapticName SDLOP_SYS_HapticName
#define SDL_SYS_HapticOpen SDLOP_SYS_HapticOpen
#define SDL_SYS_HapticMouse SDLOP_SYS_HapticMouse
#define SDL_SYS_JoystickIsHaptic SDLOP_SYS_JoystickIsHaptic
#define SDL_SYS_HapticOpenFromJoystick SDLOP_SYS_HapticOpenFromJoystick
#define SDL_SYS_JoystickSameHaptic SDLOP_SYS_JoystickSameHaptic
#define SDL_SYS_HapticClose SDLOP_SYS_HapticClose
#define SDL_SYS_HapticQuit SDLOP_SYS_HapticQuit
#define SDL_SYS_HapticNewEffect SDLOP_SYS_HapticNewEffect
#define SDL_SYS_HapticUpdateEffect SDLOP_SYS_HapticUpdateEffect
#define SDL_SYS_HapticRunEffect SDLOP_SYS_HapticRunEffect
#define SDL_SYS_HapticStopEffect SDLOP_SYS_HapticStopEffect
#define SDL_SYS_HapticDestroyEffect SDLOP_SYS_HapticDestroyEffect
#define SDL_SYS_HapticGetEffectStatus SDLOP_SYS_HapticGetEffectStatus
#define SDL_SYS_HapticSetGain SDLOP_SYS_HapticSetGain
#define SDL_SYS_HapticSetAutocenter SDLOP_SYS_HapticSetAutocenter
#define SDL_SYS_HapticPause SDLOP_SYS_HapticPause
#define SDL_SYS_HapticResume SDLOP_SYS_HapticResume
#define SDL_SYS_HapticStopAll SDLOP_SYS_HapticStopAll

/* Backend interface (SDL3's SDL_SYS_* hooks, dummy implementation). */
extern bool SDLOP_SYS_HapticInit(void);
extern int SDLOP_SYS_NumHaptics(void);
extern SDL_HapticID SDLOP_SYS_HapticInstanceID(int index);
extern const char *SDLOP_SYS_HapticName(int index);
extern bool SDLOP_SYS_HapticOpen(SDL_Haptic *haptic);
extern int SDLOP_SYS_HapticMouse(void);
extern bool SDLOP_SYS_JoystickIsHaptic(SDL_Joystick *joystick);
extern bool SDLOP_SYS_HapticOpenFromJoystick(SDL_Haptic *haptic, SDL_Joystick *joystick);
extern bool SDLOP_SYS_JoystickSameHaptic(SDL_Haptic *haptic, SDL_Joystick *joystick);
extern void SDLOP_SYS_HapticClose(SDL_Haptic *haptic);
extern void SDLOP_SYS_HapticQuit(void);
extern bool SDLOP_SYS_HapticNewEffect(SDL_Haptic *haptic, struct haptic_effect *effect, const SDL_HapticEffect *base);
extern bool SDLOP_SYS_HapticUpdateEffect(SDL_Haptic *haptic, struct haptic_effect *effect, const SDL_HapticEffect *data);
extern bool SDLOP_SYS_HapticRunEffect(SDL_Haptic *haptic, struct haptic_effect *effect, Uint32 iterations);
extern bool SDLOP_SYS_HapticStopEffect(SDL_Haptic *haptic, struct haptic_effect *effect);
extern void SDLOP_SYS_HapticDestroyEffect(SDL_Haptic *haptic, struct haptic_effect *effect);
extern int SDLOP_SYS_HapticGetEffectStatus(SDL_Haptic *haptic, struct haptic_effect *effect);
extern bool SDLOP_SYS_HapticSetGain(SDL_Haptic *haptic, int gain);
extern bool SDLOP_SYS_HapticSetAutocenter(SDL_Haptic *haptic, int autocenter);
extern bool SDLOP_SYS_HapticPause(SDL_Haptic *haptic);
extern bool SDLOP_SYS_HapticResume(SDL_Haptic *haptic);
extern bool SDLOP_SYS_HapticStopAll(SDL_Haptic *haptic);

#endif /* sdloP_haptic_internal_h_ */
