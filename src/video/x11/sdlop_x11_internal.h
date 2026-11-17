/*
  SDLop - X11 driver internals shared by the window, GL and Vulkan modules.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_x11_internal_h_
#define sdlop_x11_internal_h_

#include "internal/sdlop_internal.h"
#include "sdlop_x11_dyn.h"

/* accessors implemented in sdlop_x11.c */
xcb_connection_t *SDLOP_X11_GetConnection(void);
int SDLOP_X11_GetScreenNumber(void);
xcb_visualid_t SDLOP_X11_GetVisual(void);
xcb_window_t SDLOP_X11_GetWindowXID(SDL_Window *window);
void SDLOP_X11_SetGLActive(SDL_Window *window, bool active);
void **SDLOP_X11_GetGLSurfaceSlot(SDL_Window *window);

/* sdlop_x11_gl.c */
void *SDLOP_X11_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window);
bool SDLOP_X11_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context);
bool SDLOP_X11_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window);
void SDLOP_X11_GL_DeleteContext(SDLop_VideoDevice *device, void *context);
SDL_FunctionPointer SDLOP_X11_GL_GetProcAddress(SDLop_VideoDevice *device, const char *proc);
bool SDLOP_X11_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval);
bool SDLOP_X11_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval);
void SDLOP_X11_GL_WindowDestroyed(SDL_Window *window);
void SDLOP_X11_GL_Quit(void);

/* sdlop_x11_vulkan.c */
bool SDLOP_X11_Vulkan_CreateSurface(SDLop_VideoDevice *device, SDL_Window *window, void *instance,
                                    const void *allocator, Uint64 *surface);
const char *const *SDLOP_X11_Vulkan_GetInstanceExtensions(SDLop_VideoDevice *device, Uint32 *count);

#endif /* sdlop_x11_internal_h_ */
