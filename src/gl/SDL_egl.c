/*
  SDLop -- the GL driver (EGL).

  One implementation covers Wayland and X11: EGL selects its platform at
  eglGetPlatformDisplay() time, so the only backend-specific code is the window
  object handed to eglCreateWindowSurface() (a wl_egl_window on Wayland, an
  X11 Window ID on X11).

  Notes on staying small:
    * the GL library is opened with dlopen() on first use and closed on
      SDL_GL_UnloadLibrary(), so a program that never asks for GL never loads a
      GL driver (SDL does this too, but through a much larger per-platform
      loader);
    * attributes are validated up front, so an unsupported request fails at
      SDL_GL_SetAttribute() time instead of producing a confusing context
      creation failure later.
*/

#include "../sdlop_internal.h"

#include <string.h>
#include <dlfcn.h>
/* Without X11 in the build, eglplatform.h must not pull in Xlib: EGL_NO_X11
   makes the native types plain integers, which is all this file needs. */
#ifndef SDLOP_HAVE_X11
#define EGL_NO_X11
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-egl.h>

/* Every EGL entry point is resolved with dlsym() from the library opened on
   demand, so nothing in the build links against libEGL: an application that
   never asks for GL runs on a machine that has no EGL at all. This is what SDL
   does with its own loader; the trick that keeps this file readable is that the
   EGL names below become pointers, so the rest of the code looks unchanged. */
#define SDLOP_EGL_FUNCS     X(eglBindAPI) X(eglChooseConfig) X(eglCreateContext) X(eglCreateWindowSurface)     X(eglDestroyContext) X(eglDestroySurface) X(eglGetConfigAttrib) X(eglGetDisplay)     X(eglGetProcAddress) X(eglInitialize) X(eglMakeCurrent) X(eglQueryString)     X(eglSwapBuffers) X(eglSwapInterval) X(eglTerminate)

#define X(name) static __typeof__(&name) sdlop_##name;
SDLOP_EGL_FUNCS
#undef X

#define X(name) #name,
static const char *const sdlop_egl_names[] = { SDLOP_EGL_FUNCS };
#undef X

#define eglBindAPI            (*sdlop_eglBindAPI)
#define eglChooseConfig       (*sdlop_eglChooseConfig)
#define eglCreateContext      (*sdlop_eglCreateContext)
#define eglCreateWindowSurface (*sdlop_eglCreateWindowSurface)
#define eglDestroyContext     (*sdlop_eglDestroyContext)
#define eglDestroySurface     (*sdlop_eglDestroySurface)
#define eglGetConfigAttrib    (*sdlop_eglGetConfigAttrib)
#define eglGetDisplay         (*sdlop_eglGetDisplay)
#define eglGetProcAddress     (*sdlop_eglGetProcAddress)
#define eglInitialize         (*sdlop_eglInitialize)
#define eglMakeCurrent        (*sdlop_eglMakeCurrent)
#define eglQueryString        (*sdlop_eglQueryString)
#define eglSwapBuffers        (*sdlop_eglSwapBuffers)
#define eglSwapInterval       (*sdlop_eglSwapInterval)
#define eglTerminate          (*sdlop_eglTerminate)

typedef struct SDLOP_EGL
{
    void *lib;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;
    SDL_Window *window;
    int swap_interval;
    bool initialized;
} SDLOP_EGL;

static SDLOP_EGL sdlop_egl;

/* attributes requested through SDL_GL_SetAttribute() */
static int sdlop_gl_attr[SDL_GL_EGL_PLATFORM + 1];
static bool sdlop_gl_attr_set[SDL_GL_EGL_PLATFORM + 1];

static PFNEGLGETPLATFORMDISPLAYEXTPROC p_eglGetPlatformDisplayEXT;

/* Resolve the entry points from the handle just opened. A missing core function
   means the library is not an EGL implementation, so the whole load fails. */
static bool sdlop_egl_resolve(void *lib)
{
#define X(name)     sdlop_##name = (__typeof__(sdlop_##name))dlsym(lib, sdlop_egl_names[idx++]);
    size_t idx = 0;
    SDLOP_EGL_FUNCS
#undef X
    if (!sdlop_eglInitialize || !sdlop_eglGetDisplay || !sdlop_eglMakeCurrent) {
        return SDL_SetError("The GL library is not an EGL implementation");
    }
    return true;
}

static bool sdlop_egl_open_library(const char *path)
{
    const char *names[4];
    int num_names = 0;

    if (sdlop_egl.lib) {
        return true;
    }
    if (path) {
        names[num_names++] = path;
    }
    names[num_names++] = "libEGL.so.1";
    names[num_names++] = "libEGL.so";
    for (int i = 0; i < num_names; i++) {
        void *lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (lib && sdlop_egl_resolve(lib)) {
            sdlop_egl.lib = lib;
            return true;
        }
        if (lib) {
            dlclose(lib);
        }
    }
    return SDL_SetError("Couldn't load the EGL library");
}

static void sdlop_egl_close_library(void)
{
    if (!sdlop_egl.lib) {
        sdlop_egl.initialized = false;
        return;
    }
    if (sdlop_egl.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(sdlop_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (sdlop_egl.surface != EGL_NO_SURFACE) {
            eglDestroySurface(sdlop_egl.display, sdlop_egl.surface);
            sdlop_egl.surface = EGL_NO_SURFACE;
        }
        if (sdlop_egl.context != EGL_NO_CONTEXT) {
            eglDestroyContext(sdlop_egl.display, sdlop_egl.context);
            sdlop_egl.context = EGL_NO_CONTEXT;
        }
        eglTerminate(sdlop_egl.display);
        sdlop_egl.display = EGL_NO_DISPLAY;
    }
    if (sdlop_egl.lib) {
        dlclose(sdlop_egl.lib);
        sdlop_egl.lib = NULL;
    }
    sdlop_egl.initialized = false;
}

static bool sdlop_egl_load_library(const char *path)
{
    return sdlop_egl_open_library(path);
}

static SDL_FunctionPointer sdlop_egl_get_proc_address(const char *proc)
{
    if (sdlop_egl.display != EGL_NO_DISPLAY) {
        SDL_FunctionPointer address = (SDL_FunctionPointer)eglGetProcAddress(proc);
        if (address) {
            return address;
        }
    }
    return (SDL_FunctionPointer)dlsym(sdlop_egl.lib ? sdlop_egl.lib : RTLD_DEFAULT, proc);
}

static void sdlop_egl_unload_library(void)
{
    sdlop_egl_close_library();
}

static bool sdlop_egl_extension_supported(const char *extension)
{
    const char *extensions;

    if (!extension) {
        return false;
    }
    extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!extensions) {
        return false;
    }
    /* naive but sufficient: SDL does the same for GL extension strings */
    {
        size_t len = SDL_strlen(extension);
        const char *p = extensions;
        while ((p = SDL_strstr(p, extension)) != NULL) {
            const char *end = p + len;
            bool ok_left = (p == extensions || p[-1] == ' ');
            bool ok_right = (*end == ' ' || *end == '\0');
            if (ok_left && ok_right) {
                return true;
            }
            p = end;
        }
    }
    return false;
}

static bool sdlop_egl_set_attribute(SDL_GLAttr attr, int value)
{
    if (attr < 0 || attr > SDL_GL_EGL_PLATFORM) {
        return SDL_InvalidParamError("attr");
    }
    sdlop_gl_attr[attr] = value;
    sdlop_gl_attr_set[attr] = true;
    return true;
}

static bool sdlop_egl_get_attribute(SDL_GLAttr attr, int *value)
{
    if (attr < 0 || attr > SDL_GL_EGL_PLATFORM) {
        return SDL_InvalidParamError("attr");
    }
    *value = sdlop_gl_attr_set[attr] ? sdlop_gl_attr[attr] : 0;
    return true;
}

static void sdlop_egl_reset_attributes(void)
{
    memset(sdlop_gl_attr, 0, sizeof(sdlop_gl_attr));
    memset(sdlop_gl_attr_set, 0, sizeof(sdlop_gl_attr_set));
}

static int sdlop_egl_attr(SDL_GLAttr attr, int fallback)
{
    return sdlop_gl_attr_set[attr] ? sdlop_gl_attr[attr] : fallback;
}

static bool sdlop_egl_init_display(void)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();
    EGLint major = 0, minor = 0;

    if (sdlop_egl.initialized) {
        return true;
    }
    if (!sdlop_egl_open_library(NULL)) {
        return false;
    }

    p_eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (driver && SDL_strcmp(driver->name, "wayland") == 0 && p_eglGetPlatformDisplayEXT) {
        /* The display of the Wayland connection this process already has: no
           second connection, and no guess at which one to use. */
        sdlop_egl.display = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT,
                                                       sdlop_wl_display_handle(), NULL);
    }
#ifdef SDLOP_HAVE_X11
    if (driver && SDL_strcmp(driver->name, "x11") == 0 && p_eglGetPlatformDisplayEXT) {
        /* Same idea on X11: EGL_PLATFORM_X11_EXT pins the platform, so
           eglGetDisplay() never has to guess between X11 and a headless device. */
        sdlop_egl.display = p_eglGetPlatformDisplayEXT(
            EGL_PLATFORM_X11_EXT, (void *)(uintptr_t)sdlop_x11_display_handle(), NULL);
    }
#endif
    if (sdlop_egl.display == EGL_NO_DISPLAY || sdlop_egl.display == NULL) {
        sdlop_egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (sdlop_egl.display == EGL_NO_DISPLAY) {
        SDL_SetError("eglGetDisplay() failed");
        return false;
    }
    if (!eglInitialize(sdlop_egl.display, &major, &minor)) {
        SDL_SetError("eglInitialize() failed");
        return false;
    }
    if (sdlop_egl_attr(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES) == SDL_GL_CONTEXT_PROFILE_ES) {
        eglBindAPI(EGL_OPENGL_ES_API);
    } else {
        eglBindAPI(EGL_OPENGL_API);
    }
    sdlop_egl.initialized = true;
    sdlop_egl.swap_interval = -1;
    return true;
}

static bool sdlop_egl_choose_config(void)
{
    EGLint attributes[40];
    int i = 0;
    EGLint num_configs = 0;

    attributes[i++] = EGL_SURFACE_TYPE;   attributes[i++] = EGL_WINDOW_BIT;
    attributes[i++] = EGL_RENDERABLE_TYPE;
    attributes[i++] = sdlop_egl_attr(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES) ==
                              SDL_GL_CONTEXT_PROFILE_ES
                          ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_BIT;
    attributes[i++] = EGL_RED_SIZE;       attributes[i++] = sdlop_egl_attr(SDL_GL_RED_SIZE, 8);
    attributes[i++] = EGL_GREEN_SIZE;     attributes[i++] = sdlop_egl_attr(SDL_GL_GREEN_SIZE, 8);
    attributes[i++] = EGL_BLUE_SIZE;      attributes[i++] = sdlop_egl_attr(SDL_GL_BLUE_SIZE, 8);
    attributes[i++] = EGL_ALPHA_SIZE;     attributes[i++] = sdlop_egl_attr(SDL_GL_ALPHA_SIZE, 0);
    attributes[i++] = EGL_DEPTH_SIZE;     attributes[i++] = sdlop_egl_attr(SDL_GL_DEPTH_SIZE, 0);
    attributes[i++] = EGL_STENCIL_SIZE;   attributes[i++] = sdlop_egl_attr(SDL_GL_STENCIL_SIZE, 0);
    if (sdlop_gl_attr_set[SDL_GL_MULTISAMPLEBUFFERS] && sdlop_gl_attr[SDL_GL_MULTISAMPLEBUFFERS]) {
        attributes[i++] = EGL_SAMPLE_BUFFERS; attributes[i++] = 1;
        attributes[i++] = EGL_SAMPLES;        attributes[i++] = sdlop_egl_attr(SDL_GL_MULTISAMPLESAMPLES, 4);
    }
    attributes[i++] = EGL_NONE;

    if (!eglChooseConfig(sdlop_egl.display, attributes, &sdlop_egl.config, 1, &num_configs) || num_configs < 1) {
        return SDL_SetError("No matching EGL config for the requested attributes");
    }
    return true;
}

static SDL_GLContext sdlop_egl_create_context(SDL_Window *window, SDL_GLContext shared)
{
    EGLint context_attributes[10];
    int i = 0;
    EGLContext context;
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();
    EGLNativeWindowType native_window = 0;

    if (!window) {
        SDL_InvalidParamError("window");
        return NULL;
    }
    if (!sdlop_egl_init_display() || !sdlop_egl_choose_config()) {
        return NULL;
    }

    if (driver && SDL_strcmp(driver->name, "wayland") == 0) {
        if (!window->driver.wayland.egl_window) {
            window->driver.wayland.egl_window = (struct wl_egl_window *)sdlop_wl_egl_window_create(window);
            if (!window->driver.wayland.egl_window) {
                SDL_SetError("Couldn't create a wl_egl_window");
                return NULL;
            }
        }
        native_window = (EGLNativeWindowType)window->driver.wayland.egl_window;
    }
#ifdef SDLOP_HAVE_X11
    else if (driver && SDL_strcmp(driver->name, "x11") == 0) {
        /* On X11 the EGL native window is the X window itself. It has to have
           been created with the visual of the chosen EGL config, which is what
           SDLOP_X11_GLVisual() is for. */
        native_window = (EGLNativeWindowType)sdlop_x11_window_handle(window);
        if (!native_window) {
            SDL_SetError("The X11 window has not been created yet");
            return NULL;
        }
    }
#endif
    else {
        SDL_SetError("The %s driver cannot create GL contexts", driver ? driver->name : "current");
        return NULL;
    }

    sdlop_egl.surface = eglCreateWindowSurface(sdlop_egl.display, sdlop_egl.config, native_window, NULL);
    if (sdlop_egl.surface == EGL_NO_SURFACE) {
        SDL_SetError("eglCreateWindowSurface() failed");
        return NULL;
    }

    if (sdlop_egl_attr(SDL_GL_CONTEXT_PROFILE_MASK, 0) == SDL_GL_CONTEXT_PROFILE_ES) {
        context_attributes[i++] = EGL_CONTEXT_MAJOR_VERSION;
        context_attributes[i++] = sdlop_egl_attr(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    } else if (sdlop_gl_attr_set[SDL_GL_CONTEXT_MAJOR_VERSION]) {
        context_attributes[i++] = EGL_CONTEXT_MAJOR_VERSION;
        context_attributes[i++] = sdlop_gl_attr[SDL_GL_CONTEXT_MAJOR_VERSION];
    }
    if (sdlop_gl_attr_set[SDL_GL_CONTEXT_MINOR_VERSION]) {
        context_attributes[i++] = EGL_CONTEXT_MINOR_VERSION;
        context_attributes[i++] = sdlop_gl_attr[SDL_GL_CONTEXT_MINOR_VERSION];
    }
    if (sdlop_gl_attr_set[SDL_GL_CONTEXT_FLAGS]) {
        EGLint flags = 0;
        if (sdlop_gl_attr[SDL_GL_CONTEXT_FLAGS] & SDL_GL_CONTEXT_DEBUG_FLAG) {
            flags |= EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
        }
        if (sdlop_gl_attr[SDL_GL_CONTEXT_FLAGS] & SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG) {
            flags |= EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR;
        }
        if (sdlop_gl_attr[SDL_GL_CONTEXT_FLAGS] & SDL_GL_CONTEXT_ROBUST_ACCESS_FLAG) {
            flags |= EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR;
        }
        context_attributes[i++] = EGL_CONTEXT_FLAGS_KHR;
        context_attributes[i++] = flags;
    }
    context_attributes[i++] = EGL_NONE;

    context = eglCreateContext(sdlop_egl.display, sdlop_egl.config, (EGLContext)shared, context_attributes);
    if (context == EGL_NO_CONTEXT) {
        SDL_SetError("eglCreateContext() failed");
        return NULL;
    }
    sdlop_egl.context = context;
    if (!eglMakeCurrent(sdlop_egl.display, sdlop_egl.surface, sdlop_egl.surface, context)) {
        eglDestroyContext(sdlop_egl.display, context);
        sdlop_egl.context = EGL_NO_CONTEXT;
        SDL_SetError("eglMakeCurrent() failed");
        return NULL;
    }
    sdlop_egl.window = window;
    return (SDL_GLContext)context;
}

static bool sdlop_egl_make_current(SDL_Window *window, SDL_GLContext context)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();

    if (!sdlop_egl.initialized) {
        return SDL_SetError("No EGL display");
    }
    if (!window || !context) {
        if (!eglMakeCurrent(sdlop_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
            return SDL_SetError("eglMakeCurrent() failed");
        }
        sdlop_egl.window = NULL;
        return true;
    }

    if (driver && SDL_strcmp(driver->name, "wayland") == 0 && window->driver.wayland.egl_window) {
        /* the window may have been resized */
        wl_egl_window_resize(window->driver.wayland.egl_window, window->pixel_w,
                             window->pixel_h, 0, 0);
    }
    if (sdlop_egl.surface == EGL_NO_SURFACE ||
        (!(window->flags & SDL_WINDOW_OPENGL) && sdlop_egl.window != window)) {
        EGLNativeWindowType native_window = 0;
        if (driver && SDL_strcmp(driver->name, "wayland") == 0) {
            native_window = (EGLNativeWindowType)window->driver.wayland.egl_window;
        }
#ifdef SDLOP_HAVE_X11
        else if (driver && SDL_strcmp(driver->name, "x11") == 0) {
            native_window = (EGLNativeWindowType)sdlop_x11_window_handle(window);
        }
#endif
        if (sdlop_egl.surface != EGL_NO_SURFACE) {
            eglDestroySurface(sdlop_egl.display, sdlop_egl.surface);
        }
        sdlop_egl.surface = eglCreateWindowSurface(sdlop_egl.display, sdlop_egl.config, native_window, NULL);
        if (sdlop_egl.surface == EGL_NO_SURFACE) {
            return SDL_SetError("eglCreateWindowSurface() failed");
        }
    }
    if (!eglMakeCurrent(sdlop_egl.display, sdlop_egl.surface, sdlop_egl.surface, (EGLContext)context)) {
        return SDL_SetError("eglMakeCurrent() failed");
    }
    sdlop_egl.context = (EGLContext)context;
    sdlop_egl.window = window;
    if (sdlop_egl.swap_interval >= 0) {
        eglSwapInterval(sdlop_egl.display, sdlop_egl.swap_interval);
    }
    return true;
}

static bool sdlop_egl_set_swap_interval(int interval)
{
    sdlop_egl.swap_interval = interval;
    if (sdlop_egl.display != EGL_NO_DISPLAY && sdlop_egl.context != EGL_NO_CONTEXT) {
        return eglSwapInterval(sdlop_egl.display, interval);
    }
    return true;
}

static int sdlop_egl_get_swap_interval(void)
{
    return sdlop_egl.swap_interval;
}

static bool sdlop_egl_swap_window(SDL_Window *window)
{
    if (!sdlop_egl.initialized || window != sdlop_egl.window) {
        return SDL_SetError("No EGL surface for this window");
    }
    return eglSwapBuffers(sdlop_egl.display, sdlop_egl.surface) ? true : SDL_SetError("eglSwapBuffers() failed");
}

static void sdlop_egl_destroy_context(SDL_GLContext context)
{
    if (!sdlop_egl.initialized || !context) {
        return;
    }
    if (sdlop_egl.context == (EGLContext)context) {
        eglMakeCurrent(sdlop_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        sdlop_egl.context = EGL_NO_CONTEXT;
    }
    eglDestroyContext(sdlop_egl.display, (EGLContext)context);
}

#ifdef SDLOP_HAVE_X11
/* The X11 visual an SDL_WINDOW_OPENGL window has to be created with, so that the
   X window matches the EGL config eglCreateWindowSurface() will be given. This is
   SDL3's "get the visual before creating the window" step, done through EGL
   instead of GLX. */
bool sdlop_egl_x11_visual(unsigned long *visual_id, int *depth)
{
    EGLint native_visual = 0;

    if (visual_id) {
        *visual_id = 0;
    }
    if (depth) {
        *depth = 0;
    }
    if (!sdlop_egl_init_display() || !sdlop_egl_choose_config()) {
        return false;
    }
    /* EGL reports the X visual the config maps to; the X server owns the depth. */
    if (!eglGetConfigAttrib(sdlop_egl.display, sdlop_egl.config, EGL_NATIVE_VISUAL_ID,
                            &native_visual) || !native_visual) {
        return false;
    }
    if (visual_id) {
        *visual_id = (unsigned long)native_visual;
    }
    return true;
}
#endif /* SDLOP_HAVE_X11 */

const SDLOP_GLDriver SDLOP_EGLDriver = {
    .name = "egl",
    .load_library = sdlop_egl_load_library,
    .get_proc_address = sdlop_egl_get_proc_address,
    .unload_library = sdlop_egl_unload_library,
    .extension_supported = sdlop_egl_extension_supported,
    .set_attribute = sdlop_egl_set_attribute,
    .get_attribute = sdlop_egl_get_attribute,
    .reset_attributes = sdlop_egl_reset_attributes,
    .create_context = sdlop_egl_create_context,
    .make_current = sdlop_egl_make_current,
    .set_swap_interval = sdlop_egl_set_swap_interval,
    .get_swap_interval = sdlop_egl_get_swap_interval,
    .swap_window = sdlop_egl_swap_window,
    .destroy_context = sdlop_egl_destroy_context,
};
