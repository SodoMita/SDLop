/*
  SDLop - libEGL dlopen loader (SDL3-style dynamic loading).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "sdlop_egl_dyn.h"

#include <dlfcn.h>
#include <stdlib.h>

#define SDLOP_EGL_SYM(rc, fn, params) SDLOP_DYNEGL_##fn SDLOP_EGL_##fn = NULL;
#include "sdlop_egl_sym.h"
#undef SDLOP_EGL_SYM

static void *egl_lib = NULL;
static int egl_load_refcount = 0;

bool SDLOP_EGL_LoadSymbols(void)
{
    if (egl_lib) {
        egl_load_refcount++;
        return true;
    }

    const char *libname = getenv("SDLOP_LIB_EGL");
    if (!libname || !libname[0]) {
        libname = "libEGL.so.1";
    }
    egl_lib = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
    if (!egl_lib) {
        const char *err = dlerror();
        return SDL_SetError("Could not load %s: %s", libname, err ? err : "unknown error");
    }

    bool ok = true;
#define SDLOP_EGL_SYM(rc, fn, params)                                  \
    SDLOP_EGL_##fn = (SDLOP_DYNEGL_##fn)(uintptr_t)dlsym(egl_lib, #fn); \
    if (!SDLOP_EGL_##fn) {                                             \
        if (ok) {                                                      \
            SDL_SetError("Could not load symbol %s from %s", #fn, libname); \
        }                                                              \
        ok = false;                                                    \
    }
#include "sdlop_egl_sym.h"
#undef SDLOP_EGL_SYM

    if (!ok) {
        /* leave no pointers into the closed library */
#define SDLOP_EGL_SYM(rc, fn, params) SDLOP_EGL_##fn = NULL;
#include "sdlop_egl_sym.h"
#undef SDLOP_EGL_SYM
        dlclose(egl_lib);
        egl_lib = NULL;
        return false; /* error already set: first missing symbol */
    }
    egl_load_refcount = 1;
    return true;
}

void SDLOP_EGL_UnloadSymbols(void)
{
    if (!egl_lib || --egl_load_refcount > 0) {
        return;
    }
    egl_load_refcount = 0;
#define SDLOP_EGL_SYM(rc, fn, params) SDLOP_EGL_##fn = NULL;
#include "sdlop_egl_sym.h"
#undef SDLOP_EGL_SYM
    dlclose(egl_lib);
    egl_lib = NULL;
}
