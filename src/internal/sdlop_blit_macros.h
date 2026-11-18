/*
  SDLop - pixel load/store macros from SDL3's src/video/SDL_blit.h
  (release-3.4.20), shared by the ported blitters.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#ifndef SDLOP_BLIT_MACROS_H
#define SDLOP_BLIT_MACROS_H

#include "sdlop_pixels.h"

// Load pixel of the specified format from a buffer and get its R-G-B values
#define RGB_FROM_PIXEL(Pixel, fmt, r, g, b)                                     \
    {                                                                           \
        r = SDLOP_expand_byte[fmt->Rbits][((Pixel & fmt->Rmask) >> fmt->Rshift)]; \
        g = SDLOP_expand_byte[fmt->Gbits][((Pixel & fmt->Gmask) >> fmt->Gshift)]; \
        b = SDLOP_expand_byte[fmt->Bbits][((Pixel & fmt->Bmask) >> fmt->Bshift)]; \
    }
#define RGB_FROM_RGB565(Pixel, r, g, b)                   \
    {                                                     \
        r = SDLOP_expand_byte[5][((Pixel & 0xF800) >> 11)]; \
        g = SDLOP_expand_byte[6][((Pixel & 0x07E0) >> 5)];  \
        b = SDLOP_expand_byte[5][(Pixel & 0x001F)];         \
    }
#define RGB_FROM_RGB555(Pixel, r, g, b)                   \
    {                                                     \
        r = SDLOP_expand_byte[5][((Pixel & 0x7C00) >> 10)]; \
        g = SDLOP_expand_byte[5][((Pixel & 0x03E0) >> 5)];  \
        b = SDLOP_expand_byte[5][(Pixel & 0x001F)];         \
    }
#define RGB_FROM_XRGB8888(Pixel, r, g, b) \
    {                                   \
        r = ((Pixel & 0xFF0000) >> 16); \
        g = ((Pixel & 0xFF00) >> 8);    \
        b = (Pixel & 0xFF);             \
    }
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
#define GET_RGB24(B) (B[2] << 16) | (B[1] << 8) | B[0]
#else
#define GET_RGB24(B) (B[0] << 16) | (B[1] << 8) | B[2]
#endif
#define RETRIEVE_RGB_PIXEL(buf, bpp, Pixel)                \
    do {                                                   \
        switch (bpp) {                                     \
        case 1:                                            \
            Pixel = *((Uint8 *)(buf));                     \
            break;                                         \
                                                           \
        case 2:                                            \
            Pixel = *((Uint16 *)(buf));                    \
            break;                                         \
                                                           \
        case 3:                                            \
        {                                                  \
            Uint8 *B = (Uint8 *)(buf);                     \
            Pixel = GET_RGB24(B);                          \
        } break;                                           \
                                                           \
        case 4:                                            \
            Pixel = *((Uint32 *)(buf));                    \
            break;                                         \
                                                           \
        default:                                           \
            Pixel = 0; /* stop gcc complaints */           \
            break;                                         \
        }                                                  \
    } while (0)

#if SDL_BYTEORDER == SDL_LIL_ENDIAN
#define GET_RGB24_COMPONENT(buf, fmt, shift) *((buf) + fmt->shift / 8)
#else
#define GET_RGB24_COMPONENT(buf, fmt, shift) *((buf) + 2 - fmt->shift / 8)
#endif
#define DISEMBLE_RGB(buf, bpp, fmt, Pixel, r, g, b)    \
    do {                                               \
        switch (bpp) {                                 \
        case 1:                                        \
            Pixel = *((Uint8 *)(buf));                 \
            RGB_FROM_PIXEL(Pixel, fmt, r, g, b);       \
            break;                                     \
                                                       \
        case 2:                                        \
            Pixel = *((Uint16 *)(buf));                \
            RGB_FROM_PIXEL(Pixel, fmt, r, g, b);       \
            break;                                     \
                                                       \
        case 3:                                        \
        {                                              \
            Pixel = 0;                                 \
            r = GET_RGB24_COMPONENT(buf, fmt, Rshift); \
            g = GET_RGB24_COMPONENT(buf, fmt, Gshift); \
            b = GET_RGB24_COMPONENT(buf, fmt, Bshift); \
        } break;                                       \
                                                       \
        case 4:                                        \
            Pixel = *((Uint32 *)(buf));                \
            RGB_FROM_PIXEL(Pixel, fmt, r, g, b);       \
            break;                                     \
                                                       \
        default:                                       \
            /* stop gcc complaints */                  \
            Pixel = 0;                                 \
            r = g = b = 0;                             \
            break;                                     \
        }                                              \
    } while (0)

// Assemble R-G-B values into a specified pixel format and store them
#define PIXEL_FROM_RGB(Pixel, fmt, r, g, b)                 \
    {                                                       \
        Pixel = ((r >> (8 - fmt->Rbits)) << fmt->Rshift) |  \
                ((g >> (8 - fmt->Gbits)) << fmt->Gshift) |  \
                ((b >> (8 - fmt->Bbits)) << fmt->Bshift) |  \
                fmt->Amask;                                 \
    }
#define RGB332_FROM_RGB(Pixel, r, g, b)                        \
    {                                                          \
        Pixel = (Uint8)(((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6)); \
    }
#define RGB565_FROM_RGB(Pixel, r, g, b)                        \
    {                                                          \
        Pixel = (Uint16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); \
    }
#define RGB555_FROM_RGB(Pixel, r, g, b)                        \
    {                                                          \
        Pixel = (Uint16)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)); \
    }
#define XRGB8888_FROM_RGB(Pixel, r, g, b)   \
    {                                     \
        Pixel = (r << 16) | (g << 8) | b; \
    }
#define ARGB8888_FROM_RGBA(Pixel, r, g, b, a)         \
    {                                                 \
        Pixel = (a << 24) | (r << 16) | (g << 8) | b; \
    }
#define RGBA8888_FROM_RGBA(Pixel, r, g, b, a)         \
    {                                                 \
        Pixel = (r << 24) | (g << 16) | (b << 8) | a; \
    }
#define ABGR8888_FROM_RGBA(Pixel, r, g, b, a)         \
    {                                                 \
        Pixel = (a << 24) | (b << 16) | (g << 8) | r; \
    }
#define BGRA8888_FROM_RGBA(Pixel, r, g, b, a)         \
    {                                                 \
        Pixel = (b << 24) | (g << 16) | (r << 8) | a; \
    }
#define ARGB2101010_FROM_RGBA(Pixel, r, g, b, a)       \
    {                                                  \
        r = r ? ((r << 2) | 0x3) : 0;                  \
        g = g ? ((g << 2) | 0x3) : 0;                  \
        b = b ? ((b << 2) | 0x3) : 0;                  \
        a = (a * 3) / 255;                             \
        Pixel = (a << 30) | (r << 20) | (g << 10) | b; \
    }
#define ARGB2101010_FROM_RGBAFLOAT(Pixel, r, g, b, a) \
    {                                                 \
        r = SDL_clamp(r, 0.0f, 1.0f) * 1023.0f;       \
        g = SDL_clamp(g, 0.0f, 1.0f) * 1023.0f;       \
        b = SDL_clamp(b, 0.0f, 1.0f) * 1023.0f;       \
        a = SDL_clamp(a, 0.0f, 1.0f) * 3.0f;          \
        Pixel = (((Uint32)SDL_roundf(a)) << 30) |     \
                (((Uint32)SDL_roundf(r)) << 20) |     \
                (((Uint32)SDL_roundf(g)) << 10) |     \
                (Uint32)SDL_roundf(b);                \
    }
#define ABGR2101010_FROM_RGBA(Pixel, r, g, b, a)       \
    {                                                  \
        r = r ? ((r << 2) | 0x3) : 0;                  \
        g = g ? ((g << 2) | 0x3) : 0;                  \
        b = b ? ((b << 2) | 0x3) : 0;                  \
        a = (a * 3) / 255;                             \
        Pixel = (a << 30) | (b << 20) | (g << 10) | r; \
    }
#define ABGR2101010_FROM_RGBAFLOAT(Pixel, r, g, b, a) \
    {                                                 \
        r = SDL_clamp(r, 0.0f, 1.0f) * 1023.0f;       \
        g = SDL_clamp(g, 0.0f, 1.0f) * 1023.0f;       \
        b = SDL_clamp(b, 0.0f, 1.0f) * 1023.0f;       \
        a = SDL_clamp(a, 0.0f, 1.0f) * 3.0f;          \
        Pixel = (((Uint32)SDL_roundf(a)) << 30) |     \
                (((Uint32)SDL_roundf(b)) << 20) |     \
                (((Uint32)SDL_roundf(g)) << 10) |     \
                (Uint32)SDL_roundf(r);                \
    }
#define ASSEMBLE_RGB(buf, bpp, fmt, r, g, b)           \
    {                                                  \
        switch (bpp) {                                 \
        case 1:                                        \
        {                                              \
            Uint8 _pixel;                              \
                                                       \
            PIXEL_FROM_RGB(_pixel, fmt, r, g, b);      \
            *((Uint8 *)(buf)) = _pixel;                \
        } break;                                       \
                                                       \
        case 2:                                        \
        {                                              \
            Uint16 _pixel;                             \
                                                       \
            PIXEL_FROM_RGB(_pixel, fmt, r, g, b);      \
            *((Uint16 *)(buf)) = _pixel;               \
        } break;                                       \
                                                       \
        case 3:                                        \
        {                                              \
            GET_RGB24_COMPONENT(buf, fmt, Rshift) = r; \
            GET_RGB24_COMPONENT(buf, fmt, Gshift) = g; \
            GET_RGB24_COMPONENT(buf, fmt, Bshift) = b; \
        } break;                                       \
                                                       \
        case 4:                                        \
        {                                              \
            Uint32 _pixel;                             \
                                                       \
            PIXEL_FROM_RGB(_pixel, fmt, r, g, b);      \
            *((Uint32 *)(buf)) = _pixel;               \
        } break;                                       \
        }                                              \
    }

// FIXME: Should we rescale alpha into 0..255 here?
#define RGBA_FROM_PIXEL(Pixel, fmt, r, g, b, a)                                 \
    {                                                                           \
        r = SDLOP_expand_byte[fmt->Rbits][((Pixel & fmt->Rmask) >> fmt->Rshift)]; \
        g = SDLOP_expand_byte[fmt->Gbits][((Pixel & fmt->Gmask) >> fmt->Gshift)]; \
        b = SDLOP_expand_byte[fmt->Bbits][((Pixel & fmt->Bmask) >> fmt->Bshift)]; \
        a = SDLOP_expand_byte[fmt->Abits][((Pixel & fmt->Amask) >> fmt->Ashift)]; \
    }
#define RGBA_FROM_8888(Pixel, fmt, r, g, b, a)   \
    {                                            \
        r = (Pixel & fmt->Rmask) >> fmt->Rshift; \
        g = (Pixel & fmt->Gmask) >> fmt->Gshift; \
        b = (Pixel & fmt->Bmask) >> fmt->Bshift; \
        a = (Pixel & fmt->Amask) >> fmt->Ashift; \
    }
#define RGBA_FROM_RGBA8888(Pixel, r, g, b, a) \
    {                                         \
        r = (Pixel >> 24);                    \
        g = ((Pixel >> 16) & 0xFF);           \
        b = ((Pixel >> 8) & 0xFF);            \
        a = (Pixel & 0xFF);                   \
    }
#define RGBA_FROM_ARGB8888(Pixel, r, g, b, a) \
    {                                         \
        r = ((Pixel >> 16) & 0xFF);           \
        g = ((Pixel >> 8) & 0xFF);            \
        b = (Pixel & 0xFF);                   \
        a = (Pixel >> 24);                    \
    }
#define RGBA_FROM_ABGR8888(Pixel, r, g, b, a) \
    {                                         \
        r = (Pixel & 0xFF);                   \
        g = ((Pixel >> 8) & 0xFF);            \
        b = ((Pixel >> 16) & 0xFF);           \
        a = (Pixel >> 24);                    \
    }
#define RGBA_FROM_BGRA8888(Pixel, r, g, b, a) \
    {                                         \
        r = ((Pixel >> 8) & 0xFF);            \
        g = ((Pixel >> 16) & 0xFF);           \
        b = (Pixel >> 24);                    \
        a = (Pixel & 0xFF);                   \
    }
#define RGBA_FROM_ARGB2101010(Pixel, r, g, b, a) \
    {                                            \
        r = ((Pixel >> 22) & 0xFF);              \
        g = ((Pixel >> 12) & 0xFF);              \
        b = ((Pixel >> 2) & 0xFF);               \
        a = SDLOP_expand_byte[2][(Pixel >> 30)];   \
    }
#define RGBAFLOAT_FROM_ARGB2101010(Pixel, r, g, b, a)   \
    {                                                   \
        r = (float)((Pixel >> 20) & 0x3FF) / 1023.0f;   \
        g = (float)((Pixel >> 10) & 0x3FF) / 1023.0f;   \
        b = (float)((Pixel >> 0) & 0x3FF) / 1023.0f;    \
        a = (float)(Pixel >> 30) / 3.0f;                \
    }
#define RGBA_FROM_ABGR2101010(Pixel, r, g, b, a) \
    {                                            \
        r = ((Pixel >> 2) & 0xFF);               \
        g = ((Pixel >> 12) & 0xFF);              \
        b = ((Pixel >> 22) & 0xFF);              \
        a = SDLOP_expand_byte[2][(Pixel >> 30)];   \
    }
#define RGBAFLOAT_FROM_ABGR2101010(Pixel, r, g, b, a)   \
    {                                                   \
        r = (float)((Pixel >> 0) & 0x3FF) / 1023.0f;    \
        g = (float)((Pixel >> 10) & 0x3FF) / 1023.0f;   \
        b = (float)((Pixel >> 20) & 0x3FF) / 1023.0f;   \
        a = (float)(Pixel >> 30) / 3.0f;                \
    }
#define DISEMBLE_RGBA(buf, bpp, fmt, Pixel, r, g, b, a) \
    do {                                                \
        switch (bpp) {                                  \
        case 1:                                         \
            Pixel = *((Uint8 *)(buf));                  \
            RGBA_FROM_PIXEL(Pixel, fmt, r, g, b, a);    \
            break;                                      \
                                                        \
        case 2:                                         \
            Pixel = *((Uint16 *)(buf));                 \
            RGBA_FROM_PIXEL(Pixel, fmt, r, g, b, a);    \
            break;                                      \
                                                        \
        case 3:                                         \
        {                                               \
            Pixel = 0;                                  \
            r = GET_RGB24_COMPONENT(buf, fmt, Rshift);  \
            g = GET_RGB24_COMPONENT(buf, fmt, Gshift);  \
            b = GET_RGB24_COMPONENT(buf, fmt, Bshift);  \
            a = 0xFF;                                   \
        } break;                                        \
                                                        \
        case 4:                                         \
            Pixel = *((Uint32 *)(buf));                 \
            RGBA_FROM_PIXEL(Pixel, fmt, r, g, b, a);    \
            break;                                      \
                                                        \
        default:                                        \
            /* stop gcc complaints */                   \
            Pixel = 0;                                  \
            r = g = b = a = 0;                          \
            break;                                      \
        }                                               \
    } while (0)

// FIXME: this isn't correct, especially for Alpha (maximum != 255)
#define PIXEL_FROM_RGBA(Pixel, fmt, r, g, b, a)             \
    {                                                       \
        Pixel = ((r >> (8 - fmt->Rbits)) << fmt->Rshift) |  \
                ((g >> (8 - fmt->Gbits)) << fmt->Gshift) |  \
                ((b >> (8 - fmt->Bbits)) << fmt->Bshift) |  \
                ((a >> (8 - fmt->Abits)) << fmt->Ashift);   \
    }
#define ASSEMBLE_RGBA(buf, bpp, fmt, r, g, b, a)       \
    {                                                  \
        switch (bpp) {                                 \
        case 1:                                        \
        {                                              \
            Uint8 _pixel;                              \
                                                       \
            PIXEL_FROM_RGBA(_pixel, fmt, r, g, b, a);  \
            *((Uint8 *)(buf)) = _pixel;                \
        } break;                                       \
                                                       \
        case 2:                                        \
        {                                              \
            Uint16 _pixel;                             \
                                                       \
            PIXEL_FROM_RGBA(_pixel, fmt, r, g, b, a);  \
            *((Uint16 *)(buf)) = _pixel;               \
        } break;                                       \
                                                       \
        case 3:                                        \
        {                                              \
            GET_RGB24_COMPONENT(buf, fmt, Rshift) = r; \
            GET_RGB24_COMPONENT(buf, fmt, Gshift) = g; \
            GET_RGB24_COMPONENT(buf, fmt, Bshift) = b; \
        } break;                                       \
                                                       \
        case 4:                                        \
        {                                              \
            Uint32 _pixel;                             \
                                                       \
            PIXEL_FROM_RGBA(_pixel, fmt, r, g, b, a);  \
            *((Uint32 *)(buf)) = _pixel;               \
        } break;                                       \
        }                                              \
    }


#endif
