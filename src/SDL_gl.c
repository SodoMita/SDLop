/*
  SDLop - OpenGL context API (dispatches to the video driver; EGL on
  Wayland with Mesa llvmpipe as the software rasterizer).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

SDLOP_GLAttributes sdlop_glattrs;
static bool glattrs_initialized;

static void init_glattrs(void)
{
    if (glattrs_initialized) {
        return;
    }
    /* SDL3 defaults */
    sdlop_glattrs.red_size = 8;   /* SDL3: SDL_GL_RED_SIZE default 3? SDL uses 8/8/8/0 defaults
                                     on most platforms; EGL config matching below is forgiving */
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
    glattrs_initialized = true;
}

void SDL_GL_ResetAttributes(void)
{
    glattrs_initialized = false;
    init_glattrs();
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
    case SDL_GL_ACCELERATED_VISUAL:
    case SDL_GL_RETAINED_BACKING:
    case SDL_GL_CONTEXT_RELEASE_BEHAVIOR:
    case SDL_GL_CONTEXT_RESET_NOTIFICATION:
    case SDL_GL_CONTEXT_NO_ERROR:
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
    case SDL_GL_ACCELERATED_VISUAL: *value = 0; break; /* llvmpipe: software */
    default:
        return SDL_SetError("Unknown OpenGL attribute: %d", (int)attr);
    }
    return true;
}

bool SDL_GL_LoadLibrary(const char *path)
{
    (void)path;
    /* EGL/GLES are loaded by the active video driver on first use. */
    return true;
}

void SDL_GL_UnloadLibrary(void)
{
}

SDL_FunctionPointer SDL_GL_GetProcAddress(const char *proc)
{
    if (!proc) {
        SDL_SetError("NULL proc name");
        return NULL;
    }
    if (sdlop.video && sdlop.video->GL_GetProcAddress) {
        return sdlop.video->GL_GetProcAddress(sdlop.video, proc);
    }
    return NULL;
}

/* current context tracking */
static SDL_GLContext current_context;
static SDL_Window *current_window;

SDL_GLContext SDLOP_GL_CurrentContext(void)
{
    return current_context;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
    if (!window) {
        SDL_SetError("Invalid window");
        return NULL;
    }
    if (!sdlop.video || !sdlop.video->GL_CreateContext) {
        SDL_SetError("Video driver does not support OpenGL");
        return NULL;
    }
    init_glattrs();
    void *ctx = sdlop.video->GL_CreateContext(sdlop.video, window);
    if (!ctx) {
        return NULL; /* driver sets error */
    }
    if (!SDL_GL_MakeCurrent(window, (SDL_GLContext)ctx)) {
        sdlop.video->GL_DeleteContext(sdlop.video, ctx);
        return NULL;
    }
    return (SDL_GLContext)ctx;
}

bool SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
    if (!sdlop.video || !sdlop.video->GL_MakeCurrent) {
        return SDL_SetError("Video driver does not support OpenGL");
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
    if (!sdlop.video || !sdlop.video->GL_SetSwapInterval) {
        return SDL_SetError("Video driver does not support OpenGL");
    }
    return sdlop.video->GL_SetSwapInterval(sdlop.video, interval);
}

bool SDL_GL_GetSwapInterval(int *interval)
{
    if (!interval) {
        return SDL_SetError("NULL interval pointer");
    }
    if (!sdlop.video || !sdlop.video->GL_GetSwapInterval) {
        return SDL_SetError("Video driver does not support OpenGL");
    }
    return sdlop.video->GL_GetSwapInterval(sdlop.video, interval);
}

bool SDL_GL_SwapWindow(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (!sdlop.video || !sdlop.video->GL_SwapBuffers) {
        return SDL_SetError("Video driver does not support OpenGL");
    }
    return sdlop.video->GL_SwapBuffers(sdlop.video, window);
}

void SDL_GL_DestroyContext(SDL_GLContext context)
{
    if (!context) {
        return;
    }
    if (current_context == context) {
        current_context = NULL;
        current_window = NULL;
    }
    if (sdlop.video && sdlop.video->GL_DeleteContext) {
        sdlop.video->GL_DeleteContext(sdlop.video, context);
    }
}
