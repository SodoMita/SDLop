/*
  SDLop - the handful of names POSIX gives C for free and Windows does not.

  SDLop is written against a POSIX libc: strcasecmp, strncasecmp, strdup,
  ssize_t, and the <unistd.h> spelling of "is this the same file". Rather than
  rewrite every call site (and risk changing behaviour on Linux), this header
  defines those names for Windows only. It is included by sdlop_internal.h
  before anything else that might pull in <windows.h>.

  Including this header on POSIX is a no-op, so a stray include never breaks a
  Linux build.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_win32_h_
#define sdlop_win32_h_

#ifndef _WIN32

/* POSIX: nothing to do. */

#else

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* <windows.h> defines CreateWindow/CreateWindowEx as macros, which collide
 * with the SDLop_VideoDevice members of the same name. SDL3 solves this with
 * UNICODE + a pile of #undefs; one #undef here covers every use. */
#ifdef CreateWindow
#undef CreateWindow
#endif
#ifdef CreateWindowEx
#undef CreateWindowEx
#endif
#ifdef LoadImage
#undef LoadImage
#endif

/* Windows 10 APIs that SDLop uses opportunistically. Windows 7 has neither,
 * so the call sites must keep working when the SDK does not declare them:
 * CreateWaitableTimerExW simply fails on Win7 (the sleep code already falls
 * back to CreateWaitableTimerW), and SetThreadDescription is resolved at run
 * time by sdlop_win32_compat.c. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000004
#endif
HRESULT SDLOP_SetThreadDescription(HANDLE thread, const wchar_t *name);
/* Per-monitor DPI APIs (Windows 10 1607+) are likewise optional: the window
 * code falls back to the classic system-DPI AdjustWindowRectEx on Win7. */
UINT SDLOP_GetDpiForWindow(HWND hwnd);
UINT SDLOP_GetDpiForSystem(void);
BOOL SDLOP_AdjustWindowRectExForDpi(LPRECT rect, DWORD style, BOOL menu, DWORD exstyle, UINT dpi);
/* shcore.dll (Win8.1+): per-monitor DPI for display enumeration. Returns
 * FALSE on older systems; callers fall back to the system DPI. */
BOOL SDLOP_GetDpiForMonitor(HMONITOR monitor, UINT dpiType, UINT *dpiX, UINT *dpiY);

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX typedefs: MinGW's sys/types.h already provides them; only define
 * them for a toolchain that does not. */
#ifndef _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#endif
#ifndef S_IWUSR
#define S_IWUSR _S_IWRITE
#endif
#ifndef S_IXUSR
#define S_IXUSR _S_IEXEC
#endif

/* <strings.h>. MinGW already declares strcasecmp/strncasecmp (as macros over
 * _stricmp/_strnicmp when not in strict ISO mode), so redeclaring them here
 * only drops the dllimport attribute and draws -Wattributes. Declare the ones
 * it really lacks, which sdlop_win32_compat.c then defines. */
#if !defined(__MINGW32__) && !defined(_MSC_VER)
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
#endif
int wcscasecmp(const wchar_t *a, const wchar_t *b);
int wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n);
char *strcasestr(const char *haystack, const char *needle);

/* stdlib.h POSIX additions */
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);

/* qsort_r with the glibc argument order (base, n, size, cmp, arg) */
void qsort_r(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *, void *), void *arg);

/* <unistd.h> spellings */
char *strdup(const char *s);
int unlink(const char *path);
int rmdir(const char *path);
int access(const char *path, int mode);
int isatty(int fd);
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

/* POSIX integer formatters SDL uses via SDL_PRI*; keep names available */
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

/* POSIX file-descriptor I/O over Win32 handles (SDL_io) */
typedef intptr_t sdlop_fd_t;

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */

#endif /* sdlop_win32_h_ */
