/*
  SDLop - libdrm / libgbm dlopen loaders (SDL3-style dynamic loading).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "sdlop_kmsdrm_dyn.h"

#include <dlfcn.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* libdrm (required)                                                    */
/* ------------------------------------------------------------------ */

#define SDLOP_DRM_SYM(rc, fn, params) SDLOP_DYNDRM_##fn SDLOP_DRM_##fn = NULL;
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_DRM_SYM
#undef SDLOP_GBM_SYM
#undef SDLOP_DRM_ATOMIC_SYM
#define SDLOP_DRM_ATOMIC_SYM(rc, fn, params) SDLOP_DYNADRM_##fn SDLOP_DRM_ATOMIC_##fn = NULL;
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_DRM_ATOMIC_SYM
static bool atomic_available = false;

static void *drm_lib = NULL;
static int drm_load_refcount = 0;

bool SDLOP_KMSDRM_LoadSymbols(void)
{
    if (drm_lib) {
        drm_load_refcount++;
        return true;
    }

    const char *libname = getenv("SDLOP_LIB_DRM");
    if (!libname || !libname[0]) {
        libname = "libdrm.so.2";
    }
    drm_lib = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
    if (!drm_lib) {
        const char *err = dlerror();
        return SDL_SetError("Could not load %s: %s", libname, err ? err : "unknown error");
    }

    bool ok = true;
#define SDLOP_DRM_SYM(rc, fn, params)                                     \
    SDLOP_DRM_##fn = (SDLOP_DYNDRM_##fn)(uintptr_t)dlsym(drm_lib, #fn);    \
    if (!SDLOP_DRM_##fn) {                                                \
        if (ok) {                                                         \
            SDL_SetError("Could not load symbol %s from %s", #fn, libname); \
        }                                                                 \
        ok = false;                                                       \
    }
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_DRM_SYM
#undef SDLOP_GBM_SYM

    if (!ok) {
#define SDLOP_DRM_SYM(rc, fn, params) SDLOP_DRM_##fn = NULL;
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_DRM_SYM
#undef SDLOP_GBM_SYM
#define SDLOP_DRM_ATOMIC_SYM(rc, fn, params) SDLOP_DRM_ATOMIC_##fn = NULL;
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_DRM_ATOMIC_SYM
        atomic_available = false;
        dlclose(drm_lib);
        drm_lib = NULL;
        return false;
    }
    /* optional atomic modesetting symbols: missing -> legacy modesetting */
    {
        bool atomic_ok = true;
#define SDLOP_DRM_ATOMIC_SYM(rc, fn, params)                              \
    SDLOP_DRM_ATOMIC_##fn = (SDLOP_DYNADRM_##fn)(uintptr_t)dlsym(drm_lib, #fn); \
    if (!SDLOP_DRM_ATOMIC_##fn) {                                         \
        atomic_ok = false;                                                \
    }
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_DRM_SYM
#undef SDLOP_GBM_SYM
#undef SDLOP_DRM_ATOMIC_SYM
        if (!atomic_ok) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                        "KMSDRM: libdrm atomic modesetting API not found; using legacy modesetting");
            atomic_available = false;
        } else {
            atomic_available = true;
        }
    }

    drm_load_refcount = 1;
    return true;
}

void SDLOP_KMSDRM_UnloadSymbols(void)
{
    if (!drm_lib || --drm_load_refcount > 0) {
        return;
    }
    drm_load_refcount = 0;
#define SDLOP_DRM_SYM(rc, fn, params) SDLOP_DRM_##fn = NULL;
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_DRM_SYM
#undef SDLOP_GBM_SYM
#define SDLOP_DRM_ATOMIC_SYM(rc, fn, params) SDLOP_DRM_ATOMIC_##fn = NULL;
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_DRM_ATOMIC_SYM
    atomic_available = false;
    dlclose(drm_lib);
    drm_lib = NULL;
}

/* ------------------------------------------------------------------ */
/* libgbm (optional)                                                    */
/* ------------------------------------------------------------------ */

#define SDLOP_GBM_SYM(rc, fn, params) SDLOP_DYNGBM_##fn SDLOP_GBM_##fn = NULL;
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_GBM_SYM
#undef SDLOP_DRM_SYM

static void *gbm_lib = NULL;
static int gbm_load_refcount = 0;
static bool gbm_usable = false;

bool SDLOP_GBM_LoadSymbols(void)
{
    if (gbm_lib) {
        gbm_load_refcount++;
        return gbm_usable;
    }

    const char *libname = getenv("SDLOP_LIB_GBM");
    if (!libname || !libname[0]) {
        libname = "libgbm.so.1";
    }
    gbm_lib = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
    if (!gbm_lib) {
        /* optional: not an error, the dumb-buffer path still works */
        return false;
    }

    bool ok = true;
#define SDLOP_GBM_SYM(rc, fn, params)                                     \
    SDLOP_GBM_##fn = (SDLOP_DYNGBM_##fn)(uintptr_t)dlsym(gbm_lib, #fn);    \
    if (!SDLOP_GBM_##fn) {                                                \
        ok = false;                                                       \
    }
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_GBM_SYM
#undef SDLOP_DRM_SYM

    if (!ok) {
#define SDLOP_GBM_SYM(rc, fn, params) SDLOP_GBM_##fn = NULL;
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_GBM_SYM
#undef SDLOP_DRM_SYM
        dlclose(gbm_lib);
        gbm_lib = NULL;
        return false;
    }
    gbm_load_refcount = 1;
    gbm_usable = true;
    return true;
}

void SDLOP_GBM_UnloadSymbols(void)
{
    if (!gbm_lib || --gbm_load_refcount > 0) {
        return;
    }
    gbm_load_refcount = 0;
    gbm_usable = false;
#define SDLOP_GBM_SYM(rc, fn, params) SDLOP_GBM_##fn = NULL;
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_GBM_SYM
#undef SDLOP_DRM_SYM
    dlclose(gbm_lib);
    gbm_lib = NULL;
}

bool SDLOP_GBM_Available(void)
{
    return gbm_usable;
}

bool SDLOP_DRM_ATOMIC_Available(void)
{
    return atomic_available;
}
