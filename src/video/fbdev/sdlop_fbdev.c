/*
  SDLop - fbdev (Linux framebuffer) video driver.

  Talks to /dev/fbNN with ioctl() + mmap() only: no library and no extra
  header is required (linux/fb.h comes with the kernel headers). The core
  library therefore keeps its "dlopen everything optional" promise and fbdev
  needs no pkg-config probe at build time.

  Console semantics: an fbdev display has exactly one mode, the one the
  kernel already programmed, so every window request snaps back to it.

  Headless verification: when SDLOP_FBDEV_DUMP<name> is set (e.g.
  SDLOP_FBDEV_DUMP=/tmp/frame.ppm) the driver opens no device at all and
  keeps the scanout in a RAM buffer of SDLOP_FBDEV_SIZE (default 1024x768),
  dumping each presented frame to `name` as a binary P6 PPM. That is what
  tests/test_fbdev.c runs in CI; SDLOP_FBDEV_PATH can point the driver at a
  different device node for real hardware.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define FBDEV_MAX_WINDOWS 8
#define FBDEV_DEFAULT_W 1024
#define FBDEV_DEFAULT_H 768

typedef struct FBDEV_WindowRecord
{
    SDL_Window *window;
    bool visible;
} FBDEV_WindowRecord;

static struct
{
    /* real device */
    int fd;
    char path[256];
    bool mapped;
    Uint8 *map;
    size_t maplen;
    int fb_w, fb_h;
    int fb_bpp;
    int fb_line_length;
    SDL_PixelFormat fb_format;
    Uint32 fb_red_offset, fb_green_offset, fb_blue_offset;
    /* RAM scanout (dump mode, or a card we could not mmap) */
    Uint8 *ram;
    bool dump;
    char dump_path[512];
    int dump_counter;
    Uint32 refresh_hz;
    SDL_DisplayID display;
    Uint8 *present; /* one framebuffer worth of converted pixels */
    FBDEV_WindowRecord windows[FBDEV_MAX_WINDOWS];
} fbdev;

/* ------------------------------------------------------------------ */
/* Scanout access                                                      */
/* ------------------------------------------------------------------ */

static Uint8 *fbdev_scanout(void)
{
    return fbdev.map ? fbdev.map : fbdev.ram;
}

static size_t fbdev_scanout_pitch(void)
{
    /* the real card may pad rows; our RAM scanout never does */
    return fbdev.mapped ? (size_t)fbdev.fb_line_length : (size_t)fbdev.fb_w * 4u;
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

    const size_t maplen = (size_t)fix.line_length * (size_t)var.yres_virtual;
    void *map = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        /* a card that will not let us map still has a usable mode: render
           into RAM and skip presentation rather than failing to start */
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "fbdev: mmap of %s failed (%s); scanout stays in RAM",
                    path, strerror(errno));
        map = NULL;
    }

    fbdev.fd = fd;
    SDL_strlcpy(fbdev.path, path, sizeof(fbdev.path));
    fbdev.map = (Uint8 *)map;
    fbdev.maplen = map ? maplen : 0;
    fbdev.mapped = (map != NULL);
    fbdev.fb_w = (int)var.xres;
    fbdev.fb_h = (int)var.yres;
    fbdev.fb_bpp = (int)var.bits_per_pixel;
    fbdev.fb_line_length = (int)fix.line_length;
    fbdev.fb_red_offset = var.red.offset;
    fbdev.fb_green_offset = var.green.offset;
    fbdev.fb_blue_offset = var.blue.offset;
    fbdev.fb_format = (var.bits_per_pixel == 16) ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_XRGB8888;
    if (var.bits_per_pixel == 32 && var.red.offset == 16 && var.green.offset == 8 && var.blue.offset == 0) {
        fbdev.fb_format = SDL_PIXELFORMAT_XRGB8888;
    }
    if (var.bits_per_pixel == 32 && var.red.offset == 0 && var.green.offset == 8 && var.blue.offset == 16) {
        fbdev.fb_format = SDL_PIXELFORMAT_XBGR8888;
    }

    /* a 0 Hz variable refresh is normal on the console; report a sane rate */
    if (var.upper_margin + var.lower_margin + var.yres + var.vsync_len > 0) {
        const Uint32 htotal = var.left_margin + var.right_margin + var.xres + var.hsync_len;
        const Uint32 vtotal = var.upper_margin + var.lower_margin + var.yres + var.vsync_len;
        if (htotal > 0 && vtotal > 0 && var.pixclock > 0) {
            const Uint64 pix_hz = 1000000000000ull / (Uint64)var.pixclock;
            fbdev.refresh_hz = (Uint32)(pix_hz / ((Uint64)htotal * (Uint64)vtotal));
        }
    }
    if (fbdev.refresh_hz < 1 || fbdev.refresh_hz > 240) {
        fbdev.refresh_hz = 60;
    }
    return true;
}

static bool fbdev_open_ram(const char *dump_path, int w, int h)
{
    free(fbdev.ram);
    fbdev.ram = (Uint8 *)calloc((size_t)w * 4u, (size_t)h);
    if (!fbdev.ram) {
        return SDL_OutOfMemory();
    }
    fbdev.map = NULL;
    fbdev.mapped = false;
    fbdev.fd = -1;
    fbdev.dump = true;
    SDL_strlcpy(fbdev.dump_path, dump_path, sizeof(fbdev.dump_path));
    fbdev.fb_w = w;
    fbdev.fb_h = h;
    fbdev.fb_bpp = 32;
    fbdev.fb_line_length = w * 4;
    fbdev.fb_format = SDL_PIXELFORMAT_XRGB8888;
    fbdev.fb_red_offset = 16;
    fbdev.fb_green_offset = 8;
    fbdev.fb_blue_offset = 0;
    fbdev.refresh_hz = 60;
    return true;
}

/* Write the scanout as a binary PPM (P6). Image IO is not linked into SDLop,
   so the 15-line encoder lives here (the dump path is a test affordance). */
static void fbdev_dump_frame(void)
{
    const Uint8 *src = fbdev_scanout();
    if (!fbdev.dump || !fbdev.dump_path[0] || !src) {
        return;
    }
    FILE *f = fopen(fbdev.dump_path, "wb");
    if (!f) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "fbdev: cannot write %s: %s", fbdev.dump_path, strerror(errno));
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", fbdev.fb_w, fbdev.fb_h);
    for (int y = 0; y < fbdev.fb_h; y++) {
        const Uint32 *row = (const Uint32 *)(src + (size_t)y * fbdev_scanout_pitch());
        for (int x = 0; x < fbdev.fb_w; x++) {
            const Uint32 px = row[x];
            const Uint8 rgb[3] = { (Uint8)((px >> 16) & 0xFF), (Uint8)((px >> 8) & 0xFF), (Uint8)(px & 0xFF) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    fbdev.dump_counter++;
}

/* 32-bit XRGB8888 -> the card's pixel layout */
static Uint32 fbdev_convert_pixel(Uint32 xrgb)
{
    const Uint32 r = (xrgb >> 16) & 0xFF;
    const Uint32 g = (xrgb >> 8) & 0xFF;
    const Uint32 b = xrgb & 0xFF;

    if (fbdev.fb_bpp == 16) {
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    return (r << fbdev.fb_red_offset) | (g << fbdev.fb_green_offset) | (b << fbdev.fb_blue_offset);
}

static void fbdev_present_window(SDL_Window *window)
{
    Uint8 *dst = fbdev_scanout();
    SDL_Surface *surface = window->surface;
    if (!dst || !surface || !surface->pixels) {
        return;
    }

    const size_t pitch = fbdev_scanout_pitch();
    const int copy_w = SDL_min(surface->w, fbdev.fb_w);
    const int copy_h = SDL_min(surface->h, fbdev.fb_h);

    if (fbdev.fb_bpp == 32 && fbdev.fb_red_offset == 16 && fbdev.fb_green_offset == 8 && fbdev.fb_blue_offset == 0) {
        /* native layout: straight row copies */
        const size_t bytes = (size_t)copy_w * 4u;
        for (int y = 0; y < copy_h; y++) {
            memcpy(dst + (size_t)y * pitch, (const Uint8 *)surface->pixels + (size_t)y * (size_t)surface->pitch, bytes);
        }
    } else {
        for (int y = 0; y < copy_h; y++) {
            const Uint32 *src = (const Uint32 *)((const Uint8 *)surface->pixels + (size_t)y * (size_t)surface->pitch);
            Uint8 *row = dst + (size_t)y * pitch;
            if (fbdev.fb_bpp == 32) {
                Uint32 *out = (Uint32 *)row;
                for (int x = 0; x < copy_w; x++) {
                    out[x] = fbdev_convert_pixel(src[x]);
                }
            } else {
                Uint16 *out = (Uint16 *)row;
                for (int x = 0; x < copy_w; x++) {
                    out[x] = (Uint16)fbdev_convert_pixel(src[x]);
                }
            }
        }
    }
    fbdev_dump_frame();
}

/* ------------------------------------------------------------------ */
/* Driver entry points                                                 */
/* ------------------------------------------------------------------ */

static bool fbdev_Init(SDLop_VideoDevice *device)
{
    (void)device;
    memset(&fbdev, 0, sizeof(fbdev));
    fbdev.fd = -1;

    /* dump mode: SDLOP_FBDEV_DUMP<name> keeps everything in RAM so CI (and
       any machine without a console framebuffer) can verify the scanout */
    const char *env = SDL_getenv("SDLOP_FBDEV_DUMP");
    if (env && *env) {
        int w = FBDEV_DEFAULT_W;
        int h = FBDEV_DEFAULT_H;
        const char *size = SDL_getenv("SDLOP_FBDEV_SIZE");
        if (size && *size) {
            int pw = 0, ph = 0;
            if (sscanf(size, "%dx%d", &pw, &ph) == 2 && pw > 0 && ph > 0) {
                w = pw;
                h = ph;
            }
        }
        if (!fbdev_open_ram(env, w, h)) {
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
        if (!fbdev.mapped) {
            const size_t bytes = (size_t)fbdev.fb_w * 4u * (size_t)fbdev.fb_h;
            fbdev.ram = (Uint8 *)calloc(1, bytes);
            if (!fbdev.ram) {
                return SDL_OutOfMemory();
            }
        }
    }

    fbdev.present = (Uint8 *)calloc((size_t)fbdev.fb_w * 4u, (size_t)fbdev.fb_h);
    if (!fbdev.present) {
        return SDL_OutOfMemory();
    }

    fbdev.display = SDLOP_AddDisplay(fbdev.dump ? "fbdev-ram" : fbdev.path,
                                     fbdev.fb_w, fbdev.fb_h, (int)fbdev.refresh_hz * 1000,
                                     SDL_ORIENTATION_LANDSCAPE, NULL, sdlop.init_done);
    if (!fbdev.display) {
        return false;
    }
    return true;
}

static void fbdev_Quit(SDLop_VideoDevice *device)
{
    (void)device;
    if (fbdev.display) {
        SDLOP_DelDisplay(fbdev.display, false);
        fbdev.display = 0;
    }
    if (fbdev.map) {
        munmap(fbdev.map, fbdev.maplen);
        fbdev.map = NULL;
        fbdev.maplen = 0;
    }
    if (fbdev.fd >= 0) {
        close(fbdev.fd);
        fbdev.fd = -1;
    }
    free(fbdev.ram);
    fbdev.ram = NULL;
    free(fbdev.present);
    fbdev.present = NULL;
    memset(fbdev.windows, 0, sizeof(fbdev.windows));
    fbdev.dump = false;
}

static FBDEV_WindowRecord *fbdev_record(SDL_Window *window, bool create)
{
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

/* The console has a single mode: every window is the scanout. */
static void fbdev_snap_window(SDL_Window *window)
{
    window->w = fbdev.fb_w;
    window->h = fbdev.fb_h;
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
    /* the console mode is not negotiable: snap back and, if the request was
       honoured by the core, tell the app the real size and keep the
       window surface allocated at scanout size */
    fbdev_snap_window(window);
    if (!window->surface) {
        SDL_Surface *surface = SDL_CreateSurface(fbdev.fb_w, fbdev.fb_h, SDL_PIXELFORMAT_XRGB8888);
        if (!surface) {
            return false;
        }
        window->surface = surface;
    } else if (window->surface->w != fbdev.fb_w || window->surface->h != fbdev.fb_h) {
        SDL_Surface *surface = SDL_CreateSurface(fbdev.fb_w, fbdev.fb_h, SDL_PIXELFORMAT_XRGB8888);
        if (!surface) {
            return false;
        }
        SDL_DestroySurface(window->surface);
        window->surface = surface;
    }
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, window->w, window->h);
    return true;
}

static bool fbdev_SetWindowPosition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    /* console: the only position is (0,0) */
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
    (void)window; /* already the full console */
}

static void fbdev_RestoreWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
}

static bool fbdev_SetWindowFullscreen(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    /* the console is always fullscreen; track the flag so the app sees the
       events it expects */
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_ENTER_FULLSCREEN, 0, 0);
    } else {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 0, 0);
    }
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
    (void)enabled; /* no pointer device on the console */
    return true;
}

static void fbdev_PumpEvents(SDLop_VideoDevice *device, int timeout_ms)
{
    (void)device;
    SDLOP_KeyboardProcessRepeats();
    if (timeout_ms > 0) {
        int waited = 0;
        while (waited < timeout_ms) {
            SDL_Delay(1);
            waited++;
        }
    }
}

static int fbdev_GetEventFD(SDLop_VideoDevice *device)
{
    (void)device;
    return -1;
}

/* software scanout: the window surface is the framebuffer */

static bool fbdev_CreateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface **surface)
{
    (void)device;
    Uint8 *pixels = fbdev_scanout();
    if (!pixels) {
        return SDL_SetError("fbdev: no scanout memory");
    }
    SDL_Surface *s = SDL_CreateSurfaceFrom(fbdev.fb_w, fbdev.fb_h, SDL_PIXELFORMAT_XRGB8888,
                                           pixels, (int)fbdev_scanout_pitch());
    if (!s) {
        return false;
    }
    *surface = s;
    return true;
}

static bool fbdev_UpdateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    (void)device;
    (void)rects;
    (void)numrects;
    /* the window surface already *is* the scanout when it could be mapped,
       so presenting is only a flush (dump mode) or a copy for cards whose
       pixel layout differs from XRGB8888 */
    if (!fbdev.mapped) {
        fbdev_present_window(window);
        return true;
    }
    if (fbdev.fb_bpp != 32 || fbdev.fb_red_offset != 16 || fbdev.fb_green_offset != 8 || fbdev.fb_blue_offset != 0) {
        fbdev_present_window(window);
        return true;
    }
    if (fbdev.dump) {
        fbdev_dump_frame();
    }
    return true;
}

static void fbdev_DestroyWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (window->surface) {
        /* the pixels are the scanout, not ours to free: detach first */
        window->surface->pixels = NULL;
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
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
    *top = *left = *bottom = *right = 0;
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
    return SDL_SetError("fbdev: no parent windows on the console");
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

static bool fbdev_CreateCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    (void)cursor;
    return true;
}

static bool fbdev_ShowCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    (void)cursor;
    return true;
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
    .CreateCursor = fbdev_CreateCursor,
    .ShowCursor = fbdev_ShowCursor,
};

#endif /* __linux__ */
