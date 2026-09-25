/*
  SDLop - surface management (software rendering).

  Window surfaces wrap the driver's framebuffer directly (zero copy on
  Wayland: the surface pixels ARE the wl_shm buffer).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <stdint.h>

int SDLOP_BytesPerPixel(SDL_PixelFormat format)
{
    switch (format) {
    case SDL_PIXELFORMAT_INDEX8:
        return 1;
    case SDL_PIXELFORMAT_RGB565:
        return 2;
    case SDL_PIXELFORMAT_RGB24:
    case SDL_PIXELFORMAT_BGR24:
        return 3;
    case SDL_PIXELFORMAT_XRGB8888:
    case SDL_PIXELFORMAT_RGBX8888:
    case SDL_PIXELFORMAT_ARGB8888:
    case SDL_PIXELFORMAT_RGBA8888:
    case SDL_PIXELFORMAT_XBGR8888:
    case SDL_PIXELFORMAT_BGRX8888:
    case SDL_PIXELFORMAT_ABGR8888:
    case SDL_PIXELFORMAT_BGRA8888:
        return 4;
    default:
        return 0;
    }
}

SDL_Surface *SDL_CreateSurface(int width, int height, SDL_PixelFormat format)
{
    if (width <= 0 || height <= 0) {
        SDL_SetError("Invalid surface dimensions");
        return NULL;
    }
    int bpp = SDLOP_BytesPerPixel(format);
    if (!bpp) {
        SDL_SetError("Unsupported pixel format: 0x%08x", (unsigned)format);
        return NULL;
    }
    /* 64-bit pitch math: width*bpp can overflow int (UB) for huge widths */
    int64_t pitch64 = (int64_t)width * (int64_t)bpp;
    /* and the byte total can overflow size_t on 32-bit targets (wasm32) */
    uint64_t total64 = (uint64_t)pitch64 * (uint64_t)height;
    if (pitch64 > INT32_MAX || total64 > (uint64_t)SIZE_MAX) {
        SDL_SetError("Surface is too large");
        return NULL;
    }
    SDL_Surface *surface = (SDL_Surface *)calloc(1, sizeof(*surface));
    if (!surface) {
        SDL_OutOfMemory();
        return NULL;
    }
    surface->format = format;
    surface->w = width;
    surface->h = height;
    surface->pitch = (int)pitch64;
    surface->pixels = calloc(1, (size_t)total64);
    if (!surface->pixels) {
        free(surface);
        SDL_OutOfMemory();
        return NULL;
    }
    surface->refcount = 1;
    return surface;
}

SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels, int pitch)
{
    if (width <= 0 || height <= 0) {
        SDL_SetError("Invalid surface dimensions");
        return NULL;
    }
    int bpp = SDLOP_BytesPerPixel(format);
    if (!bpp) {
        SDL_SetError("Unsupported pixel format: 0x%08x", (unsigned)format);
        return NULL;
    }
    /* a row must fit in the given pitch or every row-strided write would
     * overrun the caller's buffer (64-bit math: width*bpp can overflow int) */
    if (pitch < 0 || (int64_t)pitch < (int64_t)width * (int64_t)bpp) {
        SDL_SetError("Pitch %d is too small for %dx%d format 0x%08x",
                     pitch, width, height, (unsigned)format);
        return NULL;
    }
    SDL_Surface *surface = (SDL_Surface *)calloc(1, sizeof(*surface));
    if (!surface) {
        SDL_OutOfMemory();
        return NULL;
    }
    surface->flags = SDL_SURFACE_PREALLOCATED;
    surface->format = format;
    surface->w = width;
    surface->h = height;
    surface->pitch = pitch;
    surface->pixels = pixels;
    surface->refcount = 1;
    return surface;
}

void SDL_DestroySurface(SDL_Surface *surface)
{
    if (!surface) {
        return;
    }
    if (!(surface->flags & SDL_SURFACE_PREALLOCATED)) {
        free(surface->pixels);
    }
    free(surface);
}

bool SDL_LockSurface(SDL_Surface *surface)
{
    if (!surface) {
        return SDL_SetError("Invalid surface");
    }
    surface->flags |= SDL_SURFACE_LOCKED;
    return true; /* always directly accessible in SDLop */
}

void SDL_UnlockSurface(SDL_Surface *surface)
{
    if (surface) {
        surface->flags &= ~SDL_SURFACE_LOCKED;
    }
}

static void fill32(SDL_Surface *dst, const SDL_Rect *r, Uint32 color)
{
    Uint32 *base = (Uint32 *)dst->pixels;
    int pitch32 = dst->pitch / 4;
    for (int y = r->y; y < r->y + r->h; y++) {
        Uint32 *row = base + (size_t)y * (size_t)pitch32 + r->x;
        for (int x = 0; x < r->w; x++) {
            row[x] = color;
        }
    }
}

static void fill24(SDL_Surface *dst, const SDL_Rect *r, Uint32 color, bool bgr)
{
    Uint8 *base = (Uint8 *)dst->pixels;
    Uint8 c[3];
    if (bgr) { /* BGR24 memory order: B,G,R */
        c[0] = (Uint8)(color & 0xFF);
        c[1] = (Uint8)((color >> 8) & 0xFF);
        c[2] = (Uint8)((color >> 16) & 0xFF);
    } else { /* RGB24 memory order: R,G,B */
        c[0] = (Uint8)((color >> 16) & 0xFF);
        c[1] = (Uint8)((color >> 8) & 0xFF);
        c[2] = (Uint8)(color & 0xFF);
    }
    for (int y = r->y; y < r->y + r->h; y++) {
        Uint8 *row = base + (size_t)y * (size_t)dst->pitch + (size_t)r->x * 3;
        for (int x = 0; x < r->w; x++) {
            row[x * 3 + 0] = c[0];
            row[x * 3 + 1] = c[1];
            row[x * 3 + 2] = c[2];
        }
    }
}

static void fill16(SDL_Surface *dst, const SDL_Rect *r, Uint32 color)
{
    Uint16 *base = (Uint16 *)dst->pixels;
    int pitch16 = dst->pitch / 2;
    Uint16 c = (Uint16)color;
    for (int y = r->y; y < r->y + r->h; y++) {
        Uint16 *row = base + (size_t)y * (size_t)pitch16 + r->x;
        for (int x = 0; x < r->w; x++) {
            row[x] = c;
        }
    }
}

bool SDL_FillSurfaceRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color)
{
    if (!dst || !dst->pixels) {
        return SDL_SetError("Invalid surface");
    }
    SDL_Rect full = { 0, 0, dst->w, dst->h };
    SDL_Rect r = rect ? *rect : full;

    /* clip */
    if (r.x < 0) {
        r.w += r.x;
        r.x = 0;
    }
    if (r.y < 0) {
        r.h += r.y;
        r.y = 0;
    }
    if (r.x + r.w > dst->w) {
        r.w = dst->w - r.x;
    }
    if (r.y + r.h > dst->h) {
        r.h = dst->h - r.y;
    }
    if (r.w <= 0 || r.h <= 0) {
        return true; /* nothing to do */
    }

    switch (SDLOP_BytesPerPixel(dst->format)) {
    case 4:
        /* honor alpha only for formats that have it; else force opaque */
        if (dst->format == SDL_PIXELFORMAT_XRGB8888 || dst->format == SDL_PIXELFORMAT_XBGR8888) {
            color |= 0xFF000000u & 0; /* X ignored; keep as-is */
        }
        if (dst->format == SDL_PIXELFORMAT_XBGR8888 || dst->format == SDL_PIXELFORMAT_ABGR8888 ||
            dst->format == SDL_PIXELFORMAT_BGRA8888 || dst->format == SDL_PIXELFORMAT_BGRX8888) {
            /* color is given as 0xAARRGGBB-style; swap R/B for BGR layouts */
            color = (color & 0xFF00FF00u) | ((color & 0x00FF0000u) >> 16) | ((color & 0x000000FFu) << 16);
        }
        fill32(dst, &r, color);
        break;
    case 3:
        fill24(dst, &r, color, dst->format == SDL_PIXELFORMAT_BGR24);
        break;
    case 2:
        fill16(dst, &r, color);
        break;
    default:
        return SDL_SetError("SDL_FillSurfaceRect: unsupported format 0x%08x", (unsigned)dst->format);
    }
    return true;
}

Uint32 SDL_MapSurfaceRGB(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b)
{
    return SDL_MapSurfaceRGBA(surface, r, g, b, 0xFF);
}

Uint32 SDL_MapSurfaceRGBA(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (!surface) {
        return 0;
    }
    switch (surface->format) {
    case SDL_PIXELFORMAT_ARGB8888:
        return ((Uint32)a << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | b;
    case SDL_PIXELFORMAT_XRGB8888:
        return ((Uint32)r << 16) | ((Uint32)g << 8) | b;
    case SDL_PIXELFORMAT_ABGR8888:
        return ((Uint32)a << 24) | ((Uint32)b << 16) | ((Uint32)g << 8) | r;
    case SDL_PIXELFORMAT_XBGR8888:
        return ((Uint32)b << 16) | ((Uint32)g << 8) | r;
    case SDL_PIXELFORMAT_RGBA8888:
        return ((Uint32)r << 24) | ((Uint32)g << 16) | ((Uint32)b << 8) | a;
    case SDL_PIXELFORMAT_BGRA8888:
        return ((Uint32)b << 24) | ((Uint32)g << 16) | ((Uint32)r << 8) | a;
    case SDL_PIXELFORMAT_RGB565:
        return ((Uint32)(r >> 3) << 11) | ((Uint32)(g >> 2) << 5) | (b >> 3);
    default:
        return ((Uint32)r << 16) | ((Uint32)g << 8) | b;
    }
}

bool SDL_ReadSurfacePixel(SDL_Surface *surface, int x, int y, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a)
{
    if (!surface || !surface->pixels || x < 0 || y < 0 || x >= surface->w || y >= surface->h) {
        return SDL_SetError("Invalid surface or coordinates");
    }
    Uint32 p;
    if (SDLOP_BytesPerPixel(surface->format) == 4) {
        p = ((const Uint32 *)(const void *)((const Uint8 *)surface->pixels + (size_t)y * surface->pitch))[x];
    } else {
        return SDL_SetError("SDL_ReadSurfacePixel: unsupported format");
    }
    /* formats are stored little-endian 0xAARRGGBB-ish; handle the common ones */
    switch (surface->format) {
    case SDL_PIXELFORMAT_ARGB8888:
    case SDL_PIXELFORMAT_XRGB8888:
        if (r) { *r = (Uint8)(p >> 16); }
        if (g) { *g = (Uint8)(p >> 8); }
        if (b) { *b = (Uint8)p; }
        if (a) { *a = (Uint8)(p >> 24); }
        break;
    case SDL_PIXELFORMAT_ABGR8888:
    case SDL_PIXELFORMAT_XBGR8888:
        if (r) { *r = (Uint8)p; }
        if (g) { *g = (Uint8)(p >> 8); }
        if (b) { *b = (Uint8)(p >> 16); }
        if (a) { *a = (Uint8)(p >> 24); }
        break;
    default:
        return SDL_SetError("SDL_ReadSurfacePixel: unsupported format");
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Window surface plumbing (dispatched to the video driver)            */
/* ------------------------------------------------------------------ */

SDL_Surface *SDL_GetWindowSurface(SDL_Window *window)
{
    if (!window) {
        SDL_SetError("Invalid window");
        return NULL;
    }
    if (window->surface) {
        return window->surface;
    }
    if (!sdlop.video || !sdlop.video->CreateWindowFramebuffer) {
        SDL_SetError("Video driver does not support window surfaces");
        return NULL;
    }
    SDL_Surface *surface = NULL;
    if (!sdlop.video->CreateWindowFramebuffer(sdlop.video, window, &surface) || !surface) {
        return NULL; /* driver sets error */
    }
    window->surface = surface;
    return surface;
}

bool SDL_UpdateWindowSurfaceRects(SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    if (!window || !window->surface) {
        return SDL_SetError("No window surface to update");
    }
    if (!sdlop.video || !sdlop.video->UpdateWindowFramebuffer) {
        return SDL_SetError("Video driver does not support window surfaces");
    }
    return sdlop.video->UpdateWindowFramebuffer(sdlop.video, window, rects, numrects);
}

bool SDL_UpdateWindowSurface(SDL_Window *window)
{
    return SDL_UpdateWindowSurfaceRects(window, NULL, 0);
}

bool SDL_WindowHasSurface(SDL_Window *window)
{
    return window && window->surface != NULL;
}

bool SDL_DestroyWindowSurface(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (!window->surface) {
        return true;
    }
    if (sdlop.video && sdlop.video->DestroyWindowFramebuffer) {
        sdlop.video->DestroyWindowFramebuffer(sdlop.video, window);
    }
    window->surface = NULL; /* driver owns/frees the surface memory */
    return true;
}
