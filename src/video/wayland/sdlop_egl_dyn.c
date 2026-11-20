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

    const char *libname = SDL_GetHint("SDLOP_LIB_EGL");
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

/* ------------------------------------------------------------------ */
/* SDL_EGL_SetAttributeCallbacks support (shared by the EGL drivers)   */
/* ------------------------------------------------------------------ */

EGLDisplay SDLOP_EGL_PlatformDisplay(EGLenum platform, void *native, const EGLint *sdl_attribs)
{
    SDL_EGLAttribArrayCallback platform_cb = NULL;
    void *userdata = NULL;
    SDLOP_GetEGLAttributeCallbacks(&platform_cb, NULL, NULL, &userdata);

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_ext =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)SDLOP_EGL_eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!platform_cb) {
        return get_ext ? get_ext(platform, native, sdl_attribs) : EGL_NO_DISPLAY;
    }

    SDL_EGLAttrib *user = platform_cb(userdata);
    if (!user) {
        SDL_SetError("EGL platform attribute callback returned NULL pointer");
        return EGL_NO_DISPLAY;
    }
    /* SDL's own attributes (e.g. the X screen) first, then the app's */
    EGLAttrib merged[64];
    int n = 0;
    for (const EGLint *a = sdl_attribs; a && *a != EGL_NONE && n + 3 < 64; a += 2) {
        merged[n++] = a[0];
        merged[n++] = a[1];
    }
    for (const SDL_EGLAttrib *a = user; *a != EGL_NONE; a += 2) {
        if (n + 3 >= 64) {
            SDL_free(user);
            SDL_SetError("EGL platform attribute callback returned too many attributes");
            return EGL_NO_DISPLAY;
        }
        merged[n++] = (EGLAttrib)a[0];
        merged[n++] = (EGLAttrib)a[1];
    }
    merged[n] = EGL_NONE;
    SDL_free(user);

    /* EGL 1.5 core takes EGLAttrib lists; the EXT entry point EGLint ones */
    typedef EGLDisplay (*PFN_getplatformdisplay)(EGLenum, void *, const EGLAttrib *);
    PFN_getplatformdisplay get_core = (PFN_getplatformdisplay)SDLOP_EGL_eglGetProcAddress("eglGetPlatformDisplay");
    if (get_core) {
        return get_core(platform, native, merged);
    }
    if (get_ext) {
        EGLint narrow[64];
        for (int i = 0; i <= n; ++i) {
            narrow[i] = (EGLint)merged[i];
        }
        return get_ext(platform, native, narrow);
    }
    return EGL_NO_DISPLAY;
}

int SDLOP_EGL_AppendUserAttribs(bool context, EGLDisplay display, EGLConfig config, EGLint *attribs, int n,
                                int max)
{
    SDL_EGLIntArrayCallback surface_cb = NULL, context_cb = NULL;
    void *userdata = NULL;
    SDLOP_GetEGLAttributeCallbacks(NULL, &surface_cb, &context_cb, &userdata);
    SDL_EGLIntArrayCallback cb = context ? context_cb : surface_cb;
    if (cb) {
        SDL_EGLint *user = cb(userdata, (SDL_EGLDisplay)display, (SDL_EGLConfig)config);
        if (!user) {
            SDL_SetError("EGL %s attribute callback returned NULL pointer", context ? "context" : "surface");
            return -1;
        }
        for (const SDL_EGLint *a = user; *a != EGL_NONE; a += 2) {
            if (n + 3 >= max) {
                SDL_free(user);
                SDL_SetError("EGL %s attribute callback returned too many attributes",
                             context ? "context" : "surface");
                return -1;
            }
            attribs[n++] = a[0];
            attribs[n++] = a[1];
        }
        SDL_free(user);
    }
    attribs[n] = EGL_NONE;
    return n;
}
