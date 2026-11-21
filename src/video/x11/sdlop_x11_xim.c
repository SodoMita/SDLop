/*
  SDLop - X11 input methods (XIM), as stock SDL3's SDL_x11keyboard.c.

  XIM is an Xlib protocol, so it runs on a private Xlib connection (libX11
  dlopen-ed when text input is first started, like the GLX connection):
    - the IM is opened with the user's locale and XMODIFIERS (stock's
      setlocale dance); when no IM is running an instantiate callback picks
      it up later, and an IM that goes away is re-awaited the same way
    - one input context per window, created when text input is first
      started on it: style PreeditNothing (the IM draws its own preedit), or
      with SDL_HINT_IME_IMPLEMENTED_UI containing "composition" on-the-spot
      PreeditCallbacks -> SDL_EVENT_TEXT_EDITING (stock)
    - while the keyboard-focused window has text input active its IC has
      focus and key events from the xcb connection go through XFilterEvent
      (filtered keys produce no SDL key events - stock); the IM's replies
      arrive on the private connection, which is drained by
      SDLOP_X11_XIM_Pump(): commits (KeyPress with keycode 0) become
      SDL_EVENT_TEXT_INPUT via Xutf8LookupString, keys the IM hands back
      are processed like unfiltered ones
    - SDL_SetTextInputArea sets XNSpotLocation (cursor x, bottom of the
      area - stock); SDL_StopTextInput / SDL_ClearComposition reset the IC
  Without libX11 or an IM, text input works as before (xkbcommon text).
  SDLOP_X11_NO_XIM disables all of it.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"

#include <dlfcn.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

/* ---- the few Xlib declarations needed (no X11 headers at build time) ---- */

typedef struct XDisplay_ XDisplay_;
typedef struct XIM_ *XIM_;
typedef struct XIC_ *XIC_;
typedef char *XPointer_;
typedef unsigned long XID_;

typedef struct
{
    int type;
    unsigned long serial;
    int send_event;
    XDisplay_ *display;
    XID_ window;
    XID_ root;
    XID_ subwindow;
    unsigned long time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    unsigned int keycode;
    int same_screen;
} XKeyEvent_;

typedef struct
{
    int type;
    unsigned long serial;
    int send_event;
    XDisplay_ *display;
    XID_ window;
} XAnyEvent_;

typedef union
{
    int type;
    XAnyEvent_ xany;
    XKeyEvent_ xkey;
    long pad[24];
} XEvent_;

typedef struct
{
    unsigned char error_code_etc[64]; /* opaque (XErrorEvent) */
} XErrorEvent_;

typedef void (*XIMProc_)(XIM_, XPointer_, XPointer_);
typedef void (*XIDProc_)(XDisplay_ *, XPointer_, XPointer_);
typedef int (*XErrorHandler_)(XDisplay_ *, XErrorEvent_ *);
typedef void (*XIOErrorExitHandler_)(XDisplay_ *, void *);

typedef struct
{
    XPointer_ client_data;
    XIMProc_ callback;
} XIMCallback_;

typedef unsigned long XIMFeedback_;
typedef unsigned long XIMStyle_;

typedef struct
{
    unsigned short length;
    XIMFeedback_ *feedback;
    int encoding_is_wchar;
    union
    {
        char *multi_byte;
        void *wide_char;
    } string;
} XIMText_;

typedef struct
{
    int caret;
    int chg_first;
    int chg_length;
    XIMText_ *text;
} XIMPreeditDrawCallbackStruct_;

typedef struct
{
    int position;
    int direction; /* XIMCaretDirection */
    int style;
} XIMPreeditCaretCallbackStruct_;

typedef struct
{
    unsigned short count_styles;
    XIMStyle_ *supported_styles;
} XIMStyles_;

typedef struct
{
    short x, y;
} XPoint_;

#define XKeyPress_ 2
#define XKeyRelease_ 3
#define XIMPreeditCallbacks_ 0x0002L
#define XIMPreeditNothing_ 0x0008L
#define XIMPreeditNone_ 0x0010L
#define XIMStatusNothing_ 0x0400L
#define XIMStatusNone_ 0x0800L
#define XIMReverse_ 1L
#define XIMHighlight_ (1L << 2)
#define XIMAbsolutePosition_ 10
#define XBufferOverflow_ (-1)
#define XLookupChars_ 2
#define XLookupBoth_ 4

#define XNQueryInputStyle_ "queryInputStyle"
#define XNClientWindow_ "clientWindow"
#define XNInputStyle_ "inputStyle"
#define XNFocusWindow_ "focusWindow"
#define XNDestroyCallback_ "destroyCallback"
#define XNPreeditAttributes_ "preeditAttributes"
#define XNSpotLocation_ "spotLocation"
#define XNPreeditStartCallback_ "preeditStartCallback"
#define XNPreeditDoneCallback_ "preeditDoneCallback"
#define XNPreeditDrawCallback_ "preeditDrawCallback"
#define XNPreeditCaretCallback_ "preeditCaretCallback"

/* ---- dlopen-ed entry points ---- */

static struct
{
    void *lib;
    XDisplay_ *(*XOpenDisplay)(const char *);
    int (*XCloseDisplay)(XDisplay_ *);
    int (*XConnectionNumber)(XDisplay_ *);
    int (*XPending)(XDisplay_ *);
    int (*XNextEvent)(XDisplay_ *, XEvent_ *);
    int (*XFilterEvent)(XEvent_ *, XID_);
    int (*XFlush)(XDisplay_ *);
    char *(*XSetLocaleModifiers)(const char *);
    XIM_ (*XOpenIM)(XDisplay_ *, void *, char *, char *);
    int (*XCloseIM)(XIM_);
    char *(*XGetIMValues)(XIM_, ...);
    char *(*XSetIMValues)(XIM_, ...);
    XIC_ (*XCreateIC)(XIM_, ...);
    void (*XDestroyIC)(XIC_);
    char *(*XSetICValues)(XIC_, ...);
    void (*XSetICFocus)(XIC_);
    void (*XUnsetICFocus)(XIC_);
    int (*Xutf8LookupString)(XIC_, XKeyEvent_ *, char *, int, XID_ *, int *);
    char *(*Xutf8ResetIC)(XIC_);
    void *(*XVaCreateNestedList)(int, ...);
    int (*XFree)(void *);
    int (*XRegisterIMInstantiateCallback)(XDisplay_ *, void *, char *, char *, XIDProc_, XPointer_);
    int (*XUnregisterIMInstantiateCallback)(XDisplay_ *, void *, char *, char *, XIDProc_, XPointer_);
    XErrorHandler_ (*XSetErrorHandler)(XErrorHandler_);
    void (*XSetIOErrorExitHandler)(XDisplay_ *, XIOErrorExitHandler_, void *); /* libX11 >= 1.7, optional */
} X;

typedef struct XimWindow
{
    struct XimWindow *next;
    SDL_Window *window;
    xcb_window_t xid;
    XIC_ ic;
    bool focused;
    bool has_callbacks; /* PreeditCallbacks style */
    XPoint_ spot;
    bool spot_set;
    /* on-the-spot preedit */
    char *preedit;           /* UTF-8 */
    XIMFeedback_ *feedback;  /* one per character */
    int preedit_length;      /* characters */
    int preedit_cursor;
    bool needs_clear;        /* an editing event is showing */
} XimWindow;

static struct
{
    bool tried;     /* lazy init done (successfully or not) */
    bool dead;      /* private connection broke */
    XDisplay_ *dpy;
    XIM_ im;
    bool waiting;   /* instantiate callback registered */
    XErrorHandler_ prev_error_handler;
    XimWindow *windows;
    Uint64 serial;  /* synthetic event serials: never a real one */
} xim;

static void *xim_lib_keep; /* libX11 handle across SDL_Quit/SDL_Init */

static XimWindow *find_window(SDL_Window *window)
{
    for (XimWindow *w = xim.windows; w; w = w->next) {
        if (w->window == window) {
            return w;
        }
    }
    return NULL;
}

/* ---- error handling: never let Xlib exit() the process ---- */

static int xim_error_handler(XDisplay_ *dpy, XErrorEvent_ *ev)
{
    if (dpy == xim.dpy) {
        return 0; /* e.g. the IM's windows went away */
    }
    return xim.prev_error_handler ? xim.prev_error_handler(dpy, ev) : 0;
}

static void xim_io_exit_handler(XDisplay_ *dpy, void *data)
{
    (void)dpy;
    (void)data;
    xim.dead = true; /* returning keeps the process alive; dpy is now inert */
}

/* ---- preedit (on-the-spot) ---- */

static void clear_composition(XimWindow *w)
{
    if (w->preedit) {
        w->preedit[0] = '\0';
    }
    w->preedit_length = 0;
    w->preedit_cursor = 0;
    if (w->needs_clear) {
        w->needs_clear = false;
        if (sdlop.keyboard_focus == w->window) {
            SDLOP_SendEditingText("", 0, 0);
        }
    }
}

static void send_editing(XimWindow *w)
{
    if (w->preedit_length == 0 || !w->preedit) {
        clear_composition(w);
        return;
    }
    /* stock: the highlighted (selected) segment, else the caret */
    int start = -1, length = 0, i;
    for (i = 0; i < w->preedit_length; i++) {
        if (w->feedback[i] & (XIMReverse_ | XIMHighlight_)) {
            if (start < 0) {
                start = i;
            }
        } else if (start >= 0) {
            break;
        }
    }
    if (start >= 0) {
        length = i - start;
    } else {
        start = SDL_clamp(w->preedit_cursor, 0, w->preedit_length);
    }
    SDLOP_SendEditingText(w->preedit, start, length);
    w->needs_clear = true;
}

static size_t utf8_offset(const char *s, int chars)
{
    const char *p = s;
    for (int i = 0; i < chars && *p; i++) {
        SDL_StepUTF8(&p, NULL);
    }
    return (size_t)(p - s);
}

static int preedit_start_cb(XIC_ ic, XPointer_ client_data, XPointer_ call_data)
{
    (void)ic;
    (void)client_data;
    (void)call_data;
    return -1; /* no length limit */
}

static void preedit_done_cb(XIC_ ic, XPointer_ client_data, XPointer_ call_data)
{
    (void)ic;
    (void)call_data;
    clear_composition((XimWindow *)client_data);
}

static void preedit_draw_cb(XIC_ ic, XPointer_ client_data, XPointer_ call_data)
{
    (void)ic;
    XimWindow *w = (XimWindow *)client_data;
    const XIMPreeditDrawCallbackStruct_ *cd = (const XIMPreeditDrawCallbackStruct_ *)call_data;
    const char *old = w->preedit ? w->preedit : "";
    const int first = SDL_clamp(cd->chg_first, 0, w->preedit_length);
    const int del = SDL_clamp(cd->chg_length, 0, w->preedit_length - first);
    const char *ins = (cd->text && !cd->text->encoding_is_wchar && cd->text->string.multi_byte)
                          ? cd->text->string.multi_byte
                          : "";
    /* Xlib's length is not reliably in characters: recount (stock) */
    const int ins_len = (int)SDL_utf8strlen(ins);
    const size_t b0 = utf8_offset(old, first);
    const size_t b1 = b0 + utf8_offset(old + b0, del);
    const size_t old_bytes = strlen(old), ins_bytes = strlen(ins);

    char *text = (char *)malloc(old_bytes - (b1 - b0) + ins_bytes + 1);
    const int new_len = w->preedit_length - del + ins_len;
    XIMFeedback_ *fb = (XIMFeedback_ *)calloc((size_t)(new_len > 0 ? new_len : 1), sizeof(*fb));
    if (!text || !fb) {
        free(text);
        free(fb);
        return;
    }
    memcpy(text, old, b0);
    memcpy(text + b0, ins, ins_bytes);
    memcpy(text + b0 + ins_bytes, old + b1, old_bytes - b1 + 1);
    if (first > 0) {
        memcpy(fb, w->feedback, (size_t)first * sizeof(*fb));
    }
    if (cd->text && cd->text->feedback) {
        const int n = SDL_min(ins_len, (int)cd->text->length);
        for (int i = 0; i < n; i++) {
            fb[first + i] = cd->text->feedback[i];
        }
    }
    if (w->preedit_length - first - del > 0) {
        memcpy(fb + first + ins_len, w->feedback + first + del,
               (size_t)(w->preedit_length - first - del) * sizeof(*fb));
    }
    free(w->preedit);
    free(w->feedback);
    w->preedit = text;
    w->feedback = fb;
    w->preedit_length = new_len;
    w->preedit_cursor = cd->caret;
    if (sdlop.keyboard_focus == w->window) {
        send_editing(w);
    }
}

static void preedit_caret_cb(XIC_ ic, XPointer_ client_data, XPointer_ call_data)
{
    (void)ic;
    XimWindow *w = (XimWindow *)client_data;
    const XIMPreeditCaretCallbackStruct_ *cd = (const XIMPreeditCaretCallbackStruct_ *)call_data;
    if (cd->direction == XIMAbsolutePosition_ && cd->position != w->preedit_cursor) {
        w->preedit_cursor = cd->position;
        if (sdlop.keyboard_focus == w->window) {
            send_editing(w);
        }
    }
}

/* ---- IM / IC lifetime ---- */

static void open_im(void);

static void ic_destroyed_cb(XIM_ im, XPointer_ client_data, XPointer_ call_data)
{
    (void)im;
    (void)call_data;
    XimWindow *w = (XimWindow *)client_data;
    w->ic = NULL; /* destroyed by Xlib (IM gone): must not be freed again */
    w->focused = false;
    w->spot_set = false;
}

static void im_instantiate_cb(XDisplay_ *dpy, XPointer_ client_data, XPointer_ call_data)
{
    (void)dpy;
    (void)client_data;
    (void)call_data;
    if (!xim.im) {
        open_im();
        SDLOP_X11_XIM_Update();
    }
}

static void im_destroyed_cb(XIM_ im, XPointer_ client_data, XPointer_ call_data)
{
    (void)im;
    (void)client_data;
    (void)call_data;
    xim.im = NULL; /* its ICs are gone too (their destroy callbacks ran) */
    for (XimWindow *w = xim.windows; w; w = w->next) {
        w->ic = NULL;
        w->focused = false;
        w->spot_set = false;
        clear_composition(w);
    }
    /* wait for the IM (or another one) to come back */
    if (!xim.waiting && X.XRegisterIMInstantiateCallback(xim.dpy, NULL, NULL, NULL, im_instantiate_cb, NULL)) {
        xim.waiting = true;
    }
}

static void open_im(void)
{
    /* stock: the user's locale and XMODIFIERS (@im=...) select the IM; the
     * process locale is restored right away */
    char *prev_locale = setlocale(LC_ALL, NULL);
    prev_locale = prev_locale ? strdup(prev_locale) : NULL;
    char *prev_mods = X.XSetLocaleModifiers(NULL);
    prev_mods = prev_mods ? strdup(prev_mods) : NULL;
    (void)setlocale(LC_ALL, "");
    X.XSetLocaleModifiers("");

    XIM_ im = X.XOpenIM(xim.dpy, NULL, NULL, NULL);
    if (!im && !xim.waiting) {
        if (X.XRegisterIMInstantiateCallback(xim.dpy, NULL, NULL, NULL, im_instantiate_cb, NULL)) {
            xim.waiting = true;
        }
    }

    if (prev_locale) {
        (void)setlocale(LC_ALL, prev_locale);
    }
    X.XSetLocaleModifiers(prev_mods ? prev_mods : "");
    free(prev_locale);
    free(prev_mods);

    if (!im) {
        return;
    }
    if (xim.waiting) {
        X.XUnregisterIMInstantiateCallback(xim.dpy, NULL, NULL, NULL, im_instantiate_cb, NULL);
        xim.waiting = false;
    }
    xim.im = im;
    XIMCallback_ destroyed = { NULL, im_destroyed_cb };
    X.XSetIMValues(im, XNDestroyCallback_, &destroyed, (char *)NULL);
}

static bool style_supported(const XIMStyles_ *styles, XIMStyle_ style)
{
    if (!styles) {
        return style == (XIMPreeditNothing_ | XIMStatusNothing_); /* IM did not say: try the basic one */
    }
    for (unsigned i = 0; i < styles->count_styles; i++) {
        if (styles->supported_styles[i] == style) {
            return true;
        }
    }
    return false;
}

static void create_ic(XimWindow *w)
{
    if (!xim.im || w->ic) {
        return;
    }
    XIMStyles_ *styles = NULL;
    if (X.XGetIMValues(xim.im, XNQueryInputStyle_, &styles, (char *)NULL) != NULL) {
        styles = NULL;
    }
    const char *hint = SDL_GetHint(SDL_HINT_IME_IMPLEMENTED_UI);
    const bool composition = hint && strstr(hint, "composition");
    XIMCallback_ destroyed = { (XPointer_)w, ic_destroyed_cb };

    if (composition) {
        static const XIMStyle_ cb_styles[] = { XIMPreeditCallbacks_ | XIMStatusNothing_,
                                               XIMPreeditCallbacks_ | XIMStatusNone_ };
        for (size_t i = 0; i < SDL_arraysize(cb_styles) && !w->ic; i++) {
            if (!style_supported(styles, cb_styles[i])) {
                continue;
            }
            XIMCallback_ start = { (XPointer_)w, (XIMProc_)(void (*)(void))preedit_start_cb };
            XIMCallback_ done = { (XPointer_)w, (XIMProc_)(void (*)(void))preedit_done_cb };
            XIMCallback_ draw = { (XPointer_)w, (XIMProc_)(void (*)(void))preedit_draw_cb };
            XIMCallback_ caret = { (XPointer_)w, (XIMProc_)(void (*)(void))preedit_caret_cb };
            void *attr = X.XVaCreateNestedList(0, XNPreeditStartCallback_, &start, XNPreeditDoneCallback_, &done,
                                               XNPreeditDrawCallback_, &draw, XNPreeditCaretCallback_, &caret,
                                               (char *)NULL);
            if (attr) {
                w->ic = X.XCreateIC(xim.im, XNInputStyle_, cb_styles[i], XNClientWindow_, (XID_)w->xid,
                                    XNFocusWindow_, (XID_)w->xid, XNDestroyCallback_, &destroyed,
                                    XNPreeditAttributes_, attr, (char *)NULL);
                X.XFree(attr);
            }
            w->has_callbacks = w->ic != NULL;
        }
    }
    static const XIMStyle_ basic_styles[] = { XIMPreeditNothing_ | XIMStatusNothing_,
                                              XIMPreeditNothing_ | XIMStatusNone_,
                                              XIMPreeditNone_ | XIMStatusNone_ };
    for (size_t i = 0; i < SDL_arraysize(basic_styles) && !w->ic; i++) {
        if (style_supported(styles, basic_styles[i])) {
            w->ic = X.XCreateIC(xim.im, XNInputStyle_, basic_styles[i], XNClientWindow_, (XID_)w->xid,
                                XNFocusWindow_, (XID_)w->xid, XNDestroyCallback_, &destroyed, (char *)NULL);
        }
    }
    if (styles) {
        X.XFree(styles);
    }
    w->spot_set = false;
}

static bool load_xlib(void)
{
    if (xim_lib_keep) {
        X.lib = xim_lib_keep;
    } else {
        const char *name = SDL_GetHint("SDLOP_LIB_X11"); /* as the GL module */
        X.lib = dlopen(name && *name ? name : "libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    }
    if (!X.lib) {
        return false;
    }
    xim_lib_keep = X.lib;
#define LOAD(fn)                                       \
    do {                                               \
        *(void **)&X.fn = dlsym(X.lib, #fn);           \
        if (!X.fn) {                                   \
            memset(&X, 0, sizeof(X));                  \
            return false;                              \
        }                                              \
    } while (0)
    LOAD(XOpenDisplay);
    LOAD(XCloseDisplay);
    LOAD(XConnectionNumber);
    LOAD(XPending);
    LOAD(XNextEvent);
    LOAD(XFilterEvent);
    LOAD(XFlush);
    LOAD(XSetLocaleModifiers);
    LOAD(XOpenIM);
    LOAD(XCloseIM);
    LOAD(XGetIMValues);
    LOAD(XSetIMValues);
    LOAD(XCreateIC);
    LOAD(XDestroyIC);
    LOAD(XSetICValues);
    LOAD(XSetICFocus);
    LOAD(XUnsetICFocus);
    LOAD(Xutf8LookupString);
    LOAD(Xutf8ResetIC);
    LOAD(XVaCreateNestedList);
    LOAD(XFree);
    LOAD(XRegisterIMInstantiateCallback);
    LOAD(XUnregisterIMInstantiateCallback);
    LOAD(XSetErrorHandler);
#undef LOAD
    *(void **)&X.XSetIOErrorExitHandler = dlsym(X.lib, "XSetIOErrorExitHandler");
    return true;
}

/* lazily on the first SDL_StartTextInput: costs nothing otherwise */
static bool ensure_init(void)
{
    if (xim.tried) {
        return xim.dpy != NULL && !xim.dead;
    }
    xim.tried = true;
    if (SDLOP_HintFlag(SDLOP_HINT_X11_NO_XIM, false) || !load_xlib()) {
        return false;
    }
    xim.dpy = X.XOpenDisplay(NULL);
    if (!xim.dpy) {
        memset(&X, 0, sizeof(X));
        return false;
    }
    if (X.XSetIOErrorExitHandler) {
        X.XSetIOErrorExitHandler(xim.dpy, xim_io_exit_handler, NULL);
    }
    xim.prev_error_handler = X.XSetErrorHandler(xim_error_handler);
    open_im();
    X.XFlush(xim.dpy);
    return true;
}

/* ---- driver entry points ---- */

void SDLOP_X11_XIM_Update(void)
{
    if (!xim.dpy || xim.dead) {
        return;
    }
    SDL_Window *focus = sdlop.keyboard_focus;
    for (XimWindow *w = xim.windows; w; w = w->next) {
        const bool want = w->window == focus && focus->text_input_active;
        if (want && !w->ic) {
            create_ic(w);
        }
        if (!w->ic) {
            w->focused = false;
            continue;
        }
        if (want && !w->focused) {
            X.XSetICFocus(w->ic);
            w->focused = true;
        } else if (!want && w->focused) {
            X.XUnsetICFocus(w->ic);
            w->focused = false;
        }
        if (want) {
            SDLOP_X11_XIM_UpdateArea(w->window);
        }
    }
    X.XFlush(xim.dpy);
}

static void reset_ic(XimWindow *w)
{
    if (w->ic) {
        /* drops pending dead keys / the IM's composition (stock) */
        char *contents = X.Xutf8ResetIC(w->ic);
        if (contents) {
            X.XFree(contents);
        }
    }
    clear_composition(w);
}

bool SDLOP_X11_XIM_Start(SDL_Window *window)
{
    if (!ensure_init()) {
        return true; /* plain xkbcommon text input */
    }
    XimWindow *w = find_window(window);
    if (!w) {
        w = (XimWindow *)calloc(1, sizeof(*w));
        if (!w) {
            return true;
        }
        w->window = window;
        w->xid = SDLOP_X11_GetWindowXID(window);
        w->next = xim.windows;
        xim.windows = w;
    }
    reset_ic(w);
    SDLOP_X11_XIM_Update();
    return true;
}

bool SDLOP_X11_XIM_Stop(SDL_Window *window)
{
    XimWindow *w = find_window(window);
    if (w && xim.dpy && !xim.dead) {
        reset_ic(w);
        SDLOP_X11_XIM_Update();
    }
    return true;
}

bool SDLOP_X11_XIM_ClearComposition(SDL_Window *window)
{
    XimWindow *w = find_window(window);
    if (w && xim.dpy && !xim.dead) {
        reset_ic(w);
        X.XFlush(xim.dpy);
    }
    return true;
}

bool SDLOP_X11_XIM_UpdateArea(SDL_Window *window)
{
    XimWindow *w = find_window(window);
    if (!w || !w->ic || xim.dead) {
        return true;
    }
    XPoint_ spot;
    spot.x = (short)(window->text_input_rect.x + window->text_input_cursor);
    spot.y = (short)(window->text_input_rect.y + window->text_input_rect.h);
    if (!w->spot_set || spot.x != w->spot.x || spot.y != w->spot.y) {
        void *attr = X.XVaCreateNestedList(0, XNSpotLocation_, &spot, (char *)NULL);
        if (attr) {
            X.XSetICValues(w->ic, XNPreeditAttributes_, attr, (char *)NULL);
            X.XFree(attr);
        }
        w->spot = spot;
        w->spot_set = true;
        X.XFlush(xim.dpy);
    }
    return true;
}

static XimWindow *active_window_for(xcb_window_t xid)
{
    for (XimWindow *w = xim.windows; w; w = w->next) {
        if (w->xid == xid) {
            return (w->ic && w->focused && w->window->text_input_active) ? w : NULL;
        }
    }
    return NULL;
}

bool SDLOP_X11_XIM_FilterKey(const xcb_key_press_event_t *kev)
{
    if (!xim.dpy || xim.dead || !xim.im) {
        return false;
    }
    XimWindow *w = active_window_for(kev->event);
    if (!w) {
        return false;
    }
    XEvent_ ev;
    memset(&ev, 0, sizeof(ev));
    ev.xkey.type = (kev->response_type & 0x7f) == XCB_KEY_PRESS ? XKeyPress_ : XKeyRelease_;
    /* serials above anything the private connection can reach, so Xlib
     * never mistakes the event for one it fabricated itself */
    ev.xkey.serial = (unsigned long)((1ull << 40) + ++xim.serial);
    ev.xkey.send_event = (kev->response_type & 0x80) != 0;
    ev.xkey.display = xim.dpy;
    ev.xkey.window = kev->event;
    ev.xkey.root = kev->root;
    ev.xkey.subwindow = kev->child;
    ev.xkey.time = kev->time;
    ev.xkey.x = kev->event_x;
    ev.xkey.y = kev->event_y;
    ev.xkey.x_root = kev->root_x;
    ev.xkey.y_root = kev->root_y;
    ev.xkey.state = kev->state;
    ev.xkey.keycode = kev->detail;
    ev.xkey.same_screen = kev->same_screen;
    const bool filtered = X.XFilterEvent(&ev, 0) != 0;
    if (filtered) {
        X.XFlush(xim.dpy);
    }
    return filtered;
}

static void handle_commit(XimWindow *w, XEvent_ *ev)
{
    char buf[256];
    char *text = buf;
    XID_ keysym = 0;
    int status = 0;
    int n = X.Xutf8LookupString(w->ic, &ev->xkey, buf, (int)sizeof(buf) - 1, &keysym, &status);
    if (status == XBufferOverflow_ && n > 0) {
        text = (char *)malloc((size_t)n + 1);
        if (!text) {
            return;
        }
        n = X.Xutf8LookupString(w->ic, &ev->xkey, text, n, &keysym, &status);
    }
    if ((status == XLookupChars_ || status == XLookupBoth_) && n > 0) {
        text[n] = '\0';
        clear_composition(w); /* stock: a commit ends the composition */
        SDLOP_SendKeyboardText(text, 0);
    }
    if (text != buf) {
        free(text);
    }
}

void SDLOP_X11_XIM_Pump(void)
{
    if (!xim.dpy || xim.dead) {
        return;
    }
    while (!xim.dead && X.XPending(xim.dpy) > 0) {
        XEvent_ ev;
        X.XNextEvent(xim.dpy, &ev);
        if (X.XFilterEvent(&ev, 0)) {
            continue; /* XIM transport traffic, callbacks already ran */
        }
        if (ev.type != XKeyPress_ && ev.type != XKeyRelease_) {
            continue;
        }
        XimWindow *w = NULL;
        for (XimWindow *it = xim.windows; it; it = it->next) {
            if (it->xid == ev.xkey.window) {
                w = it;
            }
        }
        if (!w) {
            continue;
        }
        if (ev.type == XKeyPress_ && ev.xkey.keycode == 0) {
            if (w->ic) {
                handle_commit(w, &ev); /* Xlib's carrier for XIM_COMMIT */
            }
            continue;
        }
        /* a key the IM handed back: process it as if never filtered */
        xcb_key_press_event_t kev;
        memset(&kev, 0, sizeof(kev));
        kev.response_type = ev.type == XKeyPress_ ? XCB_KEY_PRESS : XCB_KEY_RELEASE;
        kev.detail = (xcb_keycode_t)ev.xkey.keycode;
        kev.time = (xcb_timestamp_t)ev.xkey.time;
        kev.root = (xcb_window_t)ev.xkey.root;
        kev.event = (xcb_window_t)ev.xkey.window;
        kev.child = (xcb_window_t)ev.xkey.subwindow;
        kev.root_x = (int16_t)ev.xkey.x_root;
        kev.root_y = (int16_t)ev.xkey.y_root;
        kev.event_x = (int16_t)ev.xkey.x;
        kev.event_y = (int16_t)ev.xkey.y;
        kev.state = (uint16_t)ev.xkey.state;
        kev.same_screen = (uint8_t)ev.xkey.same_screen;
        SDLOP_X11_HandleKeyFromIM(&kev);
    }
    if (!xim.dead) {
        X.XFlush(xim.dpy);
    }
}

int SDLOP_X11_XIM_GetFD(void)
{
    return (xim.dpy && !xim.dead) ? X.XConnectionNumber(xim.dpy) : -1;
}

void SDLOP_X11_XIM_WindowDestroyed(SDL_Window *window)
{
    for (XimWindow **pw = &xim.windows; *pw; pw = &(*pw)->next) {
        XimWindow *w = *pw;
        if (w->window == window) {
            if (w->ic && !xim.dead) {
                X.XDestroyIC(w->ic);
            }
            *pw = w->next;
            free(w->preedit);
            free(w->feedback);
            free(w);
            return;
        }
    }
}

void SDLOP_X11_XIM_Quit(void)
{
    while (xim.windows) {
        SDLOP_X11_XIM_WindowDestroyed(xim.windows->window);
    }
    if (xim.dpy) {
        if (!xim.dead) {
            if (xim.waiting) {
                X.XUnregisterIMInstantiateCallback(xim.dpy, NULL, NULL, NULL, im_instantiate_cb, NULL);
            }
            if (xim.im) {
                X.XCloseIM(xim.im);
            }
        }
        X.XCloseDisplay(xim.dpy);
        /* restore the previous handler unless someone replaced ours since */
        XErrorHandler_ cur = X.XSetErrorHandler(xim.prev_error_handler);
        if (cur != xim_error_handler) {
            X.XSetErrorHandler(cur);
        }
    }
    /* libX11 stays loaded (X.lib kept for the next init): its locale and IM
     * caches live in its globals, and unmapping them would only turn them
     * into leaks */
    void *lib = X.lib;
    memset(&X, 0, sizeof(X));
    memset(&xim, 0, sizeof(xim));
    xim_lib_keep = lib;
}
