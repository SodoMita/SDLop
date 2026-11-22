/*
  SDLop test: the built-in session bus client (SDL_dbus.c) and the desktop
  integration on top of it (SDL_desktop.c, SDL_dialog.c), against a fake bus: a thread
  serving a unix socket with the real wire protocol (SASL EXTERNAL, Hello,
  AddMatch, the settings portal, signals). Dummy video driver.

  connection 1: SDL_GetSystemTheme() reads the portal (old Read reply
                nesting v(v(u)): dark); the SettingChanged match is added
                first; SettingChanged -> SDL_EVENT_SYSTEM_THEME_CHANGED
                (other keys ignored); progress -> Unity LauncherEntry
                Update signals; SDL_Quit closes the connection
  connection 2: the read is sent asynchronously (SDLOP_Desktop_Init, as at
                Wayland / X11 init) and its reply (v(u): light) arrives
                through SDL_PumpEvents, so SDL_GetSystemTheme() doesn't wait
  connection 3: no portal (error reply): unknown, no error left behind
  connection 4: file dialogs through the portal FileChooser: options
                (handle_token, multiple, directory, filters as
                case-insensitive globs, current_folder, modal), the match
                on the predicted request path added before the call, the
                Response (file URIs -> paths, current_filter -> index),
                cancel -> empty list; driver "portal" without a portal and
                "zenity" without zenity -> NULL + error; a fake zenity
                script (arguments, filters, exit-code environment,
                paths from its output, callback from its thread)
  SDLOP_NO_DBUS=1: no connection at all

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include "internal/sdlop_internal.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            failures++;                                          \
        }                                                        \
    } while (0)

/* ---- fake bus ---------------------------------------------------------- */

static int listen_fd = -1;
static char sock_path[108];
static atomic_int connections;
static atomic_int conn_closed;    /* connections that ended */
static atomic_int send_changed;   /* main -> bus: emit SettingChanged (value) */
static atomic_int read_calls;
static atomic_bool match_before_read;
static char match_rule[512];
static atomic_int progress_signals;
static char progress_path[256], progress_uri[256];
static atomic_bool progress_visible;
static double progress_value;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* file chooser */
static atomic_int fc_calls;
static char fc_method[32], fc_title[128], fc_token[64], fc_folder[256], fc_glob[128], fc_parent[128];
static int fc_nfilters;
static bool fc_multiple, fc_directory, fc_modal, fc_match_first;
static atomic_int fc_mode; /* 0: answer with files, 1: cancel, 2: no portal */
static Uint32 server_serial = 1000;

static bool send_msg(int fd, SDLop_DBusMsg *m)
{
    size_t len;
    SDLOP_DBus_SetSender(m, "org.freedesktop.DBus");
    Uint8 *b = SDLOP_DBus_Serialize(m, ++server_serial, &len);
    bool ok = b && write(fd, b, len) == (ssize_t)len;
    free(b);
    SDLOP_DBus_FreeMessage(m);
    return ok;
}

static void setting_changed(int fd, const char *ns, const char *key, Uint32 value)
{
    SDLop_DBusMsg *s = SDLOP_DBus_NewSignal("/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings",
                                           "SettingChanged", "ssv");
    SDLOP_DBus_PutString(s, ns);
    SDLOP_DBus_PutString(s, key);
    SDLOP_DBus_PutSignature(s, "u");
    SDLOP_DBus_PutU32(s, value);
    SDLOP_DBus_SetDestination(s, ":1.42");
    send_msg(fd, s);
}

static void handle(int fd, int conn, SDLop_DBusMsg *m)
{
    const char *member = SDLOP_DBus_MessageField(m, 'm');
    const char *iface = SDLOP_DBus_MessageField(m, 'i');
    const Uint8 type = SDLOP_DBus_MessageType(m);
    member = member ? member : "";
    iface = iface ? iface : "";
    SDLop_DBusIter it;
    if (type == SDLOP_DBUS_METHOD_CALL && strcmp(member, "Hello") == 0) {
        SDLop_DBusMsg *r = SDLOP_DBus_NewReply(m, "s");
        SDLOP_DBus_PutString(r, ":1.42");
        send_msg(fd, r);
    } else if (type == SDLOP_DBUS_METHOD_CALL && strcmp(member, "AddMatch") == 0) {
        const char *rule = "";
        SDLOP_DBus_IterInit(m, &it);
        SDLOP_DBus_IterGetString(&it, &rule);
        pthread_mutex_lock(&lock);
        snprintf(match_rule, sizeof(match_rule), "%s", rule);
        pthread_mutex_unlock(&lock);
    } else if (type == SDLOP_DBUS_METHOD_CALL && strcmp(iface, "org.freedesktop.portal.Settings") == 0 &&
               strcmp(member, "Read") == 0) {
        const char *ns = "", *key = "";
        SDLOP_DBus_IterInit(m, &it);
        SDLOP_DBus_IterGetString(&it, &ns);
        SDLOP_DBus_IterNext(&it);
        SDLOP_DBus_IterGetString(&it, &key);
        atomic_fetch_add(&read_calls, 1);
        pthread_mutex_lock(&lock);
        atomic_store(&match_before_read, strstr(match_rule, "SettingChanged") != NULL);
        pthread_mutex_unlock(&lock);
        if (strcmp(ns, "org.freedesktop.appearance") != 0 || strcmp(key, "color-scheme") != 0 || conn == 3) {
            send_msg(fd, SDLOP_DBus_NewError(m, "org.freedesktop.portal.Error.NotFound", "no such setting"));
        } else if (conn == 1) {
            SDLop_DBusMsg *r = SDLOP_DBus_NewReply(m, "v"); /* Read: v(v(u)) */
            SDLOP_DBus_PutSignature(r, "v");
            SDLOP_DBus_PutSignature(r, "u");
            SDLOP_DBus_PutU32(r, 1);
            send_msg(fd, r);
        } else {
            SDLop_DBusMsg *r = SDLOP_DBus_NewReply(m, "v"); /* ReadOne-style v(u) */
            SDLOP_DBus_PutSignature(r, "u");
            SDLOP_DBus_PutU32(r, 2);
            send_msg(fd, r);
        }
    } else if (type == SDLOP_DBUS_METHOD_CALL && strcmp(iface, "org.freedesktop.portal.FileChooser") == 0) {
        const int mode = atomic_load(&fc_mode);
        if (mode == 2) {
            send_msg(fd, SDLOP_DBus_NewError(m, "org.freedesktop.DBus.Error.ServiceUnknown", "no portal"));
            return;
        }
        const char *parent = "", *title = "", *key;
        SDLop_DBusIter arr, ent, var, sub, f, g;
        pthread_mutex_lock(&lock);
        snprintf(fc_method, sizeof(fc_method), "%s", member);
        fc_nfilters = 0;
        fc_multiple = fc_directory = fc_modal = false;
        fc_folder[0] = fc_glob[0] = fc_token[0] = '\0';
        SDLOP_DBus_IterInit(m, &it);
        SDLOP_DBus_IterGetString(&it, &parent);
        SDLOP_DBus_IterNext(&it);
        SDLOP_DBus_IterGetString(&it, &title);
        snprintf(fc_parent, sizeof(fc_parent), "%s", parent);
        snprintf(fc_title, sizeof(fc_title), "%s", title);
        SDLOP_DBus_IterNext(&it);
        if (SDLOP_DBus_IterRecurse(&it, &arr)) {
            while (SDLOP_DBus_IterType(&arr)) {
                if (SDLOP_DBus_IterRecurse(&arr, &ent) && SDLOP_DBus_IterGetString(&ent, &key) &&
                    SDLOP_DBus_IterNext(&ent) && SDLOP_DBus_IterRecurse(&ent, &var)) {
                    const char *str;
                    if (strcmp(key, "handle_token") == 0 && SDLOP_DBus_IterGetString(&var, &str)) {
                        snprintf(fc_token, sizeof(fc_token), "%s", str);
                    } else if (strcmp(key, "multiple") == 0) {
                        SDLOP_DBus_IterGetBool(&var, &fc_multiple);
                    } else if (strcmp(key, "directory") == 0) {
                        SDLOP_DBus_IterGetBool(&var, &fc_directory);
                    } else if (strcmp(key, "modal") == 0) {
                        SDLOP_DBus_IterGetBool(&var, &fc_modal);
                    } else if (strcmp(key, "current_folder") == 0 && SDLOP_DBus_IterRecurse(&var, &sub)) {
                        size_t n = 0;
                        Uint8 b;
                        while (SDLOP_DBus_IterGetByte(&sub, &b) && n + 1 < sizeof(fc_folder)) {
                            fc_folder[n++] = (char)b;
                            if (!SDLOP_DBus_IterNext(&sub)) {
                                break;
                            }
                        }
                        fc_folder[n] = '\0';
                    } else if (strcmp(key, "filters") == 0 && SDLOP_DBus_IterRecurse(&var, &sub)) {
                        while (SDLOP_DBus_IterType(&sub)) {
                            fc_nfilters++;
                            const char *glob;
                            if (fc_nfilters == 2 && SDLOP_DBus_IterRecurse(&sub, &f) && SDLOP_DBus_IterNext(&f) &&
                                SDLOP_DBus_IterRecurse(&f, &g) && SDLOP_DBus_IterRecurse(&g, &f) &&
                                SDLOP_DBus_IterNext(&f) && SDLOP_DBus_IterGetString(&f, &glob)) {
                                snprintf(fc_glob, sizeof(fc_glob), "%s", glob); /* filter 2, glob 1 */
                            }
                            if (!SDLOP_DBus_IterNext(&sub)) {
                                break;
                            }
                        }
                    }
                }
                if (!SDLOP_DBus_IterNext(&arr)) {
                    break;
                }
            }
        }
        char path[256];
        snprintf(path, sizeof(path), "/org/freedesktop/portal/desktop/request/1_42/%s", fc_token);
        fc_match_first = strstr(match_rule, path) != NULL;
        pthread_mutex_unlock(&lock);
        SDLop_DBusMsg *r = SDLOP_DBus_NewReply(m, "o");
        SDLOP_DBus_PutString(r, path);
        send_msg(fd, r);
        /* the user picks */
        SDLop_DBusMsg *sig = SDLOP_DBus_NewSignal(path, "org.freedesktop.portal.Request", "Response", "ua{sv}");
        SDLOP_DBus_SetDestination(sig, ":1.42");
        SDLOP_DBus_PutU32(sig, mode == 1 ? 1 : 0);
        SDLop_DBusArray a = SDLOP_DBus_OpenArray(sig, '{');
        if (mode == 0) {
            SDLOP_DBus_PutDictVariantKey(sig, "uris", "as");
            SDLop_DBusArray u = SDLOP_DBus_OpenArray(sig, 's');
            SDLOP_DBus_PutString(sig, "file:///tmp/a%20b.txt");
            SDLOP_DBus_PutString(sig, "file:///tmp/c.png");
            SDLOP_DBus_CloseArray(sig, &u);
            SDLOP_DBus_PutDictVariantKey(sig, "current_filter", "(sa(us))");
            SDLOP_DBus_OpenStruct(sig);
            SDLOP_DBus_PutString(sig, "Images");
            SDLop_DBusArray gl = SDLOP_DBus_OpenArray(sig, '(');
            SDLOP_DBus_OpenStruct(sig);
            SDLOP_DBus_PutU32(sig, 0);
            SDLOP_DBus_PutString(sig, "*.[pP][nN][gG]");
            SDLOP_DBus_CloseArray(sig, &gl);
        }
        SDLOP_DBus_CloseArray(sig, &a);
        send_msg(fd, sig);
        atomic_fetch_add(&fc_calls, 1);
    } else if (type == SDLOP_DBUS_SIGNAL && strcmp(iface, "com.canonical.Unity.LauncherEntry") == 0 &&
               strcmp(member, "Update") == 0) {
        const char *uri = "", *key = "";
        SDLop_DBusIter arr, ent, var;
        bool visible = false;
        double value = -1.0;
        SDLOP_DBus_IterInit(m, &it);
        SDLOP_DBus_IterGetString(&it, &uri);
        SDLOP_DBus_IterNext(&it);
        if (SDLOP_DBus_IterRecurse(&it, &arr)) {
            while (SDLOP_DBus_IterType(&arr)) {
                if (SDLOP_DBus_IterRecurse(&arr, &ent) && SDLOP_DBus_IterGetString(&ent, &key) &&
                    SDLOP_DBus_IterNext(&ent) && SDLOP_DBus_IterRecurse(&ent, &var)) {
                    if (strcmp(key, "progress-visible") == 0) {
                        SDLOP_DBus_IterGetBool(&var, &visible);
                    } else if (strcmp(key, "progress") == 0) {
                        SDLOP_DBus_IterGetDouble(&var, &value);
                    }
                }
                if (!SDLOP_DBus_IterNext(&arr)) {
                    break;
                }
            }
        }
        pthread_mutex_lock(&lock);
        snprintf(progress_path, sizeof(progress_path), "%s", SDLOP_DBus_MessageField(m, 'p'));
        snprintf(progress_uri, sizeof(progress_uri), "%s", uri);
        progress_value = value;
        atomic_store(&progress_visible, visible);
        pthread_mutex_unlock(&lock);
        atomic_fetch_add(&progress_signals, 1);
    }
}

static void serve(int fd, int conn)
{
    /* SASL: NUL, AUTH EXTERNAL <hex uid>, then BEGIN */
    char line[512];
    size_t n = 0;
    while (n < sizeof(line) - 1) {
        ssize_t r = read(fd, line + n, 1);
        if (r <= 0) {
            return;
        }
        n++;
        if (n >= 2 && line[n - 2] == '\r' && line[n - 1] == '\n') {
            break;
        }
    }
    line[n] = '\0';
    if (line[0] != '\0' || strncmp(line + 1, "AUTH EXTERNAL ", 14) != 0) {
        fprintf(stderr, "fake bus: bad auth '%s'\n", line + 1);
        return;
    }
    const char *ok = "OK 0123456789abcdef0123456789abcdef\r\n";
    if (write(fd, ok, strlen(ok)) < 0) {
        return;
    }
    Uint8 *buf = malloc(65536);
    size_t len = 0;
    bool begun = false;
    for (;;) {
        const int v = atomic_exchange(&send_changed, 0);
        if (v) {
            setting_changed(fd, "org.freedesktop.appearance", "accent-color", 7); /* not ours */
            setting_changed(fd, "org.freedesktop.appearance", "color-scheme", (Uint32)v);
        }
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 10) <= 0) {
            continue;
        }
        ssize_t r = read(fd, buf + len, 65536 - len);
        if (r <= 0) {
            break;
        }
        len += (size_t)r;
        if (!begun) {
            if (len < 7 || memcmp(buf, "BEGIN\r\n", 7) != 0) {
                continue;
            }
            memmove(buf, buf + 7, len - 7);
            len -= 7;
            begun = true;
        }
        for (;;) {
            SDLop_DBusMsg *m;
            size_t used;
            int pr = SDLOP_DBus_Parse(buf, len, &m, &used);
            if (pr <= 0) {
                if (pr < 0) {
                    fprintf(stderr, "fake bus: parse error\n");
                    len = 0;
                }
                break;
            }
            handle(fd, conn, m);
            SDLOP_DBus_FreeMessage(m);
            memmove(buf, buf + used, len - used);
            len -= used;
        }
    }
    free(buf);
}

static void *bus_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return NULL;
        }
        const int conn = atomic_fetch_add(&connections, 1) + 1;
        serve(fd, conn);
        close(fd);
        atomic_fetch_add(&conn_closed, 1);
    }
}

/* ---- test -------------------------------------------------------------- */

static struct
{
    atomic_int called;
    bool null_list;
    int n, filter;
    char files[4][256];
    char error[256];
} dlg;

static void SDLCALL dialog_cb(void *userdata, const char *const *list, int filter)
{
    (void)userdata;
    dlg.null_list = list == NULL;
    dlg.n = 0;
    for (int i = 0; list && list[i] && i < 4; i++) {
        snprintf(dlg.files[dlg.n++], sizeof(dlg.files[0]), "%s", list[i]);
    }
    dlg.filter = filter;
    snprintf(dlg.error, sizeof(dlg.error), "%s", list ? "" : SDL_GetError());
    atomic_fetch_add(&dlg.called, 1);
}

static bool wait_for(atomic_int *v, int want, int ms)
{
    for (int i = 0; i < ms / 5 && atomic_load(v) < want; i++) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
    return atomic_load(v) >= want;
}

int main(void)
{
    char dir[] = "/tmp/sdlop-dbus-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(sock_path, sizeof(sock_path), "%s/bus", dir);
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path);
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(listen_fd, 4) < 0) {
        perror("fake bus");
        return 1;
    }
    pthread_t th;
    pthread_create(&th, NULL, bus_thread, NULL);

    char addr[160];
    snprintf(addr, sizeof(addr), "unix:path=%s", sock_path);
    setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    SDL_SetHint(SDL_HINT_APP_ID, "org.example.Test-App");

    /* --- 1: synchronous first read, changes, progress ------------------- */
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init: %s", SDL_GetError());
    CHECK(atomic_load(&connections) == 0, "dummy driver: no bus connection at init");
    SDL_SystemTheme t = SDL_GetSystemTheme();
    CHECK(t == SDL_SYSTEM_THEME_DARK, "portal Read v(v(1)) = dark (got %d)", (int)t);
    CHECK(atomic_load(&connections) == 1, "one connection (%d)", atomic_load(&connections));
    CHECK(atomic_load(&match_before_read), "SettingChanged match added before the read ('%s')", match_rule);
    CHECK(strstr(match_rule, "arg0='org.freedesktop.appearance'") && strstr(match_rule, "arg1='color-scheme'"),
          "match rule filters on the key ('%s')", match_rule);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

    atomic_store(&send_changed, 2);
    int changed = 0;
    for (int i = 0; i < 200 && !changed; i++) {
        SDL_PumpEvents();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            changed += e.type == SDL_EVENT_SYSTEM_THEME_CHANGED;
        }
        SDL_Delay(5);
    }
    SDL_Delay(50);
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        changed += e.type == SDL_EVENT_SYSTEM_THEME_CHANGED;
    }
    CHECK(changed == 1, "one SDL_EVENT_SYSTEM_THEME_CHANGED (%d)", changed);
    CHECK(SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT, "theme light after SettingChanged");
    atomic_store(&send_changed, 2); /* same value again: no event */
    SDL_Delay(100);
    SDL_PumpEvents();
    changed = 0;
    while (SDL_PollEvent(&e)) {
        changed += e.type == SDL_EVENT_SYSTEM_THEME_CHANGED;
    }
    CHECK(changed == 0, "unchanged value: no event (%d)", changed);
    /* a change while blocked in SDL_WaitEvent wakes it (the bus fd is polled) */
    atomic_store(&send_changed, 1);
    {
        const Uint64 t0 = SDL_GetTicks();
        bool got = false;
        while (!got && SDL_GetTicks() - t0 < 2000 && SDL_WaitEventTimeout(&e, 2000)) {
            got = e.type == SDL_EVENT_SYSTEM_THEME_CHANGED;
        }
        CHECK(got, "SDL_WaitEventTimeout returns the theme change");
        CHECK(SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK, "theme dark again");
    }

    SDL_Window *w = SDL_CreateWindow("dbus", 64, 64, 0);
    CHECK(w != NULL, "window");
    CHECK(SDL_SetWindowProgressState(w, SDL_PROGRESS_STATE_NORMAL), "progress state");
    CHECK(SDL_SetWindowProgressValue(w, 0.5f), "progress value");
    CHECK(wait_for(&progress_signals, 2, 2000), "two Update signals (%d)", atomic_load(&progress_signals));
    pthread_mutex_lock(&lock);
    char want_path[128];
    snprintf(want_path, sizeof(want_path), "/org/libsdl/org_example_Test_App_%d", (int)getpid());
    CHECK(strcmp(progress_path, want_path) == 0, "object path '%s' (want '%s')", progress_path, want_path);
    CHECK(strcmp(progress_uri, "org.example.Test-App.desktop") == 0, "app uri '%s'", progress_uri);
    CHECK(atomic_load(&progress_visible) && progress_value > 0.49 && progress_value < 0.51,
          "visible at 0.5 (%d, %g)", (int)atomic_load(&progress_visible), progress_value);
    pthread_mutex_unlock(&lock);
    CHECK(SDL_SetWindowProgressState(w, SDL_PROGRESS_STATE_NONE), "progress none");
    CHECK(wait_for(&progress_signals, 3, 2000), "third Update signal");
    CHECK(!atomic_load(&progress_visible), "state NONE hides the bar");
    SDL_DestroyWindow(w);
    SDL_Quit();
    CHECK(wait_for(&conn_closed, 1, 2000), "SDL_Quit closes the bus connection");

    /* --- 2: asynchronous read (as at Wayland/X11 init) ------------------ */
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init 2");
    SDLOP_Desktop_Init();
    CHECK(atomic_load(&connections) == 2, "second connection");
    CHECK(wait_for(&read_calls, 2, 2000), "async Read sent");
    SDL_Delay(50);
    SDL_PumpEvents(); /* the reply is taken by the pump */
    const Uint64 t0 = SDL_GetTicksNS();
    t = SDL_GetSystemTheme();
    const Uint64 dt_ms = (SDL_GetTicksNS() - t0) / 1000000;
    CHECK(t == SDL_SYSTEM_THEME_LIGHT, "async read v(u=2) = light (got %d)", (int)t);
    CHECK(dt_ms < 50, "no wait once the reply was pumped (%u ms)", (unsigned)dt_ms);
    SDL_Quit();
    CHECK(wait_for(&conn_closed, 2, 2000), "connection 2 closed");

    /* --- 3: no portal ---------------------------------------------------- */
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init 3");
    SDL_ClearError();
    t = SDL_GetSystemTheme();
    CHECK(t == SDL_SYSTEM_THEME_UNKNOWN, "portal error: unknown (got %d)", (int)t);
    CHECK(SDL_GetError()[0] == '\0', "no error left behind ('%s')", SDL_GetError());
    SDL_Quit();
    CHECK(wait_for(&conn_closed, 3, 2000), "connection 3 closed");

    /* --- 4: file dialogs ------------------------------------------------- */
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init dialogs");
    const SDL_DialogFileFilter filters[] = { { "Text", "txt;md" }, { "Images", "png;JPG" }, { "All", "*" } };
    SDL_ShowOpenFileDialog(dialog_cb, NULL, NULL, filters, 3, "/tmp", true);
    CHECK(wait_for(&dlg.called, 1, 3000), "open dialog: callback");
    pthread_mutex_lock(&lock);
    CHECK(strcmp(fc_method, "OpenFile") == 0 && strcmp(fc_title, "Open File") == 0, "OpenFile '%s' '%s'", fc_method,
          fc_title);
    CHECK(fc_match_first, "Response match on the request path added before the call");
    CHECK(fc_multiple && !fc_directory && !fc_modal && fc_parent[0] == '\0', "options: multiple, no parent");
    CHECK(fc_nfilters == 3 && strcmp(fc_glob, "*.[pP][nN][gG]") == 0, "filters (%d, '%s')", fc_nfilters, fc_glob);
    CHECK(strcmp(fc_folder, "/tmp") == 0, "current_folder '%s'", fc_folder);
    pthread_mutex_unlock(&lock);
    CHECK(!dlg.null_list && dlg.n == 2 && strcmp(dlg.files[0], "/tmp/a b.txt") == 0 &&
              strcmp(dlg.files[1], "/tmp/c.png") == 0,
          "files (%d: '%s' '%s')", dlg.n, dlg.files[0], dlg.files[1]);
    CHECK(dlg.filter == 1, "chosen filter index (%d)", dlg.filter);

    atomic_store(&fc_mode, 1);
    /* a parent window exported over xdg-foreign (set by hand on the dummy
     * driver's window): "wayland:<handle>" (stock) */
    SDL_Window *pw = SDL_CreateWindow("parent", 32, 32, 0);
    CHECK(pw && SDL_SetStringProperty(SDL_GetWindowProperties(pw),
                                      SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_EXPORT_HANDLE_STRING, "abc-123"),
          "parent window with an export handle");
    SDL_PropertiesID dp = SDL_CreateProperties();
    SDL_SetStringProperty(dp, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Pick a folder");
    SDL_SetPointerProperty(dp, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, pw);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, dialog_cb, NULL, dp);
    SDL_DestroyProperties(dp);
    CHECK(wait_for(&dlg.called, 2, 3000), "folder dialog: callback");
    CHECK(fc_directory && strcmp(fc_title, "Pick a folder") == 0, "folder: directory option, title '%s'", fc_title);
    CHECK(fc_modal && strcmp(fc_parent, "wayland:abc-123") == 0, "folder: modal to 'wayland:abc-123' ('%s')", fc_parent);
    SDL_DestroyWindow(pw);
    CHECK(!dlg.null_list && dlg.n == 0, "cancel: empty list (null %d, n %d)", (int)dlg.null_list, dlg.n);

    atomic_store(&fc_mode, 2);
    SDL_SetHint(SDL_HINT_FILE_DIALOG_DRIVER, "portal");
    SDL_ShowSaveFileDialog(dialog_cb, NULL, NULL, NULL, 0, NULL);
    CHECK(atomic_load(&dlg.called) == 3 && dlg.null_list && strstr(dlg.error, "unsupported"),
          "portal driver without portal: NULL + error ('%s')", dlg.error);
    const char *old_path = getenv("PATH");
    char *saved_path = old_path ? strdup(old_path) : NULL;
    setenv("PATH", "/nonexistent", 1);
    SDL_SetHint(SDL_HINT_FILE_DIALOG_DRIVER, "zenity");
    SDL_ShowSaveFileDialog(dialog_cb, NULL, NULL, NULL, 0, NULL);
    CHECK(atomic_load(&dlg.called) == 4 && dlg.null_list, "zenity driver without zenity: NULL");
    /* a fake zenity: records its arguments, picks two files */
    char zdir[300], zscript[340], zargs[340];
    snprintf(zdir, sizeof(zdir), "%s/zbin", dir);
    snprintf(zscript, sizeof(zscript), "%s/zenity", zdir);
    snprintf(zargs, sizeof(zargs), "%s/args", dir);
    mkdir(zdir, 0700);
    FILE *zf = fopen(zscript, "w");
    if (zf) {
        fprintf(zf,
                "#!/bin/sh\n"
                "if [ \"$1\" = --version ]; then echo 4.0.1; exit 0; fi\n"
                "printf '%%s\\n' \"$@\" \"OK=$ZENITY_OK\" > '%s'\n"
                "printf '/tmp/x y\\n/tmp/z\\n'\n",
                zargs);
        fclose(zf);
        chmod(zscript, 0700);
    }
    setenv("PATH", zdir, 1);
    SDL_ShowOpenFileDialog(dialog_cb, NULL, NULL, filters, 3, "/tmp/", true);
    CHECK(wait_for(&dlg.called, 5, 5000), "zenity: callback (from its thread)");
    CHECK(!dlg.null_list && dlg.n == 2 && strcmp(dlg.files[0], "/tmp/x y") == 0 && strcmp(dlg.files[1], "/tmp/z") == 0,
          "zenity: files (%d '%s' '%s', error '%s')", dlg.n, dlg.files[0], dlg.files[1], dlg.error);
    char zout[2048] = "";
    zf = fopen(zargs, "r");
    if (zf) {
        size_t zn = fread(zout, 1, sizeof(zout) - 1, zf);
        zout[zn] = '\0';
        fclose(zf);
    }
    CHECK(strstr(zout, "--file-selection\n") && strstr(zout, "--multiple\n") && strstr(zout, "--filename\n/tmp/\n") &&
              strstr(zout, "--file-filter=Images | *.[pP][nN][gG] *.[jJ][pP][gG]\n") &&
              strstr(zout, "--file-filter=All | *\n") && strstr(zout, "OK=0"),
          "zenity arguments:\n%s", zout);
    unlink(zscript);
    unlink(zargs);
    rmdir(zdir);
    if (saved_path) {
        setenv("PATH", saved_path, 1);
    }

    const SDL_DialogFileFilter bad[] = { { "Bad", "*.png" } };
    SDL_ShowOpenFileDialog(dialog_cb, NULL, NULL, bad, 1, NULL, false);
    free(saved_path);
    CHECK(atomic_load(&dlg.called) == 6 && dlg.null_list && strstr(dlg.error, "Invalid dialog file filters"),
          "invalid pattern rejected ('%s')", dlg.error);
    SDL_ResetHint(SDL_HINT_FILE_DIALOG_DRIVER);
    SDL_Quit();
    CHECK(wait_for(&conn_closed, 4, 2000), "connection 4 closed");

    /* --- SDLOP_NO_DBUS ----------------------------------------------------- */
    setenv("SDLOP_NO_DBUS", "1", 1);
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init 4");
    t = SDL_GetSystemTheme();
    w = SDL_CreateWindow("dbus", 64, 64, 0);
    SDL_SetWindowProgressValue(w, 0.25f);
    SDL_Delay(50);
    CHECK(t == SDL_SYSTEM_THEME_UNKNOWN && atomic_load(&connections) == 4, "SDLOP_NO_DBUS: no connection (%d)",
          atomic_load(&connections));
    SDL_DestroyWindow(w);
    SDL_Quit();

    shutdown(listen_fd, SHUT_RDWR);
    close(listen_fd);
    unlink(sock_path);
    rmdir(dir);
    if (failures) {
        printf("test_dbus: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_dbus: PASS\n");
    return 0;
}
