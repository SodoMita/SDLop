/*
  SDLop - fbdev (Linux framebuffer) video driver.

  Talks to /dev/fbNN with ioctl() + mmap() only: no library and no header
  beyond the kernel uapi ones, so the core keeps its "dlopen everything
  optional" promise and nothing is added to the build probes.

  Console semantics (what makes fbdev a console driver instead of a picture
  on top of someone else's terminal):

    - the framebuffer has exactly one mode, the one the kernel programmed, so
      every window request snaps back to it;
    - the driver owns the VT it was started on: KD_GRAPHICS + K_OFF, so fbcon
      and the console keyboard handler do not draw over the picture and
      Ctrl-C does not reach a shell underneath;
    - switching away (Ctrl+Alt+Fn) puts KD_TEXT, the previous keyboard mode
      and no leftover cursor back, then hands the VT to the kernel;
      switching back retakes the console and asks the app to repaint;
    - quitting restores KD_TEXT, the keyboard mode and VT_AUTO.

  It never activates a VT on its own: rendering happens where the user already
  is. SDLOP_FBDEV_VT_SWITCH=1 (or $TTYNO=<n>) opts into activating a VT.
  SDLOP_FBDEV_TAKEOVER=0 disables the console takeover entirely (development
  over a serial console), SDLOP_FBDEV_CURSOR=0 disables the software cursor.

  Headless verification: with SDLOP_FBDEV_DUMP=<file.ppm> the driver opens no
  device and keeps the scanout in RAM (SDLOP_FBDEV_SIZE=WxH, default
  1024x768), dumping every presented frame as binary P6. That is what
  tests/test_fbdev.c runs in CI. SDLOP_FBDEV_PATH points the driver at
  another device node.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define FBDEV_MAX_WINDOWS 8
#define FBDEV_DEFAULT_W 1024
#define FBDEV_DEFAULT_H 768
#define FBDEV_MAX_DIM 16384
#define FBDEV_ARROW_W 12
#define FBDEV_ARROW_H 16

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/* The buffer that reaches the screen: the mapped card memory, or RAM in dump
   mode. `pitch` is the row stride in bytes and is always valid. */
typedef struct FBDEV_Scanout
{
    Uint8 *pixels;
    int w, h;
    size_t pitch;
    bool mapped;
    size_t length;
} FBDEV_Scanout;

/* Pixel layout, straight from FBIOGET_VSCREENINFO (validated: only true
   colour, right-aligned, <= 8 bit channels and in-range offsets get here). */
typedef struct FBDEV_PixelLayout
{
    int bpp;                     /* 16, 24 or 32 */
    int bytes;                   /* bpp / 8 */
    Uint8 r_off, g_off, b_off;   /* bit offsets */
    Uint8 r_len, g_len, b_len;   /* channel widths in bits */
    bool native;                 /* XRGB8888, so the copy can be a memcpy */
} FBDEV_PixelLayout;

/* Console (virtual terminal) ownership. */
typedef struct FBDEV_VT
{
    int fd;                 /* /dev/ttyN, -1 when we have no console */
    int num;                /* VT number, 0 when unknown */
    bool process_mode;      /* VT_PROCESS installed (we acknowledge switches) */
    bool graphics;          /* we set KD_GRAPHICS */
    bool kb_off;            /* we set K_OFF (only with a keyboard of our own) */
    bool raw_attached;      /* raw input delivers our key events */
    bool active;            /* our VT is the one on screen */
    int saved_kbmode;       /* -1 when unknown */
    int rel_sig, acq_sig;   /* 0 when no signal could be claimed */
    struct sigaction old_rel, old_acq;
    bool handlers_installed;
    int signal_fd;          /* signalfd over both signals, -1 if none */
    sigset_t saved_mask;
    bool mask_saved;
} FBDEV_VT;

typedef struct FBDEV_WindowRecord
{
    SDL_Window *window;
    bool visible;
} FBDEV_WindowRecord;

static struct
{
    int fd;
    char path[256];
    FBDEV_Scanout scanout;
    Uint8 *ram;               /* owns scanout.pixels when the card is not mapped */
    size_t ram_size;
    bool shadow_live;         /* a window has presented at least once */
    int pan_x, pan_y;         /* visible window inside the card memory */
    FBDEV_PixelLayout layout;
    bool dump;
    char dump_path[512];
    int dump_frames;
    Uint32 refresh_hz;
    SDL_DisplayID display;
    FBDEV_VT vt;
    SDL_Cursor *cursor;       /* last cursor handed to ShowCursor (not owned) */
    SDL_Rect cursor_rect;     /* where it is drawn; empty = not on screen */
    float cursor_drawn_x, cursor_drawn_y; /* logical position of that draw */
    bool cursor_enabled;
    FBDEV_WindowRecord windows[FBDEV_MAX_WINDOWS];
} fbdev;

/* set by the signal handlers when no signalfd could be created */
static volatile sig_atomic_t fbdev_vt_rel_pending;
static volatile sig_atomic_t fbdev_vt_acq_pending;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void fbdev_log(const char *fmt, ...)
{
    va_list ap;
    char text[512];

    va_start(ap, fmt);
    SDL_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "fbdev: %s", text);
}

static bool fbdev_replace8(const char *name, bool fallback)
{
    const char *value = SDL_getenv(name);

    if (!value || !*value) {
        return fallback;
    }
    if (SDL_strcasecmp(value, "0") == 0 || SDL_strcasecmp(value, "off") == 0 ||
        SDL_strcasecmp(value, "no") == 0 || SDL_strcasecmp(value, "false") == 0) {
        return false;
    }
    if (SDL_strcasecmp(value, "1") == 0 || SDL_strcasecmp(value, "on") == 0 ||
        SDL_strcasecmp(value, "yes") == 0 || SDL_strcasecmp(value, "true") == 0) {
        return true;
    }
    return fallback;
}

static bool fbdev_scanout_ready(void)
{
    return fbdev.scanout.pixels != NULL;
}

static void fbdev_scanout_free(void)
{
    if (fbdev.scanout.mapped && fbdev.scanout.pixels) {
        munmap(fbdev.scanout.pixels, fbdev.scanout.length);
    }
    fbdev.scanout.pixels = NULL;
    fbdev.scanout.mapped = false;
}

/* ------------------------------------------------------------------ */
/* Pixel packing                                                       */
/* ------------------------------------------------------------------ */

static Uint32 fbdev_pack(const FBDEV_PixelLayout *l, Uint32 xrgb)
{
    const Uint32 r = (xrgb >> 16) & 0xFF;
    const Uint32 g = (xrgb >> 8) & 0xFF;
    const Uint32 b = xrgb & 0xFF;
    Uint32 out = 0;

    if (l->r_len) {
        out |= (r >> (8 - l->r_len)) << l->r_off;
    }
    if (l->g_len) {
        out |= (g >> (8 - l->g_len)) << l->g_off;
    }
    if (l->b_len) {
        out |= (b >> (8 - l->b_len)) << l->b_off;
    }
    return out;
}

static Uint32 fbdev_unpack(const FBDEV_PixelLayout *l, Uint32 raw)
{
    Uint32 out = 0;

    if (l->r_len) {
        const Uint32 mask = (1u << l->r_len) - 1u;
        out |= (((raw >> l->r_off) & mask) * 255u / mask) << 16;
    }
    if (l->g_len) {
        const Uint32 mask = (1u << l->g_len) - 1u;
        out |= (((raw >> l->g_off) & mask) * 255u / mask) << 8;
    }
    if (l->b_len) {
        const Uint32 mask = (1u << l->b_len) - 1u;
        out |= ((raw >> l->b_off) & mask) * 255u / mask;
    }
    return out;
}

static Uint32 fbdev_load(const FBDEV_PixelLayout *l, const Uint8 *p)
{
    switch (l->bytes) {
    case 2: {
        Uint16 v = 0;
        memcpy(&v, p, sizeof(v));
        return v;
    }
    case 3:
        return (Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16);
    default: {
        Uint32 v = 0;
        memcpy(&v, p, sizeof(v));
        return v;
    }
    }
}

static void fbdev_store(const FBDEV_PixelLayout *l, Uint8 *p, Uint32 packed)
{
    switch (l->bytes) {
    case 2: {
        const Uint16 v = (Uint16)packed;
        memcpy(p, &v, sizeof(v));
        break;
    }
    case 3:
        p[0] = (Uint8)(packed & 0xFF);
        p[1] = (Uint8)((packed >> 8) & 0xFF);
        p[2] = (Uint8)((packed >> 16) & 0xFF);
        break;
    default: {
        const Uint32 v = packed;
        memcpy(p, &v, sizeof(v));
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

static SDL_Rect fbdev_clip(SDL_Rect rect)
{
    /* coordinates are window/shadow space; the scanout starts at the pan */
    const SDL_Rect bounds = { 0, 0, fbdev.scanout.w, fbdev.scanout.h };
    SDL_Rect out = { 0, 0, 0, 0 };

    if (!SDL_GetRectIntersection(&rect, &bounds, &out)) {
        return (SDL_Rect){ 0, 0, 0, 0 };
    }
    return out;
}

/* Copy a rectangle of the window surface into the scanout. */
static void fbdev_blit(SDL_Surface *surface, SDL_Rect rect)
{
    if (!surface || !surface->pixels || !fbdev_scanout_ready()) {
        return;
    }
    rect = fbdev_clip(rect);
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    const FBDEV_PixelLayout *l = &fbdev.layout;
    const size_t src_pitch = (size_t)surface->pitch;
    const size_t dst_pitch = fbdev.scanout.pitch;

    for (int y = 0; y < rect.h; y++) {
        const Uint8 *src_row = (const Uint8 *)surface->pixels +
                               (size_t)(rect.y + y) * src_pitch + (size_t)rect.x * 4u;
        Uint8 *dst_row = fbdev.scanout.pixels + (size_t)(fbdev.pan_y + rect.y + y) * dst_pitch +
                         (size_t)(fbdev.pan_x + rect.x) * (size_t)l->bytes;
        if (l->native) {
            memcpy(dst_row, src_row, (size_t)rect.w * 4u);
            continue;
        }
        for (int x = 0; x < rect.w; x++) {
            Uint32 px = 0;
            memcpy(&px, src_row + (size_t)x * 4u, sizeof(px));
            fbdev_store(l, dst_row + (size_t)x * (size_t)l->bytes, fbdev_pack(l, px));
        }
    }
}

static void fbdev_fill(SDL_Rect rect, Uint32 xrgb)
{
    rect = fbdev_clip(rect);
    if (rect.w <= 0 || rect.h <= 0 || !fbdev_scanout_ready()) {
        return;
    }
    const FBDEV_PixelLayout *l = &fbdev.layout;
    const Uint32 packed = fbdev_pack(l, xrgb);
    for (int y = 0; y < rect.h; y++) {
        Uint8 *row = fbdev.scanout.pixels + (size_t)(fbdev.pan_y + rect.y + y) * fbdev.scanout.pitch +
                     (size_t)(fbdev.pan_x + rect.x) * (size_t)l->bytes;
        for (int x = 0; x < rect.w; x++) {
            fbdev_store(l, row + (size_t)x * (size_t)l->bytes, packed);
        }
    }
}

static FBDEV_WindowRecord *fbdev_record(SDL_Window *window, bool create)
{
    if (!window) {
        return NULL;
    }
    for (int i = 0; i < FBDEV_MAX_WINDOWS; i++) {
        if (fbdev.windows[i].window == window) {
            return &fbdev.windows[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < FBDEV_MAX_WINDOWS; i++) {
        if (!fbdev.windows[i].window) {
            fbdev.windows[i].window = window;
            fbdev.windows[i].visible = false;
            return &fbdev.windows[i];
        }
    }
    return NULL;
}

/* The window whose surface is on screen: pointer focus first, then keyboard
   focus, then the first visible window with a surface. */
static SDL_Window *fbdev_onscreen_window(void)
{
    SDL_Window *focus[2] = { sdlop.mouse_focus, sdlop.keyboard_focus };

    for (int i = 0; i < 2; i++) {
        FBDEV_WindowRecord *rec = fbdev_record(focus[i], false);
        if (rec && rec->visible && focus[i]->surface) {
            return focus[i];
        }
    }
    for (int i = 0; i < FBDEV_MAX_WINDOWS; i++) {
        SDL_Window *w = fbdev.windows[i].window;
        if (w && fbdev.windows[i].visible && w->surface) {
            return w;
        }
    }
    return NULL;
}

static bool fbdev_console_visible(void);

/* Put window content on screen (black when there is no window/surface). */
static void fbdev_repaint(SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    if (!fbdev_scanout_ready() || !fbdev_console_visible()) {
        return;
    }
    if (!window || !window->surface) {
        fbdev_fill((SDL_Rect){ 0, 0, fbdev.scanout.w, fbdev.scanout.h }, 0x000000);
        return;
    }
    if (rects && numrects > 0) {
        for (int i = 0; i < numrects; i++) {
            fbdev_blit(window->surface, rects[i]);
        }
        return;
    }
    fbdev_blit(window->surface, (SDL_Rect){ 0, 0, fbdev.scanout.w, fbdev.scanout.h });
}

/* ---- software cursor ---- */

static bool fbdev_console_visible(void)
{
    /* without a VT takeover there is no notion of "on screen": always draw */
    return fbdev.vt.fd < 0 || fbdev.vt.active;
}

static void fbdev_cursor_erase(void)
{
    if (SDL_RectEmpty(&fbdev.cursor_rect)) {
        return;
    }
    /* restore the covered pixels from the window surface: the scanout is a
       pure function of that surface, so nothing may go stale */
    fbdev_repaint(fbdev_onscreen_window(), &fbdev.cursor_rect, 1);
    fbdev.cursor_rect = (SDL_Rect){ 0, 0, 0, 0 };
}

/* The console has no cursor theme to load, and system cursors carry no
   image, so a plain arrow is drawn instead: an outline (X), a white body (W)
   and transparency (.). Hot spot is the top-left pixel. */
static const char *const fbdev_arrow[FBDEV_ARROW_H] = {
    "X...........",
    "XX..........",
    "XWX.........",
    "XWWX........",
    "XWWWX.......",
    "XWWWWX......",
    "XWWWWWX.....",
    "XWWWWWWX....",
    "XWWWWWWWX...",
    "XWWWWXXXX...",
    "XWWXWWX.....",
    "XWX.XWWX....",
    "XX..XWWX....",
    "X....XWWX...",
    ".....XWWX...",
    "......XX...."
};

static void fbdev_put_px(int x, int y, Uint32 xrgb)
{
    if (x < 0 || y < 0 || x >= fbdev.scanout.w || y >= fbdev.scanout.h || !fbdev_scanout_ready()) {
        return;
    }
    fbdev_store(&fbdev.layout,
                fbdev.scanout.pixels + (size_t)(fbdev.pan_y + y) * fbdev.scanout.pitch +
                    (size_t)(fbdev.pan_x + x) * (size_t)fbdev.layout.bytes,
                fbdev_pack(&fbdev.layout, xrgb));
}

static void fbdev_cursor_draw_arrow(int x, int y)
{
    for (int row = 0; row < FBDEV_ARROW_H; row++) {
        const char *line = fbdev_arrow[row];
        for (int col = 0; line[col] && col < FBDEV_ARROW_W; col++) {
            if (line[col] == 'X') {
                fbdev_put_px(x + col, y + row, 0x000000);
            } else if (line[col] == 'W') {
                fbdev_put_px(x + col, y + row, 0xFFFFFF);
            }
        }
    }
    fbdev.cursor_rect = (SDL_Rect){ x, y, FBDEV_ARROW_W, FBDEV_ARROW_H };
    fbdev.cursor_drawn_x = sdlop.mouse_x;
    fbdev.cursor_drawn_y = sdlop.mouse_y;
}

static void fbdev_cursor_draw(void)
{
    /* NULL means "hidden" to ShowCursor: that is the only signal we act on,
       and SDL_CursorVisible() is the core's own flag */
    if (!fbdev.cursor_enabled || !fbdev_scanout_ready() || !fbdev_console_visible()) {
        return;
    }
    if (!fbdev.cursor && !SDLOP_CursorVisible()) {
        return; /* SDL_HideCursor() with no cursor object installed */
    }

    SDL_Surface *image = fbdev.cursor ? fbdev.cursor->surface : NULL;
    if (!image || !image->pixels || image->format != SDL_PIXELFORMAT_ARGB8888) {
        /* system cursor (no image) or an image the core could not convert:
           the built-in arrow stands in for the pointer */
        if (fbdev.scanout.w < FBDEV_ARROW_W || fbdev.scanout.h < FBDEV_ARROW_H) {
            return;
        }
        const int ax = SDL_clamp((int)sdlop.mouse_x, 0, fbdev.scanout.w - FBDEV_ARROW_W);
        const int ay = SDL_clamp((int)sdlop.mouse_y, 0, fbdev.scanout.h - FBDEV_ARROW_H);
        fbdev_cursor_draw_arrow(ax, ay);
        return;
    }

    int x = (int)sdlop.mouse_x - fbdev.cursor->hot_x;
    int y = (int)sdlop.mouse_y - fbdev.cursor->hot_y;
    if (image->w <= 0 || image->h <= 0 || image->w > fbdev.scanout.w || image->h > fbdev.scanout.h) {
        return;
    }
    x = SDL_clamp(x, 0, fbdev.scanout.w - image->w);
    y = SDL_clamp(y, 0, fbdev.scanout.h - image->h);

    const FBDEV_PixelLayout *l = &fbdev.layout;
    for (int row = 0; row < image->h; row++) {
        const Uint8 *src = (const Uint8 *)image->pixels + (size_t)row * (size_t)image->pitch;
        Uint8 *dst = fbdev.scanout.pixels + (size_t)(fbdev.pan_y + y + row) * fbdev.scanout.pitch +
                     (size_t)(fbdev.pan_x + x) * (size_t)l->bytes;
        for (int col = 0; col < image->w; col++) {
            Uint32 px = 0;
            memcpy(&px, src + (size_t)col * 4u, sizeof(px));
            const Uint32 a = (px >> 24) & 0xFF;
            if (a == 0) {
                continue;
            }
            Uint8 *out = dst + (size_t)col * (size_t)l->bytes;
            Uint32 value = px & 0x00FFFFFFu;
            if (a != 0xFF) {
                const Uint32 under = fbdev_unpack(l, fbdev_load(l, out));
                const Uint32 inv = 255u - a;
                const Uint32 r = ((((value >> 16) & 0xFF) * a) + (((under >> 16) & 0xFF) * inv)) / 255u;
                const Uint32 g = ((((value >> 8) & 0xFF) * a) + (((under >> 8) & 0xFF) * inv)) / 255u;
                const Uint32 b = ((((value & 0xFF) * a) + ((under & 0xFF) * inv)) / 255u);
                value = (r << 16) | (g << 8) | b;
            }
            fbdev_store(l, out, fbdev_pack(l, value));
        }
    }
    fbdev.cursor_rect = (SDL_Rect){ x, y, image->w, image->h };
    fbdev.cursor_drawn_x = sdlop.mouse_x;
    fbdev.cursor_drawn_y = sdlop.mouse_y;
}

/* Write the scanout as binary PPM (P6). Image IO is not linked into SDLop, so
   the encoder lives here; the dump path is a test affordance. */
static void fbdev_dump_frame(void)
{
    if (!fbdev.dump || !fbdev.dump_path[0] || !fbdev_scanout_ready()) {
        return;
    }
    FILE *f = fopen(fbdev.dump_path, "wb");
    if (!f) {
        fbdev_log("cannot write %s: %s", fbdev.dump_path, strerror(errno));
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", fbdev.scanout.w, fbdev.scanout.h);
    for (int y = 0; y < fbdev.scanout.h; y++) {
        const Uint8 *row = fbdev.scanout.pixels + (size_t)y * fbdev.scanout.pitch;
        for (int x = 0; x < fbdev.scanout.w; x++) {
            const Uint32 xrgb = fbdev_unpack(&fbdev.layout,
                                             fbdev_load(&fbdev.layout, row + (size_t)x * (size_t)fbdev.layout.bytes));
            const Uint8 rgb[3] = { (Uint8)((xrgb >> 16) & 0xFF), (Uint8)((xrgb >> 8) & 0xFF), (Uint8)(xrgb & 0xFF) };
            if (fwrite(rgb, 1, sizeof(rgb), f) != sizeof(rgb)) {
                fbdev_log("short write to %s", fbdev.dump_path);
                fclose(f);
                return;
            }
        }
    }
    fclose(f);
    fbdev.dump_frames++;
}

/* Repaint everything from the window surface, cursor on top. Used on show,
   after a VT switch back, and once at init. */
static void fbdev_refresh(void)
{
    if (!fbdev.shadow_live) {
        return; /* nothing drawn yet: do not put black over the console */
    }
    fbdev_cursor_erase();
    fbdev_repaint(fbdev_onscreen_window(), NULL, 0);
    fbdev_cursor_draw();
    fbdev_dump_frame();
}

/* ------------------------------------------------------------------ */
/* Device setup                                                        */
/* ------------------------------------------------------------------ */

static bool fbdev_alloc8(Uint8 **slot, size_t *size, size_t bytes)
{
    free(*slot);
    *slot = NULL;
    *size = 0;
    if (bytes == 0) {
        return true;
    }
    *slot = (Uint8 *)calloc(1, bytes);
    if (!*slot) {
        return SDL_OutOfMemory();
    }
    *size = bytes;
    return true;
}

static bool fbdev_open_device(const char *path)
{
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;

    const int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return SDL_SetError("fbdev: cannot open %s: %s", path, strerror(errno));
    }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        SDL_SetError("fbdev: FBIOGET_FSCREENINFO failed: %s", strerror(errno));
        close(fd);
        return false;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        SDL_SetError("fbdev: FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        close(fd);
        return false;
    }
    if (var.bits_per_pixel != 16 && var.bits_per_pixel != 24 && var.bits_per_pixel != 32) {
        SDL_SetError("fbdev: unsupported colour depth (%u bpp)", var.bits_per_pixel);
        close(fd);
        return false;
    }
    if (var.xres == 0 || var.yres == 0 || var.xres > FBDEV_MAX_DIM || var.yres > FBDEV_MAX_DIM ||
        fix.line_length == 0 || var.yres_virtual == 0) {
        SDL_SetError("fbdev: implausible mode %ux%u (stride %u, virtual height %u)",
                     var.xres, var.yres, fix.line_length, var.yres_virtual);
        close(fd);
        return false;
    }
    const size_t min_pitch = (size_t)var.xres * ((size_t)var.bits_per_pixel / 8u);
    if ((size_t)fix.line_length < min_pitch) {
        SDL_SetError("fbdev: stride %u is too small for %u px at %u bpp",
                     fix.line_length, var.xres, var.bits_per_pixel);
        close(fd);
        return false;
    }
    const Uint64 length = (Uint64)fix.line_length * (Uint64)var.yres_virtual;
    if (length == 0 || length > (Uint64)SIZE_MAX) {
        SDL_SetError("fbdev: unusable scanout size");
        close(fd);
        return false;
    }

    /* validate the layout before believing it: a channel wider than a byte
       would make the shifting below shift by a negative amount, and offsets
       past the pixel would scribble on the next pixel */
    if (fix.type != FB_TYPE_PACKED_PIXELS || fix.visual != FB_VISUAL_TRUECOLOR ||
        var.nonstd || var.grayscale != 0 || var.red.msb_right || var.green.msb_right || var.blue.msb_right ||
        var.red.length == 0 || var.green.length == 0 || var.blue.length == 0 ||
        var.red.length > 8 || var.green.length > 8 || var.blue.length > 8 ||
        var.red.offset > var.bits_per_pixel - var.red.length ||
        var.green.offset > var.bits_per_pixel - var.green.length ||
        var.blue.offset > var.bits_per_pixel - var.blue.length) {
        SDL_SetError("fbdev: unusable pixel layout (bpp %u, R%u/%u G%u/%u B%u/%u, %s)",
                     var.bits_per_pixel, var.red.offset, var.red.length,
                     var.green.offset, var.green.length,
                     var.blue.offset, var.blue.length,
                     var.grayscale ? "greyscale" : "not a true-colour visual");
        close(fd);
        return false;
    }

    const Uint32 rm = ((1u << var.red.length) - 1u) << var.red.offset;
    const Uint32 gm = ((1u << var.green.length) - 1u) << var.green.offset;
    const Uint32 bm = ((1u << var.blue.length) - 1u) << var.blue.offset;
    if ((rm & gm) || (rm & bm) || (gm & bm)) {
        close(fd);
        return SDL_SetError("fbdev: overlapping colour channels");
    }

    fbdev.fd = fd;
    SDL_strlcpy(fbdev.path, path, sizeof(fbdev.path));
    fbdev.layout.bpp = (int)var.bits_per_pixel;
    fbdev.layout.bytes = fbdev.layout.bpp / 8;
    fbdev.layout.r_off = (Uint8)var.red.offset;
    fbdev.layout.r_len = (Uint8)var.red.length;
    fbdev.layout.g_off = (Uint8)var.green.offset;
    fbdev.layout.g_len = (Uint8)var.green.length;
    fbdev.layout.b_off = (Uint8)var.blue.offset;
    fbdev.layout.b_len = (Uint8)var.blue.length;
    fbdev.layout.native = (fbdev.layout.bpp == 32 &&
                           var.red.offset == 16 && var.red.length == 8 &&
                           var.green.offset == 8 && var.green.length == 8 &&
                           var.blue.offset == 0 && var.blue.length == 8);

    /* the console may have been left scrolled: the visible window starts at
       (xoffset, yoffset) inside the memory, so present there and nowhere
       else. We never scroll ourselves. */
    if ((Uint64)var.xoffset + var.xres > var.xres_virtual ||
        ((Uint64)var.xoffset + var.xres) * (var.bits_per_pixel / 8u) > fix.line_length ||
        (Uint64)var.yoffset + var.yres > var.yres_virtual ||
        ((Uint64)var.yoffset + var.yres) * fix.line_length > length ||
        (fix.smem_len && length > fix.smem_len) || var.vmode & FB_VMODE_YWRAP) {
        close(fd);
        fbdev.fd = -1;
        return SDL_SetError("fbdev: visible framebuffer or panning offset is out of range");
    }
    fbdev.pan_x = (int)var.xoffset;
    fbdev.pan_y = (int)var.yoffset;
    if (fbdev.pan_x || fbdev.pan_y) {
        fbdev_log("console left scrolled to %dx%d; presenting at that offset",
                  fbdev.pan_x, fbdev.pan_y);
    }

    void *map = mmap(NULL, (size_t)length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        /* the card refuses the mapping but the mode is still ours: keep
           painting in RAM so the app runs, and say so once */
        fbdev_log("mmap of %s failed (%s); frames stay in RAM", path, strerror(errno));
        map = NULL;
    }

    fbdev.scanout.w = (int)var.xres;
    fbdev.scanout.h = (int)var.yres;
    fbdev.scanout.pitch = (size_t)fix.line_length;
    fbdev.scanout.length = (size_t)length;
    fbdev.scanout.mapped = (map != NULL);

    bool ok = true;
    if (map) {
        fbdev.scanout.pixels = (Uint8 *)map;
    } else {
        ok = fbdev_alloc8(&fbdev.ram, &fbdev.ram_size, (size_t)length);
        fbdev.scanout.pixels = fbdev.ram;
        if (ok) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                        "fbdev: %s cannot be mapped; drawing into RAM, nothing will be visible "
                        "(the app still runs and reads its frames back)", path);
        }
    }
    if (!ok) {
        /* release everything this call took: the mapping, the RAM scanout */
        fbdev_scanout_free();
        free(fbdev.ram);
        fbdev.ram = NULL;
        fbdev.ram_size = 0;
        close(fd);
        fbdev.fd = -1;
        return false;
    }

    /* refresh rate from the timings; console modes often report 0 pixclock */
    fbdev.refresh_hz = 60;
    if (var.pixclock > 0) {
        const Uint32 htotal = var.left_margin + var.right_margin + var.xres + var.hsync_len;
        const Uint32 vtotal = var.upper_margin + var.lower_margin + var.yres + var.vsync_len;
        if (htotal > 0 && vtotal > 0) {
            const Uint64 pix_hz = 1000000000000ull / (Uint64)var.pixclock;
            const Uint64 hz = pix_hz / ((Uint64)htotal * (Uint64)vtotal);
            if (hz >= 1 && hz <= 240) {
                fbdev.refresh_hz = (Uint32)hz;
            }
        }
    }
    return true;
}

static bool fbdev_open_ram(const char *dump_path, int w, int h)
{
    fbdev.fd = -1;
    fbdev.dump = true;
    SDL_strlcpy(fbdev.dump_path, dump_path, sizeof(fbdev.dump_path));
    fbdev.scanout.w = w;
    fbdev.scanout.h = h;
    fbdev.scanout.pitch = (size_t)w * 4u;
    fbdev.scanout.length = (size_t)w * 4u * (size_t)h;
    fbdev.scanout.mapped = false;
    fbdev.layout.bpp = 32;
    fbdev.layout.bytes = 4;
    fbdev.layout.r_off = 16;
    fbdev.layout.r_len = 8;
    fbdev.layout.g_off = 8;
    fbdev.layout.g_len = 8;
    fbdev.layout.b_off = 0;
    fbdev.layout.b_len = 8;
    fbdev.layout.native = true;
    fbdev.refresh_hz = 60;
    if (!fbdev_alloc8(&fbdev.ram, &fbdev.ram_size, fbdev.scanout.length)) {
        return false;
    }
    fbdev.scanout.pixels = fbdev.ram;
    return true;
}

/* ------------------------------------------------------------------ */
/* Console (VT) ownership                                             */
/* ------------------------------------------------------------------ */

static void fbdev_vt_signal(int signum)
{
    if (signum == fbdev.vt.rel_sig) {
        fbdev_vt_rel_pending = 1;
    } else if (signum == fbdev.vt.acq_sig) {
        fbdev_vt_acq_pending = 1;
    }
}

static bool fbdev_vt_signal_free(int signum)
{
    struct sigaction current;

    memset(&current, 0, sizeof(current));
    if (sigaction(signum, NULL, &current) != 0) {
        return false;
    }
    return current.sa_handler == SIG_DFL;
}

static int fbdev_vt_find_signal(int exclude)
{
#ifdef SIGRTMIN
    for (int signum = SIGRTMIN + 2; signum <= SIGRTMAX; signum++) {
        if (signum != exclude && fbdev_vt_signal_free(signum)) {
            return signum;
        }
    }
#endif
    if (exclude != SIGUSR1 && fbdev_vt_signal_free(SIGUSR1)) {
        return SIGUSR1;
    }
    if (exclude != SIGUSR2 && fbdev_vt_signal_free(SIGUSR2)) {
        return SIGUSR2;
    }
    return 0;
}

static bool fbdev_vt_take_signals(void)
{
    struct sigaction action;
    sigset_t mask;

    fbdev_vt_rel_pending = 0;
    fbdev_vt_acq_pending = 0;

    memset(&action, 0, sizeof(action));
    action.sa_handler = fbdev_vt_signal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);

    if (sigaction(fbdev.vt.rel_sig, &action, &fbdev.vt.old_rel) != 0) {
        return false;
    }
    if (sigaction(fbdev.vt.acq_sig, &action, &fbdev.vt.old_acq) != 0) {
        sigaction(fbdev.vt.rel_sig, &fbdev.vt.old_rel, NULL);
        return false;
    }
    fbdev.vt.handlers_installed = true;

    sigemptyset(&mask);
    sigaddset(&mask, fbdev.vt.rel_sig);
    sigaddset(&mask, fbdev.vt.acq_sig);
    if (pthread_sigmask(SIG_BLOCK, &mask, &fbdev.vt.saved_mask) != 0) {
        fbdev_log("cannot block the VT signals; falling back to handlers");
        return true; /* the handlers still work on their own */
    }
    fbdev.vt.mask_saved = true;

    fbdev.vt.signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fbdev.vt.signal_fd < 0) {
        fbdev_log("signalfd failed (%s); using the handler flag", strerror(errno));
        pthread_sigmask(SIG_SETMASK, &fbdev.vt.saved_mask, NULL);
        fbdev.vt.mask_saved = false;
    }
    return true;
}

static void fbdev_vt_drop_signals(void)
{
    if (fbdev.vt.signal_fd >= 0) {
        close(fbdev.vt.signal_fd);
        fbdev.vt.signal_fd = -1;
    }
    if (fbdev.vt.mask_saved) {
        pthread_sigmask(SIG_SETMASK, &fbdev.vt.saved_mask, NULL);
        fbdev.vt.mask_saved = false;
    }
    if (fbdev.vt.handlers_installed) {
        sigaction(fbdev.vt.rel_sig, &fbdev.vt.old_rel, NULL);
        sigaction(fbdev.vt.acq_sig, &fbdev.vt.old_acq, NULL);
        fbdev.vt.handlers_installed = false;
    }
}

/* Take the console: graphics mode + keyboard off, so fbcon and the console
   keyboard handler stay out of the way while we own the VT. The keyboard is
   only switched off when SDLop has a keyboard path of its own: with raw input
   disabled or unavailable, K_OFF would also kill Ctrl+Alt+Fn (the kernel
   handles VT switching in its keyboard driver), Ctrl-C and SysRq. */
static void fbdev_console_take(void)
{
    if (fbdev.vt.fd < 0) {
        return;
    }
    /* a VT switch while we were away switches the console back to text, so
       do not trust our own flag here */
    {
        int mode = -1;
        if (ioctl(fbdev.vt.fd, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS) {
            fbdev.vt.graphics = true;
            return; /* already ours: mode and keyboard are as we left them */
        }
    }
    if (fbdev.vt.saved_kbmode < 0) {
        int kbmode = 0;
        if (ioctl(fbdev.vt.fd, KDGKBMODE, &kbmode) == 0) {
            fbdev.vt.saved_kbmode = kbmode;
        }
    }
    fbdev.vt.kb_off = SDLOP_RawInputHasKeyboard();
    if (!fbdev.vt.kb_off) {
        fbdev_log("no keyboard of our own (raw input off/unavailable): "
                  "leaving the console keyboard alone so Ctrl+Alt+Fn keeps working");
    } else if (fbdev.vt.saved_kbmode >= 0 && ioctl(fbdev.vt.fd, KDSKBMODE, K_OFF) != 0) {
        fbdev_log("KDSKBMODE K_OFF failed: %s", strerror(errno));
        fbdev.vt.kb_off = false;
    } else {
        fbdev.vt.kb_off = fbdev.vt.saved_kbmode >= 0;
    }
    if (ioctl(fbdev.vt.fd, KDSETMODE, KD_GRAPHICS) != 0) {
        fbdev_log("KDSETMODE KD_GRAPHICS failed: %s", strerror(errno));
        return;
    }
    fbdev.vt.graphics = true;
    if (fbdev.vt.kb_off) {
        /* the console keyboard is in K_OFF, so raw input is the only way the
           application sees keys - and it has to forward Ctrl+Alt+Fn for us */
        SDLOP_RawInputAttachConsole(fbdev.vt.fd);
        fbdev.vt.raw_attached = true;
    }
    {
        int mode = -1;
        if (ioctl(fbdev.vt.fd, KDGETMODE, &mode) == 0 && mode != KD_GRAPHICS) {
            fbdev_log("VT %d reports mode %d right after KD_GRAPHICS", fbdev.vt.num, mode);
        }
    }
}

/* Give the console back in text mode with its keyboard handler restored. */
static void fbdev_console_release(void)
{
    if (fbdev.vt.fd < 0) {
        return;
    }
    fbdev_cursor_erase();
    if (fbdev.vt.raw_attached) {
        SDLOP_RawInputDetachConsole();
        fbdev.vt.raw_attached = false;
    }
    if (fbdev.vt.graphics) {
        if (ioctl(fbdev.vt.fd, KDSETMODE, KD_TEXT) != 0) {
            fbdev_log("KDSETMODE KD_TEXT failed: %s", strerror(errno));
        }
        fbdev.vt.graphics = false;
    }
    if (fbdev.vt.kb_off && fbdev.vt.saved_kbmode >= 0) {
        if (ioctl(fbdev.vt.fd, KDSKBMODE, fbdev.vt.saved_kbmode) != 0) {
            fbdev_log("KDSKBMODE restore failed: %s", strerror(errno));
        }
    }
    fbdev.vt.kb_off = false;
}

/* VT switch requested by the kernel: hand the console over. */
static void fbdev_vt_release(void)
{
    if (fbdev.vt.fd < 0) {
        return;
    }
    fbdev.vt.active = false;
    fbdev_console_release();
    if (fbdev.vt.process_mode && ioctl(fbdev.vt.fd, VT_RELDISP, 1) != 0) {
        fbdev_log("VT_RELDISP(1) failed: %s", strerror(errno));
    }
}

/* Coming back: retake the console and repaint from the window surface. */
static void fbdev_vt_acquire(void)
{
    if (fbdev.vt.fd < 0) {
        return;
    }
    fbdev.vt.active = true;
    fbdev_console_take();
    if (fbdev.vt.process_mode && ioctl(fbdev.vt.fd, VT_RELDISP, VT_ACKACQ) != 0) {
        fbdev_log("VT_RELDISP(VT_ACKACQ) failed: %s", strerror(errno));
    }
    fbdev_refresh();
    SDL_Window *window = fbdev_onscreen_window();
    if (window) {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_EXPOSED, 0, 0);
    }
}

static bool fbdev_vt_onscreen(int fd, int num)
{
    struct vt_stat state;

    if (num <= 0) {
        return false;
    }
    memset(&state, 0, sizeof(state));
    if (ioctl(fd, VT_GETSTATE, &state) != 0) {
        return true; /* cannot tell: assume we are visible */
    }
    return state.v_active == (unsigned short)num;
}

/* Open a console device only if it really is a virtual terminal: the VT
   ioctls are the test (a serial port has a kbd type but no VT). */
static int fbdev_open_vt(const char *path, int *vt_num)
{
    const int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    struct vt_mode mode;
    struct vt_stat state;

    if (fd < 0) {
        return -1;
    }
    memset(&mode, 0, sizeof(mode));
    if (ioctl(fd, VT_GETMODE, &mode) != 0) {
        close(fd);
        return -1; /* not a VT (e.g. /dev/tty bound to a serial console) */
    }
    if (vt_num) {
        memset(&state, 0, sizeof(state));
        *vt_num = (ioctl(fd, VT_GETSTATE, &state) == 0) ? (int)state.v_active : 0;
    }
    return fd;
}

/* Which VT to own. $TTYNO names one, then the controlling VT, then the VT
   on screen, otherwise the first usable console, otherwise a free one from
   VT_OPENQRY. Nothing here activates a VT: the picture appears where the user
   already is, which is the whole point of not being kmsdrm's VT_ACTIVATE. */
static bool fbdev_vt_open_target(void)
{
    const char *ttyno = SDL_getenv("TTYNO");
    char path[32];

    if (ttyno && *ttyno) {
        char *end = NULL;
        const unsigned long wanted = SDL_strtoul(ttyno, &end, 10);
        if (wanted == 0 || !end || *end != '\0' || wanted > 63) {
            fbdev_log("invalid $TTYNO '%s'; ignoring", ttyno);
        } else {
            SDL_snprintf(path, sizeof(path), "/dev/tty%lu", wanted);
            fbdev.vt.fd = fbdev_open_vt(path, NULL);
            if (fbdev.vt.fd >= 0) {
                fbdev.vt.num = (int)wanted;
                return true;
            }
            fbdev_log("$TTYNO console %s is not usable: %s", path, strerror(errno));
        }
    }

    /* Prefer the controlling VT even when a different VT is on screen.
       TIOCGDEV resolves /dev/tty to its actual device, unlike VT_GETSTATE,
       which reports the foreground VT rather than this fd's identity. */
    int controlling = fbdev_open_vt("/dev/tty", NULL);
    if (controlling >= 0) {
        unsigned int dev = 0;
        if (ioctl(controlling, TIOCGDEV, &dev) == 0 && major(dev) == 4 &&
            minor(dev) > 0 && minor(dev) <= 63) {
            fbdev.vt.fd = controlling;
            fbdev.vt.num = (int)minor(dev);
            return true;
        }
        close(controlling);
    }

    /* the active VT, as the kernel reports it through /dev/tty0 */
    int active = 0;
    const int tty0 = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (tty0 >= 0) {
        struct vt_stat state;
        memset(&state, 0, sizeof(state));
        if (ioctl(tty0, VT_GETSTATE, &state) == 0) {
            active = (int)state.v_active;
        } else {
            fbdev_log("VT_GETSTATE on /dev/tty0 failed: %s", strerror(errno));
        }
    }
    if (active > 0) {
        char path_active[32];
        SDL_snprintf(path_active, sizeof(path_active), "/dev/tty%d", active);
        fbdev.vt.fd = fbdev_open_vt(path_active, NULL);
        if (fbdev.vt.fd >= 0) {
            fbdev.vt.num = active;
        } else {
            fbdev_log("cannot open the active VT %s: %s", path_active, strerror(errno));
        }
    }
    if (fbdev.vt.fd < 0 && tty0 >= 0) {
        /* /dev/tty0 works for the takeover even when the numbered node does
           not exist yet; the VT number is the one we just read */
        fbdev.vt.fd = tty0;
        fbdev.vt.num = active;
    } else if (tty0 >= 0) {
        close(tty0);
    }
    if (fbdev.vt.fd >= 0) {
        return true;
    }

    for (int n = 1; n <= 63; n++) {
        SDL_snprintf(path, sizeof(path), "/dev/tty%d", n);
        fbdev.vt.fd = fbdev_open_vt(path, NULL);
        if (fbdev.vt.fd >= 0) {
            fbdev.vt.num = n;
            return true;
        }
    }

    /* last resort: ask the kernel for an unused VT (still no activation) */
    const int tty0b = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (tty0b >= 0) {
        int free_vt = 0;
        if (ioctl(tty0b, VT_OPENQRY, &free_vt) == 0 && free_vt > 0 && free_vt <= 63) {
            SDL_snprintf(path, sizeof(path), "/dev/tty%d", free_vt);
            fbdev.vt.fd = fbdev_open_vt(path, NULL);
            if (fbdev.vt.fd >= 0) {
                fbdev.vt.num = free_vt;
                close(tty0b);
                return true;
            }
        }
        close(tty0b);
    }
    return false;
}

/* Set up console ownership. Failure is not fatal: the picture still appears
   on the framebuffer, only VT switching is left to the kernel. */
static void fbdev_vt_setup(void)
{
    fbdev.vt.fd = -1;
    fbdev.vt.saved_kbmode = -1;
    fbdev.vt.signal_fd = -1;
    fbdev.vt.active = true;

    if (fbdev.dump) {
        return;
    }
    if (!fbdev_replace8("SDLOP_FBDEV_TAKEOVER", true)) {
        fbdev_log("console takeover disabled (SDLOP_FBDEV_TAKEOVER)");
        return;
    }
    if (!fbdev_vt_open_target()) {
        fbdev_log("no VT console available; VT switching left to the kernel");
        return;
    }

    char kbtype = 0;
    if (ioctl(fbdev.vt.fd, KDGKBTYPE, &kbtype) != 0 || (kbtype != KB_101 && kbtype != KB_84)) {
        fbdev_log("%s is not a keyboard console; leaving VT_AUTO",
                  fbdev.vt.num ? "the VT" : "/dev/tty");
        close(fbdev.vt.fd);
        fbdev.vt.fd = -1;
        return;
    }

    /* Refuse a VT that is already somebody else's console: a graphics-mode VT
       or one driven in VT_PROCESS belongs to another program (compositor,
       kiosk, another SDL app). Taking it over would put KD_TEXT and VT_AUTO
       onto their screen when we quit. */
    {
        int mode = -1;
        struct vt_mode existing;
        memset(&existing, 0, sizeof(existing));
        if ((ioctl(fbdev.vt.fd, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS) ||
            (ioctl(fbdev.vt.fd, VT_GETMODE, &existing) == 0 && existing.mode == VT_PROCESS)) {
            fbdev_log("VT %d is already owned by another program (mode=%d vt_mode=%d); "
                      "leaving the console alone", fbdev.vt.num, mode, (int)existing.mode);
            close(fbdev.vt.fd);
            fbdev.vt.fd = -1;
            return;
        }
    }

    if (fbdev.vt.num == 0) {
        struct vt_stat state;
        memset(&state, 0, sizeof(state));
        if (ioctl(fbdev.vt.fd, VT_GETSTATE, &state) == 0) {
            fbdev.vt.num = (int)state.v_active;
        }
    }

    fbdev.vt.rel_sig = fbdev_vt_find_signal(0);
    fbdev.vt.acq_sig = fbdev_vt_find_signal(fbdev.vt.rel_sig);
    if (!fbdev.vt.rel_sig || !fbdev.vt.acq_sig) {
        fbdev_log("no free signals for VT switching; leaving VT_AUTO");
        fbdev.vt.rel_sig = fbdev.vt.acq_sig = 0;
        close(fbdev.vt.fd);
        fbdev.vt.fd = -1;
        return;
    }
    if (!fbdev_vt_take_signals()) {
        fbdev_log("cannot take the VT signals; leaving VT_AUTO");
        fbdev_vt_drop_signals();
        fbdev.vt.rel_sig = fbdev.vt.acq_sig = 0;
        close(fbdev.vt.fd);
        fbdev.vt.fd = -1;
        return;
    }

    struct vt_mode mode;
    memset(&mode, 0, sizeof(mode));
    mode.mode = VT_PROCESS;
    mode.relsig = (short)fbdev.vt.rel_sig;
    mode.acqsig = (short)fbdev.vt.acq_sig;
    mode.frsig = 0;
    if (ioctl(fbdev.vt.fd, VT_SETMODE, &mode) != 0) {
        fbdev_log("VT_SETMODE(VT_PROCESS) failed (%s); leaving VT_AUTO", strerror(errno));
        fbdev_vt_drop_signals();
        fbdev.vt.rel_sig = fbdev.vt.acq_sig = 0;
        close(fbdev.vt.fd);
        fbdev.vt.fd = -1;
        return;
    }
    fbdev.vt.process_mode = true;

    fbdev_console_take();
    fbdev.vt.active = fbdev_vt_onscreen(fbdev.vt.fd, fbdev.vt.num);

    /* opt-in: switch to the VT we were asked to run on */
    const bool want_switch = fbdev_replace8("SDLOP_FBDEV_VT_SWITCH", false) ||
                             (SDL_getenv("TTYNO") && *SDL_getenv("TTYNO"));
    if (want_switch && fbdev.vt.num > 0 && !fbdev.vt.active) {
        if (ioctl(fbdev.vt.fd, VT_ACTIVATE, fbdev.vt.num) == 0) {
            /* asynchronous on purpose: VT_WAITACTIVE would block until this
               process acknowledges the acquire from the event loop, which has
               not started yet */
            fbdev_log("asked for VT %d", fbdev.vt.num);
        } else {
            fbdev_log("VT_ACTIVATE(%d) failed: %s", fbdev.vt.num, strerror(errno));
        }
    }
    if (!fbdev.vt.active) {
        fbdev_log("VT %d is not the one on screen; the picture appears when it is", fbdev.vt.num);
    }
    fbdev_log("console on VT %d (relsig=%d acqsig=%d)", fbdev.vt.num,
              fbdev.vt.rel_sig, fbdev.vt.acq_sig);
}

/* Undo everything fbdev_vt_setup did: text mode, keyboard back, VT_AUTO. */
static void fbdev_vt_shutdown(void)
{
    if (fbdev.vt.fd >= 0) {
        fbdev_console_release(); /* KD_TEXT, keyboard back, cursor gone */
        if (fbdev.vt.process_mode) {
            struct vt_mode mode;
            memset(&mode, 0, sizeof(mode));
            mode.mode = VT_AUTO;
            if (ioctl(fbdev.vt.fd, VT_SETMODE, &mode) != 0) {
                fbdev_log("VT_SETMODE(VT_AUTO) failed: %s", strerror(errno));
            }
            fbdev.vt.process_mode = false;
        }
        close(fbdev.vt.fd);
        fbdev.vt.fd = -1;
    }
    fbdev_vt_drop_signals();
    fbdev.vt.rel_sig = fbdev.vt.acq_sig = 0;
}

/* Drain VT switch requests: signalfd first, handler flags as a fallback. */
static void fbdev_vt_pump(void)
{
    if (fbdev.vt.fd < 0) {
        return;
    }
    bool release = false, acquire = false;

    if (fbdev.vt.signal_fd >= 0) {
        for (;;) {
            struct signalfd_siginfo info;
            const ssize_t got = read(fbdev.vt.signal_fd, &info, sizeof(info));
            if (got != (ssize_t)sizeof(info)) {
                break; /* EAGAIN, or a short read we cannot use */
            }
            if (info.ssi_signo == (Uint32)fbdev.vt.rel_sig) {
                release = true;
            } else if (info.ssi_signo == (Uint32)fbdev.vt.acq_sig) {
                acquire = true;
            }
        }
    }
    if (fbdev_vt_rel_pending) {
        fbdev_vt_rel_pending = 0;
        release = true;
    }
    if (fbdev_vt_acq_pending) {
        fbdev_vt_acq_pending = 0;
        acquire = true;
    }
    if (release) {
        fbdev_log("VT release requested");
        fbdev_vt_release();
    }
    if (acquire) {
        fbdev_log("VT acquire requested");
        fbdev_vt_acquire();
    }
}

/* ------------------------------------------------------------------ */
/* Driver entry points                                                */
/* ------------------------------------------------------------------ */

static bool fbdev_Init(SDLop_VideoDevice *device)
{
    (void)device;
    memset(&fbdev, 0, sizeof(fbdev));
    fbdev.fd = -1;
    fbdev.vt.fd = -1;
    fbdev.vt.saved_kbmode = -1;
    fbdev.vt.signal_fd = -1;
    fbdev.cursor_enabled = fbdev_replace8("SDLOP_FBDEV_CURSOR", true);

    /* dump mode: SDLOP_FBDEV_DUMP=<file> keeps everything in RAM so CI (and
       any machine without a console framebuffer) can verify the scanout */
    const char *dump = SDL_getenv("SDLOP_FBDEV_DUMP");
    if (dump && *dump) {
        int w = FBDEV_DEFAULT_W;
        int h = FBDEV_DEFAULT_H;
        const char *size = SDL_getenv("SDLOP_FBDEV_SIZE");
        if (size && *size) {
            int pw = 0, ph = 0;
            if (SDL_sscanf(size, "%dx%d", &pw, &ph) == 2 && pw > 0 && ph > 0 &&
                pw <= FBDEV_MAX_DIM && ph <= FBDEV_MAX_DIM) {
                w = pw;
                h = ph;
            } else {
                fbdev_log("ignoring SDLOP_FBDEV_SIZE '%s'", size);
            }
        }
        if (!fbdev_open_ram(dump, w, h)) {
            return false;
        }
    } else {
        const char *path = SDL_getenv("SDLOP_FBDEV_PATH");
        if (!path || !*path) {
            path = "/dev/fb0";
        }
        if (!fbdev_open_device(path)) {
            return false;
        }
    }

    fbdev.display = SDLOP_AddDisplay(fbdev.dump ? "fbdev-ram" : fbdev.path,
                                     fbdev.scanout.w, fbdev.scanout.h,
                                     (int)fbdev.refresh_hz * 1000,
                                     SDL_ORIENTATION_LANDSCAPE, NULL, sdlop.init_done);
    if (!fbdev.display) {
        fbdev_scanout_free();
        free(fbdev.ram);
        fbdev.ram = NULL;
        if (fbdev.fd >= 0) {
            close(fbdev.fd);
            fbdev.fd = -1;
        }
        return false;
    }

    /* Nothing is written to the scanout here: until the app presents, the
       card belongs to whoever was on the console, and the shadow already
       starts black. */
    fbdev_vt_setup();
    return true;
}

static void fbdev_Quit(SDLop_VideoDevice *device)
{
    (void)device;
    fbdev_cursor_erase();
    if (fbdev.display) {
        SDLOP_DelDisplay(fbdev.display, false);
        fbdev.display = 0;
    }
    fbdev_vt_shutdown();
    fbdev_scanout_free();
    if (fbdev.fd >= 0) {
        close(fbdev.fd);
        fbdev.fd = -1;
    }
    free(fbdev.ram);
    fbdev.ram = NULL;
    fbdev.ram_size = 0;
    fbdev.shadow_live = false;
    memset(fbdev.windows, 0, sizeof(fbdev.windows));
    fbdev.cursor = NULL;
    fbdev.cursor_rect = (SDL_Rect){ 0, 0, 0, 0 };
    fbdev.dump = false;
}

/* The console has a single mode: every window is the scanout. */
static void fbdev_snap_window(SDL_Window *window)
{
    if (!window) {
        return;
    }
    window->w = fbdev.scanout.w;
    window->h = fbdev.scanout.h;
    window->x = 0;
    window->y = 0;
}

static bool fbdev_CreateWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (!fbdev_record(window, true)) {
        return SDL_SetError("fbdev: too many windows");
    }
    fbdev_snap_window(window);
    return true;
}

static void fbdev_DestroyWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    FBDEV_WindowRecord *rec = fbdev_record(window, false);
    if (rec) {
        memset(rec, 0, sizeof(*rec));
    }
    if (sdlop.keyboard_focus == window) {
        sdlop.keyboard_focus = NULL;
    }
    if (sdlop.mouse_focus == window) {
        sdlop.mouse_focus = NULL;
    }
}

static void fbdev_ShowWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    FBDEV_WindowRecord *rec = fbdev_record(window, true);
    if (rec) {
        rec->visible = true;
    }
    sdlop.keyboard_focus = window;
    sdlop.mouse_focus = window;
    window->flags |= SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
    fbdev_refresh();
}

static void fbdev_HideWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    FBDEV_WindowRecord *rec = fbdev_record(window, false);
    if (rec) {
        rec->visible = false;
    }
    window->flags &= ~(SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
    if (sdlop.keyboard_focus == window) {
        sdlop.keyboard_focus = NULL;
    }
    if (sdlop.mouse_focus == window) {
        sdlop.mouse_focus = NULL;
    }
}

static bool fbdev_SetWindowTitle(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    return true;
}

static bool fbdev_SetWindowSize(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    /* the console mode is not negotiable: snap back and report the real size.
       The window surface always matches the scanout, so there is nothing to
       reallocate here. */
    fbdev_snap_window(window);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, window->w, window->h);
    return true;
}

static bool fbdev_SetWindowPosition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    window->x = 0;
    window->y = 0;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOVED, 0, 0);
    return true;
}

static void fbdev_MinimizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window; /* nothing to minimize on a console */
}

static void fbdev_MaximizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window; /* always the full console */
}

static void fbdev_RestoreWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
}

static bool fbdev_SetWindowFullscreen(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    /* the console is always fullscreen; report the transition so apps that
       wait for the event keep working */
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_ENTER_FULLSCREEN, 0, 0);
    } else {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 0, 0);
    }
    fbdev_snap_window(window);
    return true;
}

static void fbdev_RaiseWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    fbdev_ShowWindow(device, window);
}

static void fbdev_SetWindowClearColor(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
}

static bool fbdev_SetWindowRelativeMouseMode(SDLop_VideoDevice *device, SDL_Window *window, bool enabled)
{
    (void)device;
    (void)window;
    (void)enabled; /* the console has no pointer device to lock */
    return true;
}

static void fbdev_PumpEvents(SDLop_VideoDevice *device, int timeout_ms)
{
    (void)device;
    (void)timeout_ms; /* the core owns waiting; blocking here would stall it */

    fbdev_vt_pump();
    SDLOP_KeyboardProcessRepeats();

    /* follow the pointer: the input layer owns the logical position, we only
       have to keep the drawn cursor under it. With no cursor object yet,
       SDL_CursorVisible() is the core's answer for "is the pointer shown". */
    const bool pointer_shown = fbdev.cursor ? true : SDLOP_CursorVisible();
    const bool moved = sdlop.mouse_x != fbdev.cursor_drawn_x || sdlop.mouse_y != fbdev.cursor_drawn_y;
    if (pointer_shown && fbdev_console_visible() && (moved || SDL_RectEmpty(&fbdev.cursor_rect))) {
        fbdev_cursor_erase();
        fbdev_cursor_draw();
    }
}

static int fbdev_GetEventFD(SDLop_VideoDevice *device)
{
    (void)device;
    /* expose the VT signalfd so SDL_WaitEvent wakes on Ctrl+Alt+Fn */
    return fbdev.vt.fd >= 0 ? fbdev.vt.signal_fd : -1;
}

/* ---- software scanout ---- */

static bool fbdev_CreateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface **surface)
{
    (void)device;
    (void)window;
    /* Each window owns a RAM shadow. It never aliases scanout memory or
       another window's surface, even in the native 32-bit mode. */
    SDL_Surface *created = SDL_CreateSurface(fbdev.scanout.w, fbdev.scanout.h,
                                             SDL_PIXELFORMAT_XRGB8888);
    if (!created) {
        return false;
    }
    *surface = created;
    return true;
}

static bool fbdev_UpdateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    (void)device;
    if (!fbdev_scanout_ready()) {
        return SDL_SetError("fbdev: no scanout memory");
    }
    if (!window || !window->surface || !window->surface->pixels) {
        return SDL_SetError("fbdev: window surface is not the driver's");
    }
    fbdev.shadow_live = true;

    /* While another VT is in front, framebuffer memory belongs to that VT's
       text console: presenting now would paint over it. Keep the pixels in the
       shadow and let the switch back repaint. */
    if (!fbdev_console_visible()) {
        return true;
    }

    /* lift the old cursor first (it lives in the scanout only), publish the
       shadow, put the cursor back */
    fbdev_cursor_erase();
    fbdev_repaint(window, rects, numrects);
    fbdev_cursor_draw();
    fbdev_dump_frame();
    return true;
}

static void fbdev_DestroyWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (!window || !window->surface) {
        return;
    }
    SDL_DestroySurface(window->surface);
    window->surface = NULL;
}

/* ---- SDL 3.4 window management: the console has no window manager ---- */

static void fbdev_WindowNoop(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
}

static void fbdev_WindowNoopBool(SDLop_VideoDevice *device, SDL_Window *window, bool on)
{
    (void)device;
    (void)window;
    (void)on;
}

static bool fbdev_WindowOkBool(SDLop_VideoDevice *device, SDL_Window *window, bool on)
{
    (void)device;
    (void)window;
    (void)on;
    return true;
}

static bool fbdev_GetWindowBordersSize(SDLop_VideoDevice *device, SDL_Window *window, int *top, int *left, int *bottom, int *right)
{
    (void)device;
    (void)window;
    if (top) {
        *top = 0;
    }
    if (left) {
        *left = 0;
    }
    if (bottom) {
        *bottom = 0;
    }
    if (right) {
        *right = 0;
    }
    return true;
}

static bool fbdev_SetWindowOpacity(SDLop_VideoDevice *device, SDL_Window *window, float opacity)
{
    (void)device;
    (void)window;
    (void)opacity;
    return true;
}

static bool fbdev_SetWindowParent(SDLop_VideoDevice *device, SDL_Window *window, SDL_Window *parent)
{
    (void)device;
    (void)window;
    (void)parent;
    return SDL_Unsupported();
}

static bool fbdev_SetWindowIcon(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface *icon)
{
    (void)device;
    (void)window;
    (void)icon;
    return true;
}

static bool fbdev_SetWindowMouseRect(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    return true;
}

static bool fbdev_FlashWindow(SDLop_VideoDevice *device, SDL_Window *window, SDL_FlashOperation operation)
{
    (void)device;
    (void)window;
    (void)operation;
    return true;
}

static bool fbdev_SetWindowFramebufferVSync(SDLop_VideoDevice *device, SDL_Window *window, int vsync)
{
    (void)device;
    (void)window;
    (void)vsync;
    return true; /* presenting is a memory copy; there is nothing to sync to */
}

static bool fbdev_GetWindowFramebufferVSync(SDLop_VideoDevice *device, SDL_Window *window, int *vsync)
{
    (void)device;
    (void)window;
    if (vsync) {
        *vsync = 0;
    }
    return true;
}

static bool fbdev_CreateCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    /* nothing to upload: the cursor is composited in software at present */
    return cursor != NULL;
}

static void fbdev_FreeCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    if (fbdev.cursor == cursor) {
        fbdev_cursor_erase();
        fbdev.cursor = NULL;
    }
}

static bool fbdev_ShowCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    fbdev_cursor_erase();
    fbdev.cursor = cursor; /* NULL hides */
    if (cursor) {
        fbdev_cursor_draw();
    }
    return true;
}

static bool fbdev_WarpMouse(SDLop_VideoDevice *device, SDL_Window *window, float x, float y)
{
    (void)device;
    (void)window;
    (void)x;
    (void)y;
    /* the core makes the motion event and updates the logical position; the
       cursor follows on the next pump/present. Claiming the warp would mean
       writing core mouse state from here. */
    return false;
}

SDLop_VideoDevice SDLop_fbdev_device = {
    "fbdev",
    fbdev_Init,
    fbdev_Quit,
    fbdev_CreateWindow,
    fbdev_DestroyWindow,
    fbdev_ShowWindow,
    fbdev_HideWindow,
    fbdev_SetWindowTitle,
    fbdev_SetWindowSize,
    fbdev_SetWindowPosition,
    fbdev_MinimizeWindow,
    fbdev_MaximizeWindow,
    fbdev_RestoreWindow,
    fbdev_SetWindowFullscreen,
    fbdev_RaiseWindow,
    fbdev_SetWindowClearColor,
    fbdev_SetWindowRelativeMouseMode,
    fbdev_PumpEvents,
    fbdev_GetEventFD,
    fbdev_CreateWindowFramebuffer,
    fbdev_UpdateWindowFramebuffer,
    fbdev_DestroyWindowFramebuffer,
    NULL, /* GL_CreateContext: the console has no GL present path */
    NULL, /* GL_MakeCurrent */
    NULL, /* GL_SwapBuffers */
    NULL, /* GL_DeleteContext */
    NULL, /* GL_GetProcAddress */
    NULL, /* GL_SetSwapInterval */
    NULL, /* GL_GetSwapInterval */
    NULL, /* Vulkan_CreateSurface */
    NULL, /* Vulkan_GetInstanceExtensions: not supported */
    /* optional hooks */
    .Vulkan_GetPresentationSupport = NULL,
    .supports_popups = false,
    .SetWindowMinimumSize = fbdev_WindowNoop,
    .SetWindowMaximumSize = fbdev_WindowNoop,
    .SetWindowAspectRatio = fbdev_WindowNoop,
    .GetWindowBordersSize = fbdev_GetWindowBordersSize,
    .SetWindowOpacity = fbdev_SetWindowOpacity,
    .SetWindowParent = fbdev_SetWindowParent,
    .SetWindowModal = fbdev_WindowOkBool,
    .SetWindowBordered = fbdev_WindowNoopBool,
    .SetWindowResizable = fbdev_WindowNoopBool,
    .SetWindowAlwaysOnTop = fbdev_WindowNoopBool,
    .SetWindowIcon = fbdev_SetWindowIcon,
    .SetWindowMouseGrab = fbdev_WindowOkBool,
    .SetWindowKeyboardGrab = fbdev_WindowOkBool,
    .SetWindowMouseRect = fbdev_SetWindowMouseRect,
    .FlashWindow = fbdev_FlashWindow,
    .SetWindowFocusable = fbdev_WindowOkBool,
    .SetWindowFramebufferVSync = fbdev_SetWindowFramebufferVSync,
    .GetWindowFramebufferVSync = fbdev_GetWindowFramebufferVSync,
    .CreateCursor = fbdev_CreateCursor,
    .FreeCursor = fbdev_FreeCursor,
    .ShowCursor = fbdev_ShowCursor,
    .WarpMouse = fbdev_WarpMouse,
};

#endif /* __linux__ */
