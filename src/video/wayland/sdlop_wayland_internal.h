/*
  SDLop - Wayland driver internals shared between sdlop_wayland.c,
  sdlop_wayland_gl.c and sdlop_wayland_vulkan.c.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_wayland_internal_h_
#define sdlop_wayland_internal_h_

#include "internal/sdlop_internal.h"
#include <stdbool.h>

struct wl_display;
struct wl_surface;

/* main wayland module (sdlop_wayland.c) */
struct wl_display *SDLOP_Wayland_GetDisplay(void);
struct wl_surface *SDLOP_Wayland_GetWindowSurfaceHandle(SDL_Window *window);

/* per-window presentation state (WaylandWindowData fields, exposed so the
 * GL module can take over buffer management without EGL types here) */
void SDLOP_Wayland_SetGLActive(SDL_Window *window, bool active);
bool SDLOP_Wayland_IsGLActive(SDL_Window *window);
void *SDLOP_Wayland_GetGLSurfaceSlot(SDL_Window *window); /* -> EGLSurface storage */
void *SDLOP_Wayland_GetGLEGLWindowSlot(SDL_Window *window); /* -> wl_egl_window storage */

/* GL module (sdlop_wayland_gl.c) - vtable entry points */
void *SDLOP_Wayland_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window);
bool SDLOP_Wayland_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context);
bool SDLOP_Wayland_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window);
void SDLOP_Wayland_GL_DeleteContext(SDLop_VideoDevice *device, void *context);
SDL_FunctionPointer SDLOP_Wayland_GL_GetProcAddressThunk(SDLop_VideoDevice *device, const char *proc);
bool SDLOP_Wayland_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval);
bool SDLOP_Wayland_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval);
void SDLOP_Wayland_GL_WindowDestroyed(SDL_Window *window);
void SDLOP_Wayland_GL_WindowResized(SDL_Window *window);

/* Vulkan module (sdlop_wayland_vulkan.c) - vtable entry point */
bool SDLOP_Wayland_Vulkan_CreateSurface(SDLop_VideoDevice *device, SDL_Window *window, void *instance, const void *allocator, Uint64 *surface);

#endif /* sdlop_wayland_internal_h_ */
