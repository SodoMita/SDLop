/*
  SDLop test: the built-in session bus client (SDL_dbus.c) and the desktop
  integration on top of it (SDL_desktop.c), against a fake bus: a thread
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

    /* --- SDLOP_NO_DBUS ----------------------------------------------------- */
    setenv("SDLOP_NO_DBUS", "1", 1);
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init 4");
    t = SDL_GetSystemTheme();
    w = SDL_CreateWindow("dbus", 64, 64, 0);
    SDL_SetWindowProgressValue(w, 0.25f);
    SDL_Delay(50);
    CHECK(t == SDL_SYSTEM_THEME_UNKNOWN && atomic_load(&connections) == 3, "SDLOP_NO_DBUS: no connection (%d)",
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
