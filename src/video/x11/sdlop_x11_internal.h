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
xcb_window_t SDLOP_X11_GetRoot(void);
/* dispatch events until pred(userdata) holds or timeout_ms passes */
void SDLOP_X11_PumpUntil(bool (*pred)(void *), void *userdata, int timeout_ms);
int SDLOP_X11_GetScreenNumber(void);
xcb_visualid_t SDLOP_X11_GetVisual(void);
xcb_window_t SDLOP_X11_GetWindowXID(SDL_Window *window);
/* X visual of the window: SDLOP_X11_GetVisual() unless it is an OpenGL window
 * created with the visual of its GLX framebuffer config */
xcb_visualid_t SDLOP_X11_GetWindowVisual(SDL_Window *window);
/* whether windows can use this visual in place of the root visual: TrueColor
 * at the root depth (the window surface and its blits assume that depth) */
bool SDLOP_X11_IsCompatibleVisual(xcb_visualid_t visual);
/* visual an SDL_WINDOW_OPENGL window should be created with for the current
 * SDL_GL_* attributes, as stock SDL3's X11_GL_GetVisual; 0 = default visual */
xcb_visualid_t SDLOP_X11_GL_ChooseWindowVisual(void);
void SDLOP_X11_SetGLActive(SDL_Window *window, bool active);
void **SDLOP_X11_GetGLSurfaceSlot(SDL_Window *window);

/* sdlop_x11_gl.c */
void *SDLOP_X11_EGL_GetDisplay(SDLop_VideoDevice *device);
void *SDLOP_X11_EGL_GetConfig(SDLop_VideoDevice *device);
void *SDLOP_X11_EGL_GetWindowSurface(SDLop_VideoDevice *device, SDL_Window *window);
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
bool SDLOP_X11_Vulkan_GetPresentationSupport(SDLop_VideoDevice *device, void *instance,
                                             void *physical_device, Uint32 queue_family);

/* sdlop_x11_clipboard.c */
void SDLOP_X11_Clipboard_Init(void);
void SDLOP_X11_Clipboard_Quit(void);
bool SDLOP_X11_Clipboard_HandleEvent(xcb_generic_event_t *ev); /* true = consumed */
bool SDLOP_X11_SetClipboardData(SDLop_VideoDevice *device);
void *SDLOP_X11_GetClipboardData(SDLop_VideoDevice *device, const char *mime_type, size_t *size);
bool SDLOP_X11_HasClipboardData(SDLop_VideoDevice *device, const char *mime_type);
const char *const *SDLOP_X11_GetTextMimeTypes(SDLop_VideoDevice *device, size_t *num_mime_types);
bool SDLOP_X11_SetPrimarySelectionText(SDLop_VideoDevice *device, const char *text);
char *SDLOP_X11_GetPrimarySelectionText(SDLop_VideoDevice *device);
bool SDLOP_X11_HasPrimarySelectionText(SDLop_VideoDevice *device);

#endif /* sdlop_x11_internal_h_ */
