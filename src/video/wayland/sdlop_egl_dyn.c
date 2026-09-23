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
SDLOP_EGL_SYM(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType))
SDLOP_EGL_SYM(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))
SDLOP_EGL_SYM(EGLBoolean, eglTerminate, (EGLDisplay))
SDLOP_EGL_SYM(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
SDLOP_EGL_SYM(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))
SDLOP_EGL_SYM(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext))
SDLOP_EGL_SYM(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *))
SDLOP_EGL_SYM(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface))
SDLOP_EGL_SYM(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
SDLOP_EGL_SYM(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface))
SDLOP_EGL_SYM(EGLBoolean, eglSwapInterval, (EGLDisplay, EGLint))
SDLOP_EGL_SYM(const char *, eglQueryString, (EGLDisplay, EGLint))
SDLOP_EGL_SYM(EGLBoolean, eglQueryContext, (EGLDisplay, EGLContext, EGLint, EGLint *))
SDLOP_EGL_SYM(EGLBoolean, eglQuerySurface, (EGLDisplay, EGLSurface, EGLint, EGLint *))
SDLOP_EGL_SYM(EGLint, eglGetError, (void))
SDLOP_EGL_SYM(EGLBoolean, eglBindAPI, (EGLenum))
SDLOP_EGL_SYM(SDLOP_EGLProc, eglGetProcAddress, (const char *))
#undef SDLOP_EGL_SYM

static void *egl_lib = NULL;

bool SDLOP_EGL_LoadSymbols(void)
{
    if (egl_lib) {
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
        ok = false;                                                    \
    }
    SDLOP_EGL_SYM(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType))
    SDLOP_EGL_SYM(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))
    SDLOP_EGL_SYM(EGLBoolean, eglTerminate, (EGLDisplay))
    SDLOP_EGL_SYM(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
    SDLOP_EGL_SYM(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))
    SDLOP_EGL_SYM(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext))
    SDLOP_EGL_SYM(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *))
    SDLOP_EGL_SYM(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface))
    SDLOP_EGL_SYM(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
    SDLOP_EGL_SYM(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface))
    SDLOP_EGL_SYM(EGLBoolean, eglSwapInterval, (EGLDisplay, EGLint))
    SDLOP_EGL_SYM(const char *, eglQueryString, (EGLDisplay, EGLint))
    SDLOP_EGL_SYM(EGLBoolean, eglQueryContext, (EGLDisplay, EGLContext, EGLint, EGLint *))
    SDLOP_EGL_SYM(EGLBoolean, eglQuerySurface, (EGLDisplay, EGLSurface, EGLint, EGLint *))
    SDLOP_EGL_SYM(EGLint, eglGetError, (void))
    SDLOP_EGL_SYM(EGLBoolean, eglBindAPI, (EGLenum))
    SDLOP_EGL_SYM(SDLOP_EGLProc, eglGetProcAddress, (const char *))
#undef SDLOP_EGL_SYM

    if (!ok) {
        dlclose(egl_lib);
        egl_lib = NULL;
        return SDL_SetError("Could not load all libEGL symbols from %s", libname);
    }
    return true;
}

void SDLOP_EGL_UnloadSymbols(void)
{
    if (!egl_lib) {
        return;
    }
#define SDLOP_EGL_SYM(rc, fn, params) SDLOP_EGL_##fn = NULL;
    SDLOP_EGL_SYM(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType))
    SDLOP_EGL_SYM(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))
    SDLOP_EGL_SYM(EGLBoolean, eglTerminate, (EGLDisplay))
    SDLOP_EGL_SYM(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
    SDLOP_EGL_SYM(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))
    SDLOP_EGL_SYM(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext))
    SDLOP_EGL_SYM(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *))
    SDLOP_EGL_SYM(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface))
    SDLOP_EGL_SYM(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
    SDLOP_EGL_SYM(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface))
    SDLOP_EGL_SYM(EGLBoolean, eglSwapInterval, (EGLDisplay, EGLint))
    SDLOP_EGL_SYM(const char *, eglQueryString, (EGLDisplay, EGLint))
    SDLOP_EGL_SYM(EGLBoolean, eglQueryContext, (EGLDisplay, EGLContext, EGLint, EGLint *))
    SDLOP_EGL_SYM(EGLBoolean, eglQuerySurface, (EGLDisplay, EGLSurface, EGLint, EGLint *))
    SDLOP_EGL_SYM(EGLint, eglGetError, (void))
    SDLOP_EGL_SYM(EGLBoolean, eglBindAPI, (EGLenum))
    SDLOP_EGL_SYM(SDLOP_EGLProc, eglGetProcAddress, (const char *))
#undef SDLOP_EGL_SYM
    dlclose(egl_lib);
    egl_lib = NULL;
}
