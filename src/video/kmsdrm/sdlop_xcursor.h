/*
  SDLop - XCursor theme loading for the KMSDRM driver.

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

#ifdef __cplusplus
}
#endif

#endif /* sdlop_xcursor_h_ */
