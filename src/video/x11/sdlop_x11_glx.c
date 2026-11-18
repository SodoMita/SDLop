/*
  SDLop - X11 OpenGL support via GLX.

  libGL.so.1 is dlopen'd on first use (SDLOP_LIB_GL overrides the soname);
  contexts are legacy glXCreateContext or GLX_ARB_create_context when the
  requested profile/version needs it. On a GPU-less X server (e.g. Xvfb)
  this transparently uses indirect GLX with the server's software renderer.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"

#include <dlfcn.h>
#include <string.h>

/* GLX constants (frozen ABI, <GL/glx.h>) */
#define SDLOP_GLX_USE_GL        1
#define SDLOP_GLX_BUFFER_SIZE   2
#define SDLOP_GLX_RGBA          4
#define SDLOP_GLX_DOUBLEBUFFER  5
#define SDLOP_GLX_STEREO        6
#define SDLOP_GLX_RED_SIZE      8
#define SDLOP_GLX_GREEN_SIZE    9
#define SDLOP_GLX_BLUE_SIZE     10
#define SDLOP_GLX_ALPHA_SIZE    11
#define SDLOP_GLX_DEPTH_SIZE    12
#define SDLOP_GLX_STENCIL_SIZE  13
#define SDLOP_GLX_SAMPLE_BUFFERS 100000
#define SDLOP_GLX_SAMPLES        100001

#define SDLOP_GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define SDLOP_GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define SDLOP_GLX_CONTEXT_FLAGS_ARB         0x2094
#define SDLOP_GLX_CONTEXT_PROFILE_MASK_ARB  0x9126
#define SDLOP_GLX_CONTEXT_DEBUG_BIT_ARB              0x0001
#define SDLOP_GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x0002
#define SDLOP_GLX_CONTEXT_CORE_PROFILE_BIT_ARB          0x00000001
#define SDLOP_GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002

typedef void *SDLOP_GLXContext;
typedef void (*SDLOP_GLXProc)(void);

/* libGL entry points (dlopen'd) */
typedef SDLOP_XVisualInfo *(*PFN_glXChooseVisual)(SDLOP_Display *, int, int *);
typedef SDLOP_GLXContext (*PFN_glXCreateContext)(SDLOP_Display *, SDLOP_XVisualInfo *, SDLOP_GLXContext, SDLOP_Bool);
typedef void (*PFN_glXDestroyContext)(SDLOP_Display *, SDLOP_GLXContext);
typedef SDLOP_Bool (*PFN_glXMakeCurrent)(SDLOP_Display *, unsigned long, SDLOP_GLXContext);
typedef void (*PFN_glXSwapBuffers)(SDLOP_Display *, unsigned long);
typedef SDLOP_GLXProc (*PFN_glXGetProcAddressARB)(const unsigned char *);
typedef SDLOP_Bool (*PFN_glXQueryExtension)(SDLOP_Display *, int *, int *);
typedef SDLOP_Bool (*PFN_glXQueryVersion)(SDLOP_Display *, int *, int *);
typedef const char *(*PFN_glXQueryExtensionsString)(SDLOP_Display *, int);

static void *glx_lib;
static PFN_glXChooseVisual p_glXChooseVisual;
static PFN_glXCreateContext p_glXCreateContext;
static PFN_glXDestroyContext p_glXDestroyContext;
static PFN_glXMakeCurrent p_glXMakeCurrent;
static PFN_glXSwapBuffers p_glXSwapBuffers;
static PFN_glXGetProcAddressARB p_glXGetProcAddressARB;
static PFN_glXQueryExtension p_glXQueryExtension;
static PFN_glXQueryVersion p_glXQueryVersion;
static PFN_glXQueryExtensionsString p_glXQueryExtensionsString;

/* extension entry points (via glXGetProcAddressARB) */
typedef SDLOP_GLXContext (*PFN_glXCreateContextAttribsARB)(SDLOP_Display *, void *, SDLOP_GLXContext, SDLOP_Bool, const int *);
typedef int (*PFN_glXSwapIntervalMESA)(unsigned int);
typedef void (*PFN_glXSwapIntervalEXT)(SDLOP_Display *, unsigned long, int);
typedef int (*PFN_glXSwapIntervalSGI)(int);
static PFN_glXCreateContextAttribsARB p_glXCreateContextAttribsARB;
static PFN_glXSwapIntervalMESA p_glXSwapIntervalMESA;
static PFN_glXSwapIntervalEXT p_glXSwapIntervalEXT;
static PFN_glXSwapIntervalSGI p_glXSwapIntervalSGI;

typedef struct X11GLContext
{
    SDLOP_GLXContext ctx;
} X11GLContext;

static bool glx_available; /* server advertised the GLX extension */
static int swap_interval = 0;

static SDLOP_GLXProc glx_get_proc(const char *name);

static bool ensure_glx(SDLop_VideoDevice *device)
{
    if (glx_lib) {
        return glx_available;
    }
    const char *libname = getenv("SDLOP_LIB_GL");
    if (!libname || !libname[0]) {
        libname = "libGL.so.1";
    }
    glx_lib = dlopen(libname, RTLD_NOW | RTLD_LOCAL);
    if (!glx_lib) {
        glx_lib = dlopen("libGL.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!glx_lib) {
        return SDL_SetError("Could not load %s for GLX", libname);
    }
#define SDLOP_GLX_LOAD(field, sym)                                              \
    p_##field = (PFN_##field)(uintptr_t)dlsym(glx_lib, #sym);                   \
    if (!p_##field) {                                                           \
        dlclose(glx_lib);                                                       \
        glx_lib = NULL;                                                         \
        return SDL_SetError("libGL is missing %s", #sym);                       \
    }
    SDLOP_GLX_LOAD(glXChooseVisual, glXChooseVisual)
    SDLOP_GLX_LOAD(glXCreateContext, glXCreateContext)
    SDLOP_GLX_LOAD(glXDestroyContext, glXDestroyContext)
    SDLOP_GLX_LOAD(glXMakeCurrent, glXMakeCurrent)
    SDLOP_GLX_LOAD(glXSwapBuffers, glXSwapBuffers)
    SDLOP_GLX_LOAD(glXGetProcAddressARB, glXGetProcAddressARB)
    SDLOP_GLX_LOAD(glXQueryExtension, glXQueryExtension)
    SDLOP_GLX_LOAD(glXQueryVersion, glXQueryVersion)
    SDLOP_GLX_LOAD(glXQueryExtensionsString, glXQueryExtensionsString)
#undef SDLOP_GLX_LOAD

    SDLOP_Display *dpy = SDLOP_X11_GetDisplay(device);
    int error_base = 0, event_base = 0;
    if (!dpy || !p_glXQueryExtension(dpy, &error_base, &event_base)) {
        /* GLX is an X extension: absent on minimal servers. Not fatal -
         * the driver keeps working without GL. */
        glx_available = false;
        return SDL_SetError("X server has no GLX extension");
    }
    glx_available = true;

    p_glXCreateContextAttribsARB =
        (PFN_glXCreateContextAttribsARB)glx_get_proc("glXCreateContextAttribsARB");
    p_glXSwapIntervalMESA = (PFN_glXSwapIntervalMESA)glx_get_proc("glXSwapIntervalMESA");
    p_glXSwapIntervalEXT = (PFN_glXSwapIntervalEXT)glx_get_proc("glXSwapIntervalEXT");
    p_glXSwapIntervalSGI = (PFN_glXSwapIntervalSGI)glx_get_proc("glXSwapIntervalSGI");

    int major = 0, minor = 0;
    if (p_glXQueryVersion(dpy, &major, &minor)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop GLX %d.%d on X11 (%s)",
                    major, minor, p_glXQueryExtensionsString(dpy, 0) ? "exts ok" : "no exts");
    }
    return true;
}

static SDLOP_GLXProc glx_get_proc(const char *name)
{
    if (p_glXGetProcAddressARB) {
        SDLOP_GLXProc fn = (SDLOP_GLXProc)p_glXGetProcAddressARB((const unsigned char *)name);
        if (fn) {
            return fn;
        }
    }
    return (SDLOP_GLXProc)(uintptr_t)dlsym(glx_lib ? glx_lib : RTLD_DEFAULT, name);
}

/* visual stash for the window currently being created (window creation is
 * single-threaded, like SDL3) */
static SDLOP_XVisualInfo pending_vi;
static bool pending_vi_valid;
static SDLOP_Colormap pending_colormap;

/* build the glXChooseVisual attribute list from the SDL GL attributes */
static int glx_visual_attribs(int *out, int capacity)
{
    int n = 0;
    out[n++] = SDLOP_GLX_RGBA;
    if (sdlop_glattrs.doublebuffer) {
        out[n++] = SDLOP_GLX_DOUBLEBUFFER;
    }
    out[n++] = SDLOP_GLX_RED_SIZE;   out[n++] = sdlop_glattrs.red_size;
    out[n++] = SDLOP_GLX_GREEN_SIZE; out[n++] = sdlop_glattrs.green_size;
    out[n++] = SDLOP_GLX_BLUE_SIZE;  out[n++] = sdlop_glattrs.blue_size;
    if (sdlop_glattrs.alpha_size > 0) {
        out[n++] = SDLOP_GLX_ALPHA_SIZE; out[n++] = sdlop_glattrs.alpha_size;
    }
    if (sdlop_glattrs.depth_size > 0) {
        out[n++] = SDLOP_GLX_DEPTH_SIZE; out[n++] = sdlop_glattrs.depth_size;
    }
    if (sdlop_glattrs.stencil_size > 0) {
        out[n++] = SDLOP_GLX_STENCIL_SIZE; out[n++] = sdlop_glattrs.stencil_size;
    }
    if (sdlop_glattrs.multisamplesamples > 0) {
        out[n++] = SDLOP_GLX_SAMPLE_BUFFERS; out[n++] = sdlop_glattrs.multisamplebuffers > 0 ? sdlop_glattrs.multisamplebuffers : 1;
        out[n++] = SDLOP_GLX_SAMPLES;        out[n++] = sdlop_glattrs.multisamplesamples;
    }
    out[n++] = 0; /* None */
    (void)capacity;
    return n;
}

/* called from X11_CreateWindow for SDL_WINDOW_OPENGL windows: pick the GLX
 * visual the window must use so a later SDL_GL_CreateContext matches it */
bool SDLOP_X11_GL_ChooseVisual(SDL_Window *window, SDLOP_Visual **visual_out, int *depth_out, SDLOP_Colormap *colormap_out)
{
    SDLop_VideoDevice *device = sdlop.video;
    if (!ensure_glx(device)) {
        return false;
    }
    SDLOP_Display *dpy = SDLOP_X11_GetDisplay(device);
    int attribs[32];
    glx_visual_attribs(attribs, 32);
    SDLOP_XVisualInfo *vi = p_glXChooseVisual(dpy, 0, attribs);
    if (!vi) {
        /* retry with a minimal list (some stacks reject MSAA/alpha) */
        int minimal[] = { SDLOP_GLX_RGBA, SDLOP_GLX_DOUBLEBUFFER,
                          SDLOP_GLX_RED_SIZE, 1, SDLOP_GLX_GREEN_SIZE, 1,
                          SDLOP_GLX_BLUE_SIZE, 1, 0 };
        vi = p_glXChooseVisual(dpy, 0, minimal);
        if (!vi) {
            return SDL_SetError("glXChooseVisual found no usable visual");
        }
    }

    *visual_out = vi->visual;
    *depth_out = vi->depth;

    SDLOP_Colormap cmap = SDLOP_X11_XCreateColormap(dpy, SDLOP_X11_XRootWindow(dpy, 0), vi->visual, 0 /* AllocNone */);
    if (cmap == SDLOP_None) {
        SDLOP_X11_XFree(vi);
        return SDL_SetError("XCreateColormap failed for GLX visual");
    }
    *colormap_out = cmap;

    /* remember for the context creation: the window's driverdata does not
     * exist yet, so the visual is stashed and committed by
     * SDLOP_X11_GL_CommitVisual() right after XCreateWindow */
    pending_vi = *vi;
    pending_vi_valid = true;
    pending_colormap = cmap;
    SDLOP_X11_XFree(vi);
    (void)window;
    return true;
}

/* called by x11_CreateWindow once driverdata exists */
void SDLOP_X11_GL_CommitVisual(SDL_Window *window)
{
    if (!pending_vi_valid) {
        return;
    }
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    wd->glx_vi = pending_vi;
    wd->glx_vi_valid = true;
    wd->colormap = pending_colormap;
    pending_vi_valid = false;
    pending_colormap = SDLOP_None;
}

void *SDLOP_X11_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    if (!ensure_glx(device)) {
        return NULL;
    }
    SDLOP_Display *dpy = SDLOP_X11_GetDisplay(device);
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd) {
        SDL_SetError("Invalid window");
        return NULL;
    }

    SDLOP_GLXContext share = NULL;
    if (sdlop_glattrs.share_with_current_context) {
        SDL_GLContext cur = SDLOP_GL_CurrentContext();
        if (cur) {
            share = ((X11GLContext *)cur)->ctx;
        }
    }

    bool want_modern = sdlop_glattrs.major_version >= 3 ||
                       (sdlop_glattrs.profile_mask & (SDL_GL_CONTEXT_PROFILE_CORE | SDL_GL_CONTEXT_PROFILE_COMPATIBILITY));

    SDLOP_GLXContext ctx = NULL;
    if (want_modern && p_glXCreateContextAttribsARB) {
        /* GLX_ARB_create_context: needs an FBConfig whose visual matches
         * the window's; enumerate configs and pick the matching one */
        typedef void *(*PFN_glXChooseFBConfig)(SDLOP_Display *, int, const int *, int *);
        typedef SDLOP_XVisualInfo *(*PFN_glXGetVisualFromFBConfig)(SDLOP_Display *, void *);
        PFN_glXChooseFBConfig choose_fb =
            (PFN_glXChooseFBConfig)glx_get_proc("glXChooseFBConfig");
        PFN_glXGetVisualFromFBConfig visual_from_fb =
            (PFN_glXGetVisualFromFBConfig)glx_get_proc("glXGetVisualFromFBConfig");
        if (choose_fb && visual_from_fb) {
            int fb_attribs[32];
            int n = 0;
            fb_attribs[n++] = 0x8010; fb_attribs[n++] = 1;              /* GLX_X_RENDERABLE, True */
            fb_attribs[n++] = 0x8011; fb_attribs[n++] = 0x8014;         /* GLX_RENDER_TYPE, GLX_RGBA_TYPE */
            fb_attribs[n++] = 0x22;   fb_attribs[n++] = 0x8000;         /* GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR */
            fb_attribs[n++] = 5;      fb_attribs[n++] = 1;              /* GLX_DOUBLEBUFFER */
            fb_attribs[n++] = 8;      fb_attribs[n++] = sdlop_glattrs.red_size;
            fb_attribs[n++] = 9;      fb_attribs[n++] = sdlop_glattrs.green_size;
            fb_attribs[n++] = 10;     fb_attribs[n++] = sdlop_glattrs.blue_size;
            fb_attribs[n++] = 11;     fb_attribs[n++] = sdlop_glattrs.alpha_size;
            fb_attribs[n++] = 12;     fb_attribs[n++] = sdlop_glattrs.depth_size;
            fb_attribs[n++] = 13;     fb_attribs[n++] = sdlop_glattrs.stencil_size;
            fb_attribs[n++] = 0;
            int count = 0;
            void **fbs = (void **)choose_fb(dpy, 0, fb_attribs, &count);
            void *chosen = NULL;
            for (int i = 0; fbs && i < count; i++) {
                SDLOP_XVisualInfo *vi = visual_from_fb(dpy, fbs[i]);
                if (vi) {
                    bool match = !wd->glx_vi_valid || vi->visualid == wd->glx_vi.visualid;
                    SDLOP_VisualID vid = vi->visualid;
                    SDLOP_X11_XFree(vi);
                    if (match) {
                        chosen = fbs[i];
                        (void)vid;
                        break;
                    }
                }
            }
            if (chosen) {
                int ctx_attribs[16];
                int m = 0;
                ctx_attribs[m++] = SDLOP_GLX_CONTEXT_MAJOR_VERSION_ARB; ctx_attribs[m++] = sdlop_glattrs.major_version;
                ctx_attribs[m++] = SDLOP_GLX_CONTEXT_MINOR_VERSION_ARB; ctx_attribs[m++] = sdlop_glattrs.minor_version;
                if (sdlop_glattrs.profile_mask & (SDL_GL_CONTEXT_PROFILE_CORE | SDL_GL_CONTEXT_PROFILE_COMPATIBILITY)) {
                    ctx_attribs[m++] = SDLOP_GLX_CONTEXT_PROFILE_MASK_ARB;
                    ctx_attribs[m++] = (sdlop_glattrs.profile_mask & SDL_GL_CONTEXT_PROFILE_COMPATIBILITY)
                                           ? SDLOP_GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
                                           : SDLOP_GLX_CONTEXT_CORE_PROFILE_BIT_ARB;
                }
                int flags = 0;
                if (sdlop_glattrs.flags & SDL_GL_CONTEXT_DEBUG_FLAG) {
                    flags |= SDLOP_GLX_CONTEXT_DEBUG_BIT_ARB;
                }
                if (sdlop_glattrs.flags & SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG) {
                    flags |= SDLOP_GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB;
                }
                if (flags) {
                    ctx_attribs[m++] = SDLOP_GLX_CONTEXT_FLAGS_ARB;
                    ctx_attribs[m++] = flags;
                }
                ctx_attribs[m++] = 0;
                ctx = p_glXCreateContextAttribsARB(dpy, chosen, share, SDLOP_True, ctx_attribs);
                if (!ctx) {
                    /* direct rendering may be unavailable (e.g. Xvfb):
                     * retry indirect */
                    ctx = p_glXCreateContextAttribsARB(dpy, chosen, share, SDLOP_False, ctx_attribs);
                }
            }
            if (fbs) {
                SDLOP_X11_XFree(fbs);
            }
        }
        if (!ctx) {
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "SDLop GLX: modern context creation failed, trying legacy");
        }
    }

    if (!ctx) {
        /* legacy path: window visual (if chosen) or a fresh pick */
        if (wd->glx_vi_valid) {
            SDLOP_XVisualInfo vi = wd->glx_vi;
            ctx = p_glXCreateContext(dpy, &vi, share, SDLOP_True);
            if (!ctx) {
                ctx = p_glXCreateContext(dpy, &vi, share, SDLOP_False);
            }
        } else {
            int attribs[32];
            glx_visual_attribs(attribs, 32);
            SDLOP_XVisualInfo *vi = p_glXChooseVisual(dpy, 0, attribs);
            if (!vi) {
                SDL_SetError("glXChooseVisual found no usable visual");
                return NULL;
            }
            ctx = p_glXCreateContext(dpy, vi, share, SDLOP_True);
            if (!ctx) {
                ctx = p_glXCreateContext(dpy, vi, share, SDLOP_False);
            }
            SDLOP_X11_XFree(vi);
        }
        if (!ctx) {
            SDL_SetError("glXCreateContext failed");
            return NULL;
        }
    }

    X11GLContext *glctx = (X11GLContext *)calloc(1, sizeof(*glctx));
    if (!glctx) {
        p_glXDestroyContext(dpy, ctx);
        SDL_OutOfMemory();
        return NULL;
    }
    glctx->ctx = ctx;
    return glctx;
}

bool SDLOP_X11_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context)
{
    if (!glx_lib) {
        return SDL_SetError("GLX not loaded");
    }
    SDLOP_Display *dpy = SDLOP_X11_GetDisplay(device);
    if (!context) {
        if (!p_glXMakeCurrent(dpy, SDLOP_None, NULL)) {
            return SDL_SetError("glXMakeCurrent(NULL) failed");
        }
        if (window) {
            SDLOP_X11_SetGLActive(window, false);
        }
        return true;
    }
    if (!window) {
        return SDL_SetError("NULL window with a GL context");
    }
    X11GLContext *glctx = (X11GLContext *)context;
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || wd->xwindow == SDLOP_None) {
        return SDL_SetError("Invalid window for GLX");
    }
    if (!p_glXMakeCurrent(dpy, wd->xwindow, glctx->ctx)) {
        return SDL_SetError("glXMakeCurrent failed");
    }
    SDLOP_X11_SetGLActive(window, true);
    /* re-assert the swap interval on each bind (SDL3 behavior) */
    if (swap_interval > 0) {
        if (p_glXSwapIntervalEXT) {
            p_glXSwapIntervalEXT(dpy, wd->xwindow, swap_interval);
        } else if (p_glXSwapIntervalMESA) {
            p_glXSwapIntervalMESA((unsigned int)swap_interval);
        } else if (p_glXSwapIntervalSGI) {
            p_glXSwapIntervalSGI(swap_interval);
        }
    }
    return true;
}

bool SDLOP_X11_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (!glx_lib) {
        return SDL_SetError("GLX not loaded");
    }
    X11WindowData *wd = (X11WindowData *)window->driverdata;
    if (!wd || wd->xwindow == SDLOP_None) {
        return SDL_SetError("Invalid window for GLX swap");
    }
    p_glXSwapBuffers(SDLOP_X11_GetDisplay(device), wd->xwindow);
    return true;
}

void SDLOP_X11_GL_DeleteContext(SDLop_VideoDevice *device, void *context)
{
    (void)device;
    X11GLContext *glctx = (X11GLContext *)context;
    if (!glctx) {
        return;
    }
    if (glx_lib) {
        SDLOP_Display *dpy = SDLOP_X11_GetDisplay(device);
        if (dpy) {
            p_glXMakeCurrent(dpy, SDLOP_None, NULL);
            p_glXDestroyContext(dpy, glctx->ctx);
        }
    }
    free(glctx);
}

SDL_FunctionPointer SDLOP_X11_GL_GetProcAddressThunk(SDLop_VideoDevice *device, const char *proc)
{
    (void)device;
    if (!glx_lib && !ensure_glx(device)) {
        return NULL;
    }
    return (SDL_FunctionPointer)glx_get_proc(proc);
}

bool SDLOP_X11_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval)
{
    (void)device;
    if (interval < 0) {
        return SDL_SetError("Adaptive vsync (-1) is not supported");
    }
    swap_interval = interval;
    if (glx_lib && glx_available) {
        SDLOP_Display *dpy = SDLOP_X11_GetDisplay(device);
        if (p_glXSwapIntervalMESA) {
            if (p_glXSwapIntervalMESA((unsigned int)interval) == 0) {
                return true;
            }
        }
        if (p_glXSwapIntervalSGI) {
            if (p_glXSwapIntervalSGI(interval) == 0) {
                return true;
            }
        }
        if (p_glXSwapIntervalEXT && dpy) {
            /* needs the current drawable; apply lazily on MakeCurrent if
             * nothing is bound */
            SDL_GLContext cur = SDLOP_GL_CurrentContext();
            if (!cur) {
                return true; /* stored; applied on next MakeCurrent */
            }
        }
    }
    return true;
}

bool SDLOP_X11_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval)
{
    (void)device;
    *interval = swap_interval;
    return true;
}

void SDLOP_X11_GL_WindowDestroyed(SDL_Window *window)
{
    SDLOP_X11_SetGLActive(window, false);
}

void SDLOP_X11_GL_Quit(SDLop_VideoDevice *device)
{
    (void)device;
    if (glx_lib) {
        dlclose(glx_lib);
        glx_lib = NULL;
    }
    p_glXChooseVisual = NULL;
    p_glXCreateContext = NULL;
    p_glXDestroyContext = NULL;
    p_glXMakeCurrent = NULL;
    p_glXSwapBuffers = NULL;
    p_glXGetProcAddressARB = NULL;
    p_glXQueryExtension = NULL;
    p_glXQueryVersion = NULL;
    p_glXQueryExtensionsString = NULL;
    p_glXCreateContextAttribsARB = NULL;
    p_glXSwapIntervalMESA = NULL;
    p_glXSwapIntervalEXT = NULL;
    p_glXSwapIntervalSGI = NULL;
    glx_available = false;
    pending_vi_valid = false;
    pending_colormap = SDLOP_None;
    swap_interval = 0;
}
