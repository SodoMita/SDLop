/*
  SDLop - OpenGL on WGL (Windows).

  opengl32.dll is linked directly (it ships with Windows and always exports
  wglCreateContext/wglSwapBuffers); everything newer - wglCreateContextAttribsARB,
  wglSwapIntervalEXT, the 3.0+ entry points - is fetched through
  wglGetProcAddress, with LoadLibrary/GetProcAddress as the fallback for
  functions that are exported by name (wglGetProcAddress only covers the
  extension set).

  The pixel format is chosen with ChoosePixelFormat + SetPixelFormat because
  that works on every driver including Wine's; WGL_ARB_pixel_format would be
  prettier but buys nothing here (SDL3 uses it for sRGB/swaplimit requests).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#ifdef _WIN32

#include "sdlop_windows.h"

#include <GL/gl.h>

/* --- the WGL entry points we cannot get from <GL/gl.h> --- */

typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int);
typedef int (WINAPI *PFNWGLGETSWAPINTERVALEXTPROC)(void);
typedef BOOL (WINAPI *PFNWGLCHOOSEPIXELFORMATARBPROC)(HDC, const int *, const FLOAT *, UINT, int *, UINT *);
typedef const char *(WINAPI *PFNWGLGETEXTENSIONSSTRINGARBPROC)(HDC);

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
/* WGL_EXT_create_context_es2_profile / es_profile. SDL_GL_CONTEXT_PROFILE_ES is
 * 0x0004 and WGL's ES2 profile bit is the same value, so the mask can be passed
 * through unchanged -- which is what SDL3 does ("SDL profile bits match WGL
 * profile bits"). */
#define WGL_CONTEXT_ES2_PROFILE_BIT_EXT 0x0004
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#define WGL_CONTEXT_DEBUG_BIT_ARB 0x00000001
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x00000002

static struct
{
    bool initialized;
    HINSTANCE opengl32;
    PFNWGLCREATECONTEXTATTRIBSARBPROC create_context_attribs;
    PFNWGLSWAPINTERVALEXTPROC swap_interval;
    PFNWGLGETSWAPINTERVALEXTPROC get_swap_interval;
    PFNWGLGETEXTENSIONSSTRINGARBPROC get_extensions_string;
    bool has_swap_control_tear;
    int current_interval;
} wgl;

/* One shared "bootstrap" context: wglGetProcAddress only answers for a
 * thread that has a current context. */
static HGLRC bootstrap_context;
static HDC bootstrap_dc;
static HWND bootstrap_hwnd;

static void *win32_gl_load(const char *name)
{
    if (!wgl.opengl32) {
        wgl.opengl32 = LoadLibraryW(L"opengl32.dll");
        if (!wgl.opengl32) {
            return NULL;
        }
    }
    return (void *)GetProcAddress(wgl.opengl32, name);
}

static void *win32_gl_get_proc(const char *name)
{
    /* wglGetProcAddress wins for extension entry points; GL 1.1 functions
     * are only exported by opengl32.dll and come back NULL here. */
    void *p = NULL;
    if (wglGetCurrentDC()) {
        p = (void *)wglGetProcAddress(name);
        /* the "not found" sentinels are 0, 1, 2, 3 and -1 */
        if (p == (void *)0 || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {
            p = NULL;
        }
    }
    if (!p) {
        p = win32_gl_load(name);
    }
    return p;
}

static void win32_gl_init_extensions(void)
{
    if (wgl.initialized) {
        return;
    }
    wgl.create_context_attribs = (PFNWGLCREATECONTEXTATTRIBSARBPROC)win32_gl_get_proc("wglCreateContextAttribsARB");
    wgl.swap_interval = (PFNWGLSWAPINTERVALEXTPROC)win32_gl_get_proc("wglSwapIntervalEXT");
    wgl.get_swap_interval = (PFNWGLGETSWAPINTERVALEXTPROC)win32_gl_get_proc("wglGetSwapIntervalEXT");
    wgl.get_extensions_string = (PFNWGLGETEXTENSIONSSTRINGARBPROC)win32_gl_get_proc("wglGetExtensionsStringARB");
    wgl.current_interval = 1;
    /* Adaptive vsync (SDL_GL_SetSwapInterval(-1)) is only legal on a context
     * whose pixel format was created with
     * WGL_EXT_swap_control_tear present and wglSwapIntervalEXT(-1) accepted;
     * asking anyway returns FALSE and, on some drivers, silently pins the
     * interval at 0. Record whether the extension exists so -1 can be
     * rejected for the right reason instead of as a generic bad argument. */
    {
        const char *exts = wgl.get_extensions_string ? wgl.get_extensions_string(NULL) : NULL;
        wgl.has_swap_control_tear = (exts && SDL_strstr(exts, "WGL_EXT_swap_control_tear")) ? true : false;
    }
    wgl.initialized = true;
}

/* A hidden window is enough for wglGetProcAddress; creating it once keeps
 * the cost off every SDL_GL_CreateContext call. */
static bool win32_gl_ensure_bootstrap(void)
{
    if (bootstrap_context) {
        return true;
    }
    if (!sdlop_win32.class_registered) {
        return false;
    }
    HWND hwnd = CreateWindowExW(0, L"SDLop_Window", L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 16, 16, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) {
        return false;
    }
    bootstrap_dc = GetDC(hwnd);
    if (!bootstrap_dc) {
        DestroyWindow(hwnd);
        return false;
    }
    PIXELFORMATDESCRIPTOR pfd;
    SDL_zero(pfd);
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    const int fmt = ChoosePixelFormat(bootstrap_dc, &pfd);
    if (fmt && SetPixelFormat(bootstrap_dc, fmt, &pfd)) {
        bootstrap_context = wglCreateContext(bootstrap_dc);
    }
    if (!bootstrap_context) {
        ReleaseDC(hwnd, bootstrap_dc);
        bootstrap_dc = NULL;
        DestroyWindow(hwnd);
        return false;
    }
    /* The window has to outlive the context -- wglGetProcAddress needs a
     * current context tied to a live DC -- so it is kept for the process, and
     * released by SDLOP_Win32GLQuit() from SDL_Quit rather than never. */
    bootstrap_hwnd = hwnd;
    return true;
}

/* Release the bootstrap context and its window. Only called from SDL_Quit,
 * where no application context can be current any more. */
void SDLOP_Win32GLQuit(void)
{
    if (bootstrap_context) {
        /* leave nothing current on this thread before releasing the DC */
        if (wglGetCurrentContext() == bootstrap_context) {
            wglMakeCurrent(NULL, NULL);
        }
        wglDeleteContext(bootstrap_context);
    }
    bootstrap_context = NULL;
    if (bootstrap_hwnd) {
        if (bootstrap_dc) {
            ReleaseDC(bootstrap_hwnd, bootstrap_dc);
        }
        DestroyWindow(bootstrap_hwnd);
        bootstrap_hwnd = NULL;
    }
    bootstrap_dc = NULL;
    wgl.initialized = false;
}

/* Create it without disturbing an application's current context. */
static void win32_gl_bootstrap_proc_addresses(void)
{
    if (wgl.initialized) {
        return;
    }
    if (wglGetCurrentContext()) {
        win32_gl_init_extensions();
        return;
    }
    if (!win32_gl_ensure_bootstrap()) {
        wgl.initialized = true; /* no bootstrap: only GL 1.1 is reachable */
        return;
    }
    if (wglMakeCurrent(bootstrap_dc, bootstrap_context)) {
        win32_gl_init_extensions();
        wglMakeCurrent(NULL, NULL);
    } else {
        wgl.initialized = true;
    }
}

/* ------------------------------------------------------------------ */
/* Contexts                                                            */
/* ------------------------------------------------------------------ */

/* Requested attributes live in the window's SDL_GL_* properties; SDL_video.c
 * stores them on the window and this driver reads them. */

static bool choose_pixel_format(SDL_Window *window, HDC hdc, PIXELFORMATDESCRIPTOR *pfd)
{
    (void)window;
    SDL_zero(*pfd);
    pfd->nSize = sizeof(*pfd);
    pfd->nVersion = 1;
    pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd->iPixelType = PFD_TYPE_RGBA;

    /* SDL3 keeps the requested attributes in one place per process
     * (sdlop_glattrs here) and every driver reads them from there. */
    const SDLOP_GLAttributes *gl = &sdlop_glattrs;
    int r = gl->red_size > 0 ? gl->red_size : 8;
    int g = gl->green_size > 0 ? gl->green_size : 8;
    int b = gl->blue_size > 0 ? gl->blue_size : 8;
    int a = gl->alpha_size > 0 ? gl->alpha_size : 8;
    int depth = gl->depth_size > 0 ? gl->depth_size : 24;
    int stencil = gl->stencil_size > 0 ? gl->stencil_size : 8;
    const bool doublebuffer = gl->doublebuffer != 0;
    if (!doublebuffer) {
        pfd->dwFlags &= ~PFD_DOUBLEBUFFER;
    }
    pfd->cColorBits = (BYTE)(r + g + b);
    pfd->cRedBits = (BYTE)r;
    pfd->cGreenBits = (BYTE)g;
    pfd->cBlueBits = (BYTE)b;
    pfd->cAlphaBits = (BYTE)a;
    pfd->cDepthBits = (BYTE)depth;
    pfd->cStencilBits = (BYTE)stencil;

    const int fmt = ChoosePixelFormat(hdc, pfd);
    if (!fmt) {
        /* Retry without alpha/stencil: some drivers (and Wine's) have no
         * 32-bit-alpha window formats at all. */
        pfd->cAlphaBits = 0;
        pfd->cStencilBits = 0;
        const int fmt2 = ChoosePixelFormat(hdc, pfd);
        if (!fmt2) {
            return SDL_SetError("No usable OpenGL pixel format");
        }
        if (!SetPixelFormat(hdc, fmt2, pfd)) {
            return SDL_SetError("SetPixelFormat() failed");
        }
        return true;
    }
    if (!SetPixelFormat(hdc, fmt, pfd)) {
        return SDL_SetError("SetPixelFormat() failed");
    }
    return true;
}

void *SDLOP_Win32GLCreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        SDL_SetError("Window has no HWND");
        return NULL;
    }
    if (!wgl.opengl32) {
        wgl.opengl32 = LoadLibraryW(L"opengl32.dll");
        if (!wgl.opengl32) {
            SDL_SetError("Couldn't load opengl32.dll");
            return NULL;
        }
    }
    win32_gl_bootstrap_proc_addresses();

    HDC hdc = GetDC(data->hwnd);
    if (!hdc) {
        SDL_SetError("GetDC() failed");
        return NULL;
    }
    /* SetPixelFormat can only be called once per window: remember it */
    PIXELFORMATDESCRIPTOR pfd;
    const int existing = GetPixelFormat(hdc);
    if (!existing) {
        if (!choose_pixel_format(window, hdc, &pfd)) {
            ReleaseDC(data->hwnd, hdc);
            return NULL;
        }
    } else {
        DescribePixelFormat(hdc, existing, sizeof(pfd), &pfd);
    }

    /* Default to 2.1 only when nothing was requested. A requested minor of 0
     * is legitimate and must be passed through verbatim: `minor > 0 ? minor : 1`
     * turned GL 3.0 into "3.1" and GL 4.0 into "4.1", and asked
     * WGL_EXT_create_context_es2_profile for a non-existent OpenGL ES 2.1
     * (ERROR_INVALID_VERSION_ARB) when the app asked for ES 2.0. */
    const int major = sdlop_glattrs.major_version > 0 ? sdlop_glattrs.major_version : 2;
    const int minor = sdlop_glattrs.major_version > 0 ? sdlop_glattrs.minor_version : 1;
    const int profile = sdlop_glattrs.profile_mask;
    const bool debug = (sdlop_glattrs.flags & SDL_GL_CONTEXT_DEBUG_FLAG) != 0;
    const bool forward = (sdlop_glattrs.flags & SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG) != 0;

    HGLRC ctx = NULL;
    /* The attribs path is also required whenever a profile is requested. An
     * OpenGL ES request is major 2 with a non-core profile, so testing only
     * "major > 2 || core" sent it down the legacy wglCreateContext branch,
     * where the profile mask cannot be expressed at all -- the driver then
     * hands back a desktop context and nothing reports an error. */
    if (wgl.create_context_attribs && (major > 2 || profile != 0)) {
        int attribs[16];
        int n = 0;
        attribs[n++] = WGL_CONTEXT_MAJOR_VERSION_ARB;
        attribs[n++] = major;
        attribs[n++] = WGL_CONTEXT_MINOR_VERSION_ARB;
        attribs[n++] = minor;
        if (profile) {
            /* Pass the mask through. Collapsing everything that is not CORE to
             * COMPATIBILITY, as this used to, silently answered an OpenGL ES
             * request with a desktop compatibility context: an app asking for
             * ES 2.0 was handed "GL 4.5 (Compatibility Profile)" with no
             * error. Measured. */
            attribs[n++] = WGL_CONTEXT_PROFILE_MASK_ARB;
            attribs[n++] = profile;
        }
        if (debug || forward) {
            attribs[n++] = WGL_CONTEXT_FLAGS_ARB;
            attribs[n++] = (debug ? WGL_CONTEXT_DEBUG_BIT_ARB : 0) |
                           (forward ? WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB : 0);
        }
        attribs[n++] = 0;
        HGLRC share = sdlop_glattrs.share_with_current_context ? wglGetCurrentContext() : NULL;
        ctx = wgl.create_context_attribs(hdc, share, attribs);
        if (!ctx) {
            if (profile != 0) {
                /* Do not substitute a desktop context for a profile request.
                 * The legacy wglCreateContext cannot express a profile, so the
                 * result would be a plain compatibility context handed back as
                 * if it were what was asked for -- an OpenGL ES app would get
                 * "GL 4.5 (Compatibility Profile)" and only discover the
                 * mismatch when its shaders fail. Measured on llvmpipe, whose
                 * WGL exposes no ES2 profile bit. Stock SDL3 fails here too
                 * ("Could not create GL context") rather than downgrading. */
                ReleaseDC(data->hwnd, hdc);
                SDL_SetError("Could not create a GL context with the requested profile "
                             "(the driver does not support it)");
                return NULL;
            }
            /* A driver that cannot give the requested version still gives a
             * legacy context; SDL3 falls back the same way. */
            ctx = wglCreateContext(hdc);
        }
    } else {
        /* No ARB attribs entry point at all (e.g. Win7's GDI Generic driver):
         * wglCreateContext cannot express a profile, so handing back a desktop
         * context for an ES request would be the same silent downgrade the
         * attribs branch refuses above. Report the unsupported profile. */
        if (profile != 0) {
            ReleaseDC(data->hwnd, hdc);
            SDL_SetError("Could not create a GL context with the requested profile "
                         "(the driver does not support WGL_ARB_create_context)");
            return NULL;
        }
        ctx = wglCreateContext(hdc);
    }
    if (!ctx) {
        ReleaseDC(data->hwnd, hdc);
        SDL_SetError("wglCreateContext() failed: %lu", (unsigned long)GetLastError());
        return NULL;
    }

    /* Bind it just long enough to load the extension entry points, then put
     * the caller's previous context back. SDL_GL_MakeCurrent does the real
     * binding. */
    const HDC prev_dc = wglGetCurrentDC();
    const HGLRC prev_ctx = wglGetCurrentContext();
    if (wglMakeCurrent(hdc, ctx)) {
        if (!wgl.initialized) {
            win32_gl_init_extensions();
        }
        if (wgl.swap_interval) {
            wgl.swap_interval(wgl.current_interval);
        }
        wglMakeCurrent(prev_dc, prev_ctx);
    } else {
        wglDeleteContext(ctx);
        ReleaseDC(data->hwnd, hdc);
        SDL_SetError("wglMakeCurrent() failed");
        return NULL;
    }

    if (!data->gl_context_owned && !data->gl_context) {
        data->gl_context = ctx;
        data->gl_context_owned = true;
    }
    return (void *)ctx;
}

bool SDLOP_Win32GLMakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context)
{
    (void)device;
    if (!context) {
        if (!wglMakeCurrent(NULL, NULL)) {
            return SDL_SetError("wglMakeCurrent(NULL) failed");
        }
        return true;
    }
    if (!window || !window->driverdata) {
        return SDL_SetError("No window to make current on");
    }
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data->hwnd) {
        return SDL_SetError("Window has no HWND");
    }
    HDC hdc = GetDC(data->hwnd);
    if (!hdc) {
        return SDL_SetError("GetDC() failed");
    }
    if (!wglMakeCurrent(hdc, (HGLRC)context)) {
        ReleaseDC(data->hwnd, hdc);
        return SDL_SetError("wglMakeCurrent() failed: %lu", (unsigned long)GetLastError());
    }
    /* The context keeps using the DC it was made current on, but the acquired
     * HDC itself must be released or every MakeCurrent leaks a DC. Render
     * calls re-acquire with GetCurrentDC(). */
    ReleaseDC(data->hwnd, hdc);
    return true;
}

bool SDLOP_Win32GLSwapBuffers(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = window ? (SDLop_Win32WindowData *)window->driverdata : NULL;
    if (!data || !data->hwnd) {
        return SDL_SetError("Window has no HWND");
    }
    HDC hdc = GetDC(data->hwnd);
    if (!hdc) {
        return SDL_SetError("GetDC() failed");
    }
    if (!SwapBuffers(hdc)) {
        ReleaseDC(data->hwnd, hdc);
        return SDL_SetError("SwapBuffers() failed: %lu", (unsigned long)GetLastError());
    }
    ReleaseDC(data->hwnd, hdc);
    return true;
}

void SDLOP_Win32GLDeleteContext(SDLop_VideoDevice *device, void *context)
{
    (void)device;
    if (!context) {
        return;
    }
    if (wglGetCurrentContext() == (HGLRC)context) {
        wglMakeCurrent(NULL, NULL);
    }
    wglDeleteContext((HGLRC)context);
}

SDL_FunctionPointer SDLOP_Win32GLGetProcAddress(SDLop_VideoDevice *device, const char *proc)
{
    (void)device;
    if (!proc) {
        return NULL;
    }
    return (SDL_FunctionPointer)win32_gl_get_proc(proc);
}

bool SDLOP_Win32GLSetSwapInterval(SDLop_VideoDevice *device, int interval)
{
    (void)device;
    /* -1 is adaptive vsync, and it is legal only where the driver advertises
     * WGL_EXT_swap_control_tear; anything else negative is a caller error.
     * Stock SDL3 accepts -1 and lets wglSwapIntervalEXT reject it. */
    if (interval < -1) {
        return SDL_SetError("Negative swap interval");
    }
    if (!wgl.swap_interval) {
        win32_gl_bootstrap_proc_addresses();
    }
    if (!wgl.swap_interval) {
        /* WGL_EXT_swap_control is not there: report the failure the way
         * SDL3 does rather than pretending it worked */
        return SDL_SetError("Setting the swap interval is not supported");
    }
    if (interval == -1 && !wgl.has_swap_control_tear) {
        /* don't hand -1 to a driver that never advertised tearing support:
         * several of them answer TRUE and quietly leave the interval at 0,
         * which is not adaptive vsync and cannot be detected afterwards */
        return SDL_SetError("Adaptive vsync (interval -1) needs WGL_EXT_swap_control_tear, which this driver does not support");
    }
    if (!wgl.swap_interval(interval)) {
        return SDL_SetError("wglSwapIntervalEXT() failed");
    }
    wgl.current_interval = interval;
    return true;
}

bool SDLOP_Win32GLGetSwapInterval(SDLop_VideoDevice *device, int *interval)
{
    (void)device;
    if (!wgl.get_swap_interval) {
        win32_gl_bootstrap_proc_addresses();
    }
    if (!wgl.get_swap_interval) {
        if (interval) {
            *interval = 0;
        }
        return true;
    }
    if (interval) {
        *interval = wgl.get_swap_interval();
    }
    return true;
}

#endif /* _WIN32 */
