/*
  SDLop - libxkbcommon dlopen loader (SDL3-style dynamic loading).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "internal/sdlop_xkb_dyn.h"

#include <dlfcn.h>
#include <stdlib.h>

#define SDLOP_XKB_SYM(rc, fn, params) SDLOP_DYNXKB_##fn SDLOP_XKB_##fn = NULL;
#include "internal/sdlop_xkb_sym.h"
#undef SDLOP_XKB_SYM

static void *xkb_lib = NULL;
static int xkb_load_refcount = 0;

bool SDLOP_Xkb_LoadSymbols(void)
{
    if (xkb_lib) {
        xkb_load_refcount++;
        return true;
    }

    const char *libname = getenv("SDLOP_LIB_XKB");
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
        ok = false;                                                       \
    }
#include "internal/sdlop_xkb_sym.h"
#undef SDLOP_XKB_SYM

    if (!ok) {
        dlclose(xkb_lib);
        xkb_lib = NULL;
        return SDL_SetError("Could not load all libxkbcommon symbols from %s", libname);
    }
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
#undef SDLOP_XKB_SYM
    dlclose(xkb_lib);
    xkb_lib = NULL;
}
