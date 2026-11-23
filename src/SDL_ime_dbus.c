/*
  SDLop - input method clients over D-Bus: Fcitx 5 and IBus.

  Stock SDL3 uses these on Wayland when the compositor has no
  zwp_text_input_manager_v3 (src/core/linux/SDL_ime.c, SDL_fcitx.c,
  SDL_ibus.c); X11 uses XIM in both. SDLop does the same, on its own
  session bus client (SDL_dbus.c) instead of libdbus:

  - Fcitx 5:  org.freedesktop.portal.Fcitx, /org/freedesktop/portal/inputmethod,
              org.fcitx.Fcitx.InputMethod1.CreateInputContext(a(ss)) -> (o ay);
              input context org.fcitx.Fcitx.InputContext1
  - IBus:     org.freedesktop.portal.IBus, /org/freedesktop/IBus,
              org.freedesktop.IBus.Portal.CreateInputContext(s) -> o;
              input context org.freedesktop.IBus.InputContext. The portal
              is on the session bus (ibus >= 1.5.17), which is also what
              stock tries first; the private ibus-daemon bus is not used.

  Which one: SDL_IM_MODULE as in stock ("fcitx" / "ibus"; anything else
  turns the D-Bus input method off), else the module named by
  XMODIFIERS (@im=...), GTK_IM_MODULE or QT_IM_MODULE. With nothing named,
  only an input method that is already running is used (Fcitx owning its
  portal name, or an IBus address in IBUS_ADDRESS / ~/.config/ibus/bus),
  so a desktop without one never D-Bus-activates a daemon.

  Lazy: nothing touches the bus until text input is first started on a
  focused window. Keys go to the input method only while its context has
  focus (text input active), so games pay no D-Bus round trip per key;
  stock asks the input method about every key.

  The key path is synchronous like stock's (ProcessKeyEvent -> handled),
  with a short timeout: a stuck daemon costs at most that, then typing
  falls back to plain text. Commits and preedit arrive as signals,
  dispatched from SDL_PumpEvents.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#define FCITX_SERVICE "org.freedesktop.portal.Fcitx"
#define FCITX_IM_PATH "/org/freedesktop/portal/inputmethod"
#define FCITX_IM_IFACE "org.fcitx.Fcitx.InputMethod1"
#define FCITX_IC_IFACE "org.fcitx.Fcitx.InputContext1"

#define IBUS_SERVICE "org.freedesktop.portal.IBus"
#define IBUS_PATH "/org/freedesktop/IBus"
#define IBUS_PORTAL_IFACE "org.freedesktop.IBus.Portal"
#define IBUS_IC_IFACE "org.freedesktop.IBus.InputContext"
#define IBUS_SERVICE_IFACE "org.freedesktop.IBus.Service"

/* IBus capabilities and modifier masks (ibustypes.h) */
#define IBUS_CAP_PREEDIT_TEXT (1u << 0)
#define IBUS_CAP_FOCUS (1u << 3)
#define IBUS_RELEASE_MASK (1u << 30)

/* Fcitx capabilities (fcitx-utils/capabilityflags.h) */
#define FCITX_CAP_PREEDIT (1ull << 1)
#define FCITX_CAP_FORMATTED_PREEDIT (1ull << 4)

#define IME_CREATE_TIMEOUT_MS 1000 /* may D-Bus-activate the portal */
#define IME_KEY_TIMEOUT_MS 300

typedef enum
{
    IME_NONE,
    IME_FCITX,
    IME_IBUS
} IMEKind;

static struct
{
    bool tried;  /* init attempted (success or not) */
    IMEKind kind;
    char *ic_path;
    char *match;
    bool handler;
    bool focused;
    SDL_Window *focus_window;
    SDL_Rect sent_rect; /* last cursor rectangle sent, screen space */
    bool sent_rect_valid;
    /* IBus: Hide/ShowPreeditText toggle visibility of the last preedit */
    char *preedit;
    int preedit_start, preedit_length;
    bool preedit_visible;
} ime;

/* ------------------------------------------------------------------ */
/* selection                                                           */
/* ------------------------------------------------------------------ */

/* "fcitx"/"fcitx5" -> IME_FCITX, "ibus" -> IME_IBUS, other text -> IME_NONE,
 * NULL/empty -> -1 (not named) */
static int kind_from_name(const char *name)
{
    if (!name || !*name) {
        return -1;
    }
    if (SDL_strncasecmp(name, "fcitx", 5) == 0) {
        return IME_FCITX;
    }
    if (SDL_strncasecmp(name, "ibus", 4) == 0) {
        return IME_IBUS;
    }
    return IME_NONE;
}

/* the module the environment names; -1 when none is named */
static int requested_kind(void)
{
    int k = kind_from_name(SDL_getenv("SDL_IM_MODULE"));
    if (k >= 0) {
        return k; /* stock's knob: decides on its own, including "off" */
    }
    const char *xmod = SDL_getenv("XMODIFIERS");
    const char *at = xmod ? SDL_strstr(xmod, "@im=") : NULL;
    if (at) {
        k = kind_from_name(at + 4);
        if (k > IME_NONE) {
            return k;
        }
    }
    k = kind_from_name(SDL_getenv("GTK_IM_MODULE"));
    if (k > IME_NONE) {
        return k;
    }
    k = kind_from_name(SDL_getenv("QT_IM_MODULE"));
    if (k > IME_NONE) {
        return k;
    }
    return -1;
}

static bool name_has_owner(const char *name)
{
    SDLop_DBusMsg *m = SDLOP_DBus_NewMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                "org.freedesktop.DBus", "NameHasOwner", "s");
    if (!m) {
        return false;
    }
    SDLOP_DBus_PutString(m, name);
    SDLop_DBusMsg *reply = SDLOP_DBus_Call(m, 500);
    SDLOP_DBus_FreeMessage(m);
    bool owned = false;
    SDLop_DBusIter it;
    if (reply && SDLOP_DBus_IterInit(reply, &it)) {
        (void)SDLOP_DBus_IterGetBool(&it, &owned);
    }
    SDLOP_DBus_FreeMessage(reply);
    return owned;
}

/* ibus-daemon writes its address under ~/.config/ibus/bus/ while running */
static bool ibus_running(void)
{
    const char *addr = SDL_getenv("IBUS_ADDRESS");
    if (addr && *addr) {
        return true;
    }
    char dir[1024];
    const char *config = SDL_getenv("XDG_CONFIG_HOME");
    const char *home = SDL_getenv("HOME");
    if (config && *config) {
        SDL_snprintf(dir, sizeof(dir), "%s/ibus/bus", config);
    } else if (home && *home) {
        SDL_snprintf(dir, sizeof(dir), "%s/.config/ibus/bus", home);
    } else {
        return false;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/* ------------------------------------------------------------------ */
/* input context creation                                              */
/* ------------------------------------------------------------------ */

static const char *app_name(void)
{
    const char *name = SDLOP_GetExeName();
    return (name && *name) ? name : "SDL_App";
}

static char *create_fcitx_ic(void)
{
    SDLop_DBusMsg *m = SDLOP_DBus_NewMethodCall(FCITX_SERVICE, FCITX_IM_PATH, FCITX_IM_IFACE,
                                                "CreateInputContext", "a(ss)");
    if (!m) {
        return NULL;
    }
    SDLop_DBusArray a = SDLOP_DBus_OpenArray(m, '(');
    SDLOP_DBus_OpenStruct(m);
    SDLOP_DBus_PutString(m, "program");
    SDLOP_DBus_PutString(m, app_name());
    SDLOP_DBus_CloseArray(m, &a);
    SDLop_DBusMsg *reply = SDLOP_DBus_Call(m, IME_CREATE_TIMEOUT_MS);
    SDLOP_DBus_FreeMessage(m);
    char *path = NULL;
    SDLop_DBusIter it;
    const char *s = NULL;
    if (reply && SDLOP_DBus_IterInit(reply, &it) && SDLOP_DBus_IterGetString(&it, &s) && s && *s) {
        path = SDL_strdup(s);
    }
    SDLOP_DBus_FreeMessage(reply);
    return path;
}

static char *create_ibus_ic(void)
{
    SDLop_DBusMsg *m = SDLOP_DBus_NewMethodCall(IBUS_SERVICE, IBUS_PATH, IBUS_PORTAL_IFACE,
                                                "CreateInputContext", "s");
    if (!m) {
        return NULL;
    }
    SDLOP_DBus_PutString(m, "SDL3_Application");
    SDLop_DBusMsg *reply = SDLOP_DBus_Call(m, IME_CREATE_TIMEOUT_MS);
    SDLOP_DBus_FreeMessage(m);
    char *path = NULL;
    SDLop_DBusIter it;
    const char *s = NULL;
    if (reply && SDLOP_DBus_IterInit(reply, &it) && SDLOP_DBus_IterGetString(&it, &s) && s && *s) {
        path = SDL_strdup(s);
    }
    SDLOP_DBus_FreeMessage(reply);
    return path;
}

static const char *ic_service(void)
{
    return ime.kind == IME_FCITX ? FCITX_SERVICE : IBUS_SERVICE;
}

static const char *ic_iface(void)
{
    return ime.kind == IME_FCITX ? FCITX_IC_IFACE : IBUS_IC_IFACE;
}

/* fire-and-forget call on the input context (ordered with the others) */
static void ic_send(const char *member, const char *sig, const Uint32 *u32s, int n)
{
    if (!ime.ic_path) {
        return;
    }
    SDLop_DBusMsg *m = SDLOP_DBus_NewMethodCall(ic_service(), ime.ic_path, ic_iface(), member, sig);
    if (!m) {
        return;
    }
    for (int i = 0; i < n; i++) {
        SDLOP_DBus_PutU32(m, u32s[i]);
    }
    SDLOP_DBus_Send(m);
    SDLOP_DBus_FreeMessage(m);
}

static void set_capabilities(void)
{
    const char *ui = SDL_GetHint(SDL_HINT_IME_IMPLEMENTED_UI);
    const bool composition = ui && SDL_strstr(ui, "composition");
    if (ime.kind == IME_FCITX) {
        SDLop_DBusMsg *m = SDLOP_DBus_NewMethodCall(FCITX_SERVICE, ime.ic_path, FCITX_IC_IFACE, "SetCapability", "t");
        if (m) {
            SDLOP_DBus_PutU64(m, composition ? (FCITX_CAP_PREEDIT | FCITX_CAP_FORMATTED_PREEDIT) : 0);
            SDLOP_DBus_Send(m);
            SDLOP_DBus_FreeMessage(m);
        }
    } else {
        const Uint32 caps = IBUS_CAP_FOCUS | (composition ? IBUS_CAP_PREEDIT_TEXT : 0);
        ic_send("SetCapabilities", "u", &caps, 1);
    }
}

/* ------------------------------------------------------------------ */
/* signals                                                             */
/* ------------------------------------------------------------------ */

static void send_preedit(const char *text, int start, int length)
{
    SDLOP_SendEditingText(text ? text : "", start, length);
}

static void remember_preedit(const char *text, int start, int length)
{
    SDL_free(ime.preedit);
    ime.preedit = (text && *text) ? SDL_strdup(text) : NULL;
    ime.preedit_start = start;
    ime.preedit_length = length;
}

/* number of UTF-8 code points in the first `bytes` bytes */
static int utf8_chars(const char *s, size_t bytes)
{
    int n = 0;
    for (size_t i = 0; i < bytes && s[i]; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

/* Fcitx UpdateFormattedPreedit(a(si)i): segments with format flags, then
 * the cursor as a byte offset. The highlighted segment (bit 4) is the
 * selection, as stock reports it; otherwise the cursor. */
static void fcitx_preedit(SDLop_DBusMsg *msg)
{
    SDLop_DBusIter it, arr, seg;
    if (!SDLOP_DBus_IterInit(msg, &it) || SDLOP_DBus_IterType(&it) != 'a' || !SDLOP_DBus_IterRecurse(&it, &arr)) {
        return;
    }
    char *text = NULL;
    size_t len = 0;
    int chars = 0, sel_start = -1, sel_end = -1;
    while (SDLOP_DBus_IterType(&arr) == '(') {
        if (SDLOP_DBus_IterRecurse(&arr, &seg)) {
            const char *s = NULL;
            Sint32 format = 0;
            if (SDLOP_DBus_IterGetString(&seg, &s) && SDLOP_DBus_IterNext(&seg)) {
                (void)SDLOP_DBus_IterGetI32(&seg, &format);
            }
            if (s && *s) {
                const size_t n = SDL_strlen(s);
                char *grown = (char *)SDL_realloc(text, len + n + 1);
                if (!grown) {
                    break;
                }
                text = grown;
                SDL_memcpy(text + len, s, n + 1);
                const int seg_chars = utf8_chars(s, n);
                if ((format & (1 << 4)) && sel_start < 0) {
                    sel_start = chars;
                    sel_end = chars + seg_chars;
                }
                len += n;
                chars += seg_chars;
            }
        }
        if (!SDLOP_DBus_IterNext(&arr)) {
            break;
        }
    }
    if (!text || !len) {
        SDL_free(text);
        send_preedit("", 0, 0);
        return;
    }
    if (sel_start >= 0) {
        send_preedit(text, sel_start, sel_end - sel_start);
    } else {
        Sint32 cursor = -1;
        if (SDLOP_DBus_IterNext(&it)) {
            (void)SDLOP_DBus_IterGetI32(&it, &cursor);
        }
        const int pos = (cursor >= 0 && (size_t)cursor <= len) ? utf8_chars(text, (size_t)cursor) : -1;
        send_preedit(text, pos, -1);
    }
    SDL_free(text);
}

/* enter an IBus serializable: v -> (s name, a{sv} attachments, ...);
 * leaves `fields` at the first field after the attachments */
static bool ibus_enter(const SDLop_DBusIter *v, const char *name, SDLop_DBusIter *fields)
{
    SDLop_DBusIter inner;
    const char *n = NULL;
    if (SDLOP_DBus_IterType(v) != 'v' || !SDLOP_DBus_IterRecurse(v, &inner) || SDLOP_DBus_IterType(&inner) != '(' ||
        !SDLOP_DBus_IterRecurse(&inner, fields) || !SDLOP_DBus_IterGetString(fields, &n) || !n ||
        SDL_strcmp(n, name) != 0) {
        return false;
    }
    return SDLOP_DBus_IterNext(fields) && SDLOP_DBus_IterNext(fields); /* name, attachments */
}

/* IBusText: (s "IBusText", a{sv}, s text, v IBusAttrList) */
static const char *ibus_text(const SDLop_DBusIter *v, SDLop_DBusIter *attrs_out)
{
    SDLop_DBusIter f;
    const char *text = NULL;
    if (!ibus_enter(v, "IBusText", &f) || !SDLOP_DBus_IterGetString(&f, &text)) {
        return NULL;
    }
    if (attrs_out) {
        memset(attrs_out, 0, sizeof(*attrs_out));
        if (SDLOP_DBus_IterNext(&f)) {
            *attrs_out = f;
        }
    }
    return text;
}

/* IBusAttrList: (s, a{sv}, av of IBusAttribute (s, a{sv}, u type, u value,
 * u start, u end)); the background attribute (type 3) marks the selection,
 * in characters, as stock reads it */
static bool ibus_selection(const SDLop_DBusIter *attrlist, Uint32 *start, Uint32 *end)
{
    SDLop_DBusIter f, arr;
    if (!attrlist->data || !ibus_enter(attrlist, "IBusAttrList", &f) || SDLOP_DBus_IterType(&f) != 'a' ||
        !SDLOP_DBus_IterRecurse(&f, &arr)) {
        return false;
    }
    while (SDLOP_DBus_IterType(&arr) == 'v') {
        SDLop_DBusIter a;
        Uint32 type = 0, value = 0, s = 0, e = 0;
        if (ibus_enter(&arr, "IBusAttribute", &a) && SDLOP_DBus_IterGetU32(&a, &type) && SDLOP_DBus_IterNext(&a) &&
            SDLOP_DBus_IterGetU32(&a, &value) && SDLOP_DBus_IterNext(&a) && SDLOP_DBus_IterGetU32(&a, &s) &&
            SDLOP_DBus_IterNext(&a) && SDLOP_DBus_IterGetU32(&a, &e) && type == 3 && e > s) {
            *start = s;
            *end = e;
            return true;
        }
        if (!SDLOP_DBus_IterNext(&arr)) {
            break;
        }
    }
    return false;
}

static void ibus_preedit(SDLop_DBusMsg *msg)
{
    SDLop_DBusIter it, attrs;
    if (!SDLOP_DBus_IterInit(msg, &it)) {
        return;
    }
    const char *text = ibus_text(&it, &attrs);
    Uint32 cursor = 0;
    bool visible = true;
    bool has_cursor = SDLOP_DBus_IterNext(&it) && SDLOP_DBus_IterGetU32(&it, &cursor);
    if (has_cursor && SDLOP_DBus_IterNext(&it)) {
        (void)SDLOP_DBus_IterGetBool(&it, &visible);
    }
    Uint32 s = 0, e = 0;
    int start = -1, length = -1;
    if (text && *text) {
        if (ibus_selection(&attrs, &s, &e)) {
            start = (int)s;
            length = (int)(e - s);
        } else if (has_cursor) {
            start = (int)cursor;
        }
    }
    remember_preedit(text, start, length);
    ime.preedit_visible = visible;
    if (visible && text && *text) {
        send_preedit(text, start, length);
    } else {
        send_preedit("", 0, 0);
    }
}

static bool ime_signal(SDLop_DBusMsg *msg, void *userdata)
{
    (void)userdata;
    if (SDLOP_DBus_MessageType(msg) != SDLOP_DBUS_SIGNAL || !ime.ic_path) {
        return false;
    }
    const char *path = SDLOP_DBus_MessageField(msg, 'p');
    const char *iface = SDLOP_DBus_MessageField(msg, 'i');
    const char *member = SDLOP_DBus_MessageField(msg, 'm');
    if (!path || !iface || !member || SDL_strcmp(path, ime.ic_path) != 0 || SDL_strcmp(iface, ic_iface()) != 0) {
        return false;
    }
    SDLop_DBusIter it;
    if (ime.kind == IME_FCITX) {
        if (SDL_strcmp(member, "CommitString") == 0) {
            const char *text = NULL;
            if (SDLOP_DBus_IterInit(msg, &it) && SDLOP_DBus_IterGetString(&it, &text) && text && *text) {
                SDLOP_SendKeyboardText(text, 0);
            }
            return true;
        }
        if (SDL_strcmp(member, "UpdateFormattedPreedit") == 0) {
            fcitx_preedit(msg);
            return true;
        }
        return false;
    }
    if (SDL_strcmp(member, "CommitText") == 0) {
        const char *text = SDLOP_DBus_IterInit(msg, &it) ? ibus_text(&it, NULL) : NULL;
        if (text && *text) {
            SDLOP_SendKeyboardText(text, 0);
        }
        return true;
    }
    if (SDL_strcmp(member, "UpdatePreeditText") == 0) {
        ibus_preedit(msg);
        return true;
    }
    if (SDL_strcmp(member, "HidePreeditText") == 0) {
        ime.preedit_visible = false;
        send_preedit("", 0, 0);
        return true;
    }
    if (SDL_strcmp(member, "ShowPreeditText") == 0) {
        ime.preedit_visible = true;
        if (ime.preedit) {
            send_preedit(ime.preedit, ime.preedit_start, ime.preedit_length);
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* key filter (called by the core for every key while focused)          */
/* ------------------------------------------------------------------ */

static Uint32 fcitx_mods(SDL_Keymod m)
{
    Uint32 s = 0;
    if (m & SDL_KMOD_SHIFT) { s |= 1u << 0; }
    if (m & SDL_KMOD_CAPS) { s |= 1u << 1; }
    if (m & SDL_KMOD_CTRL) { s |= 1u << 2; }
    if (m & SDL_KMOD_ALT) { s |= 1u << 3; }
    if (m & SDL_KMOD_NUM) { s |= 1u << 4; }
    if (m & SDL_KMOD_GUI) { s |= 1u << 6; } /* Super */
    if (m & SDL_KMOD_MODE) { s |= 1u << 7; }
    return s;
}

static Uint32 ibus_mods(SDL_Keymod m)
{
    Uint32 s = 0;
    if (m & SDL_KMOD_SHIFT) { s |= 1u << 0; }
    if (m & SDL_KMOD_CAPS) { s |= 1u << 1; }
    if (m & SDL_KMOD_CTRL) { s |= 1u << 2; }
    if (m & SDL_KMOD_ALT) { s |= 1u << 3; } /* Mod1 */
    if (m & SDL_KMOD_NUM) { s |= 1u << 4; } /* Mod2 */
    if (m & SDL_KMOD_MODE) { s |= 1u << 7; } /* Mod5 */
    if (m & SDL_KMOD_GUI) { s |= 1u << 26; } /* Super */
    return s;
}

/* keysym: X11/xkb keysym; keycode: xkb keycode (evdev + 8) */
static bool ime_process_key(Uint32 keysym, Uint32 keycode, bool down)
{
    if (!ime.focused || !ime.ic_path || !keysym) {
        return false;
    }
    SDLop_DBusMsg *m;
    if (ime.kind == IME_FCITX) {
        m = SDLOP_DBus_NewMethodCall(FCITX_SERVICE, ime.ic_path, FCITX_IC_IFACE, "ProcessKeyEvent", "uuubu");
        if (!m) {
            return false;
        }
        SDLOP_DBus_PutU32(m, keysym);
        SDLOP_DBus_PutU32(m, keycode);
        SDLOP_DBus_PutU32(m, fcitx_mods(sdlop.modstate));
        SDLOP_DBus_PutBool(m, !down);
        SDLOP_DBus_PutU32(m, (Uint32)(SDL_GetTicks() & 0xffffffffu));
    } else {
        m = SDLOP_DBus_NewMethodCall(IBUS_SERVICE, ime.ic_path, IBUS_IC_IFACE, "ProcessKeyEvent", "uuu");
        if (!m) {
            return false;
        }
        SDLOP_DBus_PutU32(m, keysym);
        SDLOP_DBus_PutU32(m, keycode >= 8 ? keycode - 8 : 0); /* IBus wants the evdev code */
        SDLOP_DBus_PutU32(m, ibus_mods(sdlop.modstate) | (down ? 0 : IBUS_RELEASE_MASK));
    }
    SDLop_DBusMsg *reply = SDLOP_DBus_Call(m, IME_KEY_TIMEOUT_MS);
    SDLOP_DBus_FreeMessage(m);
    bool handled = false;
    SDLop_DBusIter it;
    if (reply && SDLOP_DBus_IterInit(reply, &it)) {
        (void)SDLOP_DBus_IterGetBool(&it, &handled);
    }
    SDLOP_DBus_FreeMessage(reply);
    return handled;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

static bool try_kind(IMEKind kind)
{
    char *path = kind == IME_FCITX ? create_fcitx_ic() : create_ibus_ic();
    SDL_ClearError(); /* a missing input method is not an application error */
    if (!path) {
        return false;
    }
    ime.kind = kind;
    ime.ic_path = path;
    char rule[512];
    SDL_snprintf(rule, sizeof(rule), "type='signal',interface='%s',path='%s'", ic_iface(), path);
    ime.match = SDL_strdup(rule);
    if (ime.match) {
        SDLOP_DBus_AddMatch(ime.match);
    }
    ime.handler = SDLOP_DBus_AddHandler(ime_signal, NULL);
    return true;
}

static bool ime_init(void)
{
    if (ime.tried) {
        return ime.ic_path != NULL;
    }
    ime.tried = true;
    const int requested = requested_kind();
    if (requested == IME_NONE || !SDLOP_DBus_Connect()) {
        return false;
    }
    if (requested == IME_FCITX || requested == IME_IBUS) {
        return try_kind((IMEKind)requested); /* named: D-Bus activation is fine */
    }
    /* not named: only an input method that is already running */
    if (name_has_owner(FCITX_SERVICE) && try_kind(IME_FCITX)) {
        return true;
    }
    if ((name_has_owner(IBUS_SERVICE) || ibus_running()) && try_kind(IME_IBUS)) {
        return true;
    }
    return false;
}

/* tell the input method where the text cursor is, in screen coordinates
 * as far as the window system lets us know them (a square at the cursor,
 * as stock) */
void SDLOP_IME_DBus_UpdateArea(SDL_Window *window)
{
    if (!ime.focused || !window || window != ime.focus_window) {
        return;
    }
    SDL_Rect r;
    r.x = window->x + window->text_input_rect.x + window->text_input_cursor;
    r.y = window->y + window->text_input_rect.y;
    r.w = window->text_input_rect.h;
    r.h = window->text_input_rect.h;
    if (window->text_input_rect.w <= 0 && window->text_input_rect.h <= 0 && window->text_input_rect.x == 0 &&
        window->text_input_rect.y == 0) {
        r.x = window->x; /* no area set: below the window's bottom-left corner */
        r.y = window->y + window->h;
        r.w = r.h = 0;
    }
    if (ime.sent_rect_valid && r.x == ime.sent_rect.x && r.y == ime.sent_rect.y && r.w == ime.sent_rect.w &&
        r.h == ime.sent_rect.h) {
        return;
    }
    ime.sent_rect = r;
    ime.sent_rect_valid = true;
    const Uint32 v[4] = { (Uint32)r.x, (Uint32)r.y, (Uint32)r.w, (Uint32)r.h };
    ic_send(ime.kind == IME_FCITX ? "SetCursorRect" : "SetCursorLocation", "iiii", v, 4);
}

static void focus_out(void)
{
    if (ime.focused) {
        ic_send("FocusOut", "", NULL, 0);
        ime.focused = false;
        ime.focus_window = NULL;
        sdlop.ime_process_key = NULL;
        if (ime.preedit) {
            remember_preedit(NULL, 0, 0);
            send_preedit("", 0, 0);
        }
    }
}

/* Follow the keyboard focus and its text input state. Starts the client on
 * first use. */
void SDLOP_IME_DBus_Update(void)
{
    SDL_Window *focus = sdlop.keyboard_focus;
    const bool want = focus && focus->text_input_active;
    if (!want) {
        focus_out();
        return;
    }
    if (!ime_init()) {
        return;
    }
    if (ime.focused && ime.focus_window == focus) {
        SDLOP_IME_DBus_UpdateArea(focus);
        return;
    }
    focus_out(); /* another window: the context moves with the focus */
    set_capabilities();
    ic_send("FocusIn", "", NULL, 0);
    ime.focused = true;
    ime.focus_window = focus;
    ime.sent_rect_valid = false;
    sdlop.ime_process_key = ime_process_key;
    SDLOP_IME_DBus_UpdateArea(focus);
}

/* SDL_ClearComposition: drop the preedit */
void SDLOP_IME_DBus_Reset(void)
{
    if (ime.focused) {
        ic_send("Reset", "", NULL, 0);
    }
    remember_preedit(NULL, 0, 0);
    send_preedit("", 0, 0);
}

void SDLOP_IME_DBus_WindowDestroyed(SDL_Window *window)
{
    if (ime.focus_window == window) {
        focus_out();
    }
}

void SDLOP_IME_DBus_Quit(void)
{
    if (ime.ic_path) {
        if (ime.focused) {
            ic_send("FocusOut", "", NULL, 0);
        }
        if (ime.kind == IME_FCITX) {
            ic_send("DestroyIC", "", NULL, 0);
        } else {
            SDLop_DBusMsg *m = SDLOP_DBus_NewMethodCall(IBUS_SERVICE, ime.ic_path, IBUS_SERVICE_IFACE, "Destroy", "");
            if (m) {
                SDLOP_DBus_Send(m);
                SDLOP_DBus_FreeMessage(m);
            }
        }
    }
    if (ime.match) {
        SDLOP_DBus_RemoveMatch(ime.match);
    }
    if (ime.handler) {
        SDLOP_DBus_RemoveHandler(ime_signal, NULL);
    }
    if (sdlop.ime_process_key == ime_process_key) {
        sdlop.ime_process_key = NULL;
    }
    SDL_free(ime.ic_path);
    SDL_free(ime.match);
    SDL_free(ime.preedit);
    SDL_zero(ime);
}
