/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  GENERATED FILE -- do not edit.  Produced by tools/sdlop.py from the upstream SDL3
  header SDL_version.h (3.2.10), which is zlib licensed, Copyright (C) 1997-2025 Sam Lantinga
  and SDL contributors.  Declarations are copied verbatim so that source and binary
  compatibility with SDL3 are exact; the items SDLop does not implement were
  removed (the full list is in tools/dropped.txt).

  In this header: SDL_GetVersion() reports the SDL3 version whose API this build tracks;
 * SDL_GetSDLopVersion() reports the SDLop version.
*/

#ifndef SDL_version_h_
#define SDL_version_h_

#include <SDL3/SDL_begin_code.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDL_MAJOR_VERSION 3

#define SDL_MINOR_VERSION 2

#define SDL_MICRO_VERSION 10

#define SDL_VERSIONNUM(major, minor, patch) \
 ((major) * 1000000 + (minor) * 1000 + (patch))

#define SDL_VERSIONNUM_MAJOR(version) ((version) / 1000000)

#define SDL_VERSIONNUM_MINOR(version) (((version) / 1000) % 1000)

#define SDL_VERSIONNUM_MICRO(version) ((version) % 1000)

#define SDL_VERSION \
 SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION)

#define SDL_VERSION_ATLEAST(X, Y, Z) \
 (SDL_VERSION >= SDL_VERSIONNUM(X, Y, Z))

extern SDL_DECLSPEC int SDLCALL SDL_GetVersion(void);

/**
 * Marker macro: SDLop builds define this, upstream SDL3 does not.
 *
 * \since This is a SDLop extension, not part of SDL3.
 */
#define SDL_SDLOP 1
#define SDL_SDLOP_MAJOR_VERSION 0
#define SDL_SDLOP_MINOR_VERSION 1
#define SDL_SDLOP_MICRO_VERSION 0

typedef struct SDL_SDLopVersion
{
    int major;
    int minor;
    int micro;
} SDL_SDLopVersion;

/**
 * Get the version of the SDLop library that is actually linked.
 *
 * \param ver a pointer filled in with the SDLop version.
 *
 * \threadsafety It is safe to call this function from any thread.
 *
 * \since This is a SDLop extension, not part of SDL3.
 */
extern SDL_DECLSPEC void SDLCALL SDL_GetSDLopVersion(SDL_SDLopVersion *ver);

#include <SDL3/SDL_close_code.h>

#ifdef __cplusplus
}
#endif

#endif /* SDL_version_h_ */
