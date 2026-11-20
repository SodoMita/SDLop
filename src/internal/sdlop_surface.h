/*
  SDLop - internal surface representation.

  Every SDL_Surface handed out by SDLop is the first member of an
  SDLOP_SurfaceData; `reserved` points at a magic byte so foreign or
  destroyed surfaces are rejected (as SDL3's SDL_SurfaceValid does).
  The blit state mirrors SDL3's SDL_BlitMap.info (flags, color/alpha
  modulation, colorkey) and uses the same SDL_COPY_* flag values.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#ifndef SDLOP_SURFACE_H
#define SDLOP_SURFACE_H

#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_properties.h>

#define SDL_COPY_MODULATE_COLOR      0x00000001
#define SDL_COPY_MODULATE_ALPHA      0x00000002
#define SDL_COPY_MODULATE_MASK       (SDL_COPY_MODULATE_COLOR | SDL_COPY_MODULATE_ALPHA)
#define SDL_COPY_BLEND               0x00000010
#define SDL_COPY_BLEND_PREMULTIPLIED 0x00000020
#define SDL_COPY_ADD                 0x00000040
#define SDL_COPY_ADD_PREMULTIPLIED   0x00000080
#define SDL_COPY_MOD                 0x00000100
#define SDL_COPY_MUL                 0x00000200
#define SDL_COPY_BLEND_MASK          (SDL_COPY_BLEND | SDL_COPY_BLEND_PREMULTIPLIED | SDL_COPY_ADD | SDL_COPY_ADD_PREMULTIPLIED | SDL_COPY_MOD | SDL_COPY_MUL)
#define SDL_COPY_COLORKEY            0x00000400
#define SDL_COPY_NEAREST             0x00000800
#define SDL_COPY_RLE_DESIRED         0x00001000
#define SDL_COPY_RLE_COLORKEY        0x00002000
#define SDL_COPY_RLE_ALPHAKEY        0x00004000
#define SDL_COPY_RLE_MASK            (SDL_COPY_RLE_DESIRED | SDL_COPY_RLE_COLORKEY | SDL_COPY_RLE_ALPHAKEY)

#define SDL_INTERNAL_SURFACE_DONTFREE 0x00000001u /* referenced internally */
#define SDL_INTERNAL_SURFACE_STACK    0x00000002u /* allocated on the stack */

typedef struct SDLOP_BlitInfo
{
    int flags;
    Uint32 colorkey;
    Uint8 r, g, b, a;
} SDLOP_BlitInfo;

typedef struct SDLOP_BlitMap
{
    SDLOP_BlitInfo info;
} SDLOP_BlitMap;

typedef struct SDLOP_SurfaceData
{
    SDL_Surface pub; /* must be first */
    Uint32 internal_flags;
    SDL_PropertiesID props;
    const SDL_PixelFormatDetails *fmt;
    SDL_Colorspace colorspace;
    SDL_Palette *palette;
    int locked;
    int num_images;
    SDL_Surface **images;
    SDL_Rect clip_rect;
    SDLOP_BlitMap map;
} SDLOP_SurfaceData;

#define SI(s) ((SDLOP_SurfaceData *)(s))

#define SDL_SurfaceValid SDLOP_SurfaceValid
bool SDL_SurfaceValid(SDL_Surface *surface);
#define SDL_InvalidateMap(map) ((void)(map))

/* surface with the best-matching alternate image for a display scale
 * (new reference), as SDL3's SDL_GetSurfaceImage */
SDL_Surface *SDLOP_GetSurfaceImage(SDL_Surface *surface, float display_scale);

/* SDR white point / HDR headroom of a surface in a colorspace (SDL3's
 * internal SDL_GetSurfaceSDRWhitePoint / SDL_GetSurfaceHDRHeadroom) */
#define SDL_GetSurfaceSDRWhitePoint SDLOP_GetSurfaceSDRWhitePoint
#define SDL_GetSurfaceHDRHeadroom   SDLOP_GetSurfaceHDRHeadroom
float SDL_GetSurfaceSDRWhitePoint(SDL_Surface *surface, SDL_Colorspace colorspace);
float SDL_GetSurfaceHDRHeadroom(SDL_Surface *surface, SDL_Colorspace colorspace);

/* SDL_blit_float.c: SDL3's SDL_Blit_Slow_Float, used when the colorspaces
 * differ or a format is wider than 32 bits. src/dst point at the top-left
 * pixel of the source and destination rectangles. */
void SDLOP_BlitSlowFloat(const SDLOP_BlitInfo *info, SDL_Surface *src_surface, const Uint8 *src, int src_pitch,
                         int src_w, int src_h, SDL_Surface *dst_surface, Uint8 *dst, int dst_pitch, int dst_w, int dst_h);

/* SDL_blit.c */
bool SDLOP_BlitUnchecked(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect);
SDL_Surface *SDLOP_ConvertSurfaceRect(SDL_Surface *surface, const SDL_Rect *rect, SDL_PixelFormat format);

/* SDL_rotate.c (SDL_gfx rotozoom, as in SDL3) */
void SDLgfx_rotozoomSurfaceSizeTrig(int width, int height, double angle, const SDL_FPoint *center,
                                    SDL_Rect *rect_dest, double *cangle, double *sangle);
SDL_Surface *SDLgfx_rotateSurface(SDL_Surface *src, double angle, int smooth, int flipx, int flipy,
                                  const SDL_Rect *rect_dest, double cangle, double sangle, const SDL_FPoint *center);

#endif
