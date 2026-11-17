/*
  SDLop - libX11 dlopen loader.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "sdlop_x11_dyn.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>

#define SDLOP_X11_SYM(rc, fn, params) SDLOP_DYNX11_##fn SDLOP_X11_##fn = NULL;
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_SYM

static void *x11_lib;
static unsigned x11_load_refcount;

bool SDLOP_X11_LoadSymbols(void)
{
    if (x11_lib) {
        ++x11_load_refcount;
        return true;
    }

    const char *libname = getenv("SDLOP_LIB_X11");
    if (!libname || !libname[0]) {
        libname = "libX11.so.6";
    }
    x11_lib = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
    if (!x11_lib && (!getenv("SDLOP_LIB_X11") || !getenv("SDLOP_LIB_X11")[0])) {
        /* A few non-glibc systems use the unversioned soname. */
        x11_lib = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
        if (x11_lib) {
            libname = "libX11.so";
        }
    }
    if (!x11_lib) {
        const char *err = dlerror();
        SDL_SetError("Could not load %s: %s", libname, err ? err : "unknown error");
        return false;
    }

    bool ok = true;
#define SDLOP_X11_SYM(rc, fn, params)                                      \
    SDLOP_X11_##fn = (SDLOP_DYNX11_##fn)(uintptr_t)dlsym(x11_lib, #fn);     \
    if (!SDLOP_X11_##fn) {                                                  \
        if (ok) {                                                           \
            SDL_SetError("Could not load symbol %s from %s", #fn, libname); \
        }                                                                   \
        ok = false;                                                         \
    }
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_SYM

    if (!ok) {
#define SDLOP_X11_SYM(rc, fn, params) SDLOP_X11_##fn = NULL;
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_SYM
        dlclose(x11_lib);
        x11_lib = NULL;
        return false;
    }

    x11_load_refcount = 1;
    return true;
}

void SDLOP_X11_UnloadSymbols(void)
{
    if (!x11_lib || x11_load_refcount == 0) {
        return;
    }
    if (--x11_load_refcount != 0) {
        return;
    }

#define SDLOP_X11_SYM(rc, fn, params) SDLOP_X11_##fn = NULL;
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_SYM

    dlclose(x11_lib);
    x11_lib = NULL;
}
