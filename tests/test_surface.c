/*
  SDLop test: software window surface (dummy driver, headless).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    assert(SDL_Init(SDL_INIT_VIDEO));

    SDL_Window *w = SDL_CreateWindow("surface", 320, 200, 0);
    assert(w);

    assert(!SDL_WindowHasSurface(w));
    SDL_Surface *s = SDL_GetWindowSurface(w);
    assert(s);
    assert(SDL_WindowHasSurface(w));
    assert(s->w == 320 && s->h == 200);
    assert(s->pitch == 320 * 4);
    assert(s->format == SDL_PIXELFORMAT_XRGB8888);
    assert(s->pixels);

    /* same surface on repeat calls */
    assert(SDL_GetWindowSurface(w) == s);

    /* full fill + readback */
    assert(SDL_FillSurfaceRect(s, NULL, 0x00FF0000)); /* red */
    Uint8 r = 0, g = 0, b = 0, a = 0;
    assert(SDL_ReadSurfacePixel(s, 0, 0, &r, &g, &b, &a));
    assert(r == 0xFF && g == 0 && b == 0);
    assert(SDL_ReadSurfacePixel(s, 319, 199, &r, &g, &b, &a));
    assert(r == 0xFF);

    /* partial rect fill */
    SDL_Rect rect = { 10, 10, 20, 20 };
    assert(SDL_FillSurfaceRect(s, &rect, 0x0000FF00)); /* green */
    assert(SDL_ReadSurfacePixel(s, 15, 15, &r, &g, &b, &a));
    assert(g == 0xFF && r == 0);
    assert(SDL_ReadSurfacePixel(s, 5, 5, &r, &g, &b, &a));
    assert(r == 0xFF && g == 0); /* outside the rect: still red */

    /* clipping does not crash or write out of bounds */
    SDL_Rect oob = { -50, -50, 1000, 1000 };
    assert(SDL_FillSurfaceRect(s, &oob, 0x000000FF));
    assert(SDL_ReadSurfacePixel(s, 0, 0, &r, &g, &b, &a));
    assert(b == 0xFF);

    /* map helpers */
    assert(SDL_MapSurfaceRGB(s, 1, 2, 3) == 0x00010203u);
    assert(SDL_MapSurfaceRGBA(s, 1, 2, 3, 4) == 0x00010203u); /* X format drops A */

    /* lock/unlock no-ops */
    assert(SDL_LockSurface(s));
    SDL_UnlockSurface(s);

    /* present */
    assert(SDL_UpdateWindowSurface(w));
    assert(SDL_UpdateWindowSurfaceRects(w, &rect, 1));

    /* resize: surface follows the window (same pointer, new dims) */
    assert(SDL_SetWindowSize(w, 400, 300));
    SDL_Surface *s2 = SDL_GetWindowSurface(w);
    assert(s2->w == 400 && s2->h == 300);
    assert(s2->pitch == 400 * 4);
    assert(s2->pixels);

    /* standalone surfaces */
    SDL_Surface *mem = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_ARGB8888);
    assert(mem);
    assert(SDL_FillSurfaceRect(mem, NULL, 0x80112233));
    assert(SDL_ReadSurfacePixel(mem, 4, 4, &r, &g, &b, &a));
    assert(a == 0x80 && r == 0x11 && g == 0x22 && b == 0x33);
    SDL_DestroySurface(mem);

    /* destroy window surface then recreate */
    assert(SDL_DestroyWindowSurface(w));
    assert(!SDL_WindowHasSurface(w));
    s2 = SDL_GetWindowSurface(w);
    assert(s2 && s2->w == 400);

    SDL_DestroyWindow(w);
    SDL_Quit();
    printf("test_surface: PASS\n");
    return 0;
}
