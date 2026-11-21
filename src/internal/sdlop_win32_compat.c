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
#include <io.h>

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
