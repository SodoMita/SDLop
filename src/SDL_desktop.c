/*
  SDLop - desktop integration over the session bus (SDL_dbus.c), like
  stock SDL3's core/linux SDL_system_theme.c + SDL_progressbar.c:

  - system theme: org.freedesktop.portal.Settings "color-scheme" in the
    "org.freedesktop.appearance" namespace (1 = dark, 2 = light, 0 = no
    preference). The first read is sent asynchronously at video init so
    startup never waits for the portal; SDL_GetSystemTheme() only waits
    (300 ms at most) when it's called before the reply arrived. Changes
    (SettingChanged) become SDL_EVENT_SYSTEM_THEME_CHANGED.
  - progress: com.canonical.Unity.LauncherEntry.Update signals (taskbar /
    dock progress on KDE, GNOME's Dash to Dock, Unity, ...)

  Disabled with SDLOP_NO_DBUS=1.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PORTAL_DEST "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_SETTINGS "org.freedesktop.portal.Settings"
#define APPEARANCE_NS "org.freedesktop.appearance"
#define COLOR_SCHEME_KEY "color-scheme"
#define THEME_MATCH                                                                                          \
    "type='signal',interface='" PORTAL_SETTINGS "',member='SettingChanged',path='" PORTAL_PATH "',arg0='" \
    APPEARANCE_NS "',arg1='" COLOR_SCHEME_KEY "'"

static struct
{
    bool started;     /* theme lookup started (or impossible) */
    bool handler;     /* message handler installed */
    bool match;       /* SettingChanged match rule added */
    Uint32 theme_serial; /* pending Settings.Read */
    SDL_SystemTheme theme;
} dt;

/* color-scheme value: v(u) from ReadOne / SettingChanged, v(v(u)) from
 * the older Read */
static bool parse_color_scheme(SDLop_DBusIter *it, SDL_SystemTheme *out)
{
    SDLop_DBusIter cur = *it, sub;
    for (int depth = 0; depth < 3 && SDLOP_DBus_IterType(&cur) == 'v'; depth++) {
        if (!SDLOP_DBus_IterRecurse(&cur, &sub)) {
            return false;
        }
        cur = sub;
    }
    Uint32 v;
    if (!SDLOP_DBus_IterGetU32(&cur, &v)) {
        return false;
    }
    *out = v == 1 ? SDL_SYSTEM_THEME_DARK : v == 2 ? SDL_SYSTEM_THEME_LIGHT : SDL_SYSTEM_THEME_UNKNOWN;
    return true;
}

static void theme_from_reply(SDLop_DBusMsg *reply)
{
    SDLop_DBusIter it;
    SDL_SystemTheme t;
    if (reply && SDLOP_DBus_MessageType(reply) == SDLOP_DBUS_METHOD_RETURN && SDLOP_DBus_IterInit(reply, &it) &&
        parse_color_scheme(&it, &t)) {
        dt.theme = t;
    }
}

static bool desktop_handler(SDLop_DBusMsg *msg, void *userdata)
{
    (void)userdata;
    const Uint8 type = SDLOP_DBus_MessageType(msg);
    if (type != SDLOP_DBUS_SIGNAL) {
        if (dt.theme_serial && SDLOP_DBus_MessageReplySerial(msg) == dt.theme_serial) {
            dt.theme_serial = 0;
            theme_from_reply(msg); /* the initial value: no event (stock) */
            return true;
        }
        return false;
    }
    const char *iface = SDLOP_DBus_MessageField(msg, 'i');
    const char *member = SDLOP_DBus_MessageField(msg, 'm');
    if (!iface || !member || strcmp(iface, PORTAL_SETTINGS) != 0 || strcmp(member, "SettingChanged") != 0) {
        return false;
    }
    SDLop_DBusIter it;
    const char *ns = NULL, *key = NULL;
    if (!SDLOP_DBus_IterInit(msg, &it) || !SDLOP_DBus_IterGetString(&it, &ns) || !SDLOP_DBus_IterNext(&it) ||
        !SDLOP_DBus_IterGetString(&it, &key) || !SDLOP_DBus_IterNext(&it) || strcmp(ns, APPEARANCE_NS) != 0 ||
        strcmp(key, COLOR_SCHEME_KEY) != 0) {
        return false;
    }
    SDL_SystemTheme t;
    if (!parse_color_scheme(&it, &t)) {
        return true;
    }
    dt.theme_serial = 0; /* newer than any pending read */
    if (t != dt.theme) {
        dt.theme = t;
        SDL_Event e;
        SDL_zero(e);
        e.common.type = SDL_EVENT_SYSTEM_THEME_CHANGED;
        e.common.timestamp = SDL_GetTicksNS();
        SDLOP_PushEventInternal(&e);
    }
    return true;
}

static bool ensure_handler(void)
{
    if (!SDLOP_DBus_Connect()) {
        return false;
    }
    if (!dt.handler) {
        dt.handler = SDLOP_DBus_AddHandler(desktop_handler, NULL);
    }
    return dt.handler;
}

/* from video init (Wayland, X11) and on first use: subscribe, then ask */
void SDLOP_Desktop_Init(void)
{
    if (dt.started) {
        return;
    }
    dt.started = true;
    if (!ensure_handler()) {
        return;
    }
    dt.match = SDLOP_DBus_AddMatch(THEME_MATCH); /* before reading: no change gets lost */
    SDLop_DBusMsg *m = SDLOP_DBus_NewMethodCall(PORTAL_DEST, PORTAL_PATH, PORTAL_SETTINGS, "Read", "ss");
    if (m) {
        SDLOP_DBus_PutString(m, APPEARANCE_NS);
        SDLOP_DBus_PutString(m, COLOR_SCHEME_KEY);
        dt.theme_serial = SDLOP_DBus_CallAsync(m);
        SDLOP_DBus_FreeMessage(m);
    }
}

SDL_SystemTheme SDLOP_Desktop_GetSystemTheme(void)
{
    SDLOP_Desktop_Init();
    if (dt.theme_serial) {
        const Uint32 serial = dt.theme_serial;
        dt.theme_serial = 0;
        SDLop_DBusMsg *reply = SDLOP_DBus_WaitReply(serial, 300);
        theme_from_reply(reply);
        SDLOP_DBus_FreeMessage(reply);
        SDL_ClearError(); /* no portal: unknown, not an error */
    }
    return dt.theme;
}

/* "/org/libsdl/<app id, [A-Za-z0-9_]>_<pid>" (stock) */
static void progress_object_path(char *out, size_t size)
{
    const char *app = SDLOP_GetAppID();
    char id[128];
    size_t n = 0;
    if (app[0] && !isalpha((unsigned char)app[0]) && app[0] != '_') {
        id[n++] = '_';
    }
    for (size_t i = 0; app[i] && n + 1 < sizeof(id); i++) {
        id[n++] = isalnum((unsigned char)app[i]) ? app[i] : '_';
    }
    id[n] = '\0';
    snprintf(out, size, "/org/libsdl/%s_%d", id, (int)getpid());
}

void SDLOP_Desktop_UpdateProgress(SDL_Window *window)
{
    if (!window || !SDLOP_DBus_Connect()) {
        return;
    }
    char path[192], uri[160];
    progress_object_path(path, sizeof(path));
    snprintf(uri, sizeof(uri), "%s.desktop", SDLOP_GetAppID());
    SDLop_DBusMsg *m = SDLOP_DBus_NewSignal(path, "com.canonical.Unity.LauncherEntry", "Update", "sa{sv}");
    if (!m) {
        return;
    }
    /* the launcher API only knows visible or not: every state but NONE
     * shows the bar (stock) */
    const bool visible =
        window->progress_state != SDL_PROGRESS_STATE_NONE && window->progress_state != SDL_PROGRESS_STATE_INVALID;
    SDLOP_DBus_PutString(m, uri);
    SDLop_DBusArray a = SDLOP_DBus_OpenArray(m, '{');
    SDLOP_DBus_PutDictVariantKey(m, "progress-visible", "b");
    SDLOP_DBus_PutBool(m, visible);
    SDLOP_DBus_PutDictVariantKey(m, "progress", "d");
    SDLOP_DBus_PutDouble(m, (double)window->progress_value);
    SDLOP_DBus_CloseArray(m, &a);
    SDLOP_DBus_Send(m);
    SDLOP_DBus_FreeMessage(m);
}

void SDLOP_Desktop_Quit(void)
{
    if (SDLOP_DBus_IsConnected()) {
        if (dt.match) {
            SDLOP_DBus_RemoveMatch(THEME_MATCH);
        }
    }
    if (dt.handler) {
        SDLOP_DBus_RemoveHandler(desktop_handler, NULL);
    }
    memset(&dt, 0, sizeof(dt));
}
