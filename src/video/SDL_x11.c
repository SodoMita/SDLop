/*
  SDLop -- the X11 video driver.

  What this backend is for: a Linux session where the asynchronous evdev reader
  cannot be used (a sandbox, a Flatpak, XWayland, a remote X session) still has to
  get a window, a keyboard and a mouse. X11 is also the second desktop backend, so
  it is what proves the internal contract really is one (see ARCHITECTURE.md).

  Input: when the evdev worker runs it stays the input source - it reads the
  hardware directly, with kernel timestamps - and this driver only delivers the
  events that belong to the *window* (configure, focus, expose, close, hotplug).
  Without it, the X event stream is translated instead: that is the documented
  fallback, and it uses the very same record -> SDL_Event path, so scancodes,
  keycodes and text stay identical to the Wayland backend.

  Deliberately not implemented (out of scope, not forgotten):
    * XIM/IME: text is synthesized from the XKB layout, so there are no preedit
      events (SDL_StartTextInput() is still honoured for text input as a whole);
    * clipboard and drag-and-drop (the whole clipboard API is out of scope);
    * GL goes through EGL, not GLX - `sdlop_egl_x11_visual()` picks the visual an
      SDL_WINDOW_OPENGL window must be created with, so the X window matches the
      EGL config;
    * custom cursors are 1-bit (XCreatePixmapCursor): X11's core protocol has no
      ARGB cursors without Xcursor, which is not linked.
*/

#include "../sdlop_internal.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#ifdef SDLOP_HAVE_XSHAPE
#include <X11/extensions/shape.h>
#endif

#ifdef SDLOP_HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif
#ifdef SDLOP_HAVE_XINPUT2
#include <X11/extensions/XInput2.h>
#endif
#ifdef SDLOP_HAVE_XSHM
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */
/* Driver state                                                              */
/* ------------------------------------------------------------------------- */

static Display *sdlop_x11_display;
static int sdlop_x11_screen;
static Window sdlop_x11_root;
static Visual *sdlop_x11_visual;             /* the screen's default visual */
static int sdlop_x11_depth;

static Atom atom_wm_protocols, atom_wm_delete_window, atom_wm_state, atom_wm_change_state;
static Atom atom_net_wm_name, atom_net_wm_state, atom_net_wm_state_fullscreen;
static Atom atom_net_wm_state_maximized_vert, atom_net_wm_state_maximized_horz;
static Atom atom_net_wm_state_hidden, atom_net_wm_state_above;
static Atom atom_net_wm_state_demands_attention, atom_net_wm_window_opacity;
static Atom atom_net_wm_icon, atom_net_active_window, atom_motif_wm_hints, atom_utf8_string;
/* Root window properties the X server (or the desktop's layout tool) rewrites
   when the keyboard layout changes; see the PropertyNotify case below. */
static Atom atom_xkb_rules_names, atom_xklavier_state;

static bool sdlop_x11_xrandr;
static bool sdlop_x11_xinput2;
static int sdlop_x11_xinput2_opcode;
static bool sdlop_x11_shm;
static bool sdlop_x11_shm_checked;
static Bool sdlop_x11_detectable_repeat;

static Cursor sdlop_x11_blank_cursor;

/* Repeat detection: with XKB's detectable auto-repeat the server sends a
   KeyPress (and no KeyRelease) for every repeat of a held key. */
static unsigned int sdlop_x11_last_keycode;
static bool sdlop_x11_last_key_down;

/* Where the pointer was last seen, in window coordinates: the deltas of an
   absolute move and the first delta after a warp are derived from it. */
static float sdlop_x11_mouse_x, sdlop_x11_mouse_y;

/* The backend state a window keeps: SDLOP_X11WindowState, declared in
   sdlop_internal.h as the type of the x11 member of the driver union. It used to
   be re-declared here, and the copy drifted from the union (it kept three fields
   the union had dropped), so fields past the drift were read and written at the
   wrong offsets. Never redeclare it. */
#define SDLOP_X11_STATE(window) (&(window)->driver.x11)

/* ------------------------------------------------------------------------- */
/* Windows: lookup, visual selection                                         */
/* ------------------------------------------------------------------------- */

typedef struct SDLOP_X11Find
{
    unsigned long xid;
    SDL_Window *found;
} SDLOP_X11Find;

static void sdlop_x11_find_window(SDL_Window *window, void *userdata)
{
    SDLOP_X11Find *find = (SDLOP_X11Find *)userdata;

    if (!find->found && window->driver.x11.window == find->xid) {
        find->found = window;
    }
}

static SDL_Window *sdlop_x11_window_from_xid(unsigned long xid)
{
    SDLOP_X11Find find;

    if (!xid) {
        return NULL;
    }
    find.xid = xid;
    find.found = NULL;
    SDLOP_WindowIterate(sdlop_x11_find_window, &find);
    return find.found;
}

/* Ask the GL driver which visual an SDL_WINDOW_OPENGL window has to be created
   with: on X11 the window *is* the drawable, so its visual has to match the EGL
   config, or eglCreateWindowSurface() fails. */
static Visual *sdlop_x11_gl_visual(int *depth_out)
{
#if defined(SDLOP_HAVE_EGL)
    unsigned long visual_id = 0;
    int depth = 0;

    if (sdlop_egl_x11_visual(&visual_id, &depth) && visual_id) {
        XVisualInfo template;
        XVisualInfo *info;
        int n = 0;

        template.visualid = (VisualID)visual_id;
        info = XGetVisualInfo(sdlop_x11_display, VisualIDMask, &template, &n);
        if (info && n > 0) {
            Visual *visual = info->visual;
            *depth_out = info->depth;
            XFree(info);
            return visual;
        }
        if (info) {
            XFree(info);
        }
    }
#endif
    *depth_out = sdlop_x11_depth;
    return sdlop_x11_visual;
}

/* ------------------------------------------------------------------------- */
/* Displays: one per RandR output                                            */
/*                                                                           */
/* Like the Wayland backend, an output gets a display ID once and keeps it: a */
/* window that outlives an unplugged monitor must not find itself on a        */
/* different display afterwards. Slots stay put for the same reason.          */
/* ------------------------------------------------------------------------- */

#define SDLOP_X11_MAX_OUTPUTS 16

typedef struct SDLOP_X11Output
{
    unsigned long output;      /* RROutput */
    unsigned long crtc;        /* RRCrtc, None when the output is off */
    SDL_DisplayID id;
} SDLOP_X11Output;

static SDLOP_X11Output sdlop_x11_outputs[SDLOP_X11_MAX_OUTPUTS];
static int sdlop_x11_num_outputs;
static SDL_DisplayID sdlop_x11_next_display_id = 1;

#ifdef SDLOP_HAVE_XRANDR
static int sdlop_x11_mhz_from_mode(const XRRModeInfo *mode)
{
    long long total = (long long)mode->hTotal * (long long)mode->vTotal;

    if (!total || !mode->dotClock) {
        return 0;                      /* Xvfb and friends report no clock */
    }
    return (int)((mode->dotClock * 1000LL) / total);
}

static XRRModeInfo *sdlop_x11_find_mode(XRRScreenResources *res, RRMode mode)
{
    int i;

    for (i = 0; i < res->nmode; i++) {
        if (res->modes[i].id == mode) {
            return &res->modes[i];
        }
    }
    return NULL;
}

static SDLOP_X11Output *sdlop_x11_output_slot(unsigned long output, bool create)
{
    int i;

    for (i = 0; i < sdlop_x11_num_outputs; i++) {
        if (sdlop_x11_outputs[i].output == output) {
            return &sdlop_x11_outputs[i];
        }
    }
    if (!create || sdlop_x11_num_outputs >= SDLOP_X11_MAX_OUTPUTS) {
        return NULL;
    }
    memset(&sdlop_x11_outputs[sdlop_x11_num_outputs], 0, sizeof(sdlop_x11_outputs[0]));
    sdlop_x11_outputs[sdlop_x11_num_outputs].output = output;
    return &sdlop_x11_outputs[sdlop_x11_num_outputs++];
}

static void sdlop_x11_fill_mode(SDL_DisplayMode *mode, SDL_DisplayID id, int w, int h, int mhz)
{
    memset(mode, 0, sizeof(*mode));
    mode->displayID = id;
    mode->format = SDL_PIXELFORMAT_XRGB8888;
    mode->w = w;
    mode->h = h;
    mode->refresh_rate = (float)mhz / 1000.0f;
    mode->refresh_rate_numerator = mhz;
    mode->refresh_rate_denominator = 1000;
    mode->pixel_density = 1.0f;
}

/* Re-read the RandR tree and reconcile it with the display list. */
static void sdlop_x11_refresh_displays(void)
{
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(sdlop_x11_display, sdlop_x11_root);
    bool seen[SDLOP_X11_MAX_OUTPUTS];
    int i;

    if (!res) {
        return;
    }
    memset(seen, 0, sizeof(seen));

    for (i = 0; i < res->noutput; i++) {
        XRROutputInfo *out = XRRGetOutputInfo(sdlop_x11_display, res, res->outputs[i]);
        XRRCrtcInfo *crtc;
        SDLOP_X11Output *slot;
        struct SDLOP_Display *display;
        SDL_DisplayID id;
        SDL_Rect bounds;

        if (!out) {
            continue;
        }
        if (out->connection != RR_Connected || out->crtc == None) {
            XRRFreeOutputInfo(out);
            continue;
        }
        crtc = XRRGetCrtcInfo(sdlop_x11_display, res, out->crtc);
        if (!crtc) {
            XRRFreeOutputInfo(out);
            continue;
        }
        slot = sdlop_x11_output_slot(res->outputs[i], true);
        if (!slot) {
            XRRFreeCrtcInfo(crtc);
            XRRFreeOutputInfo(out);
            continue;
        }
        if (!slot->id) {
            slot->id = sdlop_x11_next_display_id++;
        }
        slot->crtc = out->crtc;
        for (int k = 0; k < sdlop_x11_num_outputs; k++) {
            if (&sdlop_x11_outputs[k] == slot) {
                seen[k] = true;
            }
        }
        id = slot->id;

        display = SDLOP_GetDisplay(id);
        if (!display) {
            display = SDLOP_AddDisplay(id);
        }
        if (display) {
            bounds.x = (int)crtc->x;
            bounds.y = (int)crtc->y;
            bounds.w = (int)crtc->width;
            bounds.h = (int)crtc->height;

            if (!display->name) {
                /* The RandR output name ("HDMI-1", "eDP-1") - what SDL3 reports. */
                SDLOP_SetDisplayName(display, out->name);
            }
            SDLOP_SetDisplayBounds(display, &bounds);
            display->usable = bounds;
            SDLOP_SetDisplayScale(display, 1.0f);

            {
                SDL_DisplayMode current;
                int mhz = 0;

                if (crtc->mode) {
                    XRRModeInfo *mode = sdlop_x11_find_mode(res, crtc->mode);
                    if (mode) {
                        mhz = sdlop_x11_mhz_from_mode(mode);
                    }
                }
                sdlop_x11_fill_mode(&current, id, (int)crtc->width, (int)crtc->height, mhz);
                SDLOP_ClearDisplayModes(display);
                SDLOP_AddDisplayMode(display, &current);
                if (crtc->mode) {
                    /* Everything this CRTC can show, so the fullscreen mode list
                       has something in it. */
                    for (int m = 0; m < out->nmode; m++) {
                        XRRModeInfo *mi = sdlop_x11_find_mode(res, out->modes[m]);
                        SDL_DisplayMode candidate;
                        unsigned int rotation = crtc->rotation;

                        if (!mi || mi->id == crtc->mode) {
                            continue;
                        }
                        if (rotation == RR_Rotate_90 || rotation == RR_Rotate_270) {
                            sdlop_x11_fill_mode(&candidate, id, (int)mi->height, (int)mi->width,
                                                sdlop_x11_mhz_from_mode(mi));
                        } else {
                            sdlop_x11_fill_mode(&candidate, id, (int)mi->width, (int)mi->height,
                                                sdlop_x11_mhz_from_mode(mi));
                        }
                        SDLOP_AddDisplayMode(display, &candidate);
                    }
                }
                SDLOP_SetDisplayCurrentMode(display, &current);
            }
        }
        XRRFreeCrtcInfo(crtc);
        XRRFreeOutputInfo(out);
    }

    /* Outputs that are gone: unplugged, or their CRTC was switched off. */
    for (i = 0; i < sdlop_x11_num_outputs; i++) {
        if (!seen[i] && sdlop_x11_outputs[i].id) {
            SDLOP_RemoveDisplay(sdlop_x11_outputs[i].id);
            sdlop_x11_outputs[i].id = 0;
            sdlop_x11_outputs[i].crtc = None;
        }
    }
    XRRFreeScreenResources(res);
}
#endif /* SDLOP_HAVE_XRANDR */

/* A single display covering the whole screen: no RandR, or RandR without a
   connected output (a bare X server). */
static void sdlop_x11_setup_screen_display(void)
{
    SDL_DisplayID id = 1;
    struct SDLOP_Display *display = SDLOP_GetDisplay(id);
    SDL_DisplayMode mode;
    int w = DisplayWidth(sdlop_x11_display, sdlop_x11_screen);
    int h = DisplayHeight(sdlop_x11_display, sdlop_x11_screen);

    if (!display) {
        display = SDLOP_AddDisplay(id);
    }
    if (!display) {
        return;
    }
    SDLOP_SetDisplayName(display, "screen");
    SDLOP_SetDisplayBounds(display, &(SDL_Rect){ 0, 0, w, h });
    display->usable = display->bounds;
    SDLOP_SetDisplayScale(display, 1.0f);
    sdlop_x11_fill_mode(&mode, id, w, h, 0);
    SDLOP_ClearDisplayModes(display);
    SDLOP_AddDisplayMode(display, &mode);
    SDLOP_SetDisplayCurrentMode(display, &mode);
}

static void sdlop_x11_select_global_events(void)
{
    /* Layout switches on a server that does not send this client the core
       MappingNotify show up as a change of these root properties, so the root
       window's property changes are wanted as well as the per-window ones. */
    XSelectInput(sdlop_x11_display, sdlop_x11_root, PropertyChangeMask);
#ifdef SDLOP_HAVE_XRANDR
    if (sdlop_x11_xrandr) {
        XRRSelectInput(sdlop_x11_display, sdlop_x11_root,
                       RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
    }
#endif
#ifdef SDLOP_HAVE_XINPUT2
    if (sdlop_x11_xinput2) {
        /* Raw motion is what a locked pointer needs: it is unaccelerated and
           unaffected by the pointer being warped back to the window centre. */
        XIEventMask mask;
        unsigned char bits[XIMaskLen(XI_RawMotion)];
        XISetMask(bits, XI_RawMotion);
        mask.deviceid = XIAllMasterDevices;
        mask.mask_len = sizeof(bits);
        mask.mask = bits;
        XISelectEvents(sdlop_x11_display, sdlop_x11_root, &mask, 1);
    }
#endif
}

/* ------------------------------------------------------------------------- */
/* Init / quit                                                               */
/* ------------------------------------------------------------------------- */

static bool sdlop_x11_init(void)
{
    const char *display_name = SDL_getenv("DISPLAY");

    /* SDL_PumpEvents() may be called from a thread that did not create the
       window, and Xlib needs to know that before the first request. */
    XInitThreads();

    sdlop_x11_display = XOpenDisplay(display_name);
    if (!sdlop_x11_display) {
        return SDL_SetError("Couldn't open the X display '%s'",
                            display_name ? display_name : "(DISPLAY is not set)");
    }
    sdlop_x11_screen = DefaultScreen(sdlop_x11_display);
    sdlop_x11_root = RootWindow(sdlop_x11_display, sdlop_x11_screen);
    sdlop_x11_visual = DefaultVisual(sdlop_x11_display, sdlop_x11_screen);
    sdlop_x11_depth = DefaultDepth(sdlop_x11_display, sdlop_x11_screen);

    atom_wm_protocols = XInternAtom(sdlop_x11_display, "WM_PROTOCOLS", False);
    atom_wm_delete_window = XInternAtom(sdlop_x11_display, "WM_DELETE_WINDOW", False);
    atom_wm_state = XInternAtom(sdlop_x11_display, "WM_STATE", False);
    atom_wm_change_state = XInternAtom(sdlop_x11_display, "WM_CHANGE_STATE", False);
    atom_net_wm_name = XInternAtom(sdlop_x11_display, "_NET_WM_NAME", False);
    atom_net_wm_state = XInternAtom(sdlop_x11_display, "_NET_WM_STATE", False);
    atom_net_wm_state_fullscreen = XInternAtom(sdlop_x11_display, "_NET_WM_STATE_FULLSCREEN", False);
    atom_net_wm_state_maximized_vert =
        XInternAtom(sdlop_x11_display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    atom_net_wm_state_maximized_horz =
        XInternAtom(sdlop_x11_display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    atom_net_wm_state_hidden = XInternAtom(sdlop_x11_display, "_NET_WM_STATE_HIDDEN", False);
    atom_net_wm_state_above = XInternAtom(sdlop_x11_display, "_NET_WM_STATE_ABOVE", False);
    atom_net_wm_state_demands_attention =
        XInternAtom(sdlop_x11_display, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
    atom_net_wm_window_opacity = XInternAtom(sdlop_x11_display, "_NET_WM_WINDOW_OPACITY", False);
    atom_net_wm_icon = XInternAtom(sdlop_x11_display, "_NET_WM_ICON", False);
    atom_net_active_window = XInternAtom(sdlop_x11_display, "_NET_ACTIVE_WINDOW", False);
    atom_motif_wm_hints = XInternAtom(sdlop_x11_display, "_MOTIF_WM_HINTS", False);
    atom_utf8_string = XInternAtom(sdlop_x11_display, "UTF8_STRING", False);
    atom_xkb_rules_names = XInternAtom(sdlop_x11_display, "_XKB_RULES_NAMES", False);
    /* The property stock SDL3 watches as its "this server does not send
       MappingNotify" fallback (SDL_x11events.c, the XKLAVIER_STATE hack). */
    atom_xklavier_state = XInternAtom(sdlop_x11_display, "XKLAVIER_STATE", False);

    XkbSetDetectableAutoRepeat(sdlop_x11_display, True, &sdlop_x11_detectable_repeat);

    {
        XColor dummy;
        char empty[4] = { 0, 0, 0, 0 };
        Pixmap blank = XCreateBitmapFromData(sdlop_x11_display, sdlop_x11_root, empty, 1, 1);
        memset(&dummy, 0, sizeof(dummy));
        sdlop_x11_blank_cursor =
            XCreatePixmapCursor(sdlop_x11_display, blank, blank, &dummy, &dummy, 0, 0);
        XFreePixmap(sdlop_x11_display, blank);
    }

#ifdef SDLOP_HAVE_XRANDR
    {
        int major = 0, minor = 0, error = 0, event = 0;
        sdlop_x11_xrandr = XRRQueryExtension(sdlop_x11_display, &event, &error) &&
                           XRRQueryVersion(sdlop_x11_display, &major, &minor);
        if (sdlop_x11_xrandr) {
            SDLOP_LogInfo("sdlop: X11 driver using RandR %d.%d", major, minor);
        }
    }
#endif
#ifdef SDLOP_HAVE_XINPUT2
    {
        int first_event = 0, first_error = 0, major = 2, minor = 0;
        if (XQueryExtension(sdlop_x11_display, "XInputExtension", &sdlop_x11_xinput2_opcode,
                            &first_event, &first_error) &&
            XIQueryVersion(sdlop_x11_display, &major, &minor) == Success) {
            sdlop_x11_xinput2 = true;
        }
    }
#endif

#ifdef SDLOP_HAVE_XRANDR
    if (sdlop_x11_xrandr) {
        bool any;

        sdlop_x11_refresh_displays();
        any = false;
        for (int i = 0; i < sdlop_x11_num_outputs; i++) {
            if (sdlop_x11_outputs[i].id) {
                any = true;
            }
        }
        if (!any) {
            /* RandR is there but nothing is connected: the screen is still a
               display, and an application needs somewhere to put a window. */
            sdlop_x11_setup_screen_display();
        }
    } else
#endif
    {
        sdlop_x11_setup_screen_display();
    }

#ifdef SDLOP_HAVE_XKBCOMMON
    /* Have a layout before the first key can arrive: the override, the local
       configuration, then the X server's own keymap on top of it. */
    SDLOP_XKBInit();
#ifdef SDLOP_HAVE_XKBCOMMON_X11
    if (!SDLOP_XKBOverrideActive()) {
        int device_id = SDLOP_XKBX11DeviceID(sdlop_x11_display);
        if (device_id < 0 || !SDLOP_XKBLoadFromX11(sdlop_x11_display, device_id, false)) {
            SDLOP_LogWarn("sdlop: could not read the X server keymap; using the local layout");
        }
    }
#endif
#endif

    sdlop_x11_select_global_events();
    XSync(sdlop_x11_display, False);
    return true;
}

static void sdlop_x11_quit(void)
{
    if (sdlop_x11_blank_cursor) {
        XFreeCursor(sdlop_x11_display, sdlop_x11_blank_cursor);
        sdlop_x11_blank_cursor = 0;
    }
    sdlop_x11_num_outputs = 0;
#ifdef SDLOP_HAVE_XKBCOMMON
    SDLOP_SetKeyLayout(NULL);
    SDLOP_XKBQuit();
#endif
    if (sdlop_x11_display) {
        XSync(sdlop_x11_display, False);
        XCloseDisplay(sdlop_x11_display);
        sdlop_x11_display = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* Window management                                                         */
/* ------------------------------------------------------------------------- */

static void sdlop_x11_request_focus(SDL_Window *window);
static void sdlop_x11_pump_events(void);

static unsigned long sdlop_x11_event_mask(void)
{
    /* KeymapStateMask is what makes the server send this client KeymapNotify;
       stock SDL3 selects it too, because its own KeymapNotify handler is how it
       notices a *group* switch (the XKB group changed while the keys it saw did
       not say so). SDLop asks for the same mask so the server's view of this
       client is the same one stock has, but it answers KeymapNotify in the
       property/MappingNotify paths instead: its layout state follows the keys the
       input path delivers, which is what makes a group switch the evdev worker
       sees work as well. */
    return StructureNotifyMask | ExposureMask | FocusChangeMask | EnterWindowMask |
           LeaveWindowMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
           ButtonReleaseMask | PointerMotionMask | PropertyChangeMask |
           VisibilityChangeMask | KeymapStateMask;
}

static void sdlop_x11_set_motif_hints(SDL_Window *window, bool decorated)
{
    /* _MOTIF_WM_HINTS with MWM_HINTS_DECORATIONS: the portable way to ask for a
       borderless window. It only takes effect after an unmap/map cycle. */
    struct
    {
        unsigned long flags;
        unsigned long functions;
        unsigned long decorations;
        long input_mode;
        unsigned long status;
    } hints;

    memset(&hints, 0, sizeof(hints));
    hints.flags = 2;
    hints.decorations = decorated ? 1 : 0;
    XChangeProperty(sdlop_x11_display, (Window)window->driver.x11.window, atom_motif_wm_hints,
                    atom_motif_wm_hints, 32, PropModeReplace, (unsigned char *)&hints, 5);
}

static void sdlop_x11_set_size_hints(SDL_Window *window)
{
    XSizeHints hints;
    long supplied = 0;

    memset(&hints, 0, sizeof(hints));
    if (!XGetWMNormalHints(sdlop_x11_display, (Window)window->driver.x11.window, &hints, &supplied)) {
        hints.flags = 0;
    }
    if (window->flags & SDL_WINDOW_RESIZABLE) {
        hints.flags &= (long)~(PMinSize | PMaxSize);
    } else {
        hints.flags |= PMinSize | PMaxSize;
        hints.min_width = window->w;
        hints.min_height = window->h;
        hints.max_width = window->w;
        hints.max_height = window->h;
    }
    XSetWMNormalHints(sdlop_x11_display, (Window)window->driver.x11.window, &hints);
}

static void sdlop_x11_set_title(SDL_Window *window, const char *title)
{
    Window w = (Window)window->driver.x11.window;

    if (!title) {
        title = "";
    }
    XStoreName(sdlop_x11_display, w, title);
    /* _NET_WM_NAME is the UTF-8 one; WM_NAME stays for window managers that
       predate it. */
    XChangeProperty(sdlop_x11_display, w, atom_net_wm_name, atom_utf8_string, 8, PropModeReplace,
                    (const unsigned char *)title, (int)strlen(title));
}

static bool sdlop_x11_create_window(SDL_Window *window)
{
    XSetWindowAttributes attrs;
    XClassHint class_hint;
    Visual *visual;
    int depth;
    unsigned long mask;
    Window w;

    if (!sdlop_x11_display) {
        return SDL_SetError("The X11 driver is not initialized");
    }
    depth = sdlop_x11_depth;
    visual = sdlop_x11_visual;
    if (window->flags & SDL_WINDOW_OPENGL) {
        visual = sdlop_x11_gl_visual(&depth);
    }

    memset(&attrs, 0, sizeof(attrs));
    attrs.event_mask = sdlop_x11_event_mask();
    attrs.background_pixel = BlackPixel(sdlop_x11_display, sdlop_x11_screen);
    attrs.border_pixel = 0;
    attrs.bit_gravity = NorthWestGravity;
    mask = CWEventMask | CWBackPixel | CWBorderPixel | CWBitGravity;
    if (visual != DefaultVisual(sdlop_x11_display, sdlop_x11_screen)) {
        attrs.colormap = XCreateColormap(sdlop_x11_display, sdlop_x11_root, visual, AllocNone);
        window->driver.x11.colormap = (unsigned long)attrs.colormap;
        mask |= CWColormap;
    }

    w = XCreateWindow(sdlop_x11_display, sdlop_x11_root, window->x, window->y,
                      (unsigned)window->w, (unsigned)window->h, 0, depth, InputOutput, visual,
                      mask, &attrs);
    if (!w) {
        return SDL_SetError("Couldn't create the X11 window");
    }
    window->driver.x11.window = (unsigned long)w;
    window->driver.x11.is_popup = (window->flags & SDL_WINDOW_POPUP_MENU) != 0;

    /* SDL3's platform properties, so an application can reach the objects behind
       the SDL_Window (a GL or Vulkan layer can be handed the number directly). */
    SDL_SetNumberProperty(window->props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, (Sint64)w);
    SDL_SetNumberProperty(window->props, SDL_PROP_WINDOW_X11_SCREEN_NUMBER, (Sint64)sdlop_x11_screen);
    SDL_SetNumberProperty(window->props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER,
                          (Sint64)(intptr_t)sdlop_x11_display);

    class_hint.res_name = (char *)"SDLop";
    class_hint.res_class = (char *)"SDLop";
    XSetClassHint(sdlop_x11_display, w, &class_hint);

    /* Ask the window manager to send us the close request instead of killing the
       connection, and to keep the EWMH state where we can read it back. */
    XSetWMProtocols(sdlop_x11_display, w, &atom_wm_delete_window, 1);

    sdlop_x11_set_title(window, window->title);
    sdlop_x11_set_size_hints(window);
    if (window->flags & SDL_WINDOW_BORDERLESS) {
        sdlop_x11_set_motif_hints(window, false);
    }
    if (window->parent) {
        XSetTransientForHint(sdlop_x11_display, w, (Window)window->parent->driver.x11.window);
    }

    /* X11 has no per-window pixel density: pixels are pixels. */
    SDLOP_OnWindowPixelSizeChanged(window, window->w, window->h);

    if (!(window->flags & SDL_WINDOW_HIDDEN)) {
        XMapWindow(sdlop_x11_display, w);
        /* Mapped straight away, so the focus request has to happen here too: an
           application that never calls SDL_ShowWindow() (the window was created
           visible) still has to be able to type into it. */
        sdlop_x11_request_focus(window);
        /* ...and let the server answer before this returns. The XSync() round
           trip means the server has processed the map by the time it returns and
           X delivers the events a request produces before the reply to a later
           one, so the Expose, the FocusIn and the first ConfigureNotify are in
           the queue already - pumping once dispatches them, without blocking on
           anything. Stock SDL3 ends up in the same state a harder way (it waits
           for MapNotify inside ShowWindow); either way SDL_EVENT_WINDOW_SHOWN,
           which the core sends after the driver is done, arrives after the expose
           and the focus, and SDL_GetWindowFlags() already reports
           SDL_WINDOW_INPUT_FOCUS for a window that was created visible. Without
           this the same events would only arrive at the application's first
           pump. */
        XSync(sdlop_x11_display, False);
        sdlop_x11_pump_events();
    }
    XFlush(sdlop_x11_display);
    return true;
}

static void sdlop_x11_destroy_ximage(SDL_Window *window)
{
    SDLOP_X11WindowState *x = SDLOP_X11_STATE(window);

    if (!x->image) {
        return;
    }
#ifdef SDLOP_HAVE_XSHM
    if (x->shm) {
        XShmSegmentInfo *shminfo = (XShmSegmentInfo *)x->shm;

        XShmDetach(sdlop_x11_display, shminfo);
        XDestroyImage((XImage *)x->image);
        shmdt(shminfo->shmaddr);
        shmctl(shminfo->shmid, IPC_RMID, NULL);
        SDLOP_Free(shminfo);
        x->shm = NULL;
    } else
#endif
    {
        XDestroyImage((XImage *)x->image);   /* frees the data, which we own */
    }
    x->image = NULL;
    x->buffer = NULL;
    x->buffer_pitch = 0;
}

static void sdlop_x11_destroy_window(SDL_Window *window)
{
    sdlop_x11_destroy_ximage(window);
    if (window->driver.x11.gc) {
        XFreeGC(sdlop_x11_display, (GC)window->driver.x11.gc);
        window->driver.x11.gc = NULL;
    }
    if (window->driver.x11.window) {
        XDestroyWindow(sdlop_x11_display, (Window)window->driver.x11.window);
        window->driver.x11.window = 0;
    }
    if (window->driver.x11.colormap) {
        XFreeColormap(sdlop_x11_display, (Colormap)window->driver.x11.colormap);
        window->driver.x11.colormap = 0;
    }
    XFlush(sdlop_x11_display);
}

static bool sdlop_x11_set_window_title(SDL_Window *window, const char *title)
{
    sdlop_x11_set_title(window, title);
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_set_window_position(SDL_Window *window, int x, int y)
{
    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        return true;               /* a fullscreen window is placed by the WM */
    }
    XMoveWindow(sdlop_x11_display, (Window)window->driver.x11.window, x, y);
    XFlush(sdlop_x11_display);
    /* No event and no state update here: the ConfigureNotify this request
       answers with is the only thing that reports a new position, exactly as in
       stock SDL3 - which is why SDL_SetWindowPosition() is documented to need a
       pump (or SDL_SyncWindow()) before SDL_GetWindowPosition() reflects it.
       Reporting the position eagerly as well was wrong twice over: it doubled
       the MOVED event and let the notifications X had already queued for the
       window's previous position arrive after it as a stale move back (the
       trace showed MOVED 480,280 - the position the window was created at -
       after the move to 120,96). */
    return true;
}

static bool sdlop_x11_set_window_size(SDL_Window *window, int w, int h)
{
    sdlop_x11_set_size_hints(window);
    XResizeWindow(sdlop_x11_display, (Window)window->driver.x11.window, (unsigned)w, (unsigned)h);
    XFlush(sdlop_x11_display);
    /* The window manager may adjust the size, and ConfigureNotify answers this
       request with what the window actually got - stock SDL3 reports the size
       from there and not from here, and until it arrives SDL_GetWindowSize()
       keeps saying what the window's size still is (SDL_SyncWindow() waits for
       the change). X11 has no scale factor of its own, so the pixel size follows
       the same event. */
    return true;
}

static bool sdlop_x11_set_window_bordered(SDL_Window *window, bool bordered)
{
    sdlop_x11_set_motif_hints(window, bordered);
    /* The decorations belong to the frame the window manager draws, so the window
       has to be remapped for the change to take effect. */
    XUnmapWindow(sdlop_x11_display, (Window)window->driver.x11.window);
    XSync(sdlop_x11_display, False);
    if (!(window->flags & SDL_WINDOW_HIDDEN)) {
        XMapWindow(sdlop_x11_display, (Window)window->driver.x11.window);
    }
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_set_window_resizable(SDL_Window *window, bool resizable)
{
    (void)resizable;
    sdlop_x11_set_size_hints(window);
    return true;
}

static void sdlop_x11_change_wm_state(SDL_Window *window, Atom state, bool add)
{
    XEvent event;

    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = (Window)window->driver.x11.window;
    event.xclient.message_type = atom_net_wm_state;
    event.xclient.format = 32;
    event.xclient.data.l[0] = add ? 1 : 0;      /* _NET_WM_STATE_ADD / _REMOVE */
    event.xclient.data.l[1] = (long)state;
    event.xclient.data.l[2] = 0;
    event.xclient.data.l[3] = 1;                /* source: application */
    XSendEvent(sdlop_x11_display, sdlop_x11_root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &event);
    XFlush(sdlop_x11_display);
}

static bool sdlop_x11_set_window_always_on_top(SDL_Window *window, bool on_top)
{
    sdlop_x11_change_wm_state(window, atom_net_wm_state_above, on_top);
    return true;
}

static bool sdlop_x11_set_window_fullscreen(SDL_Window *window, bool fullscreen)
{
    if (fullscreen || (window->flags & SDL_WINDOW_BORDERLESS)) {
        sdlop_x11_set_motif_hints(window, false);
    } else {
        sdlop_x11_set_motif_hints(window, true);
    }
    sdlop_x11_change_wm_state(window, atom_net_wm_state_fullscreen, fullscreen);
    return true;
}

/* Ask for the keyboard focus. XSetInputFocus() is what works without a window
   manager (Xvfb, a bare X session); with one, the EWMH request is what actually
   focuses us, since only the window manager may set the input focus. */
static void sdlop_x11_request_focus(SDL_Window *window)
{
    XEvent event;
    Window w = (Window)window->driver.x11.window;

    if (!w || (window->flags & SDL_WINDOW_NOT_FOCUSABLE)) {
        return;
    }
    XSetInputFocus(sdlop_x11_display, w, RevertToParent, CurrentTime);
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = w;
    event.xclient.message_type = atom_net_active_window;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;               /* source: application */
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(sdlop_x11_display, sdlop_x11_root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &event);
}

static bool sdlop_x11_show_window(SDL_Window *window)
{
    Window w = (Window)window->driver.x11.window;

    XMapWindow(sdlop_x11_display, w);
    XRaiseWindow(sdlop_x11_display, w);
    sdlop_x11_request_focus(window);
    /* Dispatch what the server had to say about the map before returning (the
       XSync() round trip above guarantees the events are queued), so that
       SDL_ShowWindow() returns with the window's state settled: SDLop sends
       SDL_EVENT_WINDOW_SHOWN from the core after this returns, and stock SDL3's
       SHOWN likewise comes after the EXPOSED and FOCUS_GAINED the server sent. */
    XSync(sdlop_x11_display, False);
    sdlop_x11_pump_events();
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_hide_window(SDL_Window *window)
{
    XUnmapWindow(sdlop_x11_display, (Window)window->driver.x11.window);
    /* As in show: unmap, then let the server answer before returning. The
       FocusOut that goes with the unmap is dispatched here, so the core's
       SDL_EVENT_WINDOW_HIDDEN (sent after this returns) lands after the focus
       change - which is the order stock SDL3 reports them in. */
    XSync(sdlop_x11_display, False);
    sdlop_x11_pump_events();
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_raise_window(SDL_Window *window)
{
    XRaiseWindow(sdlop_x11_display, (Window)window->driver.x11.window);
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_maximize_window(SDL_Window *window)
{
    sdlop_x11_change_wm_state(window, atom_net_wm_state_maximized_vert, true);
    sdlop_x11_change_wm_state(window, atom_net_wm_state_maximized_horz, true);
    return true;
}

static bool sdlop_x11_minimize_window(SDL_Window *window)
{
    XIconifyWindow(sdlop_x11_display, (Window)window->driver.x11.window, sdlop_x11_screen);
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_restore_window(SDL_Window *window)
{
    if (window->flags & SDL_WINDOW_MINIMIZED) {
        XMapWindow(sdlop_x11_display, (Window)window->driver.x11.window);
    }
    sdlop_x11_change_wm_state(window, atom_net_wm_state_maximized_vert, false);
    sdlop_x11_change_wm_state(window, atom_net_wm_state_maximized_horz, false);
    return true;
}

static bool sdlop_x11_set_window_opacity(SDL_Window *window, float opacity)
{
    unsigned long value = (unsigned long)(opacity * 4294967295.0f);

    XChangeProperty(sdlop_x11_display, (Window)window->driver.x11.window,
                    atom_net_wm_window_opacity, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&value, 1);
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_set_window_parent(SDL_Window *window, SDL_Window *parent)
{
    if (parent) {
        XSetTransientForHint(sdlop_x11_display, (Window)window->driver.x11.window,
                             (Window)parent->driver.x11.window);
    } else {
        XDeleteProperty(sdlop_x11_display, (Window)window->driver.x11.window, XA_WM_TRANSIENT_FOR);
    }
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_set_window_modal(SDL_Window *window, SDL_Window *parent)
{
    /* Modality is window-manager policy: being transient for the parent window is
       how the X toolkits ask for it. */
    return sdlop_x11_set_window_parent(window, parent);
}

static bool sdlop_x11_set_window_focusable(SDL_Window *window, bool focusable)
{
    XWMHints hints;

    memset(&hints, 0, sizeof(hints));
    hints.flags = InputHint;
    hints.input = focusable ? True : False;
    XSetWMHints(sdlop_x11_display, (Window)window->driver.x11.window, &hints);
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_flash_window(SDL_Window *window, SDL_FlashOperation operation)
{
    sdlop_x11_change_wm_state(window, atom_net_wm_state_demands_attention,
                              operation != SDL_FLASH_CANCEL);
    return true;
}

static bool sdlop_x11_set_window_icon(SDL_Window *window, SDL_Surface *icon)
{
    /* _NET_WM_ICON is CARDINAL[]: width, height, then ARGB pixels. */
    unsigned long *data;
    size_t count;
    int x, y;

    if (!icon || !icon->pixels) {
        XDeleteProperty(sdlop_x11_display, (Window)window->driver.x11.window, atom_net_wm_icon);
        XFlush(sdlop_x11_display);
        return true;
    }
    count = 2 + (size_t)icon->w * (size_t)icon->h;
    data = (unsigned long *)SDLOP_Alloc(count * sizeof(unsigned long));
    if (!data) {
        return SDL_SetError("Out of memory");
    }
    data[0] = (unsigned long)icon->w;
    data[1] = (unsigned long)icon->h;
    for (y = 0; y < icon->h; y++) {
        for (x = 0; x < icon->w; x++) {
            Uint8 r = 0, g = 0, b = 0, a = 255;
            const SDL_PixelFormatDetails *details = SDL_GetPixelFormatDetails(icon->format);
            const Uint8 *src = (const Uint8 *)icon->pixels + (size_t)y * (size_t)icon->pitch +
                               (size_t)x * details->bytes_per_pixel;
            Uint32 pixel = 0;

            switch (details->bytes_per_pixel) {
            case 4:
                memcpy(&pixel, src, 4);
                break;
            case 3:
                pixel = (Uint32)src[0] | ((Uint32)src[1] << 8) | ((Uint32)src[2] << 16);
                break;
            case 2: {
                Uint16 half = 0;
                memcpy(&half, src, 2);
                pixel = half;
                break;
            }
            default:
                pixel = src[0];
                break;
            }
            SDL_GetRGBA(pixel, details, NULL, &r, &g, &b, &a);
            data[2 + (size_t)y * (size_t)icon->w + (size_t)x] =
                ((unsigned long)a << 24) | ((unsigned long)r << 16) | ((unsigned long)g << 8) |
                (unsigned long)b;
        }
    }
    XChangeProperty(sdlop_x11_display, (Window)window->driver.x11.window, atom_net_wm_icon,
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)data, (int)count);
    SDLOP_Free(data);
    XFlush(sdlop_x11_display);
    return true;
}

/* The hit test is applied as the X *input shape*: the window keeps drawing
   everywhere, but only the rectangles SDL_HitTest() says are interactive take
   the pointer. That is how SDL3's X11 backend does it too. */
static void sdlop_x11_apply_hit_test(SDL_Window *window, int x, int y)
{
#ifdef SDLOP_HAVE_XSHAPE
    SDL_HitTestResult result;
    XRectangle rects[4];
    int n = 0;

    if (!window || !window->hit_test) {
        return;
    }
    result = window->hit_test(window, &(SDL_Point){ x, y }, window->hit_test_data);
    switch (result) {
        case SDL_HITTEST_DRAGGABLE:
            return;                     /* keep the whole window interactive */
        case SDL_HITTEST_RESIZE_TOPLEFT:
        case SDL_HITTEST_RESIZE_TOP:
        case SDL_HITTEST_RESIZE_TOPRIGHT:
            rects[n++] = (XRectangle){ 0, 0, (unsigned short)window->w, 4 };
            break;
        case SDL_HITTEST_RESIZE_BOTTOMLEFT:
        case SDL_HITTEST_RESIZE_BOTTOM:
        case SDL_HITTEST_RESIZE_BOTTOMRIGHT:
            rects[n++] = (XRectangle){ 0, (short)(window->h - 4), (unsigned short)window->w, 4 };
            break;
        default:
            break;
    }
    switch (result) {
        case SDL_HITTEST_RESIZE_TOPLEFT:
        case SDL_HITTEST_RESIZE_LEFT:
        case SDL_HITTEST_RESIZE_BOTTOMLEFT:
            rects[n++] = (XRectangle){ 0, 0, 4, (unsigned short)window->h };
            break;
        case SDL_HITTEST_RESIZE_TOPRIGHT:
        case SDL_HITTEST_RESIZE_RIGHT:
        case SDL_HITTEST_RESIZE_BOTTOMRIGHT:
            rects[n++] = (XRectangle){ (short)(window->w - 4), 0, 4, (unsigned short)window->h };
            break;
        default:
            break;
    }
    XShapeCombineRectangles(sdlop_x11_display, (Window)window->driver.x11.window, ShapeInput, 0, 0,
                            rects, n, ShapeSet, Unsorted);
#else
    (void)window; (void)x; (void)y;
#endif
}

static bool sdlop_x11_set_window_hit_test(SDL_Window *window, SDL_HitTest callback, void *data)
{
    (void)data;
#ifdef SDLOP_HAVE_XSHAPE
    if (!callback) {
        /* An empty rectangle list means "the whole window". */
        XShapeCombineRectangles(sdlop_x11_display, (Window)window->driver.x11.window, ShapeInput,
                                0, 0, NULL, 0, ShapeSet, Unsorted);
    } else {
        sdlop_x11_apply_hit_test(window, window->w / 2, window->h / 2);
    }
#else
    (void)window; (void)callback;
#endif
    return true;
}

static bool sdlop_x11_sync_window(SDL_Window *window)
{
    (void)window;
    /* Not just a flush: the events the server has already sent are dispatched
       here, so a position or size that was just requested has arrived by the time
       this returns. That is what SDL_SyncWindow() is for in SDL3 (an application
       that does not call it sees the change whenever it next pumps), and stock's
       X11 driver drains the queue in the same place. */
    XSync(sdlop_x11_display, False);
    sdlop_x11_pump_events();
    XSync(sdlop_x11_display, False);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Software presentation                                                     */
/* ------------------------------------------------------------------------- */

/* The XImage a window is presented from. With MIT-SHM it lives in a segment the
   X server reads directly (no copy over the socket); otherwise it is a client
   buffer XPutImage() sends. Either way the pixels come from the window surface. */
static bool sdlop_x11_ensure_ximage(SDL_Window *window, int w, int h)
{
    SDLOP_X11WindowState *x = SDLOP_X11_STATE(window);
    XWindowAttributes attrs;

    if (w <= 0 || h <= 0) {
        return false;
    }
    if (x->image && ((XImage *)x->image)->width == w && ((XImage *)x->image)->height == h) {
        return true;
    }
    sdlop_x11_destroy_ximage(window);

    if (!XGetWindowAttributes(sdlop_x11_display, (Window)x->window, &attrs)) {
        return SDL_SetError("Couldn't query the X11 window");
    }

#ifdef SDLOP_HAVE_XSHM
    if (!sdlop_x11_shm_checked) {
        sdlop_x11_shm_checked = true;
        sdlop_x11_shm = XShmQueryExtension(sdlop_x11_display) ? true : false;
        if (sdlop_x11_shm) {
            SDLOP_LogInfo("sdlop: X11 presentation uses shared memory (MIT-SHM)");
        }
    }
    if (sdlop_x11_shm) {
        XShmSegmentInfo *shminfo = (XShmSegmentInfo *)SDLOP_Calloc(1, sizeof(XShmSegmentInfo));

        if (shminfo) {
            size_t bytes = (size_t)w * (size_t)h * 4;
            XImage *image;

            shminfo->shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
            if (shminfo->shmid >= 0) {
                shminfo->shmaddr = (char *)shmat(shminfo->shmid, NULL, 0);
                shminfo->readOnly = False;
                if (shminfo->shmaddr != (char *)-1) {
                    image = XShmCreateImage(sdlop_x11_display, attrs.visual, (unsigned)attrs.depth,
                                            ZPixmap, NULL, shminfo, (unsigned)w, (unsigned)h);
                    if (image) {
                        image->data = shminfo->shmaddr;
                        if (XShmAttach(sdlop_x11_display, shminfo)) {
                            x->image = image;
                            x->buffer = shminfo->shmaddr;
                            x->buffer_pitch = image->bytes_per_line;
                            x->shm = shminfo;
                            XSync(sdlop_x11_display, False);
                            return true;
                        }
                        XDestroyImage(image);
                    }
                    shmdt(shminfo->shmaddr);
                }
                shmctl(shminfo->shmid, IPC_RMID, NULL);
            }
            SDLOP_Free(shminfo);
            /* No shared memory after all: fall back to XPutImage for every
               window from here on (same pixels, one more copy). */
            sdlop_x11_shm = false;
        }
    }
#endif

    {
        size_t bytes = (size_t)w * (size_t)h * 4;
        char *buffer = (char *)SDLOP_Alloc(bytes);
        XImage *image;

        if (!buffer) {
            return SDL_SetError("Out of memory");
        }
        image = XCreateImage(sdlop_x11_display, attrs.visual, (unsigned)attrs.depth, ZPixmap, 0,
                             buffer, (unsigned)w, (unsigned)h, 32, 0);
        if (!image) {
            SDLOP_Free(buffer);
            return SDL_SetError("Couldn't create the XImage");
        }
        if (image->bytes_per_line <= 0) {
            image->bytes_per_line = w * 4;
        }
        x->image = image;
        x->buffer = buffer;
        x->buffer_pitch = image->bytes_per_line;
    }
    return true;
}

/* Copy one rectangle of the window surface into the XImage, converting to the
   visual's pixel layout when it is not the usual 32-bit XRGB one. */
static void sdlop_x11_copy_rect(SDL_Window *window, const SDL_Rect *rect)
{
    SDL_Surface *surface = window->surface;
    SDLOP_X11WindowState *x = SDLOP_X11_STATE(window);
    XImage *image = (XImage *)x->image;
    const SDL_PixelFormatDetails *details = surface ? SDL_GetPixelFormatDetails(surface->format)
                                                    : NULL;
    bool fast_path;
    int y;

    if (!surface || !surface->pixels || !image || !details) {
        return;
    }
    fast_path = details->bytes_per_pixel == 4 && image->bits_per_pixel == 32 &&
                image->byte_order == LSBFirst && image->red_mask == 0x00FF0000 &&
                image->green_mask == 0x0000FF00 && image->blue_mask == 0x000000FF;

    for (y = rect->y; y < rect->y + rect->h; y++) {
        Uint8 *dst = (Uint8 *)x->buffer + (size_t)y * (size_t)x->buffer_pitch +
                     (size_t)rect->x * 4;
        Uint8 *src = (Uint8 *)surface->pixels + (size_t)y * (size_t)surface->pitch +
                     (size_t)rect->x * details->bytes_per_pixel;

        if (fast_path) {
            memcpy(dst, src, (size_t)rect->w * 4);
            continue;
        }
        for (int i = 0; i < rect->w; i++) {
            Uint32 pixel = 0, packed = 0;
            Uint8 r = 0, g = 0, b = 0, a = 255;
            Uint32 values[3];
            Uint32 masks[3];

            switch (details->bytes_per_pixel) {
            case 4:
                memcpy(&pixel, src + (size_t)i * 4, 4);
                break;
            case 3:
                pixel = (Uint32)src[i * 3] | ((Uint32)src[i * 3 + 1] << 8) |
                        ((Uint32)src[i * 3 + 2] << 16);
                break;
            case 2: {
                Uint16 half = 0;
                memcpy(&half, src + (size_t)i * 2, 2);
                pixel = half;
                break;
            }
            default:
                pixel = src[i];
                break;
            }
            SDL_GetRGBA(pixel, details, NULL, &r, &g, &b, &a);
            values[0] = r; values[1] = g; values[2] = b;
            masks[0] = (Uint32)image->red_mask;
            masks[1] = (Uint32)image->green_mask;
            masks[2] = (Uint32)image->blue_mask;
            for (int c = 0; c < 3; c++) {
                Uint32 mask = masks[c];
                Uint32 shift = 0, bits = 0;

                if (!mask) {
                    continue;
                }
                while (!(mask & 1u)) {
                    mask >>= 1;
                    shift++;
                }
                while (mask & 1u) {
                    mask >>= 1;
                    bits++;
                }
                packed |= ((values[c] >> (8 - bits)) << shift) & masks[c];
            }
            memcpy(dst + (size_t)i * 4, &packed, 4);
        }
    }
}

static bool sdlop_x11_present(SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDLOP_X11WindowState *x = SDLOP_X11_STATE(window);
    SDL_Rect whole;
    int i;

    if (!x->window || !window->surface) {
        return true;
    }
    if (!sdlop_x11_ensure_ximage(window, window->surface->w, window->surface->h)) {
        return false;
    }
    if (!x->gc) {
        x->gc = (void *)XCreateGC(sdlop_x11_display, (Window)x->window, 0, NULL);
        if (!x->gc) {
            return SDL_SetError("Couldn't create an X11 graphics context");
        }
    }

    whole = (SDL_Rect){ 0, 0, window->surface->w, window->surface->h };
    if (!rects || numrects <= 0) {
        rects = &whole;
        numrects = 1;
    }
    for (i = 0; i < numrects; i++) {
        SDL_Rect rect = rects[i];

        if (rect.x < 0) { rect.w += rect.x; rect.x = 0; }
        if (rect.y < 0) { rect.h += rect.y; rect.y = 0; }
        if (rect.x + rect.w > whole.w) { rect.w = whole.w - rect.x; }
        if (rect.y + rect.h > whole.h) { rect.h = whole.h - rect.y; }
        if (rect.w <= 0 || rect.h <= 0) {
            continue;
        }
        sdlop_x11_copy_rect(window, &rect);
#ifdef SDLOP_HAVE_XSHM
        if (x->shm) {
            XShmPutImage(sdlop_x11_display, (Window)x->window, (GC)x->gc, (XImage *)x->image,
                         rect.x, rect.y, rect.x, rect.y, (unsigned)rect.w, (unsigned)rect.h, False);
            continue;
        }
#endif
        XPutImage(sdlop_x11_display, (Window)x->window, (GC)x->gc, (XImage *)x->image,
                  rect.x, rect.y, rect.x, rect.y, (unsigned)rect.w, (unsigned)rect.h);
    }
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_destroy_window_surface(SDL_Window *window)
{
    sdlop_x11_destroy_ximage(window);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Input: the X event stream (the fallback when evdev is unavailable)         */
/* ------------------------------------------------------------------------- */

static void sdlop_x11_handle_key(XKeyEvent *event, bool down)
{
    Uint32 evdev;
    SDL_Scancode scancode;
    bool repeat = false;

    if (!down) {
        sdlop_x11_last_key_down = false;
    } else if (sdlop_x11_detectable_repeat && sdlop_x11_last_key_down &&
               sdlop_x11_last_keycode == event->keycode) {
        repeat = true;
    }
    if (down) {
        sdlop_x11_last_keycode = event->keycode;
        sdlop_x11_last_key_down = true;
    }
    evdev = (event->keycode >= 8) ? (Uint32)(event->keycode - 8) : 0;
    scancode = SDLOP_ScancodeFromEvdevKeycode(evdev);
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    {
        /* Keep the layout's state (shift, caps lock, dead keys) in step with the
           keys actually seen - the same line the asynchronous input path runs, so
           both give the same keycode and the same text. */
        const SDLOP_KeyLayout *layout = SDLOP_GetKeyLayout();

        if (layout && layout->update_key) {
            layout->update_key(evdev, down);
        }
    }
    /* The SDL keycode and the text come from the layout - the X server's keymap,
       read through the same xkbcommon code the Wayland backend uses. */
    SDLOP_SendKeyEvent(scancode, SDLK_UNKNOWN, SDL_KMOD_NONE, down, repeat, SDL_GetTicksNS(),
                       evdev);
}

static Uint8 sdlop_x11_button(unsigned int button)
{
    switch (button) {
        case Button1: return SDL_BUTTON_LEFT;
        case Button2: return SDL_BUTTON_MIDDLE;
        case Button3: return SDL_BUTTON_RIGHT;
        case 8: return SDL_BUTTON_X1;
        case 9: return SDL_BUTTON_X2;
        default: return 0;
    }
}

static void sdlop_x11_handle_xrandr_event(XEvent *event)
{
#ifdef SDLOP_HAVE_XRANDR
    int xrandr_event = 0, xrandr_error = 0;

    if (!sdlop_x11_xrandr ||
        !XRRQueryExtension(sdlop_x11_display, &xrandr_event, &xrandr_error)) {
        return;
    }
    if (event->type == xrandr_event + RRScreenChangeNotify ||
        event->type == xrandr_event + RRNotify) {
        XRRUpdateConfiguration(event);
        sdlop_x11_refresh_displays();
    }
#else
    (void)event;
#endif
}

static void sdlop_x11_handle_property(SDL_Window *window, XPropertyEvent *event)
{
    if (event->atom == atom_wm_state && event->state == PropertyNewValue) {
        Atom actual = None;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char *data = NULL;

        if (XGetWindowProperty(sdlop_x11_display, event->window, atom_wm_state, 0, 2, False,
                               atom_wm_state, &actual, &format, &count, &remaining,
                               &data) == Success && data) {
            unsigned long state = ((unsigned long *)data)[0];

            SDLOP_OnWindowMinimized(window, state == IconicState);
            if (state == NormalState && (window->flags & SDL_WINDOW_HIDDEN)) {
                SDLOP_OnWindowShown(window, true);
            }
            XFree(data);
        }
    }
    if (event->atom == atom_net_wm_state) {
        /* Fullscreen and maximized can be changed by the window manager behind
           our back, so the property is read back rather than assumed. */
        Atom actual = None;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char *data = NULL;

        if (XGetWindowProperty(sdlop_x11_display, event->window, atom_net_wm_state, 0, 32, False,
                               XA_ATOM, &actual, &format, &count, &remaining,
                               &data) == Success && data) {
            Atom *states = (Atom *)data;
            bool fullscreen = false, maximized = false, hidden = false;
            unsigned long i;

            for (i = 0; i < count; i++) {
                if (states[i] == atom_net_wm_state_fullscreen) {
                    fullscreen = true;
                }
                if (states[i] == atom_net_wm_state_maximized_vert ||
                    states[i] == atom_net_wm_state_maximized_horz) {
                    maximized = true;
                }
                if (states[i] == atom_net_wm_state_hidden) {
                    /* The window manager's own "this window is not visible"
                       state, which is where stock SDL3 takes SDL_WINDOW_OCCLUDED
                       from on this backend. */
                    SDLOP_SetWindowOccludedFlag(window, true);
                    hidden = true;
                }
            }
            if (!hidden) {
                SDLOP_SetWindowOccludedFlag(window, false);
            }
            SDLOP_OnWindowFullscreenChanged(window, fullscreen);
            SDLOP_OnWindowMaximized(window, maximized);
            XFree(data);
        }
    }
}

static void sdlop_x11_handle_button(SDL_Window *window, XButtonEvent *button, bool down)
{
    Uint8 sdl_button;

    if (SDLOP_AsyncInputActive()) {
        return;                    /* the evdev worker is the input source */
    }
    if (button->button >= Button4 && button->button <= 7) {
        float dx = 0.0f, dy = 0.0f;

        if (!down) {
            return;                /* one wheel event per click, not per edge */
        }
        /* X11 reports the wheel as buttons 4/5 (vertical) and 6/7 (horizontal),
           with the same signs SDL3 uses. */
        if (button->button == Button4) {
            dy = 1.0f;
        } else if (button->button == Button5) {
            dy = -1.0f;
        } else if (button->button == 6) {
            dx = -1.0f;
        } else {
            dx = 1.0f;
        }
        SDLOP_SendMouseWheel(window ? window->id : 0, dx, dy, SDL_MOUSEWHEEL_NORMAL,
                             SDL_GetTicksNS());
        return;
    }
    sdl_button = sdlop_x11_button(button->button);
    if (!sdl_button || !window) {
        return;
    }
    SDLOP_SendMouseButton(1, sdl_button, down, sdlop_x11_mouse_x, sdlop_x11_mouse_y,
                          SDL_GetTicksNS());
}

/* One place for "the pointer is here now": the MotionNotify path and the
   crossing events (which carry a position of their own) both end up here. */
static void sdlop_x11_send_motion(SDL_Window *window, float x, float y)
{
    if (SDLOP_AsyncInputActive() || !window) {
        return;
    }
    if (window->flags & SDL_WINDOW_MOUSE_RELATIVE_MODE) {
        /* The deltas of a locked pointer come from XI2 raw motion; the position
           of a pointer that is being warped back is meaningless. */
        sdlop_x11_mouse_x = x;
        sdlop_x11_mouse_y = y;
        return;
    }
    sdlop_x11_mouse_x = x;
    sdlop_x11_mouse_y = y;
    SDLOP_SendMouseMotionAbsolute(window->id, x, y, SDL_GetTicksNS());

    if (window->hit_test) {
        sdlop_x11_apply_hit_test(window, (int)x, (int)y);
    }
    /* SDL_SetWindowMouseRect() is emulated: X11 has no way to confine the pointer
       to a rectangle, so one that leaves is warped back inside. */
    if (window->mouse_rect_set && window->mouse_rect.w > 0 && window->mouse_rect.h > 0) {
        const SDL_Rect *r = &window->mouse_rect;

        if (x < r->x || y < r->y || x >= r->x + r->w || y >= r->y + r->h) {
            float nx = (float)r->x + (float)r->w / 2.0f;
            float ny = (float)r->y + (float)r->h / 2.0f;

            XWarpPointer(sdlop_x11_display, None, (Window)window->driver.x11.window, 0, 0, 0, 0,
                         (int)nx, (int)ny);
            sdlop_x11_mouse_x = nx;
            sdlop_x11_mouse_y = ny;
        }
    }
}

static void sdlop_x11_handle_motion(SDL_Window *window, XMotionEvent *motion)
{
    sdlop_x11_send_motion(window, (float)motion->x, (float)motion->y);
}

/* Re-read the server's keymap after the server said it changed. The rebuild is
   asked not to announce anything: the callers send exactly one
   SDL_EVENT_KEYMAP_CHANGED per notification, the way stock SDL3 does. */
static void sdlop_x11_refresh_keymap(void)
{
#if defined(SDLOP_HAVE_XKBCOMMON) && defined(SDLOP_HAVE_XKBCOMMON_X11)
    if (!SDLOP_XKBOverrideActive()) {
        int device_id = SDLOP_XKBX11DeviceID(sdlop_x11_display);
        if (device_id >= 0 && !SDLOP_XKBLoadFromX11(sdlop_x11_display, device_id, false)) {
            SDLOP_LogWarn("sdlop: could not refresh the X server keymap");
        }
    }
#endif
}

static void sdlop_x11_handle_event(XEvent *event)
{
    SDL_Window *window = sdlop_x11_window_from_xid(event->xany.window);
    static int trace = -1;

    if (trace < 0) {
        const char *env = SDL_getenv("SDLOP_X11_DEBUG_EVENTS");
        trace = (env && *env && *env != '0') ? 1 : 0;
        if (trace) {
            /* The events are logged at debug level, which nothing prints by
               default: asking for the trace is also asking for that level. */
            SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_DEBUG);
        }
    }
    if (trace) {
        /* What the X server actually sent, in order: the only way to tell a
           driver problem from a server-side one. Same idea as SDLOP_XKB_DUMP -
           set the variable, read stderr. */
        if (event->type == Expose) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "sdlop: x11 Expose window=0x%lx count=%d x=%d y=%d w=%d h=%d",
                         event->xany.window, event->xexpose.count, event->xexpose.x,
                         event->xexpose.y, event->xexpose.width, event->xexpose.height);
        } else if (event->type == ConfigureNotify) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "sdlop: x11 ConfigureNotify window=0x%lx x=%d y=%d w=%d h=%d",
                         event->xany.window, event->xconfigure.x, event->xconfigure.y,
                         event->xconfigure.width, event->xconfigure.height);
        } else if (event->type == EnterNotify || event->type == LeaveNotify) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "sdlop: x11 %s window=0x%lx x=%d y=%d mode=%d detail=%d",
                         event->type == EnterNotify ? "EnterNotify" : "LeaveNotify",
                         event->xany.window, event->xcrossing.x, event->xcrossing.y,
                         event->xcrossing.mode, event->xcrossing.detail);
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "sdlop: x11 event type=%d window=0x%lx",
                         event->type, event->xany.window);
        }
    }

    switch (event->type) {
        case KeyPress:
        case KeyRelease:
            if (SDLOP_AsyncInputActive()) {
                break;
            }
            if (window && SDLOP_GetKeyboardFocusWindow() != window) {
                SDLOP_OnWindowFocusGained(window);
            }
            sdlop_x11_handle_key(&event->xkey, event->type == KeyPress);
            break;
        case ButtonPress:
            sdlop_x11_handle_button(window, &event->xbutton, true);
            break;
        case ButtonRelease:
            sdlop_x11_handle_button(window, &event->xbutton, false);
            break;
        case MotionNotify:
            sdlop_x11_handle_motion(window, &event->xmotion);
            break;
        case EnterNotify:
            if (window) {
                SDLOP_OnWindowMouseEnter(window);
                /* The pointer is inside the window now and the crossing event
                   says where: report that as a motion, because an application
                   that tracks the pointer through motion events has no other way
                   to learn where it is until it moves again. Stock SDL3 does the
                   same on EnterNotify and LeaveNotify - not doing it was one of
                   the differences the behaviour probe found (two MOTION events
                   in stock's phase-1 trace, none in SDLop's). */
                sdlop_x11_send_motion(window, (float)event->xcrossing.x,
                                      (float)event->xcrossing.y);
            }
            break;
        case LeaveNotify:
            if (window) {
                /* The same on the way out: the event carries where the pointer
                   was, and the core clamps it into the window, so a pointer that
                   left through the edge is reported at that edge instead of
                   vanishing without a trace. */
                sdlop_x11_send_motion(window, (float)event->xcrossing.x,
                                      (float)event->xcrossing.y);
                SDLOP_OnWindowMouseLeave(window);
            }
            break;
        case FocusIn:
            if (window) {
                SDLOP_OnWindowFocusGained(window);
            }
            break;
        case FocusOut:
            if (window) {
                SDLOP_OnWindowFocusLost(window);
                SDLOP_ResetKeyboardState();
                sdlop_x11_last_key_down = false;
            }
            break;
        case MappingNotify:
            /* The server's keyboard mapping changed: this is the event a live
               layout switch turns into, and it is answered the way stock SDL3
               answers it - rebuild the keymap and send SDL_EVENT_KEYMAP_CHANGED,
               for every MappingNotify, whether or not the rebuild differs from
               what was there (stock's X11 driver does exactly this; the only
               keymap that is announced to nobody is a session's first one).
               The rebuild itself is asked not to announce, so that one event
               goes out per MappingNotify instead of one per keymap file that
               changed. */
            if (event->xmapping.request == MappingKeyboard ||
                event->xmapping.request == MappingModifier) {
                XRefreshKeyboardMapping(&event->xmapping);
                sdlop_x11_refresh_keymap();
            }
            SDLOP_SendKeymapChanged(SDL_GetTicksNS());
            break;
        case ConfigureNotify:
            if (window) {
                SDLOP_OnWindowMoved(window, event->xconfigure.x, event->xconfigure.y);
                SDLOP_OnWindowResized(window, event->xconfigure.width, event->xconfigure.height);
                SDLOP_OnWindowPixelSizeChanged(window, event->xconfigure.width,
                                               event->xconfigure.height);
            }
            break;
        case Expose:
            /* X splits an expose into one event per rectangle and numbers how
               many are still queued behind this one, so a single redraw reports
               itself once - in the last event of the run. Pushing every event
               made SDLop announce two or three times per show where stock SDL3
               announces once (the behaviour probe counted 6 against 3). */
            if (window && event->xexpose.count == 0) {
                SDLOP_OnWindowExposed(window);
            }
            break;
        case MapNotify:
            /* The window is mapped: report it only when that is news, so a
               window the application showed does not emit SHOWN twice (the core
               already emitted it) while one the window manager unmapped and
               mapped again does. */
            if (window && (window->flags & SDL_WINDOW_HIDDEN)) {
                SDLOP_OnWindowShown(window, true);
            }
            break;
        case UnmapNotify:
            if (window && !(window->flags & SDL_WINDOW_HIDDEN)) {
                SDLOP_OnWindowShown(window, false);
            }
            break;
        case VisibilityNotify:
            /* Deliberately not handled: the X server's visibility states are not
               the window manager's occlusion, and stock SDL3's X11 driver ignores
               this event entirely (it takes SDL_WINDOW_OCCLUDED from
               _NET_WM_STATE_HIDDEN, which SDLop reads in the property handler).
               Sending EXPOSED for every VisibilityUnobscured is what made SDLop
               announce twice as many exposes as stock. */
            break;
        case PropertyNotify:
            /* A live layout switch, seen from the root window. X servers do not
               agree on how they tell clients about one: some post the core
               MappingNotify handled above, and the ones that do send it only to
               clients that never spoke XKB to them at all (measured on Xorg
               21.1.16: a client that has sent XkbUseExtension receives nothing
               when setxkbmap reloads a layout). SDLop's connection does speak
               XKB - its keymap comes from xkbcommon-x11 - so on those servers
               the switch would be invisible without this. What every server that
               reloads a layout does rewrite is these root properties, and the new
               keymap is already readable when the property event arrives
               (measured, not assumed: a fetch at the event returns the layout
               that was just switched to). XKLAVIER_STATE is the property stock
               SDL3 watches for exactly this reason (SDL_x11events.c, "Hack for
               Ubuntu 12.04 (etc) that doesn't send MappingNotify events"); the
               rules property is what this X server actually writes. */
            if (event->xproperty.window == sdlop_x11_root &&
                (event->xproperty.atom == atom_xkb_rules_names ||
                 event->xproperty.atom == atom_xklavier_state)) {
                sdlop_x11_refresh_keymap();
                SDLOP_SendKeymapChanged(SDL_GetTicksNS());
                break;
            }
            if (window) {
                sdlop_x11_handle_property(window, &event->xproperty);
            }
            break;
        case ClientMessage:
            if (window && event->xclient.message_type == atom_wm_protocols &&
                (Atom)event->xclient.data.l[0] == atom_wm_delete_window) {
                SDLOP_OnWindowClosed(window);
            }
            break;
        case DestroyNotify:
            if (window) {
                SDLOP_OnWindowClosed(window);
            }
            break;
        default:
            sdlop_x11_handle_xrandr_event(event);
            break;
    }
}

static void sdlop_x11_handle_xinput_event(XGenericEventCookie *cookie)
{
#ifdef SDLOP_HAVE_XINPUT2
    XIRawEvent *raw;
    SDL_Window *window;
    double dx = 0.0, dy = 0.0, *values;
    int i;

    if (!sdlop_x11_xinput2 || cookie->extension != sdlop_x11_xinput2_opcode) {
        return;
    }
    if (cookie->evtype != XI_RawMotion) {
        return;
    }
    if (SDLOP_AsyncInputActive() || !SDLOP_RelativeMouseModeActive()) {
        return;
    }
    raw = (XIRawEvent *)cookie->data;
    values = raw->valuators.values;
    /* The valuator mask says which axes this event carries; the first two are
       the pointer's x and y. */
    for (i = 0; i < raw->valuators.mask_len * 8; i++) {
        if (XIMaskIsSet(raw->valuators.mask, i)) {
            double value = *values++;

            if (i == 0) {
                dx = value;
            } else if (i == 1) {
                dy = value;
            }
        }
    }
    window = SDLOP_InputTargetWindow();
    if (!window || (dx == 0.0 && dy == 0.0)) {
        return;
    }
    /* Raw motion is unaccelerated: exactly the deltas relative mouse mode wants. */
    SDLOP_SendMouseMotionRelative(window->id, (float)dx, (float)dy, SDL_GetTicksNS());

    /* Keep the pointer in the middle of the window, so it can never escape and
       the next raw delta is measured from the same place. */
    XWarpPointer(sdlop_x11_display, None, (Window)window->driver.x11.window, 0, 0, 0, 0,
                 window->w / 2, window->h / 2);
#else
    (void)cookie;
#endif
}

static void sdlop_x11_pump_events(void)
{
    if (!sdlop_x11_display) {
        return;
    }
    while (XPending(sdlop_x11_display)) {
        XEvent event;

        XNextEvent(sdlop_x11_display, &event);
        if (event.type == GenericEvent) {
            XGenericEventCookie *cookie = &event.xcookie;

            if (XGetEventData(sdlop_x11_display, cookie)) {
                sdlop_x11_handle_xinput_event(cookie);
                XFreeEventData(sdlop_x11_display, cookie);
            }
            continue;
        }
        sdlop_x11_handle_event(&event);
    }
}

static int sdlop_x11_get_event_fd(void)
{
    return sdlop_x11_display ? ConnectionNumber(sdlop_x11_display) : -1;
}

/* ------------------------------------------------------------------------- */
/* Cursor, warp, capture, relative mode                                      */
/* ------------------------------------------------------------------------- */

static Cursor sdlop_x11_cursor_for(SDL_Cursor *cursor)
{
    if (!cursor) {
        return None;
    }
    if (cursor->backend) {
        return (Cursor)cursor->backend;
    }
    if (!cursor->data) {
        /* A system cursor: one glyph per shape in the X cursor font. */
        static const unsigned int shapes[] = {
            XC_left_ptr, XC_xterm, XC_watch, XC_watch, XC_crosshair,
            XC_fleur, XC_sb_h_double_arrow, XC_sb_v_double_arrow,
            XC_top_left_corner, XC_top_right_corner, XC_sb_up_arrow,
            XC_sb_down_arrow, XC_sb_left_arrow, XC_sb_right_arrow,
            XC_ll_angle, XC_ur_angle, XC_ur_angle, XC_ll_angle,
            XC_hand2, XC_X_cursor, XC_question_arrow,
        };
        size_t index = (size_t)cursor->system;
        unsigned int shape = (index < sizeof(shapes) / sizeof(shapes[0]))
                                 ? shapes[index] : XC_left_ptr;

        cursor->backend = (void *)XCreateFontCursor(sdlop_x11_display, shape);
        return (Cursor)cursor->backend;
    }

    /* A custom cursor: the core protocol only has 1-bit masks, so alpha becomes
       opaque or transparent (SDL3 does the same when Xcursor is unavailable). */
    {
        unsigned int w = (unsigned int)cursor->w, h = (unsigned int)cursor->h;
        unsigned int stride = (w + 7) / 8;
        char *mask_data = (char *)SDLOP_Calloc(1, (size_t)stride * h);
        XColor fg, bg;
        Pixmap source, mask;
        GC gc;
        Cursor xcursor;
        int x, y;

        if (!mask_data) {
            return None;
        }
        for (y = 0; y < cursor->h; y++) {
            for (x = 0; x < cursor->w; x++) {
                const Uint8 *p = cursor->data + ((size_t)y * (size_t)cursor->w + (size_t)x) * 4;

                if (p[3] > 128) {
                    mask_data[y * (int)stride + x / 8] |= (char)(1 << (x % 8));
                }
            }
        }
        memset(&fg, 0, sizeof(fg));
        memset(&bg, 0, sizeof(bg));
        fg.red = fg.green = fg.blue = 0xFFFF;
        source = XCreateBitmapFromData(sdlop_x11_display, sdlop_x11_root, mask_data, w, h);
        mask = XCreateBitmapFromData(sdlop_x11_display, sdlop_x11_root, mask_data, w, h);
        if (!source || !mask) {
            SDLOP_Free(mask_data);
            return None;
        }
        gc = XCreateGC(sdlop_x11_display, source, 0, NULL);
        XSetForeground(sdlop_x11_display, gc, 1);
        for (y = 0; y < cursor->h; y++) {
            for (x = 0; x < cursor->w; x++) {
                const Uint8 *p = cursor->data + ((size_t)y * (size_t)cursor->w + (size_t)x) * 4;

                if (p[3] > 128) {
                    XDrawPoint(sdlop_x11_display, source, gc, x, y);
                }
            }
        }
        xcursor = XCreatePixmapCursor(sdlop_x11_display, source, mask, &fg, &bg,
                                      (unsigned)cursor->hot_x, (unsigned)cursor->hot_y);
        XFreeGC(sdlop_x11_display, gc);
        XFreePixmap(sdlop_x11_display, source);
        XFreePixmap(sdlop_x11_display, mask);
        SDLOP_Free(mask_data);
        cursor->backend = (void *)xcursor;
        return xcursor;
    }
}

typedef struct SDLOP_X11SetCursor
{
    Cursor cursor;
} SDLOP_X11SetCursor;

static void sdlop_x11_define_cursor(SDL_Window *window, void *userdata)
{
    SDLOP_X11SetCursor *set = (SDLOP_X11SetCursor *)userdata;

    if (window->driver.x11.window) {
        XDefineCursor(sdlop_x11_display, (Window)window->driver.x11.window, set->cursor);
    }
}

static bool sdlop_x11_set_cursor(SDL_Window *window, SDL_Cursor *cursor)
{
    Cursor xcursor = sdlop_x11_cursor_for(cursor);

    if (!window) {
        /* No window: this is the cursor every window should use. */
        SDLOP_X11SetCursor set;

        set.cursor = xcursor;
        SDLOP_WindowIterate(sdlop_x11_define_cursor, &set);
        XFlush(sdlop_x11_display);
        return true;
    }
    XDefineCursor(sdlop_x11_display, (Window)window->driver.x11.window,
                  xcursor ? xcursor : None);
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_show_cursor(SDL_Window *window, bool show)
{
    if (!window) {
        return true;
    }
    /* Hiding a cursor on X11 means defining the invisible one: X11 has no
       show/hide request. */
    XDefineCursor(sdlop_x11_display, (Window)window->driver.x11.window,
                  show ? None : sdlop_x11_blank_cursor);
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_warp_mouse(SDL_Window *window, float x, float y)
{
    if (!window) {
        return true;
    }
    XWarpPointer(sdlop_x11_display, None, (Window)window->driver.x11.window, 0, 0, 0, 0,
                 (int)x, (int)y);
    sdlop_x11_mouse_x = x;
    sdlop_x11_mouse_y = y;
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_capture_mouse(bool enabled)
{
    if (enabled) {
        if (XGrabPointer(sdlop_x11_display, sdlop_x11_root, False,
                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime) != GrabSuccess) {
            return SDL_SetError("Couldn't grab the pointer");
        }
    } else {
        XUngrabPointer(sdlop_x11_display, CurrentTime);
    }
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_relative_mode(SDL_Window *window, bool enabled)
{
    if (!window) {
        return true;
    }
    if (enabled) {
        /* The core protocol has no pointer lock: the pointer is hidden, grabbed
           and kept in the middle of the window, and the deltas come from XI2 raw
           motion (see sdlop_x11_handle_xinput_event). Without XI2 the same
           emulation still works, only with accelerated deltas. */
        XDefineCursor(sdlop_x11_display, (Window)window->driver.x11.window,
                      sdlop_x11_blank_cursor);
        XGrabPointer(sdlop_x11_display, (Window)window->driver.x11.window, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, (Window)window->driver.x11.window, None,
                     CurrentTime);
        sdlop_x11_warp_mouse(window, (float)window->w / 2.0f, (float)window->h / 2.0f);
    } else {
        XUngrabPointer(sdlop_x11_display, CurrentTime);
        XDefineCursor(sdlop_x11_display, (Window)window->driver.x11.window, None);
    }
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_set_window_grab(SDL_Window *window, bool keyboard, bool mouse)
{
    if (!window) {
        return true;
    }
    if (keyboard) {
        XGrabKeyboard(sdlop_x11_display, (Window)window->driver.x11.window, False,
                      GrabModeAsync, GrabModeAsync, CurrentTime);
    } else {
        XUngrabKeyboard(sdlop_x11_display, CurrentTime);
    }
    if (mouse) {
        XGrabPointer(sdlop_x11_display, (Window)window->driver.x11.window, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask |
                     LeaveWindowMask,
                     GrabModeAsync, GrabModeAsync, (Window)window->driver.x11.window, None,
                     CurrentTime);
    } else if (!SDLOP_RelativeMouseModeActive()) {
        XUngrabPointer(sdlop_x11_display, CurrentTime);
    }
    XFlush(sdlop_x11_display);
    return true;
}

static bool sdlop_x11_set_window_mouse_rect(SDL_Window *window, const SDL_Rect *rect)
{
    (void)window;
    (void)rect;
    /* The rect is enforced by warping the pointer back inside (see the motion
       handler). X11's core protocol cannot confine the pointer to a rectangle;
       the grab state that comes with it is handled by the core and by
       sdlop_x11_set_window_grab(). */
    return true;
}

/* ------------------------------------------------------------------------- */
/* Vulkan: the X11 WSI (VK_KHR_xlib_surface)                                 */
/*                                                                           */
/* The ABI is declared here, like the Vulkan module's, so no Vulkan headers  */
/* are needed to build against a runtime-only system.                        */
/* ------------------------------------------------------------------------- */

#define SDLOP_VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR 1000009000

typedef int (*SDLOP_PFN_vkCreateXlibSurfaceKHR)(VkInstance instance, const void *create_info,
                                                const struct VkAllocationCallbacks *allocator,
                                                VkSurfaceKHR *surface);
typedef bool (*SDLOP_PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR)(
    VkPhysicalDevice physical_device, Uint32 queue_family_index, Display *display,
    VisualID visual_id);

typedef struct SDLOP_VkXlibSurfaceCreateInfoKHR
{
    Uint32 sType;
    const void *pNext;
    Uint32 flags;
    Display *dpy;
    unsigned long window;
} SDLOP_VkXlibSurfaceCreateInfoKHR;

static bool sdlop_x11_create_vulkan_surface(SDL_Window *window, VkInstance instance,
                                            const struct VkAllocationCallbacks *allocator,
                                            VkSurfaceKHR *surface)
{
    SDLOP_PFN_vkCreateXlibSurfaceKHR create_surface;
    SDLOP_VkXlibSurfaceCreateInfoKHR info;
    int result;

    if (!sdlop_x11_display || !window->driver.x11.window) {
        return SDL_SetError("The window has no X11 window");
    }
    create_surface = (SDLOP_PFN_vkCreateXlibSurfaceKHR)SDLOP_VulkanGetInstanceProc(
        instance, "vkCreateXlibSurfaceKHR");
    if (!create_surface) {
        return SDL_SetError("VK_KHR_xlib_surface extension is not enabled in the Vulkan instance");
    }
    memset(&info, 0, sizeof(info));
    info.sType = SDLOP_VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    info.dpy = sdlop_x11_display;
    info.window = (unsigned long)window->driver.x11.window;
    result = create_surface(instance, &info, allocator, surface);
    if (result != 0) {
        return SDL_SetError("vkCreateXlibSurfaceKHR failed: %s", SDLOP_VulkanResultString(result));
    }
    return true;
}

static bool sdlop_x11_vulkan_presentation_support(VkInstance instance,
                                                  VkPhysicalDevice physical_device,
                                                  Uint32 queue_family_index)
{
    SDLOP_PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR get_support =
        (SDLOP_PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR)SDLOP_VulkanGetInstanceProc(
            instance, "vkGetPhysicalDeviceXlibPresentationSupportKHR");

    if (!get_support) {
        return SDL_SetError("VK_KHR_xlib_surface extension is not enabled in the Vulkan instance");
    }
    if (!sdlop_x11_display) {
        return SDL_SetError("The X display is not open");
    }
    /* Which visuals can be presented to is per-visual on X11; the screen's
       default visual is the one a window is created with unless GL asked for
       another. */
    return get_support(physical_device, queue_family_index, sdlop_x11_display,
                       XVisualIDFromVisual(sdlop_x11_visual)) ? true : false;
}

/* ------------------------------------------------------------------------- */
/* The driver                                                                */
/* ------------------------------------------------------------------------- */

static const char *const *sdlop_x11_vulkan_extensions(Uint32 *count)
{
    static const char *extensions[] = { "VK_KHR_surface", "VK_KHR_xlib_surface" };

    if (count) {
        *count = SDL_arraysize(extensions);
    }
    return extensions;
}

const SDLOP_VideoDriver SDLOP_X11VideoDriver = {
    .name = "x11",
    .init = sdlop_x11_init,
    .quit = sdlop_x11_quit,

    .create_window = sdlop_x11_create_window,
    .destroy_window = sdlop_x11_destroy_window,
    .set_window_title = sdlop_x11_set_window_title,
    .set_window_position = sdlop_x11_set_window_position,
    .set_window_size = sdlop_x11_set_window_size,
    .set_window_bordered = sdlop_x11_set_window_bordered,
    .set_window_resizable = sdlop_x11_set_window_resizable,
    .set_window_always_on_top = sdlop_x11_set_window_always_on_top,
    .set_window_fullscreen = sdlop_x11_set_window_fullscreen,
    .show_window = sdlop_x11_show_window,
    .hide_window = sdlop_x11_hide_window,
    .raise_window = sdlop_x11_raise_window,
    .maximize_window = sdlop_x11_maximize_window,
    .minimize_window = sdlop_x11_minimize_window,
    .restore_window = sdlop_x11_restore_window,
    .set_window_opacity = sdlop_x11_set_window_opacity,
    .set_window_mouse_rect = sdlop_x11_set_window_mouse_rect,
    .set_window_grab = sdlop_x11_set_window_grab,
    .set_window_modal = sdlop_x11_set_window_modal,
    .set_window_parent = sdlop_x11_set_window_parent,
    .set_window_focusable = sdlop_x11_set_window_focusable,
    .flash_window = sdlop_x11_flash_window,
    .set_window_icon = sdlop_x11_set_window_icon,
    .set_window_hit_test = sdlop_x11_set_window_hit_test,
    .sync_window = sdlop_x11_sync_window,

    .present_surface = sdlop_x11_present,
    .destroy_window_surface = sdlop_x11_destroy_window_surface,

    .pump_events = sdlop_x11_pump_events,
    .get_event_fd = sdlop_x11_get_event_fd,

    .set_cursor = sdlop_x11_set_cursor,
    .show_cursor = sdlop_x11_show_cursor,
    .warp_mouse = sdlop_x11_warp_mouse,
    .capture_mouse = sdlop_x11_capture_mouse,
    .set_relative_mouse_mode = sdlop_x11_relative_mode,

    .create_vulkan_surface = sdlop_x11_create_vulkan_surface,
    .get_vulkan_instance_extensions = sdlop_x11_vulkan_extensions,
    .vulkan_presentation_support = sdlop_x11_vulkan_presentation_support,

    .gl = &SDLOP_EGLDriver,
};

/* Handles the GL driver needs (src/gl/SDL_egl.c). */
unsigned long sdlop_x11_display_handle(void)
{
    return (unsigned long)sdlop_x11_display;
}

unsigned long sdlop_x11_window_handle(SDL_Window *window)
{
    return window ? window->driver.x11.window : 0;
}
