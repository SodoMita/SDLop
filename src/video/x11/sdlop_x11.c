/*
  SDLop - X11 video driver.

  This is a small Xlib backend for the SDL3-compatible window/event API:
  X11 owns the native windows and input dispatch, XImages back software
  window surfaces, and EGL/Vulkan are delegated to their X11 WSI modules.
  libX11 is loaded at runtime; it is never a link-time dependency.

  The event and window-manager paths follow the same general approach used
  by SDL3 and nyorain/swa, adapted to SDLop's driver vtable and event core.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"
#include "internal/scancode_evdev.h"

#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#define SDLOP_X11_MAX_WINDOW_DIMENSION 65535

typedef struct X11WindowData
{
    Window window;
    GC gc;
    XImage *image;
    SDL_PixelFormat image_format;

    int last_x, last_y;
    int last_w, last_h;
    bool mapped;
    bool fullscreen_requested;
    unsigned char key_down[256];

    /* relative mouse mode uses an X pointer grab and a center warp */
    bool warp_pending;
    int warp_x, warp_y;

    /* geometry restored when fullscreen is requested without a WM */
    bool fullscreen_saved;
    int saved_x, saved_y, saved_w, saved_h;
    bool maximized_saved;
    int max_saved_x, max_saved_y, max_saved_w, max_saved_h;

    /* GL presentation state (managed by sdlop_x11_gl.c) */
    void *gl_surface;
    bool gl_active;
} X11WindowData;

typedef struct X11DeviceData
{
    Display *display;
    Visual *visual;
    Window root;
    int screen;
    int depth;
    int width, height;
    SDL_DisplayID display_id;

    Atom wm_protocols;
    Atom wm_delete_window;
    Atom utf8_string;
    Atom net_wm_name;
    Atom net_wm_pid;
    Atom net_moveresize_window;
    Atom net_wm_state;
    Atom net_wm_state_fullscreen;
    Atom net_wm_state_max_horz;
    Atom net_wm_state_max_vert;
    Atom motif_wm_hints;
    bool window_manager_present;
    bool detectable_auto_repeat;

    Cursor invisible_cursor;
    SDL_Window *relative_window;
    bool symbols_loaded;
} X11DeviceData;

static X11DeviceData x11;

static X11WindowData *x11_window_data(SDL_Window *window)
{
    return window ? (X11WindowData *)window->driverdata : NULL;
}

Display *SDLOP_X11_GetDisplay(void)
{
    return x11.display;
}

Visual *SDLOP_X11_GetVisual(void)
{
    return x11.visual;
}

int SDLOP_X11_GetScreen(void)
{
    return x11.screen;
}

Window SDLOP_X11_GetWindowHandle(SDL_Window *window)
{
    X11WindowData *wd = x11_window_data(window);
    return wd ? wd->window : None;
}

void SDLOP_X11_SetGLActive(SDL_Window *window, bool active)
{
    X11WindowData *wd = x11_window_data(window);
    if (wd) {
        wd->gl_active = active;
    }
}

void **SDLOP_X11_GetGLSurfaceSlot(SDL_Window *window)
{
    X11WindowData *wd = x11_window_data(window);
    return wd ? &wd->gl_surface : NULL;
}

static SDL_Window *x11_find_window(Window native)
{
    for (SDL_Window *window = sdlop.windows; window; window = window->next) {
        X11WindowData *wd = x11_window_data(window);
        if (wd && wd->window == native) {
            return window;
        }
    }
    return NULL;
}

static bool x11_image_pixel_format(const XImage *image, SDL_PixelFormat *format)
{
    const unsigned long r = image->red_mask;
    const unsigned long g = image->green_mask;
    const unsigned long b = image->blue_mask;

    if (image->bits_per_pixel == 32) {
        if (r == 0x00ff0000ul && g == 0x0000ff00ul && b == 0x000000fful) {
            *format = SDL_PIXELFORMAT_XRGB8888;
            return true;
        }
        if (r == 0x000000fful && g == 0x0000ff00ul && b == 0x00ff0000ul) {
            *format = SDL_PIXELFORMAT_XBGR8888;
            return true;
        }
    } else if (image->bits_per_pixel == 24) {
        if (r == 0x00ff0000ul && g == 0x0000ff00ul && b == 0x000000fful) {
            *format = image->byte_order == LSBFirst ? SDL_PIXELFORMAT_BGR24 : SDL_PIXELFORMAT_RGB24;
            return true;
        }
        if (r == 0x000000fful && g == 0x0000ff00ul && b == 0x00ff0000ul) {
            *format = image->byte_order == LSBFirst ? SDL_PIXELFORMAT_RGB24 : SDL_PIXELFORMAT_BGR24;
            return true;
        }
    } else if (image->bits_per_pixel == 16 &&
               r == 0x0000f800ul && g == 0x000007e0ul && b == 0x0000001ful) {
        *format = SDL_PIXELFORMAT_RGB565;
        return true;
    }
    return false;
}

static XImage *x11_create_image(int width, int height, SDL_PixelFormat *format)
{
    if (!x11.display || !x11.visual || width <= 0 || height <= 0 ||
        width > SDLOP_X11_MAX_WINDOW_DIMENSION || height > SDLOP_X11_MAX_WINDOW_DIMENSION) {
        SDL_SetError("Unsupported X11 window surface size %dx%d", width, height);
        return NULL;
    }

    XImage *image = SDLOP_X11_XCreateImage(x11.display, x11.visual,
                                            (unsigned int)x11.depth, ZPixmap,
                                            0, NULL, (unsigned int)width,
                                            (unsigned int)height, 32, 0);
    if (!image) {
        SDL_SetError("XCreateImage failed");
        return NULL;
    }
    if (image->bytes_per_line <= 0 ||
        (size_t)height > SIZE_MAX / (size_t)image->bytes_per_line) {
        SDLOP_X11_XDestroyImage(image);
        SDL_SetError("Invalid XImage row size");
        return NULL;
    }
    if (!x11_image_pixel_format(image, format)) {
        int bpp = image->bits_per_pixel;
        unsigned long rm = image->red_mask, gm = image->green_mask, bm = image->blue_mask;
        SDLOP_X11_XDestroyImage(image);
        SDL_SetError("Unsupported X11 visual (%d bpp, masks %lx/%lx/%lx)", bpp, rm, gm, bm);
        return NULL;
    }

    size_t bytes = (size_t)image->bytes_per_line * (size_t)height;
    image->data = (char *)calloc(1, bytes);
    if (!image->data) {
        SDLOP_X11_XDestroyImage(image);
        SDL_OutOfMemory();
        return NULL;
    }
    return image;
}

static void x11_fill_surface_with_clear_color(SDL_Window *window)
{
    if (!window || !window->surface) {
        return;
    }
    Uint32 pixel = SDL_MapSurfaceRGB(window->surface, window->clear_r,
                                     window->clear_g, window->clear_b);
    SDL_FillSurfaceRect(window->surface, NULL, pixel);
}

/* Replace an existing XImage without invalidating SDL_Surface: its public
 * struct is retained across a resize and retargeted at the new pixel store. */
static bool x11_resize_image(SDL_Window *window, int width, int height)
{
    X11WindowData *wd = x11_window_data(window);
    if (!wd) {
        return SDL_SetError("Invalid X11 window");
    }
    if (wd->image && wd->image->width == width && wd->image->height == height) {
        return true;
    }

    SDL_PixelFormat format = SDL_PIXELFORMAT_UNKNOWN;
    XImage *replacement = x11_create_image(width, height, &format);
    if (!replacement) {
        return false;
    }

    XImage *old_image = wd->image;
    wd->image = replacement;
    wd->image_format = format;
    if (window->surface) {
        window->surface->format = format;
        window->surface->w = width;
        window->surface->h = height;
        window->surface->pitch = replacement->bytes_per_line;
        window->surface->pixels = replacement->data;
        x11_fill_surface_with_clear_color(window);
    }
    if (old_image) {
        SDLOP_X11_XDestroyImage(old_image);
    }
    return true;
}

static bool x11_create_window_framebuffer(SDLop_VideoDevice *device,
                                           SDL_Window *window,
                                           SDL_Surface **surface)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (!wd || !surface) {
        return SDL_SetError("Invalid X11 window surface request");
    }
    if (wd->gl_active) {
        return SDL_SetError("OpenGL windows do not have a software window surface");
    }
    if (!wd->gc) {
        wd->gc = SDLOP_X11_XCreateGC(x11.display, wd->window, 0, NULL);
        if (!wd->gc) {
            return SDL_SetError("XCreateGC failed");
        }
    }
    if (!wd->image && !x11_resize_image(window, window->w, window->h)) {
        return false;
    }

    SDL_Surface *result = SDL_CreateSurfaceFrom(wd->image->width, wd->image->height,
                                                 wd->image_format, wd->image->data,
                                                 wd->image->bytes_per_line);
    if (!result) {
        return false;
    }
    *surface = result;
    Uint32 clear = SDL_MapSurfaceRGB(result, window->clear_r,
                                    window->clear_g, window->clear_b);
    SDL_FillSurfaceRect(result, NULL, clear);
    return true;
}

static bool x11_update_window_framebuffer(SDLop_VideoDevice *device,
                                          SDL_Window *window,
                                          const SDL_Rect *rects, int numrects)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (!wd || !wd->image || !wd->gc || !window->surface || wd->gl_active) {
        return SDL_SetError("X11 window surface is not presentable");
    }

    const int image_w = wd->image->width;
    const int image_h = wd->image->height;
    if (rects && numrects > 0) {
        for (int i = 0; i < numrects; ++i) {
            int64_t x0 = rects[i].x;
            int64_t y0 = rects[i].y;
            int64_t x1 = x0 + (int64_t)rects[i].w;
            int64_t y1 = y0 + (int64_t)rects[i].h;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > image_w) x1 = image_w;
            if (y1 > image_h) y1 = image_h;
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            SDLOP_X11_XPutImage(x11.display, wd->window, wd->gc, wd->image,
                                (int)x0, (int)y0, (int)x0, (int)y0,
                                (unsigned int)(x1 - x0), (unsigned int)(y1 - y0));
        }
    } else {
        SDLOP_X11_XPutImage(x11.display, wd->window, wd->gc, wd->image,
                            0, 0, 0, 0, (unsigned int)image_w,
                            (unsigned int)image_h);
    }
    SDLOP_X11_XFlush(x11.display);
    return true;
}

static void x11_destroy_window_framebuffer(SDLop_VideoDevice *device,
                                            SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (!wd) {
        return;
    }
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
    if (wd->image) {
        SDLOP_X11_XDestroyImage(wd->image);
        wd->image = NULL;
    }
}

static void x11_set_window_title(SDL_Window *window)
{
    X11WindowData *wd = x11_window_data(window);
    if (!wd) {
        return;
    }
    const char *title = window->title ? window->title : "";
    SDLOP_X11_XStoreName(x11.display, wd->window, title);
    if (x11.net_wm_name && x11.utf8_string) {
        size_t len = strlen(title);
        if (len > (size_t)INT_MAX) {
            len = INT_MAX;
        }
        SDLOP_X11_XChangeProperty(x11.display, wd->window, x11.net_wm_name,
                                  x11.utf8_string, 8, PropModeReplace,
                                  (const unsigned char *)title, (int)len);
    }
    SDLOP_X11_XFlush(x11.display);
}

static void x11_set_size_hints(SDL_Window *window)
{
    X11WindowData *wd = x11_window_data(window);
    if (!wd) {
        return;
    }
    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    if (!(window->flags & SDL_WINDOW_RESIZABLE)) {
        hints.flags = PMinSize | PMaxSize;
        hints.min_width = hints.max_width = window->w;
        hints.min_height = hints.max_height = window->h;
    }
    SDLOP_X11_XSetWMNormalHints(x11.display, wd->window, &hints);
}

static void x11_set_decorations(SDL_Window *window)
{
    X11WindowData *wd = x11_window_data(window);
    if (!wd || !(window->flags & SDL_WINDOW_BORDERLESS) || !x11.motif_wm_hints) {
        return;
    }
    /* MWM_HINTS_DECORATIONS = 2; a zero decoration mask asks the WM for no
     * frame. The five-long layout is the Motif property ABI. */
    unsigned long hints[5] = { 2ul, 0ul, 0ul, 0ul, 0ul };
    SDLOP_X11_XChangeProperty(x11.display, wd->window, x11.motif_wm_hints,
                              x11.motif_wm_hints, 32, PropModeReplace,
                              (const unsigned char *)hints, 5);
}

static void x11_send_wm_state(SDL_Window *window, long action, Atom first, Atom second)
{
    X11WindowData *wd = x11_window_data(window);
    if (!wd || !x11.window_manager_present || !x11.net_wm_state) {
        return;
    }

    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = wd->window;
    event.xclient.message_type = x11.net_wm_state;
    event.xclient.format = 32;
    event.xclient.data.l[0] = action; /* 0=remove, 1=add, 2=toggle */
    event.xclient.data.l[1] = (long)first;
    event.xclient.data.l[2] = (long)second;
    event.xclient.data.l[3] = 1; /* source indication: normal application */
    SDLOP_X11_XSendEvent(x11.display, x11.root, False,
                         SubstructureRedirectMask | SubstructureNotifyMask,
                         &event);
    SDLOP_X11_XFlush(x11.display);
}

static void x11_set_window_flag_event(SDL_Window *window, SDL_WindowFlags flag,
                                      bool enabled, SDL_EventType on_event,
                                      SDL_EventType off_event)
{
    bool old = (window->flags & flag) != 0;
    if (old == enabled) {
        return;
    }
    if (enabled) {
        window->flags |= flag;
    } else {
        window->flags &= ~flag;
    }
    SDLOP_SendWindowEvent(window, enabled ? on_event : off_event, 0, 0);
}

static void x11_update_ewmh_state(SDL_Window *window)
{
    X11WindowData *wd = x11_window_data(window);
    if (!wd || !x11.net_wm_state) {
        return;
    }

    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *property = NULL;
    int result = SDLOP_X11_XGetWindowProperty(x11.display, wd->window,
                                               x11.net_wm_state, 0, 64,
                                               False, XA_ATOM, &actual_type,
                                               &actual_format, &nitems,
                                               &bytes_after, &property);
    if (result != Success ||
        (actual_type != None &&
         (actual_type != XA_ATOM || actual_format != 32))) {
        if (property) {
            SDLOP_X11_XFree(property);
        }
        return;
    }

    /* A WM may delete _NET_WM_STATE when the last state is removed. Treat
     * that as an empty atom list so fullscreen/maximize flags are cleared. */
    bool fullscreen = false;
    bool max_horz = false;
    bool max_vert = false;
    unsigned long *atoms = (unsigned long *)property;
    for (unsigned long i = 0; atoms && i < nitems; ++i) {
        Atom atom = (Atom)atoms[i];
        fullscreen |= atom == x11.net_wm_state_fullscreen;
        max_horz |= atom == x11.net_wm_state_max_horz;
        max_vert |= atom == x11.net_wm_state_max_vert;
    }
    SDLOP_X11_XFree(property);

    wd->fullscreen_requested = fullscreen;
    x11_set_window_flag_event(window, SDL_WINDOW_FULLSCREEN, fullscreen,
                              SDL_EVENT_WINDOW_ENTER_FULLSCREEN,
                              SDL_EVENT_WINDOW_LEAVE_FULLSCREEN);
    x11_set_window_flag_event(window, SDL_WINDOW_MAXIMIZED, max_horz && max_vert,
                              SDL_EVENT_WINDOW_MAXIMIZED,
                              SDL_EVENT_WINDOW_RESTORED);
}

static void x11_update_geometry(SDL_Window *window, int x, int y, int w, int h,
                                bool request_server, bool send_events)
{
    X11WindowData *wd = x11_window_data(window);
    if (!wd || w <= 0 || h <= 0) {
        return;
    }
    bool moved = wd->last_x != x || wd->last_y != y;
    bool resized = wd->last_w != w || wd->last_h != h;

    if (resized && wd->image &&
        (wd->image->width != w || wd->image->height != h)) {
        (void)x11_resize_image(window, w, h);
    }

    window->x = x;
    window->y = y;
    window->w = w;
    window->h = h;
    wd->last_x = x;
    wd->last_y = y;
    wd->last_w = w;
    wd->last_h = h;

    if (request_server) {
        if (x11.window_manager_present && x11.net_moveresize_window) {
            XEvent event;
            memset(&event, 0, sizeof(event));
            event.xclient.type = ClientMessage;
            event.xclient.window = wd->window;
            event.xclient.message_type = x11.net_moveresize_window;
            event.xclient.format = 32;
            /* gravity + X/Y/W/H flags + source indication (normal app). */
            event.xclient.data.l[0] = (long)NorthWestGravity |
                                      (1l << 8) | (1l << 9) |
                                      (1l << 10) | (1l << 11) |
                                      (1l << 12);
            event.xclient.data.l[1] = x;
            event.xclient.data.l[2] = y;
            event.xclient.data.l[3] = w;
            event.xclient.data.l[4] = h;
            SDLOP_X11_XSendEvent(x11.display, x11.root, False,
                                 SubstructureRedirectMask | SubstructureNotifyMask,
                                 &event);
        } else {
            SDLOP_X11_XMoveResizeWindow(x11.display, wd->window, x, y,
                                        (unsigned int)w, (unsigned int)h);
        }
        SDLOP_X11_XFlush(x11.display);
    }
    if (send_events && moved) {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOVED, x, y);
    }
    if (send_events && resized) {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, w, h);
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, w, h);
    }
}

static bool x11_Init(SDLop_VideoDevice *device)
{
    (void)device;
    memset(&x11, 0, sizeof(x11));

    if (!SDLOP_X11_LoadSymbols()) {
        return false;
    }
    x11.symbols_loaded = true;

    /* XInitThreads must be the first Xlib call in this process. SDLop does
     * all window/event work on the main thread, but enabling Xlib's locking
     * also makes EGL/GL use on other application threads well-defined. */
    if (!SDLOP_X11_XInitThreads()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "XInitThreads did not enable Xlib locking");
    }

    x11.display = SDLOP_X11_XOpenDisplay(NULL);
    if (!x11.display) {
        SDL_SetError("Could not connect to X11 display (check DISPLAY)");
        SDLOP_X11_UnloadSymbols();
        memset(&x11, 0, sizeof(x11));
        return false;
    }

    x11.screen = SDLOP_X11_XDefaultScreen(x11.display);
    x11.root = SDLOP_X11_XRootWindow(x11.display, x11.screen);
    x11.visual = SDLOP_X11_XDefaultVisual(x11.display, x11.screen);
    x11.depth = SDLOP_X11_XDefaultDepth(x11.display, x11.screen);
    x11.width = SDLOP_X11_XDisplayWidth(x11.display, x11.screen);
    x11.height = SDLOP_X11_XDisplayHeight(x11.display, x11.screen);
    if (!x11.visual || x11.width <= 0 || x11.height <= 0) {
        SDL_SetError("X11 returned an invalid default screen");
        SDLOP_X11_XCloseDisplay(x11.display);
        x11.display = NULL;
        SDLOP_X11_UnloadSymbols();
        memset(&x11, 0, sizeof(x11));
        return false;
    }

    x11.wm_protocols = SDLOP_X11_XInternAtom(x11.display, "WM_PROTOCOLS", False);
    x11.wm_delete_window = SDLOP_X11_XInternAtom(x11.display, "WM_DELETE_WINDOW", False);
    x11.utf8_string = SDLOP_X11_XInternAtom(x11.display, "UTF8_STRING", False);
    x11.net_wm_name = SDLOP_X11_XInternAtom(x11.display, "_NET_WM_NAME", False);
    x11.net_wm_pid = SDLOP_X11_XInternAtom(x11.display, "_NET_WM_PID", False);
    x11.net_moveresize_window = SDLOP_X11_XInternAtom(
        x11.display, "_NET_MOVERESIZE_WINDOW", False);
    x11.net_wm_state = SDLOP_X11_XInternAtom(x11.display, "_NET_WM_STATE", False);
    x11.net_wm_state_fullscreen = SDLOP_X11_XInternAtom(x11.display,
                                                        "_NET_WM_STATE_FULLSCREEN", False);
    x11.net_wm_state_max_horz = SDLOP_X11_XInternAtom(x11.display,
                                                      "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    x11.net_wm_state_max_vert = SDLOP_X11_XInternAtom(x11.display,
                                                      "_NET_WM_STATE_MAXIMIZED_VERT", False);
    x11.motif_wm_hints = SDLOP_X11_XInternAtom(x11.display, "_MOTIF_WM_HINTS", False);

    char wm_selection_name[32];
    snprintf(wm_selection_name, sizeof(wm_selection_name), "WM_S%d", x11.screen);
    Atom wm_selection = SDLOP_X11_XInternAtom(x11.display, wm_selection_name, False);
    x11.window_manager_present =
        SDLOP_X11_XGetSelectionOwner(x11.display, wm_selection) != None;

    Bool detectable = False;
    if (SDLOP_X11_XkbSetDetectableAutoRepeat(x11.display, True, &detectable)) {
        x11.detectable_auto_repeat = detectable != False;
    }

    /* A transparent 1x1 cursor for SDL relative mouse mode. Failure to create
     * it is non-fatal: pointer capture still works, only cursor hiding is lost. */
    {
        static const char empty_bitmap[1] = { 0 };
        Pixmap pixmap = SDLOP_X11_XCreateBitmapFromData(x11.display, x11.root,
                                                         empty_bitmap, 1, 1);
        if (pixmap != None) {
            XColor color;
            memset(&color, 0, sizeof(color));
            x11.invisible_cursor = SDLOP_X11_XCreatePixmapCursor(
                x11.display, pixmap, pixmap, &color, &color, 0, 0);
            SDLOP_X11_XFreePixmap(x11.display, pixmap);
        }
    }

    x11.display_id = SDLOP_AddDisplay("X11 display", x11.width, x11.height,
                                     0, SDL_ORIENTATION_LANDSCAPE, NULL,
                                     sdlop.init_done);
    if (!x11.display_id) {
        SDL_SetError("Could not register X11 display");
        if (x11.invisible_cursor) {
            SDLOP_X11_XFreeCursor(x11.display, x11.invisible_cursor);
        }
        SDLOP_X11_XCloseDisplay(x11.display);
        x11.display = NULL;
        SDLOP_X11_UnloadSymbols();
        memset(&x11, 0, sizeof(x11));
        return false;
    }

    SDLOP_X11_XSync(x11.display, False);
    return true;
}

static void x11_release_pointer(void)
{
    if (!x11.display || !x11.relative_window) {
        return;
    }
    X11WindowData *wd = x11_window_data(x11.relative_window);
    if (wd) {
        SDLOP_X11_XDefineCursor(x11.display, wd->window, None);
        wd->warp_pending = false;
    }
    SDLOP_X11_XUngrabPointer(x11.display, CurrentTime);
    SDLOP_X11_XFlush(x11.display);
    x11.relative_window = NULL;
}

static void x11_Quit(SDLop_VideoDevice *device)
{
    (void)device;
    x11_release_pointer();
    SDLOP_X11_GL_Quit();
    if (x11.invisible_cursor && x11.display) {
        SDLOP_X11_XFreeCursor(x11.display, x11.invisible_cursor);
    }
    if (x11.display) {
        if (x11.display_id) {
            SDLOP_DelDisplay(x11.display_id, false);
        }
        SDLOP_X11_XSync(x11.display, False);
        SDLOP_X11_XCloseDisplay(x11.display);
    }
    bool unload = x11.symbols_loaded;
    memset(&x11, 0, sizeof(x11));
    if (unload) {
        SDLOP_X11_UnloadSymbols();
    }
}

static bool x11_CreateWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (!x11.display || window->w <= 0 || window->h <= 0 ||
        window->w > SDLOP_X11_MAX_WINDOW_DIMENSION ||
        window->h > SDLOP_X11_MAX_WINDOW_DIMENSION) {
        return SDL_SetError("Unsupported X11 window size %dx%d", window->w, window->h);
    }

    X11WindowData *wd = (X11WindowData *)calloc(1, sizeof(*wd));
    if (!wd) {
        return SDL_OutOfMemory();
    }
    wd->last_x = window->x;
    wd->last_y = window->y;
    wd->last_w = window->w;
    wd->last_h = window->h;
    wd->fullscreen_requested = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
    wd->window = SDLOP_X11_XCreateSimpleWindow(x11.display, x11.root,
                                                window->x, window->y,
                                                (unsigned int)window->w,
                                                (unsigned int)window->h,
                                                0, 0, 0);
    if (wd->window == None) {
        free(wd);
        return SDL_SetError("XCreateSimpleWindow failed");
    }
    window->driverdata = wd;

    long event_mask = ExposureMask | StructureNotifyMask | FocusChangeMask |
                      EnterWindowMask | LeaveWindowMask | PointerMotionMask |
                      ButtonPressMask | ButtonReleaseMask | KeyPressMask |
                      KeyReleaseMask | PropertyChangeMask;
    SDLOP_X11_XSelectInput(x11.display, wd->window, event_mask);

    Atom protocols[1] = { x11.wm_delete_window };
    if (x11.wm_protocols && x11.wm_delete_window) {
        SDLOP_X11_XSetWMProtocols(x11.display, wd->window, protocols, 1);
    }
    XWMHints wm_hints;
    memset(&wm_hints, 0, sizeof(wm_hints));
    wm_hints.flags = InputHint;
    wm_hints.input = True;
    SDLOP_X11_XSetWMHints(x11.display, wd->window, &wm_hints);

    unsigned long pid = (unsigned long)getpid();
    if (x11.net_wm_pid) {
        SDLOP_X11_XChangeProperty(x11.display, wd->window, x11.net_wm_pid,
                                  XA_CARDINAL, 32, PropModeReplace,
                                  (const unsigned char *)&pid, 1);
    }
    x11_set_size_hints(window);
    x11_set_decorations(window);
    x11_set_window_title(window);

    if ((window->flags & SDL_WINDOW_FULLSCREEN) && !x11.window_manager_present) {
        wd->fullscreen_saved = true;
        wd->saved_x = wd->last_x;
        wd->saved_y = wd->last_y;
        wd->saved_w = wd->last_w;
        wd->saved_h = wd->last_h;
        x11_update_geometry(window, 0, 0, x11.width, x11.height, true, false);
    } else if ((window->flags & SDL_WINDOW_MAXIMIZED) && !x11.window_manager_present) {
        wd->maximized_saved = true;
        wd->max_saved_x = wd->last_x;
        wd->max_saved_y = wd->last_y;
        wd->max_saved_w = wd->last_w;
        wd->max_saved_h = wd->last_h;
        x11_update_geometry(window, 0, 0, x11.width, x11.height, true, false);
    }
    SDLOP_X11_XFlush(x11.display);
    return true;
}

static void x11_DestroyWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (!wd) {
        return;
    }
    if (x11.relative_window == window) {
        x11_release_pointer();
    }
    SDLOP_X11_GL_WindowDestroyed(window);
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
    if (wd->image) {
        SDLOP_X11_XDestroyImage(wd->image);
    }
    if (wd->gc) {
        SDLOP_X11_XFreeGC(x11.display, wd->gc);
    }
    if (wd->window != None) {
        SDLOP_X11_XDestroyWindow(x11.display, wd->window);
    }
    window->driverdata = NULL;
    free(wd);
    SDLOP_X11_XFlush(x11.display);
}

static void x11_ShowWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (wd) {
        SDLOP_X11_XMapWindow(x11.display, wd->window);
        if (x11.window_manager_present) {
            if (window->flags & SDL_WINDOW_FULLSCREEN) {
                x11_send_wm_state(window, 1, x11.net_wm_state_fullscreen, None);
            } else if (window->flags & SDL_WINDOW_MAXIMIZED) {
                x11_send_wm_state(window, 1, x11.net_wm_state_max_horz,
                                  x11.net_wm_state_max_vert);
            }
        } else if (!(window->flags & SDL_WINDOW_NOT_FOCUSABLE)) {
            SDLOP_X11_XSetInputFocus(x11.display, wd->window,
                                     RevertToParent, CurrentTime);
        }
        SDLOP_X11_XFlush(x11.display);
    }
}

static void x11_HideWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (wd) {
        if (x11.relative_window == window) {
            x11_release_pointer();
        }
        SDLOP_X11_XUnmapWindow(x11.display, wd->window);
        SDLOP_X11_XFlush(x11.display);
    }
}

static bool x11_SetWindowTitle(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (!x11_window_data(window)) {
        return SDL_SetError("Invalid X11 window");
    }
    x11_set_window_title(window);
    return true;
}

static bool x11_SetWindowSize(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (!wd || window->w <= 0 || window->h <= 0 ||
        window->w > SDLOP_X11_MAX_WINDOW_DIMENSION ||
        window->h > SDLOP_X11_MAX_WINDOW_DIMENSION) {
        return SDL_SetError("Unsupported X11 window size");
    }
    x11_set_size_hints(window);
    x11_update_geometry(window, window->x, window->y, window->w, window->h,
                        true, true);
    return true;
}

static bool x11_SetWindowPosition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (!wd) {
        return SDL_SetError("Invalid X11 window");
    }
    x11_update_geometry(window, window->x, window->y, window->w, window->h,
                        true, true);
    return true;
}

static void x11_MinimizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (!wd) {
        return;
    }
    if (window->flags & SDL_WINDOW_MINIMIZED) {
        return;
    }
    if (x11.window_manager_present) {
        SDLOP_X11_XIconifyWindow(x11.display, wd->window, x11.screen);
    } else {
        SDLOP_X11_XUnmapWindow(x11.display, wd->window);
    }
    window->flags |= SDL_WINDOW_MINIMIZED;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MINIMIZED, 0, 0);
    SDLOP_X11_XFlush(x11.display);
}

static void x11_MaximizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (!wd || (window->flags & SDL_WINDOW_MAXIMIZED)) {
        return;
    }
    if (x11.window_manager_present) {
        x11_send_wm_state(window, 1, x11.net_wm_state_max_horz,
                          x11.net_wm_state_max_vert);
        return;
    }

    wd->maximized_saved = true;
    wd->max_saved_x = wd->last_x;
    wd->max_saved_y = wd->last_y;
    wd->max_saved_w = wd->last_w;
    wd->max_saved_h = wd->last_h;
    x11_update_geometry(window, 0, 0, x11.width, x11.height, true, true);
    window->flags |= SDL_WINDOW_MAXIMIZED;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MAXIMIZED, 0, 0);
}

static void x11_RestoreWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (!wd) {
        return;
    }
    bool was_minimized = (window->flags & SDL_WINDOW_MINIMIZED) != 0;
    bool was_maximized = (window->flags & SDL_WINDOW_MAXIMIZED) != 0;
    bool was_fullscreen = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;

    x11_send_wm_state(window, 0, x11.net_wm_state_fullscreen, None);
    x11_send_wm_state(window, 0, x11.net_wm_state_max_horz,
                      x11.net_wm_state_max_vert);

    if (!x11.window_manager_present) {
        if (wd->maximized_saved) {
            wd->maximized_saved = false;
            wd->fullscreen_saved = false;
            x11_update_geometry(window, wd->max_saved_x, wd->max_saved_y,
                                wd->max_saved_w, wd->max_saved_h, true, true);
        } else if (wd->fullscreen_saved) {
            wd->fullscreen_saved = false;
            x11_update_geometry(window, wd->saved_x, wd->saved_y,
                                wd->saved_w, wd->saved_h, true, true);
        }
    }
    window->flags &= ~(SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED |
                       SDL_WINDOW_FULLSCREEN);
    wd->fullscreen_requested = false;
    if (was_fullscreen) {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 0, 0);
    }
    if (was_minimized || was_maximized) {
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESTORED, 0, 0);
    }
    SDLOP_X11_XMapWindow(x11.display, wd->window);
    SDLOP_X11_XFlush(x11.display);
}

static bool x11_SetWindowFullscreen(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (!wd) {
        return SDL_SetError("Invalid X11 window");
    }
    bool fullscreen = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
    bool changed = fullscreen != wd->fullscreen_requested;
    wd->fullscreen_requested = fullscreen;
    x11_send_wm_state(window, fullscreen ? 1 : 0,
                      x11.net_wm_state_fullscreen, None);

    if (!x11.window_manager_present) {
        if (fullscreen) {
            if (!wd->fullscreen_saved) {
                wd->fullscreen_saved = true;
                wd->saved_x = wd->last_x;
                wd->saved_y = wd->last_y;
                wd->saved_w = wd->last_w;
                wd->saved_h = wd->last_h;
            }
            x11_update_geometry(window, 0, 0, x11.width, x11.height,
                                true, true);
        } else if (wd->fullscreen_saved) {
            wd->fullscreen_saved = false;
            x11_update_geometry(window, wd->saved_x, wd->saved_y,
                                wd->saved_w, wd->saved_h, true, true);
        }
    }
    if (changed) {
        SDLOP_SendWindowEvent(window,
                              fullscreen ? SDL_EVENT_WINDOW_ENTER_FULLSCREEN
                                         : SDL_EVENT_WINDOW_LEAVE_FULLSCREEN,
                              0, 0);
    }
    SDLOP_X11_XFlush(x11.display);
    return true;
}

static void x11_RaiseWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (wd) {
        SDLOP_X11_XRaiseWindow(x11.display, wd->window);
        if (!x11.window_manager_present && !(window->flags & SDL_WINDOW_NOT_FOCUSABLE)) {
            SDLOP_X11_XSetInputFocus(x11.display, wd->window,
                                     RevertToParent, CurrentTime);
        }
        SDLOP_X11_XFlush(x11.display);
    }
}

static void x11_SetWindowClearColor(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (window && window->surface) {
        x11_fill_surface_with_clear_color(window);
        (void)x11_update_window_framebuffer(device, window, NULL, 0);
    }
}

static bool x11_SetWindowRelativeMouseMode(SDLop_VideoDevice *device,
                                            SDL_Window *window, bool enabled)
{
    (void)device;
    X11WindowData *wd = x11_window_data(window);
    if (!wd) {
        return SDL_SetError("Invalid X11 window");
    }
    if (!enabled) {
        if (x11.relative_window == window) {
            x11_release_pointer();
        }
        return true;
    }

    if (x11.relative_window && x11.relative_window != window) {
        x11_release_pointer();
    }
    int status = SDLOP_X11_XGrabPointer(x11.display, wd->window, True,
                                        PointerMotionMask | ButtonPressMask |
                                        ButtonReleaseMask,
                                        GrabModeAsync, GrabModeAsync,
                                        wd->window, x11.invisible_cursor,
                                        CurrentTime);
    if (status != GrabSuccess) {
        return SDL_SetError("XGrabPointer failed (status %d)", status);
    }
    x11.relative_window = window;
    if (x11.invisible_cursor) {
        SDLOP_X11_XDefineCursor(x11.display, wd->window, x11.invisible_cursor);
    }
    wd->warp_x = window->w / 2;
    wd->warp_y = window->h / 2;
    wd->warp_pending = true;
    SDLOP_X11_XWarpPointer(x11.display, None, wd->window, 0, 0, 0, 0,
                           wd->warp_x, wd->warp_y);
    SDLOP_X11_XFlush(x11.display);
    return true;
}

static Uint8 x11_button_to_sdl(unsigned int button)
{
    switch (button) {
    case Button1: return SDL_BUTTON_LEFT;
    case Button2: return SDL_BUTTON_MIDDLE;
    case Button3: return SDL_BUTTON_RIGHT;
    case 8: return SDL_BUTTON_X1;
    case 9: return SDL_BUTTON_X2;
    default: return 0;
    }
}

static bool x11_is_repeat_release(const XKeyEvent *release)
{
    if (x11.detectable_auto_repeat || SDLOP_X11_XPending(x11.display) <= 0) {
        return false;
    }
    XEvent next;
    SDLOP_X11_XPeekEvent(x11.display, &next);
    return next.type == KeyPress &&
           next.xkey.keycode == release->keycode &&
           next.xkey.time == release->time;
}

static void x11_handle_key_event(SDL_Window *window, XEvent *event)
{
    X11WindowData *wd = x11_window_data(window);
    if (!wd || SDLOP_RawInputKeyboardActive()) {
        return;
    }
    unsigned int keycode = event->xkey.keycode;
    if (keycode >= SDL_arraysize(wd->key_down) || keycode < 8) {
        return;
    }
    Uint16 rawcode = (Uint16)(keycode - 8);
    if (rawcode > SDLOP_EVDEV_KEY_MAX) {
        return;
    }
    SDL_Scancode scancode = (SDL_Scancode)sdlop_evdev_to_scancode[rawcode];
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        return;
    }

    if (event->type == KeyPress) {
        /* SDLop's shared repeat timer owns key repeat. Ignore the repeated
         * KeyPress events generated by X11 so the server and timer cannot
         * deliver duplicate repeat events. */
        if (wd->key_down[keycode]) {
            return;
        }
        wd->key_down[keycode] = 1;
        SDLOP_SendKeyboardKey(true, false, scancode, rawcode, 0);
    } else {
        if (x11_is_repeat_release(&event->xkey)) {
            return;
        }
        if (!wd->key_down[keycode]) {
            return;
        }
        wd->key_down[keycode] = 0;
        SDLOP_SendKeyboardKey(false, false, scancode, rawcode, 0);
    }
}

static void x11_handle_focus(SDL_Window *window, bool focused)
{
    if (focused) {
        if (sdlop.keyboard_focus == window) {
            return;
        }
        SDL_Window *old = sdlop.keyboard_focus;
        if (old && old != window) {
            old->flags &= ~SDL_WINDOW_INPUT_FOCUS;
            SDLOP_SendWindowEvent(old, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
        }
        sdlop.keyboard_focus = window;
        window->flags |= SDL_WINDOW_INPUT_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_FOCUS_GAINED, 0, 0);
    } else if (sdlop.keyboard_focus == window) {
        sdlop.keyboard_focus = NULL;
        window->flags &= ~SDL_WINDOW_INPUT_FOCUS;
        if (!SDLOP_RawInputKeyboardActive()) {
            SDL_ResetKeyboard();
            memset(x11_window_data(window)->key_down, 0,
                   sizeof(x11_window_data(window)->key_down));
        }
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
    }
}

static void x11_handle_mouse_enter(SDL_Window *window, XCrossingEvent *event)
{
    if (event->mode == NotifyGrab || event->mode == NotifyUngrab) {
        return;
    }
    SDL_Window *old = sdlop.mouse_focus;
    if (old && old != window) {
        old->flags &= ~SDL_WINDOW_MOUSE_FOCUS;
        SDLOP_SendWindowEvent(old, SDL_EVENT_WINDOW_MOUSE_LEAVE, 0, 0);
    }
    sdlop.mouse_focus = window;
    window->flags |= SDL_WINDOW_MOUSE_FOCUS;
    sdlop.mouse_has_position = false;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_ENTER, 0, 0);
    if (SDLOP_RawInputMouseActive()) {
        sdlop.mouse_x = (float)event->x;
        sdlop.mouse_y = (float)event->y;
        sdlop.mouse_last_x = sdlop.mouse_x;
        sdlop.mouse_last_y = sdlop.mouse_y;
        sdlop.mouse_has_position = true;
    } else {
        SDLOP_SendMouseMotion((float)event->x, (float)event->y,
                              0.0f, 0.0f, 0);
    }
}

static void x11_handle_mouse_leave(SDL_Window *window, XCrossingEvent *event)
{
    if (event->mode == NotifyGrab || event->mode == NotifyUngrab) {
        return;
    }
    if (sdlop.mouse_focus == window) {
        sdlop.mouse_focus = NULL;
        window->flags &= ~SDL_WINDOW_MOUSE_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_LEAVE, 0, 0);
    }
}

static void x11_handle_motion(SDL_Window *window, XMotionEvent *event)
{
    X11WindowData *wd = x11_window_data(window);
    if (!wd) {
        return;
    }
    if (SDLOP_RawInputMouseActive()) {
        if (x11.relative_window == window) {
            return;
        }
        /* The raw evdev ring owns motion delivery; retain X11's absolute
         * pointer position as an anchor for the next enter/focus transition. */
        sdlop.mouse_x = (float)event->x;
        sdlop.mouse_y = (float)event->y;
        sdlop.mouse_last_x = sdlop.mouse_x;
        sdlop.mouse_last_y = sdlop.mouse_y;
        sdlop.mouse_has_position = true;
        return;
    }

    if (x11.relative_window == window) {
        if (wd->warp_pending && event->x == wd->warp_x && event->y == wd->warp_y) {
            wd->warp_pending = false;
            return;
        }
        wd->warp_pending = false;
        int dx = event->x - wd->warp_x;
        int dy = event->y - wd->warp_y;
        if (dx || dy) {
            SDLOP_SendMouseMotion(SDLOP_NO_POS, SDLOP_NO_POS,
                                  (float)dx, (float)dy, 0);
            wd->warp_pending = true;
            SDLOP_X11_XWarpPointer(x11.display, None, wd->window, 0, 0, 0, 0,
                                   wd->warp_x, wd->warp_y);
            SDLOP_X11_XFlush(x11.display);
        }
        return;
    }
    SDLOP_SendMouseMotion((float)event->x, (float)event->y, 0.0f, 0.0f, 0);
}

static void x11_handle_button(SDL_Window *window, XButtonEvent *event, bool down)
{
    if (SDLOP_RawInputMouseActive()) {
        return;
    }
    if (event->button >= Button4 && event->button <= 7) {
        if (down) {
            float sx = 0.0f, sy = 0.0f;
            switch (event->button) {
            case Button4: sy = 1.0f; break;
            case Button5: sy = -1.0f; break;
            case 6: sx = 1.0f; break;
            case 7: sx = -1.0f; break;
            }
            SDLOP_SendMouseWheel(sx, sy, 0);
        }
        return;
    }

    Uint8 button = x11_button_to_sdl(event->button);
    if (!button) {
        return;
    }
    if (x11.relative_window == window) {
        SDLOP_SendMouseButton(down, button, SDLOP_NO_POS, SDLOP_NO_POS, 0);
    } else {
        SDLOP_SendMouseButton(down, button, (float)event->x,
                              (float)event->y, 0);
    }
}

static void x11_handle_event(XEvent *event)
{
    SDL_Window *window = NULL;
    switch (event->type) {
    case Expose:
        if (event->xexpose.count == 0 &&
            (window = x11_find_window(event->xexpose.window)) != NULL) {
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_EXPOSED, 0, 0);
        }
        break;
    case ConfigureNotify:
        if ((window = x11_find_window(event->xconfigure.window)) != NULL) {
            int x = event->xconfigure.x;
            int y = event->xconfigure.y;
            int translated_x = x, translated_y = y;
            Window child = None;
            if (SDLOP_X11_XTranslateCoordinates(x11.display,
                                                event->xconfigure.window,
                                                x11.root, 0, 0,
                                                &translated_x, &translated_y,
                                                &child)) {
                x = translated_x;
                y = translated_y;
            }
            x11_update_geometry(window, x, y,
                                event->xconfigure.width,
                                event->xconfigure.height, false, true);
        }
        break;
    case ClientMessage:
        if (event->xclient.message_type == x11.wm_protocols &&
            (Atom)event->xclient.data.l[0] == x11.wm_delete_window &&
            (window = x11_find_window(event->xclient.window)) != NULL) {
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
        }
        break;
    case FocusIn:
        if (event->xfocus.mode != NotifyGrab && event->xfocus.mode != NotifyUngrab &&
            (window = x11_find_window(event->xfocus.window)) != NULL) {
            x11_handle_focus(window, true);
        }
        break;
    case FocusOut:
        if (event->xfocus.mode != NotifyGrab && event->xfocus.mode != NotifyUngrab &&
            (window = x11_find_window(event->xfocus.window)) != NULL) {
            x11_handle_focus(window, false);
        }
        break;
    case EnterNotify:
        if ((window = x11_find_window(event->xcrossing.window)) != NULL) {
            x11_handle_mouse_enter(window, &event->xcrossing);
        }
        break;
    case LeaveNotify:
        if ((window = x11_find_window(event->xcrossing.window)) != NULL) {
            x11_handle_mouse_leave(window, &event->xcrossing);
        }
        break;
    case MotionNotify:
        if ((window = x11_find_window(event->xmotion.window)) != NULL) {
            x11_handle_motion(window, &event->xmotion);
        }
        break;
    case ButtonPress:
    case ButtonRelease:
        if ((window = x11_find_window(event->xbutton.window)) != NULL) {
            x11_handle_button(window, &event->xbutton,
                              event->type == ButtonPress);
        }
        break;
    case KeyPress:
    case KeyRelease:
        if ((window = x11_find_window(event->xkey.window)) != NULL) {
            x11_handle_key_event(window, event);
        }
        break;
    case PropertyNotify:
        if (event->xproperty.atom == x11.net_wm_state &&
            (window = x11_find_window(event->xproperty.window)) != NULL) {
            x11_update_ewmh_state(window);
        }
        break;
    case MapNotify:
        if ((window = x11_find_window(event->xmap.window)) != NULL) {
            X11WindowData *wd = x11_window_data(window);
            wd->mapped = true;
        }
        break;
    case UnmapNotify:
        if ((window = x11_find_window(event->xunmap.window)) != NULL) {
            X11WindowData *wd = x11_window_data(window);
            wd->mapped = false;
        }
        break;
    default:
        break;
    }
}

static void x11_PumpEvents(SDLop_VideoDevice *device, int timeout_ms)
{
    (void)device;
    if (!x11.display) {
        return;
    }

    int pending = SDLOP_X11_XPending(x11.display);
    if (pending == 0 && timeout_ms >= 0 && timeout_ms > 0) {
        struct pollfd pfd = { SDLOP_X11_XConnectionNumber(x11.display), POLLIN, 0 };
        (void)poll(&pfd, 1, timeout_ms);
        pending = SDLOP_X11_XPending(x11.display);
    }
    while (pending > 0) {
        XEvent event;
        SDLOP_X11_XNextEvent(x11.display, &event);
        x11_handle_event(&event);
        /* XPending reports the queue's total size; do not add it to the
         * previous count or the loop will overrun the queue and block. */
        pending = SDLOP_X11_XPending(x11.display);
    }
    SDLOP_KeyboardProcessRepeats();
}

static int x11_GetEventFD(SDLop_VideoDevice *device)
{
    (void)device;
    return x11.display ? SDLOP_X11_XConnectionNumber(x11.display) : -1;
}

SDLop_VideoDevice SDLop_x11_device = {
    "x11",
    x11_Init,
    x11_Quit,
    x11_CreateWindow,
    x11_DestroyWindow,
    x11_ShowWindow,
    x11_HideWindow,
    x11_SetWindowTitle,
    x11_SetWindowSize,
    x11_SetWindowPosition,
    x11_MinimizeWindow,
    x11_MaximizeWindow,
    x11_RestoreWindow,
    x11_SetWindowFullscreen,
    x11_RaiseWindow,
    x11_SetWindowClearColor,
    x11_SetWindowRelativeMouseMode,
    x11_PumpEvents,
    x11_GetEventFD,

    x11_create_window_framebuffer,
    x11_update_window_framebuffer,
    x11_destroy_window_framebuffer,

    SDLOP_X11_GL_CreateContext,
    SDLOP_X11_GL_MakeCurrent,
    SDLOP_X11_GL_SwapBuffers,
    SDLOP_X11_GL_DeleteContext,
    SDLOP_X11_GL_GetProcAddress,
    SDLOP_X11_GL_SetSwapInterval,
    SDLOP_X11_GL_GetSwapInterval,

    SDLOP_X11_Vulkan_CreateSurface,
    SDLOP_X11_Vulkan_GetInstanceExtensions,
};
