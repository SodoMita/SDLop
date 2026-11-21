/*
  SDLop - OpenGL context API (dispatches to the video driver, with a
  platform-agnostic headless GPU offload fallback in SDL_gl.c).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#ifndef __EMSCRIPTEN__
#include "video/sdlop_gl_offload.h"
#endif

SDLOP_GLAttributes sdlop_glattrs;
static bool glattrs_initialized;

/* current context tracking */
static SDL_GLContext current_context;
static SDL_Window *current_window;

static void init_glattrs(void)
{
    if (glattrs_initialized) {
        return;
    }
    /* SDL3 defaults */
    sdlop_glattrs.red_size = 8;
    sdlop_glattrs.green_size = 8;
    sdlop_glattrs.blue_size = 8;
    sdlop_glattrs.alpha_size = 0;
    sdlop_glattrs.buffer_size = 0;
    sdlop_glattrs.doublebuffer = 1;
    sdlop_glattrs.depth_size = 16; /* SDL3 default */
    sdlop_glattrs.stencil_size = 0;
    sdlop_glattrs.stereo = 0;
    sdlop_glattrs.multisamplebuffers = 0;
    sdlop_glattrs.multisamplesamples = 0;
    sdlop_glattrs.major_version = 2;
    sdlop_glattrs.minor_version = 0;
    sdlop_glattrs.flags = 0;
    sdlop_glattrs.profile_mask = 0;
    sdlop_glattrs.share_with_current_context = 0;
    sdlop_glattrs.framebuffer_srgb_capable = 0;
    sdlop_glattrs.floatbuffers = 0;
    sdlop_glattrs.release_behavior = SDL_GL_CONTEXT_RELEASE_BEHAVIOR_FLUSH;
    sdlop_glattrs.reset_notification = SDL_GL_CONTEXT_RESET_NO_NOTIFICATION;
    sdlop_glattrs.no_error = 0;
    sdlop_glattrs.accelerated_visual = -1;
    glattrs_initialized = true;
}

/* SDL_EGL_SetAttributeCallbacks (read by the EGL drivers) */
static SDL_EGLAttribArrayCallback egl_platform_cb;
static SDL_EGLIntArrayCallback egl_surface_cb;
static SDL_EGLIntArrayCallback egl_context_cb;
static void *egl_cb_userdata;

void SDLOP_GetEGLAttributeCallbacks(SDL_EGLAttribArrayCallback *platform, SDL_EGLIntArrayCallback *surface,
                                    SDL_EGLIntArrayCallback *context, void **userdata)
{
    if (platform) {
        *platform = egl_platform_cb;
    }
    if (surface) {
        *surface = egl_surface_cb;
    }
    if (context) {
        *context = egl_context_cb;
    }
    if (userdata) {
        *userdata = egl_cb_userdata;
    }
}

void SDL_EGL_SetAttributeCallbacks(SDL_EGLAttribArrayCallback platformAttribCallback,
                                   SDL_EGLIntArrayCallback surfaceAttribCallback,
                                   SDL_EGLIntArrayCallback contextAttribCallback, void *userdata)
{
    if (!sdlop.video) {
        return; /* stock: ignored before SDL_Init(VIDEO) */
    }
    egl_platform_cb = platformAttribCallback;
    egl_surface_cb = surfaceAttribCallback;
    egl_context_cb = contextAttribCallback;
    egl_cb_userdata = userdata;
}

void SDL_GL_ResetAttributes(void)
{
    /* stock also forgets the EGL attribute callbacks here */
    egl_platform_cb = NULL;
    egl_surface_cb = NULL;
    egl_context_cb = NULL;
    egl_cb_userdata = NULL;
    glattrs_initialized = false;
    init_glattrs();
}

SDL_EGLDisplay SDL_EGL_GetCurrentDisplay(void)
{
    if (!sdlop.video) {
        SDL_SetError("Video subsystem has not been initialized");
        return NULL;
    }
    void *dpy = sdlop.video->EGL_GetDisplay ? sdlop.video->EGL_GetDisplay(sdlop.video) : NULL;
    if (!dpy) {
        SDL_SetError("There is no current EGL display");
    }
    return (SDL_EGLDisplay)dpy;
}

SDL_EGLConfig SDL_EGL_GetCurrentConfig(void)
{
    if (!sdlop.video) {
        SDL_SetError("Video subsystem has not been initialized");
        return NULL;
    }
    if (!sdlop.video->EGL_GetDisplay || !sdlop.video->EGL_GetDisplay(sdlop.video)) {
        SDL_SetError("There is no current EGL display");
        return NULL;
    }
    return (SDL_EGLConfig)(sdlop.video->EGL_GetConfig ? sdlop.video->EGL_GetConfig(sdlop.video) : NULL);
}

SDL_EGLSurface SDL_EGL_GetWindowSurface(SDL_Window *window)
{
    if (!sdlop.video) {
        SDL_SetError("Video subsystem has not been initialized");
        return NULL;
    }
    if (!sdlop.video->EGL_GetDisplay || !sdlop.video->EGL_GetDisplay(sdlop.video)) {
        SDL_SetError("There is no current EGL display");
        return NULL;
    }
    if (!SDLOP_WindowValid(window)) {
        SDL_SetError("Invalid window");
        return NULL;
    }
    return (SDL_EGLSurface)(sdlop.video->EGL_GetWindowSurface ? sdlop.video->EGL_GetWindowSurface(sdlop.video, window)
                                                              : NULL);
}

SDL_FunctionPointer SDL_EGL_GetProcAddress(const char *proc)
{
    if (!sdlop.video) {
        SDL_SetError("Video subsystem has not been initialized");
        return NULL;
    }
    if (!sdlop.video->EGL_GetDisplay || !sdlop.video->EGL_GetDisplay(sdlop.video) ||
        !sdlop.video->GL_GetProcAddress) {
        SDL_SetError("No EGL library has been loaded");
        return NULL;
    }
    if (!proc) {
        SDL_SetError("Parameter '%s' is invalid", "proc");
        return NULL;
    }
    /* the EGL drivers resolve through eglGetProcAddress, then the
     * process symbol table (where libEGL's own entry points live) */
    return sdlop.video->GL_GetProcAddress(sdlop.video, proc);
}

/* SDL_GL_ExtensionSupported: ported from SDL 3.4.20 */
#define SDLOP_GL_EXTENSIONS 0x1F03
#define SDLOP_GL_VERSION 0x1F02
#define SDLOP_GL_NUM_EXTENSIONS 0x821D

bool SDL_GL_ExtensionSupported(const char *extension)
{
    typedef const unsigned char *(*PFN_glGetString)(unsigned int);
    typedef const unsigned char *(*PFN_glGetStringi)(unsigned int, unsigned int);
    typedef void (*PFN_glGetIntegerv)(unsigned int, int *);

    if (!extension || *extension == '\0' || SDL_strchr(extension, ' ')) {
        return false; /* extension names have no spaces */
    }
    /* a hint/environment variable named after the extension can veto it */
    const char *override = SDL_GetHint(extension);
    if (override && *override == '0') {
        return false;
    }
    if (!current_context) {
        return false; /* glGetString needs a current context */
    }
    PFN_glGetString get_string = (PFN_glGetString)SDL_GL_GetProcAddress("glGetString");
    if (!get_string) {
        return false;
    }
    const char *version = (const char *)get_string(SDLOP_GL_VERSION);
    /* "OpenGL ES 3.2 Mesa..." has no leading digit: stock's atoi reads 0,
     * so GLES always takes the GL_EXTENSIONS string path, as here */
    if (version && SDL_atoi(version) >= 3) {
        PFN_glGetStringi get_stringi = (PFN_glGetStringi)SDL_GL_GetProcAddress("glGetStringi");
        PFN_glGetIntegerv get_integerv = (PFN_glGetIntegerv)SDL_GL_GetProcAddress("glGetIntegerv");
        if (!get_stringi || !get_integerv) {
            return false;
        }
        int num = 0;
        get_integerv(SDLOP_GL_NUM_EXTENSIONS, &num);
        for (int i = 0; i < num; i++) {
            const char *e = (const char *)get_stringi(SDLOP_GL_EXTENSIONS, (unsigned int)i);
            if (e && SDL_strcmp(e, extension) == 0) {
                return true;
            }
        }
        return false;
    }
    const char *extensions = (const char *)get_string(SDLOP_GL_EXTENSIONS);
    if (!extensions) {
        return false;
    }
    const size_t len = SDL_strlen(extension);
    for (const char *start = extensions;;) {
        const char *where = SDL_strstr(start, extension);
        if (!where) {
            break;
        }
        const char *terminator = where + len;
        if ((where == extensions || where[-1] == ' ') && (*terminator == ' ' || *terminator == '\0')) {
            return true;
        }
        start = terminator;
    }
    return false;
}

bool SDL_GL_SetAttribute(SDL_GLAttr attr, int value)
{
    init_glattrs();
    switch (attr) {
    case SDL_GL_RED_SIZE: sdlop_glattrs.red_size = value; break;
    case SDL_GL_GREEN_SIZE: sdlop_glattrs.green_size = value; break;
    case SDL_GL_BLUE_SIZE: sdlop_glattrs.blue_size = value; break;
    case SDL_GL_ALPHA_SIZE: sdlop_glattrs.alpha_size = value; break;
    case SDL_GL_BUFFER_SIZE: sdlop_glattrs.buffer_size = value; break;
    case SDL_GL_DOUBLEBUFFER: sdlop_glattrs.doublebuffer = value; break;
    case SDL_GL_DEPTH_SIZE: sdlop_glattrs.depth_size = value; break;
    case SDL_GL_STENCIL_SIZE: sdlop_glattrs.stencil_size = value; break;
    case SDL_GL_ACCUM_RED_SIZE: sdlop_glattrs.accum_red = value; break;
    case SDL_GL_ACCUM_GREEN_SIZE: sdlop_glattrs.accum_green = value; break;
    case SDL_GL_ACCUM_BLUE_SIZE: sdlop_glattrs.accum_blue = value; break;
    case SDL_GL_ACCUM_ALPHA_SIZE: sdlop_glattrs.accum_alpha = value; break;
    case SDL_GL_STEREO: sdlop_glattrs.stereo = value; break;
    case SDL_GL_MULTISAMPLEBUFFERS: sdlop_glattrs.multisamplebuffers = value; break;
    case SDL_GL_MULTISAMPLESAMPLES: sdlop_glattrs.multisamplesamples = value; break;
    case SDL_GL_CONTEXT_MAJOR_VERSION: sdlop_glattrs.major_version = value; break;
    case SDL_GL_CONTEXT_MINOR_VERSION: sdlop_glattrs.minor_version = value; break;
    case SDL_GL_CONTEXT_FLAGS: sdlop_glattrs.flags = value; break;
    case SDL_GL_CONTEXT_PROFILE_MASK: sdlop_glattrs.profile_mask = value; break;
    case SDL_GL_SHARE_WITH_CURRENT_CONTEXT: sdlop_glattrs.share_with_current_context = value; break;
    case SDL_GL_FRAMEBUFFER_SRGB_CAPABLE: sdlop_glattrs.framebuffer_srgb_capable = value; break;
    case SDL_GL_FLOATBUFFERS: sdlop_glattrs.floatbuffers = value; break;
    /* used by GLX (stock passes them only when the GLX extension exists) */
    case SDL_GL_CONTEXT_RELEASE_BEHAVIOR: sdlop_glattrs.release_behavior = value; break;
    case SDL_GL_CONTEXT_RESET_NOTIFICATION: sdlop_glattrs.reset_notification = value; break;
    case SDL_GL_CONTEXT_NO_ERROR: sdlop_glattrs.no_error = value; break;
    case SDL_GL_ACCELERATED_VISUAL: sdlop_glattrs.accelerated_visual = value; break;
    case SDL_GL_RETAINED_BACKING:
    case SDL_GL_EGL_PLATFORM:
        break; /* accepted, not acted upon */
    default:
        return SDL_SetError("Unknown OpenGL attribute: %d", (int)attr);
    }
    return true;
}

bool SDL_GL_GetAttribute(SDL_GLAttr attr, int *value)
{
    init_glattrs();
    if (!value) {
        return SDL_SetError("NULL value pointer");
    }
    switch (attr) {
    case SDL_GL_RED_SIZE: *value = sdlop_glattrs.red_size; break;
    case SDL_GL_GREEN_SIZE: *value = sdlop_glattrs.green_size; break;
    case SDL_GL_BLUE_SIZE: *value = sdlop_glattrs.blue_size; break;
    case SDL_GL_ALPHA_SIZE: *value = sdlop_glattrs.alpha_size; break;
    case SDL_GL_BUFFER_SIZE: *value = sdlop_glattrs.buffer_size; break;
    case SDL_GL_DOUBLEBUFFER: *value = sdlop_glattrs.doublebuffer; break;
    case SDL_GL_DEPTH_SIZE: *value = sdlop_glattrs.depth_size; break;
    case SDL_GL_STENCIL_SIZE: *value = sdlop_glattrs.stencil_size; break;
    case SDL_GL_ACCUM_RED_SIZE: *value = sdlop_glattrs.accum_red; break;
    case SDL_GL_ACCUM_GREEN_SIZE: *value = sdlop_glattrs.accum_green; break;
    case SDL_GL_ACCUM_BLUE_SIZE: *value = sdlop_glattrs.accum_blue; break;
    case SDL_GL_ACCUM_ALPHA_SIZE: *value = sdlop_glattrs.accum_alpha; break;
    case SDL_GL_STEREO: *value = sdlop_glattrs.stereo; break;
    case SDL_GL_MULTISAMPLEBUFFERS: *value = sdlop_glattrs.multisamplebuffers; break;
    case SDL_GL_MULTISAMPLESAMPLES: *value = sdlop_glattrs.multisamplesamples; break;
    case SDL_GL_CONTEXT_MAJOR_VERSION: *value = sdlop_glattrs.major_version; break;
    case SDL_GL_CONTEXT_MINOR_VERSION: *value = sdlop_glattrs.minor_version; break;
    case SDL_GL_CONTEXT_FLAGS: *value = sdlop_glattrs.flags; break;
    case SDL_GL_CONTEXT_PROFILE_MASK: *value = sdlop_glattrs.profile_mask; break;
    case SDL_GL_FRAMEBUFFER_SRGB_CAPABLE: *value = sdlop_glattrs.framebuffer_srgb_capable; break;
    case SDL_GL_FLOATBUFFERS: *value = sdlop_glattrs.floatbuffers; break;
    case SDL_GL_CONTEXT_RELEASE_BEHAVIOR: *value = sdlop_glattrs.release_behavior; break;
    case SDL_GL_CONTEXT_RESET_NOTIFICATION: *value = sdlop_glattrs.reset_notification; break;
    case SDL_GL_CONTEXT_NO_ERROR: *value = sdlop_glattrs.no_error; break;
    case SDL_GL_ACCELERATED_VISUAL: *value = sdlop_glattrs.accelerated_visual >= 0 ? sdlop_glattrs.accelerated_visual : 0; break;
    default:
        return SDL_SetError("Unknown OpenGL attribute: %d", (int)attr);
    }
    return true;
}

bool SDL_GL_LoadLibrary(const char *path)
{
    (void)path;
    /* EGL/GLES are loaded by the active video driver or offload layer on first use. */
    return true;
}

void SDL_GL_UnloadLibrary(void)
{
}

/* current context tracking */
static SDL_GLContext current_context;
static SDL_Window *current_window;

#ifndef __EMSCRIPTEN__
/* ------------------------------------------------------------------ */
/* Platform-agnostic headless GPU GL offload (any framebuffer driver) */
/* ------------------------------------------------------------------ */

typedef struct SDLOP_GenericGLCtx
{
    SDLOP_GLOffloadContext *offload_ctx;
    int swap_interval;
    struct SDLOP_GenericGLCtx *next;
} SDLOP_GenericGLCtx;

typedef struct SDLOP_GenericGLWin
{
    SDL_Window *window;
    SDLOP_GLOffloadSurface *surf;
    SDLOP_GenericGLCtx *last_ctx;
    struct SDLOP_GenericGLWin *next;
} SDLOP_GenericGLWin;

static SDLOP_GenericGLCtx *generic_ctxs;
static SDLOP_GenericGLWin *generic_wins;
static bool generic_offload_used;

static SDLOP_GenericGLCtx *find_generic_ctx(SDL_GLContext ctx)
{
    for (SDLOP_GenericGLCtx *c = generic_ctxs; c; c = c->next) {
        if ((SDL_GLContext)c == ctx) {
            return c;
        }
    }
    return NULL;
}

static SDLOP_GenericGLWin *find_generic_win(SDL_Window *window)
{
    for (SDLOP_GenericGLWin *w = generic_wins; w; w = w->next) {
        if (w->window == window) {
            return w;
        }
    }
    return NULL;
}

static SDLOP_GenericGLWin *ensure_generic_win(SDLOP_GenericGLCtx *gctx, SDL_Window *window)
{
    SDLOP_GenericGLWin *gw = find_generic_win(window);
    if (!gw) {
        gw = (SDLOP_GenericGLWin *)calloc(1, sizeof(*gw));
        if (!gw) {
            SDL_OutOfMemory();
            return NULL;
        }
        gw->window = window;
        gw->next = generic_wins;
        generic_wins = gw;
    }
    int w = window->w > 0 ? window->w : 1;
    int h = window->h > 0 ? window->h : 1;
    if (!gw->surf) {
        gw->surf = SDLOP_GLOffload_CreateSurface(gctx->offload_ctx, w, h);
        if (!gw->surf) {
            return NULL;
        }
    } else {
        if (!SDLOP_GLOffload_ResizeSurface(gctx->offload_ctx, &gw->surf, w, h)) {
            return NULL;
        }
    }
    gw->last_ctx = gctx;
    return gw;
}

static bool can_use_generic_gl_offload(void)
{
    if (!sdlop.video ||
        sdlop.video->GL_CreateContext ||
        !sdlop.video->CreateWindowFramebuffer ||
        !sdlop.video->UpdateWindowFramebuffer) {
        return false;
    }
    SDLOP_GLOffloadPolicy policy = SDLOP_GLOffload_GetPolicy();
    if (policy == SDLOP_GL_OFFLOAD_NEVER) {
        return false;
    }
    if (sdlop_glattrs.accelerated_visual == 0 && policy != SDLOP_GL_OFFLOAD_FORCE) {
        return false;
    }
    return SDLOP_GLOffload_HasHardwareGPU();
}

static SDL_GLContext create_generic_offload_context(SDL_Window *window)
{
    SDLOP_GLOffloadContext *share = NULL;
    if (sdlop_glattrs.share_with_current_context && current_context) {
        SDLOP_GenericGLCtx *cur = find_generic_ctx(current_context);
        if (cur) {
            share = cur->offload_ctx;
        }
    }
    SDLOP_GLOffloadContext *oc = SDLOP_GLOffload_CreateContextAuto(share);
    if (!oc) {
        return NULL;
    }
    SDLOP_GenericGLCtx *gctx = (SDLOP_GenericGLCtx *)calloc(1, sizeof(*gctx));
    if (!gctx) {
        SDLOP_GLOffload_DeleteContext(oc);
        SDL_OutOfMemory();
        return NULL;
    }
    gctx->offload_ctx = oc;
    gctx->swap_interval = 0;
    gctx->next = generic_ctxs;
    generic_ctxs = gctx;
    generic_offload_used = true;

    if (!SDL_GL_MakeCurrent(window, (SDL_GLContext)gctx)) {
        SDL_GL_DestroyContext((SDL_GLContext)gctx);
        return NULL;
    }
    const char *desc = SDLOP_GLOffload_GetGPUDescription();
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "SDLop: OpenGL offload active on %s (%s)",
                sdlop.video ? sdlop.video->name : "?",
                desc ? desc : "hardware GPU");
    return (SDL_GLContext)gctx;
}
#endif /* !__EMSCRIPTEN__ */

SDL_FunctionPointer SDL_GL_GetProcAddress(const char *proc)
{
    if (!proc) {
        SDL_SetError("NULL proc name");
        return NULL;
    }
#ifndef __EMSCRIPTEN__
    if (current_context && find_generic_ctx(current_context)) {
        return SDLOP_GLOffload_GetProcAddress(proc);
    }
    if (generic_ctxs && (!sdlop.video || !sdlop.video->GL_GetProcAddress)) {
        return SDLOP_GLOffload_GetProcAddress(proc);
    }
#endif
    if (sdlop.video && sdlop.video->GL_GetProcAddress) {
        return sdlop.video->GL_GetProcAddress(sdlop.video, proc);
    }
    return NULL;
}

SDL_GLContext SDLOP_GL_CurrentContext(void)
{
    return current_context;
}

void SDLOP_GL_NotifyWindowResized(SDL_Window *window)
{
#ifndef __EMSCRIPTEN__
    if (!window) {
        return;
    }
    SDLOP_GenericGLWin *gw = find_generic_win(window);
    if (!gw || !gw->surf) {
        return;
    }
    SDLOP_GenericGLCtx *gctx = current_context ? find_generic_ctx(current_context) : NULL;
    if (!gctx) {
        gctx = gw->last_ctx;
    }
    if (!gctx || !gctx->offload_ctx) {
        return;
    }
    int w = window->w > 0 ? window->w : 1;
    int h = window->h > 0 ? window->h : 1;
    int sw = 0, sh = 0;
    SDLOP_GLOffload_GetSurfaceSize(gw->surf, &sw, &sh);
    if (sw != w || sh != h) {
        if (!SDLOP_GLOffload_ResizeSurface(gctx->offload_ctx, &gw->surf, w, h)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                        "SDLop: failed to resize GL offload surface to %dx%d: %s",
                        w, h, SDL_GetError());
        }
    }
#else
    (void)window;
#endif
}

void SDLOP_GL_NotifyWindowDestroyed(SDL_Window *window)
{
#ifndef __EMSCRIPTEN__
    if (!window) {
        return;
    }
    SDLOP_GenericGLWin **pp = &generic_wins;
    while (*pp) {
        SDLOP_GenericGLWin *gw = *pp;
        if (gw->window == window) {
            *pp = gw->next;
            if (current_window == window) {
                current_window = NULL;
            }
            if (gw->surf) {
                SDLOP_GLOffload_DestroySurface(gw->surf);
            }
            free(gw);
            break;
        }
        pp = &gw->next;
    }
#else
    (void)window;
#endif
}

void SDLOP_GL_Quit(void)
{
#ifndef __EMSCRIPTEN__
    while (generic_wins) {
        SDLOP_GenericGLWin *gw = generic_wins;
        generic_wins = gw->next;
        if (gw->surf) {
            SDLOP_GLOffload_DestroySurface(gw->surf);
        }
        free(gw);
    }
    while (generic_ctxs) {
        SDLOP_GenericGLCtx *gc = generic_ctxs;
        generic_ctxs = gc->next;
        if (gc->offload_ctx) {
            SDLOP_GLOffload_DeleteContext(gc->offload_ctx);
        }
        free(gc);
    }
    current_context = NULL;
    current_window = NULL;
    if (generic_offload_used || (sdlop.video && !sdlop.video->GL_CreateContext)) {
        generic_offload_used = false;
        SDLOP_GLOffload_Shutdown();
    }
#endif
}

bool SDLOP_GL_CanCreateWindow(void)
{
    if (!sdlop.video) {
        return false;
    }
    if (sdlop.video->GL_CreateContext) {
        return true;
    }
#ifndef __EMSCRIPTEN__
    if (sdlop.video->CreateWindowFramebuffer &&
        SDLOP_GLOffload_GetPolicy() != SDLOP_GL_OFFLOAD_NEVER &&
        SDLOP_GLOffload_HasHardwareGPU()) {
        return true;
    }
#endif
    return false;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
    if (!window) {
        SDL_SetError("Invalid window");
        return NULL;
    }
    if (!sdlop.video) {
        SDL_SetError("Video subsystem not initialized");
        return NULL;
    }
    init_glattrs();

    if (sdlop.video->GL_CreateContext) {
        void *ctx = sdlop.video->GL_CreateContext(sdlop.video, window);
        if (ctx) {
            if (!SDL_GL_MakeCurrent(window, (SDL_GLContext)ctx)) {
                sdlop.video->GL_DeleteContext(sdlop.video, ctx);
                return NULL;
            }
            return (SDL_GLContext)ctx;
        }
    }

#ifndef __EMSCRIPTEN__
    if (can_use_generic_gl_offload()) {
        return create_generic_offload_context(window);
    }
#endif

    if (!sdlop.video->GL_CreateContext) {
        SDL_SetError("Video driver '%s' does not support OpenGL", sdlop.video->name);
    }
    return NULL;
}

bool SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
    if (!sdlop.video) {
        return SDL_SetError("Video subsystem not initialized");
    }
#ifndef __EMSCRIPTEN__
    if (!context) {
        if (current_context && find_generic_ctx(current_context)) {
            SDLOP_GLOffload_MakeCurrent(NULL, NULL);
            current_context = NULL;
            current_window = NULL;
            return true;
        }
    } else {
        SDLOP_GenericGLCtx *gctx = find_generic_ctx(context);
        if (gctx) {
            if (!window) {
                return SDL_SetError("Invalid window");
            }
            SDLOP_GenericGLWin *gw = ensure_generic_win(gctx, window);
            if (!gw) {
                return false;
            }
            if (!SDLOP_GLOffload_MakeCurrent(gctx->offload_ctx, gw->surf)) {
                return false;
            }
            current_context = context;
            current_window = window;
            return true;
        }
    }
#endif

    if (!sdlop.video->GL_MakeCurrent) {
        return SDL_SetError("Video driver '%s' does not support OpenGL", sdlop.video->name);
    }
    if (!sdlop.video->GL_MakeCurrent(sdlop.video, window, context)) {
        return false;
    }
    current_context = context;
    current_window = context ? window : NULL;
    return true;
}

SDL_Window *SDL_GL_GetCurrentWindow(void)
{
    return current_window;
}

SDL_GLContext SDL_GL_GetCurrentContext(void)
{
    return current_context;
}

bool SDL_GL_SetSwapInterval(int interval)
{
    if (!sdlop.video) {
        return SDL_SetError("Video subsystem not initialized");
    }
#ifndef __EMSCRIPTEN__
    SDLOP_GenericGLCtx *gctx = current_context ? find_generic_ctx(current_context) : NULL;
    if (gctx) {
        gctx->swap_interval = interval;
        return true;
    }
    if (!sdlop.video->GL_SetSwapInterval && generic_ctxs) {
        generic_ctxs->swap_interval = interval;
        return true;
    }
#endif
    if (!sdlop.video->GL_SetSwapInterval) {
        return SDL_SetError("Video driver '%s' does not support OpenGL", sdlop.video->name);
    }
    return sdlop.video->GL_SetSwapInterval(sdlop.video, interval);
}

bool SDL_GL_GetSwapInterval(int *interval)
{
    if (!interval) {
        return SDL_SetError("NULL interval pointer");
    }
    if (!sdlop.video) {
        return SDL_SetError("Video subsystem not initialized");
    }
#ifndef __EMSCRIPTEN__
    SDLOP_GenericGLCtx *gctx = current_context ? find_generic_ctx(current_context) : NULL;
    if (gctx) {
        *interval = gctx->swap_interval;
        return true;
    }
    if (!sdlop.video->GL_GetSwapInterval && generic_ctxs) {
        *interval = generic_ctxs->swap_interval;
        return true;
    }
#endif
    if (!sdlop.video->GL_GetSwapInterval) {
        return SDL_SetError("Video driver '%s' does not support OpenGL", sdlop.video->name);
    }
    return sdlop.video->GL_GetSwapInterval(sdlop.video, interval);
}

bool SDL_GL_SwapWindow(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (!sdlop.video) {
        return SDL_SetError("Video subsystem not initialized");
    }
#ifndef __EMSCRIPTEN__
    SDLOP_GenericGLWin *gw = find_generic_win(window);
    SDLOP_GenericGLCtx *gctx = current_context ? find_generic_ctx(current_context) : NULL;
    if (!gctx && gw) {
        gctx = gw->last_ctx;
    }
    if (gctx && gw && gw->surf) {
        int ww = window->w > 0 ? window->w : 1;
        int wh = window->h > 0 ? window->h : 1;
        if (window->surface && (window->surface->w != ww || window->surface->h != wh)) {
            if (sdlop.video->DestroyWindowFramebuffer) {
                sdlop.video->DestroyWindowFramebuffer(sdlop.video, window);
            } else {
                SDL_DestroySurface(window->surface);
                window->surface = NULL;
            }
        }
        if (!window->surface) {
            if (!sdlop.video->CreateWindowFramebuffer ||
                !sdlop.video->CreateWindowFramebuffer(sdlop.video, window, &window->surface)) {
                return false;
            }
        }
        if (!SDLOP_GLOffload_ReadToXRGB8888(gctx->offload_ctx, gw->surf,
                                            window->surface->pixels,
                                            window->surface->pitch,
                                            window->surface->w,
                                            window->surface->h)) {
            return false;
        }
        SDL_Rect rect = { 0, 0, window->surface->w, window->surface->h };
        if (!sdlop.video->UpdateWindowFramebuffer ||
            !sdlop.video->UpdateWindowFramebuffer(sdlop.video, window, &rect, 1)) {
            return false;
        }
        int sw = 0, sh = 0;
        SDLOP_GLOffload_GetSurfaceSize(gw->surf, &sw, &sh);
        if (sw != ww || sh != wh) {
            if (!SDLOP_GLOffload_ResizeSurface(gctx->offload_ctx, &gw->surf, ww, wh)) {
                return false;
            }
        }
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_EXPOSED, 0, 0);
        return true;
    }
#endif

    if (!sdlop.video->GL_SwapBuffers) {
        return SDL_SetError("Video driver '%s' does not support OpenGL", sdlop.video->name);
    }
    if (!sdlop.video->GL_SwapBuffers(sdlop.video, window)) {
        return false;
    }
    /* stock SDL3 announces presented content with an expose event */
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_EXPOSED, 0, 0);
    return true;
}

bool SDL_GL_DestroyContext(SDL_GLContext context)
{
    if (!sdlop.video) {
        return SDL_SetError("Video subsystem has not been initialized");
    }
    if (!context) {
        return SDL_InvalidParamError("context");
    }
#ifndef __EMSCRIPTEN__
    SDLOP_GenericGLCtx *gctx = find_generic_ctx(context);
    if (gctx) {
        if (current_context == context) {
            SDLOP_GLOffload_MakeCurrent(NULL, NULL);
            current_context = NULL;
            current_window = NULL;
        }
        for (SDLOP_GenericGLWin *gw = generic_wins; gw; gw = gw->next) {
            if (gw->last_ctx == gctx) {
                gw->last_ctx = NULL;
            }
        }
        SDLOP_GenericGLCtx **pp = &generic_ctxs;
        while (*pp) {
            if (*pp == gctx) {
                *pp = gctx->next;
                break;
            }
            pp = &(*pp)->next;
        }
        if (gctx->offload_ctx) {
            SDLOP_GLOffload_DeleteContext(gctx->offload_ctx);
        }
        free(gctx);
        return true;
    }
#endif

    if (current_context == context) {
        current_context = NULL;
        current_window = NULL;
    }
    if (sdlop.video && sdlop.video->GL_DeleteContext) {
        sdlop.video->GL_DeleteContext(sdlop.video, context);
    }
    return true;
}
