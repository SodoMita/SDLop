/*
  SDLop - shared XCursor theme loading for console video drivers.

  Adapted from swa (github.com/nyorain/swa, src/swa/kms/xcursor.c),
  which in turn combines code from:

    wayland-cursor / libEGL cursor handling
      Copyright 2012 Intel Corporation
    libXcursor (X.Org)
      Copyright 2002 Keith Packard

  All MIT / X11-style licenses compatible with SDLop's zlib license;
  see the file header in sdlop_xcursor.c for the full texts.
  dlg was replaced with SDLop's logging; everything else is a straight
  adaptation (rename swa_ -> sdlop_).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_xcursor_h_
#define sdlop_xcursor_h_

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sdlop_xcursor_image {
    uint32_t width;
    uint32_t height;
    uint32_t hotspot_x;
    uint32_t hotspot_y;
    uint32_t delay;         /* animation delay to next frame (ms) */
    uint8_t *buffer;        /* ARGB8888, row-major */
};

struct sdlop_xcursor {
    unsigned int image_count;
    struct sdlop_xcursor_image **images;
    char *name;
    uint32_t total_delay;   /* length of the animation in ms */
};

/* Container for an XCursor theme. */
struct sdlop_xcursor_theme {
    unsigned int cursor_count;
    struct sdlop_xcursor **cursors;
    char *name;
    int size;
};

/* Loads the named xcursor theme at the given cursor size (in pixels).
 * theme == NULL uses XCURSOR_THEME / the default theme.
 * Returns NULL on failure (e.g. no themes installed). */
struct sdlop_xcursor_theme *sdlop_xcursor_theme_load(const char *theme, int size);
void sdlop_xcursor_theme_destroy(struct sdlop_xcursor_theme *theme);

/* Obtains a cursor image set for the specified cursor name (e.g. "left_ptr"). */
struct sdlop_xcursor *sdlop_xcursor_theme_get_cursor(struct sdlop_xcursor_theme *theme,
                                                     const char *name);

int sdlop_xcursor_frame(struct sdlop_xcursor *cursor, uint32_t time);
int sdlop_xcursor_frame_and_duration(struct sdlop_xcursor *cursor, uint32_t time,
                                     uint32_t *duration);

/* One premultiplied ARGB8888 cursor image, shared by the console drivers so
 * KMSDRM and fbdev draw exactly the same pointer. Pixels returned by
 * sdlop_system_cursor_bitmap_fit() are owned by the theme loader and stay
 * valid until sdlop_cursor_theme_quit(); sdlop_cursor_bitmap_from_surface()
 * returns a caller-owned copy instead. */
struct sdlop_cursor_bitmap {
    int w, h;
    int hot_x, hot_y;
    const uint32_t *pixels; /* premultiplied ARGB8888, packed rows */
};

/* Returns the first frame of the system cursor from the XCursor theme, with
 * the same built-in arrow fallback for both drivers. max_w/max_h may be 0
 * when there is no hardware limit; an image that does not fit falls back to
 * the built-in arrow. */
const struct sdlop_cursor_bitmap *sdlop_system_cursor_bitmap_fit(SDL_SystemCursor id,
                                                                 int max_w, int max_h);
const struct sdlop_cursor_bitmap *sdlop_builtin_cursor_bitmap(void);

/* Premultiplies a color cursor once into `out`. *owned receives the pixel
 * allocation (free it with SDL_free); out->pixels borrows it. */
bool sdlop_cursor_bitmap_from_surface(SDL_Surface *surface, int hot_x, int hot_y,
                                      struct sdlop_cursor_bitmap *out, uint32_t **owned);
void sdlop_cursor_theme_quit(void);

#ifdef __cplusplus
}
#endif

#endif /* sdlop_xcursor_h_ */
