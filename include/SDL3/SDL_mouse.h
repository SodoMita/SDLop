/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  GENERATED FILE -- do not edit.  Produced by tools/sdlop.py from the upstream SDL3
  header SDL_mouse.h (3.2.10), which is zlib licensed, Copyright (C) 1997-2025 Sam Lantinga
  and SDL contributors.  Declarations are copied verbatim so that source and binary
  compatibility with SDL3 are exact; the items SDLop does not implement were
  removed (the full list is in tools/dropped.txt).

  In this header: Mouse state, relative mode, warping, cursors.
*/

#ifndef SDL_mouse_h_
#define SDL_mouse_h_

#include <SDL3/SDL_begin_code.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_surface.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef Uint32 SDL_MouseID;

typedef struct SDL_Cursor SDL_Cursor;

typedef enum SDL_SystemCursor
{
 SDL_SYSTEM_CURSOR_DEFAULT,
 SDL_SYSTEM_CURSOR_TEXT,
 SDL_SYSTEM_CURSOR_WAIT,
 SDL_SYSTEM_CURSOR_CROSSHAIR,
 SDL_SYSTEM_CURSOR_PROGRESS,
 SDL_SYSTEM_CURSOR_NWSE_RESIZE,
 SDL_SYSTEM_CURSOR_NESW_RESIZE,
 SDL_SYSTEM_CURSOR_EW_RESIZE,
 SDL_SYSTEM_CURSOR_NS_RESIZE,
 SDL_SYSTEM_CURSOR_MOVE,
 SDL_SYSTEM_CURSOR_NOT_ALLOWED,
 SDL_SYSTEM_CURSOR_POINTER,
 SDL_SYSTEM_CURSOR_NW_RESIZE,
 SDL_SYSTEM_CURSOR_N_RESIZE,
 SDL_SYSTEM_CURSOR_NE_RESIZE,
 SDL_SYSTEM_CURSOR_E_RESIZE,
 SDL_SYSTEM_CURSOR_SE_RESIZE,
 SDL_SYSTEM_CURSOR_S_RESIZE,
 SDL_SYSTEM_CURSOR_SW_RESIZE,
 SDL_SYSTEM_CURSOR_W_RESIZE,
 SDL_SYSTEM_CURSOR_COUNT
} SDL_SystemCursor;

typedef enum SDL_MouseWheelDirection
{
 SDL_MOUSEWHEEL_NORMAL,
 SDL_MOUSEWHEEL_FLIPPED
} SDL_MouseWheelDirection;

typedef Uint32 SDL_MouseButtonFlags;

#define SDL_BUTTON_LEFT 1

#define SDL_BUTTON_MIDDLE 2

#define SDL_BUTTON_RIGHT 3

#define SDL_BUTTON_X1 4

#define SDL_BUTTON_X2 5

#define SDL_BUTTON_MASK(X) (1u << ((X)-1))

#define SDL_BUTTON_LMASK SDL_BUTTON_MASK(SDL_BUTTON_LEFT)

#define SDL_BUTTON_MMASK SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)

#define SDL_BUTTON_RMASK SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)

#define SDL_BUTTON_X1MASK SDL_BUTTON_MASK(SDL_BUTTON_X1)

#define SDL_BUTTON_X2MASK SDL_BUTTON_MASK(SDL_BUTTON_X2)

extern SDL_DECLSPEC SDL_Window * SDLCALL SDL_GetMouseFocus(void);

extern SDL_DECLSPEC SDL_MouseButtonFlags SDLCALL SDL_GetMouseState(float *x, float *y);

extern SDL_DECLSPEC SDL_MouseButtonFlags SDLCALL SDL_GetGlobalMouseState(float *x, float *y);

extern SDL_DECLSPEC SDL_MouseButtonFlags SDLCALL SDL_GetRelativeMouseState(float *x, float *y);

extern SDL_DECLSPEC void SDLCALL SDL_WarpMouseInWindow(SDL_Window *window,
 float x, float y);

extern SDL_DECLSPEC bool SDLCALL SDL_WarpMouseGlobal(float x, float y);

extern SDL_DECLSPEC bool SDLCALL SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled);

extern SDL_DECLSPEC bool SDLCALL SDL_GetWindowRelativeMouseMode(SDL_Window *window);

extern SDL_DECLSPEC bool SDLCALL SDL_CaptureMouse(bool enabled);

extern SDL_DECLSPEC SDL_Cursor * SDLCALL SDL_CreateCursor(const Uint8 *data,
 const Uint8 *mask,
 int w, int h, int hot_x,
 int hot_y);

extern SDL_DECLSPEC SDL_Cursor * SDLCALL SDL_CreateColorCursor(SDL_Surface *surface,
 int hot_x,
 int hot_y);

extern SDL_DECLSPEC SDL_Cursor * SDLCALL SDL_CreateSystemCursor(SDL_SystemCursor id);

extern SDL_DECLSPEC bool SDLCALL SDL_SetCursor(SDL_Cursor *cursor);

extern SDL_DECLSPEC SDL_Cursor * SDLCALL SDL_GetCursor(void);

extern SDL_DECLSPEC SDL_Cursor * SDLCALL SDL_GetDefaultCursor(void);

extern SDL_DECLSPEC void SDLCALL SDL_DestroyCursor(SDL_Cursor *cursor);

extern SDL_DECLSPEC bool SDLCALL SDL_ShowCursor(void);

extern SDL_DECLSPEC bool SDLCALL SDL_HideCursor(void);

extern SDL_DECLSPEC bool SDLCALL SDL_CursorVisible(void);

#include <SDL3/SDL_close_code.h>

#ifdef __cplusplus
}
#endif

#endif /* SDL_mouse_h_ */
