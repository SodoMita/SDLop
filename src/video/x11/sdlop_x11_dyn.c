/*
  SDLop - X11 client library dlopen loader (SDL3-style dynamic loading).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "sdlop_x11_dyn.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <strings.h>

/* pointer definitions */
#define SDLOP_X11_MODULE(mod)
#define SDLOP_X11_SYM(mod, fn) __typeof__(fn) *X11_##fn = NULL;
#define SDLOP_X11_DATA(mod, sym) __typeof__(sym) *X11_##sym = NULL;
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_MODULE
#undef SDLOP_X11_SYM
#undef SDLOP_X11_DATA

typedef struct X11ModuleInfo
{
    const char *env;     /* override variable */
    const char *soname;  /* default library */
    bool required;
} X11ModuleInfo;

static const X11ModuleInfo module_info[SDLOP_X11_MOD_COUNT] = {
    [SDLOP_X11_MOD_XCB] = { "SDLOP_LIB_XCB", "libxcb.so.1", true },
    [SDLOP_X11_MOD_SHM] = { "SDLOP_LIB_XCB_SHM", "libxcb-shm.so.0", false },
    [SDLOP_X11_MOD_XKB] = { "SDLOP_LIB_XCB_XKB", "libxcb-xkb.so.1", false },
    [SDLOP_X11_MOD_XKBX11] = { "SDLOP_LIB_XKBCOMMON_X11", "libxkbcommon-x11.so.0", false },
    [SDLOP_X11_MOD_XINPUT] = { "SDLOP_LIB_XCB_XINPUT", "libxcb-xinput.so.0", false },
    [SDLOP_X11_MOD_RANDR] = { "SDLOP_LIB_XCB_RANDR", "libxcb-randr.so.0", false },
};

static void *module_handle[SDLOP_X11_MOD_COUNT];
static bool module_ok[SDLOP_X11_MOD_COUNT];
static int x11_load_refcount = 0;

/* resolve (or, with h == NULL, clear) every symbol of one module */
static bool resolve_module(SDLOP_X11_Module which, void *h, const char **missing)
{
    bool ok = true;
#define SDLOP_X11_MODULE(mod)
#define SDLOP_X11_SYM(mod, fn)                                           \
    if (which == SDLOP_X11_MOD_##mod) {                                  \
        X11_##fn = h ? (__typeof__(X11_##fn))dlsym(h, #fn) : NULL;      \
        if (h && !X11_##fn && ok) {                                      \
            ok = false;                                                  \
            *missing = #fn;                                              \
        }                                                                \
    }
#define SDLOP_X11_DATA(mod, sym) SDLOP_X11_SYM(mod, sym)
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_MODULE
#undef SDLOP_X11_SYM
#undef SDLOP_X11_DATA
    return ok;
}

static void unload_module(SDLOP_X11_Module which)
{
    const char *unused = NULL;
    resolve_module(which, NULL, &unused);
    if (module_handle[which]) {
        dlclose(module_handle[which]);
        module_handle[which] = NULL;
    }
    module_ok[which] = false;
}

static bool load_module(SDLOP_X11_Module which)
{
    const X11ModuleInfo *info = &module_info[which];
    const char *libname = getenv(info->env);
    if (libname && !info->required && strcasecmp(libname, "none") == 0) {
        return false; /* explicitly disabled */
    }
    if (!libname || !libname[0]) {
        libname = info->soname;
    }
    void *h = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        if (info->required) {
            SDL_SetError("Could not load %s: %s", libname, dlerror());
        }
        return false;
    }
    const char *missing = NULL;
    module_handle[which] = h;
    if (!resolve_module(which, h, &missing)) {
        if (info->required) {
            SDL_SetError("%s is missing symbol %s", libname, missing);
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop x11: %s lacks %s, not using it", libname, missing);
        }
        unload_module(which);
        return false;
    }
    module_ok[which] = true;
    return true;
}

bool SDLOP_X11_LoadSymbols(void)
{
    if (x11_load_refcount > 0) {
        x11_load_refcount++;
        return true;
    }
    if (!load_module(SDLOP_X11_MOD_XCB)) {
        return false;
    }
    for (int m = 0; m < SDLOP_X11_MOD_COUNT; m++) {
        if (m != SDLOP_X11_MOD_XCB) {
            load_module((SDLOP_X11_Module)m);
        }
    }
    x11_load_refcount = 1;
    return true;
}

void SDLOP_X11_UnloadSymbols(void)
{
    if (x11_load_refcount == 0) {
        return;
    }
    if (--x11_load_refcount > 0) {
        return;
    }
    /* extensions first: they hold references into libxcb */
    for (int m = SDLOP_X11_MOD_COUNT - 1; m >= 0; m--) {
        unload_module((SDLOP_X11_Module)m);
    }
}

bool SDLOP_X11_HaveModule(SDLOP_X11_Module mod)
{
    return mod >= 0 && mod < SDLOP_X11_MOD_COUNT && module_ok[mod];
}
