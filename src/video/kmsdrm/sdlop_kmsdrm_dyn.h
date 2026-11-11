/*
  SDLop - dynamic (dlopen) loading of libdrm / libgbm, SDL3-style.
  Call sites use the SDLOP_DRM_* / SDLOP_GBM_* function pointers explicitly;
  load via SDLOP_KMSDRM_LoadSymbols() (libdrm, required) and
  SDLOP_GBM_LoadSymbols() (libgbm, optional - GL path and hardware cursor
  degrade gracefully without it).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_kmsdrm_dyn_h_
#define sdlop_kmsdrm_dyn_h_

#include <stdbool.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#ifdef __cplusplus
extern "C" {
#endif

bool SDLOP_KMSDRM_LoadSymbols(void);   /* libdrm - required */
void SDLOP_KMSDRM_UnloadSymbols(void);
bool SDLOP_GBM_LoadSymbols(void);      /* libgbm - optional */
void SDLOP_GBM_UnloadSymbols(void);
bool SDLOP_GBM_Available(void);        /* true iff GBM symbols are loaded */

#define SDLOP_DRM_SYM(rc, fn, params) \
    typedef rc(*SDLOP_DYNDRM_##fn) params; \
    extern SDLOP_DYNDRM_##fn SDLOP_DRM_##fn;
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_DRM_SYM
#undef SDLOP_GBM_SYM

#define SDLOP_GBM_SYM(rc, fn, params) \
    typedef rc(*SDLOP_DYNGBM_##fn) params; \
    extern SDLOP_DYNGBM_##fn SDLOP_GBM_##fn;
#include "sdlop_kmsdrm_sym.h"
#undef SDLOP_GBM_SYM
#undef SDLOP_DRM_SYM

#ifdef __cplusplus
}
#endif

#endif /* sdlop_kmsdrm_dyn_h_ */
