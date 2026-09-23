/*
  SDLop - dynamic (dlopen) loading of libEGL, SDL3-style.
  Call sites use the SDLOP_EGL_* function pointers explicitly; load via
  SDLOP_EGL_LoadSymbols() before first use (GL paths only - software and
  Vulkan work without libEGL present).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_egl_dyn_h_
#define sdlop_egl_dyn_h_

#include <stdbool.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool SDLOP_EGL_LoadSymbols(void);
extern void SDLOP_EGL_UnloadSymbols(void);

typedef void (*SDLOP_EGLProc)(void);

#define SDLOP_EGL_SYM(rc, fn, params)   \
    typedef rc(*SDLOP_DYNEGL_##fn) params; \
    extern SDLOP_DYNEGL_##fn SDLOP_EGL_##fn;

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

#ifdef __cplusplus
}
#endif

#endif /* sdlop_egl_dyn_h_ */
