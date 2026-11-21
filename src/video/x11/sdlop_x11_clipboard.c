/*
  SDLop - X11 clipboard and primary selection (ICCCM selections over xcb).

  Same model as stock SDL3 (src/video/x11/SDL_x11clipboard.c): an unmapped
  InputOnly window owns CLIPBOARD / PRIMARY and answers SelectionRequest
  events from the application's offer (SDL_SetClipboardData callback, kept
  in SDL_clipboard.c); reading another client's selection converts it into
  a property of that window and waits for SelectionNotify, dispatching
  other events meanwhile.

  Beyond stock:
    - INCR in both directions (stock only reads INCR): data larger than the
      server's maximum request is served in chunks, as ICCCM 2.7.2
      requires. SDLOP_X11_SELECTION_CHUNK (bytes) lowers the chunk size,
      which is how INCR is tested.
    - XFixes is optional: with it, owner changes produce
      SDL_EVENT_CLIPBOARD_UPDATE with the new owner's TARGETS (as stock);
      without it, or when the TARGETS reply has not been pumped yet, the
      owner's TARGETS are fetched on demand when the clipboard is read.
    - a conversion timeout does not take over the selection (stock claims
      it with empty data so later reads don't wait again)

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"

#include <string.h>

static const char *const text_mime_types[] = {
    "UTF8_STRING",
    "text/plain;charset=utf-8",
    "text/plain",
    "TEXT",
    "STRING",
};

/* an INCR transfer we serve: one requestor property, chunk by chunk */
typedef struct IncrTransfer
{
    xcb_window_t requestor;
    xcb_atom_t property;
    xcb_atom_t type;
    Uint8 *data;
    size_t size, offset;
    Uint64 last_ns;
    bool done; /* zero-length terminator written */
    struct IncrTransfer *next;
} IncrTransfer;

static struct
{
    xcb_window_t window; /* selection owner / requestor window (lazy) */
    struct
    {
        xcb_atom_t CLIPBOARD, PRIMARY, TARGETS, INCR, SDL_SELECTION, SDL_FORMATS, MULTIPLE, TIMESTAMP, SDL_TIMESTAMP;
    } atom;
    bool atoms_ok;
    bool have_xfixes;
    uint8_t xfixes_event_base;

    /* PRIMARY text we own (the generic layer keeps none when the driver
     * implements SetPrimarySelectionText) */
    char *primary_text;
    xcb_timestamp_t primary_time, clipboard_time;

    /* owner whose TARGETS are the generic layer's mime types; 0 = ours or
     * unknown */
    xcb_window_t targets_owner;
    bool targets_valid;
    bool targets_pending; /* asynchronous TARGETS request in flight */
    Uint64 targets_fetched_ns;

    /* synchronous conversion in progress */
    bool waiting;
    xcb_atom_t wait_target;
    bool notify_ok;
    bool incr_waiting;

    IncrTransfer *transfers;
} cb;

static xcb_connection_t *conn(void)
{
    return SDLOP_X11_GetConnection();
}

static xcb_atom_t intern(const char *name)
{
    xcb_intern_atom_reply_t *r =
        X11_xcb_intern_atom_reply(conn(), X11_xcb_intern_atom(conn(), 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static char *atom_name(xcb_atom_t atom)
{
    xcb_get_atom_name_reply_t *r = X11_xcb_get_atom_name_reply(conn(), X11_xcb_get_atom_name(conn(), atom), NULL);
    if (!r) {
        return NULL;
    }
    int len = X11_xcb_get_atom_name_name_length(r);
    char *s = (char *)SDL_malloc((size_t)len + 1);
    if (s) {
        memcpy(s, X11_xcb_get_atom_name_name(r), (size_t)len);
        s[len] = '\0';
    }
    free(r);
    return s;
}

static bool ensure_atoms(void)
{
    if (cb.atoms_ok) {
        return true;
    }
    static const char *const names[] = { "CLIPBOARD", "PRIMARY", "TARGETS", "INCR",
                                         "SDL_SELECTION", "SDL_FORMATS", "MULTIPLE", "TIMESTAMP",
                                         "SDL_TIMESTAMP" };
    xcb_atom_t *dst = &cb.atom.CLIPBOARD;
    xcb_intern_atom_cookie_t cookies[SDL_arraysize(names)];
    for (size_t i = 0; i < SDL_arraysize(names); i++) {
        cookies[i] = X11_xcb_intern_atom(conn(), 0, (uint16_t)strlen(names[i]), names[i]);
    }
    bool ok = true;
    for (size_t i = 0; i < SDL_arraysize(names); i++) {
        xcb_intern_atom_reply_t *r = X11_xcb_intern_atom_reply(conn(), cookies[i], NULL);
        dst[i] = r ? r->atom : XCB_ATOM_NONE;
        ok = ok && dst[i] != XCB_ATOM_NONE;
        free(r);
    }
    cb.atoms_ok = ok;
    return ok;
}

/* the selection window: created on first use (or at init with XFixes) */
static xcb_window_t get_window(void)
{
    if (cb.window || !conn() || !ensure_atoms()) {
        return cb.window;
    }
    cb.window = X11_xcb_generate_id(conn());
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    X11_xcb_create_window_checked(conn(), XCB_COPY_FROM_PARENT, cb.window, SDLOP_X11_GetRoot(), -10, -10, 1, 1, 0,
                                  XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &mask);
    if (cb.have_xfixes) {
        const uint32_t ev = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                            XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                            XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
        X11_xcb_xfixes_select_selection_input(conn(), cb.window, cb.atom.CLIPBOARD, ev);
        X11_xcb_xfixes_select_selection_input(conn(), cb.window, cb.atom.PRIMARY, ev);
    }
    X11_xcb_flush(conn());
    return cb.window;
}

static xcb_window_t selection_owner(xcb_atom_t selection)
{
    xcb_get_selection_owner_reply_t *r =
        X11_xcb_get_selection_owner_reply(conn(), X11_xcb_get_selection_owner(conn(), selection), NULL);
    xcb_window_t owner = r ? r->owner : XCB_WINDOW_NONE;
    free(r);
    return owner;
}

/* a server timestamp: a zero-length append to our own window's property
 * produces a PropertyNotify carrying the server time (ICCCM 2.1 says not
 * to use CurrentTime for SetSelectionOwner) */
static bool got_timestamp;
static xcb_timestamp_t timestamp_value;

static bool timestamp_arrived(void *unused)
{
    (void)unused;
    return got_timestamp;
}

static xcb_timestamp_t server_time(void)
{
    got_timestamp = false;
    X11_xcb_change_property(conn(), XCB_PROP_MODE_APPEND, cb.window, cb.atom.SDL_TIMESTAMP, XCB_ATOM_INTEGER, 32,
                            0, NULL);
    SDLOP_X11_PumpUntil(timestamp_arrived, NULL, 200);
    return got_timestamp ? timestamp_value : XCB_CURRENT_TIME;
}

static bool set_owner(xcb_atom_t selection)
{
    if (!get_window()) {
        return SDL_SetError("Couldn't create the X11 selection window");
    }
    xcb_timestamp_t t = server_time();
    X11_xcb_set_selection_owner(conn(), cb.window, selection, t);
    if (selection == cb.atom.PRIMARY) {
        cb.primary_time = t;
    } else {
        cb.clipboard_time = t;
    }
    X11_xcb_flush(conn());
    if (selection_owner(selection) != cb.window) {
        return SDL_SetError("Couldn't take the X11 %s selection", selection == cb.atom.PRIMARY ? "PRIMARY" : "CLIPBOARD");
    }
    return true;
}

static bool notify_arrived(void *unused)
{
    (void)unused;
    return !cb.waiting;
}

static bool incr_arrived(void *unused)
{
    (void)unused;
    return !cb.incr_waiting;
}

/* property `prop` of our window, deleted after reading; type returned */
static void *read_property(xcb_atom_t prop, xcb_atom_t *type, size_t *len)
{
    xcb_get_property_reply_t *r = X11_xcb_get_property_reply(
        conn(), X11_xcb_get_property(conn(), 1, cb.window, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x1fffffff), NULL);
    *len = 0;
    *type = XCB_ATOM_NONE;
    if (!r) {
        return NULL;
    }
    *type = r->type;
    int n = X11_xcb_get_property_value_length(r);
    void *data = SDL_malloc((size_t)(n > 0 ? n : 0) + sizeof(Uint32));
    if (data) {
        if (n > 0) {
            memcpy(data, X11_xcb_get_property_value(r), (size_t)n);
        }
        memset((Uint8 *)data + (n > 0 ? n : 0), 0, sizeof(Uint32));
        *len = (size_t)(n > 0 ? n : 0);
    }
    free(r);
    return data;
}

/* ask the owner of `selection` to convert it to `target` into our window's
 * SDL_SELECTION property; INCR transfers are reassembled */
static void *convert(xcb_atom_t selection, xcb_atom_t target, size_t *length, xcb_atom_t *out_type)
{
    *length = 0;
    if (!get_window()) {
        return NULL;
    }
    X11_xcb_delete_property(conn(), cb.window, cb.atom.SDL_SELECTION);
    X11_xcb_convert_selection(conn(), cb.window, selection, target, cb.atom.SDL_SELECTION, XCB_CURRENT_TIME);
    cb.waiting = true;
    cb.wait_target = target;
    cb.notify_ok = false;
    SDLOP_X11_PumpUntil(notify_arrived, NULL, 1000);
    if (cb.waiting) {
        cb.waiting = false;
        SDL_SetError("Selection timeout");
        return NULL;
    }
    if (!cb.notify_ok) {
        return NULL; /* owner refused this target */
    }
    xcb_atom_t type;
    size_t len;
    void *data = read_property(cb.atom.SDL_SELECTION, &type, &len);
    if (!data || type != cb.atom.INCR) {
        if (out_type) {
            *out_type = type;
        }
        *length = len;
        return data;
    }
    /* INCR: deleting the property (read_property did) asks for the first
     * chunk; each chunk arrives as PropertyNotify(NewValue), a zero-length
     * one ends the transfer */
    SDL_free(data);
    Uint8 *buf = NULL;
    size_t total = 0;
    for (;;) {
        cb.incr_waiting = true;
        SDLOP_X11_PumpUntil(incr_arrived, NULL, 1000);
        if (cb.incr_waiting) {
            cb.incr_waiting = false;
            SDL_free(buf);
            SDL_SetError("Selection timeout (INCR)");
            return NULL;
        }
        void *chunk = read_property(cb.atom.SDL_SELECTION, &type, &len);
        X11_xcb_flush(conn());
        if (!chunk) {
            SDL_free(buf);
            return NULL;
        }
        if (len == 0) {
            SDL_free(chunk);
            break;
        }
        Uint8 *grown = (Uint8 *)SDL_realloc(buf, total + len + sizeof(Uint32));
        if (!grown) {
            SDL_free(chunk);
            SDL_free(buf);
            return NULL;
        }
        buf = grown;
        memcpy(buf + total, chunk, len);
        total += len;
        memset(buf + total, 0, sizeof(Uint32));
        SDL_free(chunk);
        if (out_type) {
            *out_type = type;
        }
    }
    if (!buf) {
        buf = (Uint8 *)SDL_calloc(1, sizeof(Uint32));
    }
    *length = total;
    return buf;
}

/* TARGETS of the current CLIPBOARD owner -> SDL_EVENT_CLIPBOARD_UPDATE and
 * the generic layer's mime types */
static void publish_targets(const xcb_atom_t *atoms, size_t count)
{
    size_t alloc = sizeof(char *);
    char **names = (char **)SDL_calloc(count ? count : 1, sizeof(char *));
    size_t n = 0;
    for (size_t i = 0; names && i < count; i++) {
        if (atoms[i] == cb.atom.TARGETS || atoms[i] == cb.atom.MULTIPLE || atoms[i] == cb.atom.TIMESTAMP) {
            continue;
        }
        names[n] = atom_name(atoms[i]);
        if (names[n]) {
            alloc += sizeof(char *) + strlen(names[n]) + 1;
            n++;
        }
    }
    char **types = (char **)SDLOP_AllocateTemporaryMemory(alloc);
    if (types) {
        char *p = (char *)(types + n + 1);
        for (size_t i = 0; i < n; i++) {
            size_t len = strlen(names[i]) + 1;
            types[i] = p;
            memcpy(p, names[i], len);
            p += len;
        }
        types[n] = NULL;
        SDLOP_SendClipboardUpdate(false, types, n);
    }
    for (size_t i = 0; names && i < n; i++) {
        SDL_free(names[i]);
    }
    SDL_free(names);
}

/* make the generic layer's mime types those of the current external owner */
static bool targets_arrived(void *unused)
{
    (void)unused;
    return !cb.targets_pending;
}

static void refresh_targets(xcb_window_t owner)
{
    if (cb.targets_pending && cb.targets_owner == owner) {
        /* the XFixes-triggered request is on its way: don't ask twice */
        SDLOP_X11_PumpUntil(targets_arrived, NULL, 1000);
        cb.targets_pending = false;
    }
    /* without XFixes nothing reports the owner re-taking the selection with
     * other content, so a fetched list is only trusted briefly (enough for
     * the Has/Get calls of one SDL_GetClipboardText) */
    if (cb.targets_valid && cb.targets_owner == owner &&
        (cb.have_xfixes || SDLOP_MonotonicNS() - cb.targets_fetched_ns < 100000000ull)) {
        return;
    }
    size_t len = 0;
    xcb_atom_t type = XCB_ATOM_NONE;
    void *data = convert(cb.atom.CLIPBOARD, cb.atom.TARGETS, &len, &type);
    if (data && type == XCB_ATOM_ATOM) {
        publish_targets((const xcb_atom_t *)data, len / sizeof(xcb_atom_t));
    } else {
        publish_targets(NULL, 0);
    }
    SDL_free(data);
    cb.targets_owner = owner;
    cb.targets_valid = true;
    cb.targets_fetched_ns = SDLOP_MonotonicNS();
}

/* ------------------------------------------------------------------ */
/* serving requests                                                    */
/* ------------------------------------------------------------------ */

static size_t chunk_size(void)
{
    const char *hint = SDL_GetHint("SDLOP_X11_SELECTION_CHUNK");
    size_t max = (size_t)X11_xcb_get_maximum_request_length(conn()) * 4;
    max = max > 4096 ? max - 1024 : 3072; /* room for the request header */
    if (hint && *hint) {
        long v = SDL_strtol(hint, NULL, 0);
        if (v > 0 && (size_t)v < max) {
            return (size_t)v;
        }
    }
    return max;
}

static void free_transfer(IncrTransfer *t)
{
    IncrTransfer **pp = &cb.transfers;
    while (*pp && *pp != t) {
        pp = &(*pp)->next;
    }
    if (*pp) {
        *pp = t->next;
    }
    /* stop listening to the requestor unless another transfer needs it */
    bool still = false;
    for (IncrTransfer *o = cb.transfers; o; o = o->next) {
        still = still || o->requestor == t->requestor;
    }
    if (!still) {
        uint32_t none = 0;
        X11_xcb_change_window_attributes(conn(), t->requestor, XCB_CW_EVENT_MASK, &none);
    }
    SDL_free(t->data);
    SDL_free(t);
}

static void expire_transfers(void)
{
    const Uint64 now = SDLOP_MonotonicNS();
    for (IncrTransfer *t = cb.transfers, *next; t; t = next) {
        next = t->next;
        if (now - t->last_ns > 10ull * 1000000000ull) {
            free_transfer(t); /* requestor went away mid-transfer */
        }
    }
}

static void send_notify(const xcb_selection_request_event_t *req, xcb_atom_t property)
{
    xcb_selection_notify_event_t n;
    memset(&n, 0, sizeof(n));
    n.response_type = XCB_SELECTION_NOTIFY;
    n.time = req->time;
    n.requestor = req->requestor;
    n.selection = req->selection;
    n.target = req->target;
    n.property = property;
    X11_xcb_send_event(conn(), 0, req->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&n);
}

/* data for `target` from our offer on `selection`; malloc'd */
static void *offered_data(xcb_atom_t selection, xcb_atom_t target, size_t *size)
{
    *size = 0;
    char *name = atom_name(target);
    if (!name) {
        return NULL;
    }
    void *data = NULL;
    if (selection == cb.atom.PRIMARY) {
        for (size_t i = 0; i < SDL_arraysize(text_mime_types); i++) {
            if (strcmp(name, text_mime_types[i]) == 0 && cb.primary_text) {
                *size = strlen(cb.primary_text);
                data = SDL_strdup(cb.primary_text);
                break;
            }
        }
    } else {
        data = SDLOP_GetInternalClipboardData(name, size);
    }
    SDL_free(name);
    return data;
}

static void handle_request(const xcb_selection_request_event_t *req)
{
    const bool primary = req->selection == cb.atom.PRIMARY;
    xcb_atom_t property = req->property ? req->property : req->target; /* obsolete clients: None */

    if ((primary && !cb.primary_text) || (!primary && req->selection != cb.atom.CLIPBOARD)) {
        send_notify(req, XCB_ATOM_NONE);
        return;
    }
    if (req->target == cb.atom.TARGETS) {
        size_t count = 0;
        const char *const *types;
        if (primary) {
            types = text_mime_types;
            count = SDL_arraysize(text_mime_types);
        } else {
            types = SDLOP_GetClipboardOfferMimeTypes(&count);
        }
        xcb_atom_t *atoms = (xcb_atom_t *)SDL_malloc((count + 2) * sizeof(xcb_atom_t));
        if (!atoms) {
            send_notify(req, XCB_ATOM_NONE);
            return;
        }
        size_t n = 0;
        atoms[n++] = cb.atom.TARGETS;
        atoms[n++] = cb.atom.TIMESTAMP;
        for (size_t i = 0; i < count; i++) {
            xcb_atom_t a = intern(types[i]);
            if (a) {
                atoms[n++] = a;
            }
        }
        X11_xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, req->requestor, property, XCB_ATOM_ATOM, 32,
                                (uint32_t)n, atoms);
        SDL_free(atoms);
        send_notify(req, property);
        return;
    }
    if (req->target == cb.atom.TIMESTAMP) {
        xcb_timestamp_t t = primary ? cb.primary_time : cb.clipboard_time;
        X11_xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, req->requestor, property, XCB_ATOM_INTEGER, 32, 1,
                                &t);
        send_notify(req, property);
        return;
    }
    size_t size = 0;
    void *data = offered_data(req->selection, req->target, &size);
    if (!data) {
        send_notify(req, XCB_ATOM_NONE);
        return;
    }
    const size_t chunk = chunk_size();
    if (size <= chunk) {
        X11_xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, req->requestor, property, req->target, 8,
                                (uint32_t)size, data);
        SDL_free(data);
        send_notify(req, property);
        return;
    }
    /* INCR (ICCCM 2.7.2): announce the size, then write a chunk whenever
     * the requestor deletes the property */
    IncrTransfer *t = (IncrTransfer *)SDL_calloc(1, sizeof(*t));
    if (!t) {
        SDL_free(data);
        send_notify(req, XCB_ATOM_NONE);
        return;
    }
    t->requestor = req->requestor;
    t->property = property;
    t->type = req->target;
    t->data = (Uint8 *)data;
    t->size = size;
    t->last_ns = SDLOP_MonotonicNS();
    t->next = cb.transfers;
    cb.transfers = t;
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    X11_xcb_change_window_attributes(conn(), req->requestor, XCB_CW_EVENT_MASK, &mask);
    uint32_t announced = size > 0xffffffffu ? 0xffffffffu : (uint32_t)size;
    X11_xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, req->requestor, property, cb.atom.INCR, 32, 1,
                            &announced);
    send_notify(req, property);
}

static void continue_transfer(IncrTransfer *t)
{
    t->last_ns = SDLOP_MonotonicNS();
    if (t->done) {
        free_transfer(t); /* the terminator was read */
        return;
    }
    size_t n = t->size - t->offset;
    const size_t chunk = chunk_size();
    if (n > chunk) {
        n = chunk;
    }
    X11_xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, t->requestor, t->property, t->type, 8, (uint32_t)n,
                            t->data + t->offset);
    t->offset += n;
    if (n == 0) {
        t->done = true;
    }
}

/* ------------------------------------------------------------------ */
/* driver entry points                                                 */
/* ------------------------------------------------------------------ */

bool SDLOP_X11_Clipboard_HandleEvent(xcb_generic_event_t *ev)
{
    if (!cb.window) {
        return false;
    }
    const uint8_t type = ev->response_type & 0x7f;
    if (cb.have_xfixes && type == cb.xfixes_event_base + XCB_XFIXES_SELECTION_NOTIFY) {
        xcb_xfixes_selection_notify_event_t *xf = (xcb_xfixes_selection_notify_event_t *)ev;
        if (xf->selection == cb.atom.CLIPBOARD && xf->owner != cb.window) {
            /* new external owner (or none): ask for its TARGETS
             * asynchronously; the reply publishes them */
            cb.targets_valid = false;
            if (xf->owner == XCB_WINDOW_NONE) {
                publish_targets(NULL, 0);
                cb.targets_owner = XCB_WINDOW_NONE;
                cb.targets_valid = true;
            } else {
                X11_xcb_convert_selection(conn(), cb.window, cb.atom.CLIPBOARD, cb.atom.TARGETS,
                                          cb.atom.SDL_FORMATS, xf->timestamp);
                cb.targets_owner = xf->owner;
                cb.targets_pending = true;
            }
        }
        return true;
    }
    switch (type) {
    case XCB_SELECTION_REQUEST: {
        xcb_selection_request_event_t *req = (xcb_selection_request_event_t *)ev;
        if (req->owner != cb.window) {
            return false;
        }
        expire_transfers();
        handle_request(req);
        X11_xcb_flush(conn());
        return true;
    }
    case XCB_SELECTION_NOTIFY: {
        xcb_selection_notify_event_t *sn = (xcb_selection_notify_event_t *)ev;
        if (sn->requestor != cb.window) {
            return false;
        }
        const bool sync_reply = cb.waiting && sn->target == cb.wait_target && sn->property != cb.atom.SDL_FORMATS;
        if (!sync_reply && sn->target == cb.atom.TARGETS) {
            /* asynchronous TARGETS (XFixes owner change); stale when we
             * have taken the clipboard since */
            size_t len = 0;
            xcb_atom_t t = XCB_ATOM_NONE;
            void *data = sn->property ? read_property(cb.atom.SDL_FORMATS, &t, &len) : NULL;
            cb.targets_pending = false;
            if (selection_owner(cb.atom.CLIPBOARD) == cb.window) {
                SDL_free(data);
                return true;
            }
            if (data && t == XCB_ATOM_ATOM) {
                publish_targets((const xcb_atom_t *)data, len / sizeof(xcb_atom_t));
            } else {
                publish_targets(NULL, 0);
            }
            SDL_free(data);
            cb.targets_valid = true;
            return true;
        }
        if (sync_reply) {
            cb.waiting = false;
            cb.notify_ok = sn->property != XCB_ATOM_NONE;
        }
        return true;
    }
    case XCB_SELECTION_CLEAR: {
        xcb_selection_clear_event_t *sc = (xcb_selection_clear_event_t *)ev;
        if (sc->owner != cb.window) {
            return false;
        }
        if (sc->selection == cb.atom.PRIMARY) {
            SDL_free(cb.primary_text);
            cb.primary_text = NULL;
        } else if (sc->selection == cb.atom.CLIPBOARD && selection_owner(cb.atom.CLIPBOARD) != cb.window) {
            /* another client took it: our offer is void (stock cancels it);
             * the owner check skips a stale clear queued before we took the
             * selection back */
            SDLOP_CancelClipboardData();
            if (cb.targets_owner == cb.window) {
                cb.targets_valid = false;
            }
        }
        return true;
    }
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *pn = (xcb_property_notify_event_t *)ev;
        if (pn->window == cb.window) {
            if (pn->atom == cb.atom.SDL_SELECTION && pn->state == XCB_PROPERTY_NEW_VALUE) {
                cb.incr_waiting = false;
            } else if (pn->atom == cb.atom.SDL_TIMESTAMP && !got_timestamp) {
                got_timestamp = true;
                timestamp_value = pn->time;
            }
            return true;
        }
        if (pn->state == XCB_PROPERTY_DELETE) {
            for (IncrTransfer *t = cb.transfers; t; t = t->next) {
                if (t->requestor == pn->window && t->property == pn->atom) {
                    continue_transfer(t);
                    X11_xcb_flush(conn());
                    return true;
                }
            }
        }
        return false;
    }
    default:
        return false;
    }
}

void SDLOP_X11_Clipboard_Init(void)
{
    memset(&cb, 0, sizeof(cb));
    if (!conn()) {
        return;
    }
    if (SDLOP_X11_HaveModule(SDLOP_X11_MOD_XFIXES)) {
        const xcb_query_extension_reply_t *ext = X11_xcb_get_extension_data(conn(), X11_xcb_xfixes_id);
        if (ext && ext->present) {
            xcb_xfixes_query_version_reply_t *v =
                X11_xcb_xfixes_query_version_reply(conn(), X11_xcb_xfixes_query_version(conn(), 5, 0), NULL);
            if (v && v->major_version >= 1) {
                cb.have_xfixes = true;
                cb.xfixes_event_base = ext->first_event;
            }
            free(v);
        }
    }
    if (cb.have_xfixes && !SDLOP_HintFlag("SDLOP_X11_LAZY_CLIPBOARD", false)) {
        get_window(); /* owner-change events need the window from the start */
    }
}

void SDLOP_X11_Clipboard_Quit(void)
{
    while (cb.transfers) {
        free_transfer(cb.transfers);
    }
    if (cb.window && conn()) {
        X11_xcb_destroy_window(conn(), cb.window);
        X11_xcb_flush(conn());
    }
    SDL_free(cb.primary_text);
    memset(&cb, 0, sizeof(cb));
}

const char *const *SDLOP_X11_GetTextMimeTypes(SDLop_VideoDevice *device, size_t *num_mime_types)
{
    (void)device;
    *num_mime_types = SDL_arraysize(text_mime_types);
    return text_mime_types;
}

bool SDLOP_X11_SetClipboardData(SDLop_VideoDevice *device)
{
    (void)device;
    if (!get_window()) {
        return SDL_SetError("Couldn't create the X11 selection window");
    }
    size_t count = 0;
    SDLOP_GetClipboardOfferMimeTypes(&count);
    if (count == 0) {
        /* SDL_ClearClipboardData: give the selection up if it is ours */
        if (selection_owner(cb.atom.CLIPBOARD) == cb.window) {
            X11_xcb_set_selection_owner(conn(), XCB_WINDOW_NONE, cb.atom.CLIPBOARD, cb.clipboard_time);
            /* round trip: released once SDL_ClearClipboardData returns (the
             * server doesn't order requests across connections) */
            if (selection_owner(cb.atom.CLIPBOARD) == cb.window) {
                return SDL_SetError("Couldn't release the X11 CLIPBOARD selection");
            }
        }
        cb.targets_owner = XCB_WINDOW_NONE;
        cb.targets_valid = true;
        return true;
    }
    cb.targets_owner = cb.window;
    cb.targets_valid = true;
    return set_owner(cb.atom.CLIPBOARD);
}

void *SDLOP_X11_GetClipboardData(SDLop_VideoDevice *device, const char *mime_type, size_t *size)
{
    (void)device;
    *size = 0;
    if (!get_window()) {
        return NULL;
    }
    xcb_window_t owner = selection_owner(cb.atom.CLIPBOARD);
    if (owner == XCB_WINDOW_NONE) {
        return NULL;
    }
    if (owner == cb.window) {
        return SDLOP_GetInternalClipboardData(mime_type, size);
    }
    refresh_targets(owner);
    if (!SDLOP_HasInternalClipboardData(mime_type)) {
        return NULL; /* the owner doesn't offer it (stock checks this too) */
    }
    xcb_atom_t target = intern(mime_type);
    return target ? convert(cb.atom.CLIPBOARD, target, size, NULL) : NULL;
}

bool SDLOP_X11_HasClipboardData(SDLop_VideoDevice *device, const char *mime_type)
{
    (void)device;
    if (!get_window()) {
        return false;
    }
    xcb_window_t owner = selection_owner(cb.atom.CLIPBOARD);
    if (owner == XCB_WINDOW_NONE) {
        return false;
    }
    if (owner != cb.window) {
        refresh_targets(owner);
    }
    return SDLOP_HasInternalClipboardData(mime_type);
}

bool SDLOP_X11_SetPrimarySelectionText(SDLop_VideoDevice *device, const char *text)
{
    (void)device;
    if (!get_window()) {
        return SDL_SetError("Couldn't create the X11 selection window");
    }
    char *copy = SDL_strdup(text ? text : "");
    if (!copy) {
        return false;
    }
    SDL_free(cb.primary_text);
    cb.primary_text = copy;
    return set_owner(cb.atom.PRIMARY);
}

char *SDLOP_X11_GetPrimarySelectionText(SDLop_VideoDevice *device)
{
    (void)device;
    if (get_window()) {
        xcb_window_t owner = selection_owner(cb.atom.PRIMARY);
        if (owner == cb.window && cb.primary_text) {
            return SDL_strdup(cb.primary_text);
        }
        if (owner != XCB_WINDOW_NONE && owner != cb.window) {
            static const char *const tries[] = { "UTF8_STRING", "text/plain;charset=utf-8", "STRING" };
            for (size_t i = 0; i < SDL_arraysize(tries); i++) {
                size_t len = 0;
                void *data = convert(cb.atom.PRIMARY, intern(tries[i]), &len, NULL);
                if (data) {
                    return (char *)data;
                }
            }
        }
    }
    return SDL_strdup("");
}

bool SDLOP_X11_HasPrimarySelectionText(SDLop_VideoDevice *device)
{
    char *text = SDLOP_X11_GetPrimarySelectionText(device);
    bool result = text && text[0] != '\0';
    SDL_free(text);
    return result;
}
