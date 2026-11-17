/*
  SDLop - X11 OpenGL support through the shared runtime-loaded EGL API.

  EGL and libX11 are loaded dynamically. The native EGL window is the Xlib
  Window (XID); no link-time X11/EGL dependency is introduced.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"
#include "sdlop_egl_dyn.h"

#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#ifndef EGL_PLATFORM_X11_EXT
#define EGL_PLATFORM_X11_EXT 0x31D5
#endif
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

/* eglext.h versions predating EGL 1.5 may not define this typedef. */
typedef EGLDisplay (EGLAPIENTRYP SDLOP_PFNEGLGETPLATFORMDISPLAYEXTPROC)(
    EGLenum platform, void *native_display, const EGLint *attrib_list);

typedef struct X11GLContext
{
    EGLContext context;
    EGLConfig config;
    EGLint api;
} X11GLContext;

static EGLDisplay x11_egl_display = EGL_NO_DISPLAY;
static bool x11_egl_symbols_loaded;
static bool x11_egl_initialized;
static int x11_swap_interval;
static void *x11_current_context;
static SDL_Window *x11_current_window;

static bool x11_ensure_egl_display(void)
{
    if (x11_egl_initialized) {
        return true;
    }
    if (!x11_egl_symbols_loaded) {
        if (!SDLOP_EGL_LoadSymbols()) {
            return false;
        }
        x11_egl_symbols_loaded = true;
    }

    Display *display = SDLOP_X11_GetDisplay();
    if (!display) {
        return SDL_SetError("X11 display not available for EGL");
    }

    SDLOP_PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (SDLOP_PFNEGLGETPLATFORMDISPLAYEXTPROC)
        SDLOP_EGL_eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display) {
        x11_egl_display = get_platform_display(EGL_PLATFORM_X11_EXT,
                                                (void *)display, NULL);
    }
    if (x11_egl_display == EGL_NO_DISPLAY) {
        x11_egl_display = SDLOP_EGL_eglGetDisplay((EGLNativeDisplayType)display);
    }
    if (x11_egl_display == EGL_NO_DISPLAY) {
        return SDL_SetError("eglGetDisplay failed for X11");
    }

    EGLint major = 0, minor = 0;
    if (!SDLOP_EGL_eglInitialize(x11_egl_display, &major, &minor)) {
        EGLint error = SDLOP_EGL_eglGetError();
        x11_egl_display = EGL_NO_DISPLAY;
        return SDL_SetError("eglInitialize failed on X11: 0x%x", error);
    }
    x11_egl_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop EGL %d.%d on X11 (%s)",
                major, minor,
                SDLOP_EGL_eglQueryString(x11_egl_display, EGL_VERSION));
    return true;
}

static bool x11_pick_config(EGLConfig *out, EGLint api)
{
    EGLint renderable;
    if (api == EGL_OPENGL_API) {
        renderable = EGL_OPENGL_BIT;
    } else if (sdlop_glattrs.major_version >= 3) {
        renderable = EGL_OPENGL_ES3_BIT;
    } else {
        renderable = EGL_OPENGL_ES2_BIT;
    }

    EGLint attribs[32];
    int n = 0;
    attribs[n++] = EGL_SURFACE_TYPE;    attribs[n++] = EGL_WINDOW_BIT;
    attribs[n++] = EGL_RENDERABLE_TYPE; attribs[n++] = renderable;
    attribs[n++] = EGL_RED_SIZE;        attribs[n++] = sdlop_glattrs.red_size;
    attribs[n++] = EGL_GREEN_SIZE;      attribs[n++] = sdlop_glattrs.green_size;
    attribs[n++] = EGL_BLUE_SIZE;       attribs[n++] = sdlop_glattrs.blue_size;
    attribs[n++] = EGL_ALPHA_SIZE;      attribs[n++] = sdlop_glattrs.alpha_size;
    attribs[n++] = EGL_DEPTH_SIZE;      attribs[n++] = sdlop_glattrs.depth_size;
    attribs[n++] = EGL_STENCIL_SIZE;    attribs[n++] = sdlop_glattrs.stencil_size;
    if (sdlop_glattrs.multisamplesamples > 0) {
        attribs[n++] = EGL_SAMPLE_BUFFERS; attribs[n++] = 1;
        attribs[n++] = EGL_SAMPLES;        attribs[n++] = sdlop_glattrs.multisamplesamples;
    }
    attribs[n++] = EGL_NONE;

    EGLConfig configs[128];
    EGLint num_configs = 0;
    if (!SDLOP_EGL_eglChooseConfig(x11_egl_display, attribs, configs,
                                   (EGLint)SDL_arraysize(configs), &num_configs) ||
        num_configs == 0) {
        return SDL_SetError("eglChooseConfig found no matching X11 config");
    }

    /* The chosen EGLConfig must use the same X visual as the native Window.
     * EGL_NATIVE_VISUAL_ID is queried after eglChooseConfig instead of being
     * passed in its attribute list (it is not a portable choose filter). */
    Visual *visual = SDLOP_X11_GetVisual();
    if (visual) {
        for (EGLint i = 0; i < num_configs; ++i) {
            EGLint visual_id = 0;
            if (SDLOP_EGL_eglGetConfigAttrib(x11_egl_display, configs[i],
                                             EGL_NATIVE_VISUAL_ID, &visual_id) &&
                (VisualID)(unsigned int)visual_id == visual->visualid) {
                *out = configs[i];
                return true;
            }
        }
        return SDL_SetError("No EGL config matches the X11 window visual");
    }
    *out = configs[0];
    return true;
}

static EGLint x11_api_for_context(void)
{
    bool desktop_gl =
        (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_CORE) != 0 ||
        ((sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_ES) == 0 &&
         sdlop_glattrs.major_version >= 3);
    return desktop_gl ? EGL_OPENGL_API : EGL_OPENGL_ES_API;
}

static EGLConfig x11_config_for_context(X11GLContext *context)
{
    if (context->config) {
        return context->config;
    }
    EGLint config_id = 0;
    if (SDLOP_EGL_eglQueryContext(x11_egl_display, context->context,
                                  EGL_CONFIG_ID, &config_id)) {
        EGLint attribs[] = { EGL_CONFIG_ID, config_id, EGL_NONE };
        EGLConfig config = NULL;
        EGLint count = 0;
        if (SDLOP_EGL_eglChooseConfig(x11_egl_display, attribs, &config, 1,
                                      &count) && count == 1) {
            context->config = config;
            return config;
        }
    }
    EGLConfig config = NULL;
    if (x11_pick_config(&config, context->api)) {
        context->config = config;
        return config;
    }
    return NULL;
}

void *SDLOP_X11_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    if (!x11_ensure_egl_display()) {
        return NULL;
    }

    EGLint api = x11_api_for_context();
    if (!SDLOP_EGL_eglBindAPI((EGLenum)api)) {
        return (SDL_SetError("eglBindAPI failed: 0x%x",
                             SDLOP_EGL_eglGetError()), NULL);
    }
    EGLConfig config = NULL;
    if (!x11_pick_config(&config, api)) {
        return NULL;
    }

    EGLint ctx_attribs[16];
    int n = 0;
    if (api == EGL_OPENGL_API) {
        ctx_attribs[n++] = EGL_CONTEXT_MAJOR_VERSION_KHR;
        ctx_attribs[n++] = sdlop_glattrs.major_version;
        ctx_attribs[n++] = EGL_CONTEXT_MINOR_VERSION_KHR;
        ctx_attribs[n++] = sdlop_glattrs.minor_version;
        ctx_attribs[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
        ctx_attribs[n++] = (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_COMPATIBILITY)
                               ? EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR
                               : EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
        if (sdlop_glattrs.flags & SDL_GL_CONTEXT_DEBUG_FLAG) {
            ctx_attribs[n++] = EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
        }
        if (sdlop_glattrs.flags & SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG) {
            ctx_attribs[n++] = EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR;
        }
    } else {
        ctx_attribs[n++] = EGL_CONTEXT_CLIENT_VERSION;
        ctx_attribs[n++] = sdlop_glattrs.major_version >= 3 ? 3 : 2;
    }
    ctx_attribs[n++] = EGL_NONE;

    EGLContext share = EGL_NO_CONTEXT;
    if (sdlop_glattrs.share_with_current_context) {
        SDL_GLContext current = SDLOP_GL_CurrentContext();
        if (current) {
            share = ((X11GLContext *)current)->context;
        }
    }
    EGLContext context = SDLOP_EGL_eglCreateContext(x11_egl_display, config,
                                                    share, ctx_attribs);
    if (context == EGL_NO_CONTEXT) {
        return (SDL_SetError("eglCreateContext failed: 0x%x",
                             SDLOP_EGL_eglGetError()), NULL);
    }

    X11GLContext *result = (X11GLContext *)calloc(1, sizeof(*result));
    if (!result) {
        SDLOP_EGL_eglDestroyContext(x11_egl_display, context);
        SDL_OutOfMemory();
        return NULL;
    }
    result->context = context;
    result->config = config;
    result->api = api;
    return result;
}

bool SDLOP_X11_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window,
                              void *context_ptr)
{
    (void)device;
    if (!x11_egl_initialized && !x11_ensure_egl_display()) {
        return false;
    }

    if (!context_ptr) {
        if (!SDLOP_EGL_eglMakeCurrent(x11_egl_display, EGL_NO_SURFACE,
                                      EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
            return SDL_SetError("eglMakeCurrent(unbind) failed: 0x%x",
                                SDLOP_EGL_eglGetError());
        }
        SDLOP_X11_SetGLActive(x11_current_window, false);
        x11_current_context = NULL;
        x11_current_window = NULL;
        (void)window;
        return true;
    }

    X11GLContext *context = (X11GLContext *)context_ptr;
    if (!SDLOP_EGL_eglBindAPI((EGLenum)context->api)) {
        return SDL_SetError("eglBindAPI failed: 0x%x", SDLOP_EGL_eglGetError());
    }

    EGLSurface surface = EGL_NO_SURFACE;
    if (window) {
        void **slot = SDLOP_X11_GetGLSurfaceSlot(window);
        Window native = SDLOP_X11_GetWindowHandle(window);
        if (!slot || native == None) {
            return SDL_SetError("Invalid X11 window for GL");
        }
        if (!*slot) {
            EGLConfig config = x11_config_for_context(context);
            if (!config) {
                return SDL_SetError("No EGL config for X11 context");
            }
            EGLNativeWindowType native_window =
                (EGLNativeWindowType)(uintptr_t)native;
            EGLSurface created = SDLOP_EGL_eglCreateWindowSurface(
                x11_egl_display, config, native_window, NULL);
            if (created == EGL_NO_SURFACE) {
                return SDL_SetError("eglCreateWindowSurface failed: 0x%x",
                                    SDLOP_EGL_eglGetError());
            }
            *slot = (void *)created;
        }
        surface = (EGLSurface)*slot;
    }

    if (!SDLOP_EGL_eglMakeCurrent(x11_egl_display, surface, surface,
                                  context->context)) {
        return SDL_SetError("eglMakeCurrent failed: 0x%x",
                            SDLOP_EGL_eglGetError());
    }
    if (x11_current_window && x11_current_window != window) {
        SDLOP_X11_SetGLActive(x11_current_window, false);
    }
    SDLOP_X11_SetGLActive(window, window != NULL);
    x11_current_context = context_ptr;
    x11_current_window = window;
    (void)SDLOP_EGL_eglSwapInterval(x11_egl_display, x11_swap_interval);
    return true;
}

bool SDLOP_X11_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    void **slot = SDLOP_X11_GetGLSurfaceSlot(window);
    if (!slot || !*slot) {
        return SDL_SetError("Window has no X11 EGL surface");
    }
    if (!SDLOP_EGL_eglSwapBuffers(x11_egl_display, (EGLSurface)*slot)) {
        EGLint error = SDLOP_EGL_eglGetError();
        if (error == EGL_BAD_SURFACE || error == EGL_BAD_NATIVE_WINDOW) {
            SDLOP_X11_SetGLActive(window, false);
            *slot = NULL;
        }
        return SDL_SetError("eglSwapBuffers failed: 0x%x", error);
    }
    return true;
}

void SDLOP_X11_GL_DeleteContext(SDLop_VideoDevice *device, void *context_ptr)
{
    (void)device;
    X11GLContext *context = (X11GLContext *)context_ptr;
    if (!context) {
        return;
    }
    if (x11_egl_initialized) {
        if (x11_current_context == context_ptr) {
            (void)SDLOP_EGL_eglMakeCurrent(x11_egl_display, EGL_NO_SURFACE,
                                           EGL_NO_SURFACE, EGL_NO_CONTEXT);
            SDLOP_X11_SetGLActive(x11_current_window, false);
            x11_current_context = NULL;
            x11_current_window = NULL;
        }
        (void)SDLOP_EGL_eglDestroyContext(x11_egl_display, context->context);
    }
    free(context);
}

void SDLOP_X11_GL_WindowDestroyed(SDL_Window *window)
{
    if (x11_egl_initialized && x11_current_window == window) {
        (void)SDLOP_EGL_eglMakeCurrent(x11_egl_display, EGL_NO_SURFACE,
                                       EGL_NO_SURFACE, EGL_NO_CONTEXT);
        x11_current_context = NULL;
        x11_current_window = NULL;
    }
    void **slot = SDLOP_X11_GetGLSurfaceSlot(window);
    if (slot && *slot) {
        if (x11_egl_initialized) {
            (void)SDLOP_EGL_eglDestroySurface(x11_egl_display,
                                              (EGLSurface)*slot);
        }
        *slot = NULL;
    }
    SDLOP_X11_SetGLActive(window, false);
}

SDL_FunctionPointer SDLOP_X11_GL_GetProcAddress(SDLop_VideoDevice *device,
                                                 const char *proc)
{
    (void)device;
    if (!proc || !x11_ensure_egl_display()) {
        return NULL;
    }
    SDL_FunctionPointer fn = SDLOP_EGL_eglGetProcAddress(proc);
    if (fn) {
        return fn;
    }
    return (SDL_FunctionPointer)dlsym(RTLD_DEFAULT, proc);
}

bool SDLOP_X11_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval)
{
    (void)device;
    if (interval < 0) {
        return SDL_SetError("Adaptive vsync (-1) is not supported");
    }
    x11_swap_interval = interval;
    if (x11_egl_initialized &&
        !SDLOP_EGL_eglSwapInterval(x11_egl_display, interval)) {
        return SDL_SetError("eglSwapInterval failed: 0x%x",
                            SDLOP_EGL_eglGetError());
    }
    return true;
}

bool SDLOP_X11_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval)
{
    (void)device;
    if (!interval) {
        return SDL_SetError("NULL swap interval pointer");
    }
    *interval = x11_swap_interval;
    return true;
}

void SDLOP_X11_GL_Quit(void)
{
    if (x11_egl_initialized) {
        (void)SDLOP_EGL_eglMakeCurrent(x11_egl_display, EGL_NO_SURFACE,
                                       EGL_NO_SURFACE, EGL_NO_CONTEXT);
        (void)SDLOP_EGL_eglTerminate(x11_egl_display);
    }
    SDLOP_X11_SetGLActive(x11_current_window, false);
    x11_current_context = NULL;
    x11_current_window = NULL;
    x11_egl_display = EGL_NO_DISPLAY;
    x11_egl_initialized = false;
    x11_swap_interval = 0;
    bool unload_symbols = x11_egl_symbols_loaded;
    x11_egl_symbols_loaded = false;
    if (unload_symbols) {
        SDLOP_EGL_UnloadSymbols();
    }
}
