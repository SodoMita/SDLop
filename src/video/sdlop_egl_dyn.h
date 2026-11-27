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

#ifndef SDLOP_HAVE_EGL
#  if defined(__has_include)
#    if __has_include(<EGL/egl.h>) && __has_include(<EGL/eglext.h>)
#      define SDLOP_HAVE_EGL 1
#    else
#      define SDLOP_HAVE_EGL 0
#    endif
#  else
#    define SDLOP_HAVE_EGL 0
#  endif
#endif

#if SDLOP_HAVE_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern bool SDLOP_EGL_LoadSymbols(void);
extern void SDLOP_EGL_UnloadSymbols(void);

#if SDLOP_HAVE_EGL

typedef void (*SDLOP_EGLProc)(void);

#define SDLOP_EGL_SYM(rc, fn, params)   \
    typedef rc(*SDLOP_DYNEGL_##fn) params; \
    extern SDLOP_DYNEGL_##fn SDLOP_EGL_##fn;

#include "sdlop_egl_sym.h"

#undef SDLOP_EGL_SYM

/* eglGetPlatformDisplay(EXT) honoring SDL_EGL_SetAttributeCallbacks:
 * sdl_attribs (EGL_NONE-terminated, may be NULL) plus the app's
 * platform attributes. EGL_NO_DISPLAY when unavailable or on error. */
extern EGLDisplay SDLOP_EGL_PlatformDisplay(EGLenum platform, void *native, const EGLint *sdl_attribs);
/* append the app's surface (context=false) or context attributes to
 * attribs[0..n) and terminate it; returns the new n, -1 on error (set) */
extern int SDLOP_EGL_AppendUserAttribs(bool context, EGLDisplay display, EGLConfig config, EGLint *attribs, int n,
                                       int max);

/* OpenGL ES version handling of stock SDL_EGL_ChooseConfig/CreateContext,
 * from sdlop_glattrs: the EGL_RENDERABLE_TYPE bit for the requested ES
 * major version, and the context version attributes (X.0 ->
 * EGL_CONTEXT_CLIENT_VERSION X; X.Y -> EGL_KHR_create_context major/minor,
 * error without it). AppendESVersion returns the new n, -1 on error (set). */
extern EGLint SDLOP_EGL_ESRenderableType(EGLDisplay display);
extern int SDLOP_EGL_AppendESVersion(EGLDisplay display, EGLint *attribs, int n);

#endif /* SDLOP_HAVE_EGL */

#ifdef __cplusplus
}
#endif

#endif /* sdlop_egl_dyn_h_ */
