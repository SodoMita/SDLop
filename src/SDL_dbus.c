/*
  SDLop - a small D-Bus client (session bus only), written from the D-Bus
  specification instead of loading libdbus: stock SDL3 dlopens libdbus-1
  for the desktop portal (system theme, file dialogs), the Unity launcher
  progress and friends; SDLop speaks the wire protocol itself, so there is
  no build or runtime dependency.

  - transport: unix:path= / unix:abstract= from DBUS_SESSION_BUS_ADDRESS,
    else $XDG_RUNTIME_DIR/bus; SASL EXTERNAL authentication; Hello
  - marshalling of all basic types, arrays, structs, dict entries and
    variants, either byte order on input (little endian on output)
  - method calls with a timeout (signals arriving meanwhile are queued),
    fire-and-forget messages, AddMatch, signal handlers dispatched from
    SDL_PumpEvents (main thread)
  - the message (de)serializers are exported to the tests, which play the
    bus themselves

  SDLOP_NO_DBUS=1 disables it (no connection is ever made).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DBUS_MAX_MESSAGE (64u * 1024u * 1024u)

struct SDLop_DBusMsg
{
    Uint8 type;
    Uint8 flags;
    Uint32 serial;
    Uint32 reply_serial;
    char *path, *iface, *member, *error_name, *dest, *sender, *sig;
    Uint8 *body;
    size_t body_len, body_cap;
    bool swap; /* body is big endian */
    bool oom;
};

/* ------------------------------------------------------------------ */
/* signatures                                                          */
/* ------------------------------------------------------------------ */

static size_t type_align(char t)
{
    switch (t) {
    case 'n':
    case 'q':
        return 2;
    case 'b':
    case 'i':
    case 'u':
    case 's':
    case 'o':
    case 'a':
    case 'h':
        return 4;
    case 'x':
    case 't':
    case 'd':
    case '(':
    case '{':
        return 8;
    default: /* y g v */
        return 1;
    }
}

/* past one complete type, NULL if malformed */
static const char *sig_skip(const char *s, const char *end)
{
    if (!s || s >= end) {
        return NULL;
    }
    switch (*s) {
    case 'a':
        return sig_skip(s + 1, end);
    case '(':
    case '{': {
        const char close = *s == '(' ? ')' : '}';
        s++;
        while (s && s < end && *s != close) {
            s = sig_skip(s, end);
        }
        return s && s < end ? s + 1 : NULL;
    }
    default:
        return *s && strchr("ybnqiuxtdsogvh", *s) ? s + 1 : NULL;
    }
}

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static bool take(SDLop_DBusIter *it, size_t align, size_t n, const Uint8 **p)
{
    const size_t pos = align_up(it->pos, align);
    if (pos > it->end || n > it->end - pos) {
        return false;
    }
    *p = it->data + pos;
    it->pos = pos + n;
    return true;
}

static Uint32 rd32(const SDLop_DBusIter *it, const Uint8 *p)
{
    if (it->swap) {
        return ((Uint32)p[0] << 24) | ((Uint32)p[1] << 16) | ((Uint32)p[2] << 8) | p[3];
    }
    return (Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16) | ((Uint32)p[3] << 24);
}

static Uint64 rd64(const SDLop_DBusIter *it, const Uint8 *p)
{
    const Uint64 a = rd32(it, p), b = rd32(it, p + 4);
    return it->swap ? (a << 32) | b : (b << 32) | a;
}

static const char *cur_sig(const SDLop_DBusIter *it)
{
    return it->array ? it->elem : it->sig;
}

int SDLOP_DBus_IterType(const SDLop_DBusIter *it)
{
    if (it->array) {
        return it->pos < it->end ? *it->elem : 0;
    }
    return it->sig && it->sig < it->sig_end ? *it->sig : 0;
}

static bool read_string(SDLop_DBusIter *it, bool short_len, const char **out)
{
    const Uint8 *p;
    Uint32 len;
    if (short_len) {
        if (!take(it, 1, 1, &p)) {
            return false;
        }
        len = *p;
    } else {
        if (!take(it, 4, 4, &p)) {
            return false;
        }
        len = rd32(it, p);
    }
    if (len >= DBUS_MAX_MESSAGE || !take(it, 1, (size_t)len + 1, &p) || p[len] != '\0' || memchr(p, '\0', len)) {
        return false;
    }
    *out = (const char *)p;
    return true;
}

static bool skip_value(SDLop_DBusIter *it);

static bool advance(SDLop_DBusIter *it)
{
    if (!it->array) {
        it->sig = sig_skip(it->sig, it->sig_end);
    }
    return it->array || it->sig != NULL;
}

static bool skip_value(SDLop_DBusIter *it)
{
    const char t = (char)SDLOP_DBus_IterType(it);
    const Uint8 *p;
    const char *s;
    switch (t) {
    case 'y':
        if (!take(it, 1, 1, &p)) {
            return false;
        }
        break;
    case 'n':
    case 'q':
        if (!take(it, 2, 2, &p)) {
            return false;
        }
        break;
    case 'b':
    case 'i':
    case 'u':
    case 'h':
        if (!take(it, 4, 4, &p)) {
            return false;
        }
        break;
    case 'x':
    case 't':
    case 'd':
        if (!take(it, 8, 8, &p)) {
            return false;
        }
        break;
    case 's':
    case 'o':
    case 'g':
        if (!read_string(it, t == 'g', &s)) {
            return false;
        }
        break;
    case 'v':
    case '(':
    case '{': {
        SDLop_DBusIter sub;
        if (!SDLOP_DBus_IterRecurse(it, &sub)) {
            return false;
        }
        while (SDLOP_DBus_IterType(&sub)) {
            if (!skip_value(&sub)) {
                return false;
            }
        }
        if (sub.sig != sub.sig_end) {
            return false;
        }
        it->pos = sub.pos;
        break;
    }
    case 'a': {
        SDLop_DBusIter sub;
        if (!SDLOP_DBus_IterRecurse(it, &sub)) {
            return false;
        }
        it->pos = sub.end;
        break;
    }
    default:
        return false;
    }
    return advance(it);
}

bool SDLOP_DBus_IterNext(SDLop_DBusIter *it)
{
    return skip_value(it) && SDLOP_DBus_IterType(it) != 0;
}

bool SDLOP_DBus_IterRecurse(const SDLop_DBusIter *it, SDLop_DBusIter *sub)
{
    const char *s = cur_sig(it);
    const char *s_end = it->array ? it->elem_end : it->sig_end;
    SDLop_DBusIter tmp = *it;
    const Uint8 *p;
    if (!SDLOP_DBus_IterType(it)) {
        return false;
    }
    memset(sub, 0, sizeof(*sub));
    sub->data = it->data;
    sub->swap = it->swap;
    switch (*s) {
    case 'a': {
        if (!take(&tmp, 4, 4, &p)) {
            return false;
        }
        const Uint32 n = rd32(it, p);
        const char *elem = s + 1;
        const char *elem_end = sig_skip(elem, s_end);
        if (!elem_end) {
            return false;
        }
        const size_t start = align_up(tmp.pos, type_align(*elem));
        if (n > DBUS_MAX_MESSAGE || start > tmp.end || n > tmp.end - start) {
            return false;
        }
        sub->pos = start;
        sub->end = start + n;
        sub->array = true;
        sub->elem = elem;
        sub->elem_end = elem_end;
        return true;
    }
    case 'v': {
        const char *vs;
        if (!read_string(&tmp, true, &vs)) {
            return false;
        }
        const char *ve = vs + strlen(vs);
        if (sig_skip(vs, ve) != ve) {
            return false; /* exactly one complete type */
        }
        sub->pos = tmp.pos;
        sub->end = it->end;
        sub->sig = vs;
        sub->sig_end = ve;
        return true;
    }
    case '(':
    case '{': {
        const char *e = sig_skip(s, s_end);
        if (!e) {
            return false;
        }
        sub->pos = align_up(it->pos, 8);
        if (sub->pos > it->end) {
            return false;
        }
        sub->end = it->end;
        sub->sig = s + 1;
        sub->sig_end = e - 1;
        return true;
    }
    default:
        return false;
    }
}

static bool get_fixed(const SDLop_DBusIter *it, char want, size_t size, const Uint8 **p)
{
    SDLop_DBusIter tmp = *it;
    return SDLOP_DBus_IterType(it) == want && take(&tmp, size, size, p);
}

bool SDLOP_DBus_IterGetByte(const SDLop_DBusIter *it, Uint8 *v)
{
    const Uint8 *p;
    if (!get_fixed(it, 'y', 1, &p)) {
        return false;
    }
    *v = *p;
    return true;
}

bool SDLOP_DBus_IterGetBool(const SDLop_DBusIter *it, bool *v)
{
    const Uint8 *p;
    if (!get_fixed(it, 'b', 4, &p)) {
        return false;
    }
    *v = rd32(it, p) != 0;
    return true;
}

bool SDLOP_DBus_IterGetU32(const SDLop_DBusIter *it, Uint32 *v)
{
    const Uint8 *p;
    const int t = SDLOP_DBus_IterType(it);
    if ((t != 'u' && t != 'i') || !get_fixed(it, (char)t, 4, &p)) {
        return false;
    }
    *v = rd32(it, p);
    return true;
}

bool SDLOP_DBus_IterGetI32(const SDLop_DBusIter *it, Sint32 *v)
{
    Uint32 u;
    if (!SDLOP_DBus_IterGetU32(it, &u)) {
        return false;
    }
    *v = (Sint32)u;
    return true;
}

bool SDLOP_DBus_IterGetDouble(const SDLop_DBusIter *it, double *v)
{
    const Uint8 *p;
    if (!get_fixed(it, 'd', 8, &p)) {
        return false;
    }
    const Uint64 bits = rd64(it, p);
    memcpy(v, &bits, sizeof(*v));
    return true;
}

bool SDLOP_DBus_IterGetString(const SDLop_DBusIter *it, const char **v)
{
    const int t = SDLOP_DBus_IterType(it);
    SDLop_DBusIter tmp = *it;
    return (t == 's' || t == 'o' || t == 'g') && read_string(&tmp, t == 'g', v);
}

bool SDLOP_DBus_IterInit(const SDLop_DBusMsg *msg, SDLop_DBusIter *it)
{
    memset(it, 0, sizeof(*it));
    if (!msg) {
        return false;
    }
    it->data = msg->body;
    it->end = msg->body_len;
    it->swap = msg->swap;
    it->sig = msg->sig ? msg->sig : "";
    it->sig_end = it->sig + strlen(it->sig);
    return true;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    Uint8 *buf;
    size_t len, cap;
    bool oom;
} WBuf;

static void w_reserve(WBuf *w, size_t n)
{
    if (w->oom || n == 0 || w->len + n <= w->cap) {
        return;
    }
    size_t cap = w->cap ? w->cap : 128;
    while (cap < w->len + n) {
        cap *= 2;
    }
    Uint8 *nb = (Uint8 *)realloc(w->buf, cap);
    if (!nb) {
        w->oom = true;
        return;
    }
    w->buf = nb;
    w->cap = cap;
}

static void w_pad(WBuf *w, size_t a)
{
    const size_t n = align_up(w->len, a) - w->len;
    w_reserve(w, n);
    if (!w->oom && n) {
        memset(w->buf + w->len, 0, n);
        w->len += n;
    }
}

static void w_raw(WBuf *w, const void *p, size_t n)
{
    w_reserve(w, n);
    if (!w->oom && n) { /* empty body: p may be NULL */
        memcpy(w->buf + w->len, p, n);
        w->len += n;
    }
}

static void w_u32(WBuf *w, Uint32 v)
{
    const Uint8 b[4] = { (Uint8)v, (Uint8)(v >> 8), (Uint8)(v >> 16), (Uint8)(v >> 24) };
    w_pad(w, 4);
    w_raw(w, b, 4);
}

static void w_string(WBuf *w, const char *s, bool short_len)
{
    const size_t n = strlen(s);
    if (short_len) {
        const Uint8 l = (Uint8)(n > 255 ? 255 : n);
        w_raw(w, &l, 1);
        w_raw(w, s, l);
    } else {
        w_u32(w, (Uint32)n);
        w_raw(w, s, n);
    }
    w_raw(w, "", 1);
}

/* the body writer works on the message's body buffer */
static WBuf body_of(SDLop_DBusMsg *m)
{
    WBuf w = { m->body, m->body_len, m->body_cap, m->oom };
    return w;
}

static void body_back(SDLop_DBusMsg *m, const WBuf *w)
{
    m->body = w->buf;
    m->body_len = w->len;
    m->body_cap = w->cap;
    m->oom = w->oom;
}

void SDLOP_DBus_PutByte(SDLop_DBusMsg *m, Uint8 v)
{
    WBuf w = body_of(m);
    w_raw(&w, &v, 1);
    body_back(m, &w);
}

void SDLOP_DBus_PutBool(SDLop_DBusMsg *m, bool v)
{
    SDLOP_DBus_PutU32(m, v ? 1 : 0);
}

void SDLOP_DBus_PutU32(SDLop_DBusMsg *m, Uint32 v)
{
    WBuf w = body_of(m);
    w_u32(&w, v);
    body_back(m, &w);
}

void SDLOP_DBus_PutDouble(SDLop_DBusMsg *m, double v)
{
    Uint64 bits;
    memcpy(&bits, &v, sizeof(bits));
    WBuf w = body_of(m);
    w_pad(&w, 8);
    w_u32(&w, (Uint32)bits);
    w_u32(&w, (Uint32)(bits >> 32));
    body_back(m, &w);
}

void SDLOP_DBus_PutString(SDLop_DBusMsg *m, const char *s)
{
    WBuf w = body_of(m);
    w_string(&w, s ? s : "", false);
    body_back(m, &w);
}

void SDLOP_DBus_PutSignature(SDLop_DBusMsg *m, const char *sig)
{
    WBuf w = body_of(m);
    w_string(&w, sig, true);
    body_back(m, &w);
}

SDLop_DBusArray SDLOP_DBus_OpenArray(SDLop_DBusMsg *m, char elem_type)
{
    SDLop_DBusArray a;
    WBuf w = body_of(m);
    w_u32(&w, 0);
    a.len_pos = w.len - 4;
    w_pad(&w, type_align(elem_type));
    a.start = w.len;
    body_back(m, &w);
    return a;
}

void SDLOP_DBus_CloseArray(SDLop_DBusMsg *m, const SDLop_DBusArray *a)
{
    if (m->oom) {
        return;
    }
    const Uint32 n = (Uint32)(m->body_len - a->start);
    const Uint8 b[4] = { (Uint8)n, (Uint8)(n >> 8), (Uint8)(n >> 16), (Uint8)(n >> 24) };
    memcpy(m->body + a->len_pos, b, 4);
}

void SDLOP_DBus_OpenStruct(SDLop_DBusMsg *m)
{
    WBuf w = body_of(m);
    w_pad(&w, 8);
    body_back(m, &w);
}

/* dict entry {sv}: key, then the variant's signature; write the value next */
void SDLOP_DBus_PutDictVariantKey(SDLop_DBusMsg *m, const char *key, const char *value_sig)
{
    SDLOP_DBus_OpenStruct(m);
    SDLOP_DBus_PutString(m, key);
    SDLOP_DBus_PutSignature(m, value_sig);
}

/* ------------------------------------------------------------------ */
/* messages                                                            */
/* ------------------------------------------------------------------ */

static char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

SDLop_DBusMsg *SDLOP_DBus_NewMessage(Uint8 type, const char *dest, const char *path, const char *iface,
                                     const char *member, const char *body_sig)
{
    SDLop_DBusMsg *m = (SDLop_DBusMsg *)calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->type = type;
    m->dest = dup_or_null(dest);
    m->path = dup_or_null(path);
    m->iface = dup_or_null(iface);
    m->member = dup_or_null(member);
    m->sig = strdup(body_sig ? body_sig : "");
    return m;
}

SDLop_DBusMsg *SDLOP_DBus_NewMethodCall(const char *dest, const char *path, const char *iface, const char *member,
                                        const char *body_sig)
{
    return SDLOP_DBus_NewMessage(SDLOP_DBUS_METHOD_CALL, dest, path, iface, member, body_sig);
}

SDLop_DBusMsg *SDLOP_DBus_NewSignal(const char *path, const char *iface, const char *member, const char *body_sig)
{
    return SDLOP_DBus_NewMessage(SDLOP_DBUS_SIGNAL, NULL, path, iface, member, body_sig);
}

SDLop_DBusMsg *SDLOP_DBus_NewReply(const SDLop_DBusMsg *call, const char *body_sig)
{
    SDLop_DBusMsg *m = SDLOP_DBus_NewMessage(SDLOP_DBUS_METHOD_RETURN, call->sender, NULL, NULL, NULL, body_sig);
    if (m) {
        m->reply_serial = call->serial;
    }
    return m;
}

SDLop_DBusMsg *SDLOP_DBus_NewError(const SDLop_DBusMsg *call, const char *error_name, const char *text)
{
    SDLop_DBusMsg *m = SDLOP_DBus_NewMessage(SDLOP_DBUS_ERROR, call->sender, NULL, NULL, NULL, "s");
    if (m) {
        m->reply_serial = call->serial;
        m->error_name = strdup(error_name);
        SDLOP_DBus_PutString(m, text ? text : "");
    }
    return m;
}

void SDLOP_DBus_FreeMessage(SDLop_DBusMsg *m)
{
    if (!m) {
        return;
    }
    free(m->path);
    free(m->iface);
    free(m->member);
    free(m->error_name);
    free(m->dest);
    free(m->sender);
    free(m->sig);
    free(m->body);
    free(m);
}

Uint8 SDLOP_DBus_MessageType(const SDLop_DBusMsg *m)
{
    return m->type;
}

Uint32 SDLOP_DBus_MessageSerial(const SDLop_DBusMsg *m)
{
    return m->serial;
}

Uint32 SDLOP_DBus_MessageReplySerial(const SDLop_DBusMsg *m)
{
    return m->reply_serial;
}

const char *SDLOP_DBus_MessageField(const SDLop_DBusMsg *m, char which)
{
    switch (which) {
    case 'p':
        return m->path;
    case 'i':
        return m->iface;
    case 'm':
        return m->member;
    case 'e':
        return m->error_name;
    case 'd':
        return m->dest;
    case 's':
        return m->sender;
    case 'g':
        return m->sig;
    default:
        return NULL;
    }
}

void SDLOP_DBus_SetSender(SDLop_DBusMsg *m, const char *sender)
{
    free(m->sender);
    m->sender = dup_or_null(sender);
}

void SDLOP_DBus_SetDestination(SDLop_DBusMsg *m, const char *dest)
{
    free(m->dest);
    m->dest = dup_or_null(dest);
}

void SDLOP_DBus_SetNoReply(SDLop_DBusMsg *m)
{
    m->flags |= 0x1; /* NO_REPLY_EXPECTED */
}

static void header_field(WBuf *w, Uint8 code, char type, const char *s, Uint32 u)
{
    w_pad(w, 8);
    w_raw(w, &code, 1);
    const char sig[2] = { type, '\0' };
    w_string(w, sig, true);
    if (type == 'u') {
        w_u32(w, u);
    } else {
        w_string(w, s, type == 'g');
    }
}

/* the whole message, little endian; malloc'd */
Uint8 *SDLOP_DBus_Serialize(SDLop_DBusMsg *m, Uint32 serial, size_t *len)
{
    WBuf w = { NULL, 0, 0, false };
    if (m->oom) {
        return NULL;
    }
    m->serial = serial;
    const Uint8 fixed[4] = { 'l', m->type, m->flags, 1 };
    w_raw(&w, fixed, 4);
    w_u32(&w, (Uint32)m->body_len);
    w_u32(&w, serial);
    w_u32(&w, 0); /* header fields array length */
    const size_t fields_start = w.len;
    if (m->path) {
        header_field(&w, 1, 'o', m->path, 0);
    }
    if (m->iface) {
        header_field(&w, 2, 's', m->iface, 0);
    }
    if (m->member) {
        header_field(&w, 3, 's', m->member, 0);
    }
    if (m->error_name) {
        header_field(&w, 4, 's', m->error_name, 0);
    }
    if (m->reply_serial) {
        header_field(&w, 5, 'u', NULL, m->reply_serial);
    }
    if (m->dest) {
        header_field(&w, 6, 's', m->dest, 0);
    }
    if (m->sender) {
        header_field(&w, 7, 's', m->sender, 0);
    }
    if (m->sig && *m->sig) {
        header_field(&w, 8, 'g', m->sig, 0);
    }
    const Uint32 flen = (Uint32)(w.len - fields_start);
    w_pad(&w, 8);
    w_raw(&w, m->body, m->body_len);
    if (w.oom) {
        free(w.buf);
        return NULL;
    }
    const Uint8 b[4] = { (Uint8)flen, (Uint8)(flen >> 8), (Uint8)(flen >> 16), (Uint8)(flen >> 24) };
    memcpy(w.buf + 12, b, 4);
    *len = w.len;
    return w.buf;
}

/* one message from buf: 1 = parsed (*consumed set), 0 = need more, -1 = garbage */
int SDLOP_DBus_Parse(const Uint8 *buf, size_t len, SDLop_DBusMsg **out, size_t *consumed)
{
    *out = NULL;
    if (len < 16) {
        return 0;
    }
    if ((buf[0] != 'l' && buf[0] != 'B') || buf[3] != 1) {
        return -1;
    }
    SDLop_DBusIter it;
    memset(&it, 0, sizeof(it));
    it.swap = buf[0] == 'B';
    const Uint32 body_len = rd32(&it, buf + 4);
    const Uint32 fields_len = rd32(&it, buf + 12);
    if (body_len > DBUS_MAX_MESSAGE || fields_len > DBUS_MAX_MESSAGE) {
        return -1;
    }
    const size_t body_off = align_up(16 + (size_t)fields_len, 8);
    const size_t total = body_off + body_len;
    if (len < total) {
        return 0;
    }
    SDLop_DBusMsg *m = (SDLop_DBusMsg *)calloc(1, sizeof(*m));
    if (!m) {
        return -1;
    }
    m->type = buf[1];
    m->flags = buf[2];
    m->serial = rd32(&it, buf + 8);
    m->swap = it.swap;
    /* header fields: a(yv) starting at offset 12 */
    it.data = buf;
    it.pos = 12;
    it.end = 16 + (size_t)fields_len;
    it.sig = "a(yv)";
    it.sig_end = it.sig + 5;
    SDLop_DBusIter arr, st, var;
    bool ok = SDLOP_DBus_IterRecurse(&it, &arr);
    while (ok && SDLOP_DBus_IterType(&arr)) {
        Uint8 code = 0;
        ok = SDLOP_DBus_IterRecurse(&arr, &st) && SDLOP_DBus_IterGetByte(&st, &code) && SDLOP_DBus_IterNext(&st) &&
             SDLOP_DBus_IterRecurse(&st, &var);
        if (!ok) {
            break;
        }
        const char *s = NULL;
        Uint32 u = 0;
        char **slot = NULL;
        switch (code) {
        case 1:
            slot = &m->path;
            break;
        case 2:
            slot = &m->iface;
            break;
        case 3:
            slot = &m->member;
            break;
        case 4:
            slot = &m->error_name;
            break;
        case 6:
            slot = &m->dest;
            break;
        case 7:
            slot = &m->sender;
            break;
        case 8:
            slot = &m->sig;
            break;
        default:
            break;
        }
        if (slot && SDLOP_DBus_IterGetString(&var, &s)) {
            free(*slot);
            *slot = strdup(s);
        } else if (code == 5 && SDLOP_DBus_IterGetU32(&var, &u)) {
            m->reply_serial = u;
        }
        ok = skip_value(&arr);
    }
    if (!ok) {
        SDLOP_DBus_FreeMessage(m);
        return -1;
    }
    if (body_len) {
        m->body = (Uint8 *)malloc(body_len);
        if (!m->body) {
            SDLOP_DBus_FreeMessage(m);
            return -1;
        }
        memcpy(m->body, buf + body_off, body_len);
    }
    m->body_len = m->body_cap = body_len;
    *out = m;
    *consumed = total;
    return 1;
}

/* ------------------------------------------------------------------ */
/* connection                                                          */
/* ------------------------------------------------------------------ */

typedef struct
{
    SDLop_DBusHandler fn;
    void *userdata;
} Handler;

static struct
{
    int fd;
    bool tried;
    Uint32 serial;
    char *unique_name;
    Uint8 *rbuf;
    size_t rlen, rcap;
    SDLop_DBusMsg **queue;
    int nqueue, cqueue;
    Handler handlers[32];
    int nhandlers;
    int dispatching;
} db = { .fd = -1 };

static void close_bus(void)
{
    if (db.fd >= 0) {
        close(db.fd);
    }
    db.fd = -1;
}

static bool write_all_fd(int fd, const void *p, size_t n)
{
    const Uint8 *b = (const Uint8 *)p;
    while (n > 0) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                struct pollfd pf = { fd, POLLOUT, 0 };
                if (poll(&pf, 1, 1000) <= 0) {
                    return false;
                }
                continue;
            }
            return false;
        }
        b += w;
        n -= (size_t)w;
    }
    return true;
}

/* "\r\n"-terminated auth line */
static bool read_line(int fd, char *line, size_t size, int timeout_ms)
{
    size_t n = 0;
    const Uint64 end = SDL_GetTicks() + (Uint64)timeout_ms;
    while (n + 1 < size) {
        const Sint64 left = (Sint64)(end - SDL_GetTicks());
        struct pollfd pf = { fd, POLLIN, 0 };
        if (left <= 0 || poll(&pf, 1, (int)left) <= 0) {
            return false;
        }
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) {
            if (r < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            return false;
        }
        line[n++] = c;
        if (n >= 2 && line[n - 2] == '\r' && line[n - 1] == '\n') {
            line[n - 2] = '\0';
            return true;
        }
    }
    return false;
}

static void unescape(const char *in, size_t n, char *out, size_t size)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < size; i++) {
        if (in[i] == '%' && i + 2 < n) {
            char hex[3] = { in[i + 1], in[i + 2], '\0' };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

/* first usable unix: address of a D-Bus address list */
static int connect_address(const char *addresses)
{
    const char *a = addresses;
    while (a && *a) {
        const char *semi = strchr(a, ';');
        const size_t alen = semi ? (size_t)(semi - a) : strlen(a);
        if (alen > 5 && strncmp(a, "unix:", 5) == 0) {
            struct sockaddr_un sa;
            socklen_t salen = 0;
            memset(&sa, 0, sizeof(sa));
            sa.sun_family = AF_UNIX;
            const char *kv = a + 5;
            while (kv < a + alen) {
                const char *comma = memchr(kv, ',', (size_t)(a + alen - kv));
                const size_t kvlen = comma ? (size_t)(comma - kv) : (size_t)(a + alen - kv);
                char value[sizeof(sa.sun_path)];
                if (kvlen > 5 && strncmp(kv, "path=", 5) == 0) {
                    unescape(kv + 5, kvlen - 5, value, sizeof(value));
                    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", value);
                    salen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(sa.sun_path) + 1);
                } else if (kvlen > 9 && strncmp(kv, "abstract=", 9) == 0) {
                    unescape(kv + 9, kvlen - 9, value, sizeof(value) - 1);
                    const size_t vl = strlen(value);
                    memcpy(sa.sun_path + 1, value, vl);
                    salen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + vl);
                }
                kv += kvlen + (comma ? 1 : 0);
            }
            if (salen) {
                int fd = socket(AF_UNIX, SOCK_STREAM | SDLOP_SOCK_CLOEXEC, 0);
                if (fd >= 0) {
                    SDLOP_EnsureCloexec(fd);
                }
                if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, salen) == 0) {
                    return fd;
                }
                if (fd >= 0) {
                    close(fd);
                }
            }
        }
        a = semi ? semi + 1 : NULL;
    }
    return -1;
}

static bool send_msg(SDLop_DBusMsg *m, Uint32 *serial_out)
{
    if (db.fd < 0) {
        return false;
    }
    size_t len;
    const Uint32 serial = ++db.serial ? db.serial : ++db.serial;
    Uint8 *bytes = SDLOP_DBus_Serialize(m, serial, &len);
    if (!bytes) {
        return false;
    }
    const bool ok = write_all_fd(db.fd, bytes, len);
    free(bytes);
    if (!ok) {
        close_bus();
        return false;
    }
    if (serial_out) {
        *serial_out = serial;
    }
    return true;
}

static void enqueue(SDLop_DBusMsg *m)
{
    if (db.nqueue == db.cqueue) {
        const int cap = db.cqueue ? db.cqueue * 2 : 16;
        SDLop_DBusMsg **nq = (SDLop_DBusMsg **)realloc(db.queue, sizeof(*nq) * (size_t)cap);
        if (!nq) {
            SDLOP_DBus_FreeMessage(m);
            return;
        }
        db.queue = nq;
        db.cqueue = cap;
    }
    db.queue[db.nqueue++] = m;
}

/* read what's there (waiting up to timeout_ms for the first bytes) */
static bool read_some(int timeout_ms)
{
    if (db.fd < 0) {
        return false;
    }
    struct pollfd pf = { db.fd, POLLIN, 0 };
    const int r = poll(&pf, 1, timeout_ms);
    if (r <= 0) {
        return r == 0;
    }
    for (;;) {
        if (db.rcap - db.rlen < 4096) {
            const size_t cap = db.rcap ? db.rcap * 2 : 8192;
            Uint8 *nb = (Uint8 *)realloc(db.rbuf, cap);
            if (!nb) {
                close_bus();
                return false;
            }
            db.rbuf = nb;
            db.rcap = cap;
        }
        ssize_t n = recv(db.fd, db.rbuf + db.rlen, db.rcap - db.rlen, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno == EAGAIN) {
            break;
        }
        if (n <= 0) {
            close_bus(); /* the bus went away */
            return false;
        }
        db.rlen += (size_t)n;
        if ((size_t)n < 4096) {
            break;
        }
    }
    size_t off = 0;
    for (;;) {
        SDLop_DBusMsg *m;
        size_t used;
        const int pr = SDLOP_DBus_Parse(db.rbuf + off, db.rlen - off, &m, &used);
        if (pr < 0) {
            close_bus();
            return false;
        }
        if (pr == 0) {
            break;
        }
        off += used;
        enqueue(m);
    }
    memmove(db.rbuf, db.rbuf + off, db.rlen - off);
    db.rlen -= off;
    return true;
}

static SDLop_DBusMsg *take_reply(Uint32 serial)
{
    for (int i = 0; i < db.nqueue; i++) {
        SDLop_DBusMsg *m = db.queue[i];
        if ((m->type == SDLOP_DBUS_METHOD_RETURN || m->type == SDLOP_DBUS_ERROR) && m->reply_serial == serial) {
            memmove(&db.queue[i], &db.queue[i + 1], sizeof(*db.queue) * (size_t)(db.nqueue - i - 1));
            db.nqueue--;
            return m;
        }
    }
    return NULL;
}

static SDLop_DBusMsg *wait_reply(Uint32 serial, int timeout_ms, const char *what)
{
    const Uint64 end = SDL_GetTicks() + (Uint64)(timeout_ms < 0 ? 25000 : timeout_ms);
    for (;;) {
        SDLop_DBusMsg *r = take_reply(serial);
        if (r) {
            if (r->type == SDLOP_DBUS_ERROR) {
                SDLop_DBusIter it;
                const char *text = "";
                SDLOP_DBus_IterInit(r, &it);
                SDLOP_DBus_IterGetString(&it, &text);
                SDL_SetError("D-Bus: %s: %s", r->error_name ? r->error_name : "error", text);
                SDLOP_DBus_FreeMessage(r);
                return NULL;
            }
            return r;
        }
        const Sint64 left = (Sint64)(end - SDL_GetTicks());
        if (left <= 0) {
            SDL_SetError("D-Bus: no reply to %s", what);
            return NULL;
        }
        if (!read_some((int)left)) {
            SDL_SetError("D-Bus: connection lost");
            return NULL;
        }
    }
}

static SDLop_DBusMsg *call_raw(SDLop_DBusMsg *m, int timeout_ms)
{
    Uint32 serial;
    if (!send_msg(m, &serial)) {
        SDL_SetError("D-Bus: not connected");
        return NULL;
    }
    return wait_reply(serial, timeout_ms, m->member ? m->member : "call");
}

bool SDLOP_DBus_Connect(void)
{
    if (db.fd >= 0) {
        return true;
    }
    if (db.tried) {
        return false;
    }
    db.tried = true;
    if (SDLOP_HintFlag("SDLOP_NO_DBUS", false)) {
        return false;
    }
    const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    char fallback[256];
    if (!addr || !*addr) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (!rt || !*rt) {
            return false;
        }
        snprintf(fallback, sizeof(fallback), "unix:path=%s/bus", rt);
        addr = fallback;
    }
    db.fd = connect_address(addr);
    if (db.fd < 0) {
        return false;
    }
    /* SASL EXTERNAL with our uid, in hex-encoded ASCII */
    char uid[32], hex[80], line[256];
    snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
    for (size_t i = 0; uid[i]; i++) {
        snprintf(hex + 2 * i, 3, "%02x", (unsigned char)uid[i]);
    }
    snprintf(line, sizeof(line), "AUTH EXTERNAL %s\r\n", hex);
    if (!write_all_fd(db.fd, "", 1) || !write_all_fd(db.fd, line, strlen(line)) ||
        !read_line(db.fd, line, sizeof(line), 1000) || strncmp(line, "OK ", 3) != 0 ||
        !write_all_fd(db.fd, "BEGIN\r\n", 7)) {
        close_bus();
        return false;
    }
    SDLop_DBusMsg *hello = SDLOP_DBus_NewMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                    "org.freedesktop.DBus", "Hello", "");
    SDLop_DBusMsg *reply = hello ? call_raw(hello, 1000) : NULL;
    SDLOP_DBus_FreeMessage(hello);
    SDLop_DBusIter it;
    const char *name = NULL;
    if (!reply || !SDLOP_DBus_IterInit(reply, &it) || !SDLOP_DBus_IterGetString(&it, &name)) {
        SDLOP_DBus_FreeMessage(reply);
        close_bus();
        return false;
    }
    db.unique_name = strdup(name);
    SDLOP_DBus_FreeMessage(reply);
    return true;
}

bool SDLOP_DBus_IsConnected(void)
{
    return db.fd >= 0;
}

const char *SDLOP_DBus_UniqueName(void)
{
    return db.unique_name;
}

bool SDLOP_DBus_Send(SDLop_DBusMsg *m)
{
    if (!SDLOP_DBus_Connect()) {
        return false;
    }
    if (m->type == SDLOP_DBUS_METHOD_CALL) {
        SDLOP_DBus_SetNoReply(m);
    }
    return send_msg(m, NULL);
}

SDLop_DBusMsg *SDLOP_DBus_Call(SDLop_DBusMsg *m, int timeout_ms)
{
    if (!SDLOP_DBus_Connect()) {
        SDL_SetError("D-Bus: no session bus");
        return NULL;
    }
    return call_raw(m, timeout_ms);
}

Uint32 SDLOP_DBus_CallAsync(SDLop_DBusMsg *m)
{
    Uint32 serial = 0;
    if (!SDLOP_DBus_Connect() || !send_msg(m, &serial)) {
        return 0;
    }
    return serial;
}

SDLop_DBusMsg *SDLOP_DBus_WaitReply(Uint32 serial, int timeout_ms)
{
    if (!serial || (db.fd < 0 && db.nqueue == 0)) {
        SDL_SetError("D-Bus: not connected");
        return NULL;
    }
    if (db.fd < 0) {
        SDLop_DBusMsg *r = take_reply(serial);
        if (!r) {
            SDL_SetError("D-Bus: connection lost");
        }
        return r && r->type == SDLOP_DBUS_ERROR ? (SDLOP_DBus_FreeMessage(r), NULL) : r;
    }
    return wait_reply(serial, timeout_ms, "call");
}

bool SDLOP_DBus_AddMatch(const char *rule)
{
    SDLop_DBusMsg *m = SDLOP_DBus_NewMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                "org.freedesktop.DBus", "AddMatch", "s");
    if (!m) {
        return false;
    }
    SDLOP_DBus_PutString(m, rule);
    const bool ok = SDLOP_DBus_Send(m);
    SDLOP_DBus_FreeMessage(m);
    return ok;
}

bool SDLOP_DBus_RemoveMatch(const char *rule)
{
    SDLop_DBusMsg *m = SDLOP_DBus_NewMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                "org.freedesktop.DBus", "RemoveMatch", "s");
    if (!m) {
        return false;
    }
    SDLOP_DBus_PutString(m, rule);
    const bool ok = SDLOP_DBus_IsConnected() && SDLOP_DBus_Send(m);
    SDLOP_DBus_FreeMessage(m);
    return ok;
}

bool SDLOP_DBus_AddHandler(SDLop_DBusHandler fn, void *userdata)
{
    if (db.nhandlers >= (int)SDL_arraysize(db.handlers)) {
        return false;
    }
    db.handlers[db.nhandlers].fn = fn;
    db.handlers[db.nhandlers].userdata = userdata;
    db.nhandlers++;
    return true;
}

void SDLOP_DBus_RemoveHandler(SDLop_DBusHandler fn, void *userdata)
{
    for (int i = 0; i < db.nhandlers; i++) {
        if (db.handlers[i].fn == fn && db.handlers[i].userdata == userdata) {
            db.handlers[i].fn = NULL; /* compacted after dispatching */
        }
    }
}

static void compact_handlers(void)
{
    int o = 0;
    for (int i = 0; i < db.nhandlers; i++) {
        if (db.handlers[i].fn) {
            db.handlers[o++] = db.handlers[i];
        }
    }
    db.nhandlers = o;
}

int SDLOP_DBus_GetFD(void)
{
    return db.fd;
}

void SDLOP_DBus_Pump(void)
{
    if (db.fd < 0 && db.nqueue == 0) {
        return;
    }
    if (db.dispatching) {
        return; /* a handler pumped events */
    }
    read_some(0);
    db.dispatching++;
    while (db.nqueue > 0) {
        SDLop_DBusMsg *m = db.queue[0];
        memmove(&db.queue[0], &db.queue[1], sizeof(*db.queue) * (size_t)(db.nqueue - 1));
        db.nqueue--;
        if (m->type == SDLOP_DBUS_SIGNAL || m->type == SDLOP_DBUS_METHOD_RETURN || m->type == SDLOP_DBUS_ERROR) {
            /* signals, and replies to SDLOP_DBus_CallAsync() */
            const int n = db.nhandlers;
            for (int i = 0; i < n; i++) {
                if (db.handlers[i].fn && db.handlers[i].fn(m, db.handlers[i].userdata)) {
                    break;
                }
            }
        } else if (m->type == SDLOP_DBUS_METHOD_CALL && !(m->flags & 0x1)) {
            /* nobody exports objects here */
            SDLop_DBusMsg *e = SDLOP_DBus_NewError(m, "org.freedesktop.DBus.Error.UnknownMethod", "No such object");
            if (e) {
                send_msg(e, NULL);
                SDLOP_DBus_FreeMessage(e);
            }
        }
        /* unclaimed replies (late ones of timed-out calls) are dropped */
        SDLOP_DBus_FreeMessage(m);
    }
    db.dispatching--;
    compact_handlers();
}

void SDLOP_DBus_Quit(void)
{
    close_bus();
    for (int i = 0; i < db.nqueue; i++) {
        SDLOP_DBus_FreeMessage(db.queue[i]);
    }
    free(db.queue);
    free(db.rbuf);
    free(db.unique_name);
    const int dispatching = db.dispatching; /* quit from inside a handler */
    memset(&db, 0, sizeof(db));
    db.fd = -1;
    db.dispatching = dispatching;
}
