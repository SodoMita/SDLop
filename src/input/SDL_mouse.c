/*
  SDLop -- SDL_mouse.h: mouse state, cursors and mouse modes.

  Position bookkeeping follows SDL3: absolute coordinates are relative to the
  focused window, global coordinates are relative to the desktop origin, and the
  relative state is the movement accumulated since the last call to
  SDL_GetRelativeMouseState().

  Cursors are (data, mask) 1-bit images or ARGB images. System cursors are named
  handles: the backend turns the name into whatever the platform offers (an X11
  font cursor, a wp_cursor_shape name, ...) and falls back to a built-in arrow.
*/

#include "../sdlop_internal.h"

#include <string.h>

static float sdlop_mouse_x;
/* SDL3 distinguishes "the pointer has never been seen" from "it is at 0,0": the
   first absolute motion has no delta to report, and a motion that does not change
   the position produces no event at all. */
static bool sdlop_mouse_has_position;
static float sdlop_mouse_y;
static float sdlop_mouse_global_x;
static float sdlop_mouse_global_y;
static float sdlop_mouse_rel_x;
static float sdlop_mouse_rel_y;
static SDL_MouseButtonFlags sdlop_mouse_buttons;
static bool sdlop_cursor_visible = true;
static SDL_Cursor *sdlop_cursors;
static SDL_Cursor *sdlop_current_cursor;
static SDL_Cursor *sdlop_default_cursor;

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y)
{
    if (x) {
        *x = sdlop_mouse_x;
    }
    if (y) {
        *y = sdlop_mouse_y;
    }
    return sdlop_mouse_buttons;
}

SDL_MouseButtonFlags SDL_GetGlobalMouseState(float *x, float *y)
{
    SDL_Window *focus = SDLOP_GetMouseFocusWindow();
    if (x) {
        *x = sdlop_mouse_global_x;
    }
    if (y) {
        *y = sdlop_mouse_global_y;
    }
    (void)focus;
    return sdlop_mouse_buttons;
}

SDL_MouseButtonFlags SDL_GetRelativeMouseState(float *x, float *y)
{
    if (x) {
        *x = sdlop_mouse_rel_x;
    }
    if (y) {
        *y = sdlop_mouse_rel_y;
    }
    sdlop_mouse_rel_x = 0.0f;
    sdlop_mouse_rel_y = 0.0f;
    return sdlop_mouse_buttons;
}

void SDL_WarpMouseInWindow(SDL_Window *window, float x, float y)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();

    if (!window) {
        SDL_InvalidParamError("window");
        return;
    }
    if (driver && driver->warp_mouse) {
        driver->warp_mouse(window, x, y);
    }
    sdlop_mouse_x = x;
    sdlop_mouse_y = y;
}

bool SDL_WarpMouseGlobal(float x, float y)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();
    SDL_Window *window = SDLOP_GetMouseFocusWindow();

    sdlop_mouse_global_x = x;
    sdlop_mouse_global_y = y;
    if (!window) {
        return SDL_SetError("No window has mouse focus");
    }
    if (driver && driver->warp_mouse) {
        return driver->warp_mouse(window, x - (float)window->x, y - (float)window->y);
    }
    return true;
}

bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (driver && driver->set_relative_mouse_mode) {
        if (!driver->set_relative_mouse_mode(window, enabled)) {
            return false;
        }
    }
    if (enabled) {
        window->flags |= SDL_WINDOW_MOUSE_RELATIVE_MODE;
    } else {
        window->flags &= ~SDL_WINDOW_MOUSE_RELATIVE_MODE;
    }
    SDLOP_SetRelativeMouseMode(enabled);
    return true;
}

bool SDL_GetWindowRelativeMouseMode(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return false;
    }
    return (window->flags & SDL_WINDOW_MOUSE_RELATIVE_MODE) != 0;
}

bool SDL_CaptureMouse(bool enabled)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();

    if (driver && driver->capture_mouse && !driver->capture_mouse(enabled)) {
        return false;
    }
    SDLOP_SetMouseCapture(enabled);
    return true;
}

void SDLOP_ClearMouseButtons(void)
{
    sdlop_mouse_buttons = 0;
}

/* ------------------------------------------------------------------------- */
/* Events (called by the input translation and by the video backends)         */
/* ------------------------------------------------------------------------- */

void SDLOP_SendMouseMotionAbsolute(SDL_WindowID windowID, float x, float y, Uint64 timestamp)
{
    SDL_Event event;
    SDL_Window *window = SDLOP_GetWindowFromIDInternal(windowID);
    float xrel = 0.0f, yrel = 0.0f;

    if (sdlop_mouse_has_position) {
        /* An absolute move still carries a delta, which is the difference from
           the previous position (SDL3 does the same), so an application may
           integrate xrel/yrel on any backend. */
        xrel = x - sdlop_mouse_x;
        yrel = y - sdlop_mouse_y;
        if (xrel == 0.0f && yrel == 0.0f) {
            return;   /* nothing changed, and SDL3 sends no event for it */
        }
    }

    sdlop_mouse_x = x;
    sdlop_mouse_y = y;
    sdlop_mouse_has_position = true;
    if (window) {
        sdlop_mouse_global_x = (float)window->x + x;
        sdlop_mouse_global_y = (float)window->y + y;
    }

    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.timestamp = timestamp;
    event.motion.windowID = windowID;
    event.motion.which = 1;
    event.motion.state = sdlop_mouse_buttons;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.xrel = xrel;
    event.motion.yrel = yrel;
    SDLOP_PushEvent(&event);
}

void SDLOP_SendMouseMotionRelative(SDL_WindowID windowID, float dx, float dy, Uint64 timestamp)
{
    SDL_Event event;
    SDL_Window *window = SDLOP_GetWindowFromIDInternal(windowID);

    sdlop_mouse_has_position = true;
    sdlop_mouse_rel_x += dx;
    sdlop_mouse_rel_y += dy;
    if (window && !(window->flags & SDL_WINDOW_MOUSE_RELATIVE_MODE)) {
        /* pointer is being moved by the OS: keep our idea of the position in sync */
        sdlop_mouse_x += dx;
        sdlop_mouse_y += dy;
        if (sdlop_mouse_x < 0) sdlop_mouse_x = 0;
        if (sdlop_mouse_y < 0) sdlop_mouse_y = 0;
        if (sdlop_mouse_x > (float)window->w) sdlop_mouse_x = (float)window->w;
        if (sdlop_mouse_y > (float)window->h) sdlop_mouse_y = (float)window->h;
    }

    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.timestamp = timestamp;
    event.motion.windowID = windowID;
    event.motion.which = 1;
    event.motion.state = sdlop_mouse_buttons;
    event.motion.x = sdlop_mouse_x;
    event.motion.y = sdlop_mouse_y;
    event.motion.xrel = dx;
    event.motion.yrel = dy;
    SDLOP_PushEvent(&event);
}

void SDLOP_SendMouseButton(SDL_MouseID mouseID, Uint8 button, bool down, float x, float y,
                           Uint64 timestamp)
{
    SDL_Event event;
    SDL_MouseButtonFlags mask = 0;
    SDL_Window *focus = SDLOP_GetMouseFocusWindow();

    switch (button) {
        case SDL_BUTTON_LEFT: mask = SDL_BUTTON_LMASK; break;
        case SDL_BUTTON_MIDDLE: mask = SDL_BUTTON_MMASK; break;
        case SDL_BUTTON_RIGHT: mask = SDL_BUTTON_RMASK; break;
        case SDL_BUTTON_X1: mask = SDL_BUTTON_X1MASK; break;
        case SDL_BUTTON_X2: mask = SDL_BUTTON_X2MASK; break;
        default: break;
    }

    if (down) {
        if (sdlop_mouse_buttons & mask) {
            return;                    /* already down, ignore the duplicate */
        }
        sdlop_mouse_buttons |= mask;
    } else {
        if (!(sdlop_mouse_buttons & mask)) {
            return;
        }
        sdlop_mouse_buttons &= ~mask;
    }
    if (x >= 0.0f) {
        sdlop_mouse_x = x;
        sdlop_mouse_y = y;
    }

    memset(&event, 0, sizeof(event));
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.timestamp = timestamp;
    event.button.windowID = focus ? focus->id : 0;
    event.button.which = mouseID;
    event.button.button = button;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = sdlop_mouse_x;
    event.button.y = sdlop_mouse_y;
    SDLOP_PushEvent(&event);
}

void SDLOP_SendMouseWheel(SDL_WindowID windowID, float dx, float dy, SDL_MouseWheelDirection dir,
                          Uint64 timestamp)
{
    SDL_Event event;
    SDL_Window *window = SDLOP_GetWindowFromIDInternal(windowID);

    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.timestamp = timestamp;
    event.wheel.windowID = windowID;
    event.wheel.which = 1;
    event.wheel.x = dx;
    event.wheel.y = dy;
    event.wheel.direction = dir;
    event.wheel.mouse_x = sdlop_mouse_x;
    event.wheel.mouse_y = sdlop_mouse_y;
    (void)window;
    SDLOP_PushEvent(&event);
}

void SDLOP_SendWindowFocusEvents(SDL_WindowID id, bool keyboard, bool mouse, Uint64 timestamp)
{
    SDL_Event event;
    SDL_Window *window = SDLOP_GetWindowFromIDInternal(id);

    if (keyboard && window) {
        SDLOP_SetKeyboardFocus(window);
    }
    (void)mouse;
    (void)timestamp;
    memset(&event, 0, sizeof(event));
}

/* ------------------------------------------------------------------------- */
/* Cursors                                                                   */
/* ------------------------------------------------------------------------- */

SDL_Cursor *SDLOP_CreateCursorFromData(const Uint8 *data, const Uint8 *mask, int w, int h,
                                       int hot_x, int hot_y)
{
    SDL_Cursor *cursor;
    Uint8 *pixels;
    int x, y;

    if (!data || w <= 0 || h <= 0) {
        SDL_InvalidParamError(data ? (h <= 0 ? "h" : "w") : "data");
        return NULL;
    }
    cursor = (SDL_Cursor *)SDLOP_Calloc(1, sizeof(*cursor));
    if (!cursor) {
        SDL_OutOfMemory();
        return NULL;
    }
    pixels = (Uint8 *)SDLOP_Alloc((size_t)w * (size_t)h * 4);
    if (!pixels) {
        SDLOP_Free(cursor);
        SDL_OutOfMemory();
        return NULL;
    }
    /* SDL3's 1-bit cursor format is MSB-first, row by row */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            Uint8 byte = data[y * ((w + 7) / 8) + x / 8];
            bool on = (byte & (0x80 >> (x % 8))) != 0;
            bool opaque = true;
            Uint8 *dst = pixels + ((size_t)y * w + x) * 4;
            if (mask) {
                Uint8 mbyte = mask[y * ((w + 7) / 8) + x / 8];
                opaque = (mbyte & (0x80 >> (x % 8))) != 0;
            }
            dst[0] = 0xFF;
            dst[1] = 0xFF;
            dst[2] = 0xFF;
            dst[3] = on ? 0xFF : 0x00;
            if (!opaque) {
                dst[3] = 0x00;
            }
        }
    }
    cursor->data = pixels;
    cursor->w = w;
    cursor->h = h;
    cursor->hot_x = hot_x;
    cursor->hot_y = hot_y;
    cursor->system = SDL_SYSTEM_CURSOR_DEFAULT;
    cursor->next = sdlop_cursors;
    sdlop_cursors = cursor;
    return cursor;
}

SDL_Cursor *SDL_CreateCursor(const Uint8 *data, const Uint8 *mask, int w, int h, int hot_x, int hot_y)
{
    return SDLOP_CreateCursorFromData(data, mask, w, h, hot_x, hot_y);
}

SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *surface, int hot_x, int hot_y)
{
    SDL_Cursor *cursor;
    Uint32 *dst;
    int x, y;

    if (!surface || !surface->pixels) {
        SDL_InvalidParamError("surface");
        return NULL;
    }
    cursor = (SDL_Cursor *)SDLOP_Calloc(1, sizeof(*cursor));
    if (!cursor) {
        SDL_OutOfMemory();
        return NULL;
    }
    cursor->w = surface->w;
    cursor->h = surface->h;
    cursor->hot_x = hot_x;
    cursor->hot_y = hot_y;
    cursor->data = (Uint8 *)SDLOP_Alloc((size_t)surface->w * (size_t)surface->h * 4);
    if (!cursor->data) {
        SDLOP_Free(cursor);
        SDL_OutOfMemory();
        return NULL;
    }
    dst = (Uint32 *)cursor->data;
    for (y = 0; y < surface->h; y++) {
        const Uint8 *src = (const Uint8 *)surface->pixels + (size_t)y * surface->pitch;
        for (x = 0; x < surface->w; x++) {
            Uint8 r, g, b, a;
            SDL_GetRGBA(*(const Uint32 *)(const void *)(src + (size_t)x * 4),
                        SDL_GetPixelFormatDetails(surface->format), NULL, &r, &g, &b, &a);
            dst[(size_t)y * surface->w + x] = ((Uint32)a << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | b;
        }
    }
    cursor->next = sdlop_cursors;
    sdlop_cursors = cursor;
    return cursor;
}

SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id)
{
    SDL_Cursor *cursor;

    if (id < SDL_SYSTEM_CURSOR_DEFAULT || id >= SDL_SYSTEM_CURSOR_COUNT) {
        SDL_InvalidParamError("id");
        return NULL;
    }
    cursor = (SDL_Cursor *)SDLOP_Calloc(1, sizeof(*cursor));
    if (!cursor) {
        SDL_OutOfMemory();
        return NULL;
    }
    cursor->system = id;
    cursor->next = sdlop_cursors;
    sdlop_cursors = cursor;
    if (id == SDL_SYSTEM_CURSOR_DEFAULT) {
        sdlop_default_cursor = cursor;
    }
    return cursor;
}

bool SDL_SetCursor(SDL_Cursor *cursor)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();
    SDL_Window *focus = SDLOP_GetMouseFocusWindow();

    if (!cursor) {
        cursor = sdlop_default_cursor;
    }
    if (!cursor) {
        return SDL_SetError("No cursor to set");
    }
    if (driver && driver->set_cursor && focus) {
        if (!driver->set_cursor(focus, cursor)) {
            return false;
        }
    }
    sdlop_current_cursor = cursor;
    return true;
}

SDL_Cursor *SDL_GetCursor(void)
{
    return sdlop_current_cursor;
}

SDL_Cursor *SDL_GetDefaultCursor(void)
{
    return sdlop_default_cursor;
}

void SDL_DestroyCursor(SDL_Cursor *cursor)
{
    SDL_Cursor **link;

    if (!cursor) {
        return;
    }
    if (cursor == sdlop_current_cursor) {
        SDL_SetCursor(sdlop_default_cursor);
        if (cursor == sdlop_current_cursor) {
            sdlop_current_cursor = NULL;
        }
    }
    link = &sdlop_cursors;
    while (*link) {
        if (*link == cursor) {
            *link = cursor->next;
            break;
        }
        link = &(*link)->next;
    }
    if (sdlop_default_cursor == cursor) {
        sdlop_default_cursor = NULL;
    }
    SDLOP_Free(cursor->data);
    SDLOP_Free(cursor);
}

bool SDL_ShowCursor(void)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();
    SDL_Window *focus = SDLOP_GetMouseFocusWindow();

    sdlop_cursor_visible = true;
    if (driver && driver->show_cursor && focus) {
        return driver->show_cursor(focus, true);
    }
    return true;
}

bool SDL_HideCursor(void)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();
    SDL_Window *focus = SDLOP_GetMouseFocusWindow();

    sdlop_cursor_visible = false;
    if (driver && driver->show_cursor && focus) {
        return driver->show_cursor(focus, false);
    }
    return true;
}

bool SDL_CursorVisible(void)
{
    return sdlop_cursor_visible;
}

SDL_Cursor *SDLOP_GetActiveCursor(void)
{
    return sdlop_current_cursor;
}
