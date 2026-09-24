/*
  SDLop -- SDL_surface.h and SDL_pixels.h.

  A surface is a rectangle of pixels with a pitch and a format. SDLop ships
  only what the window-surface path needs: create/destroy, lock/unlock, fill and
  the pixel format helpers. Blitting, scaling, converting and the renderer are
  not part of the SDLop build (see tools/dropped.txt).

  The two functions worth noting are SDL_MapRGBA()/SDL_GetRGBA(): they are the
  primitives every software renderer needs, and implementing them from the
  format's shifts and masks (rather than a table of 65 hand written cases) keeps
  them correct for all 65 SDL3 pixel formats at once.
*/

#include "../sdlop_internal.h"
#include "sdlop_pixelformats.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* SDL_pixels.h                                                              */
/* ------------------------------------------------------------------------- */

const char *SDL_GetPixelFormatName(SDL_PixelFormat format)
{
    for (int i = 0; i < SDLOP_NUM_PIXEL_FORMATS; i++) {
        if (sdlop_pixel_formats[i].format == format) {
            return sdlop_pixel_formats[i].name;
        }
    }
    return "SDL_PIXELFORMAT_UNKNOWN";
}

const SDLOP_PixelFormatInfo *SDLOP_GetPixelFormatInfo(SDL_PixelFormat format)
{
    for (int i = 0; i < SDLOP_NUM_PIXEL_FORMATS; i++) {
        if (sdlop_pixel_formats[i].format == format) {
            return &sdlop_pixel_formats[i];
        }
    }
    return NULL;
}

const SDL_PixelFormatDetails *SDL_GetPixelFormatDetails(SDL_PixelFormat format)
{
    static SDL_PixelFormatDetails details;
    const SDLOP_PixelFormatInfo *info = SDLOP_GetPixelFormatInfo(format);

    if (!info) {
        SDL_SetError("Unknown pixel format");
        return NULL;
    }
    memset(&details, 0, sizeof(details));
    details.format = format;
    details.bits_per_pixel = info->bits;
    details.bytes_per_pixel = info->bytes;
    details.Rmask = info->rmask;
    details.Gmask = info->gmask;
    details.Bmask = info->bmask;
    details.Amask = info->amask;
    {
        Uint32 mask = info->rmask;
        int n = 0;
        while (mask && !(mask & 1u)) { mask >>= 1; n++; }
        details.Rshift = (Uint8)n;
        while (mask & 1u) { mask >>= 1; details.Rbits++; }
    }
    {
        Uint32 mask = info->gmask;
        int n = 0;
        while (mask && !(mask & 1u)) { mask >>= 1; n++; }
        details.Gshift = (Uint8)n;
        while (mask & 1u) { mask >>= 1; details.Gbits++; }
    }
    {
        Uint32 mask = info->bmask;
        int n = 0;
        while (mask && !(mask & 1u)) { mask >>= 1; n++; }
        details.Bshift = (Uint8)n;
        while (mask & 1u) { mask >>= 1; details.Bbits++; }
    }
    {
        Uint32 mask = info->amask;
        int n = 0;
        while (mask && !(mask & 1u)) { mask >>= 1; n++; }
        details.Ashift = (Uint8)n;
        while (mask & 1u) { mask >>= 1; details.Abits++; }
    }
    return &details;
}

bool SDL_GetMasksForPixelFormat(SDL_PixelFormat format, int *bpp, Uint32 *Rmask, Uint32 *Gmask,
                                Uint32 *Bmask, Uint32 *Amask)
{
    const SDLOP_PixelFormatInfo *info = SDLOP_GetPixelFormatInfo(format);
    if (!info || !info->implemented) {
        return SDL_SetError("Unsupported pixel format '%s'", SDL_GetPixelFormatName(format));
    }
    if (bpp) {
        *bpp = info->bits;
    }
    if (Rmask) {
        *Rmask = info->rmask;
    }
    if (Gmask) {
        *Gmask = info->gmask;
    }
    if (Bmask) {
        *Bmask = info->bmask;
    }
    if (Amask) {
        *Amask = info->amask;
    }
    return true;
}

SDL_PixelFormat SDL_GetPixelFormatForMasks(int bpp, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask,
                                           Uint32 Amask)
{
    for (int i = 0; i < SDLOP_NUM_PIXEL_FORMATS; i++) {
        const SDLOP_PixelFormatInfo *info = &sdlop_pixel_formats[i];
        if (info->bits == bpp && info->rmask == Rmask && info->gmask == Gmask &&
            info->bmask == Bmask && info->amask == Amask) {
            return info->format;
        }
    }
    SDL_SetError("No pixel format with those masks");
    return SDL_PIXELFORMAT_UNKNOWN;
}

Uint32 SDL_MapRGB(const SDL_PixelFormatDetails *format, const SDL_Palette *palette, Uint8 r,
                  Uint8 g, Uint8 b)
{
    (void)palette;
    if (!format) {
        return 0;
    }
    return (((Uint32)r * ((format->Rmask >> format->Rshift) + 1) / 256) << format->Rshift) |
           (((Uint32)g * ((format->Gmask >> format->Gshift) + 1) / 256) << format->Gshift) |
           (((Uint32)b * ((format->Bmask >> format->Bshift) + 1) / 256) << format->Bshift);
}

Uint32 SDL_MapRGBA(const SDL_PixelFormatDetails *format, const SDL_Palette *palette, Uint8 r,
                   Uint8 g, Uint8 b, Uint8 a)
{
    Uint32 pixel;
    (void)palette;
    if (!format) {
        return 0;
    }
    pixel = (((Uint32)r * ((format->Rmask >> format->Rshift) + 1) / 256) << format->Rshift) |
            (((Uint32)g * ((format->Gmask >> format->Gshift) + 1) / 256) << format->Gshift) |
            (((Uint32)b * ((format->Bmask >> format->Bshift) + 1) / 256) << format->Bshift);
    if (format->Amask) {
        pixel |= ((Uint32)a * ((format->Amask >> format->Ashift) + 1) / 256) << format->Ashift;
    }
    return pixel;
}

void SDL_GetRGB(Uint32 pixel, const SDL_PixelFormatDetails *format, const SDL_Palette *palette,
                Uint8 *r, Uint8 *g, Uint8 *b)
{
    SDL_GetRGBA(pixel, format, palette, r, g, b, NULL);
}

void SDL_GetRGBA(Uint32 pixel, const SDL_PixelFormatDetails *format, const SDL_Palette *palette,
                 Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a)
{
    Uint32 v;
    (void)palette;
    if (!format) {
        if (r) *r = 0;
        if (g) *g = 0;
        if (b) *b = 0;
        if (a) *a = 255;
        return;
    }
    v = (pixel & format->Rmask) >> format->Rshift;
    if (r) {
        *r = format->Rbits >= 8 ? (Uint8)(v >> (format->Rbits - 8))
                                : (Uint8)(v * 255 / ((1u << format->Rbits) - 1));
    }
    v = (pixel & format->Gmask) >> format->Gshift;
    if (g) {
        *g = format->Gbits >= 8 ? (Uint8)(v >> (format->Gbits - 8))
                                : (Uint8)(v * 255 / ((1u << format->Gbits) - 1));
    }
    v = (pixel & format->Bmask) >> format->Bshift;
    if (b) {
        *b = format->Bbits >= 8 ? (Uint8)(v >> (format->Bbits - 8))
                                : (Uint8)(v * 255 / ((1u << format->Bbits) - 1));
    }
    if (a) {
        if (format->Amask) {
            v = (pixel & format->Amask) >> format->Ashift;
            *a = format->Abits >= 8 ? (Uint8)(v >> (format->Abits - 8))
                                    : (Uint8)(v * 255 / ((1u << format->Abits) - 1));
        } else {
            *a = 255;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* SDL_surface.h                                                             */
/* ------------------------------------------------------------------------- */

/* Upstream 3.2.10's SDL_Surface ends with `void *reserved`, so the two pieces of
   per-surface state SDLop needs beyond upstream's fields live there. */
typedef struct SDLOP_SurfaceExtra
{
    SDL_PropertiesID props;
    SDL_Colorspace colorspace;
    bool owns_pixels;      /* the pixel buffer was allocated here, so it can grow */
} SDLOP_SurfaceExtra;

static SDLOP_SurfaceExtra *sdlop_surface_extra(SDL_Surface *surface, bool create)
{
    SDLOP_SurfaceExtra *extra = (SDLOP_SurfaceExtra *)surface->reserved;
    if (!extra && create) {
        extra = (SDLOP_SurfaceExtra *)SDLOP_Calloc(1, sizeof(*extra));
        if (!extra) {
            SDL_OutOfMemory();
            return NULL;
        }
        extra->colorspace = SDL_COLORSPACE_SRGB;
        surface->reserved = extra;
    }
    return extra;
}

SDL_Surface *SDL_CreateSurface(int width, int height, SDL_PixelFormat format)
{
    return SDL_CreateSurfaceFrom(width, height, format, NULL, 0);
}

/*----------------------------------------------------------------------------*/
/* Colour mapping                                                             */
/*----------------------------------------------------------------------------*/

Uint32 SDL_MapSurfaceRGB(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b)
{
    return SDL_MapSurfaceRGBA(surface, r, g, b, 255);
}

Uint32 SDL_MapSurfaceRGBA(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    const SDL_PixelFormatDetails *details;

    if (!surface) {
        SDL_InvalidParamError("surface");
        return 0;
    }
    details = SDL_GetPixelFormatDetails(surface->format);
    if (!details) {
        return 0;
    }
    /* SDLop has no palettes: every supported format is direct colour, so the
       NULL palette SDL_MapRGBA() expects for indexed formats is never needed. */
    return SDL_MapRGBA(details, NULL, r, g, b, a);
}

SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels,
                                   int pitch)
{
    SDL_Surface *surface;
    const SDL_PixelFormatDetails *details;
    Uint64 size;

    if (width <= 0 || height <= 0) {
        SDL_SetError("Invalid surface size %dx%d", width, height);
        return NULL;
    }
    details = SDL_GetPixelFormatDetails(format);
    if (!details) {
        return NULL;
    }
    if (!details->bytes_per_pixel) {
        SDL_SetError("Pixel format '%s' has no defined layout", SDL_GetPixelFormatName(format));
        return NULL;
    }

    size = (Uint64)(Uint32)width * (Uint64)(Uint32)height * details->bytes_per_pixel;
    if (size > (Uint64)0x7FFFFFFF) {
        SDL_SetError("Surface is too large");
        return NULL;
    }

    surface = (SDL_Surface *)SDLOP_Calloc(1, sizeof(*surface));
    if (!surface) {
        SDL_OutOfMemory();
        return NULL;
    }
    surface->format = format;
    surface->w = width;
    surface->h = height;
    surface->pitch = pitch ? pitch : (int)((Uint64)width * details->bytes_per_pixel);
    surface->flags = SDL_SURFACE_PREALLOCATED;
    if (pixels) {
        surface->pixels = pixels;
    } else {
        size_t bytes = (size_t)surface->h * (size_t)surface->pitch;
#ifdef HAVE_MALLOC_USABLE_SIZE
        surface->pixels = malloc(bytes);
#else
        surface->pixels = SDLOP_Alloc(bytes);
#endif
        if (!surface->pixels) {
            SDLOP_Free(surface);
            SDL_OutOfMemory();
            return NULL;
        }
        surface->flags = SDL_SURFACE_SIMD_ALIGNED | SDL_SURFACE_PREALLOCATED;
    }
    surface->refcount = 1;
    if (sdlop_surface_extra(surface, true)) {
        /* SDL_CreateSurfaceFrom() was given the buffer: it is not ours to move. */
        sdlop_surface_extra(surface, false)->owns_pixels = (pixels == NULL);
    }
    return surface;
}

void SDL_DestroySurface(SDL_Surface *surface)
{
    if (!surface) {
        SDL_InvalidParamError("surface");
        return;
    }
    if (--surface->refcount > 0) {
        return;
    }
    if (surface->reserved) {
        SDLOP_SurfaceExtra *extra = (SDLOP_SurfaceExtra *)surface->reserved;
        SDL_DestroyProperties(extra->props);
        SDLOP_Free(extra);
    }
    if (surface->pixels) {
        SDLOP_Free(surface->pixels);
    }
    SDLOP_Free(surface);
}

SDL_PropertiesID SDL_GetSurfaceProperties(SDL_Surface *surface)
{
    if (!surface) {
        SDL_InvalidParamError("surface");
        return 0;
    }
    SDLOP_SurfaceExtra *extra = sdlop_surface_extra(surface, true);
    if (!extra) {
        return 0;
    }
    if (!extra->props) {
        extra->props = SDL_CreateProperties();
    }
    return extra->props;
}

bool SDL_SetSurfaceColorspace(SDL_Surface *surface, SDL_Colorspace colorspace)
{
    if (!surface) {
        return SDL_InvalidParamError("surface");
    }
    /* The colorspace travels inside the SDL_PixelFormat value (SDLK_STYLE
       SDL_DEFINE_PIXELFORMAT packs it in the upper bits), so storing it is a
       matter of rewriting those bits. SDLop accepts the call and records it in
       a surface property; nothing in the window-surface path consumes it. */
    SDLOP_SurfaceExtra *extra = sdlop_surface_extra(surface, true);
    if (!extra) {
        return false;
    }
    extra->colorspace = colorspace == SDL_COLORSPACE_UNKNOWN ? SDL_COLORSPACE_SRGB : colorspace;
    return true;
}

SDL_Colorspace SDL_GetSurfaceColorspace(SDL_Surface *surface)
{
    if (!surface) {
        SDL_InvalidParamError("surface");
        return SDL_COLORSPACE_UNKNOWN;
    }
    SDLOP_SurfaceExtra *extra = sdlop_surface_extra(surface, false);
    return extra ? extra->colorspace : SDL_COLORSPACE_SRGB;
}

bool SDL_LockSurface(SDL_Surface *surface)
{
    if (!surface) {
        return SDL_InvalidParamError("surface");
    }
    if (surface->flags & SDL_SURFACE_LOCKED) {
        return SDL_SetError("Surface is already locked");
    }
    surface->flags |= SDL_SURFACE_LOCKED;
    return true;
}

void SDL_UnlockSurface(SDL_Surface *surface)
{
    if (!surface) {
        SDL_InvalidParamError("surface");
        return;
    }
    surface->flags &= ~SDL_SURFACE_LOCKED;
}

static void sdlop_fill_span(Uint8 *dst, const SDL_PixelFormatDetails *format, int width, Uint32 pixel,
                            int bytes_per_pixel)
{
    switch (bytes_per_pixel) {
        case 1:
            memset(dst, (int)(pixel & 0xFF), (size_t)width);
            break;
        case 2:
            for (int i = 0; i < width; i++) {
                ((Uint16 *)(void *)dst)[i] = (Uint16)pixel;
            }
            break;
        case 3: {
            Uint8 b0 = (Uint8)(pixel & 0xFF);
            Uint8 b1 = (Uint8)((pixel >> 8) & 0xFF);
            Uint8 b2 = (Uint8)((pixel >> 16) & 0xFF);
            for (int i = 0; i < width; i++) {
                dst[i * 3 + 0] = b0;
                dst[i * 3 + 1] = b1;
                dst[i * 3 + 2] = b2;
            }
            break;
        }
        case 4:
            for (int i = 0; i < width; i++) {
                ((Uint32 *)(void *)dst)[i] = pixel;
            }
            break;
        default:
            break;
    }
    (void)format;
}

bool SDL_FillSurfaceRects(SDL_Surface *surface, const SDL_Rect *rects, int count, Uint32 color)
{
    const SDL_PixelFormatDetails *format;
    int i;

    if (!surface) {
        return SDL_InvalidParamError("surface");
    }
    if (count < 0) {
        return SDL_InvalidParamError("count");
    }
    format = SDL_GetPixelFormatDetails(surface->format);
    if (!format || !format->bytes_per_pixel) {
        return SDL_SetError("Unsupported pixel format '%s'", SDL_GetPixelFormatName(surface->format));
    }

    if (!rects || count == 0) {
        SDL_Rect whole;
        whole.x = 0;
        whole.y = 0;
        whole.w = surface->w;
        whole.h = surface->h;
        return SDL_FillSurfaceRects(surface, &whole, 1, color);
    }

    for (i = 0; i < count; i++) {
        SDL_Rect clipped;
        if (!SDL_GetRectIntersection(&rects[i], &(SDL_Rect){ 0, 0, surface->w, surface->h }, &clipped)) {
            continue;
        }
        for (int y = clipped.y; y < clipped.y + clipped.h; y++) {
            Uint8 *dst = (Uint8 *)surface->pixels + (size_t)y * surface->pitch +
                         (size_t)clipped.x * format->bytes_per_pixel;
            sdlop_fill_span(dst, format, clipped.w, color, format->bytes_per_pixel);
        }
    }
    return true;
}

bool SDL_FillSurfaceRect(SDL_Surface *surface, const SDL_Rect *rect, Uint32 color)
{
    return SDL_FillSurfaceRects(surface, rect, rect ? 1 : 0, color);
}

/* ------------------------------------------------------------------------- */
/* Window surfaces                                                           */
/* ------------------------------------------------------------------------- */

bool SDLOP_CreateWindowSurface(SDL_Window *window, int w, int h, SDL_PixelFormat format)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();
    SDL_Surface *surface = NULL;

    if (driver && driver->create_window_surface) {
        if (!driver->create_window_surface(window, &surface)) {
            return false;
        }
    }
    if (!surface) {
        surface = SDL_CreateSurface(w, h, format);
        if (!surface) {
            return false;
        }
    }
    window->surface = surface;
    window->surface_dirty = true;
    return true;
}

/* SDL3 hands the application the same window surface back after the window grew
   or shrank, so the pixels move instead of the surface being replaced. */
void SDLOP_ResizeWindowSurface(SDL_Window *window, int w, int h)
{
    SDL_Surface *surface;
    SDLOP_SurfaceExtra *extra;
    const SDL_PixelFormatDetails *details;
    void *pixels;
    int pitch, copy_rows, y;

    if (!window || !(surface = window->surface) || w <= 0 || h <= 0) {
        return;
    }
    if (surface->w == w && surface->h == h) {
        return;
    }
    extra = sdlop_surface_extra(surface, false);
    details = SDL_GetPixelFormatDetails(surface->format);
    if (!extra || !extra->owns_pixels || !details || !details->bytes_per_pixel) {
        return;
    }
    pitch = (int)((Uint64)(Uint32)w * details->bytes_per_pixel);
    pixels = SDLOP_Alloc((size_t)pitch * (size_t)h);
    if (!pixels) {
        SDL_OutOfMemory();
        return;
    }
    memset(pixels, 0, (size_t)pitch * (size_t)h);
    copy_rows = surface->h < h ? surface->h : h;
    for (y = 0; y < copy_rows; y++) {
        const int bytes = surface->pitch < pitch ? surface->pitch : pitch;
        memcpy((Uint8 *)pixels + (size_t)y * (size_t)pitch,
               (const Uint8 *)surface->pixels + (size_t)y * (size_t)surface->pitch, (size_t)bytes);
    }
    SDLOP_Free(surface->pixels);
    surface->pixels = pixels;
    surface->w = w;
    surface->h = h;
    surface->pitch = pitch;
    window->surface_dirty = true;
}

void SDLOP_DestroyWindowSurface(SDL_Window *window)
{
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
}
