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
    /* _putenv_s copies both strings, unlike _putenv */
    return _putenv_s(name, value);
}

int unsetenv(const char *name)
{
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    return _putenv_s(name, "");
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

#endif /* _WIN32 */
