/*
  SDLop test: the D-Bus input method clients (SDL_ime_dbus.c) against a fake
  session bus that plays Fcitx 5 and IBus. Dummy video driver; the hooks the
  Wayland driver calls without text-input-v3 are called directly.

  connection 1: XMODIFIERS=@im=fcitx -> Fcitx: nothing on the bus before text
                input starts; CreateInputContext a(ss) with the program name;
                signal match on the context; SetCapability t (preedit +
                formatted with IME_IMPLEMENTED_UI=composition); FocusIn;
                SetCursorRect from the text input area; keys go through
                ProcessKeyEvent (keysym, keycode, state, release, time): a
                handled key still sends its key event but no text, an
                unhandled one types normally; CommitString -> TEXT_INPUT;
                UpdateFormattedPreedit -> EDITING (highlighted segment as the
                selection, else the byte cursor as a character index);
                ClearComposition -> Reset; stop -> FocusOut and no more
                ProcessKeyEvent; quit -> DestroyIC
  connection 2: SDL_IM_MODULE=ibus -> IBus portal: CreateInputContext s,
                SetCapabilities u, ProcessKeyEvent (keysym, evdev code,
                state with the release bit), SetCursorLocation, CommitText
                v(IBusText), UpdatePreeditText with a background attribute
                (selection) and with only the cursor, Hide/ShowPreeditText,
                quit -> Destroy
  connection 3: nothing named and no input method running: NameHasOwner only,
                no input context, typing unaffected
  SDL_IM_MODULE=none: no connection at all

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include "internal/sdlop_internal.h"
#include <errno.h>
#include <stdarg.h>
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

#define FCITX_IC "/org/freedesktop/portal/inputcontext/7"
#define IBUS_IC "/org/freedesktop/IBus/InputContext_3"

/* ---- fake bus ---------------------------------------------------------- */

static int listen_fd = -1;
static atomic_int connections;
static atomic_int conn_closed;
static atomic_int send_signal; /* main -> bus: which signal to emit (1-based) */
static atomic_int signals_sent;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char calls[4096];       /* "Member(args) " log of input method calls */
static char match_rule[512];
static char program[128];
static Uint32 server_serial = 1000;

static void log_call(const char *fmt, ...)
{
    char e[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e, sizeof(e), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&lock);
    if (strlen(calls) + strlen(e) + 2 < sizeof(calls)) {
        strcat(calls, e);
        strcat(calls, " ");
    }
    pthread_mutex_unlock(&lock);
}

static void take_calls(char *out, size_t n)
{
    pthread_mutex_lock(&lock);
    snprintf(out, n, "%s", calls);
    calls[0] = '\0';
    pthread_mutex_unlock(&lock);
}

static bool send_msg(int fd, SDLop_DBusMsg *m)
{
    size_t len;
    SDLOP_DBus_SetSender(m, ":1.7");
    Uint8 *b = SDLOP_DBus_Serialize(m, ++server_serial, &len);
    bool ok = b && send(fd, b, len, MSG_NOSIGNAL) == (ssize_t)len;
    free(b);
    SDLOP_DBus_FreeMessage(m);
    return ok;
}

/* IBusText as a variant: v (s "IBusText", a{sv}, s text, v IBusAttrList) where
 * the list holds one background attribute [start, end) when end > start */
static void put_ibus_text(SDLop_DBusMsg *s, const char *text, Uint32 start, Uint32 end)
{
    SDLOP_DBus_PutSignature(s, "(sa{sv}sv)");
    SDLOP_DBus_OpenStruct(s);
    SDLOP_DBus_PutString(s, "IBusText");
    SDLop_DBusArray att = SDLOP_DBus_OpenArray(s, '{');
    SDLOP_DBus_CloseArray(s, &att);
    SDLOP_DBus_PutString(s, text);
    SDLOP_DBus_PutSignature(s, "(sa{sv}av)");
    SDLOP_DBus_OpenStruct(s);
    SDLOP_DBus_PutString(s, "IBusAttrList");
    att = SDLOP_DBus_OpenArray(s, '{');
    SDLOP_DBus_CloseArray(s, &att);
    SDLop_DBusArray list = SDLOP_DBus_OpenArray(s, 'v');
    /* an underline first (type 1), which is not the selection */
    const Uint32 attrs[2][4] = { { 1, 1, 0, 1 }, { 3, 0x00ff00, start, end } };
    for (int i = 0; i < (end > start ? 2 : 1); i++) {
        SDLOP_DBus_PutSignature(s, "(sa{sv}uuuu)");
        SDLOP_DBus_OpenStruct(s);
        SDLOP_DBus_PutString(s, "IBusAttribute");
        att = SDLOP_DBus_OpenArray(s, '{');
        SDLOP_DBus_CloseArray(s, &att);
        for (int j = 0; j < 4; j++) {
            SDLOP_DBus_PutU32(s, attrs[i][j]);
        }
    }
    SDLOP_DBus_CloseArray(s, &list);
}

static void emit(int fd, int which)
{
    SDLop_DBusMsg *s = NULL;
    switch (which) {
    case 1: /* Fcitx commit */
        s = SDLOP_DBus_NewSignal(FCITX_IC, "org.fcitx.Fcitx.InputContext1", "CommitString", "s");
        SDLOP_DBus_PutString(s, "\xe4\xbd\xa0\xe5\xa5\xbd"); /* 你好 */
        break;
    case 2: /* Fcitx preedit: "ni" plain, "hao" highlighted (bit 4) */
    case 3: /* Fcitx preedit without highlight: cursor at byte 5 of "ni你x" (after 你) */
    {
        s = SDLOP_DBus_NewSignal(FCITX_IC, "org.fcitx.Fcitx.InputContext1", "UpdateFormattedPreedit", "a(si)i");
        SDLop_DBusArray a = SDLOP_DBus_OpenArray(s, '(');
        SDLOP_DBus_OpenStruct(s);
        SDLOP_DBus_PutString(s, "ni");
        SDLOP_DBus_PutU32(s, 8); /* underline */
        SDLOP_DBus_OpenStruct(s);
        SDLOP_DBus_PutString(s, which == 2 ? "hao" : "\xe4\xbd\xa0x");
        SDLOP_DBus_PutU32(s, which == 2 ? (1u << 4) | 8 : 8);
        SDLOP_DBus_CloseArray(s, &a);
        SDLOP_DBus_PutU32(s, 5);
        break;
    }
    case 4: /* Fcitx preedit cleared */
    {
        s = SDLOP_DBus_NewSignal(FCITX_IC, "org.fcitx.Fcitx.InputContext1", "UpdateFormattedPreedit", "a(si)i");
        SDLop_DBusArray a = SDLOP_DBus_OpenArray(s, '(');
        SDLOP_DBus_CloseArray(s, &a);
        SDLOP_DBus_PutU32(s, 0);
        break;
    }
    case 5: /* a signal of another context: ignored */
        s = SDLOP_DBus_NewSignal("/org/freedesktop/portal/inputcontext/99", "org.fcitx.Fcitx.InputContext1",
                                 "CommitString", "s");
        SDLOP_DBus_PutString(s, "other");
        break;
    case 11: /* IBus commit */
        s = SDLOP_DBus_NewSignal(IBUS_IC, "org.freedesktop.IBus.InputContext", "CommitText", "v");
        put_ibus_text(s, "\xe6\x97\xa5\xe6\x9c\xac", 0, 0); /* 日本 */
        break;
    case 12: /* IBus preedit with a selection [1, 3) */
        s = SDLOP_DBus_NewSignal(IBUS_IC, "org.freedesktop.IBus.InputContext", "UpdatePreeditText", "vub");
        put_ibus_text(s, "nihon", 1, 3);
        SDLOP_DBus_PutU32(s, 4);
        SDLOP_DBus_PutBool(s, true);
        break;
    case 13: /* IBus preedit, cursor only */
        s = SDLOP_DBus_NewSignal(IBUS_IC, "org.freedesktop.IBus.InputContext", "UpdatePreeditText", "vub");
        put_ibus_text(s, "ka", 0, 0);
        SDLOP_DBus_PutU32(s, 2);
        SDLOP_DBus_PutBool(s, true);
        break;
    case 14:
        s = SDLOP_DBus_NewSignal(IBUS_IC, "org.freedesktop.IBus.InputContext", "HidePreeditText", "");
        break;
    case 15:
        s = SDLOP_DBus_NewSignal(IBUS_IC, "org.freedesktop.IBus.InputContext", "ShowPreeditText", "");
        break;
    default:
        return;
    }
    send_msg(fd, s);
}

static void reply_bool(int fd, SDLop_DBusMsg *m, bool v)
{
    SDLop_DBusMsg *r = SDLOP_DBus_NewReply(m, "b");
    SDLOP_DBus_PutBool(r, v);
    send_msg(fd, r);
}

static void handle(int fd, int conn, SDLop_DBusMsg *m)
{
    const char *member = SDLOP_DBus_MessageField(m, 'm');
    const char *iface = SDLOP_DBus_MessageField(m, 'i');
    const char *path = SDLOP_DBus_MessageField(m, 'p');
    const char *dest = SDLOP_DBus_MessageField(m, 'd');
    member = member ? member : "";
    iface = iface ? iface : "";
    path = path ? path : "";
    dest = dest ? dest : "";
    SDLop_DBusIter it;
    SDLOP_DBus_IterInit(m, &it);
    if (SDLOP_DBus_MessageType(m) != SDLOP_DBUS_METHOD_CALL) {
        return;
    }
    if (strcmp(member, "Hello") == 0) {
        SDLop_DBusMsg *r = SDLOP_DBus_NewReply(m, "s");
        SDLOP_DBus_PutString(r, ":1.42");
        send_msg(fd, r);
    } else if (strcmp(member, "AddMatch") == 0 || strcmp(member, "RemoveMatch") == 0) {
        const char *rule = "";
        SDLOP_DBus_IterGetString(&it, &rule);
        if (member[0] == 'A') {
            pthread_mutex_lock(&lock);
            snprintf(match_rule, sizeof(match_rule), "%s", rule);
            pthread_mutex_unlock(&lock);
        }
        log_call("%s", member);
    } else if (strcmp(member, "NameHasOwner") == 0) {
        const char *name = "";
        SDLOP_DBus_IterGetString(&it, &name);
        log_call("NameHasOwner(%s)", name);
        reply_bool(fd, m, false);
    } else if (strcmp(iface, "org.fcitx.Fcitx.InputMethod1") == 0 && strcmp(member, "CreateInputContext") == 0) {
        SDLop_DBusIter arr, st;
        const char *k = "", *v = "";
        if (SDLOP_DBus_IterRecurse(&it, &arr) && SDLOP_DBus_IterRecurse(&arr, &st)) {
            SDLOP_DBus_IterGetString(&st, &k);
            SDLOP_DBus_IterNext(&st);
            SDLOP_DBus_IterGetString(&st, &v);
        }
        pthread_mutex_lock(&lock);
        snprintf(program, sizeof(program), "%s=%s", k, v);
        pthread_mutex_unlock(&lock);
        log_call("Create(%s)", dest);
        SDLop_DBusMsg *r = SDLOP_DBus_NewReply(m, "oay");
        SDLOP_DBus_PutString(r, FCITX_IC);
        SDLop_DBusArray uuid = SDLOP_DBus_OpenArray(r, 'y');
        SDLOP_DBus_CloseArray(r, &uuid);
        send_msg(fd, r);
    } else if (strcmp(iface, "org.freedesktop.IBus.Portal") == 0 && strcmp(member, "CreateInputContext") == 0) {
        const char *name = "";
        SDLOP_DBus_IterGetString(&it, &name);
        log_call("Create(%s,%s)", dest, name);
        SDLop_DBusMsg *r = SDLOP_DBus_NewReply(m, "o");
        SDLOP_DBus_PutString(r, IBUS_IC);
        send_msg(fd, r);
    } else if (strcmp(member, "ProcessKeyEvent") == 0) {
        Uint32 sym = 0, code = 0, state = 0;
        bool release = false;
        SDLOP_DBus_IterGetU32(&it, &sym);
        SDLOP_DBus_IterNext(&it);
        SDLOP_DBus_IterGetU32(&it, &code);
        SDLOP_DBus_IterNext(&it);
        SDLOP_DBus_IterGetU32(&it, &state);
        if (conn == 1) {
            SDLOP_DBus_IterNext(&it);
            SDLOP_DBus_IterGetBool(&it, &release);
            log_call("Key(%x,%u,%x,%d)", sym, code, state, (int)release);
        } else {
            log_call("Key(%x,%u,%x)", sym, code, state);
        }
        reply_bool(fd, m, sym == 'a' || sym == 'A'); /* the input method takes 'a' */
    } else if (strcmp(member, "SetCapability") == 0) {
        Uint64 caps = 0;
        SDLOP_DBus_IterGetU64(&it, &caps);
        log_call("SetCapability(%llx)", (unsigned long long)caps);
    } else if (strcmp(member, "SetCapabilities") == 0) {
        Uint32 caps = 0;
        SDLOP_DBus_IterGetU32(&it, &caps);
        log_call("SetCapabilities(%x)", caps);
    } else if (strcmp(member, "SetCursorRect") == 0 || strcmp(member, "SetCursorLocation") == 0) {
        Sint32 v[4] = { 0 };
        for (int i = 0; i < 4; i++) {
            SDLOP_DBus_IterGetI32(&it, &v[i]);
            SDLOP_DBus_IterNext(&it);
        }
        log_call("%s(%d,%d,%d,%d)", member, v[0], v[1], v[2], v[3]);
    } else {
        log_call("%s@%s", member, path);
    }
}

static void serve(int fd, int conn)
{
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
    const char *ok = "OK 0123456789abcdef0123456789abcdef\r\n";
    if (send(fd, ok, strlen(ok), MSG_NOSIGNAL) < 0) {
        return;
    }
    Uint8 *buf = malloc(65536);
    size_t len = 0;
    bool begun = false;
    for (;;) {
        const int which = atomic_exchange(&send_signal, 0);
        if (which) {
            emit(fd, which);
            atomic_fetch_add(&signals_sent, 1);
        }
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 5) <= 0) {
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

/* ---- helpers ----------------------------------------------------------- */

static bool wait_for(atomic_int *v, int want, int ms)
{
    for (int i = 0; i < ms && atomic_load(v) < want; i++) {
        SDL_Delay(1);
    }
    return atomic_load(v) >= want;
}

/* fire-and-forget calls are in flight: a synchronous round trip on the same
 * connection (in order) makes sure the bus has seen them */
static void sync_bus(void)
{
    SDLop_DBusMsg *m = SDLOP_DBus_NewMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                "org.freedesktop.DBus", "NameHasOwner", "s");
    SDLOP_DBus_PutString(m, "sync");
    SDLOP_DBus_FreeMessage(SDLOP_DBus_Call(m, 1000));
    SDLOP_DBus_FreeMessage(m);
}

/* the bus emits a signal; pump until it has been dispatched */
static void signal_and_pump(int which)
{
    const int before = atomic_load(&signals_sent);
    atomic_store(&send_signal, which);
    wait_for(&signals_sent, before + 1, 2000);
    sync_bus(); /* the signal is queued before this reply */
    SDL_PumpEvents();
}

/* the events since the last call, as text */
static void take_events(char *out, size_t n)
{
    out[0] = '\0';
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        char item[200] = "";
        switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            snprintf(item, sizeof(item), "down:%s ", SDL_GetKeyName(e.key.key));
            break;
        case SDL_EVENT_KEY_UP:
            snprintf(item, sizeof(item), "up:%s ", SDL_GetKeyName(e.key.key));
            break;
        case SDL_EVENT_TEXT_INPUT:
            snprintf(item, sizeof(item), "text:%s ", e.text.text);
            break;
        case SDL_EVENT_TEXT_EDITING:
            snprintf(item, sizeof(item), "edit:%s@%d+%d ", e.edit.text, (int)e.edit.start, (int)e.edit.length);
            break;
        default:
            break;
        }
        if (strlen(out) + strlen(item) < n) {
            strcat(out, item);
        }
    }
}

static void key(SDL_Scancode sc, Uint16 evdev, bool down)
{
    SDLOP_SendKeyboardKey(down, false, sc, evdev, 0);
}

/* ---- test -------------------------------------------------------------- */

int main(void)
{
    char dir[] = "/tmp/sdlop-ime-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    char sock_path[108];
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
    setenv("XDG_CONFIG_HOME", dir, 1); /* no ~/.config/ibus/bus here */
    unsetenv("IBUS_ADDRESS");
    unsetenv("SDL_IM_MODULE");
    unsetenv("GTK_IM_MODULE");
    unsetenv("QT_IM_MODULE");
    setenv("XMODIFIERS", "@im=fcitx", 1);
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");

    char got[4096], ev[4096];

    /* --- 1: Fcitx ------------------------------------------------------- */
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init: %s", SDL_GetError());
    SDL_Window *w = SDL_CreateWindow("ime", 320, 240, 0);
    CHECK(w != NULL, "window: %s", SDL_GetError());
    SDLOP_IME_DBus_Update(); /* focus, no text input */
    key(SDL_SCANCODE_A, 30, true);
    key(SDL_SCANCODE_A, 30, false);
    CHECK(atomic_load(&connections) == 0, "no bus before text input (%d)", atomic_load(&connections));
    take_events(ev, sizeof(ev));

    SDL_StartTextInput(w);
    const SDL_Rect area = { 10, 20, 100, 16 };
    SDL_SetTextInputArea(w, &area, 5);
    SDLOP_IME_DBus_Update();
    sync_bus();
    int wx = 0, wy = 0;
    SDL_GetWindowPosition(w, &wx, &wy);
    char want[512];
    take_calls(got, sizeof(got));
    snprintf(want, sizeof(want),
             "Create(org.freedesktop.portal.Fcitx) AddMatch SetCapability(12) FocusIn@" FCITX_IC " "
             "SetCursorRect(%d,%d,16,16) NameHasOwner(sync) ",
             wx + 15, wy + 20); /* screen space: window + area + cursor offset, height square */
    CHECK(strcmp(got, want) == 0, "fcitx setup: '%s'", got);
    CHECK(strstr(match_rule, "interface='org.fcitx.Fcitx.InputContext1'") && strstr(match_rule, "path='" FCITX_IC "'"),
          "match '%s'", match_rule);
    CHECK(strncmp(program, "program=", 8) == 0 && strlen(program) > 8, "program '%s'", program);
    CHECK(strcmp(SDL_GetError(), "") == 0, "no error left: '%s'", SDL_GetError());

    /* unchanged area: not sent again; a new one is */
    SDLOP_IME_DBus_UpdateArea(w);
    const SDL_Rect area2 = { 30, 40, 50, 20 };
    SDL_SetTextInputArea(w, &area2, 0);
    SDLOP_IME_DBus_UpdateArea(w);
    sync_bus();
    take_calls(got, sizeof(got));
    snprintf(want, sizeof(want), "SetCursorRect(%d,%d,20,20) NameHasOwner(sync) ", wx + 30, wy + 40);
    CHECK(strcmp(got, want) == 0, "area: '%s'", got);

    /* keys: 'a' is the input method's (key events, no text), 'b' types */
    take_events(ev, sizeof(ev));
    key(SDL_SCANCODE_A, 30, true);
    key(SDL_SCANCODE_A, 30, false);
    key(SDL_SCANCODE_B, 48, true);
    key(SDL_SCANCODE_B, 48, false);
    key(SDL_SCANCODE_LSHIFT, 42, true);
    key(SDL_SCANCODE_A, 30, true);
    key(SDL_SCANCODE_A, 30, false);
    key(SDL_SCANCODE_LSHIFT, 42, false);
    take_calls(got, sizeof(got));
    CHECK(strncmp(got, "Key(61,38,0,0) Key(61,38,0,1) Key(62,56,0,0) Key(62,56,0,1) Key(ffe1,50,1,0) Key(41,38,1,0) "
                       "Key(41,38,1,1) Key(ffe1,50,0,1) ",
                  strlen("Key(61,38,0,0) Key(61,38,0,1) Key(62,56,0,0) Key(62,56,0,1) Key(ffe1,50,1,0) "
                         "Key(41,38,1,0) Key(41,38,1,1) Key(ffe1,50,0,1) ")) == 0,
          "fcitx keys: '%s'", got);
    take_events(ev, sizeof(ev));
    CHECK(strcmp(ev, "down:A up:A down:B text:b up:B down:Left Shift down:A up:A up:Left Shift ") == 0,
          "fcitx key events: '%s'", ev);

    /* commits and preedit */
    signal_and_pump(1);
    signal_and_pump(5);
    signal_and_pump(2);
    signal_and_pump(3);
    signal_and_pump(4);
    take_events(ev, sizeof(ev));
    CHECK(strcmp(ev, "text:\xe4\xbd\xa0\xe5\xa5\xbd edit:nihao@2+3 edit:ni\xe4\xbd\xa0x@3+-1 edit:@0+0 ") == 0,
          "fcitx signals: '%s'", ev);
    take_calls(got, sizeof(got)); /* the syncs */

    SDL_ClearComposition(w);
    SDLOP_IME_DBus_Reset();
    SDL_StopTextInput(w);
    SDLOP_IME_DBus_Update();
    key(SDL_SCANCODE_A, 30, true);
    key(SDL_SCANCODE_A, 30, false);
    sync_bus();
    take_calls(got, sizeof(got));
    CHECK(strcmp(got, "Reset@" FCITX_IC " FocusOut@" FCITX_IC " NameHasOwner(sync) ") == 0, "fcitx stop: '%s'", got);
    take_events(ev, sizeof(ev));

    /* focus comes back with text input */
    SDL_StartTextInput(w);
    SDLOP_IME_DBus_Update();
    sync_bus();
    take_calls(got, sizeof(got));
    CHECK(strncmp(got, "SetCapability(12) FocusIn@" FCITX_IC " SetCursorRect(", 26) == 0, "fcitx refocus: '%s'", got);

    SDL_Quit();
    CHECK(wait_for(&conn_closed, 1, 2000), "connection 1 closed");
    take_calls(got, sizeof(got));
    CHECK(strstr(got, "FocusOut@" FCITX_IC " DestroyIC@" FCITX_IC " RemoveMatch") != NULL, "fcitx quit: '%s'", got);

    /* --- 2: IBus -------------------------------------------------------- */
    setenv("SDL_IM_MODULE", "ibus", 1); /* wins over XMODIFIERS */
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "candidates");
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init: %s", SDL_GetError());
    w = SDL_CreateWindow("ime", 320, 240, 0);
    SDL_StartTextInput(w);
    SDL_SetTextInputArea(w, &area, 2);
    SDLOP_IME_DBus_Update();
    sync_bus();
    SDL_GetWindowPosition(w, &wx, &wy);
    take_calls(got, sizeof(got));
    snprintf(want, sizeof(want),
             "Create(org.freedesktop.portal.IBus,SDL3_Application) AddMatch SetCapabilities(8) FocusIn@" IBUS_IC
             " SetCursorLocation(%d,%d,16,16) NameHasOwner(sync) ",
             wx + 12, wy + 20);
    CHECK(strcmp(got, want) == 0, "ibus setup: '%s'", got);
    CHECK(strstr(match_rule, "interface='org.freedesktop.IBus.InputContext'") != NULL, "match '%s'", match_rule);

    take_events(ev, sizeof(ev));
    key(SDL_SCANCODE_LCTRL, 29, true);
    key(SDL_SCANCODE_A, 30, true);
    key(SDL_SCANCODE_A, 30, false);
    key(SDL_SCANCODE_LCTRL, 29, false);
    key(SDL_SCANCODE_RETURN, 28, true);
    take_calls(got, sizeof(got));
    CHECK(strcmp(got, "Key(ffe3,29,4) Key(61,30,4) Key(61,30,40000004) Key(ffe3,29,40000000) Key(ff0d,28,0) ") == 0,
          "ibus keys: '%s'", got);
    take_events(ev, sizeof(ev));
    CHECK(strcmp(ev, "down:Left Ctrl down:A up:A up:Left Ctrl down:Return ") == 0, "ibus key events: '%s'", ev);
    key(SDL_SCANCODE_RETURN, 28, false);
    take_calls(got, sizeof(got));
    take_events(ev, sizeof(ev));

    signal_and_pump(11);
    signal_and_pump(12);
    signal_and_pump(14);
    signal_and_pump(15);
    signal_and_pump(13);
    take_events(ev, sizeof(ev));
    CHECK(strcmp(ev, "text:\xe6\x97\xa5\xe6\x9c\xac edit:nihon@1+2 edit:@0+0 edit:nihon@1+2 edit:ka@2+-1 ") == 0,
          "ibus signals: '%s'", ev);
    take_calls(got, sizeof(got)); /* the syncs */

    SDL_DestroyWindow(w); /* the focused window goes: FocusOut, preedit dropped */
    take_events(ev, sizeof(ev));
    sync_bus();
    take_calls(got, sizeof(got));
    CHECK(strcmp(got, "FocusOut@" IBUS_IC " NameHasOwner(sync) ") == 0, "ibus window gone: '%s'", got);
    SDL_Quit();
    CHECK(wait_for(&conn_closed, 2, 2000), "connection 2 closed");
    take_calls(got, sizeof(got));
    CHECK(strcmp(got, "Destroy@" IBUS_IC " RemoveMatch ") == 0, "ibus quit: '%s'", got);

    /* --- 3: nothing named, nothing running ------------------------------- */
    unsetenv("SDL_IM_MODULE");
    unsetenv("XMODIFIERS");
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init: %s", SDL_GetError());
    w = SDL_CreateWindow("ime", 320, 240, 0);
    SDL_StartTextInput(w);
    SDLOP_IME_DBus_Update();
    take_events(ev, sizeof(ev));
    key(SDL_SCANCODE_A, 30, true);
    key(SDL_SCANCODE_A, 30, false);
    take_events(ev, sizeof(ev));
    CHECK(strcmp(ev, "down:A text:a up:A ") == 0, "plain typing: '%s'", ev);
    sync_bus();
    take_calls(got, sizeof(got));
    CHECK(strcmp(got, "NameHasOwner(org.freedesktop.portal.Fcitx) NameHasOwner(org.freedesktop.portal.IBus) "
                      "NameHasOwner(sync) ") == 0,
          "probe only: '%s'", got);
    SDL_Quit();
    CHECK(wait_for(&conn_closed, 3, 2000), "connection 3 closed");

    /* --- SDL_IM_MODULE=none: not even a connection ----------------------- */
    setenv("SDL_IM_MODULE", "none", 1);
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init: %s", SDL_GetError());
    w = SDL_CreateWindow("ime", 320, 240, 0);
    SDL_StartTextInput(w);
    SDLOP_IME_DBus_Update();
    SDL_Quit();
    CHECK(atomic_load(&connections) == 3, "no connection with SDL_IM_MODULE=none (%d)", atomic_load(&connections));

    shutdown(listen_fd, SHUT_RDWR);
    close(listen_fd);
    unlink(sock_path);
    rmdir(dir);
    if (failures) {
        fprintf(stderr, "test_ime_dbus: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_ime_dbus: ok\n");
    return 0;
}
