/*
  SDLop test: X11 system clipboard and primary selection.

  Another X client is played by raw xcb connections in this process:
    - a requestor (main thread) converts SDL's selections while pumping
      SDL's events, so SDLop answers SelectionRequest as it would for any
      other application;
    - an owner (separate thread) holds CLIPBOARD / PRIMARY and serves
      requests while SDL blocks in SDL_GetClipboardText() & co.
  Both directions are checked with plain and INCR (chunked) transfers, plus
  TARGETS, custom mime types, ownership loss / takeover and, when XFixes is
  available, SDL_EVENT_CLIPBOARD_UPDATE for a foreign owner.
  Skips cleanly when DISPLAY is unset.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

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

static xcb_screen_t *screen_of(xcb_connection_t *c, int num)
{
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < num; i++) {
        xcb_screen_next(&si);
    }
    return si.data;
}

static xcb_atom_t atom_on(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

static xcb_window_t make_window(xcb_connection_t *c, xcb_screen_t *s)
{
    xcb_window_t w = xcb_generate_id(c);
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &mask);
    xcb_flush(c);
    return w;
}

static struct
{
    xcb_atom_t CLIPBOARD, PRIMARY, TARGETS, INCR, UTF8_STRING, TEXT_PLAIN_UTF8, XTEST, PROP;
} A;

/* ------------------------------------------------------------------ */
/* requestor: reads SDL's selections                                   */
/* ------------------------------------------------------------------ */

static xcb_connection_t *rq;
static xcb_window_t rq_win;

/* next event on rq, pumping SDL meanwhile (SDL must answer our requests) */
static xcb_generic_event_t *rq_wait(uint8_t type, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(rq)) != NULL) {
            if ((ev->response_type & 0x7f) == type) {
                return ev;
            }
            free(ev);
        }
        struct pollfd pfd = { xcb_get_file_descriptor(rq), POLLIN, 0 };
        poll(&pfd, 1, 5);
    }
    return NULL;
}

static void *get_prop(xcb_connection_t *c, xcb_window_t w, xcb_atom_t prop, xcb_atom_t *type, size_t *len)
{
    xcb_get_property_reply_t *r =
        xcb_get_property_reply(c, xcb_get_property(c, 1, w, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x1fffffff), NULL);
    *len = 0;
    *type = XCB_NONE;
    if (!r) {
        return NULL;
    }
    *type = r->type;
    int n = xcb_get_property_value_length(r);
    char *d = (char *)malloc((size_t)n + 1);
    memcpy(d, xcb_get_property_value(r), (size_t)n);
    d[n] = '\0';
    *len = (size_t)n;
    free(r);
    return d;
}

/* ConvertSelection like any client; NULL = refused/timeout. *incr tells
 * whether SDL used the INCR protocol */
static char *request(xcb_atom_t sel, xcb_atom_t target, size_t *len, xcb_atom_t *type, bool *incr)
{
    *len = 0;
    if (incr) {
        *incr = false;
    }
    xcb_delete_property(rq, rq_win, A.PROP);
    xcb_convert_selection(rq, rq_win, sel, target, A.PROP, XCB_CURRENT_TIME);
    xcb_flush(rq);
    xcb_selection_notify_event_t *sn = (xcb_selection_notify_event_t *)rq_wait(XCB_SELECTION_NOTIFY, 2000);
    if (!sn) {
        fprintf(stderr, "  (no SelectionNotify)\n");
        return NULL;
    }
    bool refused = sn->property == XCB_NONE;
    free(sn);
    if (refused) {
        return NULL;
    }
    char *data = (char *)get_prop(rq, rq_win, A.PROP, type, len);
    xcb_flush(rq);
    if (!data || *type != A.INCR) {
        return data;
    }
    if (incr) {
        *incr = true;
    }
    free(data);
    size_t total = 0;
    char *buf = (char *)calloc(1, 1);
    for (;;) {
        xcb_property_notify_event_t *pn = NULL;
        for (;;) {
            pn = (xcb_property_notify_event_t *)rq_wait(XCB_PROPERTY_NOTIFY, 2000);
            if (!pn || (pn->atom == A.PROP && pn->state == XCB_PROPERTY_NEW_VALUE)) {
                break;
            }
            free(pn);
        }
        if (!pn) {
            fprintf(stderr, "  (INCR stalled after %zu bytes)\n", total);
            free(buf);
            return NULL;
        }
        free(pn);
        size_t n;
        char *chunk = (char *)get_prop(rq, rq_win, A.PROP, type, &n);
        xcb_flush(rq);
        if (!chunk || n == 0) {
            free(chunk);
            break;
        }
        buf = (char *)realloc(buf, total + n + 1);
        memcpy(buf + total, chunk, n);
        total += n;
        buf[total] = '\0';
        free(chunk);
    }
    *len = total;
    return buf;
}

static bool list_has(const xcb_atom_t *atoms, size_t n, xcb_atom_t a)
{
    for (size_t i = 0; i < n; i++) {
        if (atoms[i] == a) {
            return true;
        }
    }
    return false;
}

static xcb_window_t owner_of(xcb_atom_t sel)
{
    xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(rq, xcb_get_selection_owner(rq, sel), NULL);
    xcb_window_t w = r ? r->owner : XCB_NONE;
    free(r);
    return w;
}

/* ------------------------------------------------------------------ */
/* owner: serves selections to SDL from another thread                 */
/* ------------------------------------------------------------------ */

static xcb_connection_t *ow;
static xcb_window_t ow_win;
static pthread_t ow_thread;
static atomic_bool ow_stop;
static atomic_bool ow_lost; /* got SelectionClear */
static xcb_atom_t ow_selection;
static const char *ow_text;
static size_t ow_text_len;
static size_t ow_chunk; /* > 0: INCR when the text is larger */
static const unsigned char ow_binary[] = { 0x00, 0x01, 0xfe, 0xff, 0x00, 'x' };

typedef struct
{
    xcb_window_t requestor;
    xcb_atom_t property, type;
    size_t offset;
    bool active, done;
} OwIncr;

static void ow_notify(const xcb_selection_request_event_t *req, xcb_atom_t property)
{
    xcb_selection_notify_event_t n;
    memset(&n, 0, sizeof(n));
    n.response_type = XCB_SELECTION_NOTIFY;
    n.time = req->time;
    n.requestor = req->requestor;
    n.selection = req->selection;
    n.target = req->target;
    n.property = property;
    xcb_send_event(ow, 0, req->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&n);
}

static void *ow_main(void *unused)
{
    (void)unused;
    OwIncr incr = { 0 };
    while (!atomic_load(&ow_stop)) {
        xcb_generic_event_t *ev = xcb_poll_for_event(ow);
        if (!ev) {
            struct pollfd pfd = { xcb_get_file_descriptor(ow), POLLIN, 0 };
            poll(&pfd, 1, 5);
            continue;
        }
        uint8_t type = ev->response_type & 0x7f;
        if (type == XCB_SELECTION_CLEAR) {
            atomic_store(&ow_lost, true);
        } else if (type == XCB_SELECTION_REQUEST) {
            xcb_selection_request_event_t *req = (xcb_selection_request_event_t *)ev;
            if (req->target == A.TARGETS) {
                xcb_atom_t t[] = { A.TARGETS, A.UTF8_STRING, A.TEXT_PLAIN_UTF8, A.XTEST };
                xcb_change_property(ow, XCB_PROP_MODE_REPLACE, req->requestor, req->property, XCB_ATOM_ATOM, 32,
                                    ow_selection == A.PRIMARY ? 3 : 4, t);
                ow_notify(req, req->property);
            } else if (req->target == A.XTEST && ow_selection == A.CLIPBOARD) {
                xcb_change_property(ow, XCB_PROP_MODE_REPLACE, req->requestor, req->property, A.XTEST, 8,
                                    sizeof(ow_binary), ow_binary);
                ow_notify(req, req->property);
            } else if (req->target == A.UTF8_STRING || req->target == A.TEXT_PLAIN_UTF8) {
                if (ow_chunk && ow_text_len > ow_chunk) {
                    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
                    xcb_change_window_attributes(ow, req->requestor, XCB_CW_EVENT_MASK, &mask);
                    uint32_t sz = (uint32_t)ow_text_len;
                    xcb_change_property(ow, XCB_PROP_MODE_REPLACE, req->requestor, req->property, A.INCR, 32, 1,
                                        &sz);
                    incr = (OwIncr){ req->requestor, req->property, req->target, 0, true, false };
                } else {
                    xcb_change_property(ow, XCB_PROP_MODE_REPLACE, req->requestor, req->property, req->target, 8,
                                        (uint32_t)ow_text_len, ow_text);
                }
                ow_notify(req, req->property);
            } else {
                ow_notify(req, XCB_NONE);
            }
        } else if (type == XCB_PROPERTY_NOTIFY && incr.active) {
            xcb_property_notify_event_t *pn = (xcb_property_notify_event_t *)ev;
            if (pn->window == incr.requestor && pn->atom == incr.property && pn->state == XCB_PROPERTY_DELETE) {
                if (incr.done) {
                    incr.active = false;
                } else {
                    size_t n = ow_text_len - incr.offset;
                    if (n > ow_chunk) {
                        n = ow_chunk;
                    }
                    xcb_change_property(ow, XCB_PROP_MODE_REPLACE, incr.requestor, incr.property, incr.type, 8,
                                        (uint32_t)n, ow_text + incr.offset);
                    incr.offset += n;
                    incr.done = n == 0;
                }
            }
        }
        free(ev);
        xcb_flush(ow);
    }
    return NULL;
}

static void own(xcb_atom_t sel, const char *text, size_t chunk)
{
    ow_selection = sel;
    ow_text = text;
    ow_text_len = strlen(text);
    ow_chunk = chunk;
    atomic_store(&ow_lost, false);
    atomic_store(&ow_stop, false);
    xcb_set_selection_owner(ow, ow_win, sel, XCB_CURRENT_TIME);
    xcb_flush(ow);
    pthread_create(&ow_thread, NULL, ow_main, NULL);
}

static void stop_owning(void)
{
    atomic_store(&ow_stop, true);
    pthread_join(ow_thread, NULL);
}

/* ------------------------------------------------------------------ */

static int cleanup_calls;
static const char custom_data[] = "custom\0payload";

static const void *SDLCALL custom_cb(void *userdata, const char *mime_type, size_t *size)
{
    (void)userdata;
    if (strcmp(mime_type, "application/x-sdlop-test") == 0) {
        *size = sizeof(custom_data);
        return custom_data;
    }
    *size = 0;
    return NULL;
}

static void SDLCALL custom_cleanup(void *userdata)
{
    (void)userdata;
    cleanup_calls++;
}

static char *big_text(size_t n, char seed)
{
    char *s = (char *)malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        s[i] = (char)('a' + (i * 7 + (size_t)seed) % 26);
    }
    s[n] = '\0';
    return s;
}

/* pump until a foreign-owner clipboard update listing `mime` arrives */
static bool wait_foreign_update(const char *mime, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_CLIPBOARD_UPDATE && !e.clipboard.owner) {
                for (int i = 0; i < e.clipboard.num_mime_types; i++) {
                    if (strcmp(e.clipboard.mime_types[i], mime) == 0) {
                        return true;
                    }
                }
            }
        }
        SDL_Delay(5);
    }
    return false;
}

static void pump_for(int ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)ms;
    while (SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_x11_clipboard: SKIP (DISPLAY not set)\n");
        return 0;
    }
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDL_VIDEODRIVER", "x11", 1);
    const char *xf = getenv("SDLOP_LIB_XCB_XFIXES");
    const bool xfixes = !(xf && strcmp(xf, "none") == 0);

    int sn = 0;
    rq = xcb_connect(NULL, &sn);
    ow = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(rq) || xcb_connection_has_error(ow)) {
        printf("test_x11_clipboard: SKIP (cannot connect to %s)\n", getenv("DISPLAY"));
        return 0;
    }
    rq_win = make_window(rq, screen_of(rq, sn));
    ow_win = make_window(ow, screen_of(ow, sn));
    A.CLIPBOARD = atom_on(rq, "CLIPBOARD");
    A.PRIMARY = XCB_ATOM_PRIMARY;
    A.TARGETS = atom_on(rq, "TARGETS");
    A.INCR = atom_on(rq, "INCR");
    A.UTF8_STRING = atom_on(rq, "UTF8_STRING");
    A.TEXT_PLAIN_UTF8 = atom_on(rq, "text/plain;charset=utf-8");
    A.XTEST = atom_on(rq, "application/x-test");
    A.PROP = atom_on(rq, "TEST_SEL");
    printf("test_x11_clipboard: %s, xfixes: %s\n", getenv("DISPLAY"), xfixes ? "auto" : "disabled");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    CHECK(strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0, "video driver is x11");
    size_t len;
    xcb_atom_t type;
    bool incr;

    /* 1. SDL owns CLIPBOARD: text */
    const char *hello = "h\xc3\xa9llo clipboard";
    CHECK(SDL_SetClipboardText(hello), "SDL_SetClipboardText: %s", SDL_GetError());
    CHECK(owner_of(A.CLIPBOARD) != XCB_NONE && owner_of(A.CLIPBOARD) != rq_win, "SDL owns CLIPBOARD");
    char *got = request(A.CLIPBOARD, A.TARGETS, &len, &type, NULL);
    CHECK(got && type == XCB_ATOM_ATOM, "TARGETS answered with ATOM list");
    if (got && type == XCB_ATOM_ATOM) {
        const xcb_atom_t *t = (const xcb_atom_t *)got;
        size_t n = len / 4;
        CHECK(list_has(t, n, A.TARGETS), "TARGETS lists TARGETS");
        CHECK(list_has(t, n, A.UTF8_STRING), "TARGETS lists UTF8_STRING");
        CHECK(list_has(t, n, A.TEXT_PLAIN_UTF8), "TARGETS lists text/plain;charset=utf-8");
    }
    free(got);
    got = request(A.CLIPBOARD, A.UTF8_STRING, &len, &type, &incr);
    CHECK(got && len == strlen(hello) && memcmp(got, hello, len) == 0, "UTF8_STRING = '%s'", got ? got : "(null)");
    CHECK(!incr, "small text not sent INCR");
    CHECK(type == A.UTF8_STRING, "property type is the target");
    free(got);
    got = request(A.CLIPBOARD, atom_on(rq, "image/png"), &len, &type, NULL);
    CHECK(!got, "unoffered target refused");
    free(got);
    CHECK(SDL_HasClipboardText(), "SDL_HasClipboardText (own)");
    char *own_text = SDL_GetClipboardText();
    CHECK(own_text && strcmp(own_text, hello) == 0, "SDL_GetClipboardText (own) = '%s'", own_text);
    SDL_free(own_text);

    /* 2. custom mime type, binary data with NULs */
    const char *mimes[] = { "application/x-sdlop-test" };
    CHECK(SDL_SetClipboardData(custom_cb, custom_cleanup, NULL, mimes, 1), "SDL_SetClipboardData");
    got = request(A.CLIPBOARD, atom_on(rq, "application/x-sdlop-test"), &len, &type, NULL);
    CHECK(got && len == sizeof(custom_data) && memcmp(got, custom_data, len) == 0, "custom mime data (len %zu)", len);
    free(got);
    got = request(A.CLIPBOARD, A.UTF8_STRING, &len, &type, NULL);
    CHECK(!got, "text refused when only custom data is offered");
    free(got);

    /* 3. large text goes INCR */
    SDL_SetHint("SDLOP_X11_SELECTION_CHUNK", "4096");
    char *big = big_text(200000, 3);
    CHECK(SDL_SetClipboardText(big), "SDL_SetClipboardText (big)");
    CHECK(cleanup_calls == 1, "replacing the offer ran its cleanup (%d)", cleanup_calls);
    got = request(A.CLIPBOARD, A.UTF8_STRING, &len, &type, &incr);
    CHECK(incr, "200000 bytes with 4096-byte chunks sent INCR");
    CHECK(got && len == 200000 && memcmp(got, big, len) == 0, "INCR data intact (len %zu)", len);
    free(got);
    free(big);
    SDL_ResetHint("SDLOP_X11_SELECTION_CHUNK");

    /* 4. SDL owns PRIMARY */
    CHECK(SDL_SetPrimarySelectionText("primary text"), "SDL_SetPrimarySelectionText");
    got = request(A.PRIMARY, A.UTF8_STRING, &len, &type, NULL);
    CHECK(got && strcmp(got, "primary text") == 0, "PRIMARY = '%s'", got ? got : "(null)");
    free(got);
    char *p = SDL_GetPrimarySelectionText();
    CHECK(p && strcmp(p, "primary text") == 0, "SDL_GetPrimarySelectionText (own)");
    SDL_free(p);

    /* 5. another client takes CLIPBOARD */
    CHECK(SDL_SetClipboardData(custom_cb, custom_cleanup, NULL, mimes, 1), "SDL_SetClipboardData (again)");
    int before = cleanup_calls;
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    own(A.CLIPBOARD, "from peer", 0);
    if (xfixes) {
        CHECK(wait_foreign_update("UTF8_STRING", 2000), "SDL_EVENT_CLIPBOARD_UPDATE (owner=false) with peer TARGETS");
    } else {
        pump_for(200);
    }
    CHECK(cleanup_calls == before + 1, "losing the selection canceled our offer (%d)", cleanup_calls - before);
    CHECK(SDL_HasClipboardText(), "SDL_HasClipboardText (peer)");
    char *t = SDL_GetClipboardText();
    CHECK(t && strcmp(t, "from peer") == 0, "SDL_GetClipboardText (peer) = '%s'", t ? t : "(null)");
    SDL_free(t);
    CHECK(SDL_HasClipboardData("application/x-test"), "peer's custom type visible");
    size_t blen = 0;
    void *bin = SDL_GetClipboardData("application/x-test", &blen);
    CHECK(bin && blen == sizeof(ow_binary) && memcmp(bin, ow_binary, blen) == 0, "peer binary data (len %zu)", blen);
    SDL_free(bin);
    CHECK(!SDL_HasClipboardData("image/png"), "type the peer doesn't offer");
    bin = SDL_GetClipboardData("image/png", &blen);
    CHECK(!bin, "no data for a type the peer doesn't offer");
    SDL_free(bin);
    size_t nm = 0;
    char **ml = SDL_GetClipboardMimeTypes(&nm);
    bool has_utf8 = false;
    for (size_t i = 0; ml && i < nm; i++) {
        has_utf8 = has_utf8 || strcmp(ml[i], "UTF8_STRING") == 0;
    }
    CHECK(has_utf8, "SDL_GetClipboardMimeTypes lists the peer's types (%zu)", nm);
    SDL_free(ml);
    stop_owning();

    /* 6. INCR from the peer */
    char *big2 = big_text(300000, 11);
    own(A.CLIPBOARD, big2, 8000);
    if (xfixes) {
        wait_foreign_update("UTF8_STRING", 2000);
    }
    t = SDL_GetClipboardText();
    CHECK(t && strlen(t) == 300000 && strcmp(t, big2) == 0, "INCR receive intact (len %zu)", t ? strlen(t) : 0);
    SDL_free(t);
    stop_owning();
    free(big2);

    /* 7. peer's PRIMARY */
    own(A.PRIMARY, "peer primary", 0);
    pump_for(50);
    CHECK(SDL_HasPrimarySelectionText(), "SDL_HasPrimarySelectionText (peer)");
    p = SDL_GetPrimarySelectionText();
    CHECK(p && strcmp(p, "peer primary") == 0, "SDL_GetPrimarySelectionText (peer) = '%s'", p ? p : "(null)");
    SDL_free(p);
    stop_owning();

    /* 8. clipboard owner goes away */
    xcb_set_selection_owner(ow, XCB_NONE, A.CLIPBOARD, XCB_CURRENT_TIME);
    xcb_flush(ow);
    pump_for(100);
    CHECK(!SDL_HasClipboardText(), "no clipboard text without an owner");

    /* 9. SDL takes the clipboard back from a live peer */
    own(A.CLIPBOARD, "peer again", 0);
    pump_for(100);
    CHECK(SDL_SetClipboardText("back to sdl"), "SDL_SetClipboardText (takeover)");
    Uint64 deadline = SDL_GetTicks() + 1000;
    while (!atomic_load(&ow_lost) && SDL_GetTicks() < deadline) {
        SDL_Delay(5);
    }
    CHECK(atomic_load(&ow_lost), "peer got SelectionClear");
    stop_owning();
    got = request(A.CLIPBOARD, A.UTF8_STRING, &len, &type, NULL);
    CHECK(got && strcmp(got, "back to sdl") == 0, "CLIPBOARD after takeover = '%s'", got ? got : "(null)");
    free(got);

    /* 10. clearing gives the selection up */
    CHECK(SDL_ClearClipboardData(), "SDL_ClearClipboardData");
    CHECK(owner_of(A.CLIPBOARD) == XCB_NONE, "CLIPBOARD released");
    CHECK(!SDL_HasClipboardText(), "nothing after clearing");

    SDL_Quit();
    xcb_disconnect(rq);
    xcb_disconnect(ow);
    if (failures) {
        fprintf(stderr, "test_x11_clipboard: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_x11_clipboard: PASS\n");
    return 0;
}
