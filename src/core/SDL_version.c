/*
  SDLop -- SDL_version.h.

  SDL_GetVersion() reports the SDL3 version whose API this build tracks, because
  that is what ported code uses to gate feature detection. SDL_GetSDLopVersion()
  reports the actual library version.
*/

#include "../sdlop_internal.h"

int SDL_GetVersion(void)
{
    return SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
}

void SDL_GetSDLopVersion(SDL_SDLopVersion *ver)
{
    if (ver) {
        ver->major = SDL_SDLOP_MAJOR_VERSION;
        ver->minor = SDL_SDLOP_MINOR_VERSION;
        ver->micro = SDL_SDLOP_MICRO_VERSION;
    }
}
