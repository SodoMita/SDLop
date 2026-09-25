/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Rectangle definitions (subset of <SDL3/SDL_rect.h>).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_rect_h_
#define SDL_rect_h_

#include <SDL3/SDL_stdinc.h>

/** The structure that defines a point (integer). */
typedef struct SDL_Point
{
    int x;
    int y;
} SDL_Point;

/** A rectangle, with the origin at the upper left (integer). */
typedef struct SDL_Rect
{
    int x, y;
    int w, h;
} SDL_Rect;

/** A rectangle, with the origin at the upper left (floating point). */
typedef struct SDL_FPoint
{
    float x;
    float y;
} SDL_FPoint;

typedef struct SDL_FRect
{
    float x, y;
    float w, h;
} SDL_FRect;

#endif /* SDL_rect_h_ */
