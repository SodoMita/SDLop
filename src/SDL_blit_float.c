/*
  SDLop - floating point blitter (colorspace conversion, HDR tone mapping,
  pixel formats wider than 32 bits).

  Port of the float half of SDL3's src/video/SDL_blit_slow.c
  (release-3.4.20): SDL_Blit_Slow_Float with ReadFloatPixel /
  WriteFloatPixel, the half-float helpers and the linear / Chrome tone
  mapping operators, verbatim apart from the blit-info plumbing. Upstream
  selects it whenever source and destination colorspaces differ (e.g.
  sRGB <-> HDR10, the default for the 2101010 formats) or either format
  has more than 4 bytes per pixel, and SDLop does the same.

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
#include "internal/sdlop_blit_macros.h"

typedef struct FloatBlitInfo
{
    int flags;
    Uint32 r, g, b, a;
    SDL_Surface *src_surface;
    SDL_Surface *dst_surface;
    const SDL_PixelFormatDetails *src_fmt;
    const SDL_PixelFormatDetails *dst_fmt;
    const SDL_Palette *src_pal;
    const SDL_Palette *dst_pal;
    Uint8 *src;
    int src_pitch;
    int src_w, src_h;
    Uint8 *dst;
    int dst_pitch;
    int dst_w, dst_h;
} FloatBlitInfo;

static Uint8 LookupRGBAColor(Uint32 pixelvalue, const SDL_Palette *pal)
{
    if (!pal) {
        return 0;
    }
    return SDLOP_FindColor(pal, (Uint8)(pixelvalue >> 24), (Uint8)(pixelvalue >> 16), (Uint8)(pixelvalue >> 8), (Uint8)pixelvalue);
}

typedef enum
{
    SlowBlitPixelAccess_Index8,
    SlowBlitPixelAccess_RGB,
    SlowBlitPixelAccess_RGBA,
    SlowBlitPixelAccess_10Bit,
    SlowBlitPixelAccess_Large,
} SlowBlitPixelAccess;

static SlowBlitPixelAccess GetPixelAccessMethod(SDL_PixelFormat format)
{
    if (SDL_BYTESPERPIXEL(format) > 4) {
        return SlowBlitPixelAccess_Large;
    } else if (SDL_ISPIXELFORMAT_10BIT(format)) {
        return SlowBlitPixelAccess_10Bit;
    } else if (format == SDL_PIXELFORMAT_INDEX8) {
        return SlowBlitPixelAccess_Index8;
    } else if (SDL_ISPIXELFORMAT_ALPHA(format)) {
        return SlowBlitPixelAccess_RGBA;
    } else {
        return SlowBlitPixelAccess_RGB;
    }
}

/* Convert from F16 to float
 * Public domain implementation from https://gist.github.com/rygorous/2144712
 */
typedef union
{
    Uint32 u;
    float f;
    struct
    {
        Uint32 Mantissa : 23;
        Uint32 Exponent : 8;
        Uint32 Sign : 1;
    } x;
} FP32;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4214)
#endif

typedef union
{
    Uint16 u;
    struct
    {
        Uint16 Mantissa : 10;
        Uint16 Exponent : 5;
        Uint16 Sign : 1;
    } x;
} FP16;

#ifdef _MSC_VER
#pragma warning(pop)
#endif

static float half_to_float(Uint16 unValue)
{
    static const FP32 magic = { (254 - 15) << 23 };
    static const FP32 was_infnan = { (127 + 16) << 23 };
    FP16 h;
    FP32 o;

    h.u = unValue;
    o.u = (h.u & 0x7fff) << 13;     // exponent/mantissa bits
    o.f *= magic.f;                 // exponent adjust
    if (o.f >= was_infnan.f)        // make sure Inf/NaN survive
        o.u |= 255 << 23;
    o.u |= (Uint32)(h.u & 0x8000) << 16;    // sign bit (SDLop: cast avoids signed-shift UB)
    return o.f;
}

/* Convert from float to F16
 * Public domain implementation from https://stackoverflow.com/questions/76799117/how-to-convert-a-float-to-a-half-type-and-the-other-way-around-in-c
 */
static Uint16 float_to_half(float a)
{
    Uint32 ia;
    Uint16 ir;

    SDL_memcpy(&ia, &a, sizeof(ia));

    ir = (ia >> 16) & 0x8000;
    if ((ia & 0x7f800000) == 0x7f800000) {
        if ((ia & 0x7fffffff) == 0x7f800000) {
            ir |= 0x7c00; // infinity
        } else {
            ir |= 0x7e00 | ((ia >> (24 - 11)) & 0x1ff); // NaN, quietened
        }
    } else if ((ia & 0x7f800000) >= 0x33000000) {
        int shift = (int)((ia >> 23) & 0xff) - 127;
        if (shift > 15) {
            ir |= 0x7c00; // infinity
        } else {
            ia = (ia & 0x007fffff) | 0x00800000; // extract mantissa
            if (shift < -14) { // denormal
                ir |= ia >> (-1 - shift);
                ia = ia << (32 - (-1 - shift));
            } else { // normal
                ir |= ia >> (24 - 11);
                ia = ia << (32 - (24 - 11));
                ir = ir + ((14 + shift) << 10);
            }
            // IEEE-754 round to nearest of even
            if ((ia > 0x80000000) || ((ia == 0x80000000) && (ir & 1))) {
                ir++;
            }
        }
    }
    return ir;
}

static void ReadFloatPixel(Uint8 *pixels, SlowBlitPixelAccess access, const SDL_PixelFormatDetails *fmt, const SDL_Palette *pal, SDL_Colorspace colorspace, float SDR_white_point,
                           float *outR, float *outG, float *outB, float *outA)
{
    Uint32 pixelvalue;
    Uint32 R, G, B, A;
    float fR = 0.0f, fG = 0.0f, fB = 0.0f, fA = 0.0f;
    float v[4];

    switch (access) {
    case SlowBlitPixelAccess_Index8:
        pixelvalue = *pixels;
        fR = (float)pal->colors[pixelvalue].r / 255.0f;
        fG = (float)pal->colors[pixelvalue].g / 255.0f;
        fB = (float)pal->colors[pixelvalue].b / 255.0f;
        fA = (float)pal->colors[pixelvalue].a / 255.0f;
        break;
    case SlowBlitPixelAccess_RGB:
        DISEMBLE_RGB(pixels, fmt->bytes_per_pixel, fmt, pixelvalue, R, G, B);
        fR = (float)R / 255.0f;
        fG = (float)G / 255.0f;
        fB = (float)B / 255.0f;
        fA = 1.0f;
        break;
    case SlowBlitPixelAccess_RGBA:
        DISEMBLE_RGBA(pixels, fmt->bytes_per_pixel, fmt, pixelvalue, R, G, B, A);
        fR = (float)R / 255.0f;
        fG = (float)G / 255.0f;
        fB = (float)B / 255.0f;
        fA = (float)A / 255.0f;
        break;
    case SlowBlitPixelAccess_10Bit:
        pixelvalue = *((Uint32 *)pixels);
        switch (fmt->format) {
        case SDL_PIXELFORMAT_XRGB2101010:
            RGBAFLOAT_FROM_ARGB2101010(pixelvalue, fR, fG, fB, fA);
            fA = 1.0f;
            break;
        case SDL_PIXELFORMAT_XBGR2101010:
            RGBAFLOAT_FROM_ABGR2101010(pixelvalue, fR, fG, fB, fA);
            fA = 1.0f;
            break;
        case SDL_PIXELFORMAT_ARGB2101010:
            RGBAFLOAT_FROM_ARGB2101010(pixelvalue, fR, fG, fB, fA);
            break;
        case SDL_PIXELFORMAT_ABGR2101010:
            RGBAFLOAT_FROM_ABGR2101010(pixelvalue, fR, fG, fB, fA);
            break;
        default:
            fR = fG = fB = fA = 0.0f;
            break;
        }
        break;
    case SlowBlitPixelAccess_Large:
        switch (SDL_PIXELTYPE(fmt->format)) {
        case SDL_PIXELTYPE_ARRAYU16:
            v[0] = (float)(((Uint16 *)pixels)[0]) / SDL_MAX_UINT16;
            v[1] = (float)(((Uint16 *)pixels)[1]) / SDL_MAX_UINT16;
            v[2] = (float)(((Uint16 *)pixels)[2]) / SDL_MAX_UINT16;
            if (fmt->bytes_per_pixel == 8) {
                v[3] = (float)(((Uint16 *)pixels)[3]) / SDL_MAX_UINT16;
            } else {
                v[3] = 1.0f;
            }
            break;
        case SDL_PIXELTYPE_ARRAYF16:
            v[0] = half_to_float(((Uint16 *)pixels)[0]);
            v[1] = half_to_float(((Uint16 *)pixels)[1]);
            v[2] = half_to_float(((Uint16 *)pixels)[2]);
            if (fmt->bytes_per_pixel == 8) {
                v[3] = half_to_float(((Uint16 *)pixels)[3]);
            } else {
                v[3] = 1.0f;
            }
            break;
        case SDL_PIXELTYPE_ARRAYF32:
            v[0] = ((float *)pixels)[0];
            v[1] = ((float *)pixels)[1];
            v[2] = ((float *)pixels)[2];
            if (fmt->bytes_per_pixel == 16) {
                v[3] = ((float *)pixels)[3];
            } else {
                v[3] = 1.0f;
            }
            break;
        default:
            // Unknown array type
            v[0] = v[1] = v[2] = v[3] = 0.0f;
            break;
        }
        switch (SDL_PIXELORDER(fmt->format)) {
        case SDL_ARRAYORDER_RGB:
            fR = v[0];
            fG = v[1];
            fB = v[2];
            fA = 1.0f;
            break;
        case SDL_ARRAYORDER_RGBA:
            fR = v[0];
            fG = v[1];
            fB = v[2];
            fA = v[3];
            break;
        case SDL_ARRAYORDER_ARGB:
            fA = v[0];
            fR = v[1];
            fG = v[2];
            fB = v[3];
            break;
        case SDL_ARRAYORDER_BGR:
            fB = v[0];
            fG = v[1];
            fR = v[2];
            fA = 1.0f;
            break;
        case SDL_ARRAYORDER_BGRA:
            fB = v[0];
            fG = v[1];
            fR = v[2];
            fA = v[3];
            break;
        case SDL_ARRAYORDER_ABGR:
            fA = v[0];
            fB = v[1];
            fG = v[2];
            fR = v[3];
            break;
        default:
            // Unknown array order
            fA = fR = fG = fB = 0.0f;
            break;
        }
        break;
    }

    // Convert to nits so src and dst are guaranteed to be linear and in the same units
    switch (SDL_COLORSPACETRANSFER(colorspace)) {
    case SDL_TRANSFER_CHARACTERISTICS_SRGB:
        fR = SDL_sRGBtoLinear(fR);
        fG = SDL_sRGBtoLinear(fG);
        fB = SDL_sRGBtoLinear(fB);
        break;
    case SDL_TRANSFER_CHARACTERISTICS_PQ:
        fR = SDL_PQtoNits(fR) / SDR_white_point;
        fG = SDL_PQtoNits(fG) / SDR_white_point;
        fB = SDL_PQtoNits(fB) / SDR_white_point;
        break;
    case SDL_TRANSFER_CHARACTERISTICS_LINEAR:
        fR /= SDR_white_point;
        fG /= SDR_white_point;
        fB /= SDR_white_point;
        break;
    default:
        // Unknown, leave it alone
        break;
    }

    *outR = fR;
    *outG = fG;
    *outB = fB;
    *outA = fA;
}

static void WriteFloatPixel(Uint8 *pixels, SlowBlitPixelAccess access, const SDL_PixelFormatDetails *fmt, SDL_Colorspace colorspace, float SDR_white_point,
                            float fR, float fG, float fB, float fA)
{
    Uint32 R, G, B, A;
    Uint32 pixelvalue;
    float v[4];

    // We converted to nits so src and dst are guaranteed to be linear and in the same units
    switch (SDL_COLORSPACETRANSFER(colorspace)) {
    case SDL_TRANSFER_CHARACTERISTICS_SRGB:
        fR = SDL_sRGBfromLinear(fR);
        fG = SDL_sRGBfromLinear(fG);
        fB = SDL_sRGBfromLinear(fB);
        break;
    case SDL_TRANSFER_CHARACTERISTICS_PQ:
        fR = SDL_PQfromNits(fR * SDR_white_point);
        fG = SDL_PQfromNits(fG * SDR_white_point);
        fB = SDL_PQfromNits(fB * SDR_white_point);
        break;
    case SDL_TRANSFER_CHARACTERISTICS_LINEAR:
        fR *= SDR_white_point;
        fG *= SDR_white_point;
        fB *= SDR_white_point;
        break;
    default:
        // Unknown, leave it alone
        break;
    }

    switch (access) {
    case SlowBlitPixelAccess_Index8:
        // This should never happen, checked before this call
        /* unreachable */
        break;
    case SlowBlitPixelAccess_RGB:
        R = (Uint8)SDL_roundf(SDL_clamp(fR, 0.0f, 1.0f) * 255.0f);
        G = (Uint8)SDL_roundf(SDL_clamp(fG, 0.0f, 1.0f) * 255.0f);
        B = (Uint8)SDL_roundf(SDL_clamp(fB, 0.0f, 1.0f) * 255.0f);
        ASSEMBLE_RGB(pixels, fmt->bytes_per_pixel, fmt, R, G, B);
        break;
    case SlowBlitPixelAccess_RGBA:
        R = (Uint8)SDL_roundf(SDL_clamp(fR, 0.0f, 1.0f) * 255.0f);
        G = (Uint8)SDL_roundf(SDL_clamp(fG, 0.0f, 1.0f) * 255.0f);
        B = (Uint8)SDL_roundf(SDL_clamp(fB, 0.0f, 1.0f) * 255.0f);
        A = (Uint8)SDL_roundf(SDL_clamp(fA, 0.0f, 1.0f) * 255.0f);
        ASSEMBLE_RGBA(pixels, fmt->bytes_per_pixel, fmt, R, G, B, A);
        break;
    case SlowBlitPixelAccess_10Bit:
    {
        switch (fmt->format) {
        case SDL_PIXELFORMAT_XRGB2101010:
            fA = 1.0f;
            SDL_FALLTHROUGH;
        case SDL_PIXELFORMAT_ARGB2101010:
            ARGB2101010_FROM_RGBAFLOAT(pixelvalue, fR, fG, fB, fA);
            break;
        case SDL_PIXELFORMAT_XBGR2101010:
            fA = 1.0f;
            SDL_FALLTHROUGH;
        case SDL_PIXELFORMAT_ABGR2101010:
            ABGR2101010_FROM_RGBAFLOAT(pixelvalue, fR, fG, fB, fA);
            break;
        default:
            pixelvalue = 0;
            break;
        }
        *(Uint32 *)pixels = pixelvalue;
        break;
    }
    case SlowBlitPixelAccess_Large:
        switch (SDL_PIXELORDER(fmt->format)) {
        case SDL_ARRAYORDER_RGB:
            v[0] = fR;
            v[1] = fG;
            v[2] = fB;
            v[3] = 1.0f;
            break;
        case SDL_ARRAYORDER_RGBA:
            v[0] = fR;
            v[1] = fG;
            v[2] = fB;
            v[3] = fA;
            break;
        case SDL_ARRAYORDER_ARGB:
            v[0] = fA;
            v[1] = fR;
            v[2] = fG;
            v[3] = fB;
            break;
        case SDL_ARRAYORDER_BGR:
            v[0] = fB;
            v[1] = fG;
            v[2] = fR;
            v[3] = 1.0f;
            break;
        case SDL_ARRAYORDER_BGRA:
            v[0] = fB;
            v[1] = fG;
            v[2] = fR;
            v[3] = fA;
            break;
        case SDL_ARRAYORDER_ABGR:
            v[0] = fA;
            v[1] = fB;
            v[2] = fG;
            v[3] = fR;
            break;
        default:
            // Unknown array order
            v[0] = v[1] = v[2] = v[3] = 0.0f;
            break;
        }
        switch (SDL_PIXELTYPE(fmt->format)) {
        case SDL_PIXELTYPE_ARRAYU16:
            ((Uint16 *)pixels)[0] = (Uint16)SDL_roundf(SDL_clamp(v[0], 0.0f, 1.0f) * SDL_MAX_UINT16);
            ((Uint16 *)pixels)[1] = (Uint16)SDL_roundf(SDL_clamp(v[1], 0.0f, 1.0f) * SDL_MAX_UINT16);
            ((Uint16 *)pixels)[2] = (Uint16)SDL_roundf(SDL_clamp(v[2], 0.0f, 1.0f) * SDL_MAX_UINT16);
            if (fmt->bytes_per_pixel == 8) {
                ((Uint16 *)pixels)[3] = (Uint16)SDL_roundf(SDL_clamp(v[3], 0.0f, 1.0f) * SDL_MAX_UINT16);
            }
            break;
        case SDL_PIXELTYPE_ARRAYF16:
            ((Uint16 *)pixels)[0] = float_to_half(v[0]);
            ((Uint16 *)pixels)[1] = float_to_half(v[1]);
            ((Uint16 *)pixels)[2] = float_to_half(v[2]);
            if (fmt->bytes_per_pixel == 8) {
                ((Uint16 *)pixels)[3] = float_to_half(v[3]);
            }
            break;
        case SDL_PIXELTYPE_ARRAYF32:
            ((float *)pixels)[0] = v[0];
            ((float *)pixels)[1] = v[1];
            ((float *)pixels)[2] = v[2];
            if (fmt->bytes_per_pixel == 16) {
                ((float *)pixels)[3] = v[3];
            }
            break;
        default:
            // Unknown array type
            break;
        }
        break;
    }
}

typedef enum
{
    SDL_TONEMAP_NONE,
    SDL_TONEMAP_LINEAR,
    SDL_TONEMAP_CHROME
} SDL_TonemapOperator;

typedef struct
{
    SDL_TonemapOperator op;

    union {
        struct {
            float scale;
        } linear;

        struct {
            float a;
            float b;
            const float *color_primaries_matrix;
        } chrome;

    } data;

} SDL_TonemapContext;

static void TonemapLinear(float *r, float *g, float *b, float scale)
{
    *r *= scale;
    *g *= scale;
    *b *= scale;
}

/* This uses the same tonemapping algorithm developed by Google for Chrome:
 * https://colab.research.google.com/drive/1hI10nq6L6ru_UFvz7-f7xQaQp0qarz_K
 *
 * Essentially, you use the source headroom and the destination headroom
 * to calculate scaling factors:
 *  tonemap_a = (dst_headroom / (src_headroom * src_headroom));
 *  tonemap_b = (1.0f / dst_headroom);
 *
 * Then you normalize your source color by the HDR whitepoint,
 * and calculate a final scaling factor in BT.2020 colorspace.
 */
static void TonemapChrome(float *r, float *g, float *b, float tonemap_a, float tonemap_b)
{
    float v1 = *r;
    float v2 = *g;
    float v3 = *b;
    float vmax = SDL_max(v1, SDL_max(v2, v3));

    if (vmax > 0.0f) {
        float scale = (1.0f + tonemap_a * vmax) / (1.0f + tonemap_b * vmax);
        TonemapLinear(r, g, b, scale);
    }
}

static void ApplyTonemap(SDL_TonemapContext *ctx, float *r, float *g, float *b)
{
    switch (ctx->op) {
    case SDL_TONEMAP_LINEAR:
        TonemapLinear(r, g, b, ctx->data.linear.scale);
        break;
    case SDL_TONEMAP_CHROME:
        if (ctx->data.chrome.color_primaries_matrix) {
            SDL_ConvertColorPrimaries(r, g, b, ctx->data.chrome.color_primaries_matrix);
        }
        TonemapChrome(r, g, b, ctx->data.chrome.a, ctx->data.chrome.b);
        break;
    default:
        break;
    }
}

/* The SECOND TRUE BLITTER
 * This one is even slower than the first, but also handles large pixel formats and colorspace conversion
 */
void SDLOP_BlitSlowFloat(const SDLOP_BlitInfo *blit, SDL_Surface *src_surface, const Uint8 *src_pixels, int src_pitch,
                         int src_w, int src_h, SDL_Surface *dst_surface, Uint8 *dst_pixels, int dst_pitch, int dst_w, int dst_h)
{
    FloatBlitInfo info_storage;
    FloatBlitInfo *info = &info_storage;
    info->flags = blit->flags;
    info->r = blit->r;
    info->g = blit->g;
    info->b = blit->b;
    info->a = blit->a;
    info->src_surface = src_surface;
    info->dst_surface = dst_surface;
    info->src_fmt = SI(src_surface)->fmt;
    info->dst_fmt = SI(dst_surface)->fmt;
    info->src_pal = SI(src_surface)->palette;
    info->dst_pal = SI(dst_surface)->palette;
    info->src = (Uint8 *)src_pixels;
    info->src_pitch = src_pitch;
    info->src_w = src_w;
    info->src_h = src_h;
    info->dst = dst_pixels;
    info->dst_pitch = dst_pitch;
    info->dst_w = dst_w;
    info->dst_h = dst_h;

    const int flags = info->flags;
    const Uint32 modulateR = info->r;
    const Uint32 modulateG = info->g;
    const Uint32 modulateB = info->b;
    const Uint32 modulateA = info->a;
    float srcR, srcG, srcB, srcA;
    float dstR, dstG, dstB, dstA;
    Uint64 srcy, srcx;
    Uint64 posy, posx;
    Uint64 incy, incx;
    const SDL_PixelFormatDetails *src_fmt = info->src_fmt;
    const SDL_Palette *src_pal = info->src_pal;
    const SDL_PixelFormatDetails *dst_fmt = info->dst_fmt;
    const SDL_Palette *dst_pal = info->dst_pal;
    int srcbpp = src_fmt->bytes_per_pixel;
    int dstbpp = dst_fmt->bytes_per_pixel;
    SlowBlitPixelAccess src_access;
    SlowBlitPixelAccess dst_access;
    SDL_Colorspace src_colorspace;
    SDL_Colorspace dst_colorspace;
    SDL_ColorPrimaries src_primaries;
    SDL_ColorPrimaries dst_primaries;
    const float *color_primaries_matrix = NULL;
    float src_white_point;
    float dst_white_point;
    float dst_headroom;
    float src_headroom;
    SDL_TonemapContext tonemap;
    Uint32 last_pixel = 0;
    Uint8 last_index = 0;

    src_colorspace = SI(info->src_surface)->colorspace;
    dst_colorspace = SI(info->dst_surface)->colorspace;
    src_primaries = SDL_COLORSPACEPRIMARIES(src_colorspace);
    dst_primaries = SDL_COLORSPACEPRIMARIES(dst_colorspace);

    src_white_point = SDL_GetSurfaceSDRWhitePoint(info->src_surface, src_colorspace);
    dst_white_point = SDL_GetSurfaceSDRWhitePoint(info->dst_surface, dst_colorspace);
    src_headroom = SDL_GetSurfaceHDRHeadroom(info->src_surface, src_colorspace);
    dst_headroom = SDL_GetSurfaceHDRHeadroom(info->dst_surface, dst_colorspace);
    if (dst_headroom == 0.0f) {
        // The destination will have the same headroom as the source
        dst_headroom = src_headroom;
        SDL_SetFloatProperty(SDL_GetSurfaceProperties(info->dst_surface), SDL_PROP_SURFACE_HDR_HEADROOM_FLOAT, dst_headroom);
    }

    SDL_zero(tonemap);

    if (src_headroom > dst_headroom) {
        const char *tonemap_operator = SDL_GetStringProperty(SDL_GetSurfaceProperties(info->src_surface), SDL_PROP_SURFACE_TONEMAP_OPERATOR_STRING, NULL);
        if (tonemap_operator) {
            if (SDL_strncmp(tonemap_operator, "*=", 2) == 0) {
                tonemap.op = SDL_TONEMAP_LINEAR;
                tonemap.data.linear.scale = (float)SDL_atof(tonemap_operator + 2);
            } else if (SDL_strcasecmp(tonemap_operator, "chrome") == 0) {
                tonemap.op = SDL_TONEMAP_CHROME;
            } else if (SDL_strcasecmp(tonemap_operator, "none") == 0) {
                tonemap.op = SDL_TONEMAP_NONE;
            }
        } else {
            tonemap.op = SDL_TONEMAP_CHROME;
        }
        if (tonemap.op == SDL_TONEMAP_CHROME) {
            tonemap.data.chrome.a = (dst_headroom / (src_headroom * src_headroom));
            tonemap.data.chrome.b = (1.0f / dst_headroom);

            // We'll convert to BT.2020 primaries for the tonemap operation
            tonemap.data.chrome.color_primaries_matrix = SDL_GetColorPrimariesConversionMatrix(src_primaries, SDL_COLOR_PRIMARIES_BT2020);
            if (tonemap.data.chrome.color_primaries_matrix) {
                src_primaries = SDL_COLOR_PRIMARIES_BT2020;
            }
        }
    }

    if (src_primaries != dst_primaries) {
        color_primaries_matrix = SDL_GetColorPrimariesConversionMatrix(src_primaries, dst_primaries);
    }

    src_access = GetPixelAccessMethod(src_fmt->format);
    dst_access = GetPixelAccessMethod(dst_fmt->format);
    if (dst_access == SlowBlitPixelAccess_Index8) {
        last_index = LookupRGBAColor(last_pixel, dst_pal);
    }

    incy = ((Uint64)info->src_h << 16) / info->dst_h;
    incx = ((Uint64)info->src_w << 16) / info->dst_w;
    posy = incy / 2; // start at the middle of pixel

    while (info->dst_h--) {
        Uint8 *src = NULL;
        Uint8 *dst = info->dst;
        int n = info->dst_w;
        posx = incx / 2; // start at the middle of pixel
        srcy = posy >> 16;
        while (n--) {
            srcx = posx >> 16;
            src = (info->src + (srcy * info->src_pitch) + (srcx * srcbpp));

            ReadFloatPixel(src, src_access, src_fmt, src_pal, src_colorspace, src_white_point, &srcR, &srcG, &srcB, &srcA);

            if (tonemap.op) {
                ApplyTonemap(&tonemap, &srcR, &srcG, &srcB);
            }

            if (color_primaries_matrix) {
                SDL_ConvertColorPrimaries(&srcR, &srcG, &srcB, color_primaries_matrix);
            }

            if (flags & SDL_COPY_COLORKEY) {
                // colorkey isn't supported
            }
            if ((flags & (SDL_COPY_BLEND | SDL_COPY_ADD | SDL_COPY_MOD | SDL_COPY_MUL))) {
                ReadFloatPixel(dst, dst_access, dst_fmt, dst_pal, dst_colorspace, dst_white_point, &dstR, &dstG, &dstB, &dstA);
            } else {
                // don't care
                dstR = dstG = dstB = dstA = 0.0f;
            }

            if (flags & SDL_COPY_MODULATE_COLOR) {
                srcR = (srcR * modulateR) / 255;
                srcG = (srcG * modulateG) / 255;
                srcB = (srcB * modulateB) / 255;
            }
            if (flags & SDL_COPY_MODULATE_ALPHA) {
                srcA = (srcA * modulateA) / 255;
            }
            if (flags & (SDL_COPY_BLEND | SDL_COPY_ADD)) {
                if (srcA < 1.0f) {
                    srcR = (srcR * srcA);
                    srcG = (srcG * srcA);
                    srcB = (srcB * srcA);
                }
            }
            switch (flags & (SDL_COPY_BLEND | SDL_COPY_ADD | SDL_COPY_MOD | SDL_COPY_MUL)) {
            case 0:
                dstR = srcR;
                dstG = srcG;
                dstB = srcB;
                dstA = srcA;
                break;
            case SDL_COPY_BLEND:
                dstR = srcR + ((1.0f - srcA) * dstR);
                dstG = srcG + ((1.0f - srcA) * dstG);
                dstB = srcB + ((1.0f - srcA) * dstB);
                dstA = srcA + ((1.0f - srcA) * dstA);
                break;
            case SDL_COPY_ADD:
                dstR = srcR + dstR;
                dstG = srcG + dstG;
                dstB = srcB + dstB;
                break;
            case SDL_COPY_MOD:
                dstR = (srcR * dstR);
                dstG = (srcG * dstG);
                dstB = (srcB * dstB);
                break;
            case SDL_COPY_MUL:
                dstR = ((srcR * dstR) + (dstR * (1.0f - srcA)));
                dstG = ((srcG * dstG) + (dstG * (1.0f - srcA)));
                dstB = ((srcB * dstB) + (dstB * (1.0f - srcA)));
                break;
            }

            if (dst_access == SlowBlitPixelAccess_Index8) {
                Uint32 R = (Uint8)SDL_roundf(SDL_clamp(SDL_sRGBfromLinear(dstR), 0.0f, 1.0f) * 255.0f);
                Uint32 G = (Uint8)SDL_roundf(SDL_clamp(SDL_sRGBfromLinear(dstG), 0.0f, 1.0f) * 255.0f);
                Uint32 B = (Uint8)SDL_roundf(SDL_clamp(SDL_sRGBfromLinear(dstB), 0.0f, 1.0f) * 255.0f);
                Uint32 A = (Uint8)SDL_roundf(SDL_clamp(dstA, 0.0f, 1.0f) * 255.0f);
                Uint32 dstpixel = ((R << 24) | (G << 16) | (B << 8) | A);
                if (dstpixel != last_pixel) {
                    last_pixel = dstpixel;
                    last_index = LookupRGBAColor(dstpixel, dst_pal);
                }
                *dst = last_index;
            } else {
                WriteFloatPixel(dst, dst_access, dst_fmt, dst_colorspace, dst_white_point, dstR, dstG, dstB, dstA);
            }

            posx += incx;
            dst += dstbpp;
        }
        posy += incy;
        info->dst += info->dst_pitch;
    }
}
