/*
  SDLop - dynamic (dlopen) loading of the X11 client libraries.

  Nothing is linked against libxcb and friends: SDLOP_X11_LoadSymbols()
  dlopen()s every module listed in sdlop_x11_sym.h and fills the X11_*
  pointers. libxcb itself is required; every other module is optional and
  the driver degrades feature by feature when one is missing (no MIT-SHM ->
  xcb_put_image, no XInput2 -> warp-based relative mouse, no RandR -> one
  display the size of the root window, no xkbcommon-x11 -> keymap from
  _XKB_RULES_NAMES).

  Library names can be overridden per module (SDLOP_LIB_XCB,
  SDLOP_LIB_XCB_SHM, SDLOP_LIB_XCB_XKB, SDLOP_LIB_XKBCOMMON_X11,
  SDLOP_LIB_XCB_XINPUT, SDLOP_LIB_XCB_RANDR). Setting an optional module's
  variable to "none" disables it, which is how the fallback paths are
  tested.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_x11_dyn_h_
#define sdlop_x11_dyn_h_

#include <stdbool.h>
#include <stdint.h>

/* the real prototypes: pointer types are derived from them via __typeof__ */
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xkb.h>
#include <xcb/xinput.h>
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xkbcommon/xkbcommon-x11.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SDLOP_X11_Module
{
#define SDLOP_X11_MODULE(mod) SDLOP_X11_MOD_##mod,
#define SDLOP_X11_SYM(mod, fn)
#define SDLOP_X11_DATA(mod, sym)
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_MODULE
#undef SDLOP_X11_SYM
#undef SDLOP_X11_DATA
    SDLOP_X11_MOD_COUNT
} SDLOP_X11_Module;

/* loads libxcb (required) and every optional module that is available */
extern bool SDLOP_X11_LoadSymbols(void);
extern void SDLOP_X11_UnloadSymbols(void);
/* true when the module and all of its symbols resolved */
extern bool SDLOP_X11_HaveModule(SDLOP_X11_Module mod);
#define SDLOP_X11_HAVE(mod) SDLOP_X11_HaveModule(SDLOP_X11_MOD_##mod)

#define SDLOP_X11_MODULE(mod)
#define SDLOP_X11_SYM(mod, fn) extern __typeof__(fn) *X11_##fn;
#define SDLOP_X11_DATA(mod, sym) extern __typeof__(sym) *X11_##sym;
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_MODULE
#undef SDLOP_X11_SYM
#undef SDLOP_X11_DATA

#ifdef __cplusplus
}
#endif

#endif /* sdlop_x11_dyn_h_ */
