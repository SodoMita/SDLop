/* SDLop - libX11 symbol list (X-macro; included multiple times with
 * different SDLOP_X11_SYM definitions; deliberately no include guard) */

/* connection */
SDLOP_X11_SYM(SDLOP_Display *, XOpenDisplay, (const char *))
SDLOP_X11_SYM(int, XCloseDisplay, (SDLOP_Display *))
SDLOP_X11_SYM(SDLOP_XErrorHandler, XSetErrorHandler, (SDLOP_XErrorHandler))
SDLOP_X11_SYM(SDLOP_XIOErrorHandler, XSetIOErrorHandler, (SDLOP_XIOErrorHandler))
SDLOP_X11_SYM(int, XConnectionNumber, (SDLOP_Display *))
SDLOP_X11_SYM(int, XFlush, (SDLOP_Display *))
SDLOP_X11_SYM(int, XSync, (SDLOP_Display *, SDLOP_Bool))
SDLOP_X11_SYM(int, XPending, (SDLOP_Display *))
SDLOP_X11_SYM(int, XEventsQueued, (SDLOP_Display *, int))
SDLOP_X11_SYM(int, XNextEvent, (SDLOP_Display *, SDLOP_XEvent *))
SDLOP_X11_SYM(int, XPeekEvent, (SDLOP_Display *, SDLOP_XEvent *))
SDLOP_X11_SYM(int, XFree, (void *))
SDLOP_X11_SYM(int, XGetErrorText, (SDLOP_Display *, int, char *, int))
SDLOP_X11_SYM(int, XDisplayKeycodes, (SDLOP_Display *, int *, int *))

/* screen / display info (all real libX11 functions - no struct access) */
SDLOP_X11_SYM(int, XDefaultScreen, (SDLOP_Display *))
SDLOP_X11_SYM(SDLOP_Window, XDefaultRootWindow, (SDLOP_Display *))
SDLOP_X11_SYM(SDLOP_Window, XRootWindow, (SDLOP_Display *, int))
SDLOP_X11_SYM(int, XDisplayWidth, (SDLOP_Display *, int))
SDLOP_X11_SYM(int, XDisplayHeight, (SDLOP_Display *, int))
SDLOP_X11_SYM(int, XDisplayWidthMM, (SDLOP_Display *, int))
SDLOP_X11_SYM(int, XDisplayHeightMM, (SDLOP_Display *, int))
SDLOP_X11_SYM(int, XDefaultDepth, (SDLOP_Display *, int))
SDLOP_X11_SYM(SDLOP_Visual *, XDefaultVisual, (SDLOP_Display *, int))
SDLOP_X11_SYM(unsigned long, XBlackPixel, (SDLOP_Display *, int))
SDLOP_X11_SYM(unsigned long, XWhitePixel, (SDLOP_Display *, int))
SDLOP_X11_SYM(char *, XDisplayString, (SDLOP_Display *))
SDLOP_X11_SYM(char *, XServerVendor, (SDLOP_Display *))
SDLOP_X11_SYM(SDLOP_Bool, XQueryExtension, (SDLOP_Display *, const char *, int *, int *, int *))

/* windows */
SDLOP_X11_SYM(SDLOP_Window, XCreateWindow, (SDLOP_Display *, SDLOP_Window, int, int, unsigned int, unsigned int, unsigned int, int, unsigned int, SDLOP_Visual *, unsigned long, SDLOP_XSetWindowAttributes *))
SDLOP_X11_SYM(SDLOP_Window, XCreateSimpleWindow, (SDLOP_Display *, SDLOP_Window, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long))
SDLOP_X11_SYM(int, XDestroyWindow, (SDLOP_Display *, SDLOP_Window))
SDLOP_X11_SYM(int, XMapWindow, (SDLOP_Display *, SDLOP_Window))
SDLOP_X11_SYM(int, XMapRaised, (SDLOP_Display *, SDLOP_Window))
SDLOP_X11_SYM(int, XUnmapWindow, (SDLOP_Display *, SDLOP_Window))
SDLOP_X11_SYM(int, XMoveWindow, (SDLOP_Display *, SDLOP_Window, int, int))
SDLOP_X11_SYM(int, XResizeWindow, (SDLOP_Display *, SDLOP_Window, unsigned int, unsigned int))
SDLOP_X11_SYM(int, XMoveResizeWindow, (SDLOP_Display *, SDLOP_Window, int, int, unsigned int, unsigned int))
SDLOP_X11_SYM(int, XRaiseWindow, (SDLOP_Display *, SDLOP_Window))
SDLOP_X11_SYM(SDLOP_Status, XIconifyWindow, (SDLOP_Display *, SDLOP_Window, int))
SDLOP_X11_SYM(int, XChangeWindowAttributes, (SDLOP_Display *, SDLOP_Window, unsigned long, SDLOP_XSetWindowAttributes *))
SDLOP_X11_SYM(int, XSelectInput, (SDLOP_Display *, SDLOP_Window, long))
SDLOP_X11_SYM(SDLOP_Status, XGetWindowAttributes, (SDLOP_Display *, SDLOP_Window, SDLOP_XWindowAttributes *))
SDLOP_X11_SYM(SDLOP_Bool, XTranslateCoordinates, (SDLOP_Display *, SDLOP_Window, SDLOP_Window, int, int, int *, int *, SDLOP_Window *))
SDLOP_X11_SYM(SDLOP_Bool, XQueryPointer, (SDLOP_Display *, SDLOP_Window, SDLOP_Window *, SDLOP_Window *, int *, int *, int *, int *, unsigned int *))

/* properties / atoms */
SDLOP_X11_SYM(SDLOP_Atom, XInternAtom, (SDLOP_Display *, const char *, SDLOP_Bool))
SDLOP_X11_SYM(int, XChangeProperty, (SDLOP_Display *, SDLOP_Window, SDLOP_Atom, SDLOP_Atom, int, int, const unsigned char *, int))
SDLOP_X11_SYM(int, XGetWindowProperty, (SDLOP_Display *, SDLOP_Window, SDLOP_Atom, long, long, SDLOP_Bool, SDLOP_Atom, SDLOP_Atom *, int *, unsigned long *, unsigned long *, unsigned char **))
SDLOP_X11_SYM(int, XDeleteProperty, (SDLOP_Display *, SDLOP_Window, SDLOP_Atom))
SDLOP_X11_SYM(int, XStoreName, (SDLOP_Display *, SDLOP_Window, const char *))
SDLOP_X11_SYM(SDLOP_Status, XSetWMProtocols, (SDLOP_Display *, SDLOP_Window, SDLOP_Atom *, int))
SDLOP_X11_SYM(void, XSetWMNormalHints, (SDLOP_Display *, SDLOP_Window, SDLOP_XSizeHints *))
SDLOP_X11_SYM(void, XSetClassHint, (SDLOP_Display *, SDLOP_Window, SDLOP_XClassHint *))
SDLOP_X11_SYM(SDLOP_Status, XSendEvent, (SDLOP_Display *, SDLOP_Window, SDLOP_Bool, long, SDLOP_XEvent *))

/* input focus + grabs */
SDLOP_X11_SYM(int, XSetInputFocus, (SDLOP_Display *, SDLOP_Window, int, SDLOP_Time))
SDLOP_X11_SYM(int, XGetInputFocus, (SDLOP_Display *, SDLOP_Window *, int *))
SDLOP_X11_SYM(int, XGrabPointer, (SDLOP_Display *, SDLOP_Window, SDLOP_Bool, unsigned int, int, int, SDLOP_Window, SDLOP_Cursor, SDLOP_Time))
SDLOP_X11_SYM(int, XUngrabPointer, (SDLOP_Display *, SDLOP_Time))
SDLOP_X11_SYM(int, XWarpPointer, (SDLOP_Display *, SDLOP_Window, SDLOP_Window, int, int, unsigned int, unsigned int, int, int))

/* cursors */
SDLOP_X11_SYM(SDLOP_Cursor, XCreateFontCursor, (SDLOP_Display *, unsigned int))
SDLOP_X11_SYM(SDLOP_Cursor, XCreatePixmapCursor, (SDLOP_Display *, SDLOP_Pixmap, SDLOP_Pixmap, SDLOP_XColor *, SDLOP_XColor *, unsigned int, unsigned int))
SDLOP_X11_SYM(int, XFreeCursor, (SDLOP_Display *, SDLOP_Cursor))
SDLOP_X11_SYM(int, XDefineCursor, (SDLOP_Display *, SDLOP_Window, SDLOP_Cursor))
SDLOP_X11_SYM(int, XUndefineCursor, (SDLOP_Display *, SDLOP_Window))

/* pixmaps / GC / images (software framebuffer) */
SDLOP_X11_SYM(SDLOP_Pixmap, XCreatePixmap, (SDLOP_Display *, SDLOP_Drawable, unsigned int, unsigned int, unsigned int))
SDLOP_X11_SYM(int, XFreePixmap, (SDLOP_Display *, SDLOP_Pixmap))
SDLOP_X11_SYM(SDLOP_Colormap, XCreateColormap, (SDLOP_Display *, SDLOP_Window, SDLOP_Visual *, int))
SDLOP_X11_SYM(int, XFreeColormap, (SDLOP_Display *, SDLOP_Colormap))
SDLOP_X11_SYM(SDLOP_GC, XCreateGC, (SDLOP_Display *, SDLOP_Drawable, unsigned long, void *))
SDLOP_X11_SYM(int, XFreeGC, (SDLOP_Display *, SDLOP_GC))
SDLOP_X11_SYM(SDLOP_XImage *, XCreateImage, (SDLOP_Display *, SDLOP_Visual *, unsigned int, int, int, char *, unsigned int, unsigned int, int, int))
SDLOP_X11_SYM(int, XPutImage, (SDLOP_Display *, SDLOP_Drawable, SDLOP_GC, SDLOP_XImage *, int, int, int, int, unsigned int, unsigned int))

/* XKB client library (shipped inside libX11) */
SDLOP_X11_SYM(SDLOP_Bool, XkbQueryExtension, (SDLOP_Display *, int *, int *, int *, int *, int *))
SDLOP_X11_SYM(SDLOP_Status, XkbGetState, (SDLOP_Display *, unsigned int, SDLOP_XkbStateRec *))
