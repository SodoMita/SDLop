/*
  SDLop - internal pixel helpers shared by SDL_pixels.c, SDL_surface.c and
  the blitters (names follow SDL3's SDL_pixels_c.h with an SDLOP_ prefix).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#ifndef SDLOP_PIXELS_H
#define SDLOP_PIXELS_H

#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>

extern const Uint8 *SDLOP_expand_byte[9];
extern const Uint16 SDLOP_expand_byte_10[];

void SDLOP_Get8888AlphaMaskAndShift(const SDL_PixelFormatDetails *fmt, Uint32 *mask, Uint32 *shift);
SDL_Colorspace SDLOP_GetDefaultColorspaceForFormat(SDL_PixelFormat format);
void SDLOP_DitherPalette(SDL_Palette *palette);
Uint8 SDLOP_FindColor(const SDL_Palette *pal, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
void SDLOP_DetectPalette(const SDL_Palette *pal, bool *is_opaque, bool *has_alpha_channel);
bool SDLOP_IsSamePalette(const SDL_Palette *src, const SDL_Palette *dst);

/* colorspace helpers, internal in SDL3 too (SDL_pixels_c.h) */
#define SDL_sRGBtoLinear                      SDLOP_sRGBtoLinear
#define SDL_sRGBfromLinear                    SDLOP_sRGBfromLinear
#define SDL_PQtoNits                          SDLOP_PQtoNits
#define SDL_PQfromNits                        SDLOP_PQfromNits
#define SDL_GetColorPrimariesConversionMatrix SDLOP_GetColorPrimariesConversionMatrix
#define SDL_ConvertColorPrimaries             SDLOP_ConvertColorPrimaries
float SDL_sRGBtoLinear(float v);
float SDL_sRGBfromLinear(float v);
float SDL_PQtoNits(float v);
float SDL_PQfromNits(float v);
const float *SDL_GetColorPrimariesConversionMatrix(SDL_ColorPrimaries src, SDL_ColorPrimaries dst);
void SDL_ConvertColorPrimaries(float *fR, float *fG, float *fB, const float *matrix);

#endif
