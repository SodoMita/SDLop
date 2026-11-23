/*
  SDLop - miscellaneous SDL3 API: assertions, GUID strings, message boxes,
  SDL_OpenURL, power info, and the platform stubs a Linux build of stock
  SDL3 exports (SDL_RegisterApp, SDL_GDKSuspendComplete, ...).

  Ported from SDL 3.4.20 (src/SDL_assert.c, src/SDL_guid.c,
  src/dialog/unix/SDL_zenitymessagebox.c, src/misc/unix/SDL_sysurl.c,
  src/power/linux/SDL_syspower.c), (C) Sam Lantinga, zlib license.
  Process spawning uses posix_spawn directly (SDLop has no SDL_Process).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define SDL_MAIN_NOIMPL

#include "internal/sdlop_internal.h"
/* SDL_RegisterApp / SDL_UnregisterApp / SDL_GDKSuspendComplete are declared
 * under #if defined(SDL_PLATFORM_WINDOWS) in SDL_main.h, and without this
 * include their definitions here compile without a dllexport prototype, so
 * they drop out of the DLL's export list as soon as the build stops relying
 * on auto-export. SDL_MAIN_NOIMPL above keeps the header from emitting main. */
#include <SDL3/SDL_main.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* Win32 has no posix_spawn/dirent/unistd; the few places that need them
 * (SDL_OpenURL, the file dialogs) use ShellExecuteExW instead. */
#include <shellapi.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif !defined(_WIN32)
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

/* ------------------------------------------------------------------ */
/* GUID                                                                */
/* ------------------------------------------------------------------ */

void SDL_GUIDToString(SDL_GUID guid, char *pszGUID, int cbGUID)
{
    static const char k_rgchHexToASCII[] = "0123456789abcdef";
    if (!pszGUID || cbGUID <= 0) {
        return;
    }
    for (int i = 0; i < (int)sizeof(guid.data) && i < (cbGUID - 1) / 2; i++) {
        const unsigned char c = guid.data[i];
        *pszGUID++ = k_rgchHexToASCII[c >> 4];
        *pszGUID++ = k_rgchHexToASCII[c & 0x0F];
    }
    *pszGUID = '\0';
}

static unsigned char guid_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return (unsigned char)(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return (unsigned char)(c - 'A' + 0x0a);
    }
    if (c >= 'a' && c <= 'f') {
        return (unsigned char)(c - 'a' + 0x0a);
    }
    return 0; /* invalid characters read as 0, like stock */
}

SDL_GUID SDL_StringToGUID(const char *pchGUID)
{
    SDL_GUID guid;
    SDL_zero(guid);
    if (!pchGUID) {
        return guid;
    }
    size_t len = strlen(pchGUID) & ~(size_t)1; /* even */
    Uint8 *p = guid.data;
    for (size_t i = 0; i < len && p < guid.data + sizeof(guid.data); i += 2, p++) {
        *p = (Uint8)((guid_nibble((unsigned char)pchGUID[i]) << 4) | guid_nibble((unsigned char)pchGUID[i + 1]));
    }
    return guid;
}

/* ------------------------------------------------------------------ */
/* Child processes (message boxes, URLs)                               */
/* ------------------------------------------------------------------ */

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
/* is `prog` an executable in $PATH? (posix_spawnp would tell us too,
 * but glibc reports a missing program only through the exit status) */
bool SDLOP_ProgramInPath(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path || !*path) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    char buf[4096];
    while (*path) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        if (n > 0 && n + strlen(prog) + 2 < sizeof(buf)) {
            memcpy(buf, path, n);
            buf[n] = '/';
            strcpy(buf + n + 1, prog);
            if (access(buf, X_OK) == 0) {
                return true;
            }
        }
        if (!end) {
            break;
        }
        path = end + 1;
    }
    return false;
}

/* environment without LD_PRELOAD (stock: Steam's overlay breaks browsers) */
static char **child_environment(const char *const *extra)
{
    size_t n = 0, nextra = 0;
    while (environ && environ[n]) {
        n++;
    }
    while (extra && extra[nextra]) {
        nextra++;
    }
    char **env = (char **)calloc(n + nextra + 1, sizeof(char *));
    if (!env) {
        return NULL;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (strncmp(environ[i], "LD_PRELOAD=", 11) == 0) {
            continue;
        }
        bool overridden = false; /* extra "NAME=value" replaces NAME */
        for (size_t j = 0; j < nextra && !overridden; j++) {
            const char *eq = strchr(extra[j], '=');
            const size_t len = eq ? (size_t)(eq - extra[j]) + 1 : 0;
            overridden = len && strncmp(environ[i], extra[j], len) == 0;
        }
        if (!overridden) {
            env[k++] = environ[i];
        }
    }
    for (size_t j = 0; j < nextra; j++) {
        env[k++] = (char *)extra[j];
    }
    return env;
}

/* run argv; with `output`, capture stdout (malloc'ed, NUL-terminated).
 * Returns the exit status (0-255), or -1 when it could not be run. */
int SDLOP_RunProcess(const char *const *argv, const char *const *extra_env, char **output, bool wait_for_exit)
{
    int pipefd[2] = { -1, -1 };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (output) {
        *output = NULL;
        if (pipe2(pipefd, O_CLOEXEC) != 0) {
            posix_spawn_file_actions_destroy(&fa);
            SDL_SetError("pipe() failed: %s", strerror(errno));
            return -1;
        }
        posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    char **env = child_environment(extra_env);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, env ? env : environ);
    free(env);
    posix_spawn_file_actions_destroy(&fa);
    if (output) {
        close(pipefd[1]);
    }
    if (rc != 0) {
        if (output) {
            close(pipefd[0]);
        }
        SDL_SetError("Couldn't run %s: %s", argv[0], strerror(rc));
        return -1;
    }

    if (output) {
        size_t cap = 256, len = 0;
        char *buf = (char *)malloc(cap);
        for (;;) {
            if (buf && len + 1 >= cap) {
                char *nb = (char *)realloc(buf, cap * 2);
                if (!nb) {
                    free(buf);
                    buf = NULL;
                } else {
                    buf = nb;
                    cap *= 2;
                }
            }
            char tmp[256];
            ssize_t r = read(pipefd[0], buf ? buf + len : tmp, buf ? cap - len - 1 : sizeof(tmp));
            if (r < 0 && errno == EINTR) {
                continue;
            }
            if (r <= 0) {
                break;
            }
            if (buf) {
                len += (size_t)r;
            }
        }
        close(pipefd[0]);
        if (buf) {
            buf[len] = '\0';
        }
        *output = buf;
    }
    if (!wait_for_exit) {
        return 0;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}
#endif /* !__EMSCRIPTEN__ && !_WIN32 */

/* ------------------------------------------------------------------ */
/* Message boxes (zenity, as stock SDL3 does on Wayland)               */
/* ------------------------------------------------------------------ */

#define SDLOP_MAX_MSGBOX_BUTTONS 8

static SDL_AtomicInt sdlop_messagebox_count; /* stock tracks open boxes too */

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
bool SDLOP_ZenityVersion(int *major, int *minor)
{
    const char *argv[] = { "zenity", "--version", NULL };
    char *out = NULL;
    if (SDLOP_RunProcess(argv, NULL, &out, true) < 0 || !out) {
        free(out);
        return false;
    }
    char *end = NULL;
    long v = strtol(out, &end, 10);
    if (v == 0 && end == out) {
        free(out);
        return SDL_SetError("failed to get zenity major version number");
    }
    *major = (int)v;
    *minor = 0;
    if (*end == '.') {
        *minor = (int)strtol(end + 1, NULL, 10);
    }
    free(out);
    return true;
}

static bool zenity_show(const SDL_MessageBoxData *data, int *buttonID)
{
    if (data->numbuttons > SDLOP_MAX_MSGBOX_BUTTONS) {
        return SDL_SetError("Too many buttons (%d max allowed)", SDLOP_MAX_MSGBOX_BUTTONS);
    }
    if (!SDLOP_ProgramInPath("zenity")) {
        return SDL_SetError("zenity is not installed");
    }
    int major = 0, minor = 0;
    if (!SDLOP_ZenityVersion(&major, &minor)) {
        return false;
    }
    const char *argv[5 + 2 + 2 + 2 + 2 * SDLOP_MAX_MSGBOX_BUTTONS + 1] = {
        "zenity", "--question", "--switch", "--no-wrap", "--no-markup"
    };
    int argc = 5;
    /* zenity 3.90 renamed --icon-name to --icon without a deprecation */
    argv[argc++] = (major > 3 || (major == 3 && minor >= 90)) ? "--icon" : "--icon-name";
    switch (data->flags & (SDL_MESSAGEBOX_ERROR | SDL_MESSAGEBOX_WARNING | SDL_MESSAGEBOX_INFORMATION)) {
    case SDL_MESSAGEBOX_ERROR:
        argv[argc++] = "dialog-error";
        break;
    case SDL_MESSAGEBOX_WARNING:
        argv[argc++] = "dialog-warning";
        break;
    default:
        argv[argc++] = "dialog-information";
        break;
    }
    if (data->title && data->title[0]) {
        argv[argc++] = "--title";
        argv[argc++] = data->title;
    } else {
        argv[argc++] = "--title=";
    }
    if (data->message && data->message[0]) {
        argv[argc++] = "--text";
        argv[argc++] = data->message;
    } else {
        argv[argc++] = "--text=";
    }
    for (int i = 0; i < data->numbuttons; ++i) {
        if (data->buttons[i].text && data->buttons[i].text[0]) {
            argv[argc++] = "--extra-button";
            argv[argc++] = data->buttons[i].text;
        } else {
            argv[argc++] = "--extra-button=";
        }
    }
    if (data->numbuttons == 0) {
        argv[argc++] = "--extra-button=OK";
    }
    argv[argc] = NULL;

    char *out = NULL;
    int code = SDLOP_RunProcess(argv, NULL, &out, true);
    if (code >= 0 && code != 255 && out) {
        char *nl = strrchr(out, '\n'); /* it likes to add a newline */
        if (nl) {
            *nl = '\0';
        }
        for (int i = 0; i < data->numbuttons; ++i) {
            if (data->buttons[i].text && strcmp(out, data->buttons[i].text) == 0) {
                *buttonID = data->buttons[i].buttonID;
                break;
            }
        }
    }
    free(out);
    return !(code < 0 || code == 255);
}
#endif /* !__EMSCRIPTEN__ && !_WIN32 */

#ifdef _WIN32
static wchar_t *utf8_to_wide(const char *text)
{
    if (!text) {
        text = "";
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (len <= 0) {
        return NULL;
    }
    wchar_t *wide = (wchar_t *)SDL_malloc((size_t)len * sizeof(wchar_t));
    if (wide) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, len);
    }
    return wide;
}

/* SDL_ShowMessageBox on Win32: MessageBoxW for the simple cases, and for
 * multiple custom buttons a task dialog is the only way to label them, so
 * the buttons are mapped onto the standard set when that is enough. */
static bool win32_show_messagebox(const SDL_MessageBoxData *data, int *buttonID)
{
    UINT type = MB_OK;
    if (data->flags & SDL_MESSAGEBOX_ERROR) {
        type = MB_ICONERROR;
    } else if (data->flags & SDL_MESSAGEBOX_WARNING) {
        type = MB_ICONWARNING;
    } else if (data->flags & SDL_MESSAGEBOX_INFORMATION) {
        type = MB_ICONINFORMATION;
    }
    /* Win32 offers no way to lay out custom buttons, so the two- and
     * three-button cases map onto the closest stock box. Both branches of the
     * old left-to-right test were identical. */
    if (data->numbuttons == 3) {
        type |= MB_YESNOCANCEL;
    } else if (data->numbuttons >= 2) {
        type |= MB_OKCANCEL;
    }

    wchar_t *wtitle = utf8_to_wide(data->title);
    wchar_t *wmessage = utf8_to_wide(data->message);
    if (!wtitle || !wmessage) {
        SDL_free(wtitle);
        SDL_free(wmessage);
        return SDL_OutOfMemory();
    }
    HWND parent = NULL;
    if (data->window) {
        SDL_PropertiesID props = SDL_GetWindowProperties(data->window);
        /* It is a *pointer* property; SDL_GetNumberProperty returns 0 for it. */
        parent = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    }
    const int rc = MessageBoxW(parent, wmessage, wtitle, type);
    SDL_free(wtitle);
    SDL_free(wmessage);
    if (rc == 0) {
        return SDL_SetError("MessageBox() failed: %lu", (unsigned long)GetLastError());
    }
    /* Map the Win32 answer back onto the caller's button list. */
    int index = 0;
    switch (rc) {
    case IDOK:
        index = 0;
        break;
    case IDCANCEL:
        /* A three-button box is MB_YESNOCANCEL, where Cancel is the *third*
         * button. Mapping it to index 1 made it indistinguishable from No and
         * left buttons[2] unreachable. */
        index = (data->numbuttons >= 3) ? 2 : ((data->numbuttons > 1) ? 1 : 0);
        break;
    case IDYES:
        index = 0;
        break;
    case IDNO:
        index = 1;
        break;
    default:
        index = 0;
        break;
    }
    if (index >= data->numbuttons) {
        index = data->numbuttons - 1;
    }
    if (buttonID) {
        *buttonID = data->buttons ? data->buttons[index].buttonID : index;
    }
    return true;
}
#endif /* _WIN32 */

bool SDL_ShowMessageBox(const SDL_MessageBoxData *messageboxdata, int *buttonID)
{
    if (!messageboxdata) {
        return SDL_SetError("Parameter '%s' is invalid", "messageboxdata");
    }
    if (messageboxdata->numbuttons < 0) {
        return SDL_SetError("Invalid number of buttons");
    }
    int dummybutton;
    if (!buttonID) {
        buttonID = &dummybutton;
    }
    *buttonID = -1; /* closed without choosing (stock reports -1 then) */

    /* title/message may point into SDL_GetError() storage: copy first */
    char *title = SDL_strdup(messageboxdata->title ? messageboxdata->title : "");
    char *message = SDL_strdup(messageboxdata->message ? messageboxdata->message : "");
    if (!title || !message) {
        SDL_free(title);
        SDL_free(message);
        return false;
    }
    SDL_MessageBoxData mb = *messageboxdata;
    mb.title = title;
    mb.message = message;

    SDL_AddAtomicInt(&sdlop_messagebox_count, 1);
    SDL_Window *focus = SDL_GetKeyboardFocus();
    const bool cursor_was_visible = SDL_CursorVisible();
    SDL_ShowCursor();
    SDL_ResetKeyboard();
    SDL_ClearError();

#if defined(__EMSCRIPTEN__)
    bool result = SDL_Unsupported();
#elif defined(_WIN32)
    bool result = win32_show_messagebox(&mb, buttonID);
#else
    bool result = zenity_show(&mb, buttonID);
#endif
    if (!result) {
        if (!*SDL_GetError()) {
            SDL_SetError("No message system available");
        }
    } else {
        SDL_ClearError();
    }

    SDL_AddAtomicInt(&sdlop_messagebox_count, -1);
    if (focus && SDLOP_WindowValid(focus)) {
        SDL_RaiseWindow(focus);
    }
    if (!cursor_was_visible) {
        SDL_HideCursor();
    }
    SDL_free(title);
    SDL_free(message);
    return result;
}

bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *window)
{
    SDL_MessageBoxData data;
    SDL_MessageBoxButtonData button;
    SDL_zero(data);
    data.flags = flags;
    data.title = title;
    data.message = message;
    data.numbuttons = 1;
    data.buttons = &button;
    data.window = window;
    SDL_zero(button);
    button.flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    button.text = "OK";
    return SDL_ShowMessageBox(&data, NULL);
}

/* ------------------------------------------------------------------ */
/* SDL_OpenURL                                                          */
/* ------------------------------------------------------------------ */

bool SDL_OpenURL(const char *url)
{
    if (!url) {
        return SDL_SetError("Parameter '%s' is invalid", "url");
    }
#if defined(__EMSCRIPTEN__)
    EM_ASM({ window.open(UTF8ToString($0), "_blank"); }, url);
    return true;
#elif defined(_WIN32)
    wchar_t wurl[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, (int)SDL_arraysize(wurl)) <= 0) {
        return SDL_SetError("Couldn't convert the URL to UTF-16");
    }
    const HINSTANCE rc = ShellExecuteW(NULL, L"open", wurl, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)rc <= 32) {
        return SDL_SetError("ShellExecute() failed: %ld", (long)(INT_PTR)rc);
    }
    return true;
#else
    if (!SDLOP_ProgramInPath("xdg-open")) {
        return SDL_SetError("xdg-open is not installed");
    }
    /* background process (stock: SDL_PROP_PROCESS_CREATE_BACKGROUND):
     * double fork so the browser is not our child and leaves no zombie */
    pid_t pid = fork();
    if (pid < 0) {
        return SDL_SetError("fork() failed: %s", strerror(errno));
    }
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
            char **env = child_environment(NULL);
            const char *argv[] = { "xdg-open", url, NULL };
            execvpe(argv[0], (char *const *)argv, env ? env : environ);
            _exit(127);
        }
        _exit(0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return true;
#endif
}

/* ------------------------------------------------------------------ */
/* Power                                                                */
/* ------------------------------------------------------------------ */

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
static bool read_power_file(const char *base, const char *node, const char *key, char *buf, size_t buflen)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", base, node, key);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    ssize_t r = read(fd, buf, buflen - 1);
    close(fd);
    if (r < 0) {
        return false;
    }
    buf[r] = '\0';
    return true;
}

/* /sys/class/power_supply, exactly like SDL3's Linux backend */
static bool power_sys_class(SDL_PowerState *state, int *seconds, int *percent)
{
    const char *base = SDL_GetHint("SDLOP_POWER_SUPPLY_PATH"); /* tests */
    if (!base) {
        base = "/sys/class/power_supply";
    }
    DIR *dirp = opendir(base);
    if (!dirp) {
        return false;
    }
    *state = SDL_POWERSTATE_NO_BATTERY; /* assume we're just plugged in */
    *seconds = -1;
    *percent = -1;
    struct dirent *dent;
    while ((dent = readdir(dirp)) != NULL) {
        const char *name = dent->d_name;
        char str[64];
        SDL_PowerState st;
        int secs, pct;
        bool choose = false;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        } else if (!read_power_file(base, name, "type", str, sizeof(str))) {
            continue;
        } else if (strcasecmp(str, "Battery\n") != 0) {
            continue; /* not UPS and such */
        }
        if (read_power_file(base, name, "scope", str, sizeof(str)) && strcasecmp(str, "Device\n") == 0) {
            continue; /* a peripheral's own battery */
        }
        if (read_power_file(base, name, "present", str, sizeof(str)) && strcmp(str, "0\n") == 0) {
            st = SDL_POWERSTATE_NO_BATTERY;
        } else if (!read_power_file(base, name, "status", str, sizeof(str))) {
            st = SDL_POWERSTATE_UNKNOWN;
        } else if (strcasecmp(str, "Charging\n") == 0) {
            st = SDL_POWERSTATE_CHARGING;
        } else if (strcasecmp(str, "Discharging\n") == 0) {
            st = SDL_POWERSTATE_ON_BATTERY;
        } else if (strcasecmp(str, "Full\n") == 0 || strcasecmp(str, "Not charging\n") == 0) {
            st = SDL_POWERSTATE_CHARGED;
        } else {
            st = SDL_POWERSTATE_UNKNOWN;
        }
        if (!read_power_file(base, name, "capacity", str, sizeof(str))) {
            pct = -1;
        } else {
            pct = atoi(str);
            pct = pct > 100 ? 100 : pct;
        }
        if (read_power_file(base, name, "time_to_empty_now", str, sizeof(str))) {
            secs = atoi(str);
            secs = secs <= 0 ? -1 : secs;
        } else if (st == SDL_POWERSTATE_ON_BATTERY) {
            /* energy is Watt*hours and power is Watts */
            int energy = read_power_file(base, name, "energy_now", str, sizeof(str)) ? atoi(str) : -1;
            int power = read_power_file(base, name, "power_now", str, sizeof(str)) ? atoi(str) : -1;
            secs = (energy >= 0 && power > 0) ? (int)((3600LL * energy) / power) : -1;
        } else {
            secs = -1;
        }
        /* the battery with the most time left (else the highest percent) */
        if (secs < 0 && *seconds < 0) {
            if (pct < 0 && *percent < 0) {
                choose = true;
            } else if (pct > *percent) {
                choose = true;
            }
        } else if (secs > *seconds) {
            choose = true;
        }
        if (choose) {
            *seconds = secs;
            *percent = pct;
            *state = st;
        }
    }
    closedir(dirp);
    return true;
}
#endif

SDL_PowerState SDL_GetPowerInfo(int *seconds, int *percent)
{
    int s_, p_;
    if (!seconds) {
        seconds = &s_;
    }
    if (!percent) {
        percent = &p_;
    }
#if defined(_WIN32)
    /* GetSystemPowerStatus covers battery/AC; Win32 has no time estimate that
     * SDL3 trusts, so seconds stays -1 like stock SDL's Windows backend. */
    SYSTEM_POWER_STATUS status;
    if (GetSystemPowerStatus(&status)) {
        *seconds = -1;
        *percent = status.BatteryLifePercent <= 100 ? (int)status.BatteryLifePercent : -1;
        if (status.ACLineStatus == 255) {
            return SDL_POWERSTATE_UNKNOWN;
        }
        if (status.BatteryFlag == 128) {
            return SDL_POWERSTATE_NO_BATTERY;
        }
        if (status.BatteryFlag == 255) {
            return SDL_POWERSTATE_UNKNOWN;
        }
        if (status.BatteryFlag & 8) {
            return SDL_POWERSTATE_CHARGING;
        }
        if (status.ACLineStatus == 1) {
            return SDL_POWERSTATE_CHARGED;
        }
        return SDL_POWERSTATE_ON_BATTERY;
    }
#elif !defined(__EMSCRIPTEN__)
    SDL_PowerState state;
    if (power_sys_class(&state, seconds, percent)) {
        return state;
    }
#endif
    *seconds = -1;
    *percent = -1;
    return SDL_POWERSTATE_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Assertions                                                           */
/* ------------------------------------------------------------------ */

#define SDLOP_ASSERT_STACK_MSG 256

static SDL_AssertState SDLCALL prompt_assertion(const SDL_AssertData *data, void *userdata);

static SDL_AssertData *triggered_assertions = NULL;
static SDL_Mutex *assertion_mutex = NULL;
static SDL_AssertionHandler assertion_handler = prompt_assertion;
static void *assertion_userdata = NULL;

static void assert_print(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
static void assert_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_ASSERT, SDL_LOG_PRIORITY_WARN, fmt, ap);
    va_end(ap);
}

static int render_assert_message(char *buf, size_t len, const SDL_AssertData *data)
{
    return snprintf(buf, len, "Assertion failure at %s (%s:%d), triggered %u %s:\n  '%s'", data->function,
                    data->filename, data->linenum, data->trigger_count,
                    data->trigger_count == 1 ? "time" : "times", data->condition);
}

static void generate_assertion_report(void)
{
    const SDL_AssertData *item = triggered_assertions;
    /* stock prints this only when the app installed its own handler */
    if (item && assertion_handler != prompt_assertion) {
        assert_print("\n\nSDL assertion report.\n");
        assert_print("All SDL assertions between last init/quit:\n\n");
        while (item) {
            assert_print("'%s'\n"
                         "    * %s (%s:%d)\n"
                         "    * triggered %u time%s.\n"
                         "    * always ignore: %s.\n",
                         item->condition, item->function, item->filename, item->linenum, item->trigger_count,
                         item->trigger_count == 1 ? "" : "s", item->always_ignore ? "yes" : "no");
            item = item->next;
        }
        assert_print("\n");
        SDL_ResetAssertionReport();
    }
}

static SDL_NORETURN void abort_assertion(void)
{
    SDL_Quit();
    abort();
}

static SDL_AssertState SDLCALL prompt_assertion(const SDL_AssertData *data, void *userdata)
{
    (void)userdata;
    SDL_AssertState state = SDL_ASSERTION_ABORT;
    char stack_buf[SDLOP_ASSERT_STACK_MSG];
    char *message = stack_buf;
    int len = render_assert_message(message, sizeof(stack_buf), data);
    if (len >= (int)sizeof(stack_buf)) {
        char *big = (char *)malloc((size_t)len + 1);
        if (big) {
            message = big;
            len = render_assert_message(message, (size_t)len + 1, data);
        }
    }
    if (len < 0) {
        if (message != stack_buf) {
            free(message);
        }
        return SDL_ASSERTION_ABORT;
    }
    assert_print("\n\n%s\n\n", message);

    /* the env/hint override keeps unit tests from blocking in a GUI */
    const char *hint = SDL_GetHint(SDL_HINT_ASSERT);
    if (hint) {
        if (message != stack_buf) {
            free(message);
        }
        if (strcmp(hint, "abort") == 0) {
            return SDL_ASSERTION_ABORT;
        } else if (strcmp(hint, "break") == 0) {
            return SDL_ASSERTION_BREAK;
        } else if (strcmp(hint, "retry") == 0) {
            return SDL_ASSERTION_RETRY;
        } else if (strcmp(hint, "ignore") == 0) {
            return SDL_ASSERTION_IGNORE;
        } else if (strcmp(hint, "always_ignore") == 0) {
            return SDL_ASSERTION_ALWAYS_IGNORE;
        }
        return SDL_ASSERTION_ABORT;
    }

    /* leave exclusive fullscreen so the prompt is visible */
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window && !(window->has_fullscreen_mode && (window->flags & SDL_WINDOW_FULLSCREEN))) {
        window = NULL;
    }
    if (window) {
        SDL_MinimizeWindow(window);
    }

    const SDL_MessageBoxButtonData buttons[] = {
        { 0, SDL_ASSERTION_RETRY, "Retry" },
        { 0, SDL_ASSERTION_BREAK, "Break" },
        { 0, SDL_ASSERTION_ABORT, "Abort" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, SDL_ASSERTION_IGNORE, "Ignore" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, SDL_ASSERTION_ALWAYS_IGNORE, "Always Ignore" },
    };
    SDL_MessageBoxData mb;
    SDL_zero(mb);
    mb.flags = SDL_MESSAGEBOX_WARNING;
    mb.window = window;
    mb.title = "Assertion Failed";
    mb.message = message;
    mb.numbuttons = (int)SDL_arraysize(buttons);
    mb.buttons = buttons;
    int selected = -1;
    if (SDL_ShowMessageBox(&mb, &selected)) {
        state = selected == -1 ? SDL_ASSERTION_IGNORE : (SDL_AssertState)selected;
    } else {
        for (;;) {
            char buf[32];
            fprintf(stderr, "Abort/Break/Retry/Ignore/AlwaysIgnore? [abriA] : ");
            fflush(stderr);
            if (!fgets(buf, sizeof(buf), stdin)) {
                break; /* no terminal: abort */
            }
            if (buf[0] == 'a') {
                state = SDL_ASSERTION_ABORT;
                break;
            } else if (buf[0] == 'b') {
                state = SDL_ASSERTION_BREAK;
                break;
            } else if (buf[0] == 'r') {
                state = SDL_ASSERTION_RETRY;
                break;
            } else if (buf[0] == 'i') {
                state = SDL_ASSERTION_IGNORE;
                break;
            } else if (buf[0] == 'A') {
                state = SDL_ASSERTION_ALWAYS_IGNORE;
                break;
            }
        }
    }
    if (window) {
        SDL_RestoreWindow(window);
    }
    if (message != stack_buf) {
        free(message);
    }
    return state;
}

SDL_AssertState SDL_ReportAssertion(SDL_AssertData *data, const char *func, const char *file, int line)
{
    SDL_AssertState state = SDL_ASSERTION_IGNORE;
    static int assertion_running = 0;
    static SDL_SpinLock spinlock = 0;

    SDL_LockSpinlock(&spinlock);
    if (!assertion_mutex) {
        assertion_mutex = SDL_CreateMutex();
        if (!assertion_mutex) {
            SDL_UnlockSpinlock(&spinlock);
            return SDL_ASSERTION_IGNORE;
        }
    }
    SDL_UnlockSpinlock(&spinlock);
    SDL_LockMutex(assertion_mutex);

    if (data->trigger_count == 0) {
        data->function = func;
        data->filename = file;
        data->linenum = line;
    }
    data->trigger_count++;
    if (data->trigger_count == 1) {
        data->next = triggered_assertions;
        triggered_assertions = data;
    }

    assertion_running++;
    if (assertion_running > 1) { /* assert during assert */
        if (assertion_running == 2) {
            abort_assertion();
        } else if (assertion_running == 3) {
            abort();
        } else {
            for (;;) {
            }
        }
    }
    if (!data->always_ignore) {
        state = assertion_handler(data, assertion_userdata);
    }
    switch (state) {
    case SDL_ASSERTION_ALWAYS_IGNORE:
        state = SDL_ASSERTION_IGNORE;
        data->always_ignore = true;
        break;
    case SDL_ASSERTION_IGNORE:
    case SDL_ASSERTION_RETRY:
    case SDL_ASSERTION_BREAK:
        break; /* the macro handles these */
    case SDL_ASSERTION_ABORT:
        abort_assertion();
    }
    assertion_running--;
    SDL_UnlockMutex(assertion_mutex);
    return state;
}

void SDLOP_AssertionsQuit(void)
{
    generate_assertion_report();
    if (assertion_mutex) {
        SDL_DestroyMutex(assertion_mutex);
        assertion_mutex = NULL;
    }
}

void SDL_SetAssertionHandler(SDL_AssertionHandler handler, void *userdata)
{
    if (handler) {
        assertion_handler = handler;
        assertion_userdata = userdata;
    } else {
        assertion_handler = prompt_assertion;
        assertion_userdata = NULL;
    }
}

const SDL_AssertData *SDL_GetAssertionReport(void)
{
    return triggered_assertions;
}

void SDL_ResetAssertionReport(void)
{
    SDL_AssertData *next;
    for (SDL_AssertData *item = triggered_assertions; item; item = next) {
        next = (SDL_AssertData *)item->next;
        item->always_ignore = false;
        item->trigger_count = 0;
        item->next = NULL;
    }
    triggered_assertions = NULL;
}

SDL_AssertionHandler SDL_GetDefaultAssertionHandler(void)
{
    return prompt_assertion;
}

SDL_AssertionHandler SDL_GetAssertionHandler(void **puserdata)
{
    if (puserdata) {
        *puserdata = assertion_userdata;
    }
    return assertion_handler;
}

/* ------------------------------------------------------------------ */
/* Platform stubs (what a Linux build of stock SDL3 exports)           */
/* ------------------------------------------------------------------ */

bool SDL_RegisterApp(const char *name, Uint32 style, void *hInst)
{
    (void)name;
    (void)style;
    (void)hInst;
    return SDL_Unsupported(); /* Windows only */
}

void SDL_UnregisterApp(void)
{
    SDL_Unsupported();
}

void SDL_GDKSuspendComplete(void)
{
    SDL_Unsupported(); /* GDK only */
}


