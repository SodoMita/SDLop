/*
  SDLop - the names POSIX provides and Windows does not (see sdlop_win32.h).

  Each function matches the POSIX/BSD behaviour SDLop relies on; MinGW-w64
  already supplies some of them, so this file only defines what is missing and
  leaves the rest alone. strcasecmp/strncasecmp are implemented here rather
  than mapped to _stricmp so the comparison is byte-wise like POSIX, not
  locale-aware.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#ifdef _WIN32

#include <ctype.h>
#include <errno.h>
#include <io.h>
#include <wctype.h>

/* MinGW declares these itself (see sdlop_win32.h); defining them again would
 * both clash with its macro and give a different comparison. */
#if !defined(__MINGW32__) && !defined(_MSC_VER)
int strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        const int ca = tolower((unsigned char)*a);
        const int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *b) {
        const int ca = tolower((unsigned char)*a);
        const int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
#endif /* !__MINGW32__ (strcasecmp/strncasecmp come from MinGW) */

int wcscasecmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && *b) {
        const int ca = towlower((unsigned int)*a);
        const int cb = towlower((unsigned int)*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return (int)towlower((unsigned int)*a) - (int)towlower((unsigned int)*b);
}

int wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    while (n && *a && *b) {
        const int ca = towlower((unsigned int)*a);
        const int cb = towlower((unsigned int)*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)towlower((unsigned int)*a) - (int)towlower((unsigned int)*b);
}

char *strcasestr(const char *haystack, const char *needle)
{
    if (!*needle) {
        return (char *)haystack;
    }
    const size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) {
            return (char *)p;
        }
    }
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (!name || !*name || strchr(name, '=') || !value) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && getenv(name)) {
        return 0;
    }
    /* _putenv_s copies both strings, unlike _putenv. It returns an errno_t,
     * not the -1 POSIX callers test for, so translate. */
    if (_putenv_s(name, value) != 0) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int unsetenv(const char *name)
{
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    /* _putenv_s(name, "") *deletes* name on the MSVCRT/UCRT, which is what
     * unsetenv wants -- so that part is correct as it stands. */
    if (_putenv_s(name, "") != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* MinGW has no qsort_r; a plain merge sort keeps the comparator contract
 * exactly (a stable sort is allowed, and cheaper to get right than
 * forwarding through qsort_s's reversed argument order). */
void qsort_r(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *, void *), void *arg)
{
    if (!base || !compar || !size || nmemb < 2) {
        return;
    }
    unsigned char *a = (unsigned char *)base;
    unsigned char *tmp = (unsigned char *)SDL_malloc(nmemb * size);
    if (!tmp) {
        /* out of memory: fall back to insertion sort, still correct */
        for (size_t i = 1; i < nmemb; i++) {
            for (size_t j = i; j > 0 && compar(a + (j - 1) * size, a + j * size, arg) > 0; j--) {
                unsigned char swap[256];
                if (size <= sizeof(swap)) {
                    SDL_memcpy(swap, a + (j - 1) * size, size);
                    SDL_memcpy(a + (j - 1) * size, a + j * size, size);
                    SDL_memcpy(a + j * size, swap, size);
                } else {
                    for (size_t k = 0; k < size; k++) {
                        const unsigned char t = a[(j - 1) * size + k];
                        a[(j - 1) * size + k] = a[j * size + k];
                        a[j * size + k] = t;
                    }
                }
            }
        }
        return;
    }
    for (size_t width = 1; width < nmemb; width *= 2) {
        for (size_t i = 0; i < nmemb; i += 2 * width) {
            const size_t mid = (i + width < nmemb) ? i + width : nmemb;
            const size_t end = (i + 2 * width < nmemb) ? i + 2 * width : nmemb;
            size_t l = i, r = mid, k = i;
            while (l < mid && r < end) {
                if (compar(a + l * size, a + r * size, arg) <= 0) {
                    SDL_memcpy(tmp + k * size, a + l * size, size);
                    l++;
                } else {
                    SDL_memcpy(tmp + k * size, a + r * size, size);
                    r++;
                }
                k++;
            }
            while (l < mid) {
                SDL_memcpy(tmp + k * size, a + l * size, size);
                l++;
                k++;
            }
            while (r < end) {
                SDL_memcpy(tmp + k * size, a + r * size, size);
                r++;
                k++;
            }
            SDL_memcpy(a + i * size, tmp + i * size, (end - i) * size);
        }
    }
    SDL_free(tmp);
}

int unlink(const char *path)
{
    return _unlink(path);
}

int rmdir(const char *path)
{
    return _rmdir(path);
}

int access(const char *path, int mode)
{
    /* _access only knows existence/read/write; POSIX X_OK on Windows is
     * "is it a directory or does it have an extension the shell runs" and
     * SDL does not depend on it, so treat X_OK as F_OK. */
    if (mode & (W_OK)) {
        return _access(path, 2);
    }
    if (mode & R_OK) {
        return _access(path, 4);
    }
    return _access(path, 0);
}

int isatty(int fd)
{
    return _isatty(fd);
}

/* The optional APIs below are resolved once, thread-safely, with InitOnce
 * (available since Win7). A missing export resolves to a NULL pointer and
 * keeps the graceful fallback -- the one-time init still completes, so later
 * calls do not repeat the lookup. */

/* SetThreadDescription is Windows 10 1607+; Win7 has no such export. Resolve
 * it lazily from kernel32 so the same binary keeps the nicer debugger thread
 * names on newer systems and silently no-ops on older ones. SDL3 does the
 * same dynamic lookup for this API. */
typedef HRESULT (WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);
static SetThreadDescriptionFn set_description_fn = NULL;

static BOOL CALLBACK resolve_set_thread_description(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    union { FARPROC raw; SetThreadDescriptionFn fn; } resolved_fn;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    (void)once;
    (void)parameter;
    (void)context;
    resolved_fn.raw = kernel32 ? GetProcAddress(kernel32, "SetThreadDescription") : NULL;
    set_description_fn = resolved_fn.fn;
    return TRUE;
}

HRESULT SDLOP_SetThreadDescription(HANDLE thread, const wchar_t *name)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, resolve_set_thread_description, NULL, NULL);
    if (set_description_fn) {
        return set_description_fn(thread, name);
    }
    return E_NOTIMPL;
}

/* Per-monitor DPI support is absent on Windows 7. Resolving these from
 * user32.dll avoids a hard import that would stop SDLop.dll from loading at
 * all there; callers already handle a 0 DPI / FALSE result. */
typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
static GetDpiForWindowFn get_dpi_for_window_fn = NULL;

static BOOL CALLBACK resolve_get_dpi_for_window(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    union { FARPROC raw; GetDpiForWindowFn fn; } resolved_fn;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    (void)once;
    (void)parameter;
    (void)context;
    resolved_fn.raw = user32 ? GetProcAddress(user32, "GetDpiForWindow") : NULL;
    get_dpi_for_window_fn = resolved_fn.fn;
    return TRUE;
}

UINT SDLOP_GetDpiForWindow(HWND hwnd)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, resolve_get_dpi_for_window, NULL, NULL);
    return get_dpi_for_window_fn ? get_dpi_for_window_fn(hwnd) : 0;
}

typedef UINT (WINAPI *GetDpiForSystemFn)(void);
static GetDpiForSystemFn get_dpi_for_system_fn = NULL;

static BOOL CALLBACK resolve_get_dpi_for_system(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    union { FARPROC raw; GetDpiForSystemFn fn; } resolved_fn;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    (void)once;
    (void)parameter;
    (void)context;
    resolved_fn.raw = user32 ? GetProcAddress(user32, "GetDpiForSystem") : NULL;
    get_dpi_for_system_fn = resolved_fn.fn;
    return TRUE;
}

UINT SDLOP_GetDpiForSystem(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, resolve_get_dpi_for_system, NULL, NULL);
    return get_dpi_for_system_fn ? get_dpi_for_system_fn() : 0;
}

typedef BOOL (WINAPI *AdjustWindowRectExForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
static AdjustWindowRectExForDpiFn adjust_window_rect_ex_for_dpi_fn = NULL;

static BOOL CALLBACK resolve_adjust_window_rect_ex_for_dpi(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    union { FARPROC raw; AdjustWindowRectExForDpiFn fn; } resolved_fn;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    (void)once;
    (void)parameter;
    (void)context;
    resolved_fn.raw = user32 ? GetProcAddress(user32, "AdjustWindowRectExForDpi") : NULL;
    adjust_window_rect_ex_for_dpi_fn = resolved_fn.fn;
    return TRUE;
}

BOOL SDLOP_AdjustWindowRectExForDpi(LPRECT rect, DWORD style, BOOL menu, DWORD exstyle, UINT dpi)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, resolve_adjust_window_rect_ex_for_dpi, NULL, NULL);
    if (adjust_window_rect_ex_for_dpi_fn) {
        return adjust_window_rect_ex_for_dpi_fn(rect, style, menu, exstyle, dpi);
    }
    return AdjustWindowRectEx(rect, style, menu, exstyle);
}

/* GetDpiForMonitor lives in shcore.dll (Win8.1+) and has no Win7 equivalent.
 * Display enumeration uses it when available; otherwise the system DPI is a
 * reasonable single-monitor approximation. */
#ifndef MDT_EFFECTIVE_DPI
#define MDT_EFFECTIVE_DPI 0
#endif
typedef HRESULT(WINAPI *GetDpiForMonitorFn)(HMONITOR, UINT, UINT *, UINT *);
static GetDpiForMonitorFn get_dpi_for_monitor_fn = NULL;

static BOOL CALLBACK resolve_get_dpi_for_monitor(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    union { FARPROC raw; GetDpiForMonitorFn fn; } resolved_fn;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    (void)once;
    (void)parameter;
    (void)context;
    resolved_fn.raw = shcore ? GetProcAddress(shcore, "GetDpiForMonitor") : NULL;
    /* Keep shcore loaded for the process lifetime: a function pointer into a
     * freed DLL would crash on the next display refresh. */
    get_dpi_for_monitor_fn = resolved_fn.fn;
    return TRUE;
}

BOOL SDLOP_GetDpiForMonitor(HMONITOR monitor, UINT dpiType, UINT *dpiX, UINT *dpiY)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, resolve_get_dpi_for_monitor, NULL, NULL);
    if (!get_dpi_for_monitor_fn || !monitor || !dpiX || !dpiY) {
        return FALSE;
    }
    return get_dpi_for_monitor_fn(monitor, dpiType, dpiX, dpiY) == S_OK;
}

#endif /* _WIN32 */
