/*
  SDLop - dynamic (dlopen) loading of libX11, SDL3-style.
  Call sites use the SDLOP_X11_* function pointers explicitly; load via
  SDLOP_X11_LoadSymbols() before first use. Nothing links against libX11:
  the library works (with other video drivers) on systems where libX11 is
  absent.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_x11_dyn_h_
#define sdlop_x11_dyn_h_

#include <stdbool.h>
#include "sdlop_x11_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

extern bool SDLOP_X11_LoadSymbols(void);
extern void SDLOP_X11_UnloadSymbols(void);

#define SDLOP_X11_SYM(rc, fn, params)    \
    typedef rc(*SDLOP_DYNX11_##fn) params;  \
    extern SDLOP_DYNX11_##fn SDLOP_X11_##fn;

#include "sdlop_x11_sym.h"

#undef SDLOP_X11_SYM

#ifdef __cplusplus
}
#endif

#endif /* sdlop_x11_dyn_h_ */
