/*
  SDLop - libwayland-client dlopen loader (SDL3-style dynamic loading).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "sdlop_wayland_dyn.h"

#include <dlfcn.h>
#include <stdlib.h>

/* pointer definitions (SDLOP_WL_* names are not macro-affected) */
#define SDLOP_WAYLAND_SYM(rc, fn, params) SDLOP_DYNWL_##fn SDLOP_WL_##fn = NULL;
#include "sdlop_wayland_sym.h"
#undef SDLOP_WAYLAND_SYM

static void *wayland_lib = NULL;
static int wayland_load_refcount = 0;

bool SDLOP_Wayland_LoadSymbols(void)
{
    if (wayland_lib) {
        wayland_load_refcount++;
        return true;
    }

    const char *libname = getenv("SDLOP_LIB_WAYLAND");
    if (!libname || !libname[0]) {
        libname = "libwayland-client.so.0";
    }
    wayland_lib = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
    if (!wayland_lib) {
        const char *err = dlerror();
        SDL_SetError("Could not load %s: %s", libname, err ? err : "unknown error");
        return false;
    }

    bool ok = true;
#define SDLOP_WAYLAND_SYM(rc, fn, params)                                            \
    SDLOP_WL_##fn = (SDLOP_DYNWL_##fn)(uintptr_t)dlsym(wayland_lib, #fn);            \
    if (!SDLOP_WL_##fn) {                                                            \
        if (ok) {                                                                    \
            SDL_SetError("Could not load symbol %s from %s", #fn, libname);           \
        }                                                                            \
        ok = false;                                                                  \
    }
#include "sdlop_wayland_sym.h"
#undef SDLOP_WAYLAND_SYM

    if (!ok) {
        /* leave no pointers into the closed library */
#define SDLOP_WAYLAND_SYM(rc, fn, params) SDLOP_WL_##fn = NULL;
#include "sdlop_wayland_sym.h"
#undef SDLOP_WAYLAND_SYM
        dlclose(wayland_lib);
        wayland_lib = NULL;
        return false; /* error already set: first missing symbol */
    }
    wayland_load_refcount = 1;
    return true;
}

void SDLOP_Wayland_UnloadSymbols(void)
{
    if (!wayland_lib || --wayland_load_refcount > 0) {
        return;
    }
    wayland_load_refcount = 0;

#define SDLOP_WAYLAND_SYM(rc, fn, params) SDLOP_WL_##fn = NULL;
#include "sdlop_wayland_sym.h"
#undef SDLOP_WAYLAND_SYM

    dlclose(wayland_lib);
    wayland_lib = NULL;
}
