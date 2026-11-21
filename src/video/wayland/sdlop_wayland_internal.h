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
uint32_t SDLOP_Wayland_GetSelectionSerial(void);
struct wl_surface *SDLOP_Wayland_GetWindowSurfaceHandle(SDL_Window *window);

/* per-window presentation state (WaylandWindowData fields, exposed so the
 * GL module can take over buffer management without EGL types here) */
void SDLOP_Wayland_SetGLActive(SDL_Window *window, bool active);
bool SDLOP_Wayland_IsGLActive(SDL_Window *window);
void SDLOP_Wayland_SetGLOffloadActive(SDL_Window *window, bool active);
bool SDLOP_Wayland_IsGLOffloadActive(SDL_Window *window);
void *SDLOP_Wayland_GetGLSurfaceSlot(SDL_Window *window); /* -> EGLSurface or SDLOP_GLOffloadSurface* storage */
void *SDLOP_Wayland_GetGLEGLWindowSlot(SDL_Window *window); /* -> wl_egl_window storage */
void *SDLOP_Wayland_GetGLOffloadContextSlot(SDL_Window *window); /* -> SDLOP_GLOffloadContext* storage */
bool SDLOP_Wayland_CompositorHasHardwareBuffers(void);
bool SDLOP_Wayland_AcquireOffloadShm(SDL_Window *window, void **pixels_out, int *pitch_out, int *w_out, int *h_out);
bool SDLOP_Wayland_PresentOffloadShm(SDL_Window *window);

/* GL module (sdlop_wayland_gl.c) - vtable entry points */
void *SDLOP_Wayland_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window);
bool SDLOP_Wayland_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context);
bool SDLOP_Wayland_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window);
void SDLOP_Wayland_GL_DeleteContext(SDLop_VideoDevice *device, void *context);
SDL_FunctionPointer SDLOP_Wayland_GL_GetProcAddressThunk(SDLop_VideoDevice *device, const char *proc);
bool SDLOP_Wayland_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval);
bool SDLOP_Wayland_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval);
void SDLOP_Wayland_GL_WindowDestroyed(SDL_Window *window);
void *SDLOP_Wayland_EGL_GetDisplay(SDLop_VideoDevice *device);
void *SDLOP_Wayland_EGL_GetConfig(SDLop_VideoDevice *device);
void *SDLOP_Wayland_EGL_GetWindowSurface(SDLop_VideoDevice *device, SDL_Window *window);
void SDLOP_Wayland_GL_WindowResized(SDL_Window *window);
void SDLOP_Wayland_GL_Deinit(void);

/* Vulkan module (sdlop_wayland_vulkan.c) - vtable entry point */
bool SDLOP_Wayland_Vulkan_CreateSurface(SDLop_VideoDevice *device, SDL_Window *window, void *instance, const void *allocator, Uint64 *surface);
bool SDLOP_Wayland_Vulkan_GetPresentationSupport(SDLop_VideoDevice *device, void *instance, void *physical_device, Uint32 queue_family);

/* clipboard module (sdlop_wayland_clipboard.c) */
struct wl_registry;
struct wl_seat;
bool SDLOP_Wayland_Clipboard_Global(struct wl_registry *registry, uint32_t name, const char *interface,
                                    uint32_t version); /* true = bound */
void SDLOP_Wayland_Clipboard_SeatAdded(struct wl_seat *seat);
void SDLOP_Wayland_Clipboard_Quit(void);
bool SDLOP_Wayland_Clipboard_Available(void);
bool SDLOP_Wayland_PrimarySelection_Available(void);
bool SDLOP_Wayland_SetClipboardData(SDLop_VideoDevice *device);
void *SDLOP_Wayland_GetClipboardData(SDLop_VideoDevice *device, const char *mime_type, size_t *size);
bool SDLOP_Wayland_HasClipboardData(SDLop_VideoDevice *device, const char *mime_type);
const char *const *SDLOP_Wayland_GetTextMimeTypes(SDLop_VideoDevice *device, size_t *num_mime_types);
bool SDLOP_Wayland_SetPrimarySelectionText(SDLop_VideoDevice *device, const char *text);
char *SDLOP_Wayland_GetPrimarySelectionText(SDLop_VideoDevice *device);
bool SDLOP_Wayland_HasPrimarySelectionText(SDLop_VideoDevice *device);

#endif /* sdlop_wayland_internal_h_ */
