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
SDL_Window *SDLOP_X11_WindowFromXID(xcb_window_t xid); /* NULL = not ours */
/* X visual of the window: SDLOP_X11_GetVisual() unless it is an OpenGL window
 * created with the visual of its GLX framebuffer config */
xcb_visualid_t SDLOP_X11_GetWindowVisual(SDL_Window *window);
/* whether windows can use this visual in place of the root visual: TrueColor
 * at the root depth (the window surface and its blits assume that depth) */
bool SDLOP_X11_IsCompatibleVisual(xcb_visualid_t visual);
/* a depth-32 TrueColor ARGB8888 visual (SDL_WINDOW_TRANSPARENT windows) */
bool SDLOP_X11_IsARGBVisual(xcb_visualid_t visual);
/* visual an SDL_WINDOW_OPENGL window should be created with for the current
 * SDL_GL_* attributes, as stock SDL3's X11_GL_GetVisual; 0 = default visual */
xcb_visualid_t SDLOP_X11_GL_ChooseWindowVisual(bool transparent);
void SDLOP_X11_SetGLActive(SDL_Window *window, bool active);
void **SDLOP_X11_GetGLSurfaceSlot(SDL_Window *window);

/* sdlop_x11_gl.c */
void *SDLOP_X11_EGL_GetDisplay(SDLop_VideoDevice *device);
void *SDLOP_X11_EGL_GetConfig(SDLop_VideoDevice *device);
void *SDLOP_X11_EGL_GetWindowSurface(SDLop_VideoDevice *device, SDL_Window *window);
void *SDLOP_X11_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window);
bool SDLOP_X11_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context);
bool SDLOP_X11_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window);
/* after a frame reached the window: answers a pending _NET_WM_SYNC_REQUEST */
void SDLOP_X11_HandlePresent(SDL_Window *window);
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

/* sdlop_x11_dnd.c */
/* sdlop_x11_xsettings.c */
void SDLOP_X11_XSettings_Init(void);
void SDLOP_X11_XSettings_Quit(void);
/* true = consumed; *changed is set when the settings changed */
bool SDLOP_X11_XSettings_HandleEvent(xcb_generic_event_t *ev, bool *changed);
bool SDLOP_X11_XSettings_GetInt(const char *name, int *value);
const char *SDLOP_X11_XSettings_GetString(const char *name);

void SDLOP_X11_DnD_Init(void);
void SDLOP_X11_DnD_Quit(void);
bool SDLOP_X11_DnD_HandleEvent(xcb_generic_event_t *ev); /* true = consumed */
void SDLOP_X11_DnD_WindowDestroyed(SDL_Window *window);
void SDLOP_X11_AcceptDragAndDrop(SDL_Window *window, bool accept);

/* sdlop_x11_pen.c - XInput 2 tablets */
void SDLOP_X11_Pen_Init(uint8_t xi_opcode); /* XI 2.2 present */
void SDLOP_X11_Pen_Quit(void);
void SDLOP_X11_Pen_WindowCreated(xcb_window_t xid);
bool SDLOP_X11_Pen_HandleEvent(const xcb_ge_generic_event_t *ge); /* true = consumed */
bool SDLOP_X11_Pen_IsPenDevice(uint16_t deviceid);
/* a core pointer event the master device made from pen input */
bool SDLOP_X11_Pen_FilterCore(xcb_timestamp_t time);

/* sdlop_x11_xim.c - input methods on a private Xlib connection */
bool SDLOP_X11_XIM_Start(SDL_Window *window);
bool SDLOP_X11_XIM_Stop(SDL_Window *window);
bool SDLOP_X11_XIM_UpdateArea(SDL_Window *window);
bool SDLOP_X11_XIM_ClearComposition(SDL_Window *window);
void SDLOP_X11_XIM_Update(void); /* keyboard focus / text input changed */
bool SDLOP_X11_XIM_FilterKey(const xcb_key_press_event_t *kev); /* true = taken by the IM */
void SDLOP_X11_XIM_Pump(void);
int SDLOP_X11_XIM_GetFD(void); /* -1 = none */
void SDLOP_X11_XIM_WindowDestroyed(SDL_Window *window);
void SDLOP_X11_XIM_Quit(void);
/* sdlop_x11.c: a key event the IM passed back (not filtered again) */
void SDLOP_X11_HandleKeyFromIM(const xcb_key_press_event_t *kev);

#endif /* sdlop_x11_internal_h_ */
