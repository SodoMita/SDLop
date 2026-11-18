/*
  SDLop - X11 driver internals shared between sdlop_x11.c and the
  GLX/Vulkan modules.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_x11_internal_h_
#define sdlop_x11_internal_h_

#include "internal/sdlop_internal.h"
#include "sdlop_x11_dyn.h"

typedef struct X11WindowData
{
    SDLOP_Window xwindow;
    SDLOP_GC gc;                   /* for XPutImage (software surface) */
    SDLOP_XImage *ximage;          /* wraps the SDL surface pixel data */
    void *fb_pixels;               /* owned framebuffer storage */
    int fb_w, fb_h;                /* current framebuffer size */
    bool gl_active;                /* GLX owns presentation */
    bool expecting_unmap;          /* HideWindow-initiated UnmapNotify */
    bool mapped;
    bool reparented;               /* a WM reparented us into a frame */
    /* SDL_SetWindowSize stores w/h synchronously, so the ConfigureNotify
     * answering our own XResizeWindow would look like a no-op. Track the
     * pending request and emit RESIZED when the awaited size arrives. */
    bool size_request_pending;
    int pending_w, pending_h;
    /* GLX visual chosen at window creation (SDL_WINDOW_OPENGL) */
    bool glx_vi_valid;
    SDLOP_XVisualInfo glx_vi;
    SDLOP_Colormap colormap;       /* != None when created for glx_vi */
    /* relative mouse mode */
    bool relative_mode;
    int warp_center_x, warp_center_y; /* last warp target (motion to swallow) */
    bool warp_pending;
} X11WindowData;

/* shared between sdlop_x11.c and sdlop_x11_glx.c */
SDLOP_Display *SDLOP_X11_GetDisplay(SDLop_VideoDevice *device);
SDLOP_Window SDLOP_X11_GetWindowHandle(SDL_Window *window);
void SDLOP_X11_SetGLActive(SDL_Window *window, bool active);
bool SDLOP_X11_GL_ChooseVisual(SDL_Window *window, SDLOP_Visual **visual_out, int *depth_out, SDLOP_Colormap *colormap_out);
void SDLOP_X11_GL_CommitVisual(SDL_Window *window);
void SDLOP_X11_DispatchEvent(SDLOP_XEvent *event);

/* GLX (sdlop_x11_glx.c) */
void *SDLOP_X11_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window);
bool SDLOP_X11_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context);
bool SDLOP_X11_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window);
void SDLOP_X11_GL_DeleteContext(SDLop_VideoDevice *device, void *context);
SDL_FunctionPointer SDLOP_X11_GL_GetProcAddressThunk(SDLop_VideoDevice *device, const char *proc);
bool SDLOP_X11_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval);
bool SDLOP_X11_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval);
void SDLOP_X11_GL_WindowDestroyed(SDL_Window *window);
void SDLOP_X11_GL_Quit(SDLop_VideoDevice *device);

/* Vulkan WSI (sdlop_x11_vulkan.c) */
bool SDLOP_X11_Vulkan_CreateSurface(SDLop_VideoDevice *device, SDL_Window *window, void *instance, const void *allocator, Uint64 *surface);
const char *const *SDLOP_X11_Vulkan_GetInstanceExtensions(SDLop_VideoDevice *device, Uint32 *count);

#endif /* sdlop_x11_internal_h_ */
