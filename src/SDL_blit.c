/*
  SDLop - software blitter and SDL_StretchSurface.

  SDLOP_BlitUnchecked() is the single blit engine behind SDL_BlitSurface*,
  SDL_ConvertSurface*, SDL_ConvertPixels* and SDL_ScaleSurface. Its
  per-pixel arithmetic is SDL3's "ONE TRUE BLITTER" (SDL_Blit_Slow in
  src/video/SDL_blit_slow.c, release-3.4.20): color/alpha modulation,
  colorkey, the seven blend modes and nearest-neighbour stepping produce
  the same results as stock SDL's generic path. Same-format copies and
  palette remaps take memcpy/table fast paths, as SDL3 does.

  The blitter that SDL3 would pick (SDL_CalculateBlit) decides the
  rounding: SDL_Blit_Slow, the generated 8888 blitters or the
  SDL_blit_A.c alpha blenders (see BlitMath). Like SDL3, blits between
  different colorspaces (e.g. sRGB <-> HDR10, the default for 2101010)
  or involving formats wider than 32 bits go through the floating point
  blitter in SDL_blit_float.c.

  Formats: every packed/array format, including 2101010, 48/64-bit and
  half/float formats, INDEX8 in both directions and INDEX1/2/4 as
  sources. FOURCC (YUV) is not supported.

  SDL_StretchSurface() is a port of SDL3's src/video/SDL_stretch.c C paths
  (nearest for 1-4 bytes per pixel, pixman-compatible bilinear for 32-bit).

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
#include "internal/sdlop_pixels.h"
#include "internal/sdlop_surface.h"

typedef enum
{
    ACCESS_INDEX_SUB, /* INDEX1/2/4 (source only) */
    ACCESS_INDEX8,
    ACCESS_RGB,
    ACCESS_RGBA,
    ACCESS_10BIT,
    ACCESS_LARGE /* > 32 bits per pixel, float blitter only */
} PixelAccess;

static bool GetPixelAccess(const SDL_PixelFormatDetails *fmt, bool is_dst, PixelAccess *access)
{
    const SDL_PixelFormat format = fmt->format;
    if (SDL_ISPIXELFORMAT_FOURCC(format)) {
        return SDL_SetError("Blit combination not supported (format %s)", SDL_GetPixelFormatName(format));
    }
    if (SDL_ISPIXELFORMAT_INDEXED(format)) {
        if (SDL_BITSPERPIXEL(format) == 8) {
            *access = ACCESS_INDEX8;
            return true;
        }
        if (is_dst) {
            return SDL_SetError("Blit combination not supported (destination %s)", SDL_GetPixelFormatName(format));
        }
        *access = ACCESS_INDEX_SUB;
        return true;
    }
    if (SDL_BYTESPERPIXEL(format) > 4) {
        *access = ACCESS_LARGE;
    } else if (SDL_ISPIXELFORMAT_10BIT(format)) {
        *access = ACCESS_10BIT;
    } else if (SDL_ISPIXELFORMAT_ALPHA(format)) {
        *access = ACCESS_RGBA;
    } else {
        *access = ACCESS_RGB;
    }
    return true;
}

static SDL_INLINE Uint32 LoadPixel(const Uint8 *p, int bpp)
{
    Uint32 pixel = 0;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    SDL_memcpy(((Uint8 *)&pixel) + (sizeof(pixel) - bpp), p, bpp);
#else
    SDL_memcpy(&pixel, p, bpp);
#endif
    return pixel;
}

static SDL_INLINE void StorePixel(Uint8 *p, int bpp, Uint32 pixel)
{
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    SDL_memcpy(p, ((Uint8 *)&pixel) + (sizeof(pixel) - bpp), bpp);
#else
    SDL_memcpy(p, &pixel, bpp);
#endif
}

static SDL_INLINE Uint8 SubByteIndex(const Uint8 *row, Uint64 x, SDL_PixelFormat format)
{
    const int bits = SDL_BITSPERPIXEL(format);
    const int per_byte = 8 / bits;
    const Uint8 byte = row[x / per_byte];
    const int slot = (int)(x % per_byte);
    const int shift = (SDL_PIXELORDER(format) == SDL_BITMAPORDER_4321) ? slot * bits : (8 - bits) - slot * bits;
    return (Uint8)((byte >> shift) & ((1 << bits) - 1));
}

/* the RGBA_FROM_ARGB2101010 family from SDL_blit.h */
static SDL_INLINE void Read10Bit(Uint32 pixel, SDL_PixelFormat format, Uint32 *r, Uint32 *g, Uint32 *b, Uint32 *a)
{
    if (format == SDL_PIXELFORMAT_ARGB2101010 || format == SDL_PIXELFORMAT_XRGB2101010) {
        *r = ((pixel >> 22) & 0xFF);
        *g = ((pixel >> 12) & 0xFF);
        *b = ((pixel >> 2) & 0xFF);
    } else {
        *r = ((pixel >> 2) & 0xFF);
        *g = ((pixel >> 12) & 0xFF);
        *b = ((pixel >> 22) & 0xFF);
    }
    *a = SDLOP_expand_byte[2][(pixel >> 30)];
    if (format == SDL_PIXELFORMAT_XRGB2101010 || format == SDL_PIXELFORMAT_XBGR2101010) {
        *a = 0xFF;
    }
}

static SDL_INLINE Uint32 Write10Bit(SDL_PixelFormat format, Uint32 r, Uint32 g, Uint32 b, Uint32 a)
{
    if (format == SDL_PIXELFORMAT_XRGB2101010 || format == SDL_PIXELFORMAT_XBGR2101010) {
        a = 0xFF;
    }
    r = r ? ((r << 2) | 0x3) : 0;
    g = g ? ((g << 2) | 0x3) : 0;
    b = b ? ((b << 2) | 0x3) : 0;
    a = (a * 3) / 255;
    if (format == SDL_PIXELFORMAT_ARGB2101010 || format == SDL_PIXELFORMAT_XRGB2101010) {
        return (a << 30) | (r << 20) | (g << 10) | b;
    }
    return (a << 30) | (b << 20) | (g << 10) | r;
}

typedef struct DstPaletteCache
{
    const SDL_Palette *pal;
    Uint32 last_pixel;
    Uint8 last_index;
    bool valid;
} DstPaletteCache;

static SDL_INLINE Uint8 LookupDstIndex(DstPaletteCache *cache, Uint32 r, Uint32 g, Uint32 b, Uint32 a)
{
    const Uint32 key = (r << 24) | (g << 16) | (b << 8) | a;
    if (!cache->valid || key != cache->last_pixel) {
        cache->last_pixel = key;
        cache->last_index = SDLOP_FindColor(cache->pal, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a);
        cache->valid = true;
    }
    return cache->last_index;
}

/* Rounding flavour of the per-pixel math. SDL3 runs different blitters
 * for different format/flag combinations and they do not all round the
 * same way; mirroring its choice keeps results bit-identical:
 *   BLIT_MATH_SLOW  - SDL_Blit_Slow: truncating x * y / 255
 *   BLIT_MATH_AUTO  - SDL_blit_auto.c (8888 formats): MULT_DIV_255
 *   BLIT_MATH_ALPHA - SDL_blit_A.c / SDL_blit_1.c per-pixel and
 *                     per-surface alpha: ALPHA_BLEND_RGBA
 * The remaining values reproduce how SDL 3.4 writes INDEX8 targets from
 * non-indexed sources in its specialised blitters. SDL_MapSurface leaves
 * info->table NULL for BitField->Palette maps, so:
 *   BLIT_MATH_ALPHA_332     - BlitNto1PixelAlpha / BlitNto1SurfaceAlpha
 *                             store the blended colour packed as RGB332,
 *                             used directly as the palette index
 *   BLIT_MATH_ALPHA_332_KEY - BlitNto1SurfaceAlphaKey: as above, colorkey
 *                             compared against the raw pixel (0 for 24bpp)
 *   BLIT_MATH_KEY_ZERO      - BlitNtoNKey: PIXEL_FROM_RGBA with an INDEX8
 *                             format yields 0 for every non-key pixel
 * They are faithful to stock SDL (verified by tests/surface_diff.c) so
 * programs see identical pixels on either library. */
typedef enum
{
    BLIT_MATH_SLOW,
    BLIT_MATH_AUTO,
    BLIT_MATH_ALPHA,
    BLIT_MATH_ALPHA_332,
    BLIT_MATH_ALPHA_332_KEY,
    BLIT_MATH_KEY_ZERO
} BlitMath;

#define BLIT_MATH_IS_ALPHA(m) ((m) == BLIT_MATH_ALPHA || (m) == BLIT_MATH_ALPHA_332 || (m) == BLIT_MATH_ALPHA_332_KEY)

static SDL_INLINE Uint32 MultDiv255(Uint32 a, Uint32 b)
{
    Uint32 x = a * b + 1;
    x += x >> 8;
    return x >> 8;
}

static SDL_INLINE Uint32 AlphaBlendChannel(Uint32 sC, Uint32 dC, Uint32 sA)
{
    Uint32 x = sC * sA + dC * (255 - sA) + 1;
    x += x >> 8;
    return x >> 8;
}

/* the generic loop, SDL_Blit_Slow semantics */
static void BlitGeneric(const SDLOP_BlitInfo *info, BlitMath math,
                        const SDL_PixelFormatDetails *src_fmt, const SDL_Palette *src_pal, PixelAccess src_access,
                        const Uint8 *src_base, int src_pitch, int src_x, int src_w, int src_h,
                        const SDL_PixelFormatDetails *dst_fmt, const SDL_Palette *dst_pal, PixelAccess dst_access,
                        Uint8 *dst_row, int dst_pitch, int dst_w, int dst_h)
{
    const Uint32 flags = info->flags;
    const Uint32 modulateR = info->r, modulateG = info->g, modulateB = info->b, modulateA = info->a;
    const int srcbpp = src_fmt->bytes_per_pixel;
    const int dstbpp = dst_fmt->bytes_per_pixel;
    const Uint32 rgbmask = ~src_fmt->Amask;
    const Uint32 ckey = info->colorkey & rgbmask;
    DstPaletteCache cache = { dst_pal, 0, 0, false };
    Uint64 incy = dst_h ? ((Uint64)src_h << 16) / dst_h : 0;
    Uint64 incx = dst_w ? ((Uint64)src_w << 16) / dst_w : 0;
    Uint64 posy = incy / 2;

    for (int row = 0; row < dst_h; ++row) {
        const Uint8 *src_row = src_base + (size_t)(posy >> 16) * src_pitch;
        Uint8 *dst = dst_row;
        Uint64 posx = incx / 2;
        for (int n = 0; n < dst_w; ++n, dst += dstbpp, posx += incx) {
            const Uint64 srcx = (Uint64)src_x + (posx >> 16);
            Uint32 srcpixel = 0, srcR = 0, srcG = 0, srcB = 0, srcA = 0;
            Uint32 dstpixel, dstR = 0, dstG = 0, dstB = 0, dstA = 0;

            switch (src_access) {
            case ACCESS_INDEX_SUB:
            case ACCESS_INDEX8:
                srcpixel = (src_access == ACCESS_INDEX8) ? src_row[srcx] : SubByteIndex(src_row, srcx, src_fmt->format);
                if ((int)srcpixel < src_pal->ncolors) {
                    srcR = src_pal->colors[srcpixel].r;
                    srcG = src_pal->colors[srcpixel].g;
                    srcB = src_pal->colors[srcpixel].b;
                    srcA = src_pal->colors[srcpixel].a;
                } else {
                    srcR = srcG = srcB = srcA = 0xFF; /* SDL3: out-of-range indices read as white */
                }
                break;
            case ACCESS_RGB:
                srcpixel = LoadPixel(src_row + srcx * srcbpp, srcbpp);
                srcR = SDLOP_expand_byte[src_fmt->Rbits][(srcpixel & src_fmt->Rmask) >> src_fmt->Rshift];
                srcG = SDLOP_expand_byte[src_fmt->Gbits][(srcpixel & src_fmt->Gmask) >> src_fmt->Gshift];
                srcB = SDLOP_expand_byte[src_fmt->Bbits][(srcpixel & src_fmt->Bmask) >> src_fmt->Bshift];
                srcA = 0xFF;
                break;
            case ACCESS_RGBA:
                srcpixel = LoadPixel(src_row + srcx * srcbpp, srcbpp);
                srcR = SDLOP_expand_byte[src_fmt->Rbits][(srcpixel & src_fmt->Rmask) >> src_fmt->Rshift];
                srcG = SDLOP_expand_byte[src_fmt->Gbits][(srcpixel & src_fmt->Gmask) >> src_fmt->Gshift];
                srcB = SDLOP_expand_byte[src_fmt->Bbits][(srcpixel & src_fmt->Bmask) >> src_fmt->Bshift];
                srcA = SDLOP_expand_byte[src_fmt->Abits][(srcpixel & src_fmt->Amask) >> src_fmt->Ashift];
                break;
            case ACCESS_10BIT:
                srcpixel = LoadPixel(src_row + srcx * 4, 4);
                Read10Bit(srcpixel, src_fmt->format, &srcR, &srcG, &srcB, &srcA);
                break;
            case ACCESS_LARGE: /* handled by SDLOP_BlitSlowFloat */
                break;
            }

            if (flags & SDL_COPY_COLORKEY) {
                if (math == BLIT_MATH_ALPHA_332_KEY) {
                    if ((srcbpp == 3 ? 0 : srcpixel) == info->colorkey) {
                        continue;
                    }
                } else if ((srcpixel & rgbmask) == ckey) {
                    continue;
                }
            }
            if (flags & SDL_COPY_BLEND_MASK) {
                switch (dst_access) {
                case ACCESS_INDEX8:
                    dstpixel = *dst;
                    if ((int)dstpixel < dst_pal->ncolors) {
                        dstR = dst_pal->colors[dstpixel].r;
                        dstG = dst_pal->colors[dstpixel].g;
                        dstB = dst_pal->colors[dstpixel].b;
                        dstA = dst_pal->colors[dstpixel].a;
                    }
                    break;
                case ACCESS_RGB:
                    dstpixel = LoadPixel(dst, dstbpp);
                    dstR = SDLOP_expand_byte[dst_fmt->Rbits][(dstpixel & dst_fmt->Rmask) >> dst_fmt->Rshift];
                    dstG = SDLOP_expand_byte[dst_fmt->Gbits][(dstpixel & dst_fmt->Gmask) >> dst_fmt->Gshift];
                    dstB = SDLOP_expand_byte[dst_fmt->Bbits][(dstpixel & dst_fmt->Bmask) >> dst_fmt->Bshift];
                    dstA = 0xFF;
                    break;
                case ACCESS_RGBA:
                    dstpixel = LoadPixel(dst, dstbpp);
                    dstR = SDLOP_expand_byte[dst_fmt->Rbits][(dstpixel & dst_fmt->Rmask) >> dst_fmt->Rshift];
                    dstG = SDLOP_expand_byte[dst_fmt->Gbits][(dstpixel & dst_fmt->Gmask) >> dst_fmt->Gshift];
                    dstB = SDLOP_expand_byte[dst_fmt->Bbits][(dstpixel & dst_fmt->Bmask) >> dst_fmt->Bshift];
                    dstA = SDLOP_expand_byte[dst_fmt->Abits][(dstpixel & dst_fmt->Amask) >> dst_fmt->Ashift];
                    break;
                case ACCESS_10BIT:
                    Read10Bit(LoadPixel(dst, 4), dst_fmt->format, &dstR, &dstG, &dstB, &dstA);
                    break;
                case ACCESS_INDEX_SUB:
                case ACCESS_LARGE: /* handled by SDLOP_BlitSlowFloat */
                    break;
                }
            }

            if (math == BLIT_MATH_AUTO) {
                if (flags & SDL_COPY_MODULATE_COLOR) {
                    srcR = MultDiv255(srcR, modulateR);
                    srcG = MultDiv255(srcG, modulateG);
                    srcB = MultDiv255(srcB, modulateB);
                }
                if (flags & SDL_COPY_MODULATE_ALPHA) {
                    srcA = MultDiv255(srcA, modulateA);
                }
                if (flags & (SDL_COPY_BLEND | SDL_COPY_ADD)) {
                    if (srcA < 255) {
                        srcR = MultDiv255(srcR, srcA);
                        srcG = MultDiv255(srcG, srcA);
                        srcB = MultDiv255(srcB, srcA);
                    }
                }
                switch (flags & SDL_COPY_BLEND_MASK) {
                case 0:
                    dstR = srcR;
                    dstG = srcG;
                    dstB = srcB;
                    dstA = srcA;
                    break;
                case SDL_COPY_BLEND:
                    dstR = MultDiv255(255 - srcA, dstR) + srcR;
                    dstG = MultDiv255(255 - srcA, dstG) + srcG;
                    dstB = MultDiv255(255 - srcA, dstB) + srcB;
                    dstA = MultDiv255(255 - srcA, dstA) + srcA;
                    break;
                case SDL_COPY_BLEND_PREMULTIPLIED:
                    dstR = SDL_min(MultDiv255(255 - srcA, dstR) + srcR, 255);
                    dstG = SDL_min(MultDiv255(255 - srcA, dstG) + srcG, 255);
                    dstB = SDL_min(MultDiv255(255 - srcA, dstB) + srcB, 255);
                    dstA = SDL_min(MultDiv255(255 - srcA, dstA) + srcA, 255);
                    break;
                case SDL_COPY_ADD:
                case SDL_COPY_ADD_PREMULTIPLIED:
                    dstR = SDL_min(srcR + dstR, 255);
                    dstG = SDL_min(srcG + dstG, 255);
                    dstB = SDL_min(srcB + dstB, 255);
                    break;
                case SDL_COPY_MOD:
                    dstR = MultDiv255(srcR, dstR);
                    dstG = MultDiv255(srcG, dstG);
                    dstB = MultDiv255(srcB, dstB);
                    break;
                case SDL_COPY_MUL:
                    dstR = SDL_min(MultDiv255(srcR, dstR) + MultDiv255(dstR, 255 - srcA), 255);
                    dstG = SDL_min(MultDiv255(srcG, dstG) + MultDiv255(dstG, 255 - srcA), 255);
                    dstB = SDL_min(MultDiv255(srcB, dstB) + MultDiv255(dstB, 255 - srcA), 255);
                    break;
                }
                goto store;
            }

            if (flags & SDL_COPY_MODULATE_COLOR) {
                srcR = (srcR * modulateR) / 255;
                srcG = (srcG * modulateG) / 255;
                srcB = (srcB * modulateB) / 255;
            }
            if (flags & SDL_COPY_MODULATE_ALPHA) {
                srcA = (srcA * modulateA) / 255;
            }
            if (BLIT_MATH_IS_ALPHA(math)) {
                dstR = AlphaBlendChannel(srcR, dstR, srcA);
                dstG = AlphaBlendChannel(srcG, dstG, srcA);
                dstB = AlphaBlendChannel(srcB, dstB, srcA);
                dstA = AlphaBlendChannel(255, dstA, srcA);
                goto store;
            }
            if (flags & (SDL_COPY_BLEND | SDL_COPY_ADD)) {
                if (srcA < 255) {
                    srcR = (srcR * srcA) / 255;
                    srcG = (srcG * srcA) / 255;
                    srcB = (srcB * srcA) / 255;
                }
            }
            switch (flags & SDL_COPY_BLEND_MASK) {
            case 0:
                dstR = srcR;
                dstG = srcG;
                dstB = srcB;
                dstA = srcA;
                break;
            case SDL_COPY_BLEND:
                dstR = srcR + ((255 - srcA) * dstR) / 255;
                dstG = srcG + ((255 - srcA) * dstG) / 255;
                dstB = srcB + ((255 - srcA) * dstB) / 255;
                dstA = srcA + ((255 - srcA) * dstA) / 255;
                break;
            case SDL_COPY_BLEND_PREMULTIPLIED:
                dstR = SDL_min(srcR + ((255 - srcA) * dstR) / 255, 255);
                dstG = SDL_min(srcG + ((255 - srcA) * dstG) / 255, 255);
                dstB = SDL_min(srcB + ((255 - srcA) * dstB) / 255, 255);
                dstA = SDL_min(srcA + ((255 - srcA) * dstA) / 255, 255);
                break;
            case SDL_COPY_ADD:
            case SDL_COPY_ADD_PREMULTIPLIED:
                dstR = SDL_min(srcR + dstR, 255);
                dstG = SDL_min(srcG + dstG, 255);
                dstB = SDL_min(srcB + dstB, 255);
                break;
            case SDL_COPY_MOD:
                dstR = (srcR * dstR) / 255;
                dstG = (srcG * dstG) / 255;
                dstB = (srcB * dstB) / 255;
                break;
            case SDL_COPY_MUL:
                dstR = SDL_min(((srcR * dstR) + (dstR * (255 - srcA))) / 255, 255);
                dstG = SDL_min(((srcG * dstG) + (dstG * (255 - srcA))) / 255, 255);
                dstB = SDL_min(((srcB * dstB) + (dstB * (255 - srcA))) / 255, 255);
                break;
            }

        store:
            switch (dst_access) {
            case ACCESS_INDEX8:
                if (math == BLIT_MATH_ALPHA_332 || math == BLIT_MATH_ALPHA_332_KEY) {
                    *dst = (Uint8)(((dstR >> 5) << 5) | ((dstG >> 5) << 2) | (dstB >> 6));
                } else if (math == BLIT_MATH_KEY_ZERO) {
                    *dst = 0;
                } else {
                    *dst = LookupDstIndex(&cache, dstR, dstG, dstB, dstA);
                }
                break;
            case ACCESS_RGB:
                StorePixel(dst, dstbpp,
                           ((dstR >> (8 - dst_fmt->Rbits)) << dst_fmt->Rshift) |
                           ((dstG >> (8 - dst_fmt->Gbits)) << dst_fmt->Gshift) |
                           ((dstB >> (8 - dst_fmt->Bbits)) << dst_fmt->Bshift));
                break;
            case ACCESS_RGBA:
                StorePixel(dst, dstbpp,
                           ((dstR >> (8 - dst_fmt->Rbits)) << dst_fmt->Rshift) |
                           ((dstG >> (8 - dst_fmt->Gbits)) << dst_fmt->Gshift) |
                           ((dstB >> (8 - dst_fmt->Bbits)) << dst_fmt->Bshift) |
                           ((dstA >> (8 - dst_fmt->Abits)) << dst_fmt->Ashift));
                break;
            case ACCESS_10BIT:
                StorePixel(dst, 4, Write10Bit(dst_fmt->format, dstR, dstG, dstB, dstA));
                break;
            case ACCESS_INDEX_SUB:
            case ACCESS_LARGE: /* handled by SDLOP_BlitSlowFloat */
                break;
            }
        }
        posy += incy;
        dst_row += dst_pitch;
    }
}

static bool IsAutoSrc(SDL_PixelFormat f)
{
    return f == SDL_PIXELFORMAT_XRGB8888 || f == SDL_PIXELFORMAT_XBGR8888 || f == SDL_PIXELFORMAT_ARGB8888 ||
           f == SDL_PIXELFORMAT_RGBA8888 || f == SDL_PIXELFORMAT_ABGR8888 || f == SDL_PIXELFORMAT_BGRA8888;
}

static bool IsAutoDst(SDL_PixelFormat f)
{
    return f == SDL_PIXELFORMAT_XRGB8888 || f == SDL_PIXELFORMAT_XBGR8888 || f == SDL_PIXELFORMAT_ARGB8888 ||
           f == SDL_PIXELFORMAT_ABGR8888;
}

/* which SDL3 blitter family would run (SDL_CalculateBlit order) */
static BlitMath ChooseMath(const SDLOP_SurfaceData *s, const SDLOP_SurfaceData *d, PixelAccess src_access,
                           PixelAccess dst_access, SDLOP_BlitInfo *info, bool nearest)
{
    const int flags = info->flags | (nearest ? SDL_COPY_NEAREST : 0);
    const int dstbpp = d->fmt->bytes_per_pixel;
    const bool auto_ok = !(flags & SDL_COPY_COLORKEY) && IsAutoSrc(s->fmt->format) && IsAutoDst(d->fmt->format);

    if (src_access == ACCESS_10BIT || dst_access == ACCESS_10BIT || src_access == ACCESS_INDEX_SUB) {
        return BLIT_MATH_SLOW;
    }
    if (src_access == ACCESS_INDEX8) { /* SDL_CalculateBlit1 */
        switch (flags) {
        case SDL_COPY_COLORKEY | SDL_COPY_BLEND:
            if (info->a == 255) {
                info->flags = SDL_COPY_COLORKEY; /* keyed copy, palette alpha ignored */
                return BLIT_MATH_SLOW;
            }
            return dstbpp >= 2 ? BLIT_MATH_ALPHA : BLIT_MATH_SLOW;
        case SDL_COPY_BLEND:
        case SDL_COPY_MODULATE_ALPHA | SDL_COPY_BLEND:
        case SDL_COPY_COLORKEY | SDL_COPY_MODULATE_ALPHA | SDL_COPY_BLEND:
            return dstbpp >= 2 ? BLIT_MATH_ALPHA : BLIT_MATH_SLOW;
        default:
            return BLIT_MATH_SLOW;
        }
    }
    if (flags & SDL_COPY_BLEND) { /* SDL_CalculateBlitA */
        if (flags == SDL_COPY_BLEND || (!s->fmt->Amask && flags == (SDL_COPY_MODULATE_ALPHA | SDL_COPY_BLEND))) {
            return (dst_access == ACCESS_INDEX8) ? BLIT_MATH_ALPHA_332 : BLIT_MATH_ALPHA;
        }
        if (!s->fmt->Amask && flags == (SDL_COPY_COLORKEY | SDL_COPY_MODULATE_ALPHA | SDL_COPY_BLEND)) {
            return (dst_access == ACCESS_INDEX8) ? BLIT_MATH_ALPHA_332_KEY : BLIT_MATH_ALPHA;
        }
    } else if (flags == SDL_COPY_COLORKEY && dst_access == ACCESS_INDEX8) {
        return BLIT_MATH_KEY_ZERO; /* SDL_CalculateBlitN: BlitNtoNKey */
    }
    /* SDL_CalculateBlitN covers plain and colorkeyed conversion (exact),
     * everything else goes to the generated 8888 blitters or Blit_Slow */
    return auto_ok ? BLIT_MATH_AUTO : BLIT_MATH_SLOW;
}

bool SDLOP_BlitUnchecked(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect)
{
    SDLOP_SurfaceData *s = SI(src);
    SDLOP_SurfaceData *d = SI(dst);
    PixelAccess src_access, dst_access;
    SDLOP_BlitInfo info = s->map.info;
    const Uint32 flags = info.flags & (SDL_COPY_MODULATE_MASK | SDL_COPY_BLEND_MASK | SDL_COPY_COLORKEY);
    const bool scaled = (srcrect->w != dstrect->w || srcrect->h != dstrect->h);
    /* SDL_BlitSurfaceUncheckedScaled sets SDL_COPY_NEAREST and SDL3 keeps
     * it until the next unscaled SDL_BlitSurface*; it selects the blitter
     * even when a (tile) rectangle ends up unscaled */
    const bool nearest = scaled || (s->map.info.flags & SDL_COPY_NEAREST);

    if (!src->pixels || !dst->pixels) {
        return SDL_SetError("Surfaces must have pixels to blit");
    }
    if (!GetPixelAccess(s->fmt, false, &src_access) || !GetPixelAccess(d->fmt, true, &dst_access)) {
        return false;
    }
    if ((src_access == ACCESS_INDEX8 || src_access == ACCESS_INDEX_SUB) && !s->palette) {
        return SDL_SetError("src does not have a palette set");
    }
    if (dstrect->w <= 0 || dstrect->h <= 0 || srcrect->w < 0 || srcrect->h < 0) {
        return true;
    }
    /* A zero-sized source is not skipped: SDL3's scaled blitters step by
     * (src_w << 16) / dst_w == 0 and repeat the first source column/row.
     * SDL_BlitSurfaceTiledWithScale relies on this for its last partial
     * tile (remaining_src_w = (int)(remaining_dst_w / scale) can be 0). */

    const int srcbpp = s->fmt->bytes_per_pixel;
    const int dstbpp = d->fmt->bytes_per_pixel;
    Uint8 *dst_row = (Uint8 *)dst->pixels + (size_t)dstrect->y * dst->pitch + (size_t)dstrect->x * dstbpp;

    /* SDL_CalculateBlit: SDL_Blit_Slow_Float first */
    if (s->colorspace != d->colorspace || src_access == ACCESS_LARGE || dst_access == ACCESS_LARGE) {
        if (dst_access == ACCESS_INDEX8 && !d->palette) {
            return SDL_SetError("dst does not have a palette set");
        }
        SDL_Surface *ftmp = NULL;
        const Uint8 *fsrc = (const Uint8 *)src->pixels + (size_t)srcrect->y * src->pitch + (size_t)srcrect->x * srcbpp;
        int fpitch = src->pitch;
        if (src == dst) {
            /* overlapping self-blit: work from a copy */
            ftmp = SDL_DuplicateSurface(src);
            if (!ftmp) {
                return false;
            }
            fsrc = (const Uint8 *)ftmp->pixels + (size_t)srcrect->y * ftmp->pitch + (size_t)srcrect->x * srcbpp;
            fpitch = ftmp->pitch;
        }
        info.flags = flags;
        SDLOP_BlitSlowFloat(&info, ftmp ? ftmp : src, fsrc, fpitch, srcrect->w, srcrect->h, dst, dst_row, dst->pitch, dstrect->w, dstrect->h);
        SDL_DestroySurface(ftmp);
        return true;
    }

    /* palette -> palette without blending: index table (SDL3 Map1to1) */
    if ((src_access == ACCESS_INDEX8 || src_access == ACCESS_INDEX_SUB) && dst_access == ACCESS_INDEX8 &&
        !(flags & (SDL_COPY_MODULATE_MASK | SDL_COPY_BLEND_MASK))) {
        Uint8 table[256];
        bool identity = !s->palette || !d->palette || SDLOP_IsSamePalette(s->palette, d->palette);
        for (int i = 0; i < 256; ++i) {
            table[i] = (Uint8)i;
        }
        if (!identity) {
            for (int i = 0; i < s->palette->ncolors; ++i) {
                const SDL_Color *c = &s->palette->colors[i];
                table[i] = SDLOP_FindColor(d->palette, c->r, c->g, c->b, c->a);
            }
        }
        const Uint64 incy = ((Uint64)srcrect->h << 16) / dstrect->h;
        const Uint64 incx = ((Uint64)srcrect->w << 16) / dstrect->w;
        Uint64 posy = incy / 2;
        for (int y = 0; y < dstrect->h; ++y, posy += incy, dst_row += dst->pitch) {
            const Uint8 *src_row = (const Uint8 *)src->pixels + (size_t)(srcrect->y + (posy >> 16)) * src->pitch;
            Uint64 posx = incx / 2;
            for (int x = 0; x < dstrect->w; ++x, posx += incx) {
                const Uint64 sx = (Uint64)srcrect->x + (posx >> 16);
                const Uint8 index = (src_access == ACCESS_INDEX8) ? src_row[sx] : SubByteIndex(src_row, sx, s->fmt->format);
                if ((flags & SDL_COPY_COLORKEY) && index == (Uint8)info.colorkey) {
                    continue;
                }
                dst_row[x] = table[index];
            }
        }
        return true;
    }
    if (dst_access == ACCESS_INDEX8 && !d->palette) {
        return SDL_SetError("dst does not have a palette set");
    }

    /* identical formats, plain copy: row memmove (handles self-blits) */
    if (!scaled && !flags && src->format == dst->format && src_access != ACCESS_INDEX_SUB) {
        const size_t len = (size_t)dstrect->w * dstbpp;
        const Uint8 *src_row = (const Uint8 *)src->pixels + (size_t)srcrect->y * src->pitch + (size_t)srcrect->x * srcbpp;
        if (src == dst && srcrect->y < dstrect->y) {
            for (int y = dstrect->h - 1; y >= 0; --y) {
                SDL_memmove(dst_row + (size_t)y * dst->pitch, src_row + (size_t)y * src->pitch, len);
            }
        } else {
            for (int y = 0; y < dstrect->h; ++y) {
                SDL_memmove(dst_row, src_row, len);
                dst_row += dst->pitch;
                src_row += src->pitch;
            }
        }
        return true;
    }

    /* self-overlapping conversions/blends go through a copy */
    SDL_Surface *tmp = NULL;
    const Uint8 *src_base;
    int src_pitch;
    int src_x;
    if (src == dst) {
        const int tw = SDL_min(SDL_max(srcrect->w, 1), src->w - srcrect->x);
        const int th = SDL_min(SDL_max(srcrect->h, 1), src->h - srcrect->y);
        if (tw <= 0 || th <= 0) {
            return true;
        }
        tmp = SDL_CreateSurface(tw, th, src->format);
        if (!tmp) {
            return false;
        }
        SDL_SetSurfacePalette(tmp, s->palette);
        for (int y = 0; y < th; ++y) {
            SDL_memcpy((Uint8 *)tmp->pixels + (size_t)y * tmp->pitch,
                       (const Uint8 *)src->pixels + (size_t)(srcrect->y + y) * src->pitch + (size_t)srcrect->x * srcbpp,
                       (size_t)tw * srcbpp);
        }
        src_base = (const Uint8 *)tmp->pixels;
        src_pitch = tmp->pitch;
        src_x = 0;
    } else {
        src_pitch = src->pitch;
        src_base = (const Uint8 *)src->pixels + (size_t)srcrect->y * src->pitch;
        src_x = srcrect->x;
    }

    info.flags = flags;
    BlitGeneric(&info, ChooseMath(s, d, src_access, dst_access, &info, nearest), s->fmt, s->palette, src_access, src_base, src_pitch, src_x, srcrect->w, srcrect->h,
                d->fmt, d->palette, dst_access, dst_row, dst->pitch, dstrect->w, dstrect->h);
    SDL_DestroySurface(tmp);
    return true;
}

/* ------------------------------------------------------------------ */
/* SDL_StretchSurface (SDL3 src/video/SDL_stretch.c, C paths)          */
/* ------------------------------------------------------------------ */

static bool SDL_StretchSurfaceUncheckedNearest(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect);
static bool SDL_StretchSurfaceUncheckedLinear(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect);

bool SDL_StretchSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect, SDL_ScaleMode scaleMode)
{
    bool result;
    int src_locked;
    int dst_locked;
    SDL_Rect full_src;
    SDL_Rect full_dst;

    if (!SDL_SurfaceValid(src)) {
        return SDL_InvalidParamError("src");
    }
    if (!SDL_SurfaceValid(dst)) {
        return SDL_InvalidParamError("dst");
    }

    if (src->format != dst->format) {
        // Slow!
        SDL_Surface *src_tmp = SDL_ConvertSurfaceAndColorspace(src, dst->format, SI(dst)->palette, SI(dst)->colorspace, SI(dst)->props);
        if (!src_tmp) {
            return false;
        }
        result = SDL_StretchSurface(src_tmp, srcrect, dst, dstrect, scaleMode);
        SDL_DestroySurface(src_tmp);
        return result;
    }

    if (SDL_ISPIXELFORMAT_FOURCC(src->format)) {
        return SDL_SetError("SDLop does not support FOURCC (YUV) surfaces");
    }

    switch (scaleMode) {
    case SDL_SCALEMODE_NEAREST:
        break;
    case SDL_SCALEMODE_LINEAR:
        break;
    case SDL_SCALEMODE_PIXELART:
        scaleMode = SDL_SCALEMODE_NEAREST;
        break;
    default:
        return SDL_InvalidParamError("scaleMode");
    }

    if (scaleMode == SDL_SCALEMODE_LINEAR) {
        if (SDL_BYTESPERPIXEL(src->format) != 4 || src->format == SDL_PIXELFORMAT_ARGB2101010) {
            return SDL_SetError("Wrong format");
        }
    } else if (SDL_BITSPERPIXEL(src->format) < 8 || SDL_BYTESPERPIXEL(src->format) > 4) {
        return SDL_SetError("Wrong format");
    }

    // Verify the blit rectangles
    if (srcrect) {
        if ((srcrect->x < 0) || (srcrect->y < 0) ||
            ((srcrect->x + srcrect->w) > src->w) ||
            ((srcrect->y + srcrect->h) > src->h)) {
            return SDL_SetError("Invalid source blit rectangle");
        }
    } else {
        full_src.x = 0;
        full_src.y = 0;
        full_src.w = src->w;
        full_src.h = src->h;
        srcrect = &full_src;
    }
    if (dstrect) {
        if ((dstrect->x < 0) || (dstrect->y < 0) ||
            ((dstrect->x + dstrect->w) > dst->w) ||
            ((dstrect->y + dstrect->h) > dst->h)) {
            return SDL_SetError("Invalid destination blit rectangle");
        }
    } else {
        full_dst.x = 0;
        full_dst.y = 0;
        full_dst.w = dst->w;
        full_dst.h = dst->h;
        dstrect = &full_dst;
    }

    if (dstrect->w <= 0 || dstrect->h <= 0) {
        return true;
    }
    if (srcrect->w <= 0 || srcrect->h <= 0) {
        return true;
    }

    if (srcrect->w > SDL_MAX_UINT16 || srcrect->h > SDL_MAX_UINT16 ||
        dstrect->w > SDL_MAX_UINT16 || dstrect->h > SDL_MAX_UINT16) {
        return SDL_SetError("Size too large for scaling");
    }

    // Lock the destination if it's in hardware
    dst_locked = 0;
    if (SDL_MUSTLOCK(dst)) {
        if (!SDL_LockSurface(dst)) {
            return SDL_SetError("Unable to lock destination surface");
        }
        dst_locked = 1;
    }
    // Lock the source if it's in hardware
    src_locked = 0;
    if (SDL_MUSTLOCK(src)) {
        if (!SDL_LockSurface(src)) {
            if (dst_locked) {
                SDL_UnlockSurface(dst);
            }
            return SDL_SetError("Unable to lock source surface");
        }
        src_locked = 1;
    }

    if (scaleMode == SDL_SCALEMODE_NEAREST) {
        result = SDL_StretchSurfaceUncheckedNearest(src, srcrect, dst, dstrect);
    } else {
        result = SDL_StretchSurfaceUncheckedLinear(src, srcrect, dst, dstrect);
    }

    // We need to unlock the surfaces if they're locked
    if (dst_locked) {
        SDL_UnlockSurface(dst);
    }
    if (src_locked) {
        SDL_UnlockSurface(src);
    }

    return result;
}

#define PRECISION 7

#define FIXED_POINT(i) ((Uint32)(i) << 16)
#define SRC_INDEX(fp)  ((Uint32)(fp) >> 16)
#define INTEGER(fp)    ((Uint32)(fp) >> PRECISION)
#define FRAC(fp)       ((Uint32)((fp) >> (16 - PRECISION)) & ((1 << PRECISION) - 1))
#define FRAC_ZERO      0
#define FRAC_ONE       (1 << PRECISION)
#define FP_ONE         FIXED_POINT(1)

static void get_scaler_datas(int src_nb, int dst_nb, Sint64 *fp_start, int *fp_step, int *left_pad, int *right_pad)
{
    int step = FIXED_POINT(src_nb) / (dst_nb); // source step in fixed point
    int x0 = FP_ONE / 2;                       // dst first pixel center at 0.5 in fixed point
    Sint64 fp_sum;
    int i;
    // Use this code for perfect match with pixman
    Sint64 tmp[2];
    tmp[0] = (Sint64)step * (x0 >> 16);
    tmp[1] = (Sint64)step * (x0 & 0xFFFF);
    x0 = (int)(tmp[0] + ((tmp[1] + 0x8000) >> 16)); // x0 == (step + 1) / 2
    // -= 0.5, get back the pixel origin, in source coordinates
    x0 -= FP_ONE / 2;

    *fp_start = x0;
    *fp_step = step;
    *left_pad = 0;
    *right_pad = 0;

    fp_sum = x0;
    for (i = 0; i < dst_nb; i++) {
        if (fp_sum < 0) {
            *left_pad += 1;
        } else {
            int index = SRC_INDEX(fp_sum);
            if (index > src_nb - 2) {
                *right_pad += 1;
            }
        }
        fp_sum += step;
    }
}

typedef struct color_t
{
    Uint8 a;
    Uint8 b;
    Uint8 c;
    Uint8 d;
} color_t;

static SDL_INLINE void INTERPOL(const Uint32 *src_x0, const Uint32 *src_x1, int frac0, int frac1, Uint32 *dst)
{
    const color_t *c0 = (const color_t *)src_x0;
    const color_t *c1 = (const color_t *)src_x1;
    color_t *cx = (color_t *)dst;
    cx->a = (Uint8)INTEGER(frac1 * c0->a + frac0 * c1->a);
    cx->b = (Uint8)INTEGER(frac1 * c0->b + frac0 * c1->b);
    cx->c = (Uint8)INTEGER(frac1 * c0->c + frac0 * c1->c);
    cx->d = (Uint8)INTEGER(frac1 * c0->d + frac0 * c1->d);
}

static SDL_INLINE void INTERPOL_BILINEAR(const Uint32 *s0, const Uint32 *s1, int frac_w0, int frac_h0, int frac_h1, Uint32 *dst)
{
    Uint32 tmp[2];
    unsigned int frac_w1 = FRAC_ONE - frac_w0;

    // Vertical first, store to 'tmp'
    INTERPOL(s0, s1, frac_h0, frac_h1, tmp);
    INTERPOL(s0 + 1, s1 + 1, frac_h0, frac_h1, tmp + 1);

    // Horizontal, store to 'dst'
    INTERPOL(tmp, tmp + 1, frac_w0, (int)frac_w1, dst);
}

static bool scale_mat(const Uint32 *src, int src_w, int src_h, int src_pitch, Uint32 *dst, int dst_w, int dst_h, int dst_pitch)
{
    int i;
    Sint64 fp_sum_h;
    int fp_step_h, left_pad_h, right_pad_h;
    Sint64 fp_sum_w;
    int fp_step_w, left_pad_w, right_pad_w;
    Sint64 fp_sum_w_init;
    int left_pad_w_init, right_pad_w_init, dst_gap, middle_init;
    get_scaler_datas(src_h, dst_h, &fp_sum_h, &fp_step_h, &left_pad_h, &right_pad_h);
    get_scaler_datas(src_w, dst_w, &fp_sum_w, &fp_step_w, &left_pad_w, &right_pad_w);
    fp_sum_w_init = fp_sum_w + left_pad_w * fp_step_w;
    left_pad_w_init = left_pad_w;
    right_pad_w_init = right_pad_w;
    dst_gap = dst_pitch - 4 * dst_w;
    middle_init = dst_w - left_pad_w - right_pad_w;

    /* a 1-pixel wide/high source has no right neighbour: SDL3 reads
     * src_w - 2 == -1 there; clamp to a duplicate of the only column */
    Uint32 *edge = NULL;
    if (src_w < 2 || src_h < 2) {
        const int ew = SDL_max(src_w, 2), eh = SDL_max(src_h, 2);
        edge = (Uint32 *)SDL_malloc((size_t)ew * eh * 4);
        if (!edge) {
            return false;
        }
        for (int y = 0; y < eh; ++y) {
            for (int x = 0; x < ew; ++x) {
                edge[y * ew + x] = *(const Uint32 *)((const Uint8 *)src + (size_t)SDL_min(y, src_h - 1) * src_pitch + (size_t)SDL_min(x, src_w - 1) * 4);
            }
        }
        src = edge;
        src_pitch = ew * 4;
        src_w = ew;
        src_h = eh;
    }

    for (i = 0; i < dst_h; i++) {
        int index_h, frac_h0, frac_h1, middle;
        const Uint32 *src_h0, *src_h1;
        int no_padding;
        Uint64 incr_h0, incr_h1;

        no_padding = !(i < left_pad_h || i > dst_h - 1 - right_pad_h);
        index_h = SRC_INDEX(fp_sum_h);
        frac_h0 = FRAC(fp_sum_h);

        index_h = no_padding ? index_h : (i < left_pad_h ? 0 : src_h - 1);
        frac_h0 = no_padding ? frac_h0 : 0;
        incr_h1 = no_padding ? src_pitch : 0;
        incr_h0 = (Uint64)index_h * src_pitch;

        src_h0 = (const Uint32 *)((const Uint8 *)src + incr_h0);
        src_h1 = (const Uint32 *)((const Uint8 *)src_h0 + incr_h1);

        fp_sum_h += fp_step_h;

        frac_h1 = FRAC_ONE - frac_h0;
        fp_sum_w = fp_sum_w_init;
        right_pad_w = right_pad_w_init;
        left_pad_w = left_pad_w_init;
        middle = middle_init;

        while (left_pad_w--) {
            INTERPOL_BILINEAR(src_h0, src_h1, FRAC_ZERO, frac_h0, frac_h1, dst);
            dst += 1;
        }

        while (middle--) {
            const Uint32 *s_00_01;
            const Uint32 *s_10_11;
            int index_w = 4 * SRC_INDEX(fp_sum_w);
            int frac_w = FRAC(fp_sum_w);
            fp_sum_w += fp_step_w;

            s_00_01 = (const Uint32 *)((const Uint8 *)src_h0 + index_w);
            s_10_11 = (const Uint32 *)((const Uint8 *)src_h1 + index_w);

            INTERPOL_BILINEAR(s_00_01, s_10_11, frac_w, frac_h0, frac_h1, dst);

            dst += 1;
        }

        while (right_pad_w--) {
            int index_w = 4 * (src_w - 2);
            const Uint32 *s_00_01 = (const Uint32 *)((const Uint8 *)src_h0 + index_w);
            const Uint32 *s_10_11 = (const Uint32 *)((const Uint8 *)src_h1 + index_w);
            INTERPOL_BILINEAR(s_00_01, s_10_11, FRAC_ONE, frac_h0, frac_h1, dst);
            dst += 1;
        }
        dst = (Uint32 *)((Uint8 *)dst + dst_gap);
    }
    SDL_free(edge);
    return true;
}

static bool SDL_StretchSurfaceUncheckedLinear(SDL_Surface *s, const SDL_Rect *srcrect, SDL_Surface *d, const SDL_Rect *dstrect)
{
    int src_pitch = s->pitch;
    int dst_pitch = d->pitch;
    const Uint32 *src = (const Uint32 *)((const Uint8 *)s->pixels + srcrect->x * 4 + srcrect->y * src_pitch);
    Uint32 *dst = (Uint32 *)((Uint8 *)d->pixels + dstrect->x * 4 + dstrect->y * dst_pitch);
    return scale_mat(src, srcrect->w, srcrect->h, src_pitch, dst, dstrect->w, dstrect->h, dst_pitch);
}

static bool SDL_StretchSurfaceUncheckedNearest(SDL_Surface *s, const SDL_Rect *srcrect, SDL_Surface *d, const SDL_Rect *dstrect)
{
    const int src_w = srcrect->w, src_h = srcrect->h;
    const int dst_w = dstrect->w, dst_h = dstrect->h;
    const int src_pitch = s->pitch, dst_pitch = d->pitch;
    const int bpp = SDL_BYTESPERPIXEL(d->format);
    const Uint8 *src_ptr = (const Uint8 *)s->pixels + srcrect->x * bpp + srcrect->y * src_pitch;
    Uint8 *dst = (Uint8 *)d->pixels + dstrect->x * bpp + dstrect->y * dst_pitch;
    const Uint64 incy = ((Uint64)src_h << 16) / dst_h;
    const Uint64 incx = ((Uint64)src_w << 16) / dst_w;
    Uint64 posy = incy / 2;

    for (int i = 0; i < dst_h; i++) {
        const Uint8 *src_h0 = src_ptr + (posy >> 16) * src_pitch;
        Uint8 *out = dst;
        Uint64 posx = incx / 2;
        posy += incy;
        for (int n = 0; n < dst_w; ++n) {
            const Uint8 *p = src_h0 + bpp * (posx >> 16);
            posx += incx;
            switch (bpp) {
            case 4:
                *(Uint32 *)out = *(const Uint32 *)p;
                break;
            case 2:
                *(Uint16 *)out = *(const Uint16 *)p;
                break;
            case 3:
                out[0] = p[0];
                out[1] = p[1];
                out[2] = p[2];
                break;
            default:
                *out = *p;
                break;
            }
            out += bpp;
        }
        dst += dst_pitch;
    }
    return true;
}
