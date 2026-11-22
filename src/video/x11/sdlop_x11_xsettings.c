/*
  SDLop - X11 XSETTINGS client.

  Follows the XSETTINGS specification (freedesktop.org, 0.5): the settings
  manager owns the _XSETTINGS_S<screen> selection and keeps the settings in
  the _XSETTINGS_SETTINGS property of the owner window. We track the owner
  (MANAGER client messages on the root announce new ones, DestroyNotify
  ends one), re-read the property on PropertyNotify and parse it into a
  small table.

  Used, as in stock SDL3, for the content scale when Xft.dpi is not in the
  resource database: Gdk/WindowScalingFactor, then Xft/DPI (1024ths of a
  dpi). SDLOP_X11_NO_XSETTINGS disables the client.

  Written from the specification; no code from xsettings-client.c.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    XSETTINGS_TYPE_INT = 0,
    XSETTINGS_TYPE_STRING = 1,
    XSETTINGS_TYPE_COLOR = 2
};

typedef struct XSetting
{
    char *name;
    int type;
    int32_t ival;
    char *sval;
    uint16_t color[4]; /* r, g, b, a */
} XSetting;

static struct
{
    bool active;
    xcb_atom_t selection, settings_atom, manager_atom;
    xcb_window_t owner;
    XSetting *settings;
    int num_settings;
} xs;

static xcb_connection_t *conn(void)
{
    return SDLOP_X11_GetConnection();
}

static xcb_atom_t intern(const char *name)
{
    xcb_intern_atom_reply_t *r =
        X11_xcb_intern_atom_reply(conn(), X11_xcb_intern_atom(conn(), 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

static void free_settings(void)
{
    for (int i = 0; i < xs.num_settings; i++) {
        free(xs.settings[i].name);
        free(xs.settings[i].sval);
    }
    free(xs.settings);
    xs.settings = NULL;
    xs.num_settings = 0;
}

/* ------------------------------------------------------------------ */
/* parsing                                                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    const uint8_t *p, *end;
    bool msb; /* byte order of the data: MSBFirst */
} Reader;

static bool need(Reader *r, size_t n)
{
    return (size_t)(r->end - r->p) >= n;
}

static uint16_t rd16(Reader *r)
{
    uint16_t v = r->msb ? (uint16_t)((r->p[0] << 8) | r->p[1]) : (uint16_t)(r->p[0] | (r->p[1] << 8));
    r->p += 2;
    return v;
}

static uint32_t rd32(Reader *r)
{
    uint32_t v = r->msb ? ((uint32_t)r->p[0] << 24) | ((uint32_t)r->p[1] << 16) | ((uint32_t)r->p[2] << 8) | r->p[3]
                        : r->p[0] | ((uint32_t)r->p[1] << 8) | ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    return v;
}

static size_t pad4(size_t n)
{
    return (n + 3) & ~(size_t)3;
}

static char *dup_n(const uint8_t *s, size_t n)
{
    char *d = (char *)malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

/* parses the property into a fresh table; false on malformed data */
static bool parse(const uint8_t *data, size_t len, XSetting **out, int *count)
{
    *out = NULL;
    *count = 0;
    if (len < 12 || (data[0] != 0 && data[0] != 1)) {
        return false;
    }
    Reader r = { data + 4, data + len, data[0] == 1 };
    (void)rd32(&r); /* serial */
    uint32_t n = rd32(&r);
    if (n > 4096) {
        return false;
    }
    XSetting *set = (XSetting *)calloc(n ? n : 1, sizeof(*set));
    if (!set) {
        return false;
    }
    int got = 0;
    bool ok = true;
    for (uint32_t i = 0; i < n && ok; i++) {
        ok = false;
        if (!need(&r, 4)) {
            break;
        }
        const uint8_t type = r.p[0];
        r.p += 2; /* type, pad */
        const uint16_t name_len = rd16(&r);
        if (!need(&r, pad4(name_len) + 4)) {
            break;
        }
        XSetting *s = &set[got];
        s->type = type;
        s->name = dup_n(r.p, name_len);
        r.p += pad4(name_len);
        (void)rd32(&r); /* last-change-serial */
        if (!s->name) {
            break;
        }
        got++;
        if (type == XSETTINGS_TYPE_INT) {
            if (!need(&r, 4)) {
                break;
            }
            s->ival = (int32_t)rd32(&r);
        } else if (type == XSETTINGS_TYPE_STRING) {
            if (!need(&r, 4)) {
                break;
            }
            const uint32_t slen = rd32(&r);
            if (slen > (uint32_t)(r.end - r.p) || !need(&r, pad4(slen))) {
                break;
            }
            s->sval = dup_n(r.p, slen);
            r.p += pad4(slen);
            if (!s->sval) {
                break;
            }
        } else if (type == XSETTINGS_TYPE_COLOR) {
            if (!need(&r, 8)) {
                break;
            }
            for (int c = 0; c < 4; c++) {
                s->color[c] = rd16(&r);
            }
        } else {
            break; /* unknown type: the length of its value is unknown */
        }
        ok = true;
    }
    if (!ok) {
        for (int i = 0; i < got; i++) {
            free(set[i].name);
            free(set[i].sval);
        }
        free(set);
        return false;
    }
    *out = set;
    *count = got;
    return true;
}

/* ------------------------------------------------------------------ */
/* manager tracking                                                    */
/* ------------------------------------------------------------------ */

static void read_settings(void)
{
    free_settings();
    if (!xs.owner) {
        return;
    }
    xcb_get_property_reply_t *r = X11_xcb_get_property_reply(
        conn(), X11_xcb_get_property(conn(), 0, xs.owner, xs.settings_atom, xs.settings_atom, 0, 1u << 20), NULL);
    if (r && r->format == 8 && r->type == xs.settings_atom) {
        XSetting *set;
        int n;
        if (parse((const uint8_t *)X11_xcb_get_property_value(r), (size_t)X11_xcb_get_property_value_length(r), &set,
                  &n)) {
            xs.settings = set;
            xs.num_settings = n;
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "SDLop x11: malformed _XSETTINGS_SETTINGS ignored");
        }
    }
    free(r);
}

/* looks up the selection owner and watches it; the server grab keeps the
 * owner from vanishing between the lookup and the event selection */
static void find_owner(void)
{
    X11_xcb_grab_server(conn());
    xcb_get_selection_owner_reply_t *r =
        X11_xcb_get_selection_owner_reply(conn(), X11_xcb_get_selection_owner(conn(), xs.selection), NULL);
    xs.owner = r ? r->owner : XCB_NONE;
    free(r);
    if (xs.owner) {
        uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
        X11_xcb_change_window_attributes(conn(), xs.owner, XCB_CW_EVENT_MASK, &mask);
    }
    X11_xcb_ungrab_server(conn());
    read_settings();
}

void SDLOP_X11_XSettings_Init(void)
{
    memset(&xs, 0, sizeof(xs));
    if (!conn() || SDLOP_HintFlag("SDLOP_X11_NO_XSETTINGS", false)) {
        return;
    }
    char name[32];
    snprintf(name, sizeof(name), "_XSETTINGS_S%d", SDLOP_X11_GetScreenNumber());
    xs.selection = intern(name);
    xs.settings_atom = intern("_XSETTINGS_SETTINGS");
    xs.manager_atom = intern("MANAGER");
    if (!xs.selection || !xs.settings_atom || !xs.manager_atom) {
        return;
    }
    xs.active = true;
    find_owner();
}

void SDLOP_X11_XSettings_Quit(void)
{
    free_settings();
    memset(&xs, 0, sizeof(xs));
}

bool SDLOP_X11_XSettings_HandleEvent(xcb_generic_event_t *ev, bool *changed)
{
    if (!xs.active) {
        return false;
    }
    switch (ev->response_type & 0x7f) {
    case XCB_CLIENT_MESSAGE: {
        /* ICCCM 2.8: a new manager announces itself on the root */
        xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
        if (cm->window == SDLOP_X11_GetRoot() && cm->type == xs.manager_atom && cm->format == 32 &&
            cm->data.data32[1] == xs.selection) {
            find_owner();
            *changed = true;
            return true;
        }
        return false;
    }
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *pn = (xcb_property_notify_event_t *)ev;
        if (xs.owner && pn->window == xs.owner) {
            if (pn->atom == xs.settings_atom) {
                read_settings();
                *changed = true;
            }
            return true;
        }
        return false;
    }
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *dn = (xcb_destroy_notify_event_t *)ev;
        if (xs.owner && dn->window == xs.owner) {
            xs.owner = XCB_NONE;
            free_settings();
            *changed = true;
            return true;
        }
        return false;
    }
    default:
        /* other StructureNotify events of the owner window */
        if ((ev->response_type & 0x7f) >= XCB_UNMAP_NOTIFY && (ev->response_type & 0x7f) <= XCB_CIRCULATE_NOTIFY &&
            xs.owner) {
            /* event window is the second field of these structures */
            const xcb_unmap_notify_event_t *sn = (const xcb_unmap_notify_event_t *)ev;
            return sn->event == xs.owner;
        }
        return false;
    }
}

static const XSetting *find(const char *name)
{
    for (int i = 0; i < xs.num_settings; i++) {
        if (strcmp(xs.settings[i].name, name) == 0) {
            return &xs.settings[i];
        }
    }
    return NULL;
}

bool SDLOP_X11_XSettings_GetInt(const char *name, int *value)
{
    const XSetting *s = find(name);
    if (!s || s->type != XSETTINGS_TYPE_INT) {
        return false;
    }
    *value = s->ival;
    return true;
}

const char *SDLOP_X11_XSettings_GetString(const char *name)
{
    const XSetting *s = find(name);
    return s && s->type == XSETTINGS_TYPE_STRING ? s->sval : NULL;
}
