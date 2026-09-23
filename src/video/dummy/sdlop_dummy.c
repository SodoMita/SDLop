/*
  SDLop - dummy (offscreen) video driver.
  Used for tests, CI, and SDL_VIDEODRIVER=dummy.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

static bool dummy_Init(SDLop_VideoDevice *device)
{
    (void)device;
    return true;
}

static void dummy_Quit(SDLop_VideoDevice *device)
{
    (void)device;
}

static bool dummy_CreateWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    return true;
}

static void dummy_DestroyWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
}

static void dummy_ShowWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    sdlop.keyboard_focus = window;
    sdlop.mouse_focus = window;
    window->flags |= SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
}

static void dummy_HideWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    window->flags &= ~(SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
    if (sdlop.keyboard_focus == window) {
        sdlop.keyboard_focus = NULL;
    }
    if (sdlop.mouse_focus == window) {
        sdlop.mouse_focus = NULL;
    }
}

static bool dummy_SetWindowTitle(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    return true;
}

static bool dummy_SetWindowSize(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    /* keep an existing software surface in sync (SDL3 semantics: the
     * surface survives resizes) */
    if (window->surface) {
        void *pixels = realloc(window->surface->pixels, (size_t)window->w * 4u * (size_t)window->h);
        if (!pixels) {
            return SDL_OutOfMemory();
        }
        window->surface->pixels = pixels;
        window->surface->w = window->w;
        window->surface->h = window->h;
        window->surface->pitch = window->w * 4;
    }
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, window->w, window->h);
    return true;
}

static bool dummy_SetWindowPosition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOVED, window->x, window->y);
    return true;
}

static void dummy_MinimizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    window->flags |= SDL_WINDOW_MINIMIZED;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MINIMIZED, 0, 0);
}

static void dummy_MaximizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    window->flags |= SDL_WINDOW_MAXIMIZED;
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MAXIMIZED, 0, 0);
}

static void dummy_RestoreWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    window->flags &= ~(SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESTORED, 0, 0);
}

static bool dummy_SetWindowFullscreen(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        window->flags &= ~SDL_WINDOW_FULLSCREEN;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 0, 0);
    } else {
        window->flags |= SDL_WINDOW_FULLSCREEN;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_ENTER_FULLSCREEN, 0, 0);
    }
    return true;
}

static void dummy_RaiseWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    dummy_ShowWindow(device, window);
}

static void dummy_SetWindowClearColor(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
}

static bool dummy_SetWindowRelativeMouseMode(SDLop_VideoDevice *device, SDL_Window *window, bool enabled)
{
    (void)device;
    (void)window;
    (void)enabled;
    return true;
}

static void dummy_PumpEvents(SDLop_VideoDevice *device, int timeout_ms)
{
    (void)device;
    SDLOP_KeyboardProcessRepeats();
    if (timeout_ms > 0) {
        /* nothing to wait on in the dummy driver; sleep in small steps so
         * SDL_WaitEventTimeout stays responsive to injected events */
        int waited = 0;
        while (waited < timeout_ms) {
            SDL_Delay(1);
            waited++;
        }
    }
}

static int dummy_GetEventFD(SDLop_VideoDevice *device)
{
    (void)device;
    return -1;
}

/* software surface: plain RAM framebuffer */

static bool dummy_CreateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface **surface)
{
    (void)device;
    SDL_Surface *s = SDL_CreateSurface(window->w, window->h, SDL_PIXELFORMAT_XRGB8888);
    if (!s) {
        return false;
    }
    *surface = s;
    return true;
}

static bool dummy_UpdateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    (void)device;
    (void)window;
    (void)rects;
    (void)numrects;
    return true; /* nowhere to present */
}

static void dummy_DestroyWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (window->surface) {
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
}

static bool dummy_NotSupportedGL(void)
{
    return SDL_SetError("The dummy video driver does not support OpenGL/Vulkan");
}

static void *dummy_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    dummy_NotSupportedGL();
    return NULL;
}

static bool dummy_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context)
{
    (void)device;
    (void)window;
    (void)context;
    return dummy_NotSupportedGL();
}

static bool dummy_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    return dummy_NotSupportedGL();
}

static void dummy_GL_DeleteContext(SDLop_VideoDevice *device, void *context)
{
    (void)device;
    (void)context;
}

static SDL_FunctionPointer dummy_GL_GetProcAddress(SDLop_VideoDevice *device, const char *proc)
{
    (void)device;
    (void)proc;
    return NULL;
}

static bool dummy_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval)
{
    (void)device;
    (void)interval;
    return true;
}

static bool dummy_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval)
{
    (void)device;
    *interval = 0;
    return true;
}

static bool dummy_Vulkan_CreateSurface(SDLop_VideoDevice *device, SDL_Window *window, void *instance, const void *allocator, Uint64 *surface)
{
    (void)device;
    (void)window;
    (void)instance;
    (void)allocator;
    (void)surface;
    return dummy_NotSupportedGL();
}

SDLop_VideoDevice SDLop_dummy_device = {
    "dummy",
    dummy_Init,
    dummy_Quit,
    dummy_CreateWindow,
    dummy_DestroyWindow,
    dummy_ShowWindow,
    dummy_HideWindow,
    dummy_SetWindowTitle,
    dummy_SetWindowSize,
    dummy_SetWindowPosition,
    dummy_MinimizeWindow,
    dummy_MaximizeWindow,
    dummy_RestoreWindow,
    dummy_SetWindowFullscreen,
    dummy_RaiseWindow,
    dummy_SetWindowClearColor,
    dummy_SetWindowRelativeMouseMode,
    dummy_PumpEvents,
    dummy_GetEventFD,
    dummy_CreateWindowFramebuffer,
    dummy_UpdateWindowFramebuffer,
    dummy_DestroyWindowFramebuffer,
    dummy_GL_CreateContext,
    dummy_GL_MakeCurrent,
    dummy_GL_SwapBuffers,
    dummy_GL_DeleteContext,
    dummy_GL_GetProcAddress,
    dummy_GL_SetSwapInterval,
    dummy_GL_GetSwapInterval,
    dummy_Vulkan_CreateSurface,
};
