/*
  SDLop - libxkbcommon dlopen loader (SDL3-style dynamic loading).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "internal/sdlop_xkb_dyn.h"

#if SDLOP_HAVE_XKBCOMMON

#include <dlfcn.h>
#include <stdlib.h>

#define SDLOP_XKB_SYM(rc, fn, params) SDLOP_DYNXKB_##fn SDLOP_XKB_##fn = NULL;
#include "internal/sdlop_xkb_sym.h"
#include "internal/sdlop_xkb_opt_sym.h"
#undef SDLOP_XKB_SYM

static void *xkb_lib = NULL;
static int xkb_load_refcount = 0;

bool SDLOP_Xkb_LoadSymbols(void)
{
    if (xkb_lib) {
        xkb_load_refcount++;
        return true;
    }

    const char *libname = SDL_GetHint("SDLOP_LIB_XKB");
    if (!libname || !libname[0]) {
        libname = "libxkbcommon.so.0";
    }
    xkb_lib = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
    if (!xkb_lib) {
        const char *err = dlerror();
        return SDL_SetError("Could not load %s: %s", libname, err ? err : "unknown error");
    }

    bool ok = true;
#define SDLOP_XKB_SYM(rc, fn, params)                                     \
    SDLOP_XKB_##fn = (SDLOP_DYNXKB_##fn)(uintptr_t)dlsym(xkb_lib, #fn);   \
    if (!SDLOP_XKB_##fn) {                                                \
        if (ok) {                                                         \
            SDL_SetError("Could not load symbol %s from %s", #fn, libname); \
        }                                                                 \
        ok = false;                                                       \
    }
#include "internal/sdlop_xkb_sym.h"
#undef SDLOP_XKB_SYM

    if (!ok) {
        /* leave no pointers into the closed library */
#define SDLOP_XKB_SYM(rc, fn, params) SDLOP_XKB_##fn = NULL;
#include "internal/sdlop_xkb_sym.h"
#include "internal/sdlop_xkb_opt_sym.h"
#undef SDLOP_XKB_SYM
        dlclose(xkb_lib);
        xkb_lib = NULL;
        return false; /* error already set: first missing symbol */
    }
    /* optional symbols: each may be missing */
#define SDLOP_XKB_SYM(rc, fn, params) \
    SDLOP_XKB_##fn = (SDLOP_DYNXKB_##fn)(uintptr_t)dlsym(xkb_lib, #fn);
#include "internal/sdlop_xkb_opt_sym.h"
#undef SDLOP_XKB_SYM
    xkb_load_refcount = 1;
    return true;
}

void SDLOP_Xkb_UnloadSymbols(void)
{
    if (!xkb_lib || --xkb_load_refcount > 0) {
        return;
    }
    xkb_load_refcount = 0;
#define SDLOP_XKB_SYM(rc, fn, params) SDLOP_XKB_##fn = NULL;
#include "internal/sdlop_xkb_sym.h"
#include "internal/sdlop_xkb_opt_sym.h"
#undef SDLOP_XKB_SYM
    dlclose(xkb_lib);
    xkb_lib = NULL;
}

bool SDLOP_Xkb_HaveCompose(void)
{
    return SDLOP_XKB_xkb_compose_table_new_from_locale && SDLOP_XKB_xkb_compose_table_unref &&
           SDLOP_XKB_xkb_compose_state_new && SDLOP_XKB_xkb_compose_state_unref &&
           SDLOP_XKB_xkb_compose_state_feed && SDLOP_XKB_xkb_compose_state_reset &&
           SDLOP_XKB_xkb_compose_state_get_status && SDLOP_XKB_xkb_compose_state_get_utf8;
}

#else /* !SDLOP_HAVE_XKBCOMMON */

bool SDLOP_Xkb_LoadSymbols(void)
{
    return SDL_SetError("xkbcommon support was not built in");
}

void SDLOP_Xkb_UnloadSymbols(void)
{
}

bool SDLOP_Xkb_HaveCompose(void)
{
    return false;
}

#endif /* SDLOP_HAVE_XKBCOMMON */
