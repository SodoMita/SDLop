/*
  SDLop test: XInput 2.2 multitouch through the X11 driver.

  Xvfb has no touch devices and XTest cannot synthesize touches, so the
  test puts a small X protocol proxy between SDLop and the real server: it
  forwards both directions byte for byte and, at a message boundary of the
  server->client stream, injects XI2 TouchBegin/Update/End events with the
  exact wire layout the server would send. Verified:

  - SDLop selected XI_TouchBegin|Update|End on its window (queried from
    the real server with XIGetSelectedEvents)
  - FINGER_DOWN / MOTION (dx, dy) / UP with SDL3 normalization, touchID =
    source device, fingerID = touch id, two fingers tracked independently
  - the first finger synthesizes the SDL_TOUCH_MOUSEID left-button mouse
  - updates for unknown touches / other windows are ignored
  - destroying the window cancels its active touches (FINGER_CANCELED)
  - with an XI 2.4 server: injected GesturePinchBegin/Update/End become
    SDL_EVENT_PINCH_BEGIN/UPDATE (scale passed through)/END

  Skips without DISPLAY, XInput 2.2, or when the server needs
  authorization the proxy cannot forward (xvfb-run -a with cookies works:
  the cookie is passed through unchanged only when unset).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <errno.h>
#include <math.h>
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
#include <xcb/xcb.h>
#include <xcb/xinput.h>

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

/* ------------------------------------------------------------------ */
/* proxy                                                               */
/* ------------------------------------------------------------------ */

static int listen_fd = -1;
static char listen_path[108];
static char real_display[64];

#define INJECT_MAX 4096
static pthread_mutex_t inject_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char inject_buf[INJECT_MAX];
static size_t inject_len = 0;
static atomic_int inject_pending = 0; /* bytes not yet written */
static atomic_bool proxy_stop = false;
static atomic_int connections = 0;
static atomic_int proxy_accepted = 0;
static atomic_int proxy_live = 0;

static int connect_real_server(void)
{
    /* ":N" or ":N.S" -> /tmp/.X11-unix/XN */
    const char *colon = strrchr(real_display, ':');
    if (!colon) {
        return -1;
    }
    int n = atoi(colon + 1);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "/tmp/.X11-unix/X%d", n);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool write_all(int fd, const unsigned char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

typedef struct
{
    int client, server;
    bool inject; /* the first connection is SDLop's main xcb connection */
} proxy_conn;

/* server->client: buffer until whole messages are present so injected
 * events land on message boundaries with a valid sequence number */
static void *proxy_thread(void *arg)
{
    proxy_conn *pc = (proxy_conn *)arg;
    unsigned char *sbuf = malloc(1 << 20);
    size_t slen = 0;
    bool setup_done = false;
    uint16_t last_seq = 0;
    unsigned char cbuf[65536];

    while (!atomic_load(&proxy_stop)) {
        struct pollfd pf[2] = { { pc->client, POLLIN, 0 }, { pc->server, POLLIN, 0 } };
        int r = poll(pf, 2, 5);
        if (r < 0 && errno != EINTR) {
            break;
        }
        if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(pc->client, cbuf, sizeof(cbuf));
            if (n <= 0 || !write_all(pc->server, cbuf, (size_t)n)) {
                break;
            }
        }
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(pc->server, sbuf + slen, (1 << 20) - slen);
            if (n <= 0) {
                break;
            }
            slen += (size_t)n;
        }
        /* forward complete messages */
        size_t off = 0;
        for (;;) {
            size_t avail = slen - off;
            size_t need;
            if (!setup_done) {
                if (avail < 8) {
                    break;
                }
                need = 8 + 4u * (size_t)(sbuf[off + 6] | (sbuf[off + 7] << 8));
            } else {
                if (avail < 32) {
                    break;
                }
                uint8_t type = sbuf[off] & 0x7f;
                need = 32;
                if (type == 1 || type == 35) { /* reply / GenericEvent */
                    uint32_t len = (uint32_t)sbuf[off + 4] | ((uint32_t)sbuf[off + 5] << 8) |
                                   ((uint32_t)sbuf[off + 6] << 16) | ((uint32_t)sbuf[off + 7] << 24);
                    need += 4u * (size_t)len;
                }
            }
            if (avail < need) {
                break;
            }
            if (setup_done) {
                last_seq = (uint16_t)(sbuf[off + 2] | (sbuf[off + 3] << 8));
            }
            setup_done = true;
            off += need;
        }
        if (off > 0) {
            if (!write_all(pc->client, sbuf, off)) {
                break;
            }
            memmove(sbuf, sbuf + off, slen - off);
            slen -= off;
        }
        /* inject at a boundary (nothing partial buffered) */
        if (pc->inject && setup_done && slen == 0 && atomic_load(&inject_pending) > 0) {
            pthread_mutex_lock(&inject_lock);
            for (size_t i = 0; i + 32 <= inject_len;) {
                inject_buf[i + 2] = (unsigned char)(last_seq & 0xff);
                inject_buf[i + 3] = (unsigned char)(last_seq >> 8);
                uint32_t len = 0; /* only GenericEvents have a length (core events: time) */
                if ((inject_buf[i] & 0x7f) == 35) {
                    len = (uint32_t)inject_buf[i + 4] | ((uint32_t)inject_buf[i + 5] << 8);
                }
                i += 32 + 4u * len;
            }
            bool ok = write_all(pc->client, inject_buf, inject_len);
            inject_len = 0;
            atomic_store(&inject_pending, 0);
            pthread_mutex_unlock(&inject_lock);
            if (!ok) {
                break;
            }
        }
    }
    close(pc->client);
    close(pc->server);
    free(sbuf);
    free(pc);
    atomic_fetch_sub(&proxy_live, 1);
    return NULL;
}

static void *accept_thread(void *arg)
{
    (void)arg;
    while (!atomic_load(&proxy_stop)) {
        struct pollfd pf = { listen_fd, POLLIN, 0 };
        if (poll(&pf, 1, 20) <= 0) {
            continue;
        }
        int c = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) {
            continue;
        }
        int s = connect_real_server();
        if (s < 0) {
            close(c);
            continue;
        }
        proxy_conn *pc = calloc(1, sizeof(*pc));
        pc->client = c;
        pc->server = s;
        pc->inject = atomic_fetch_add(&connections, 1) == 0;
        pthread_t t;
        /* Account for the thread before publishing the accept, so callers
         * waiting for "accepted and closed" cannot observe a false zero. */
        atomic_fetch_add(&proxy_live, 1);
        if (pthread_create(&t, NULL, proxy_thread, pc) != 0) {
            atomic_fetch_sub(&proxy_live, 1);
            close(c);
            close(s);
            free(pc);
            continue;
        }
        atomic_fetch_add(&proxy_accepted, 1);
        pthread_detach(t);
    }
    return NULL;
}

/* listen on the first free /tmp/.X11-unix/XN, N >= 150; returns N */
static int start_proxy(void)
{
    mkdir("/tmp/.X11-unix", 01777);
    for (int n = 150; n < 250; n++) {
        char lock[64];
        snprintf(lock, sizeof(lock), "/tmp/.X%d-lock", n);
        snprintf(listen_path, sizeof(listen_path), "/tmp/.X11-unix/X%d", n);
        if (access(lock, F_OK) == 0 || access(listen_path, F_OK) == 0) {
            continue;
        }
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", listen_path);
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(fd, 8) == 0) {
            listen_fd = fd;
            pthread_t t;
            pthread_create(&t, NULL, accept_thread, NULL);
            pthread_detach(t);
            return n;
        }
        close(fd);
    }
    return -1;
}

static void stop_proxy(void)
{
    atomic_store(&proxy_stop, true);
    if (listen_fd >= 0) {
        close(listen_fd);
        unlink(listen_path);
    }
}

/* ------------------------------------------------------------------ */
/* XI2 touch events, wire format (little endian)                       */
/* ------------------------------------------------------------------ */

static uint8_t xi_opcode;
static xcb_window_t root_win;

static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

/* xXIDeviceEvent: 80 bytes (no button / valuator masks) */
static void queue_touch(uint16_t evtype, uint32_t touch_id, uint16_t source, xcb_window_t win, double x, double y)
{
    unsigned char e[80];
    memset(e, 0, sizeof(e));
    e[0] = 35; /* GenericEvent */
    e[1] = xi_opcode;
    put32(e + 4, (80 - 32) / 4);
    put16(e + 8, evtype);
    put16(e + 10, 2);              /* deviceid: master pointer */
    put32(e + 12, 0);              /* time */
    put32(e + 16, touch_id);       /* detail */
    put32(e + 20, root_win);       /* root */
    put32(e + 24, win);            /* event */
    put32(e + 28, 0);              /* child */
    put32(e + 32, (uint32_t)(int32_t)lround((x + 10.0) * 65536.0)); /* root_x */
    put32(e + 36, (uint32_t)(int32_t)lround((y + 10.0) * 65536.0)); /* root_y */
    put32(e + 40, (uint32_t)(int32_t)lround(x * 65536.0));          /* event_x */
    put32(e + 44, (uint32_t)(int32_t)lround(y * 65536.0));          /* event_y */
    put16(e + 48, 0);              /* buttons_len */
    put16(e + 50, 0);              /* valuators_len */
    put16(e + 52, source);         /* sourceid */
    put32(e + 56, evtype == XCB_INPUT_TOUCH_BEGIN ? XCB_INPUT_TOUCH_EVENT_FLAGS_TOUCH_EMULATING_POINTER : 0);
    pthread_mutex_lock(&inject_lock);
    if (inject_len + sizeof(e) <= INJECT_MAX) {
        memcpy(inject_buf + inject_len, e, sizeof(e));
        inject_len += sizeof(e);
        atomic_store(&inject_pending, (int)inject_len);
    }
    pthread_mutex_unlock(&inject_lock);
}

/* xXIGesturePinchEvent: 100 bytes */
static void queue_pinch(uint16_t evtype, xcb_window_t win, double scale)
{
    unsigned char e[100];
    memset(e, 0, sizeof(e));
    e[0] = 35; /* GenericEvent */
    e[1] = xi_opcode;
    put32(e + 4, (100 - 32) / 4);
    put16(e + 8, evtype);
    put16(e + 10, 2);  /* deviceid: master pointer */
    put32(e + 16, 2);  /* detail: fingers */
    put32(e + 20, root_win);
    put32(e + 24, win);
    put32(e + 40, 50u << 16); /* event_x */
    put32(e + 44, 60u << 16); /* event_y */
    put32(e + 64, (uint32_t)(int32_t)lround(scale * 65536.0));
    put16(e + 72, 12); /* sourceid */
    pthread_mutex_lock(&inject_lock);
    if (inject_len + sizeof(e) <= INJECT_MAX) {
        memcpy(inject_buf + inject_len, e, sizeof(e));
        inject_len += sizeof(e);
        atomic_store(&inject_pending, (int)inject_len);
    }
    pthread_mutex_unlock(&inject_lock);
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static xcb_connection_t *obs;

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(obs, xcb_intern_atom(obs, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static xcb_window_t find_window(xcb_window_t parent, const char *title, xcb_atom_t name_atom)
{
    xcb_query_tree_reply_t *t = xcb_query_tree_reply(obs, xcb_query_tree(obs, parent), NULL);
    if (!t) {
        return 0;
    }
    xcb_window_t found = 0;
    xcb_window_t *kids = xcb_query_tree_children(t);
    int n = xcb_query_tree_children_length(t);
    for (int i = 0; i < n && !found; i++) {
        xcb_get_property_reply_t *p = xcb_get_property_reply(
            obs, xcb_get_property(obs, 0, kids[i], name_atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 256), NULL);
        if (p && xcb_get_property_value_length(p) == (int)strlen(title) &&
            memcmp(xcb_get_property_value(p), title, strlen(title)) == 0) {
            found = kids[i];
        }
        free(p);
        if (!found) {
            found = find_window(kids[i], title, name_atom);
        }
    }
    free(t);
    return found;
}

typedef struct
{
    SDL_Event ev[64];
    int n;
} event_log;

static void pump(event_log *log, int ms)
{
    Uint64 end = SDL_GetTicks() + (Uint64)ms;
    do {
        SDL_PumpEvents();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            bool keep = (e.type >= SDL_EVENT_FINGER_DOWN && e.type <= SDL_EVENT_FINGER_CANCELED) ||
                        (e.type >= SDL_EVENT_PINCH_BEGIN && e.type <= SDL_EVENT_PINCH_END) ||
                        ((e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which == SDL_TOUCH_MOUSEID) ||
                         ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                          e.button.which == SDL_TOUCH_MOUSEID));
            if (log && keep && log->n < 64) {
                log->ev[log->n++] = e;
            }
        }
        SDL_Delay(2);
    } while (SDL_GetTicks() < end);
}

/* inject what is queued and collect the results */
static void flush_and_collect(event_log *log)
{
    memset(log, 0, sizeof(*log));
    Uint64 end = SDL_GetTicks() + 1000;
    /* The proxy clears inject_pending once it has written to our socket, not
     * once SDLop has consumed those events. Collect while waiting so events
     * delivered during this window are not drained and discarded. */
    while (atomic_load(&inject_pending) > 0 && SDL_GetTicks() < end) {
        pump(log, 2);
    }
    const int stuck = atomic_load(&inject_pending);
    pump(log, 250); /* generous: sanitizer builds are slow */
    if (stuck > 0) {
        /* tells "never injected" from "injected, events late" in a flaky run */
        fprintf(stderr, "note: %d injected byte(s) still unsent after 1s; collected %d event(s)\n", stuck, log->n);
    }
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.0005f;
}

static bool is_finger(const SDL_Event *e, Uint32 type, Uint64 finger, float x, float y)
{
    return e->type == type && e->tfinger.fingerID == finger && near(e->tfinger.x, x) && near(e->tfinger.y, y);
}

int main(void)
{
    const char *disp = getenv("DISPLAY");
    if (!disp || !*disp) {
        printf("test_x11_touch: SKIP (DISPLAY not set)\n");
        return 0;
    }
    snprintf(real_display, sizeof(real_display), "%s", disp);

    obs = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(obs)) {
        printf("test_x11_touch: SKIP (cannot connect)\n");
        return 0;
    }
    root_win = xcb_setup_roots_iterator(xcb_get_setup(obs)).data->root;
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(obs, &xcb_input_id);
    xcb_input_xi_query_version_reply_t *v =
        ext && ext->present ? xcb_input_xi_query_version_reply(obs, xcb_input_xi_query_version(obs, 2, 2), NULL) : NULL;
    bool xi22 = v && (v->major_version > 2 || (v->major_version == 2 && v->minor_version >= 2));
    free(v);
    if (!xi22) {
        printf("test_x11_touch: SKIP (no XInput 2.2)\n");
        return getenv("SDLOP_TEST_STRICT") ? 1 : 0;
    }
    xi_opcode = ext->major_opcode;

    int n = start_proxy();
    if (n < 0) {
        printf("test_x11_touch: SKIP (no free proxy display)\n");
        return getenv("SDLOP_TEST_STRICT") ? 1 : 0;
    }
    char proxy_disp[32];
    snprintf(proxy_disp, sizeof(proxy_disp), ":%d", n);
    /* the proxy display has no Xauthority entry; works on servers without
     * access control (CI Xvfb) */
    xcb_connection_t *probe = xcb_connect(proxy_disp, NULL);
    bool probe_ok = !xcb_connection_has_error(probe);
    xcb_disconnect(probe);
    if (!probe_ok) {
        stop_proxy();
        printf("test_x11_touch: SKIP (server refuses the unauthenticated proxy connection)\n");
        return getenv("SDLOP_TEST_STRICT") ? 1 : 0;
    }
    /* The proxy's accept thread may not have seen the probe connection when
     * xcb_disconnect() returns. If we reset the counter too early, the probe
     * can still be accepted as connection 0 and steal the injection flag from
     * SDLop's real connection. Wait until it has been accepted and closed. */
    for (int i = 0; i < 500 &&
         (atomic_load(&proxy_accepted) < 1 || atomic_load(&proxy_live) != 0); i++) {
        usleep(1000);
    }
    atomic_store(&connections, 0);
    printf("test_x11_touch: proxy %s -> %s, XI opcode %u\n", proxy_disp, disp, xi_opcode);

    setenv("DISPLAY", proxy_disp, 1);
    setenv("SDL_VIDEODRIVER", "x11", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDLOP_X11_NO_SHM", "1", 1); /* nothing to blit; keep the proxy simple */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        stop_proxy();
        return 1;
    }
    const char *title = "SDLop x11 touch test";
    SDL_Window *w = SDL_CreateWindow(title, 320, 240, 0);
    CHECK(w != NULL, "window");
    SDL_ShowWindow(w);
    pump(NULL, 200);

    xcb_window_t xid = 0;
    for (int i = 0; i < 100 && !xid; i++) {
        xid = find_window(root_win, title, atom("_NET_WM_NAME"));
        if (!xid) {
            pump(NULL, 20);
        }
    }
    CHECK(xid != 0, "found the X window");

    /* the real server confirms the selection: only one client may select
     * touch events on a window, so ours must fail with BadAccess
     * (XIGetSelectedEvents only reports the caller's own masks) */
    free(xcb_input_xi_query_version_reply(obs, xcb_input_xi_query_version(obs, 2, 2), NULL));
    struct
    {
        xcb_input_event_mask_t head;
        uint32_t mask;
    } em = { { XCB_INPUT_DEVICE_ALL_MASTER, 1 },
             XCB_INPUT_XI_EVENT_MASK_TOUCH_BEGIN | XCB_INPUT_XI_EVENT_MASK_TOUCH_UPDATE |
                 XCB_INPUT_XI_EVENT_MASK_TOUCH_END };
    xcb_generic_error_t *err = xcb_request_check(obs, xcb_input_xi_select_events_checked(obs, xid, 1, &em.head));
    bool selected = err && err->error_code == XCB_ACCESS;
    free(err);
    CHECK(selected, "XI2 touch events selected on the window");

    const float W = 320.0f, H = 240.0f;
    event_log log;

    /* finger 5 down at (50,60) */
    queue_touch(XCB_INPUT_TOUCH_BEGIN, 5, 11, xid, 50, 60);
    flush_and_collect(&log);
    CHECK(log.n == 3, "begin: 3 events (mouse motion, button, finger), got %d", log.n);
    if (log.n == 3) {
        CHECK(log.ev[0].type == SDL_EVENT_MOUSE_MOTION && near(log.ev[0].motion.x, 50) &&
                  near(log.ev[0].motion.y, 60),
              "synthetic mouse motion to (50,60)");
        CHECK(log.ev[1].type == SDL_EVENT_MOUSE_BUTTON_DOWN && log.ev[1].button.button == SDL_BUTTON_LEFT,
              "synthetic left button down");
        const SDL_Event *f = &log.ev[2];
        CHECK(is_finger(f, SDL_EVENT_FINGER_DOWN, 5, 50 / W, 60 / H), "FINGER_DOWN 5 at (50,60)");
        CHECK(f->tfinger.touchID == 11 && f->tfinger.windowID == SDL_GetWindowID(w) &&
                  near(f->tfinger.pressure, 1.0f),
              "touchID = source device, windowID, pressure 1");
    }

    /* finger 6 down, then both move */
    queue_touch(XCB_INPUT_TOUCH_BEGIN, 6, 11, xid, 100, 120);
    queue_touch(XCB_INPUT_TOUCH_UPDATE, 5, 11, xid, 70, 80);
    queue_touch(XCB_INPUT_TOUCH_UPDATE, 6, 11, xid, 140, 160);
    flush_and_collect(&log);
    CHECK(log.n == 4, "second finger + motion: 4 events, got %d", log.n);
    if (log.n == 4) {
        CHECK(is_finger(&log.ev[0], SDL_EVENT_FINGER_DOWN, 6, 100 / W, 120 / H), "FINGER_DOWN 6, no mouse");
        CHECK(log.ev[1].type == SDL_EVENT_MOUSE_MOTION && near(log.ev[1].motion.x, 70) &&
                  near(log.ev[1].motion.y, 80) && log.ev[1].motion.which == SDL_TOUCH_MOUSEID,
              "finger 5 drives the mouse");
        CHECK(is_finger(&log.ev[2], SDL_EVENT_FINGER_MOTION, 5, 70 / W, 80 / H) &&
                  near(log.ev[2].tfinger.dx, 20 / W) && near(log.ev[2].tfinger.dy, 20 / H),
              "FINGER_MOTION 5 with dx/dy");
        CHECK(is_finger(&log.ev[3], SDL_EVENT_FINGER_MOTION, 6, 140 / W, 160 / H) &&
                  near(log.ev[3].tfinger.dx, 40 / W) && near(log.ev[3].tfinger.dy, 40 / H),
              "FINGER_MOTION 6 tracked independently");
    }

    /* updates for an unknown touch / foreign window: ignored; out of
     * window coordinates clamp to 0..1 */
    queue_touch(XCB_INPUT_TOUCH_UPDATE, 99, 11, xid, 10, 10);
    queue_touch(XCB_INPUT_TOUCH_BEGIN, 42, 11, root_win, 10, 10);
    queue_touch(XCB_INPUT_TOUCH_UPDATE, 6, 11, xid, 400, -5);
    flush_and_collect(&log);
    CHECK(log.n == 1 && is_finger(&log.ev[0], SDL_EVENT_FINGER_MOTION, 6, 1.0f, 0.0f),
          "unknown touches ignored, coordinates clamped (%d events)", log.n);

    /* finger 5 up: mouse button released at the last position */
    queue_touch(XCB_INPUT_TOUCH_END, 5, 11, xid, 70, 80);
    flush_and_collect(&log);
    CHECK(log.n == 2, "end: 2 events, got %d", log.n);
    if (log.n == 2) {
        CHECK(log.ev[0].type == SDL_EVENT_MOUSE_BUTTON_UP && log.ev[0].button.which == SDL_TOUCH_MOUSEID,
              "synthetic left button up");
        CHECK(is_finger(&log.ev[1], SDL_EVENT_FINGER_UP, 5, 70 / W, 80 / H) && near(log.ev[1].tfinger.pressure, 0),
              "FINGER_UP 5, pressure 0");
    }
    CHECK(!(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK), "mouse button state released");

    /* XI 2.4 touchpad pinch (asked on a fresh connection: a connection
     * announces its XI version once) */
    xcb_connection_t *c24 = xcb_connect(NULL, NULL);
    xcb_input_xi_query_version_reply_t *v24 =
        xcb_connection_has_error(c24)
            ? NULL
            : xcb_input_xi_query_version_reply(c24, xcb_input_xi_query_version(c24, 2, 4), NULL);
    const bool xi24 = v24 && (v24->major_version > 2 || v24->minor_version >= 4);
    free(v24);
    xcb_disconnect(c24);
    if (xi24) {
        queue_pinch(27, xid, 1.0);
        queue_pinch(28, xid, 1.5);
        queue_pinch(28, xid, 0.75);
        queue_pinch(29, xid, 0.75);
        flush_and_collect(&log);
        CHECK(log.n == 4, "pinch: 4 events, got %d", log.n);
        if (log.n == 4) {
            const SDL_WindowID wid = SDL_GetWindowID(w);
            CHECK(log.ev[0].type == SDL_EVENT_PINCH_BEGIN && log.ev[0].pinch.windowID == wid, "PINCH_BEGIN");
            CHECK(log.ev[1].type == SDL_EVENT_PINCH_UPDATE && near(log.ev[1].pinch.scale, 1.5f), "PINCH_UPDATE 1.5");
            CHECK(log.ev[2].type == SDL_EVENT_PINCH_UPDATE && near(log.ev[2].pinch.scale, 0.75f),
                  "PINCH_UPDATE 0.75");
            CHECK(log.ev[3].type == SDL_EVENT_PINCH_END && log.ev[3].pinch.windowID == wid, "PINCH_END");
        }
    } else {
        printf("test_x11_touch: server has no XI 2.4, pinch part skipped\n");
    }

    /* finger 6 is still down: destroying the window cancels it */
    SDL_DestroyWindow(w);
    memset(&log, 0, sizeof(log));
    pump(&log, 50);
    CHECK(log.n == 1 && log.ev[0].type == SDL_EVENT_FINGER_CANCELED && log.ev[0].tfinger.fingerID == 6,
          "window destroyed: FINGER_CANCELED 6 (%d events)", log.n);

    SDL_Quit();
    stop_proxy();
    xcb_disconnect(obs);

    if (failures) {
        printf("test_x11_touch: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_x11_touch: PASS\n");
    return 0;
}
