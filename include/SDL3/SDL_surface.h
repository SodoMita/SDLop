/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Surface management (subset of <SDL3/SDL_surface.h>, identical struct
  layout and signatures).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_surface_h_
#define SDL_surface_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_blendmode.h>

/** The flags on an SDL_Surface. */
typedef Uint32 SDL_SurfaceFlags;

#define SDL_SURFACE_PREALLOCATED    0x00000001u /**< Surface uses preallocated pixel memory */
#define SDL_SURFACE_LOCK_NEEDED     0x00000002u /**< Surface needs to be locked to access pixels */
#define SDL_SURFACE_LOCKED          0x00000004u /**< Surface is currently locked */

/**
 * A collection of pixels used in software blitting.
 * Layout identical to SDL3's SDL_Surface.
 */
struct SDL_Surface
{
    SDL_SurfaceFlags flags;     /**< Read-only */
    SDL_PixelFormat format;     /**< Read-only */
    int w;                      /**< Read-only */
    int h;                      /**< Read-only */
    int pitch;                  /**< Read-only */
    void *pixels;               /**< Read-write; NULL when the surface must be locked */

    int refcount;               /**< Application reference count */

    void *reserved;             /**< Internal */
};

typedef struct SDL_Surface SDL_Surface;

/**
 * Allocate a new surface with a specific pixel format.
 */
extern SDL_Surface *SDL_CreateSurface(int width, int height, SDL_PixelFormat format);

/**
 * Allocate a new surface with a specific pixel format and existing pixel
 * data (no copy is made).
 */
extern SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels, int pitch);

/**
 * Free a surface. Safe to pass NULL.
 */
extern void SDL_DestroySurface(SDL_Surface *surface);

/**
 * Lock a surface for direct pixel access.
 */
extern bool SDL_LockSurface(SDL_Surface *surface);

/**
 * Release a surface locked with SDL_LockSurface().
 */
extern void SDL_UnlockSurface(SDL_Surface *surface);

/**
 * Perform a fast fill of a rectangle with a specific color.
 *
 * \param dst the surface to fill.
 * \param rect the rectangle to fill (NULL = entire surface).
 * \param color the color to fill with (e.g. 0x00RRGGBB for XRGB8888).
 */
extern bool SDL_FillSurfaceRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);

/**
 * Map an RGB triple to an opaque pixel value for a surface.
 */
extern Uint32 SDL_MapSurfaceRGB(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b);

/**
 * Map an RGBA quadruple to a pixel value for a surface.
 */
extern Uint32 SDL_MapSurfaceRGBA(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b, Uint8 a);

/**
 * Retrieves a single pixel from a surface (slow path, for tests/tools).
 */
extern bool SDL_ReadSurfacePixel(SDL_Surface *surface, int x, int y, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a);

#endif /* SDL_surface_h_ */
