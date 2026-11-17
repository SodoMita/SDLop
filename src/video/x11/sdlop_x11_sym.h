/*
 * Xlib entry points used by SDLop's X11 backend.
 *
 * Deliberately has no include guard: included once for declarations and once
 * for definitions/loading, following the SDL3 dynamic-symbol-list pattern.
 */
SDLOP_X11_SYM(Status, XInitThreads, (void))
SDLOP_X11_SYM(Display *, XOpenDisplay, (const char *))
SDLOP_X11_SYM(int, XCloseDisplay, (Display *))
SDLOP_X11_SYM(int, XDefaultScreen, (Display *))
SDLOP_X11_SYM(Window, XRootWindow, (Display *, int))
SDLOP_X11_SYM(Visual *, XDefaultVisual, (Display *, int))
SDLOP_X11_SYM(int, XDefaultDepth, (Display *, int))
SDLOP_X11_SYM(int, XDisplayWidth, (Display *, int))
SDLOP_X11_SYM(int, XDisplayHeight, (Display *, int))
SDLOP_X11_SYM(int, XConnectionNumber, (Display *))

SDLOP_X11_SYM(Window, XCreateSimpleWindow,
              (Display *, Window, int, int, unsigned int, unsigned int,
               unsigned int, unsigned long, unsigned long))
SDLOP_X11_SYM(int, XDestroyWindow, (Display *, Window))
SDLOP_X11_SYM(int, XSelectInput, (Display *, Window, long))
SDLOP_X11_SYM(int, XMapWindow, (Display *, Window))
SDLOP_X11_SYM(int, XUnmapWindow, (Display *, Window))
SDLOP_X11_SYM(int, XRaiseWindow, (Display *, Window))
SDLOP_X11_SYM(int, XSetInputFocus, (Display *, Window, int, Time))
SDLOP_X11_SYM(int, XStoreName, (Display *, Window, const char *))
SDLOP_X11_SYM(Atom, XInternAtom, (Display *, const char *, Bool))
SDLOP_X11_SYM(Status, XSetWMProtocols, (Display *, Window, Atom *, int))
SDLOP_X11_SYM(int, XSetWMHints, (Display *, Window, XWMHints *))
SDLOP_X11_SYM(int, XSetWMNormalHints, (Display *, Window, XSizeHints *))
SDLOP_X11_SYM(int, XChangeProperty,
              (Display *, Window, Atom, Atom, int, int,
               const unsigned char *, int))
SDLOP_X11_SYM(Status, XSendEvent, (Display *, Window, Bool, long, XEvent *))
SDLOP_X11_SYM(int, XMoveResizeWindow,
              (Display *, Window, int, int, unsigned int, unsigned int))
SDLOP_X11_SYM(Status, XIconifyWindow, (Display *, Window, int))
SDLOP_X11_SYM(Window, XGetSelectionOwner, (Display *, Atom))
SDLOP_X11_SYM(int, XGetWindowProperty,
              (Display *, Window, Atom, long, long, Bool, Atom, Atom *, int *,
               unsigned long *, unsigned long *, unsigned char **))
SDLOP_X11_SYM(int, XFree, (void *))
SDLOP_X11_SYM(Status, XTranslateCoordinates,
              (Display *, Window, Window, int, int, int *, int *, Window *))

SDLOP_X11_SYM(int, XFlush, (Display *))
SDLOP_X11_SYM(int, XSync, (Display *, Bool))
SDLOP_X11_SYM(int, XPending, (Display *))
SDLOP_X11_SYM(int, XNextEvent, (Display *, XEvent *))
SDLOP_X11_SYM(int, XPeekEvent, (Display *, XEvent *))

SDLOP_X11_SYM(GC, XCreateGC, (Display *, Drawable, unsigned long, XGCValues *))
SDLOP_X11_SYM(int, XFreeGC, (Display *, GC))
SDLOP_X11_SYM(XImage *, XCreateImage,
              (Display *, Visual *, unsigned int, int, int, char *,
               unsigned int, unsigned int, int, int))
SDLOP_X11_SYM(int, XDestroyImage, (XImage *))
SDLOP_X11_SYM(int, XPutImage,
              (Display *, Drawable, GC, XImage *, int, int, int, int,
               unsigned int, unsigned int))

SDLOP_X11_SYM(Pixmap, XCreateBitmapFromData,
              (Display *, Drawable, const char *, unsigned int, unsigned int))
SDLOP_X11_SYM(Cursor, XCreatePixmapCursor,
              (Display *, Pixmap, Pixmap, XColor *, XColor *, unsigned int,
               unsigned int))
SDLOP_X11_SYM(int, XFreePixmap, (Display *, Pixmap))
SDLOP_X11_SYM(int, XFreeCursor, (Display *, Cursor))
SDLOP_X11_SYM(int, XDefineCursor, (Display *, Window, Cursor))
SDLOP_X11_SYM(int, XGrabPointer,
              (Display *, Window, Bool, unsigned int, int, int, Window,
               Cursor, Time))
SDLOP_X11_SYM(int, XUngrabPointer, (Display *, Time))
SDLOP_X11_SYM(int, XWarpPointer,
              (Display *, Window, Window, int, int, unsigned int, unsigned int,
               int, int))

SDLOP_X11_SYM(Bool, XkbSetDetectableAutoRepeat, (Display *, Bool, Bool *))
