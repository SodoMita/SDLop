/*
  SDLop - filesystem helpers (SDL_filesystem.h).

  Not windowing, but applications built against stock SDL3 commonly locate
  their data and save files with SDL_GetBasePath() / SDL_GetPrefPath(), so
  a drop-in replacement has to export them. Implemented for POSIX exactly
  like stock SDL3's unix backend; SDL_GlobDirectory is not provided.

  Ported from SDL 3.4.20 (src/filesystem/SDL_filesystem.c,
  src/filesystem/unix/SDL_sysfilesystem.c, src/filesystem/posix/
  SDL_sysfsops.c), (C) Sam Lantinga, zlib license. The XDG user-dirs
  lookup follows xdg-user-dir-lookup.c, (C) 2007 Red Hat, Inc., MIT license.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define SDL_MAIN_NOIMPL

#include "internal/sdlop_internal.h"

#include <SDL3/SDL_filesystem.h>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <shlobj.h> /* SHGetKnownFolderPath, SHCreateDirectoryExW */

/* UTF-8 <-> UTF-16 for the Win32 wide-character file APIs */
static wchar_t *fs_utf8_to_wide(const char *utf8)
{
    if (!utf8) {
        return NULL;
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    wchar_t *wide = (wchar_t *)SDL_malloc((size_t)n * sizeof(wchar_t));
    if (wide && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, n) <= 0) {
        SDL_free(wide);
        return NULL;
    }
    return wide;
}

/* Windows environment variables are ANSI (the process ACP), not UTF-8. On a
 * localized profile ("john.123-\u041f\u041a") copying %APPDATA% verbatim made
 * the later CP_UTF8 conversion fail with ERROR_INVALID_NAME. */
static char *fs_acp_to_utf8(const char *ansi)
{
    if (!ansi) {
        return NULL;
    }
    const int wlen = MultiByteToWideChar(CP_ACP, 0, ansi, -1, NULL, 0);
    if (wlen <= 0) {
        return NULL;
    }
    wchar_t *wide = (wchar_t *)SDL_malloc((size_t)wlen * sizeof(wchar_t));
    if (!wide || MultiByteToWideChar(CP_ACP, 0, ansi, -1, wide, wlen) <= 0) {
        SDL_free(wide);
        return NULL;
    }
    const int ulen = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *utf8 = ulen > 0 ? (char *)SDL_malloc((size_t)ulen) : NULL;
    if (utf8 && WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, ulen, NULL, NULL) <= 0) {
        SDL_free(utf8);
        utf8 = NULL;
    }
    SDL_free(wide);
    return utf8;
}

static char *fs_wide_to_utf8(const wchar_t *wide)
{
    if (!wide) {
        return NULL;
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (n <= 0) {
        return NULL;
    }
    char *utf8 = (char *)SDL_malloc((size_t)n);
    if (utf8 && WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, n, NULL, NULL) <= 0) {
        SDL_free(utf8);
        return NULL;
    }
    return utf8;
}

/* stock SDL3 returns paths with '/' separators on every platform */
static void fs_to_forward_slashes(char *path)
{
    for (; path && *path; path++) {
        if (*path == '\\') {
            *path = '/';
        }
    }
}

static Uint64 fs_filetime_to_ns(const FILETIME *ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    /* 100 ns ticks since 1601: SDL_Time counts from the Unix epoch */
    const Uint64 ticks = (Uint64)u.QuadPart;
    const Uint64 unix_ticks = ticks > 116444736000000000ULL ? ticks - 116444736000000000ULL : 0;
    return unix_ticks * 100ULL;
}

/* an SDL directory path ("a/b/") as a Win32 glob ("a\b\*"): CREATE takes a
 * directory, ENUMERATE takes everything inside it */
static wchar_t *fs_utf8_dir_to_wide(const char *path)
{
    size_t len = strlen(path);
    char *tmp = (char *)SDL_malloc(len + 3);
    if (!tmp) {
        return NULL;
    }
    memcpy(tmp, path, len + 1);
    while (len > 0 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    tmp[len] = '/';
    tmp[len + 1] = '*';
    tmp[len + 2] = '\0';
    wchar_t *wide = fs_utf8_to_wide(tmp);
    SDL_free(tmp);
    if (wide) {
        for (wchar_t *p = wide; *p; p++) {
            if (*p == L'/') {
                *p = L'\\';
            }
        }
    }
    return wide;
}
#endif

static SDLOP_STATIC_MUTEX(fs_lock);
static char *cached_base_path;
static char *cached_user_folders[SDL_FOLDER_COUNT];

/* ------------------------------------------------------------------ */
/* Base / pref path                                                    */
/* ------------------------------------------------------------------ */

static char *read_exe_path(void)
{
#ifdef _WIN32
    DWORD len = 256;
    for (;;) {
        wchar_t *wbuf = (wchar_t *)SDL_malloc((size_t)len * sizeof(wchar_t));
        if (!wbuf) {
            return NULL;
        }
        const DWORD n = GetModuleFileNameW(NULL, wbuf, len);
        if (n == 0) {
            SDL_free(wbuf);
            SDL_SetError("Couldn't get the executable path: %lu", (unsigned long)GetLastError());
            return NULL;
        }
        if (n < len - 1) {
            char *utf8 = fs_wide_to_utf8(wbuf);
            SDL_free(wbuf);
            fs_to_forward_slashes(utf8);
            return utf8;
        }
        /* truncated: the buffer is too small */
        SDL_free(wbuf);
        len *= 2;
        if (len > 65536) {
            SDL_SetError("Executable path too long");
            return NULL;
        }
    }
#else
    size_t len = 256;
    for (;;) {
        char *buf = (char *)SDL_malloc(len);
        if (!buf) {
            return NULL;
        }
        ssize_t n = readlink("/proc/self/exe", buf, len);
        if (n < 0) {
            SDL_free(buf);
            SDL_SetError("Couldn't read /proc/self/exe: %s", strerror(errno));
            return NULL;
        }
        if ((size_t)n < len) {
            buf[n] = '\0';
            return buf;
        }
        SDL_free(buf);
        len *= 2;
        if (len > 65536) {
            SDL_SetError("Executable path too long");
            return NULL;
        }
    }
#endif /* _WIN32 */
}

const char *SDL_GetBasePath(void)
{
    SDLOP_MUTEX_LOCK(SDLOP_SM(&fs_lock));
    if (!cached_base_path) {
        char *path = read_exe_path();
        if (path) {
            char *slash = strrchr(path, '/');
            if (slash) {
                slash[1] = '\0'; /* keep the trailing '/', as stock SDL */
                cached_base_path = path;
            } else {
                SDL_free(path);
                SDL_SetError("Executable path is not absolute");
            }
        }
    }
    const char *result = cached_base_path;
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&fs_lock));
    return result;
}

char *SDL_GetPrefPath(const char *org, const char *app)
{
    if (!app) {
        SDL_InvalidParamError("app");
        return NULL;
    }
    if (!org) {
        org = "";
    }

#ifdef _WIN32
    /* SDL3 on Windows: %APPDATA%\org\app\ (FOLDERID_RoamingAppData) */
    PWSTR roaming = NULL;
    char *envr;
    /* DONT_VERIFY avoids an intermittent Win7 Shell existence check failure
     * for the localized profile path. A failed call here used to fall back to
     * the ANSI-encoded %APPDATA%, which was then misread as UTF-8 and made
     * SDL_CreateDirectory fail with ERROR_INVALID_NAME (123). */
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, KF_FLAG_CREATE | KF_FLAG_DONT_VERIFY, NULL, &roaming))) {
        envr = fs_wide_to_utf8(roaming);
        CoTaskMemFree(roaming);
        if (!envr) {
            SDL_OutOfMemory();
            return NULL;
        }
    } else {
        const char *appdata = SDL_getenv("APPDATA");
        envr = fs_acp_to_utf8(appdata);
        if (!envr) {
            SDL_SetError("Couldn't find the Roaming AppData folder");
            return NULL;
        }
    }
    fs_to_forward_slashes(envr);
    const size_t elen = strlen(envr);
    const bool needs_slash = elen > 0 && envr[elen - 1] != '/';
    const size_t len = elen + strlen(org) + strlen(app) + 8;
    char *result = (char *)SDL_malloc(len);
    if (result) {
        if (*org) {
            (void)SDL_snprintf(result, len, "%s%s%s/%s/", envr, needs_slash ? "/" : "", org, app);
        } else {
            (void)SDL_snprintf(result, len, "%s%s%s/", envr, needs_slash ? "/" : "", app);
        }
        if (!SDL_CreateDirectory(result)) {
            SDL_free(result);
            result = NULL;
        }
    }
    SDL_free(envr);
    return result;
#else
    /* XDG base directory spec: $XDG_DATA_HOME/org/app/ or
     * $HOME/.local/share/org/app/ */
    const char *envr = SDL_getenv("XDG_DATA_HOME");
    const char *append;
    if (!envr) {
        envr = SDL_getenv("HOME");
        if (!envr) {
            SDL_SetError("neither XDG_DATA_HOME nor HOME environment is set");
            return NULL;
        }
        append = "/.local/share/";
    } else {
        append = "/";
    }
    size_t len = strlen(envr);
    if (len > 0 && envr[len - 1] == '/') {
        append += 1;
    }
    len += strlen(append) + strlen(org) + strlen(app) + 3;
    char *result = (char *)SDL_malloc(len);
    if (!result) {
        return NULL;
    }
    if (*org) {
        (void)SDL_snprintf(result, len, "%s%s%s/%s/", envr, append, org, app);
    } else {
        (void)SDL_snprintf(result, len, "%s%s%s/", envr, append, app);
    }

    for (char *ptr = result + 1; *ptr; ptr++) {
        if (*ptr == '/') {
            *ptr = '\0';
            if (mkdir(result, 0700) != 0 && errno != EEXIST) {
                SDL_SetError("Couldn't create directory '%s': '%s'", result, strerror(errno));
                SDL_free(result);
                return NULL;
            }
            *ptr = '/';
        }
    }
    if (mkdir(result, 0700) != 0 && errno != EEXIST) {
        SDL_SetError("Couldn't create directory '%s': '%s'", result, strerror(errno));
        SDL_free(result);
        return NULL;
    }
    return result;
#endif /* _WIN32 */
}

/* ------------------------------------------------------------------ */
/* User folders (XDG user-dirs)                                        */
/* ------------------------------------------------------------------ */

#ifndef _WIN32
static char *xdg_user_dir_from_config(const char *type)
{
    const char *home = SDL_getenv("HOME");
    if (!home) {
        return NULL;
    }
    char config_file[4096];
    const char *config_home = SDL_getenv("XDG_CONFIG_HOME");
    if (config_home && *config_home) {
        (void)SDL_snprintf(config_file, sizeof(config_file), "%s/user-dirs.dirs", config_home);
    } else {
        (void)SDL_snprintf(config_file, sizeof(config_file), "%s/.config/user-dirs.dirs", home);
    }
    FILE *file = fopen(config_file, "r");
    if (!file) {
        return NULL;
    }

    char *user_dir = NULL;
    char buffer[512];
    const size_t type_len = strlen(type);
    while (fgets(buffer, sizeof(buffer), file)) {
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        char *p = buffer;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, "XDG_", 4) != 0) {
            continue;
        }
        p += 4;
        if (strncmp(p, type, type_len) != 0) {
            continue;
        }
        p += type_len;
        if (strncmp(p, "_DIR", 4) != 0) {
            continue;
        }
        p += 4;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != '=') {
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != '"') {
            continue;
        }
        p++;
        bool relative = false;
        if (strncmp(p, "$HOME/", 6) == 0) {
            p += 6;
            relative = true;
        } else if (*p != '/') {
            continue;
        }

        SDL_free(user_dir);
        size_t l = (relative ? strlen(home) + 1 : 0) + strlen(p) + 1;
        user_dir = (char *)SDL_malloc(l);
        if (!user_dir) {
            break;
        }
        if (relative) {
            (void)SDL_snprintf(user_dir, l, "%s/", home);
        } else {
            *user_dir = '\0';
        }
        char *d = user_dir + strlen(user_dir);
        while (*p && *p != '"') {
            if (*p == '\\' && p[1] != '\0') {
                p++;
            }
            *d++ = *p++;
        }
        *d = '\0';
    }
    fclose(file);
    return user_dir;
}
#endif /* !_WIN32 */

#ifndef _WIN32
static char *xdg_user_dir_lookup(const char *type)
{
    char *dir = xdg_user_dir_from_config(type);
    if (dir) {
        return dir;
    }
    /* historical fallback: only the desktop folder has a default */
    const char *home = SDL_getenv("HOME");
    if (home && strcmp(type, "DESKTOP") == 0) {
        size_t l = strlen(home) + sizeof("/Desktop");
        dir = (char *)SDL_malloc(l);
        if (dir) {
            (void)SDL_snprintf(dir, l, "%s/Desktop", home);
        }
        return dir;
    }
    return NULL;
}
#endif /* !_WIN32 */

static char *lookup_user_folder(SDL_Folder folder)
{
    char *result = NULL;
#ifndef _WIN32
    const char *param = NULL;
#endif

#ifdef _WIN32
    /* Windows has no XDG: ask the Shell for the real folder. Before this the
     * Win32 build fell through to the XDG code below, so every folder came
     * from $HOME/.config/user-dirs.dirs (absent on Windows) and Saved
     * Games / Screenshots reported "unavailable on XDG". */
    {
        static const struct {
            SDL_Folder folder;
            const GUID *id;
        } known[] = {
            { SDL_FOLDER_HOME,        &FOLDERID_Profile },
            { SDL_FOLDER_DESKTOP,     &FOLDERID_Desktop },
            { SDL_FOLDER_DOCUMENTS,   &FOLDERID_Documents },
            { SDL_FOLDER_DOWNLOADS,   &FOLDERID_Downloads },
            { SDL_FOLDER_MUSIC,       &FOLDERID_Music },
            { SDL_FOLDER_PICTURES,    &FOLDERID_Pictures },
            { SDL_FOLDER_PUBLICSHARE, &FOLDERID_Public },
            { SDL_FOLDER_TEMPLATES,   &FOLDERID_Templates },
            { SDL_FOLDER_VIDEOS,      &FOLDERID_Videos },
            { SDL_FOLDER_SAVEDGAMES,  &FOLDERID_SavedGames },
            { SDL_FOLDER_SCREENSHOTS, &FOLDERID_Screenshots },
        };
        for (size_t i = 0; i < SDL_arraysize(known); i++) {
            if (known[i].folder != folder) {
                continue;
            }
            PWSTR wide = NULL;
            /* KF_FLAG_DONT_VERIFY: the Shell's existence check can fail with
             * ERROR_FILE_NOT_FOUND on Win7 for a localized profile path even
             * though the directory exists (measured under ctest). Returning
             * the configured path is the useful behaviour for an app. */
            const HRESULT hr = SHGetKnownFolderPath(known[i].id, KF_FLAG_DONT_VERIFY, NULL, &wide);
            if (FAILED(hr)) {
                SDL_SetError("Couldn't find folder %d (HRESULT 0x%08lx)", (int)folder, (unsigned long)hr);
                return NULL;
            }
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
            if (bytes > 0) {
                result = (char *)SDL_malloc((size_t)bytes + 1);
                if (result) {
                    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, result, bytes, NULL, NULL) <= 0) {
                        SDL_free(result);
                        result = NULL;
                    }
                }
            }
            CoTaskMemFree(wide);
            if (!result) {
                return NULL;
            }
            /* SDL reports folders with a trailing separator */
            for (char *p = result; *p; p++) {
                if (*p == '\\') {
                    *p = '/';
                }
            }
            const size_t l = SDL_strlen(result);
            if (l == 0 || result[l - 1] != '/') {
                char *with_slash = (char *)SDL_realloc(result, l + 2);
                if (!with_slash) {
                    SDL_free(result);
                    return NULL;
                }
                with_slash[l] = '/';
                with_slash[l + 1] = '\0';
                result = with_slash;
            }
            return result;
        }
        SDL_SetError("Invalid SDL_Folder: %d", (int)folder);
        return NULL;
    }
#else
    switch (folder) {
    case SDL_FOLDER_HOME: {
        const char *home = SDL_getenv("HOME");
        if (!home) {
            SDL_SetError("No $HOME environment variable available");
            return NULL;
        }
        result = SDL_strdup(home);
        break;
    }
    case SDL_FOLDER_DESKTOP:     param = "DESKTOP"; break;
    case SDL_FOLDER_DOCUMENTS:   param = "DOCUMENTS"; break;
    case SDL_FOLDER_DOWNLOADS:   param = "DOWNLOAD"; break;
    case SDL_FOLDER_MUSIC:       param = "MUSIC"; break;
    case SDL_FOLDER_PICTURES:    param = "PICTURES"; break;
    case SDL_FOLDER_PUBLICSHARE: param = "PUBLICSHARE"; break;
    case SDL_FOLDER_TEMPLATES:   param = "TEMPLATES"; break;
    case SDL_FOLDER_VIDEOS:      param = "VIDEOS"; break;
    case SDL_FOLDER_SAVEDGAMES:
        SDL_SetError("Saved Games folder unavailable on XDG");
        return NULL;
    case SDL_FOLDER_SCREENSHOTS:
        SDL_SetError("Screenshots folder unavailable on XDG");
        return NULL;
    default:
        SDL_SetError("Invalid SDL_Folder: %d", (int)folder);
        return NULL;
    }

    if (param) {
        result = xdg_user_dir_lookup(param);
        if (!result) {
            SDL_SetError("XDG directory not available");
            return NULL;
        }
    }
    if (!result) {
        return NULL;
    }
    size_t l = strlen(result);
    char *with_slash = (char *)SDL_realloc(result, l + 2);
    if (!with_slash) {
        SDL_free(result);
        return NULL;
    }
    with_slash[l] = '/';
    with_slash[l + 1] = '\0';
    return with_slash;
#endif /* !_WIN32 */
}

const char *SDL_GetUserFolder(SDL_Folder folder)
{
    const int idx = (int)folder;
    if (idx < 0 || idx >= SDL_FOLDER_COUNT) {
        SDL_InvalidParamError("folder");
        return NULL;
    }
    SDLOP_MUTEX_LOCK(SDLOP_SM(&fs_lock));
    if (!cached_user_folders[idx]) {
        cached_user_folders[idx] = lookup_user_folder(folder);
    }
    const char *result = cached_user_folders[idx];
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&fs_lock));
    return result;
}

/* Called from SDL_Quit (stock frees its caches in SDL_QuitFilesystem). */
void SDLOP_QuitFilesystem(void)
{
    SDLOP_MUTEX_LOCK(SDLOP_SM(&fs_lock));
    SDL_free(cached_base_path);
    cached_base_path = NULL;
    for (int i = 0; i < SDL_FOLDER_COUNT; i++) {
        SDL_free(cached_user_folders[i]);
        cached_user_folders[i] = NULL;
    }
    SDLOP_MUTEX_UNLOCK(SDLOP_SM(&fs_lock));
}

/* ------------------------------------------------------------------ */
/* Filesystem operations                                               */
/* ------------------------------------------------------------------ */

bool SDL_CreateDirectory(const char *path)
{
    if (!path) {
        return SDL_InvalidParamError("path");
    }
#ifdef _WIN32
    /* SHCreateDirectoryExW creates every missing parent and, unlike
     * CreateDirectoryW, succeeds when the directory already exists. It is a
     * Shell API, though: it wants backslashes and a full path. SDL paths are
     * forward-slashed and may be relative ("a/b"), which used to fail with
     * ERROR_BAD_PATHNAME -- including every SDL_GetPrefPath() caller. */
    wchar_t *wide = fs_utf8_to_wide(path);
    if (!wide) {
        return SDL_OutOfMemory();
    }
    for (wchar_t *w = wide; *w; w++) {
        if (*w == L'/') {
            *w = L'\\';
        }
    }
    /* SHCreateDirectoryExW on Win7 can return ERROR_INVALID_NAME (123) for a
     * path ending in a separator. SDL_GetPrefPath always appends one, so trim
     * it (but keep a drive root like "C:\\"). */
    for (size_t len = wcslen(wide); len > 3 && wide[len - 1] == L'\\'; len--) {
        wide[len - 1] = L'\0';
    }
    wchar_t full[32768]; /* Win32 long-path maximum */
    DWORD full_len = GetFullPathNameW(wide, (DWORD)SDL_arraysize(full), full, NULL);
    if (full_len > 0 && full_len < (DWORD)SDL_arraysize(full)) {
        SDL_free(wide);
        wide = (wchar_t *)SDL_malloc(((size_t)full_len + 1) * sizeof(wchar_t));
        if (!wide) {
            return SDL_OutOfMemory();
        }
        SDL_memcpy(wide, full, ((size_t)full_len + 1) * sizeof(wchar_t));
    }
    const int rc = SHCreateDirectoryExW(NULL, wide, NULL);
    SDL_free(wide);
    if (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS) {
        return true;
    }
    return SDL_SetError("Can't create directory '%s': error %d", path, rc);
#else
    /* stock SDL3 creates missing parents too */
    char *copy = SDL_strdup(path);
    if (!copy) {
        return false;
    }
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0770) != 0 && errno != EEXIST) {
                const int err = errno;
                SDL_free(copy);
                return SDL_SetError("Can't create directory: %s", strerror(err));
            }
            *p = '/';
        }
    }
    SDL_free(copy);
    if (mkdir(path, 0770) < 0) {
        const int err = errno;
        struct stat st;
        if (err == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return true; /* already a directory: success */
        }
        return SDL_SetError("Can't create directory: %s", strerror(err));
    }
    return true;
#endif /* _WIN32 */
}

bool SDL_EnumerateDirectory(const char *path, SDL_EnumerateDirectoryCallback callback, void *userdata)
{
    if (!path) {
        return SDL_InvalidParamError("path");
    }
    if (!callback) {
        return SDL_InvalidParamError("callback");
    }
    /* the callback's dirname always ends with exactly one '/' */
    size_t len = strlen(path);
#ifdef _WIN32
    {
        char *dirname = (char *)SDL_malloc(len + 2);
        if (!dirname) {
            return false;
        }
        memcpy(dirname, path, len + 1);
        while (len > 0 && dirname[len - 1] == '/') {
            dirname[--len] = '\0';
        }
        if (*path) {
            dirname[len] = '/';
            dirname[len + 1] = '\0';
        }
        wchar_t *glob = fs_utf8_dir_to_wide(path);
        if (!glob) {
            SDL_free(dirname);
            return SDL_OutOfMemory();
        }
        WIN32_FIND_DATAW data;
        HANDLE h = FindFirstFileW(glob, &data);
        SDL_free(glob);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            SDL_free(dirname);
            if (err == ERROR_FILE_NOT_FOUND) {
                return true; /* an empty directory: nothing to enumerate */
            }
            return SDL_SetError("Can't open directory: error %lu", (unsigned long)err);
        }
        SDL_EnumerationResult result = SDL_ENUM_CONTINUE;
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            char *name = fs_wide_to_utf8(data.cFileName);
            if (!name) {
                result = SDL_ENUM_FAILURE;
                break;
            }
            result = callback(userdata, dirname, name);
            SDL_free(name);
        } while (result == SDL_ENUM_CONTINUE && FindNextFileW(h, &data));
        FindClose(h);
        SDL_free(dirname);
        return result != SDL_ENUM_FAILURE;
    }
#endif
#ifndef _WIN32
    char *dirname = (char *)SDL_malloc(len + 2);
    if (!dirname) {
        return false;
    }
    memcpy(dirname, path, len + 1);
    while (len > 0 && dirname[len - 1] == '/') {
        dirname[--len] = '\0';
    }
    if (*path) {
        dirname[len] = '/';
        dirname[len + 1] = '\0';
    }

    DIR *dir = opendir(*dirname ? dirname : (*path ? "/" : "."));
    if (!dir) {
        const int err = errno;
        SDL_free(dirname);
        return SDL_SetError("Can't open directory: %s", strerror(err));
    }
    SDL_EnumerationResult result = SDL_ENUM_CONTINUE;
    struct dirent *ent;
    while (result == SDL_ENUM_CONTINUE && (ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        result = callback(userdata, dirname, ent->d_name);
    }
    closedir(dir);
    SDL_free(dirname);
    return result != SDL_ENUM_FAILURE;
#endif /* _WIN32 */
}

bool SDL_RemovePath(const char *path)
{
    if (!path) {
        return SDL_InvalidParamError("path");
    }
#ifdef _WIN32
    wchar_t *wide = fs_utf8_to_wide(path);
    if (!wide) {
        return SDL_OutOfMemory();
    }
    /* Decide by attribute, not by trial and error: RemoveDirectoryW on a
     * regular file fails with ERROR_DIRECTORY (267), *not* ERROR_ACCESS_DENIED,
     * so the old fallback never ran and SDL_RemovePath could not delete a
     * file at all. Ask what the path is first, as stock SDL3's sysfsops does. */
    DWORD attrs = GetFileAttributesW(wide);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD aerr = GetLastError();
        SDL_free(wide);
        /* Only "already gone" counts as success; a missing parent directory is
         * a genuine failure (the path never existed) and must not be masked. */
        if (aerr == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        return SDL_SetError("Couldn't delete %s: error %lu", path, (unsigned long)aerr);
    }
    BOOL ok = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(wide) : DeleteFileW(wide);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    SDL_free(wide);
    if (!ok && err == ERROR_FILE_NOT_FOUND) {
        return true; /* raced with someone else deleting it */
    }
    if (!ok) {
        /* a non-empty directory surfaces as ERROR_DIR_NOT_EMPTY; deleting it
         * recursively is what stock SDL3 does */
        if (err == ERROR_DIR_NOT_EMPTY) {
            return SDL_SetError("Can't remove path: directory not empty");
        }
        return SDL_SetError("Can't remove path: error %lu", (unsigned long)err);
    }
    return true;
#else
    if (remove(path) < 0) {
        if (errno == ENOENT) {
            return true; /* already gone: success, as stock */
        }
        return SDL_SetError("Can't remove path: %s", strerror(errno));
    }
    return true;
#endif /* _WIN32 */
}

bool SDL_RenamePath(const char *oldpath, const char *newpath)
{
    if (!oldpath) {
        return SDL_InvalidParamError("oldpath");
    }
    if (!newpath) {
        return SDL_InvalidParamError("newpath");
    }
#ifdef _WIN32
    wchar_t *wold = fs_utf8_to_wide(oldpath);
    wchar_t *wnew = fs_utf8_to_wide(newpath);
    if (!wold || !wnew) {
        SDL_free(wold);
        SDL_free(wnew);
        return SDL_OutOfMemory();
    }
    const BOOL ok = MoveFileExW(wold, wnew, MOVEFILE_REPLACE_EXISTING);
    const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    SDL_free(wold);
    SDL_free(wnew);
    if (!ok) {
        return SDL_SetError("Can't rename path: error %lu", (unsigned long)err);
    }
    return true;
#else
    if (rename(oldpath, newpath) < 0) {
        return SDL_SetError("Can't rename path: %s", strerror(errno));
    }
    return true;
#endif /* _WIN32 */
}

bool SDL_CopyFile(const char *oldpath, const char *newpath)
{
    if (!oldpath) {
        return SDL_InvalidParamError("oldpath");
    }
    if (!newpath) {
        return SDL_InvalidParamError("newpath");
    }
#ifdef _WIN32
    wchar_t *wold = fs_utf8_to_wide(oldpath);
    wchar_t *wnew = fs_utf8_to_wide(newpath);
    if (!wold || !wnew) {
        SDL_free(wold);
        SDL_free(wnew);
        return SDL_OutOfMemory();
    }
    const BOOL ok = CopyFileW(wold, wnew, FALSE /* overwrite, as stock SDL3 */);
    const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    SDL_free(wold);
    SDL_free(wnew);
    if (!ok) {
        return SDL_SetError("Can't copy file: error %lu", (unsigned long)err);
    }
    return true;
#else
int in = open(oldpath, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        return SDL_SetError("Couldn't open %s: %s", oldpath, strerror(errno));
    }
    int out = open(newpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (out < 0) {
        const int err = errno;
        close(in);
        return SDL_SetError("Couldn't open %s: %s", newpath, strerror(err));
    }
    bool ok = true;
    char buf[16384];
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = SDL_SetError("Error reading from datastream: %s", strerror(errno));
            break;
        }
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ok = SDL_SetError("Error writing to datastream: %s", strerror(errno));
                break;
            }
            off += w;
        }
        if (!ok) {
            break;
        }
    }
    close(in);
    if (close(out) < 0 && ok) {
        ok = SDL_SetError("Error writing to datastream: %s", strerror(errno));
    }
    return ok;
#endif /* _WIN32 */
}

bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info)
{
    SDL_PathInfo dummy;
    if (!info) {
        info = &dummy;
    }
    memset(info, 0, sizeof(*info));
    if (!path) {
        return SDL_InvalidParamError("path");
    }
#ifdef _WIN32
    wchar_t *wide = fs_utf8_to_wide(path);
    if (!wide) {
        return SDL_OutOfMemory();
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
        const DWORD err = GetLastError();
        SDL_free(wide);
        return SDL_SetError("Can't stat: error %lu", (unsigned long)err);
    }
    SDL_free(wide);
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        info->type = SDL_PATHTYPE_DIRECTORY;
        info->size = 0;
    } else if (data.dwFileAttributes & (FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT)) {
        info->type = SDL_PATHTYPE_OTHER;
        info->size = ((Uint64)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    } else {
        info->type = SDL_PATHTYPE_FILE;
        info->size = ((Uint64)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    }
    info->create_time = fs_filetime_to_ns(&data.ftCreationTime);
    info->modify_time = fs_filetime_to_ns(&data.ftLastWriteTime);
    info->access_time = fs_filetime_to_ns(&data.ftLastAccessTime);
    return true;
#else
    struct stat st;
    if (stat(path, &st) < 0) {
        return SDL_SetError("Can't stat: %s", strerror(errno));
    }
    if (S_ISREG(st.st_mode)) {
        info->type = SDL_PATHTYPE_FILE;
        info->size = (Uint64)st.st_size;
    } else if (S_ISDIR(st.st_mode)) {
        info->type = SDL_PATHTYPE_DIRECTORY;
        info->size = 0;
    } else {
        info->type = SDL_PATHTYPE_OTHER;
        info->size = (Uint64)st.st_size;
    }
    info->create_time = (SDL_Time)SDL_SECONDS_TO_NS(st.st_ctim.tv_sec) + st.st_ctim.tv_nsec;
    info->modify_time = (SDL_Time)SDL_SECONDS_TO_NS(st.st_mtim.tv_sec) + st.st_mtim.tv_nsec;
    info->access_time = (SDL_Time)SDL_SECONDS_TO_NS(st.st_atim.tv_sec) + st.st_atim.tv_nsec;
    return true;
#endif /* _WIN32 */
}

char *SDL_GetCurrentDirectory(void)
{
#ifdef _WIN32
    DWORD len = 64;
    char *buf = NULL;
    for (;;) {
        wchar_t *wbuf = (wchar_t *)SDL_malloc((size_t)len * sizeof(wchar_t));
        if (!wbuf) {
            SDL_free(buf);
            return NULL;
        }
        const DWORD n = GetCurrentDirectoryW(len, wbuf);
        if (n > 0 && n < len - 1) {
            char *utf8 = fs_wide_to_utf8(wbuf);
            SDL_free(wbuf);
            SDL_free(buf);
            if (!utf8) {
                return NULL;
            }
            fs_to_forward_slashes(utf8);
            /* stock SDL3 returns the directory with a trailing '/' */
            const size_t l = strlen(utf8);
            if (l == 0 || utf8[l - 1] != '/') {
                char *grown = (char *)SDL_realloc(utf8, l + 2);
                if (!grown) {
                    SDL_free(utf8);
                    return NULL;
                }
                utf8 = grown;
                utf8[l] = '/';
                utf8[l + 1] = '\0';
            }
            return utf8;
        }
        SDL_free(wbuf);
        SDL_free(buf);
        buf = NULL;
        if (n == 0) {
            SDL_SetError("getcwd failed: error %lu", (unsigned long)GetLastError());
            return NULL;
        }
        len = n + 1; /* GetCurrentDirectoryW reported the size it needs */
    }
#else
    size_t len = 64;
    char *buf = NULL;
    for (;;) {
        char *p = (char *)SDL_realloc(buf, len);
        if (!p) {
            SDL_free(buf);
            return NULL;
        }
        buf = p;
        if (getcwd(buf, len - 1) != NULL) {
            break;
        }
        if (errno != ERANGE) {
            SDL_SetError("getcwd failed: %s", strerror(errno));
            SDL_free(buf);
            return NULL;
        }
        len *= 2;
    }
    /* stock SDL3 returns the directory with a trailing '/' */
    size_t l = strlen(buf);
    if (l == 0 || buf[l - 1] != '/') {
        buf[l] = '/';
        buf[l + 1] = '\0';
    }
    return buf;
#endif /* _WIN32 */
}
