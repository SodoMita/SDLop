/*
  SDLop - header-free X11/Xlib ABI subset.

  Everything SDLop's X11 driver needs from <X11/Xlib.h>, <X11/Xutil.h> and
  <X11/X.h>, declared directly: the X11 protocol constants and the public
  Xlib structs have been frozen ABI since X11R6 (they are on-the-wire and
  shared-library ABI, they cannot change), and libX11 itself is dlopen'd
  at run time - so the X11 driver builds with zero X11 development
  packages installed and links against nothing X11-related.

  Only the stable public ABI is reproduced here; opaque server-side types
  (Display, Screen, GC, ...) stay opaque.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_x11_defs_h_
#define sdlop_x11_defs_h_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Base types (<X11/X.h>)                                              */
/* ------------------------------------------------------------------ */

typedef unsigned long SDLOP_XID;
typedef SDLOP_XID SDLOP_Window;
typedef SDLOP_XID SDLOP_Drawable;
typedef SDLOP_XID SDLOP_Font;
typedef SDLOP_XID SDLOP_Pixmap;
typedef SDLOP_XID SDLOP_Cursor;
typedef SDLOP_XID SDLOP_Colormap;
typedef SDLOP_XID SDLOP_GContext;
typedef SDLOP_XID SDLOP_KeySym;
typedef unsigned long SDLOP_Atom;
typedef unsigned long SDLOP_VisualID;
typedef unsigned long SDLOP_Time;
typedef int SDLOP_Bool;
typedef int SDLOP_Status;
typedef char *SDLOP_XPointer;

#define SDLOP_None           0L
#define SDLOP_CurrentTime    0L
#define SDLOP_GrabSuccess    0
#define SDLOP_AlreadyGrabbed 1
#define SDLOP_True           1
#define SDLOP_False          0

/* ------------------------------------------------------------------ */
/* Opaque libX11 types                                                 */
/* ------------------------------------------------------------------ */

typedef struct SDLOP_Display_ SDLOP_Display;      /* struct _XDisplay */
typedef struct SDLOP_Screen_ SDLOP_Screen;        /* struct _XScreen  */
typedef struct SDLOP_GC_ *SDLOP_GC;               /* struct _XGC*     */
typedef struct SDLOP_XExtData_ SDLOP_XExtData;

/* ------------------------------------------------------------------ */
/* Public frozen structs                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    SDLOP_XExtData *ext_data;
    SDLOP_VisualID visualid;
    int class_;
    unsigned long red_mask, green_mask, blue_mask;
    int bits_per_rgb;
    int map_entries;
} SDLOP_Visual;

typedef struct
{
    SDLOP_Visual *visual;
    SDLOP_VisualID visualid;
    int screen;
    int depth;
    int class_;
    unsigned long red_mask, green_mask, blue_mask;
    int colormap_size;
    int bits_per_rgb;
} SDLOP_XVisualInfo;

typedef struct
{
    SDLOP_Pixmap background_pixmap;
    unsigned long background_pixel;
    SDLOP_Pixmap border_pixmap;
    unsigned long border_pixel;
    int bit_gravity;
    int win_gravity;
    int backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    SDLOP_Bool save_under;
    long event_mask;
    long do_not_propagate_mask;
    SDLOP_Bool override_redirect;
    SDLOP_Colormap colormap;
    SDLOP_Cursor cursor;
} SDLOP_XSetWindowAttributes;

typedef struct
{
    int x, y;
    int width, height;
    int border_width;
    int depth;
    SDLOP_Visual *visual;
    SDLOP_Window root;
    int class_;
    int bit_gravity;
    int win_gravity;
    int backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    SDLOP_Bool save_under;
    SDLOP_Colormap colormap;
    SDLOP_Bool map_installed;
    int map_state;
    long all_event_masks;
    long your_event_mask;
    long do_not_propagate_mask;
    SDLOP_Bool override_redirect;
    SDLOP_Screen *screen;
} SDLOP_XWindowAttributes;

typedef struct
{
    long flags;
    int x, y;
    int width, height;
    int min_width, min_height;
    int max_width, max_height;
    int width_inc, height_inc;
    struct { int x; int y; } min_aspect, max_aspect;
    int base_width, base_height;
    int win_gravity;
} SDLOP_XSizeHints;

#define SDLOP_USPosition  (1L << 0)
#define SDLOP_USSize      (1L << 1)
#define SDLOP_PPosition   (1L << 2)
#define SDLOP_PSize       (1L << 3)
#define SDLOP_PMinSize    (1L << 4)
#define SDLOP_PMaxSize    (1L << 5)
#define SDLOP_PResizeInc  (1L << 6)
#define SDLOP_PAspect     (1L << 7)
#define SDLOP_PBaseSize   (1L << 8)
#define SDLOP_PWinGravity (1L << 9)

typedef struct
{
    unsigned long pixel;
    unsigned short red, green, blue;
    char flags;
    char pad;
} SDLOP_XColor;

typedef struct
{
    char *res_name;
    char *res_class;
} SDLOP_XClassHint;

/* XImage: public frozen ABI (Xlib.h); apps destroy via f.destroy_image,
 * which is exactly what XDestroyImage expands to. */
struct SDLOP_XImage_;
typedef struct SDLOP_XImage_
{
    int width, height;
    int xoffset;
    int format;
    char *data;
    int byte_order;
    int bitmap_unit;
    int bitmap_bit_order;
    int bitmap_pad;
    int depth;
    int bytes_per_line;
    int bits_per_pixel;
    unsigned long red_mask, green_mask, blue_mask;
    SDLOP_XPointer obdata;
    struct
    {
        struct SDLOP_XImage_ *(*create_image)(SDLOP_Display *, SDLOP_Visual *, unsigned int, int, int, char *, unsigned int, unsigned int, int, int);
        int (*destroy_image)(struct SDLOP_XImage_ *);
        unsigned long (*get_pixel)(struct SDLOP_XImage_ *, int, int);
        int (*put_pixel)(struct SDLOP_XImage_ *, int, int, unsigned long);
        struct SDLOP_XImage_ *(*sub_image)(struct SDLOP_XImage_ *, int, int, unsigned int, unsigned int);
        int (*add_pixel)(struct SDLOP_XImage_ *, long);
    } f;
} SDLOP_XImage;

#define SDLOP_XYBitmap 0
#define SDLOP_XYPixmap 1
#define SDLOP_ZPixmap  2

/* ------------------------------------------------------------------ */
/* Events (<X11/Xlib.h>)                                               */
/* ------------------------------------------------------------------ */

enum
{
    SDLOP_KeyPress = 2,
    SDLOP_KeyRelease = 3,
    SDLOP_ButtonPress = 4,
    SDLOP_ButtonRelease = 5,
    SDLOP_MotionNotify = 6,
    SDLOP_EnterNotify = 7,
    SDLOP_LeaveNotify = 8,
    SDLOP_FocusIn = 9,
    SDLOP_FocusOut = 10,
    SDLOP_KeymapNotify = 11,
    SDLOP_Expose = 12,
    SDLOP_GraphicsExpose = 13,
    SDLOP_NoExpose = 14,
    SDLOP_VisibilityNotify = 15,
    SDLOP_CreateNotify = 16,
    SDLOP_DestroyNotify = 17,
    SDLOP_UnmapNotify = 18,
    SDLOP_MapNotify = 19,
    SDLOP_MapRequest = 20,
    SDLOP_ReparentNotify = 21,
    SDLOP_ConfigureNotify = 22,
    SDLOP_ConfigureRequest = 23,
    SDLOP_GravityNotify = 24,
    SDLOP_ResizeRequest = 25,
    SDLOP_CirculateNotify = 26,
    SDLOP_CirculateRequest = 27,
    SDLOP_PropertyNotify = 28,
    SDLOP_SelectionClear = 29,
    SDLOP_SelectionRequest = 30,
    SDLOP_SelectionNotify = 31,
    SDLOP_ColormapNotify = 32,
    SDLOP_ClientMessage = 33,
    SDLOP_MappingNotify = 34
};

/* event masks */
#define SDLOP_KeyPressMask            (1L << 0)
#define SDLOP_KeyReleaseMask          (1L << 1)
#define SDLOP_ButtonPressMask         (1L << 2)
#define SDLOP_ButtonReleaseMask       (1L << 3)
#define SDLOP_EnterWindowMask         (1L << 4)
#define SDLOP_LeaveWindowMask         (1L << 5)
#define SDLOP_PointerMotionMask       (1L << 6)
#define SDLOP_PointerMotionHintMask   (1L << 7)
#define SDLOP_ButtonMotionMask        (1L << 13)
#define SDLOP_KeymapStateMask         (1L << 14)
#define SDLOP_ExposureMask            (1L << 15)
#define SDLOP_VisibilityChangeMask    (1L << 16)
#define SDLOP_StructureNotifyMask     (1L << 17)
#define SDLOP_ResizeRedirectMask      (1L << 18)
#define SDLOP_SubstructureNotifyMask  (1L << 19)
#define SDLOP_SubstructureRedirectMask (1L << 20)
#define SDLOP_FocusChangeMask         (1L << 21)
#define SDLOP_PropertyChangeMask      (1L << 22)

/* XCreateWindow value mask bits */
#define SDLOP_CWBackPixmap       (1L << 0)
#define SDLOP_CWBackPixel        (1L << 1)
#define SDLOP_CWBorderPixel      (1L << 3)
#define SDLOP_CWBitGravity       (1L << 4)
#define SDLOP_CWWinGravity       (1L << 5)
#define SDLOP_CWBackingStore     (1L << 6)
#define SDLOP_CWOverrideRedirect (1L << 9)
#define SDLOP_CWEventMask        (1L << 11)
#define SDLOP_CWColormap         (1L << 13)
#define SDLOP_CWCursor           (1L << 14)

/* ConfigureWindow value mask bits */
#define SDLOP_CWX           (1 << 0)
#define SDLOP_CWY           (1 << 1)
#define SDLOP_CWWidth       (1 << 2)
#define SDLOP_CWHeight      (1 << 3)
#define SDLOP_CWBorderWidth (1 << 4)
#define SDLOP_CWSibling     (1 << 5)
#define SDLOP_CWStackMode   (1 << 6)

#define SDLOP_Above    0
#define SDLOP_Below    1
#define SDLOP_TopIf    2
#define SDLOP_BottomIf 3
#define SDLOP_Opposite 4

/* window class */
#define SDLOP_CopyFromParent 0
#define SDLOP_InputOutput    1
#define SDLOP_InputOnly      2

/* map_state */
#define SDLOP_IsUnmapped   0
#define SDLOP_IsUnviewable 1
#define SDLOP_IsViewable   2

/* visibility */
#define SDLOP_VisibilityUnobscured        0
#define SDLOP_VisibilityPartiallyObscured 1
#define SDLOP_VisibilityFullyObscured     2

/* grab / focus modes */
#define SDLOP_GrabModeSync  0
#define SDLOP_GrabModeAsync 1
#define SDLOP_RevertToParent 2
#define SDLOP_RevertToNone   1

/* notify modes */
#define SDLOP_NotifyNormal       0
#define SDLOP_NotifyGrab         1
#define SDLOP_NotifyUngrab       2
#define SDLOP_NotifyWhileGrabbed 3

/* notify detail */
#define SDLOP_NotifyAncestor         0
#define SDLOP_NotifyVirtual          1
#define SDLOP_NotifyInferior         2
#define SDLOP_NotifyNonlinear        3
#define SDLOP_NotifyNonlinearVirtual 4
#define SDLOP_NotifyPointer          5
#define SDLOP_NotifyPointerRoot      6
#define SDLOP_NotifyDetailNone       7

/* PropertyNotify state */
#define SDLOP_PropertyNewValue 0
#define SDLOP_PropertyDelete   1

/* property change modes */
#define SDLOP_PropModeReplace 0
#define SDLOP_PropModePrepend 1
#define SDLOP_PropModeAppend  2

/* MappingNotify request */
#define SDLOP_MappingModifier 0
#define SDLOP_MappingKeyboard 1
#define SDLOP_MappingPointer  2

/* modifier key state bits */
#define SDLOP_ShiftMask   (1 << 0)
#define SDLOP_LockMask    (1 << 1)
#define SDLOP_ControlMask (1 << 2)
#define SDLOP_Mod1Mask    (1 << 3)
#define SDLOP_Mod2Mask    (1 << 4)
#define SDLOP_Mod3Mask    (1 << 5)
#define SDLOP_Mod4Mask    (1 << 6)
#define SDLOP_Mod5Mask    (1 << 7)
#define SDLOP_Button1Mask (1 << 8)
#define SDLOP_Button2Mask (1 << 9)
#define SDLOP_Button3Mask (1 << 10)
#define SDLOP_Button4Mask (1 << 11)
#define SDLOP_Button5Mask (1 << 12)

/* mouse button numbers */
#define SDLOP_Button1 1
#define SDLOP_Button2 2
#define SDLOP_Button3 3
#define SDLOP_Button4 4
#define SDLOP_Button5 5

/* XEventsQueued modes */
#define SDLOP_QueuedAlready     0
#define SDLOP_QueuedAfterFlush  1
#define SDLOP_QueuedAfterReading 2

/* predefined atoms we actually use (all others are interned by name) */
#define SDLOP_XA_PRIMARY     1
#define SDLOP_XA_ATOM        4
#define SDLOP_XA_CARDINAL    6
#define SDLOP_XA_STRING      31
#define SDLOP_XA_WM_NAME     39
#define SDLOP_XA_WM_CLASS    67

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window window;
} SDLOP_XAnyEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window window;
    SDLOP_Window root;
    SDLOP_Window subwindow;
    SDLOP_Time time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    unsigned int keycode;
    SDLOP_Bool same_screen;
} SDLOP_XKeyEvent;
typedef SDLOP_XKeyEvent SDLOP_XKeyPressedEvent;
typedef SDLOP_XKeyEvent SDLOP_XKeyReleasedEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window window;
    SDLOP_Window root;
    SDLOP_Window subwindow;
    SDLOP_Time time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    unsigned int button;
    SDLOP_Bool same_screen;
} SDLOP_XButtonEvent;
typedef SDLOP_XButtonEvent SDLOP_XButtonPressedEvent;
typedef SDLOP_XButtonEvent SDLOP_XButtonReleasedEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window window;
    SDLOP_Window root;
    SDLOP_Window subwindow;
    SDLOP_Time time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    char is_hint;
    SDLOP_Bool same_screen;
} SDLOP_XMotionEvent;
typedef SDLOP_XMotionEvent SDLOP_XPointerMovedEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window window;
    SDLOP_Window root;
    SDLOP_Window subwindow;
    SDLOP_Time time;
    int x, y;
    int x_root, y_root;
    int mode;
    int detail;
    SDLOP_Bool same_screen;
    SDLOP_Bool focus;
    unsigned int state;
} SDLOP_XCrossingEvent;
typedef SDLOP_XCrossingEvent SDLOP_XEnterWindowEvent;
typedef SDLOP_XCrossingEvent SDLOP_XLeaveWindowEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window window;
    int mode;
    int detail;
} SDLOP_XFocusChangeEvent;
typedef SDLOP_XFocusChangeEvent SDLOP_XFocusInEvent;
typedef SDLOP_XFocusChangeEvent SDLOP_XFocusOutEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window window;
    int x, y;
    int width, height;
    int count;
} SDLOP_XExposeEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window window;
    int state;
} SDLOP_XVisibilityEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window parent;
    SDLOP_Window window;
    int x, y;
    int width, height;
    int border_width;
    SDLOP_Bool override_redirect;
} SDLOP_XCreateWindowEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window event;
    SDLOP_Window window;
} SDLOP_XDestroyWindowEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window event;
    SDLOP_Window window;
    SDLOP_Bool from_configure;
} SDLOP_XUnmapEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window event;
    SDLOP_Window window;
    SDLOP_Bool override_redirect;
} SDLOP_XMapEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window event;
    SDLOP_Window window;
    SDLOP_Window parent;
    int x, y;
    SDLOP_Bool override_redirect;
} SDLOP_XReparentEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window event;
    SDLOP_Window window;
    int x, y;
    int width, height;
    int border_width;
    SDLOP_Window above;
    SDLOP_Bool override_redirect;
} SDLOP_XConfigureEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window window;
    SDLOP_Atom atom;
    SDLOP_Time time;
    int state;
} SDLOP_XPropertyEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window window;
    SDLOP_Atom message_type;
    int format;
    union
    {
        char b[20];
        short s[10];
        long l[5];
    } data;
} SDLOP_XClientMessageEvent;

typedef struct
{
    int type;
    unsigned long serial;
    SDLOP_Bool send_event;
    SDLOP_Display *display;
    SDLOP_Window window;
    int request;
    int first_keycode;
    int count;
} SDLOP_XMappingEvent;

typedef struct
{
    int type;
    SDLOP_Display *display;
    SDLOP_XID resourceid;
    unsigned long serial;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
} SDLOP_XErrorEvent;

typedef union SDLOP_XEvent_
{
    int type;
    SDLOP_XAnyEvent xany;
    SDLOP_XKeyEvent xkey;
    SDLOP_XButtonEvent xbutton;
    SDLOP_XMotionEvent xmotion;
    SDLOP_XCrossingEvent xcrossing;
    SDLOP_XFocusChangeEvent xfocus;
    SDLOP_XExposeEvent xexpose;
    SDLOP_XVisibilityEvent xvisibility;
    SDLOP_XCreateWindowEvent xcreatewindow;
    SDLOP_XDestroyWindowEvent xdestroywindow;
    SDLOP_XUnmapEvent xunmap;
    SDLOP_XMapEvent xmap;
    SDLOP_XReparentEvent xreparent;
    SDLOP_XConfigureEvent xconfigure;
    SDLOP_XPropertyEvent xproperty;
    SDLOP_XClientMessageEvent xclient;
    SDLOP_XMappingEvent xmapping;
    SDLOP_XErrorEvent xerror;
    long pad[24]; /* XEvent is 192 bytes on LP64 */
} SDLOP_XEvent;

typedef int (*SDLOP_XErrorHandler)(SDLOP_Display *, SDLOP_XErrorEvent *);
typedef int (*SDLOP_XIOErrorHandler)(SDLOP_Display *);

/* ------------------------------------------------------------------ */
/* XKB client-library bits (XKBlib lives inside libX11)                */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char group;
    unsigned char locked_group;
    unsigned short base_group;
    unsigned short latched_group;
    unsigned char mods;
    unsigned char base_mods;
    unsigned short latched_mods;
    unsigned short locked_mods;
    unsigned char compat_state;
    unsigned char grab_mods;
    unsigned char compat_grab_mods;
    unsigned char lookup_mods;
    unsigned char compat_lookup_mods;
    unsigned short ptr_buttons;
} SDLOP_XkbStateRec;

#define SDLOP_XkbUseCoreKbd 0x0100

#ifdef __cplusplus
}
#endif

#endif /* sdlop_x11_defs_h_ */
