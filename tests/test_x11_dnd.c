/*
  SDLop test: X11 drag-and-drop target (XDND v5).

  A raw xcb client plays the drag source against an SDL window: XdndEnter,
  XdndPosition (expects XdndStatus), XdndDrop, serving the XdndSelection
  conversion, and expects XdndFinished. Cases:
    files  - three inline types, text/uri-list chosen: escapes, comment,
             non-local URIs skipped -> BEGIN, POSITION, FILE x2, COMPLETE
    list   - more than three types via XdndTypeList, a text type chosen
    incr   - 3 x 100 kB text lines served with INCR
    refuse - only image/png: XdndStatus/XdndFinished refuse, no events
    leave  - XdndLeave after a position: BEGIN, POSITION, COMPLETE
  plus XdndAware following SDL_SetEventEnabled(SDL_EVENT_DROP_*).
  Skips cleanly when DISPLAY is unset.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <poll.h>
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

static xcb_connection_t *c;
static xcb_screen_t *screen;
static xcb_window_t src, target;

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

static struct
{
    xcb_atom_t XdndAware, XdndEnter, XdndPosition, XdndStatus, XdndLeave, XdndDrop, XdndFinished, XdndSelection,
        XdndTypeList, XdndActionCopy, INCR, uri_list, utf8_text, text_plain, png, foo, TEXT;
} A;

/* ------------------------------------------------------------------ */
/* drag source                                                         */
/* ------------------------------------------------------------------ */

static const char *serve_data;
static size_t serve_len;
static size_t serve_chunk; /* > 0: INCR */
static struct
{
    bool active, done;
    xcb_window_t requestor;
    xcb_atom_t property, type;
    size_t offset;
} incr;

static void client_message(xcb_atom_t type, uint32_t l0, uint32_t l1, uint32_t l2, uint32_t l3, uint32_t l4)
{
    xcb_client_message_event_t m;
    memset(&m, 0, sizeof(m));
    m.response_type = XCB_CLIENT_MESSAGE;
    m.format = 32;
    m.window = target;
    m.type = type;
    m.data.data32[0] = l0;
    m.data.data32[1] = l1;
    m.data.data32[2] = l2;
    m.data.data32[3] = l3;
    m.data.data32[4] = l4;
    xcb_send_event(c, 0, target, XCB_EVENT_MASK_NO_EVENT, (const char *)&m);
    xcb_flush(c);
}

static void handle_source_event(xcb_generic_event_t *ev)
{
    uint8_t type = ev->response_type & 0x7f;
    if (type == XCB_SELECTION_REQUEST) {
        xcb_selection_request_event_t *req = (xcb_selection_request_event_t *)ev;
        xcb_atom_t prop = req->property;
        if (serve_chunk && serve_len > serve_chunk) {
            uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
            xcb_change_window_attributes(c, req->requestor, XCB_CW_EVENT_MASK, &mask);
            uint32_t sz = (uint32_t)serve_len;
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, req->requestor, prop, A.INCR, 32, 1, &sz);
            incr.active = true;
            incr.done = false;
            incr.requestor = req->requestor;
            incr.property = prop;
            incr.type = req->target;
            incr.offset = 0;
        } else {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, req->requestor, prop, req->target, 8, (uint32_t)serve_len,
                                serve_data);
        }
        xcb_selection_notify_event_t n;
        memset(&n, 0, sizeof(n));
        n.response_type = XCB_SELECTION_NOTIFY;
        n.time = req->time;
        n.requestor = req->requestor;
        n.selection = req->selection;
        n.target = req->target;
        n.property = prop;
        xcb_send_event(c, 0, req->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&n);
        xcb_flush(c);
    } else if (type == XCB_PROPERTY_NOTIFY && incr.active) {
        xcb_property_notify_event_t *pn = (xcb_property_notify_event_t *)ev;
        if (pn->window == incr.requestor && pn->atom == incr.property && pn->state == XCB_PROPERTY_DELETE) {
            if (incr.done) {
                incr.active = false;
                return;
            }
            size_t n = serve_len - incr.offset;
            if (n > serve_chunk) {
                n = serve_chunk;
            }
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, incr.requestor, incr.property, incr.type, 8, (uint32_t)n,
                                serve_data + incr.offset);
            incr.offset += n;
            incr.done = n == 0;
            xcb_flush(c);
        }
    }
}

/* pump SDL and serve the source until a client message of `type` reaches
 * the source window; returns it (malloc'd) or NULL */
static xcb_client_message_event_t *wait_message(xcb_atom_t type, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(c)) != NULL) {
            if ((ev->response_type & 0x7f) == XCB_CLIENT_MESSAGE &&
                ((xcb_client_message_event_t *)ev)->type == type) {
                return (xcb_client_message_event_t *)ev;
            }
            handle_source_event(ev);
            free(ev);
        }
        struct pollfd pfd = { xcb_get_file_descriptor(c), POLLIN, 0 };
        poll(&pfd, 1, 5);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* SDL side                                                            */
/* ------------------------------------------------------------------ */

typedef struct Drops
{
    int begin, position, file, text, complete;
    float x, y;
    char *files[4], *texts[4];
    size_t text_len[4];
} Drops;

static void collect(Drops *d, int ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)ms;
    while (SDL_GetTicks() < deadline) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_DROP_BEGIN:
                d->begin++;
                break;
            case SDL_EVENT_DROP_POSITION:
                d->position++;
                d->x = e.drop.x;
                d->y = e.drop.y;
                break;
            case SDL_EVENT_DROP_FILE:
                if (d->file < 4) {
                    d->files[d->file] = SDL_strdup(e.drop.data);
                }
                d->file++;
                break;
            case SDL_EVENT_DROP_TEXT:
                if (d->text < 4) {
                    d->texts[d->text] = SDL_strdup(e.drop.data);
                    d->text_len[d->text] = strlen(e.drop.data);
                }
                d->text++;
                break;
            case SDL_EVENT_DROP_COMPLETE:
                d->complete++;
                break;
            default:
                break;
            }
        }
        SDL_Delay(2);
    }
}

static void drops_free(Drops *d)
{
    for (int i = 0; i < 4; i++) {
        SDL_free(d->files[i]);
        SDL_free(d->texts[i]);
    }
    memset(d, 0, sizeof(*d));
}

static int16_t origin_x, origin_y;

/* a whole drag: enter, position (15,25 in the window), then drop or leave;
 * returns whether the target accepted (XdndStatus) */
static bool drag(const xcb_atom_t *types, int ntypes, bool drop, bool *finished_ok)
{
    uint32_t l1 = 5u << 24;
    uint32_t t[3] = { 0, 0, 0 };
    if (ntypes > 3) {
        l1 |= 1;
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, src, A.XdndTypeList, XCB_ATOM_ATOM, 32, (uint32_t)ntypes, types);
    } else {
        for (int i = 0; i < ntypes; i++) {
            t[i] = types[i];
        }
    }
    xcb_set_selection_owner(c, src, A.XdndSelection, XCB_CURRENT_TIME);
    client_message(A.XdndEnter, src, l1, t[0], t[1], t[2]);
    const uint32_t root_xy = ((uint32_t)(uint16_t)(origin_x + 15) << 16) | (uint16_t)(origin_y + 25);
    client_message(A.XdndPosition, src, 0, root_xy, XCB_CURRENT_TIME, A.XdndActionCopy);
    xcb_client_message_event_t *st = wait_message(A.XdndStatus, 2000);
    CHECK(st != NULL, "XdndStatus received");
    bool accepted = st && (st->data.data32[1] & 1);
    if (st) {
        CHECK(st->data.data32[0] == target, "XdndStatus names the target window");
        if (accepted) {
            CHECK(st->data.data32[4] == A.XdndActionCopy, "copy action accepted");
        }
    }
    free(st);
    if (!drop) {
        client_message(A.XdndLeave, src, 0, 0, 0, 0);
        return accepted;
    }
    client_message(A.XdndDrop, src, 0, XCB_CURRENT_TIME, 0, 0);
    xcb_client_message_event_t *fin = wait_message(A.XdndFinished, 3000);
    CHECK(fin != NULL, "XdndFinished received");
    *finished_ok = fin && (fin->data.data32[1] & 1) && fin->data.data32[2] == A.XdndActionCopy;
    free(fin);
    return accepted;
}

static bool aware_now(void)
{
    xcb_get_property_reply_t *r =
        xcb_get_property_reply(c, xcb_get_property(c, 0, target, A.XdndAware, XCB_ATOM_ATOM, 0, 1), NULL);
    bool ok = r && xcb_get_property_value_length(r) == 4 && *(uint32_t *)xcb_get_property_value(r) == 5;
    free(r);
    return ok;
}

/* the server doesn't order SDL's requests before ours: poll briefly */
static bool aware_is(bool expected)
{
    for (int i = 0; i < 50; i++) {
        if (aware_now() == expected) {
            return true;
        }
        SDL_Delay(4);
    }
    return false;
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_x11_dnd: SKIP (DISPLAY not set)\n");
        return 0;
    }
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDL_VIDEODRIVER", "x11", 1);
    int sn = 0;
    c = xcb_connect(NULL, &sn);
    if (xcb_connection_has_error(c)) {
        printf("test_x11_dnd: SKIP (cannot connect)\n");
        return 0;
    }
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < sn; i++) {
        xcb_screen_next(&si);
    }
    screen = si.data;
    src = xcb_generate_id(c);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, src, screen->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, 0, NULL);
    const char *names[] = { "XdndAware", "XdndEnter", "XdndPosition", "XdndStatus", "XdndLeave", "XdndDrop",
                            "XdndFinished", "XdndSelection", "XdndTypeList", "XdndActionCopy", "INCR",
                            "text/uri-list", "text/plain;charset=utf-8", "text/plain", "image/png",
                            "application/x-foo", "TEXT" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        (&A.XdndAware)[i] = atom(names[i]);
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("dnd", 200, 150, 0);
    CHECK(w != NULL, "window");
    SDL_SyncWindow(w);
    target = (xcb_window_t)SDL_GetNumberProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    CHECK(target != 0, "X11 window number");
    xcb_translate_coordinates_reply_t *tr =
        xcb_translate_coordinates_reply(c, xcb_translate_coordinates(c, target, screen->root, 0, 0), NULL);
    origin_x = tr ? tr->dst_x : 0;
    origin_y = tr ? tr->dst_y : 0;
    free(tr);
    CHECK(aware_is(true), "XdndAware = 5 on the window");
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    Drops d = { 0 };
    bool fin = false;

    /* files */
    serve_data = "# comment\r\nfile:///tmp/a%20b.txt\r\nfile://localhost/tmp/c.txt\r\n"
                 "file://elsewhere.invalid/x\r\nhttp://example.com/y\r\n";
    serve_len = strlen(serve_data);
    serve_chunk = 0;
    const xcb_atom_t t_files[] = { A.text_plain, A.uri_list, A.png };
    CHECK(drag(t_files, 3, true, &fin), "uri-list drag accepted");
    CHECK(fin, "XdndFinished reports success");
    collect(&d, 100);
    CHECK(d.begin == 1 && d.complete == 1, "files: BEGIN/COMPLETE (%d/%d)", d.begin, d.complete);
    CHECK(d.position == 1 && d.x == 15.0f && d.y == 25.0f, "files: position (%d) %g,%g", d.position, d.x, d.y);
    CHECK(d.file == 2 && d.text == 0, "files: %d files %d texts", d.file, d.text);
    CHECK(d.files[0] && strcmp(d.files[0], "/tmp/a b.txt") == 0, "file 1 '%s'", d.files[0] ? d.files[0] : "");
    CHECK(d.files[1] && strcmp(d.files[1], "/tmp/c.txt") == 0, "file 2 '%s'", d.files[1] ? d.files[1] : "");
    drops_free(&d);

    /* list: more than three types */
    serve_data = "line one\nline two \xc3\xa9\n";
    serve_len = strlen(serve_data);
    const xcb_atom_t t_list[] = { A.png, A.foo, A.TEXT, A.utf8_text };
    CHECK(drag(t_list, 4, true, &fin), "XdndTypeList drag accepted");
    CHECK(fin, "XdndFinished (list)");
    collect(&d, 100);
    CHECK(d.begin == 1 && d.complete == 1 && d.text == 2 && d.file == 0, "list: %d texts", d.text);
    CHECK(d.texts[1] && strcmp(d.texts[1], "line two \xc3\xa9") == 0, "list: text 2 '%s'", d.texts[1] ? d.texts[1] : "");
    drops_free(&d);

    /* incr */
    char *big = (char *)malloc(3 * 100001 + 1);
    for (int k = 0; k < 3; k++) {
        memset(big + k * 100001, 'a' + k, 100000);
        big[k * 100001 + 100000] = '\n';
    }
    big[3 * 100001] = '\0';
    serve_data = big;
    serve_len = strlen(big);
    serve_chunk = 8192;
    const xcb_atom_t t_text[] = { A.utf8_text };
    CHECK(drag(t_text, 1, true, &fin), "INCR drag accepted");
    CHECK(fin, "XdndFinished (incr)");
    collect(&d, 100);
    CHECK(d.text == 3 && d.text_len[0] == 100000 && d.text_len[2] == 100000 && d.texts[2] && d.texts[2][0] == 'c',
          "incr: %d texts (len %zu)", d.text, d.text_len[0]);
    CHECK(d.complete == 1, "incr: COMPLETE");
    drops_free(&d);
    free(big);
    serve_chunk = 0;

    /* refuse */
    const xcb_atom_t t_png[] = { A.png };
    CHECK(!drag(t_png, 1, true, &fin), "image/png refused");
    CHECK(!fin, "XdndFinished reports refusal");
    collect(&d, 100);
    CHECK(d.begin == 0 && d.position == 0 && d.complete == 0, "refuse: no events (begin %d pos %d)", d.begin,
          d.position);
    drops_free(&d);

    /* leave */
    CHECK(drag(t_files, 3, false, &fin), "leave: accepted while hovering");
    collect(&d, 150);
    CHECK(d.begin == 1 && d.position == 1 && d.complete == 1 && d.file == 0, "leave: BEGIN POSITION COMPLETE");
    drops_free(&d);

    /* XdndAware follows the drop events */
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, false);
    CHECK(aware_is(true), "still aware while DROP_TEXT is enabled");
    SDL_SetEventEnabled(SDL_EVENT_DROP_TEXT, false);
    CHECK(aware_is(false), "XdndAware removed with drop events disabled");
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);
    SDL_SetEventEnabled(SDL_EVENT_DROP_TEXT, true);
    CHECK(aware_is(true), "XdndAware back");

    SDL_DestroyWindow(w);
    SDL_Quit();
    xcb_disconnect(c);
    if (failures) {
        fprintf(stderr, "test_x11_dnd: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_x11_dnd: PASS\n");
    return 0;
}
