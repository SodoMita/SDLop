/*
  SDLop - runtime-loaded Xlib API.

  The X11 backend is compiled against Xlib headers but never linked against
  libX11. This keeps the SDLop shared library usable on systems where the X11
  runtime is not installed. The library is loaded only when the x11 video
  driver is selected (default: libX11.so.6, override: SDLOP_LIB_X11).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_x11_dyn_h_
#define sdlop_x11_dyn_h_

#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool SDLOP_X11_LoadSymbols(void);
extern void SDLOP_X11_UnloadSymbols(void);

#define SDLOP_X11_SYM(rc, fn, params) \
    typedef rc(*SDLOP_DYNX11_##fn) params; \
    extern SDLOP_DYNX11_##fn SDLOP_X11_##fn;
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_SYM

#ifdef __cplusplus
}
#endif

#endif /* sdlop_x11_dyn_h_ */
