/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  GENERATED FILE -- do not edit.  Produced by tools/sdlop.py from the upstream SDL3
  header SDL_surface.h (3.2.10), which is zlib licensed, Copyright (C) 1997-2025 Sam Lantinga
  and SDL contributors.  Declarations are copied verbatim so that source and binary
  compatibility with SDL3 are exact; the items SDLop does not implement were
  removed (the full list is in tools/dropped.txt).

  In this header: Minimal surfaces: exactly what SDL_GetWindowSurface()/SDL_UpdateWindowSurface()
 * need, plus SDL_FillSurfaceRect(). There is no blitter, no scaling and no
 * SDL_Surface-to-SDL_Surface conversion.
*/

#ifndef SDL_surface_h_
#define SDL_surface_h_

#include <SDL3/SDL_begin_code.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_properties.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef Uint32 SDL_SurfaceFlags;

#define SDL_SURFACE_PREALLOCATED 0x00000001u

#define SDL_SURFACE_LOCK_NEEDED 0x00000002u

#define SDL_SURFACE_LOCKED 0x00000004u

#define SDL_SURFACE_SIMD_ALIGNED 0x00000008u

#define SDL_MUSTLOCK(S) (((S)->flags & SDL_SURFACE_LOCK_NEEDED) == SDL_SURFACE_LOCK_NEEDED)

#ifndef SDL_INTERNAL

struct SDL_Surface
{
 SDL_SurfaceFlags flags;
 SDL_PixelFormat format;
 int w;
 int h;
 int pitch;
 void *pixels;

 int refcount;

 void *reserved;
};

#endif

typedef struct SDL_Surface SDL_Surface;

extern SDL_DECLSPEC SDL_Surface * SDLCALL SDL_CreateSurface(int width, int height, SDL_PixelFormat format);

extern SDL_DECLSPEC SDL_Surface * SDLCALL SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels, int pitch);

extern SDL_DECLSPEC void SDLCALL SDL_DestroySurface(SDL_Surface *surface);

extern SDL_DECLSPEC SDL_PropertiesID SDLCALL SDL_GetSurfaceProperties(SDL_Surface *surface);

#define SDL_PROP_SURFACE_SDR_WHITE_POINT_FLOAT "SDL.surface.SDR_white_point"

#define SDL_PROP_SURFACE_HDR_HEADROOM_FLOAT "SDL.surface.HDR_headroom"

#define SDL_PROP_SURFACE_TONEMAP_OPERATOR_STRING "SDL.surface.tonemap"

#define SDL_PROP_SURFACE_HOTSPOT_X_NUMBER "SDL.surface.hotspot.x"

#define SDL_PROP_SURFACE_HOTSPOT_Y_NUMBER "SDL.surface.hotspot.y"

extern SDL_DECLSPEC bool SDLCALL SDL_SetSurfaceColorspace(SDL_Surface *surface, SDL_Colorspace colorspace);

extern SDL_DECLSPEC SDL_Colorspace SDLCALL SDL_GetSurfaceColorspace(SDL_Surface *surface);

extern SDL_DECLSPEC bool SDLCALL SDL_LockSurface(SDL_Surface *surface);

extern SDL_DECLSPEC void SDLCALL SDL_UnlockSurface(SDL_Surface *surface);

extern SDL_DECLSPEC bool SDLCALL SDL_FillSurfaceRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);

extern SDL_DECLSPEC bool SDLCALL SDL_FillSurfaceRects(SDL_Surface *dst, const SDL_Rect *rects, int count, Uint32 color);

extern SDL_DECLSPEC Uint32 SDLCALL SDL_MapSurfaceRGB(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b);

extern SDL_DECLSPEC Uint32 SDLCALL SDL_MapSurfaceRGBA(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b, Uint8 a);

#include <SDL3/SDL_close_code.h>

#ifdef __cplusplus
}
#endif

#endif /* SDL_surface_h_ */
