/*
  SDLop - dynamic (dlopen) loading of libxkbcommon, SDL3-style.
  Call sites use the SDLOP_XKB_* function pointers explicitly.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_xkb_dyn_h_
#define sdlop_xkb_dyn_h_

#include <stdbool.h>

#ifndef SDLOP_HAVE_XKBCOMMON
#  if defined(__has_include)
#    if __has_include(<xkbcommon/xkbcommon.h>) && __has_include(<xkbcommon/xkbcommon-compose.h>)
#      define SDLOP_HAVE_XKBCOMMON 1
#    else
#      define SDLOP_HAVE_XKBCOMMON 0
#    endif
#  else
#    define SDLOP_HAVE_XKBCOMMON 0
#  endif
#endif

#if SDLOP_HAVE_XKBCOMMON
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern bool SDLOP_Xkb_LoadSymbols(void);
extern void SDLOP_Xkb_UnloadSymbols(void);

#if SDLOP_HAVE_XKBCOMMON
#define SDLOP_XKB_SYM(rc, fn, params) \
    typedef rc(*SDLOP_DYNXKB_##fn) params; \
    extern SDLOP_DYNXKB_##fn SDLOP_XKB_##fn;

#include "sdlop_xkb_sym.h"
#include "sdlop_xkb_opt_sym.h" /* optional: may stay NULL */

#undef SDLOP_XKB_SYM
#endif /* SDLOP_HAVE_XKBCOMMON */

/* true when the optional compose API was found */
extern bool SDLOP_Xkb_HaveCompose(void);

#ifdef __cplusplus
}
#endif

#endif /* sdlop_xkb_dyn_h_ */
