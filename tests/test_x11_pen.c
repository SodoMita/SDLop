/*
  SDLop test: XInput 2 pens (graphics tablets) through the X11 driver.

  Xvfb has no tablets, so - as in test_x11_touch - a small X protocol proxy
  sits between SDLop and the server. It follows SDLop's request stream and
  appends a fake stylus ("Abs Pressure" + "Abs Tilt X/Y" valuators, 3
  buttons) and a fake eraser to XIQueryDevice replies while they are
  "plugged in", and injects XI2 hierarchy, device and core events with the
  server's exact wire layout. Verified:

  - hotplug: XI_HierarchyChanged (slave added) makes SDLop re-enumerate and
    add both pens in proximity; removing them sends proximity out and
    SDL_GetPenDeviceType turns INVALID
  - XI_Motion: PEN_MOTION at the subpixel position, pressure normalized
    0..1 over the valuator range, tilt -1..1 keeping 0 at 0; the pen moves
    the SDL_PEN_MOUSEID mouse
  - the master's core copy of pen input (same server time) is dropped, an
    ordinary core event still is a mouse event
  - button 1 = tip (PEN_DOWN/UP, left button of the pen mouse), button 2 =
    pen button 1; the eraser device reports eraser = true
  Mode "off" sets SDLOP_X11_NO_PEN=1: no pens, core events all go through.

  Skips without DISPLAY, XInput 2.2, or when the server needs authorization
  the proxy cannot forward.

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

/* the fake tablet: XIQueryDevice replies on SDLop's connection get the
 * stylus and the eraser appended while fake_present is set */
static atomic_bool fake_present = false;
static uint8_t xi_opcode;
static unsigned char fake_infos[1024];
static size_t fake_infos_len = 0;
static int fake_infos_count = 0;
#define XI_QUERY_DEVICE 48
/* diagnostics */
static atomic_int stat_queries = 0, stat_query_replies = 0, stat_rewrites = 0, stat_injections = 0;

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
        /* the peer may already be gone during teardown; a bare write() then
         * raises SIGPIPE and kills the whole test with no output at all */
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
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
    /* client->server request parsing: sequence numbers of XIQueryDevice */
    bool csetup_done;
    unsigned char chdr[12];
    size_t chdr_len;
    size_t cskip;
    uint16_t cseq;
    uint16_t query_seq[16];
    int nquery;
} proxy_conn;

/* follow the request stream (setup, then requests with BIG-REQUESTS
 * lengths) to learn the sequence number of every XIQueryDevice */
static void track_requests(proxy_conn *pc, const unsigned char *p, size_t n)
{
    while (n > 0) {
        if (pc->cskip > 0) {
            size_t k = pc->cskip < n ? pc->cskip : n;
            pc->cskip -= k;
            p += k;
            n -= k;
            continue;
        }
        pc->chdr[pc->chdr_len++] = *p++;
        n--;
        size_t total = 0;
        if (!pc->csetup_done) {
            if (pc->chdr_len < 12) {
                continue;
            }
            const size_t an = (size_t)(pc->chdr[6] | (pc->chdr[7] << 8));
            const size_t ad = (size_t)(pc->chdr[8] | (pc->chdr[9] << 8));
            total = 12 + ((an + 3) & ~(size_t)3) + ((ad + 3) & ~(size_t)3);
            pc->csetup_done = true;
        } else {
            if (pc->chdr_len < 4) {
                continue;
            }
            size_t len = (size_t)(pc->chdr[2] | (pc->chdr[3] << 8));
            if (len == 0) { /* BIG-REQUESTS */
                if (pc->chdr_len < 8) {
                    continue;
                }
                len = (size_t)pc->chdr[4] | ((size_t)pc->chdr[5] << 8) | ((size_t)pc->chdr[6] << 16) |
                      ((size_t)pc->chdr[7] << 24);
            }
            total = len * 4;
            pc->cseq++;
            if (pc->chdr[0] == xi_opcode && pc->chdr[1] == XI_QUERY_DEVICE && pc->nquery < 16) {
                pc->query_seq[pc->nquery++] = pc->cseq;
                atomic_fetch_add(&stat_queries, 1);
            }
        }
        pc->cskip = total > pc->chdr_len ? total - pc->chdr_len : 0;
        pc->chdr_len = 0;
    }
}

/* XIQueryDevice reply with this sequence number? (consumes the entry) */
static bool is_query_reply(proxy_conn *pc, uint16_t seq)
{
    for (int i = 0; i < pc->nquery; i++) {
        if (pc->query_seq[i] == seq) {
            pc->query_seq[i] = pc->query_seq[--pc->nquery];
            atomic_fetch_add(&stat_query_replies, 1);
            return true;
        }
    }
    return false;
}

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
            if (pc->inject) {
                track_requests(pc, cbuf, (size_t)n);
            }
        }
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(pc->server, sbuf + slen, (1 << 20) - slen);
            if (n <= 0) {
                break;
            }
            slen += (size_t)n;
        }
        /* forward complete messages (an XIQueryDevice reply is rewritten) */
        size_t off = 0, sent = 0;
        bool fail = false;
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
                if (pc->inject && sbuf[off] == 1 && is_query_reply(pc, last_seq) && atomic_load(&fake_present)) {
                    /* flush what precedes, then the grown reply */
                    atomic_fetch_add(&stat_rewrites, 1);
                    unsigned char *msg = malloc(need + fake_infos_len);
                    memcpy(msg, sbuf + off, need);
                    memcpy(msg + need, fake_infos, fake_infos_len);
                    uint32_t len = (uint32_t)msg[4] | ((uint32_t)msg[5] << 8) | ((uint32_t)msg[6] << 16) |
                                   ((uint32_t)msg[7] << 24);
                    len += (uint32_t)(fake_infos_len / 4);
                    msg[4] = (unsigned char)len;
                    msg[5] = (unsigned char)(len >> 8);
                    msg[6] = (unsigned char)(len >> 16);
                    msg[7] = (unsigned char)(len >> 24);
                    uint16_t num = (uint16_t)(msg[8] | (msg[9] << 8));
                    num = (uint16_t)(num + fake_infos_count);
                    msg[8] = (unsigned char)num;
                    msg[9] = (unsigned char)(num >> 8);
                    fail = !write_all(pc->client, sbuf + sent, off - sent) ||
                           !write_all(pc->client, msg, need + fake_infos_len);
                    free(msg);
                    sent = off + need;
                    if (fail) {
                        break;
                    }
                }
            }
            setup_done = true;
            off += need;
        }
        if (fail) {
            break;
        }
        if (off > 0) {
            if (off > sent && !write_all(pc->client, sbuf + sent, off - sent)) {
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
                /* only GenericEvents have a length (in core events bytes
                 * 4..7 are the time): a misparse leaves later events with a
                 * stale sequence, which xcb widens past its pending
                 * requests and then fails their replies */
                uint32_t len = 0;
                if ((inject_buf[i] & 0x7f) == 35) {
                    len = (uint32_t)inject_buf[i + 4] | ((uint32_t)inject_buf[i + 5] << 8) |
                          ((uint32_t)inject_buf[i + 6] << 16) | ((uint32_t)inject_buf[i + 7] << 24);
                }
                i += 32 + 4u * len;
            }
            bool ok = write_all(pc->client, inject_buf, inject_len);
            atomic_fetch_add(&stat_injections, 1);
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
        pthread_create(&t, NULL, proxy_thread, pc);
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
/* wire format (little endian)                                         */
/* ------------------------------------------------------------------ */

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

static void queue(const unsigned char *e, size_t n)
{
    pthread_mutex_lock(&inject_lock);
    if (inject_len + n <= INJECT_MAX) {
        memcpy(inject_buf + inject_len, e, n);
        inject_len += n;
        atomic_store(&inject_pending, (int)inject_len);
    }
    pthread_mutex_unlock(&inject_lock);
}

#define STYLUS 77
#define ERASER 78

/* xXIDeviceInfo + name + classes: buttons, then valuators x y [p tx ty] */
static void add_fake_device(uint16_t id, const char *name, xcb_atom_t pressure, xcb_atom_t tilt_x,
                            xcb_atom_t tilt_y)
{
    unsigned char *p = fake_infos + fake_infos_len;
    const size_t nlen = strlen(name), npad = (nlen + 3) & ~(size_t)3;
    const int nval = 5;
    memset(p, 0, 12 + npad);
    put16(p, id);
    put16(p + 2, XCB_INPUT_DEVICE_TYPE_SLAVE_POINTER);
    put16(p + 4, 2); /* attachment: virtual core pointer */
    put16(p + 6, (uint16_t)(1 + nval));
    put16(p + 8, (uint16_t)nlen);
    p[10] = 1; /* enabled */
    memcpy(p + 12, name, nlen);
    p += 12 + npad;
    /* button class: 3 buttons, 1 state word, 3 labels */
    memset(p, 0, 24);
    put16(p, XCB_INPUT_DEVICE_CLASS_TYPE_BUTTON);
    put16(p + 2, 6);
    put16(p + 4, id);
    put16(p + 6, 3);
    p += 24;
    static const struct
    {
        int32_t min, max;
    } range[5] = { { 0, 32767 }, { 0, 32767 }, { 0, 2048 }, { -64, 63 }, { -64, 63 } };
    const xcb_atom_t labels[5] = { XCB_ATOM_NONE, XCB_ATOM_NONE, pressure, tilt_x, tilt_y };
    for (int v = 0; v < nval; v++) {
        memset(p, 0, 44);
        put16(p, XCB_INPUT_DEVICE_CLASS_TYPE_VALUATOR);
        put16(p + 2, 11);
        put16(p + 4, id);
        put16(p + 6, (uint16_t)v);
        put32(p + 8, labels[v]);
        put32(p + 12, (uint32_t)range[v].min); /* min.integral, frac 0 */
        put32(p + 20, (uint32_t)range[v].max);
        put32(p + 36, 1000); /* resolution */
        p[40] = 1;           /* absolute */
        p += 44;
    }
    fake_infos_len = (size_t)(p - fake_infos);
    fake_infos_count++;
}

static void queue_hierarchy(uint32_t flags)
{
    unsigned char e[32 + 24];
    memset(e, 0, sizeof(e));
    e[0] = 35;
    e[1] = xi_opcode;
    put32(e + 4, 24 / 4);
    put16(e + 8, XCB_INPUT_HIERARCHY);
    put16(e + 10, 0);
    put32(e + 16, flags);
    put16(e + 20, 2);
    const uint16_t ids[2] = { STYLUS, ERASER };
    for (int i = 0; i < 2; i++) {
        unsigned char *in = e + 32 + 12 * i;
        put16(in, ids[i]);
        put16(in + 2, 2);
        in[4] = XCB_INPUT_DEVICE_TYPE_SLAVE_POINTER;
        in[5] = 1;
        put32(in + 8, flags);
    }
    queue(e, sizeof(e));
}

static uint32_t fp1616(double v)
{
    return (uint32_t)(int32_t)lround(v * 65536.0);
}

/* xXIDeviceEvent from a slave device: 1 button-mask word, 1 valuator-mask
 * word, then FP3232 values for the valuators in vmask */
static void queue_device(uint16_t evtype, uint16_t dev, uint32_t time, uint32_t detail, xcb_window_t win, double x,
                         double y, uint32_t vmask, const double *vals)
{
    unsigned char e[80 + 8 + 8 * 32];
    memset(e, 0, sizeof(e));
    int nv = 0;
    for (int b = 0; b < 32; b++) {
        nv += (vmask >> b) & 1;
    }
    const size_t total = 80 + 8 + 8 * (size_t)nv;
    e[0] = 35;
    e[1] = xi_opcode;
    put32(e + 4, (uint32_t)((total - 32) / 4));
    put16(e + 8, evtype);
    put16(e + 10, dev); /* deviceid */
    put32(e + 12, time);
    put32(e + 16, detail);
    put32(e + 20, root_win);
    put32(e + 24, win);
    put32(e + 32, fp1616(x + 10.0));
    put32(e + 36, fp1616(y + 10.0));
    put32(e + 40, fp1616(x));
    put32(e + 44, fp1616(y));
    put16(e + 48, 1); /* buttons_len */
    put16(e + 50, 1); /* valuators_len */
    put16(e + 52, dev); /* sourceid */
    put32(e + 80, 0);    /* button mask */
    put32(e + 84, vmask);
    unsigned char *v = e + 88;
    for (int b = 0, i = 0; b < 32; b++) {
        if ((vmask >> b) & 1) {
            const double d = vals[i++];
            const double fl = floor(d);
            put32(v, (uint32_t)(int32_t)fl);
            put32(v + 4, (uint32_t)((d - fl) * 4294967296.0));
            v += 8;
        }
    }
    queue(e, total);
}

/* core MotionNotify / ButtonPress / ButtonRelease */
static void queue_core(uint8_t type, uint8_t detail, uint32_t time, xcb_window_t win, int16_t x, int16_t y)
{
    unsigned char e[32];
    memset(e, 0, sizeof(e));
    e[0] = type;
    e[1] = detail;
    put32(e + 4, time);
    put32(e + 8, root_win);
    put32(e + 12, win);
    put16(e + 20, (uint16_t)(x + 10));
    put16(e + 22, (uint16_t)(y + 10));
    put16(e + 24, (uint16_t)x);
    put16(e + 26, (uint16_t)y);
    e[30] = 1;
    queue(e, sizeof(e));
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
    SDL_Event ev[128];
    int n;
} event_log;

static bool interesting(const SDL_Event *e)
{
    return (e->type >= SDL_EVENT_PEN_PROXIMITY_IN && e->type <= SDL_EVENT_PEN_AXIS) ||
           e->type == SDL_EVENT_MOUSE_MOTION || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
           e->type == SDL_EVENT_MOUSE_BUTTON_UP;
}

static void pump(event_log *log, int ms)
{
    Uint64 end = SDL_GetTicks() + (Uint64)ms;
    do {
        SDL_PumpEvents();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (log && interesting(&e) && log->n < 128) {
                log->ev[log->n++] = e;
            }
        }
        SDL_Delay(2);
    } while (SDL_GetTicks() < end);
}

static void flush_and_collect(event_log *log)
{
    memset(log, 0, sizeof(*log));
    Uint64 end = SDL_GetTicks() + 1000;
    while (atomic_load(&inject_pending) > 0 && SDL_GetTicks() < end) {
        pump(log, 2);
    }
    const int stuck = atomic_load(&inject_pending);
    pump(log, 300); /* generous: sanitizer builds are slow */
    if (stuck > 0) {
        /* tells "never injected" from "injected, events late" in a flaky run */
        fprintf(stderr, "note: %d injected byte(s) still unsent after 1s; collected %d event(s)\n", stuck, log->n);
    }
}

static int find(const event_log *log, Uint32 type, int from)
{
    for (int i = from; i < log->n; i++) {
        if (log->ev[i].type == type) {
            return i;
        }
    }
    return -1;
}

static int count_pen(const event_log *log)
{
    int n = 0;
    for (int i = 0; i < log->n; i++) {
        n += log->ev[i].type >= SDL_EVENT_PEN_PROXIMITY_IN && log->ev[i].type <= SDL_EVENT_PEN_AXIS;
    }
    return n;
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.001f;
}

static bool has_axis(const event_log *log, SDL_PenID pen, SDL_PenAxis axis, float want)
{
    for (int i = 0; i < log->n; i++) {
        const SDL_Event *e = &log->ev[i];
        if (e->type == SDL_EVENT_PEN_AXIS && e->paxis.which == pen && e->paxis.axis == axis) {
            return near(e->paxis.value, want);
        }
    }
    return false;
}

/* a mouse event from a real (non-pen) mouse at x,y */
static bool has_core_mouse(const event_log *log, Uint32 type, float x)
{
    for (int i = 0; i < log->n; i++) {
        const SDL_Event *e = &log->ev[i];
        if (e->type == type && type == SDL_EVENT_MOUSE_MOTION && e->motion.which != SDL_PEN_MOUSEID &&
            near(e->motion.x, x)) {
            return true;
        }
        if (e->type == type && type != SDL_EVENT_MOUSE_MOTION && e->button.which != SDL_PEN_MOUSEID) {
            return true;
        }
    }
    return false;
}

int main(int argc, char *argv[])
{
    const bool on = !(argc > 1 && strcmp(argv[1], "off") == 0);
    const char *disp = getenv("DISPLAY");
    if (!disp || !*disp) {
        printf("test_x11_pen: SKIP (DISPLAY not set)\n");
        return 0;
    }
    snprintf(real_display, sizeof(real_display), "%s", disp);

    obs = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(obs)) {
        printf("test_x11_pen: SKIP (cannot connect)\n");
        return 0;
    }
    root_win = xcb_setup_roots_iterator(xcb_get_setup(obs)).data->root;
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(obs, &xcb_input_id);
    xcb_input_xi_query_version_reply_t *v =
        ext && ext->present ? xcb_input_xi_query_version_reply(obs, xcb_input_xi_query_version(obs, 2, 2), NULL) : NULL;
    bool xi22 = v && (v->major_version > 2 || (v->major_version == 2 && v->minor_version >= 2));
    free(v);
    if (!xi22) {
        printf("test_x11_pen: SKIP (no XInput 2.2)\n");
        return getenv("SDLOP_TEST_STRICT") ? 1 : 0;
    }
    xi_opcode = ext->major_opcode;

    /* the valuator labels a tablet driver would have created */
    add_fake_device(STYLUS, "SDLop Test Pen stylus", atom("Abs Pressure"), atom("Abs Tilt X"), atom("Abs Tilt Y"));
    add_fake_device(ERASER, "SDLop Test Pen eraser", atom("Abs Pressure"), atom("Abs Tilt X"), atom("Abs Tilt Y"));

    int n = start_proxy();
    if (n < 0) {
        printf("test_x11_pen: SKIP (no free proxy display)\n");
        return getenv("SDLOP_TEST_STRICT") ? 1 : 0;
    }
    char proxy_disp[32];
    snprintf(proxy_disp, sizeof(proxy_disp), ":%d", n);
    xcb_connection_t *probe = xcb_connect(proxy_disp, NULL);
    bool probe_ok = !xcb_connection_has_error(probe);
    xcb_disconnect(probe);
    atomic_store(&connections, 0);
    if (!probe_ok) {
        stop_proxy();
        printf("test_x11_pen: SKIP (server refuses the unauthenticated proxy connection)\n");
        return getenv("SDLOP_TEST_STRICT") ? 1 : 0;
    }
    printf("test_x11_pen (%s): proxy %s -> %s, XI opcode %u\n", on ? "on" : "off", proxy_disp, disp, xi_opcode);

    setenv("DISPLAY", proxy_disp, 1);
    setenv("SDL_VIDEODRIVER", "x11", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDLOP_X11_NO_SHM", "1", 1);
    if (!on) {
        setenv("SDLOP_X11_NO_PEN", "1", 1);
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        stop_proxy();
        return 1;
    }
    const char *title = "SDLop x11 pen test";
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

    event_log log;
    const double hover[5] = { 1000, 2000, 1024, -32, 63 };

    /* --- plug the tablet in ------------------------------------------ */
    atomic_store(&fake_present, true);
    queue_hierarchy(XCB_INPUT_HIERARCHY_MASK_SLAVE_ADDED);
    flush_and_collect(&log);
    SDL_PenID stylus = 0, eraser = 0;
    for (int i = 0; i < log.n; i++) {
        if (log.ev[i].type == SDL_EVENT_PEN_PROXIMITY_IN) {
            if (!stylus) {
                stylus = log.ev[i].pproximity.which;
            } else if (!eraser) {
                eraser = log.ev[i].pproximity.which;
            }
        }
    }
    if (!on) {
        CHECK(count_pen(&log) == 0, "no pens with SDLOP_X11_NO_PEN (%d pen events)", count_pen(&log));
        queue_device(XCB_INPUT_MOTION, STYLUS, 1000, 0, xid, 30.5, 40.25, 0x1f, hover);
        queue_core(XCB_MOTION_NOTIFY, 0, 1000, xid, 99, 98);
        flush_and_collect(&log);
        CHECK(count_pen(&log) == 0, "no pen events with SDLOP_X11_NO_PEN (%d)", count_pen(&log));
        CHECK(has_core_mouse(&log, SDL_EVENT_MOUSE_MOTION, 99.0f), "core motion is the mouse without pens");
        goto done;
    }
    CHECK(stylus != 0 && eraser != 0 && stylus != eraser, "two pens added in proximity (%u, %u)", (unsigned)stylus,
          (unsigned)eraser);
    CHECK(SDL_GetPenDeviceType(stylus) == SDL_PEN_DEVICE_TYPE_UNKNOWN, "stylus device type");

    /* --- hover: motion + axes, core copy dropped ---------------------- */
    queue_device(XCB_INPUT_MOTION, STYLUS, 1000, 0, xid, 30.5, 40.25, 0x1f, hover);
    queue_core(XCB_MOTION_NOTIFY, 0, 1000, xid, 99, 98); /* the master's copy */
    queue_core(XCB_MOTION_NOTIFY, 0, 1001, xid, 5, 6);   /* a real mouse */
    flush_and_collect(&log);
    int i = find(&log, SDL_EVENT_PEN_MOTION, 0);
    CHECK(i >= 0 && log.ev[i].pmotion.which == stylus && near(log.ev[i].pmotion.x, 30.5f) &&
              near(log.ev[i].pmotion.y, 40.25f) && log.ev[i].pmotion.windowID == SDL_GetWindowID(w),
          "PEN_MOTION at (30.5,40.25)");
    CHECK(has_axis(&log, stylus, SDL_PEN_AXIS_PRESSURE, 0.5f), "pressure 1024/2048 -> 0.5");
    CHECK(has_axis(&log, stylus, SDL_PEN_AXIS_XTILT, -0.5f), "tilt x -32 of -64 -> -0.5");
    CHECK(has_axis(&log, stylus, SDL_PEN_AXIS_YTILT, 1.0f), "tilt y 63 of 63 -> 1");
    i = find(&log, SDL_EVENT_MOUSE_MOTION, 0);
    CHECK(i >= 0 && log.ev[i].motion.which == SDL_PEN_MOUSEID && near(log.ev[i].motion.x, 30.5f),
          "the pen moves the pen mouse");
    CHECK(!has_core_mouse(&log, SDL_EVENT_MOUSE_MOTION, 99.0f), "the master's core copy of pen motion is dropped");
    CHECK(has_core_mouse(&log, SDL_EVENT_MOUSE_MOTION, 5.0f), "other core motion still is mouse motion");

    /* --- tip down (core copy dropped), barrel button ------------------ */
    queue_device(XCB_INPUT_BUTTON_PRESS, STYLUS, 1010, 1, xid, 31, 41, 0, NULL);
    queue_core(XCB_BUTTON_PRESS, 1, 1010, xid, 31, 41);
    queue_device(XCB_INPUT_BUTTON_PRESS, STYLUS, 1011, 2, xid, 31, 41, 0, NULL);
    queue_core(XCB_BUTTON_PRESS, 2, 1011, xid, 31, 41);
    flush_and_collect(&log);
    i = find(&log, SDL_EVENT_PEN_DOWN, 0);
    CHECK(i >= 0 && log.ev[i].ptouch.which == stylus && !log.ev[i].ptouch.eraser, "tip: PEN_DOWN, not eraser");
    i = find(&log, SDL_EVENT_PEN_BUTTON_DOWN, 0);
    CHECK(i >= 0 && log.ev[i].pbutton.button == 1, "button 2 -> pen button 1");
    i = find(&log, SDL_EVENT_MOUSE_BUTTON_DOWN, 0);
    CHECK(i >= 0 && log.ev[i].button.which == SDL_PEN_MOUSEID && log.ev[i].button.button == SDL_BUTTON_LEFT,
          "the tip presses the pen mouse's left button");
    CHECK(!has_core_mouse(&log, SDL_EVENT_MOUSE_BUTTON_DOWN, 0), "core copies of pen buttons dropped");

    queue_device(XCB_INPUT_BUTTON_RELEASE, STYLUS, 1020, 2, xid, 31, 41, 0, NULL);
    queue_device(XCB_INPUT_BUTTON_RELEASE, STYLUS, 1021, 1, xid, 31, 41, 0, NULL);
    queue_core(XCB_BUTTON_RELEASE, 1, 1021, xid, 31, 41);
    flush_and_collect(&log);
    CHECK(find(&log, SDL_EVENT_PEN_BUTTON_UP, 0) >= 0 && find(&log, SDL_EVENT_PEN_UP, 0) >= 0, "pen button + tip up");
    i = find(&log, SDL_EVENT_MOUSE_BUTTON_UP, 0);
    CHECK(i >= 0 && log.ev[i].button.which == SDL_PEN_MOUSEID, "pen mouse released");
    CHECK(!has_core_mouse(&log, SDL_EVENT_MOUSE_BUTTON_UP, 0), "core copy of the release dropped");

    /* --- the eraser --------------------------------------------------- */
    queue_device(XCB_INPUT_BUTTON_PRESS, ERASER, 1030, 1, xid, 50, 50, 0, NULL);
    queue_device(XCB_INPUT_BUTTON_RELEASE, ERASER, 1031, 1, xid, 50, 50, 0, NULL);
    flush_and_collect(&log);
    i = find(&log, SDL_EVENT_PEN_DOWN, 0);
    CHECK(i >= 0 && log.ev[i].ptouch.which == eraser && log.ev[i].ptouch.eraser, "eraser: PEN_DOWN eraser=true");

    /* --- unplug ------------------------------------------------------- */
    const int q0 = atomic_load(&stat_queries), r0 = atomic_load(&stat_query_replies),
              w0 = atomic_load(&stat_rewrites), j0 = atomic_load(&stat_injections);
    atomic_store(&fake_present, false);
    queue_hierarchy(XCB_INPUT_HIERARCHY_MASK_SLAVE_REMOVED);
    flush_and_collect(&log);
    char diag[512];
    snprintf(diag, sizeof(diag), "queries %d->%d replies %d->%d rewrites %d->%d injections %d->%d types %d %d [%s]",
             q0, atomic_load(&stat_queries), r0, atomic_load(&stat_query_replies), w0, atomic_load(&stat_rewrites), j0,
             atomic_load(&stat_injections), (int)SDL_GetPenDeviceType(stylus),
             (int)SDL_GetPenDeviceType(eraser), SDL_GetError());
    int outs = 0;
    for (i = 0; i < log.n; i++) {
        outs += log.ev[i].type == SDL_EVENT_PEN_PROXIMITY_OUT;
    }
    CHECK(outs == 2, "unplugging: proximity out for both pens (%d; %s)", outs, diag);
    CHECK(SDL_GetPenDeviceType(stylus) == SDL_PEN_DEVICE_TYPE_INVALID &&
              SDL_GetPenDeviceType(eraser) == SDL_PEN_DEVICE_TYPE_INVALID,
          "removed pens are gone");
    /* events from the removed device are ignored */
    queue_device(XCB_INPUT_MOTION, STYLUS, 1040, 0, xid, 60, 60, 0x1f, hover);
    flush_and_collect(&log);
    CHECK(count_pen(&log) == 0, "no events from a removed pen (%d)", count_pen(&log));

done:
    SDL_DestroyWindow(w);
    SDL_Quit();
    stop_proxy();
    xcb_disconnect(obs);
    if (failures) {
        printf("test_x11_pen (%s): FAIL (%d checks)\n", on ? "on" : "off", failures);
        return 1;
    }
    printf("test_x11_pen (%s): PASS\n", on ? "on" : "off");
    return 0;
}
