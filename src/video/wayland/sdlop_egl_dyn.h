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

#include "sdlop_egl_sym.h"

#undef SDLOP_EGL_SYM

#ifdef __cplusplus
}
#endif

#endif /* sdlop_egl_dyn_h_ */
