/*
  SDLop - dynamic (dlopen) loading of libxkbcommon, SDL3-style.
  Call sites use the SDLOP_XKB_* function pointers explicitly.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_xkb_dyn_h_
#define sdlop_xkb_dyn_h_

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool SDLOP_Xkb_LoadSymbols(void);
extern void SDLOP_Xkb_UnloadSymbols(void);

#define SDLOP_XKB_SYM(rc, fn, params) \
    typedef rc(*SDLOP_DYNXKB_##fn) params; \
    extern SDLOP_DYNXKB_##fn SDLOP_XKB_##fn;

#include "sdlop_xkb_sym.h"
#include "sdlop_xkb_opt_sym.h" /* optional: may stay NULL */

#undef SDLOP_XKB_SYM

/* true when the optional compose API was found */
extern bool SDLOP_Xkb_HaveCompose(void);

#ifdef __cplusplus
}
#endif

#endif /* sdlop_xkb_dyn_h_ */
