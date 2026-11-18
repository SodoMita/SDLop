/*
  SDLop - libX11 dlopen loader (SDL3-style dynamic loading).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "sdlop_x11_dyn.h"

#include <dlfcn.h>
#include <stdlib.h>

#define SDLOP_X11_SYM(rc, fn, params) SDLOP_DYNX11_##fn SDLOP_X11_##fn = NULL;
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_SYM

static void *x11_lib = NULL;
static int x11_load_refcount = 0;

bool SDLOP_X11_LoadSymbols(void)
{
    if (x11_lib) {
        x11_load_refcount++;
        return true;
    }

    const char *libname = getenv("SDLOP_LIB_X11");
    if (!libname || !libname[0]) {
        libname = "libX11.so.6";
    }
    x11_lib = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
    if (!x11_lib) {
        /* fall back to the unversioned name (some minimal systems ship
         * only the .so symlink target under a different soname scheme) */
        const char *fallback = "libX11.so";
        x11_lib = dlopen(fallback, RTLD_NOW | RTLD_LOCAL);
        if (!x11_lib) {
            const char *err = dlerror();
            return SDL_SetError("Could not load %s: %s", libname, err ? err : "unknown error");
        }
        libname = fallback;
    }

    bool ok = true;
#define SDLOP_X11_SYM(rc, fn, params)                                     \
    SDLOP_X11_##fn = (SDLOP_DYNX11_##fn)(uintptr_t)dlsym(x11_lib, #fn);   \
    if (!SDLOP_X11_##fn) {                                                \
        if (ok) {                                                         \
            SDL_SetError("Could not load symbol %s from %s", #fn, libname); \
        }                                                                 \
        ok = false;                                                       \
    }
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_SYM

    if (!ok) {
        /* leave no pointers into the closed library */
#define SDLOP_X11_SYM(rc, fn, params) SDLOP_X11_##fn = NULL;
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_SYM
        dlclose(x11_lib);
        x11_lib = NULL;
        return false; /* error already set: first missing symbol */
    }
    x11_load_refcount = 1;
    return true;
}

void SDLOP_X11_UnloadSymbols(void)
{
    if (!x11_lib || --x11_load_refcount > 0) {
        return;
    }
    x11_load_refcount = 0;
#define SDLOP_X11_SYM(rc, fn, params) SDLOP_X11_##fn = NULL;
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_SYM
    dlclose(x11_lib);
    x11_lib = NULL;
}
