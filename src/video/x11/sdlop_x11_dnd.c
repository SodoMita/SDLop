/*
  SDLop - X11 drag-and-drop target (XDND, protocol version 5).

  Port of stock SDL3's handling in SDL_x11events.c / SDL_x11window.c:
    - windows carry XdndAware = 5 while SDL_EVENT_DROP_FILE/TEXT is enabled
    - XdndEnter picks a target: text/uri-list over text/plain;charset=utf-8 /
      UTF8_STRING over text/plain / TEXT (from the three inline types or
      XdndTypeList)
    - XdndPosition -> SDL_EVENT_DROP_POSITION (window coordinates, accepted
      drags only) and XdndStatus (accept, copy)
    - XdndDrop converts XdndSelection; the data becomes one
      SDL_EVENT_DROP_FILE per local file: URI or SDL_EVENT_DROP_TEXT per
      line, then SDL_EVENT_DROP_COMPLETE and XdndFinished
    - XdndLeave -> SDL_EVENT_DROP_COMPLETE
  Beyond stock: large drops sent with INCR are reassembled (stock reads a
  single property), comment lines of text/uri-list are skipped, and a
  refused or failed drop still completes the SDL_EVENT_DROP_* sequence
  when DROP_BEGIN was sent.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"

#include <string.h>

static struct
{
    bool atoms_ok;
    xcb_atom_t XdndAware, XdndEnter, XdndPosition, XdndStatus, XdndLeave, XdndDrop, XdndFinished, XdndSelection,
        XdndTypeList, XdndActionCopy, SDL_DND, INCR, uri_list, utf8_text, UTF8_STRING, text_plain, TEXT;

    /* one drag at a time (the pointer is in one window) */
    SDL_Window *window;
    xcb_window_t xwindow;
    xcb_window_t source;
    int version;
    xcb_atom_t target; /* chosen type, XCB_ATOM_NONE = refusing */

    /* INCR receive of the drop data */
    bool incr;
    Uint8 *buf;
    size_t len;
} dnd;

static xcb_connection_t *conn(void)
{
    return SDLOP_X11_GetConnection();
}

static bool ensure_atoms(void)
{
    if (dnd.atoms_ok) {
        return true;
    }
    static const char *const names[] = { "XdndAware", "XdndEnter", "XdndPosition", "XdndStatus", "XdndLeave",
                                         "XdndDrop", "XdndFinished", "XdndSelection", "XdndTypeList",
                                         "XdndActionCopy", "SDL_DND", "INCR", "text/uri-list",
                                         "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT" };
    xcb_atom_t *dst = &dnd.XdndAware;
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
    dnd.atoms_ok = ok;
    return ok;
}

static SDL_Window *window_from_xid(xcb_window_t xid)
{
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        if (SDLOP_X11_GetWindowXID(w) == xid) {
            return w;
        }
    }
    return NULL;
}

void SDLOP_X11_AcceptDragAndDrop(SDL_Window *window, bool accept)
{
    xcb_window_t xid = SDLOP_X11_GetWindowXID(window);
    if (!xid || !conn() || !ensure_atoms()) {
        return;
    }
    if (accept) {
        const uint32_t version = 5;
        X11_xcb_change_property(conn(), XCB_PROP_MODE_REPLACE, xid, dnd.XdndAware, XCB_ATOM_ATOM, 32, 1, &version);
    } else {
        X11_xcb_delete_property(conn(), xid, dnd.XdndAware);
    }
    X11_xcb_flush(conn());
}

/* text/uri-list > text/plain;charset=utf-8, UTF8_STRING > text/plain, TEXT */
static xcb_atom_t pick_target(const xcb_atom_t *atoms, size_t count)
{
    xcb_atom_t preferred = XCB_ATOM_NONE, fallback = XCB_ATOM_NONE;
    for (size_t i = 0; i < count; i++) {
        xcb_atom_t a = atoms[i];
        if (a == dnd.uri_list) {
            return a;
        }
        if ((a == dnd.utf8_text || a == dnd.UTF8_STRING) && preferred == XCB_ATOM_NONE) {
            preferred = a;
        }
        if ((a == dnd.text_plain || a == dnd.TEXT) && fallback == XCB_ATOM_NONE) {
            fallback = a;
        }
    }
    return preferred != XCB_ATOM_NONE ? preferred : fallback;
}

static void send_client_message(xcb_window_t to, xcb_atom_t type, uint32_t l0, uint32_t l1, uint32_t l2, uint32_t l3,
                                uint32_t l4)
{
    xcb_client_message_event_t m;
    memset(&m, 0, sizeof(m));
    m.response_type = XCB_CLIENT_MESSAGE;
    m.format = 32;
    m.window = to;
    m.type = type;
    m.data.data32[0] = l0;
    m.data.data32[1] = l1;
    m.data.data32[2] = l2;
    m.data.data32[3] = l3;
    m.data.data32[4] = l4;
    X11_xcb_send_event(conn(), 0, to, XCB_EVENT_MASK_NO_EVENT, (const char *)&m);
}

static void finish(bool accepted)
{
    if (dnd.source) {
        send_client_message(dnd.source, dnd.XdndFinished, dnd.xwindow, accepted ? 1 : 0,
                            accepted ? dnd.XdndActionCopy : XCB_ATOM_NONE, 0, 0);
        X11_xcb_flush(conn());
    }
    SDL_free(dnd.buf);
    dnd.buf = NULL;
    dnd.len = 0;
    dnd.incr = false;
    dnd.source = 0;
    dnd.target = XCB_ATOM_NONE;
}

/* deliver the dropped data */
static void deliver(char *data)
{
    /* the window may have been destroyed meanwhile */
    SDLOP_SendDropPayload(window_from_xid(dnd.xwindow), dnd.target == dnd.uri_list, data);
}

/* property SDL_DND of the drop window, deleted after reading */
static Uint8 *take_property(xcb_atom_t *type, size_t *len)
{
    xcb_get_property_reply_t *r = X11_xcb_get_property_reply(
        conn(), X11_xcb_get_property(conn(), 1, dnd.xwindow, dnd.SDL_DND, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x1fffffff),
        NULL);
    *type = XCB_ATOM_NONE;
    *len = 0;
    if (!r) {
        return NULL;
    }
    *type = r->type;
    int n = X11_xcb_get_property_value_length(r);
    if (n < 0) {
        n = 0;
    }
    Uint8 *d = (Uint8 *)SDL_malloc((size_t)n + 1);
    if (d) {
        memcpy(d, X11_xcb_get_property_value(r), (size_t)n);
        d[n] = '\0';
        *len = (size_t)n;
    }
    free(r);
    return d;
}

static void append(const Uint8 *chunk, size_t n)
{
    Uint8 *grown = (Uint8 *)SDL_realloc(dnd.buf, dnd.len + n + 1);
    if (!grown) {
        return;
    }
    dnd.buf = grown;
    memcpy(dnd.buf + dnd.len, chunk, n);
    dnd.len += n;
    dnd.buf[dnd.len] = '\0';
}

bool SDLOP_X11_DnD_HandleEvent(xcb_generic_event_t *ev)
{
    if (!dnd.atoms_ok) {
        return false;
    }
    const uint8_t type = ev->response_type & 0x7f;
    if (type == XCB_CLIENT_MESSAGE) {
        xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
        const uint32_t *l = cm->data.data32;
        if (cm->type == dnd.XdndEnter) {
            dnd.window = window_from_xid(cm->window);
            dnd.xwindow = cm->window;
            dnd.source = l[0];
            dnd.version = (int)(l[1] >> 24);
            SDL_free(dnd.buf);
            dnd.buf = NULL;
            dnd.len = 0;
            dnd.incr = false;
            if (l[1] & 1) {
                /* more than three types: XdndTypeList on the source */
                xcb_get_property_reply_t *r = X11_xcb_get_property_reply(
                    conn(), X11_xcb_get_property(conn(), 0, dnd.source, dnd.XdndTypeList, XCB_ATOM_ATOM, 0, 1024), NULL);
                dnd.target = r ? pick_target((const xcb_atom_t *)X11_xcb_get_property_value(r),
                                             (size_t)X11_xcb_get_property_value_length(r) / 4)
                               : XCB_ATOM_NONE;
                free(r);
            } else {
                const xcb_atom_t three[3] = { l[2], l[3], l[4] };
                dnd.target = pick_target(three, 3);
            }
            if (!dnd.window || !SDLOP_IsAcceptingDragAndDrop()) {
                dnd.target = XCB_ATOM_NONE;
            }
            return true;
        }
        if (cm->type == dnd.XdndPosition) {
            if (l[0] != dnd.source) {
                return true; /* not the drag we know (no XdndEnter seen) */
            }
            const int16_t root_x = (int16_t)(l[2] >> 16), root_y = (int16_t)(l[2] & 0xffff);
            xcb_translate_coordinates_reply_t *tr = X11_xcb_translate_coordinates_reply(
                conn(), X11_xcb_translate_coordinates(conn(), SDLOP_X11_GetRoot(), cm->window, root_x, root_y), NULL);
            /* only drags we accept produce events (stock also reports
             * refused ones, leaving a DROP_BEGIN without COMPLETE) */
            if (tr && dnd.window && dnd.target != XCB_ATOM_NONE) {
                SDLOP_SendDropPosition(dnd.window, (float)tr->dst_x, (float)tr->dst_y);
            }
            free(tr);
            send_client_message(dnd.source, dnd.XdndStatus, cm->window, dnd.target != XCB_ATOM_NONE ? 1 : 0, 0, 0,
                                dnd.XdndActionCopy);
            X11_xcb_flush(conn());
            return true;
        }
        if (cm->type == dnd.XdndLeave) {
            if (dnd.window && dnd.window->is_dropping) {
                SDLOP_SendDropComplete(dnd.window);
            }
            dnd.source = 0;
            dnd.target = XCB_ATOM_NONE;
            return true;
        }
        if (cm->type == dnd.XdndDrop) {
            if (l[0] != dnd.source) {
                return true;
            }
            if (dnd.target == XCB_ATOM_NONE) {
                if (dnd.window && dnd.window->is_dropping) {
                    SDLOP_SendDropComplete(dnd.window);
                }
                finish(false);
                return true;
            }
            X11_xcb_delete_property(conn(), cm->window, dnd.SDL_DND);
            X11_xcb_convert_selection(conn(), cm->window, dnd.XdndSelection, dnd.target, dnd.SDL_DND,
                                      dnd.version >= 1 ? l[2] : XCB_CURRENT_TIME);
            X11_xcb_flush(conn());
            return true;
        }
        return false;
    }
    if (type == XCB_SELECTION_NOTIFY) {
        xcb_selection_notify_event_t *sn = (xcb_selection_notify_event_t *)ev;
        if (sn->selection != dnd.XdndSelection || sn->requestor != dnd.xwindow || !dnd.source) {
            return false;
        }
        if (sn->property == XCB_ATOM_NONE) {
            if (dnd.window && dnd.window->is_dropping) {
                SDLOP_SendDropComplete(dnd.window);
            }
            finish(false);
            return true;
        }
        xcb_atom_t ptype;
        size_t n;
        Uint8 *data = take_property(&ptype, &n);
        X11_xcb_flush(conn());
        if (data && ptype == dnd.INCR) {
            dnd.incr = true; /* chunks follow as PropertyNotify(NewValue) */
            SDL_free(data);
            return true;
        }
        if (data) {
            deliver((char *)data);
            SDL_free(data);
        }
        finish(data != NULL);
        return true;
    }
    if (type == XCB_PROPERTY_NOTIFY && dnd.incr) {
        xcb_property_notify_event_t *pn = (xcb_property_notify_event_t *)ev;
        if (pn->window != dnd.xwindow || pn->atom != dnd.SDL_DND || pn->state != XCB_PROPERTY_NEW_VALUE) {
            return false;
        }
        xcb_atom_t ptype;
        size_t n;
        Uint8 *chunk = take_property(&ptype, &n);
        X11_xcb_flush(conn());
        if (chunk && n > 0) {
            append(chunk, n);
            SDL_free(chunk);
            return true;
        }
        SDL_free(chunk);
        /* zero-length chunk: complete */
        if (dnd.buf) {
            deliver((char *)dnd.buf);
        } else if (dnd.window && dnd.window->is_dropping) {
            SDLOP_SendDropComplete(dnd.window);
        }
        finish(true);
        return true;
    }
    return false;
}

void SDLOP_X11_DnD_Init(void)
{
    memset(&dnd, 0, sizeof(dnd));
    if (conn()) {
        ensure_atoms();
    }
}

void SDLOP_X11_DnD_Quit(void)
{
    SDL_free(dnd.buf);
    memset(&dnd, 0, sizeof(dnd));
}

void SDLOP_X11_DnD_WindowDestroyed(SDL_Window *window)
{
    if (dnd.window == window) {
        dnd.window = NULL;
    }
}
