/*
  SDLop - Wayland clipboard (wl_data_device), primary selection
  (zwp_primary_selection_v1) and drag-and-drop target.

  Same model as stock SDL3 (SDL_waylandclipboard.c / SDL_waylanddatamanager.c):
    - SDL_SetClipboardData creates a wl_data_source offering the app's mime
      types and makes it the selection; wl_data_source.send is answered from
      the app's callback through SDLOP_GetInternalClipboardData.
    - wl_data_device.selection introduces another client's offer: its mime
      types become the generic layer's (SDL_EVENT_CLIPBOARD_UPDATE, owner =
      false) and reads go through wl_data_offer.receive + a pipe.
    - while one of our own sources is the selection, the offer the
      compositor echoes back is ours: reads short-circuit to the app's data
      (reading our own pipe would deadlock) and no foreign update is sent.
      Stock detects its echo with an extra private mime type and a pipe
      round trip; a live (not yet cancelled) source says the same here.
  Primary selection is the same with zwp_primary_selection_*; it carries
  text only, as in stock.

  Drag-and-drop (as stock): an offer entering one of our surfaces is
  accepted with text/uri-list, else a text type (copy action);
  enter/motion become SDL_EVENT_DROP_POSITION, drop reads the data into
  SDL_EVENT_DROP_FILE (local file: URIs) / SDL_EVENT_DROP_TEXT (lines) +
  SDL_EVENT_DROP_COMPLETE and finishes the offer; leave completes an
  unfinished sequence.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "sdlop_wayland_internal.h"
#include "sdlop_wayland_dyn.h"
#include "primary-selection-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static const char *const text_mime_types[] = {
    "text/plain;charset=utf-8",
    "text/plain",
    "TEXT",
    "UTF8_STRING",
    "STRING",
};

typedef struct Offer
{
    void *proxy; /* wl_data_offer or zwp_primary_selection_offer_v1 */
    bool primary;
    char **mimes;
    size_t num_mimes, cap_mimes;
} Offer;

static struct
{
    struct wl_data_device_manager *manager;
    uint32_t manager_version;
    struct zwp_primary_selection_device_manager_v1 *primary_manager;
    struct wl_seat *seat;

    struct wl_data_device *device;
    struct zwp_primary_selection_device_v1 *primary_device;

    Offer *selection;     /* current CLIPBOARD offer (may be our echo) */
    Offer *dnd;           /* offer of a drag over one of our surfaces */
    SDL_Window *dnd_window;
    const char *dnd_mime; /* accepted type (points into dnd->mimes), NULL = refused */
    uint32_t dnd_action;  /* last wl_data_offer.action for dnd */
    Offer *primary_offer; /* current PRIMARY offer */

    struct wl_data_source *source;                     /* our CLIPBOARD source, until cancelled */
    struct zwp_primary_selection_source_v1 *primary_source; /* our PRIMARY source */
    char *primary_text;                                /* what primary_source serves */
} wc;

/* ------------------------------------------------------------------ */
/* offers                                                              */
/* ------------------------------------------------------------------ */

static Offer *offer_new(void *proxy, bool primary)
{
    Offer *o = (Offer *)SDL_calloc(1, sizeof(*o));
    if (o) {
        o->proxy = proxy;
        o->primary = primary;
    }
    return o;
}

static void offer_add_mime(Offer *o, const char *mime)
{
    if (!o || !mime) {
        return;
    }
    for (size_t i = 0; i < o->num_mimes; i++) {
        if (strcmp(o->mimes[i], mime) == 0) {
            return;
        }
    }
    if (o->num_mimes == o->cap_mimes) {
        size_t cap = o->cap_mimes ? o->cap_mimes * 2 : 8;
        char **grown = (char **)SDL_realloc(o->mimes, cap * sizeof(char *));
        if (!grown) {
            return;
        }
        o->mimes = grown;
        o->cap_mimes = cap;
    }
    char *copy = SDL_strdup(mime);
    if (copy) {
        o->mimes[o->num_mimes++] = copy;
    }
}

static bool offer_has_mime(const Offer *o, const char *mime)
{
    for (size_t i = 0; o && i < o->num_mimes; i++) {
        if (strcmp(o->mimes[i], mime) == 0) {
            return true;
        }
    }
    return false;
}

static void offer_destroy(Offer *o)
{
    if (!o) {
        return;
    }
    if (o->proxy) {
        if (o->primary) {
            zwp_primary_selection_offer_v1_destroy((struct zwp_primary_selection_offer_v1 *)o->proxy);
        } else {
            wl_data_offer_destroy((struct wl_data_offer *)o->proxy);
        }
    }
    for (size_t i = 0; i < o->num_mimes; i++) {
        SDL_free(o->mimes[i]);
    }
    SDL_free(o->mimes);
    SDL_free(o);
}

/* read everything the other end writes into the pipe; NUL-padded */
static void *read_pipe(int fd, size_t *length)
{
    const size_t max_bytes = 16u * 1024u * 1024u; /* overall size cap */
    size_t cap = 4096, len = 0;
    Uint8 *buf = (Uint8 *)SDL_malloc(cap + sizeof(Uint32));
    const Uint64 start = SDLOP_MonotonicNS();
    const Uint64 deadline = start + 1000000000ull;   /* 1 s without data aborts */
    const Uint64 overall = start + 30000000000ull;   /* 30 s for the whole transfer */
    Uint64 last = start;
    while (buf) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        Uint64 now = SDLOP_MonotonicNS();
        if (now - last > 1000000000ull || now > overall || (len == 0 && now > deadline)) {
            SDL_SetError("Clipboard read timeout");
            SDL_free(buf);
            buf = NULL;
            break;
        }
        if (len >= max_bytes) {
            SDL_SetError("Clipboard data too large");
            SDL_free(buf);
            buf = NULL;
            break;
        }
        int r = poll(&pfd, 1, 50);
        if (r < 0 && errno != EINTR) {
            SDL_free(buf);
            buf = NULL;
            break;
        }
        if (r <= 0) {
            continue;
        }
        if (cap - len < 4096 && cap < max_bytes) {
            const size_t newcap = cap > max_bytes / 2 ? max_bytes : cap * 2;
            Uint8 *grown = (Uint8 *)SDL_realloc(buf, newcap + sizeof(Uint32));
            if (!grown) {
                SDL_free(buf);
                buf = NULL;
                break;
            }
            buf = grown;
            cap = newcap;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            SDL_free(buf);
            buf = NULL;
            break;
        }
        if (n == 0) {
            break; /* writer closed: done */
        }
        len += (size_t)n;
        last = SDLOP_MonotonicNS();
    }
    if (buf) {
        memset(buf + len, 0, sizeof(Uint32));
    }
    *length = buf ? len : 0;
    return buf;
}

static void *offer_receive(Offer *o, const char *mime, size_t *length)
{
    *length = 0;
    if (!o || !offer_has_mime(o, mime)) {
        return NULL;
    }
    int fds[2];
    /* the write end goes to the source client, and FD passing keeps its
     * file status flags: it must stay blocking (a source with a plain write
     * loop would hit EAGAIN and truncate). The ends are separate open file
     * descriptions, so only our read end becomes nonblocking. */
    if (pipe2(fds, O_CLOEXEC) < 0) {
        SDL_SetError("Clipboard pipe: %s", strerror(errno));
        return NULL;
    }
    const int rflags = fcntl(fds[0], F_GETFL);
    if (rflags < 0 || fcntl(fds[0], F_SETFL, rflags | O_NONBLOCK) < 0) {
        SDL_SetError("Clipboard pipe: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (o->primary) {
        zwp_primary_selection_offer_v1_receive((struct zwp_primary_selection_offer_v1 *)o->proxy, mime, fds[1]);
    } else {
        wl_data_offer_receive((struct wl_data_offer *)o->proxy, mime, fds[1]);
    }
    close(fds[1]);
    wl_display_flush(SDLOP_Wayland_GetDisplay());
    void *data = read_pipe(fds[0], length);
    close(fds[0]);
    return data;
}

/* ------------------------------------------------------------------ */
/* sources                                                             */
/* ------------------------------------------------------------------ */

static void write_pipe(int fd, const void *data, size_t size)
{
    /* The reader may go away mid-transfer. Block SIGPIPE in this thread
     * only, as stock does: changing the process-wide disposition would also
     * drop the application's own SIGPIPE handling in every other thread for
     * the length of the transfer. A SIGPIPE raised by our write stays pending
     * and is consumed below, unless one was already pending before. */
    sigset_t pipe_set, old_set, pending;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);
    sigpending(&pending);
    const bool was_pending = sigismember(&pending, SIGPIPE) == 1;
    bool got_epipe = false;
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    size_t off = 0;
    Uint64 last = SDLOP_MonotonicNS();
    while (off < size) {
        ssize_t n = write(fd, (const Uint8 *)data + off, size - off);
        if (n > 0) {
            off += (size_t)n;
            last = SDLOP_MonotonicNS();
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            got_epipe = errno == EPIPE;
            break; /* EPIPE: reader gone */
        }
        if (SDLOP_MonotonicNS() - last > 2000000000ull) {
            break; /* reader stalled */
        }
        struct pollfd pfd = { fd, POLLOUT, 0 };
        poll(&pfd, 1, 50);
    }
    if (got_epipe && !was_pending) {
        const struct timespec zero = { 0, 0 };
        while (sigtimedwait(&pipe_set, NULL, &zero) < 0 && errno == EINTR) {
        }
    }
    pthread_sigmask(SIG_SETMASK, &old_set, NULL);
}

static void source_target(void *data, struct wl_data_source *source, const char *mime)
{
    (void)data;
    (void)source;
    (void)mime;
}

static void source_send(void *data, struct wl_data_source *source, const char *mime, int32_t fd)
{
    (void)data;
    size_t size = 0;
    void *payload = source == wc.source ? SDLOP_GetInternalClipboardData(mime, &size) : NULL;
    if (payload) {
        write_pipe(fd, payload, size);
        SDL_free(payload);
    }
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *source)
{
    (void)data;
    if (source == wc.source) {
        /* another client took the clipboard: the app's offer is void */
        wc.source = NULL;
        SDLOP_CancelClipboardData();
    }
    wl_data_source_destroy(source);
}

static void source_dnd_drop_performed(void *data, struct wl_data_source *source)
{
    (void)data;
    (void)source;
}

static void source_dnd_finished(void *data, struct wl_data_source *source)
{
    (void)data;
    (void)source;
}

static void source_action(void *data, struct wl_data_source *source, uint32_t action)
{
    (void)data;
    (void)source;
    (void)action;
}

static const struct wl_data_source_listener source_listener = {
    source_target, source_send, source_cancelled, source_dnd_drop_performed, source_dnd_finished, source_action,
};

static void primary_source_send(void *data, struct zwp_primary_selection_source_v1 *source, const char *mime,
                                int32_t fd)
{
    (void)data;
    (void)mime;
    if (source == wc.primary_source && wc.primary_text) {
        write_pipe(fd, wc.primary_text, strlen(wc.primary_text));
    }
    close(fd);
}

static void primary_source_cancelled(void *data, struct zwp_primary_selection_source_v1 *source)
{
    (void)data;
    if (source == wc.primary_source) {
        wc.primary_source = NULL;
        SDL_free(wc.primary_text);
        wc.primary_text = NULL;
    }
    zwp_primary_selection_source_v1_destroy(source);
}

static const struct zwp_primary_selection_source_v1_listener primary_source_listener = {
    primary_source_send,
    primary_source_cancelled,
};

/* ------------------------------------------------------------------ */
/* devices                                                             */
/* ------------------------------------------------------------------ */

static void offer_offer(void *data, struct wl_data_offer *offer, const char *mime)
{
    (void)offer;
    offer_add_mime((Offer *)data, mime);
}

static void offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t actions)
{
    (void)data;
    (void)offer;
    (void)actions;
}

static void offer_action(void *data, struct wl_data_offer *offer, uint32_t action)
{
    (void)offer;
    if ((Offer *)data == wc.dnd) {
        wc.dnd_action = action;
    }
}

static const struct wl_data_offer_listener offer_listener = {
    offer_offer,
    offer_source_actions,
    offer_action,
};

static void publish_selection(const Offer *o)
{
    size_t n = o ? o->num_mimes : 0;
    size_t alloc = sizeof(char *);
    for (size_t i = 0; i < n; i++) {
        alloc += sizeof(char *) + strlen(o->mimes[i]) + 1;
    }
    char **types = (char **)SDLOP_AllocateTemporaryMemory(alloc);
    if (!types) {
        return;
    }
    char *p = (char *)(types + n + 1);
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(o->mimes[i]) + 1;
        types[i] = p;
        memcpy(p, o->mimes[i], len);
        p += len;
    }
    types[n] = NULL;
    SDLOP_SendClipboardUpdate(false, types, n);
}

static void device_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *id)
{
    (void)data;
    (void)device;
    Offer *o = offer_new(id, false);
    if (!o) {
        wl_data_offer_destroy(id);
        return;
    }
    wl_data_offer_add_listener(id, &offer_listener, o);
}

static SDL_Window *window_from_surface(struct wl_surface *surface)
{
    for (SDL_Window *w = sdlop.windows; surface && w; w = w->next) {
        if (SDLOP_Wayland_GetWindowSurfaceHandle(w) == surface) {
            return w;
        }
    }
    return NULL;
}

static bool window_alive(SDL_Window *window)
{
    for (SDL_Window *w = sdlop.windows; window && w; w = w->next) {
        if (w == window) {
            return true;
        }
    }
    return false;
}

/* text/uri-list > text/plain;charset=utf-8 > the other text types */
static const char *pick_dnd_mime(const Offer *o)
{
    static const char *const order[] = { "text/uri-list", "text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
                                         "TEXT", "STRING" };
    for (size_t i = 0; o && i < SDL_arraysize(order); i++) {
        for (size_t k = 0; k < o->num_mimes; k++) {
            if (strcmp(o->mimes[k], order[i]) == 0) {
                return o->mimes[k];
            }
        }
    }
    return NULL;
}

static void dnd_end(void)
{
    if (wc.dnd && wc.dnd != wc.selection) {
        offer_destroy(wc.dnd);
    }
    wc.dnd = NULL;
    wc.dnd_window = NULL;
    wc.dnd_mime = NULL;
    wc.dnd_action = 0;
}

static void device_enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface,
                         wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *id)
{
    (void)data;
    (void)device;
    dnd_end();
    wc.dnd = id ? (Offer *)wl_data_offer_get_user_data(id) : NULL;
    wc.dnd_window = window_from_surface(surface);
    if (!wc.dnd) {
        return; /* a drag of our own without data */
    }
    wc.dnd_mime = (wc.dnd_window && SDLOP_IsAcceptingDragAndDrop()) ? pick_dnd_mime(wc.dnd) : NULL;
    wl_data_offer_accept(id, serial, wc.dnd_mime);
    if (wc.manager_version >= 3) {
        const uint32_t copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
        wl_data_offer_set_actions(id, wc.dnd_mime ? copy : 0, wc.dnd_mime ? copy : 0);
    }
    if (wc.dnd_mime) {
        SDLOP_SendDropPosition(wc.dnd_window, (float)wl_fixed_to_double(x), (float)wl_fixed_to_double(y));
    }
}

static void device_leave(void *data, struct wl_data_device *device)
{
    (void)data;
    (void)device;
    if (window_alive(wc.dnd_window) && wc.dnd_window->is_dropping) {
        SDLOP_SendDropComplete(wc.dnd_window);
    }
    dnd_end();
}

static void device_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    (void)device;
    (void)time;
    if (wc.dnd_mime && window_alive(wc.dnd_window)) {
        SDLOP_SendDropPosition(wc.dnd_window, (float)wl_fixed_to_double(x), (float)wl_fixed_to_double(y));
    }
}

static void device_drop(void *data, struct wl_data_device *device)
{
    (void)data;
    (void)device;
    SDL_Window *window = window_alive(wc.dnd_window) ? wc.dnd_window : NULL;
    if (wc.dnd && wc.dnd_mime && window) {
        size_t len = 0;
        char *payload = (char *)offer_receive(wc.dnd, wc.dnd_mime, &len);
        SDLOP_SendDropPayload(window, strcmp(wc.dnd_mime, "text/uri-list") == 0, payload);
        SDL_free(payload);
        /* finish is only valid with a negotiated action (v3) */
        if (payload && wc.manager_version >= 3 && wc.dnd_action != 0) {
            wl_data_offer_finish((struct wl_data_offer *)wc.dnd->proxy);
        }
    } else if (window && window->is_dropping) {
        SDLOP_SendDropComplete(window);
    }
    dnd_end();
}

static void device_selection(void *data, struct wl_data_device *device, struct wl_data_offer *id)
{
    (void)data;
    (void)device;
    Offer *o = id ? (Offer *)wl_data_offer_get_user_data(id) : NULL;
    const bool had = wc.selection != NULL;
    if (wc.selection != o) {
        if (wc.selection != wc.dnd) {
            offer_destroy(wc.selection);
        }
        wc.selection = o;
    }
    if (wc.source) {
        return; /* our own source echoed back (or cleared before cancel) */
    }
    if (o || had) {
        publish_selection(o);
    }
}

static const struct wl_data_device_listener device_listener = {
    device_data_offer, device_enter, device_leave, device_motion, device_drop, device_selection,
};

static void primary_offer_offer(void *data, struct zwp_primary_selection_offer_v1 *offer, const char *mime)
{
    (void)offer;
    offer_add_mime((Offer *)data, mime);
}

static const struct zwp_primary_selection_offer_v1_listener primary_offer_listener = {
    primary_offer_offer,
};

static void primary_device_data_offer(void *data, struct zwp_primary_selection_device_v1 *device,
                                      struct zwp_primary_selection_offer_v1 *id)
{
    (void)data;
    (void)device;
    Offer *o = offer_new(id, true);
    if (!o) {
        zwp_primary_selection_offer_v1_destroy(id);
        return;
    }
    zwp_primary_selection_offer_v1_add_listener(id, &primary_offer_listener, o);
}

static void primary_device_selection(void *data, struct zwp_primary_selection_device_v1 *device,
                                     struct zwp_primary_selection_offer_v1 *id)
{
    (void)data;
    (void)device;
    Offer *o = id ? (Offer *)zwp_primary_selection_offer_v1_get_user_data(id) : NULL;
    if (wc.primary_offer != o) {
        offer_destroy(wc.primary_offer);
        wc.primary_offer = o;
    }
}

static const struct zwp_primary_selection_device_v1_listener primary_device_listener = {
    primary_device_data_offer,
    primary_device_selection,
};

static void create_devices(void)
{
    if (!wc.seat) {
        return;
    }
    if (wc.manager && !wc.device) {
        wc.device = wl_data_device_manager_get_data_device(wc.manager, wc.seat);
        if (wc.device) {
            wl_data_device_add_listener(wc.device, &device_listener, NULL);
        }
    }
    if (wc.primary_manager && !wc.primary_device) {
        wc.primary_device = zwp_primary_selection_device_manager_v1_get_device(wc.primary_manager, wc.seat);
        if (wc.primary_device) {
            zwp_primary_selection_device_v1_add_listener(wc.primary_device, &primary_device_listener, NULL);
        }
    }
}

/* ------------------------------------------------------------------ */
/* driver entry points                                                 */
/* ------------------------------------------------------------------ */

bool SDLOP_Wayland_Clipboard_Global(struct wl_registry *registry, uint32_t name, const char *interface,
                                    uint32_t version)
{
    if (strcmp(interface, wl_data_device_manager_interface.name) == 0 && !wc.manager &&
        !SDLOP_HintFlag("SDLOP_WAYLAND_NO_DATA_DEVICE", false)) {
        wc.manager_version = version < 3 ? version : 3;
        wc.manager = (struct wl_data_device_manager *)wl_registry_bind(registry, name,
                                                                       &wl_data_device_manager_interface,
                                                                       wc.manager_version);
        create_devices();
        return true;
    }
    if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0 && !wc.primary_manager) {
        wc.primary_manager = (struct zwp_primary_selection_device_manager_v1 *)wl_registry_bind(
            registry, name, &zwp_primary_selection_device_manager_v1_interface, 1);
        create_devices();
        return true;
    }
    return false;
}

void SDLOP_Wayland_Clipboard_SeatAdded(struct wl_seat *seat)
{
    if (!wc.seat) {
        wc.seat = seat;
        create_devices();
    }
}

void SDLOP_Wayland_Clipboard_Quit(void)
{
    if (wc.dnd != wc.selection) {
        offer_destroy(wc.dnd);
    }
    offer_destroy(wc.selection);
    offer_destroy(wc.primary_offer);
    if (wc.source) {
        wl_data_source_destroy(wc.source);
    }
    if (wc.primary_source) {
        zwp_primary_selection_source_v1_destroy(wc.primary_source);
    }
    SDL_free(wc.primary_text);
    if (wc.device) {
        if (wc.manager_version >= 2) {
            wl_data_device_release(wc.device);
        } else {
            wl_data_device_destroy(wc.device);
        }
    }
    if (wc.primary_device) {
        zwp_primary_selection_device_v1_destroy(wc.primary_device);
    }
    if (wc.manager) {
        wl_data_device_manager_destroy(wc.manager); /* no request: proxy only */
    }
    if (wc.primary_manager) {
        zwp_primary_selection_device_manager_v1_destroy(wc.primary_manager);
    }
    memset(&wc, 0, sizeof(wc));
}

bool SDLOP_Wayland_Clipboard_Available(void)
{
    return wc.device != NULL;
}

bool SDLOP_Wayland_PrimarySelection_Available(void)
{
    return wc.primary_device != NULL;
}

const char *const *SDLOP_Wayland_GetTextMimeTypes(SDLop_VideoDevice *device, size_t *num_mime_types)
{
    (void)device;
    *num_mime_types = SDL_arraysize(text_mime_types);
    return text_mime_types;
}

bool SDLOP_Wayland_SetClipboardData(SDLop_VideoDevice *device)
{
    (void)device;
    if (!wc.device) {
        return SDL_SetError("The compositor has no wl_data_device_manager");
    }
    size_t count = 0;
    const char *const *mimes = SDLOP_GetClipboardOfferMimeTypes(&count);
    struct wl_data_source *old = wc.source;
    wc.source = NULL;
    if (count > 0) {
        wc.source = wl_data_device_manager_create_data_source(wc.manager);
        if (!wc.source) {
            return SDL_SetError("Couldn't create a wl_data_source");
        }
        wl_data_source_add_listener(wc.source, &source_listener, NULL);
        for (size_t i = 0; i < count; i++) {
            wl_data_source_offer(wc.source, mimes[i]);
        }
    }
    wl_data_device_set_selection(wc.device, wc.source, SDLOP_Wayland_GetSelectionSerial());
    if (old) {
        wl_data_source_destroy(old);
    }
    wl_display_flush(SDLOP_Wayland_GetDisplay());
    return true;
}

void *SDLOP_Wayland_GetClipboardData(SDLop_VideoDevice *device, const char *mime_type, size_t *size)
{
    (void)device;
    *size = 0;
    if (wc.source) {
        return SDLOP_GetInternalClipboardData(mime_type, size);
    }
    return offer_receive(wc.selection, mime_type, size);
}

bool SDLOP_Wayland_HasClipboardData(SDLop_VideoDevice *device, const char *mime_type)
{
    (void)device;
    if (wc.source) {
        return SDLOP_HasInternalClipboardData(mime_type);
    }
    return offer_has_mime(wc.selection, mime_type);
}

bool SDLOP_Wayland_SetPrimarySelectionText(SDLop_VideoDevice *device, const char *text)
{
    (void)device;
    if (!wc.primary_device) {
        return SDL_SetError("Primary selection not supported");
    }
    struct zwp_primary_selection_source_v1 *old = wc.primary_source;
    wc.primary_source = NULL;
    SDL_free(wc.primary_text);
    wc.primary_text = NULL;
    if (text && text[0] != '\0') {
        wc.primary_text = SDL_strdup(text);
        wc.primary_source = zwp_primary_selection_device_manager_v1_create_source(wc.primary_manager);
        if (!wc.primary_text || !wc.primary_source) {
            if (old) {
                zwp_primary_selection_source_v1_destroy(old);
            }
            return SDL_SetError("Couldn't create a primary selection source");
        }
        zwp_primary_selection_source_v1_add_listener(wc.primary_source, &primary_source_listener, NULL);
        for (size_t i = 0; i < SDL_arraysize(text_mime_types); i++) {
            zwp_primary_selection_source_v1_offer(wc.primary_source, text_mime_types[i]);
        }
    }
    zwp_primary_selection_device_v1_set_selection(wc.primary_device, wc.primary_source,
                                                  SDLOP_Wayland_GetSelectionSerial());
    if (old) {
        zwp_primary_selection_source_v1_destroy(old);
    }
    wl_display_flush(SDLOP_Wayland_GetDisplay());
    return true;
}

char *SDLOP_Wayland_GetPrimarySelectionText(SDLop_VideoDevice *device)
{
    (void)device;
    if (wc.primary_source && wc.primary_text) {
        return SDL_strdup(wc.primary_text);
    }
    if (!wc.primary_source) {
        for (size_t i = 0; i < SDL_arraysize(text_mime_types); i++) {
            if (offer_has_mime(wc.primary_offer, text_mime_types[i])) {
                size_t len = 0;
                char *text = (char *)offer_receive(wc.primary_offer, text_mime_types[i], &len);
                if (text) {
                    return text;
                }
                break;
            }
        }
    }
    return SDL_strdup("");
}

bool SDLOP_Wayland_HasPrimarySelectionText(SDLop_VideoDevice *device)
{
    (void)device;
    if (wc.primary_source) {
        return wc.primary_text != NULL;
    }
    for (size_t i = 0; i < SDL_arraysize(text_mime_types); i++) {
        if (offer_has_mime(wc.primary_offer, text_mime_types[i])) {
            return true;
        }
    }
    return false;
}
