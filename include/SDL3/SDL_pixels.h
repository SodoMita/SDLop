/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Pixel formats (subset of <SDL3/SDL_pixels.h>, identical values).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_pixels_h_
#define SDL_pixels_h_

#include <SDL3/SDL_stdinc.h>

/** Pixel format constants (values identical to SDL3). */
typedef enum SDL_PixelFormat
{
    SDL_PIXELFORMAT_UNKNOWN = 0,
    SDL_PIXELFORMAT_INDEX8 = 0x13000801u,
    SDL_PIXELFORMAT_RGB565 = 0x15151002u,
    SDL_PIXELFORMAT_RGB24 = 0x17101803u,
    SDL_PIXELFORMAT_BGR24 = 0x17401803u,
    SDL_PIXELFORMAT_XRGB8888 = 0x16161804u,
    SDL_PIXELFORMAT_RGBX8888 = 0x16261804u,
    SDL_PIXELFORMAT_ARGB8888 = 0x16362004u,
    SDL_PIXELFORMAT_RGBA8888 = 0x16462004u,
    SDL_PIXELFORMAT_XBGR8888 = 0x16561804u,
    SDL_PIXELFORMAT_BGRX8888 = 0x16661804u,
    SDL_PIXELFORMAT_ABGR8888 = 0x16762004u,
    SDL_PIXELFORMAT_BGRA8888 = 0x16862004u
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    ,
    SDL_PIXELFORMAT_RGBA32 = SDL_PIXELFORMAT_RGBA8888,
    SDL_PIXELFORMAT_ARGB32 = SDL_PIXELFORMAT_ARGB8888
#else
    ,
    SDL_PIXELFORMAT_RGBA32 = SDL_PIXELFORMAT_ABGR8888,
    SDL_PIXELFORMAT_ARGB32 = SDL_PIXELFORMAT_BGRA8888
#endif
} SDL_PixelFormat;

#endif /* SDL_pixels_h_ */
