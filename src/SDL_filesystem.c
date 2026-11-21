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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t fs_lock = PTHREAD_MUTEX_INITIALIZER;
static char *cached_base_path;
static char *cached_user_folders[SDL_FOLDER_COUNT];

/* ------------------------------------------------------------------ */
/* Base / pref path                                                    */
/* ------------------------------------------------------------------ */

static char *read_exe_path(void)
{
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
}

const char *SDL_GetBasePath(void)
{
    pthread_mutex_lock(&fs_lock);
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
    pthread_mutex_unlock(&fs_lock);
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
}

/* ------------------------------------------------------------------ */
/* User folders (XDG user-dirs)                                        */
/* ------------------------------------------------------------------ */

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

static char *lookup_user_folder(SDL_Folder folder)
{
    const char *param = NULL;
    char *result = NULL;

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
}

const char *SDL_GetUserFolder(SDL_Folder folder)
{
    const int idx = (int)folder;
    if (idx < 0 || idx >= SDL_FOLDER_COUNT) {
        SDL_InvalidParamError("folder");
        return NULL;
    }
    pthread_mutex_lock(&fs_lock);
    if (!cached_user_folders[idx]) {
        cached_user_folders[idx] = lookup_user_folder(folder);
    }
    const char *result = cached_user_folders[idx];
    pthread_mutex_unlock(&fs_lock);
    return result;
}

/* Called from SDL_Quit (stock frees its caches in SDL_QuitFilesystem). */
void SDLOP_QuitFilesystem(void)
{
    pthread_mutex_lock(&fs_lock);
    SDL_free(cached_base_path);
    cached_base_path = NULL;
    for (int i = 0; i < SDL_FOLDER_COUNT; i++) {
        SDL_free(cached_user_folders[i]);
        cached_user_folders[i] = NULL;
    }
    pthread_mutex_unlock(&fs_lock);
}

/* ------------------------------------------------------------------ */
/* Filesystem operations                                               */
/* ------------------------------------------------------------------ */

bool SDL_CreateDirectory(const char *path)
{
    if (!path) {
        return SDL_InvalidParamError("path");
    }
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
}

bool SDL_RemovePath(const char *path)
{
    if (!path) {
        return SDL_InvalidParamError("path");
    }
    if (remove(path) < 0) {
        if (errno == ENOENT) {
            return true; /* already gone: success, as stock */
        }
        return SDL_SetError("Can't remove path: %s", strerror(errno));
    }
    return true;
}

bool SDL_RenamePath(const char *oldpath, const char *newpath)
{
    if (!oldpath) {
        return SDL_InvalidParamError("oldpath");
    }
    if (!newpath) {
        return SDL_InvalidParamError("newpath");
    }
    if (rename(oldpath, newpath) < 0) {
        return SDL_SetError("Can't rename path: %s", strerror(errno));
    }
    return true;
}

bool SDL_CopyFile(const char *oldpath, const char *newpath)
{
    if (!oldpath) {
        return SDL_InvalidParamError("oldpath");
    }
    if (!newpath) {
        return SDL_InvalidParamError("newpath");
    }
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
}

char *SDL_GetCurrentDirectory(void)
{
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
}
