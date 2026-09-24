/*
  SDLop -- SDL_video.h: the windowing core.

  This file owns everything that is not backend specific: the driver registry,
  the display registry, the window registry, the public SDL_video.h API, the
  software window surface and the dispatch into the GL driver. Backends
  (src/video/SDL_wayland.c, src/video/SDL_x11.c) fill in a SDLOP_VideoDriver
  vtable and call the SDLOP_OnWindow* / SDLOP_SetDisplay* hooks when the
  compositor tells them something changed.

  Two deliberate differences from SDL3:

    * windows and displays are stored in plain intrusive linked lists. SDL3 uses
      hash tables keyed by ID; with the handful of windows a game opens, walking
      a dozen pointers is faster than hashing and needs no allocation.
    * state changes are pushed as events immediately (at pump time), so
      SDL_GetWindowSize() is always consistent with what the app has been told.
*/

#include "../sdlop_internal.h"

#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* Driver registry                                                           */
/* ------------------------------------------------------------------------- */

/* Drivers are compiled in conditionally: a build without X11 development files
   simply has one entry fewer, instead of a link error. */
static const SDLOP_VideoDriver *sdlop_drivers[] = {
#if defined(SDLOP_HAVE_WAYLAND)
    &SDLOP_WaylandVideoDriver,
#endif
#if defined(SDLOP_HAVE_X11)
    &SDLOP_X11VideoDriver,
#endif
    &SDLOP_OffscreenVideoDriver,
};
#define SDLOP_NUM_DRIVERS ((int)(sizeof(sdlop_drivers) / sizeof(sdlop_drivers[0])))

static const SDLOP_VideoDriver *sdlop_current_driver;
static bool sdlop_video_initialized;

/* ------------------------------------------------------------------------- */
/* Registries                                                                */
/* ------------------------------------------------------------------------- */

static SDL_Window *sdlop_windows;
static SDL_WindowID sdlop_next_window_id = 1;
static SDL_Window *sdlop_keyboard_focus;
static SDL_Window *sdlop_mouse_focus;
static SDL_Window *sdlop_grabbed_window;
static bool sdlop_relative_mouse_mode;
static bool sdlop_mouse_capture;

static struct SDLOP_Display *sdlop_displays;
static SDL_DisplayID sdlop_primary_display;
static int sdlop_display_generation;
/* SDL_HINT_VIDEO_WAYLAND_SCALE_TO_DISPLAY: render in physical pixels instead of
   letting the compositor upscale a 1x surface. */
static bool sdlop_scale_to_display;

const SDLOP_VideoDriver *SDLOP_GetVideoDriver(void)
{
    return sdlop_current_driver;
}

bool SDLOP_ScaleToDisplayEnabled(void)
{
    return sdlop_scale_to_display;
}

void SDLOP_SetScaleToDisplay(bool enabled)
{
    sdlop_scale_to_display = enabled;
}

bool SDLOP_VideoIsReady(void)
{
    return sdlop_video_initialized && sdlop_current_driver != NULL;
}

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static bool sdlop_push_window_event(SDL_Window *window, Uint32 type, Sint32 data1, Sint32 data2)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.window.timestamp = SDL_GetTicksNS();
    event.window.windowID = window ? window->id : 0;
    event.window.data1 = data1;
    event.window.data2 = data2;
    return SDLOP_PushEvent(&event);
}

/* ------------------------------------------------------------------------- */
/* Window registry                                                           */
/* ------------------------------------------------------------------------- */

bool SDLOP_AddWindow(SDL_Window *window)
{
    window->id = sdlop_next_window_id++;
    if (sdlop_next_window_id == 0) {
        sdlop_next_window_id = 1;
    }
    window->next = sdlop_windows;
    window->prev = NULL;
    if (sdlop_windows) {
        sdlop_windows->prev = window;
    }
    sdlop_windows = window;
    return true;
}

void SDLOP_RemoveWindow(SDL_Window *window)
{
    if (window->prev) {
        window->prev->next = window->next;
    } else if (sdlop_windows == window) {
        sdlop_windows = window->next;
    }
    if (window->next) {
        window->next->prev = window->prev;
    }
    if (sdlop_keyboard_focus == window) {
        sdlop_keyboard_focus = NULL;
    }
    if (sdlop_mouse_focus == window) {
        sdlop_mouse_focus = NULL;
    }
    if (sdlop_grabbed_window == window) {
        sdlop_grabbed_window = NULL;
    }
}

SDL_Window *SDLOP_GetWindowFromIDInternal(SDL_WindowID id)
{
    SDL_Window *window;
    if (id == 0) {
        return NULL;
    }
    for (window = sdlop_windows; window; window = window->next) {
        if (window->id == id) {
            return window;
        }
    }
    return NULL;
}

SDL_Window *SDL_GetWindowFromID(SDL_WindowID id)
{
    SDL_Window *window = SDLOP_GetWindowFromIDInternal(id);
    if (!window) {
        SDL_SetError("Invalid window ID");
    }
    return window;
}

SDL_Window *SDLOP_GetKeyboardFocusWindow(void)
{
    return sdlop_keyboard_focus;
}

SDL_Window *SDLOP_GetMouseFocusWindow(void)
{
    return sdlop_mouse_focus;
}

void SDLOP_SetMouseFocusWindow(SDL_Window *window)
{
    sdlop_mouse_focus = window;
}

SDL_Window *SDL_GetMouseFocus(void)
{
    return sdlop_mouse_focus;
}

SDL_Window *SDL_GetKeyboardFocus(void)
{
    return sdlop_keyboard_focus;
}

SDL_Window **SDL_GetWindows(int *count)
{
    SDL_Window **windows;
    SDL_Window *window;
    int num = 0, i = 0;

    for (window = sdlop_windows; window; window = window->next) {
        num++;
    }
    windows = (SDL_Window **)SDLOP_Alloc(sizeof(SDL_Window *) * (size_t)(num ? num : 1));
    if (!windows) {
        SDL_OutOfMemory();
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    for (window = sdlop_windows; window; window = window->next) {
        windows[i++] = window;
    }
    if (count) {
        *count = num;
    }
    return windows;
}

/* ------------------------------------------------------------------------- */
/* Display registry                                                          */
/* ------------------------------------------------------------------------- */

/* Displays exist from the moment the backend enumerates them, but SDL3 only
   reports *changes*: an application that starts up with two monitors must not
   see two SDL_EVENT_DISPLAY_ADDED events it never asked about. */
static bool sdlop_displays_enumerated;

void SDLOP_MarkDisplayEnumerationDone(void)
{
    sdlop_displays_enumerated = true;
}

static void sdlop_push_display_event(SDL_EventType type, SDL_DisplayID id, Sint32 data1)
{
    SDL_Event event;

    if (!sdlop_displays_enumerated) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.common.timestamp = SDL_GetTicksNS();
    event.display.displayID = id;
    event.display.data1 = data1;
    SDLOP_PushEvent(&event);
}

struct SDLOP_Display *SDLOP_AddDisplay(SDL_DisplayID id)
{
    struct SDLOP_Display *display = (struct SDLOP_Display *)SDLOP_Calloc(1, sizeof(*display));
    struct SDLOP_Display **link;

    if (!display) {
        SDL_OutOfMemory();
        return NULL;
    }
    display->id = id;
    display->scale = 1.0f;       /* native scale factor, the backend overwrites it */
    display->content_scale = 1.0f;
    /* Desktops are landscape unless a backend says otherwise: SDL reports
       SDL_ORIENTATION_UNKNOWN only for a display it knows nothing about. */
    display->orientation = SDL_ORIENTATION_LANDSCAPE;
    display->usable = display->bounds;
    /* keep the list sorted by ID so lookups stay cheap */
    link = &sdlop_displays;
    while (*link && (*link)->id < id) {
        link = &(*link)->next;
    }
    display->next = *link;
    *link = display;
    if (!sdlop_primary_display) {
        sdlop_primary_display = id;
    }
    sdlop_display_generation++;
    sdlop_push_display_event(SDL_EVENT_DISPLAY_ADDED, id, 0);
    return display;
}

bool SDLOP_RemoveDisplay(SDL_DisplayID id)
{
    struct SDLOP_Display **link = &sdlop_displays;
    SDL_Window *window;

    while (*link && (*link)->id != id) {
        link = &(*link)->next;
    }
    if (!*link) {
        return false;
    }

    {
        struct SDLOP_Display *display = *link;
        *link = display->next;
        SDLOP_Free(display->name);
        SDLOP_Free(display->modes);
        SDLOP_Free(display->modedata);
        SDLOP_Free(display);
    }
    sdlop_display_generation++;

    /* Any window that was showing on it has to land somewhere sensible: the new
       primary display, or the first one left. */
    if (sdlop_primary_display == id) {
        sdlop_primary_display = sdlop_displays ? sdlop_displays->id : 0;
    }
    for (window = sdlop_windows; window; window = window->next) {
        if (window->display == id) {
            SDL_DisplayID fallback = sdlop_primary_display;
            if (!fallback) {
                fallback = SDL_GetDisplayForRect(&(SDL_Rect){ window->x, window->y, window->w,
                                                             window->h });
            }
            if (fallback) {
                SDLOP_UpdateWindowDisplay(window, fallback);
                SDLOP_UpdateWindowScaleFactor(window);
            }
        }
    }

    sdlop_push_display_event(SDL_EVENT_DISPLAY_REMOVED, id, 0);
    return true;
}

struct SDLOP_Display *SDLOP_GetDisplay(SDL_DisplayID id)
{
    struct SDLOP_Display *display;
    for (display = sdlop_displays; display; display = display->next) {
        if (display->id == id) {
            return display;
        }
    }
    return NULL;
}

struct SDLOP_Display *SDLOP_GetPrimaryDisplayInternal(void)
{
    return SDLOP_GetDisplay(sdlop_primary_display);
}

void SDLOP_ResetDisplays(void)
{
    struct SDLOP_Display *display = sdlop_displays;
    while (display) {
        struct SDLOP_Display *next = display->next;
        SDLOP_Free(display->name);
        SDLOP_Free(display->modes);
        SDLOP_Free(display->modedata);
        SDLOP_Free(display);
        display = next;
    }
    sdlop_displays = NULL;
    sdlop_primary_display = 0;
    sdlop_displays_enumerated = false;
}

void SDLOP_SetDisplayBounds(struct SDLOP_Display *display, const SDL_Rect *bounds)
{
    if (display && bounds) {
        display->bounds = *bounds;
        if (display->usable.w == 0 && display->usable.h == 0) {
            display->usable = *bounds;
        }
    }
}

void SDLOP_SetDisplayName(struct SDLOP_Display *display, const char *name)
{
    if (display && name && (!display->name || strcmp(display->name, name) != 0)) {
        SDLOP_Free(display->name);
        display->name = SDL_strdup(name);
    }
}

/* A window renders at the display's scale factor only when the application (or
   the backend's scale-to-display mode) asked for it: SDL3 keeps everything in
   logical pixels by default and lets the compositor upscale the surface. */
static float sdlop_window_render_scale(const SDL_Window *window)
{
    if (window->flags & SDL_WINDOW_HIGH_PIXEL_DENSITY || sdlop_scale_to_display) {
        return window->scale_factor > 0.0f ? window->scale_factor : 1.0f;
    }
    return 1.0f;
}

void SDLOP_SetDisplayContentScale(struct SDLOP_Display *display, float scale)
{
    SDL_Window *window;

    if (!display || scale <= 0.0f || display->content_scale == scale) {
        return;
    }
    display->content_scale = scale;
    sdlop_push_display_event(SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED, display->id, 0);
    for (window = sdlop_windows; window; window = window->next) {
        if (window->display == display->id) {
            SDLOP_UpdateWindowScaleFactor(window);
        }
    }
}

/* The backend reports the scale factor of the output; windows showing there pick
   it up as their native scale factor (and then decide how much of it to use). */
void SDLOP_SetDisplayScale(struct SDLOP_Display *display, float scale)
{
    SDL_Window *window;

    if (!display || scale <= 0.0f || display->scale == scale) {
        return;
    }
    display->scale = scale;
    if (sdlop_scale_to_display) {
        SDLOP_SetDisplayContentScale(display, scale);
    }
    for (window = sdlop_windows; window; window = window->next) {
        if (window->display == display->id) {
            SDLOP_UpdateWindowScaleFactor(window);
        }
    }
}

void SDLOP_UpdateWindowScaleFactor(SDL_Window *window)
{
    struct SDLOP_Display *display;
    float scale;

    if (!window) {
        return;
    }
    display = SDLOP_GetDisplay(window->display);
    window->scale_factor = (display && display->scale > 0.0f) ? display->scale : 1.0f;
    scale = sdlop_window_render_scale(window);
    if (scale == window->display_scale) {
        return;   /* the window does not render any differently than before */
    }
    SDLOP_OnWindowDisplayScaleChanged(window, scale);
    SDLOP_OnWindowPixelSizeChanged(window, (int)(window->w * scale + 0.5f),
                                   (int)(window->h * scale + 0.5f));
    SDLOP_ResizeWindowSurface(window, window->pixel_w, window->pixel_h);
}

void SDLOP_AddDisplayMode(struct SDLOP_Display *display, const SDL_DisplayMode *mode)
{
    SDL_DisplayMode *modes;
    if (!display || !mode) {
        return;
    }
    modes = (SDL_DisplayMode *)SDLOP_Realloc(display->modes, sizeof(*modes) * (size_t)(display->num_modes + 1));
    if (!modes) {
        SDL_OutOfMemory();
        return;
    }
    display->modes = modes;
    display->modes[display->num_modes++] = *mode;
}

/* Backends rebuild the mode list whenever the compositor re-announces an
   output (resolution change, scale change, monitor swapped). */
void SDLOP_ClearDisplayModes(struct SDLOP_Display *display)
{
    if (!display) {
        return;
    }
    display->num_modes = 0;
    display->modedata = NULL;
}

void SDLOP_SetDisplayCurrentMode(struct SDLOP_Display *display, const SDL_DisplayMode *mode)
{
    if (display && mode) {
        display->current = *mode;
        display->current.internal = NULL;
    }
}

struct SDLOP_Display *SDLOP_GetDisplayForPoint(const SDL_Point *point)
{
    struct SDLOP_Display *display;
    if (!point) {
        return SDLOP_GetPrimaryDisplayInternal();
    }
    for (display = sdlop_displays; display; display = display->next) {
        if (SDL_PointInRect(point, &display->bounds)) {
            return display;
        }
    }
    return SDLOP_GetPrimaryDisplayInternal();
}

/* ------------------------------------------------------------------------- */
/* Video init / quit                                                         */
/* ------------------------------------------------------------------------- */

void SDLOP_RegisterVideoDriver(const SDLOP_VideoDriver *driver)
{
    (void)driver;   /* the SDLop build has a fixed driver list */
}

bool SDLOP_VideoInit(const char *driver_name)
{
    const char *requested = driver_name;
    const char *env;
    int i;

    if (sdlop_video_initialized) {
        return true;
    }
    if (!requested) {
        requested = SDL_GetHint("SDL_VIDEODRIVER");
    }
    if (!requested || !requested[0]) {
        /* auto-detect: Wayland first, then X11 */
        env = SDL_getenv("WAYLAND_DISPLAY");
        if (env && env[0]) {
            requested = "wayland";
        } else {
            env = SDL_getenv("DISPLAY");
            if (env && env[0]) {
                requested = "x11";
            }
        }
    }

    for (i = 0; i < SDLOP_NUM_DRIVERS; i++) {
        if (requested && requested[0] && strcmp(requested, sdlop_drivers[i]->name) != 0) {
            continue;
        }
        if (sdlop_drivers[i]->init()) {
            sdlop_current_driver = sdlop_drivers[i];
            sdlop_video_initialized = true;
            /* Everything the backend found on the way up is the initial state;
               anything after this is hotplug and is reported as such. */
            SDLOP_MarkDisplayEnumerationDone();
            return true;
        }
        if (requested && requested[0]) {
            break;                     /* an explicit request is not retried */
        }
    }

    if (!SDL_GetError()[0]) {
        SDL_SetError("No available video device; set SDL_VIDEODRIVER (tried %d driver%s)",
                     SDLOP_NUM_DRIVERS, SDLOP_NUM_DRIVERS == 1 ? "" : "s");
    }
    return false;
}

void SDLOP_VideoQuit(void)
{
    if (!sdlop_video_initialized) {
        return;
    }
    while (sdlop_windows) {
        SDL_DestroyWindow(sdlop_windows);
    }
    if (sdlop_current_driver && sdlop_current_driver->quit) {
        sdlop_current_driver->quit();
    }
    SDLOP_ResetDisplays();
    sdlop_current_driver = NULL;
    sdlop_video_initialized = false;
}

void SDLOP_VideoPumpEvents(void)
{
    if (sdlop_current_driver && sdlop_current_driver->pump_events) {
        sdlop_current_driver->pump_events();
    }
}

int SDL_GetNumVideoDrivers(void)
{
    return SDLOP_NUM_DRIVERS;
}

const char *SDL_GetVideoDriver(int index)
{
    if (index < 0 || index >= SDLOP_NUM_DRIVERS) {
        SDL_InvalidParamError("index");
        return NULL;
    }
    return sdlop_drivers[index]->name;
}

const char *SDL_GetCurrentVideoDriver(void)
{
    return sdlop_current_driver ? sdlop_current_driver->name : NULL;
}

SDL_SystemTheme SDL_GetSystemTheme(void)
{
    /* SDLop does not talk to the settings portal; apps get UNKNOWN and
       should use their own default. */
    return SDL_SYSTEM_THEME_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* Displays                                                                  */
/* ------------------------------------------------------------------------- */

SDL_DisplayID *SDL_GetDisplays(int *count)
{
    SDL_DisplayID *ids;
    struct SDLOP_Display *display;
    int num = 0, i = 0;

    for (display = sdlop_displays; display; display = display->next) {
        num++;
    }
    if (count) {
        *count = num;
    }
    if (num == 0) {
        return NULL;
    }
    ids = (SDL_DisplayID *)SDLOP_Alloc(sizeof(SDL_DisplayID) * (size_t)num);
    if (!ids) {
        SDL_OutOfMemory();
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    for (display = sdlop_displays; display; display = display->next) {
        ids[i++] = display->id;
    }
    return ids;
}

SDL_DisplayID SDL_GetPrimaryDisplay(void)
{
    struct SDLOP_Display *display = SDLOP_GetPrimaryDisplayInternal();
    if (!display) {
        SDL_SetError("No displays are available");
        return 0;
    }
    return display->id;
}

SDL_PropertiesID SDL_GetDisplayProperties(SDL_DisplayID displayID)
{
    struct SDLOP_Display *display = SDLOP_GetDisplay(displayID);
    if (!display) {
        SDL_SetError("Invalid display ID");
        return 0;
    }
    if (!display->props) {
        display->props = SDL_CreateProperties();
    }
    return display->props;
}

const char *SDL_GetDisplayName(SDL_DisplayID displayID)
{
    struct SDLOP_Display *display = SDLOP_GetDisplay(displayID);
    if (!display) {
        SDL_SetError("Invalid display ID");
        return NULL;
    }
    return display->name ? display->name : "Display";
}

bool SDL_GetDisplayBounds(SDL_DisplayID displayID, SDL_Rect *rect)
{
    struct SDLOP_Display *display = SDLOP_GetDisplay(displayID);
    if (!display) {
        return SDL_SetError("Invalid display ID");
    }
    if (!rect) {
        return SDL_InvalidParamError("rect");
    }
    *rect = display->bounds;
    return true;
}

bool SDL_GetDisplayUsableBounds(SDL_DisplayID displayID, SDL_Rect *rect)
{
    struct SDLOP_Display *display = SDLOP_GetDisplay(displayID);
    if (!display) {
        return SDL_SetError("Invalid display ID");
    }
    if (!rect) {
        return SDL_InvalidParamError("rect");
    }
    *rect = display->usable;
    return true;
}

SDL_DisplayOrientation SDL_GetNaturalDisplayOrientation(SDL_DisplayID displayID)
{
    struct SDLOP_Display *display = SDLOP_GetDisplay(displayID);
    return display ? display->orientation : SDL_ORIENTATION_UNKNOWN;
}

SDL_DisplayOrientation SDL_GetCurrentDisplayOrientation(SDL_DisplayID displayID)
{
    return SDL_GetNaturalDisplayOrientation(displayID);
}

float SDL_GetDisplayContentScale(SDL_DisplayID displayID)
{
    struct SDLOP_Display *display = SDLOP_GetDisplay(displayID);
    if (!display) {
        SDL_SetError("Invalid display ID");
        return 0.0f;
    }
    return display->content_scale;
}

SDL_DisplayMode **SDL_GetFullscreenDisplayModes(SDL_DisplayID displayID, int *count)
{
    struct SDLOP_Display *display = SDLOP_GetDisplay(displayID);
    SDL_DisplayMode **modes;
    int i;

    if (!display) {
        SDL_SetError("Invalid display ID");
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    if (count) {
        *count = display->num_modes;
    }
    if (display->num_modes == 0) {
        return NULL;
    }
    modes = (SDL_DisplayMode **)SDLOP_Alloc(sizeof(*modes) * (size_t)display->num_modes);
    if (!modes) {
        SDL_OutOfMemory();
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    for (i = 0; i < display->num_modes; i++) {
        modes[i] = &display->modes[i];
    }
    return modes;
}

bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID displayID, int w, int h, float refresh_rate,
                                         bool include_high_density_modes, SDL_DisplayMode *closest)
{
    struct SDLOP_Display *display = SDLOP_GetDisplay(displayID);
    const SDL_DisplayMode *best = NULL;
    Sint64 best_score = 0;
    int i;

    (void)include_high_density_modes;
    if (!display) {
        return SDL_SetError("Invalid display ID");
    }
    if (!closest) {
        return SDL_InvalidParamError("closest");
    }
    for (i = 0; i < display->num_modes; i++) {
        const SDL_DisplayMode *mode = &display->modes[i];
        Sint64 score = (Sint64)(abs(mode->w - w) + abs(mode->h - h)) * 1000;
        if (refresh_rate > 0.0f) {
            score += (Sint64)(SDL_fabsf(mode->refresh_rate - refresh_rate) * 100.0f);
        }
        if (!best || score < best_score) {
            best = mode;
            best_score = score;
        }
    }
    if (!best) {
        *closest = display->current;
        return true;
    }
    *closest = *best;
    return true;
}

const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID displayID)
{
    struct SDLOP_Display *display = SDLOP_GetDisplay(displayID);
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!display) {
        SDL_SetError("Invalid display ID");
        return NULL;
    }
    /* A display whose geometry has not been described yet (a monitor that was
       just plugged in, an output the compositor has not filled in) has no mode:
       ask the backend, like SDL3 does, instead of reporting 0x0. */
    if (display->current.w <= 0 || display->current.h <= 0) {
        if (driver && driver->get_display_mode) {
            SDL_DisplayMode mode;
            if (driver->get_display_mode(displayID, &mode)) {
                SDLOP_SetDisplayCurrentMode(display, &mode);
            }
        }
    }
    return &display->current;
}

const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID displayID)
{
    return SDL_GetDesktopDisplayMode(displayID);
}

SDL_DisplayID SDL_GetDisplayForPoint(const SDL_Point *point)
{
    struct SDLOP_Display *display = SDLOP_GetDisplayForPoint(point);
    return display ? display->id : 0;
}

SDL_DisplayID SDL_GetDisplayForRect(const SDL_Rect *rect)
{
    SDL_Point point;
    if (!rect) {
        return SDL_GetPrimaryDisplay();
    }
    point.x = rect->x + rect->w / 2;
    point.y = rect->y + rect->h / 2;
    return SDL_GetDisplayForPoint(&point);
}

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return 0;
    }
    if (!SDLOP_GetDisplay(window->display)) {
        window->display = SDL_GetPrimaryDisplay();
    }
    return window->display;
}

/* ------------------------------------------------------------------------- */
/* Windows                                                                   */
/* ------------------------------------------------------------------------- */

static void sdlop_set_window_flag(SDL_Window *window, SDL_WindowFlags flag, bool enabled)
{
    if (enabled) {
        window->flags |= flag;
    } else {
        window->flags &= ~flag;
    }
}

/* An undefined or centered position means "put it in the middle of the display",
   and SDL3 resolves that at creation: the low 16 bits of the value name the
   display, and the usable bounds of that display give the centre. The resolved
   value is what SDL_GetWindowPosition() reports from then on, which matters on
   Wayland, where the compositor decides the real position and never tells the
   client (a raw SDL_WINDOWPOS_UNDEFINED would otherwise leak to the app). */
static void sdlop_resolve_window_position(SDL_Window *window)
{
    SDL_DisplayID displayID = 0;
    SDL_Rect bounds;

    if (!SDL_WINDOWPOS_ISUNDEFINED(window->x) && !SDL_WINDOWPOS_ISCENTERED(window->x) &&
        !SDL_WINDOWPOS_ISUNDEFINED(window->y) && !SDL_WINDOWPOS_ISCENTERED(window->y)) {
        return;
    }
    if ((SDL_WINDOWPOS_ISUNDEFINED(window->x) || SDL_WINDOWPOS_ISCENTERED(window->x)) &&
        (window->x & 0xFFFF)) {
        displayID = (SDL_DisplayID)(window->x & 0xFFFF);
    } else if ((SDL_WINDOWPOS_ISUNDEFINED(window->y) || SDL_WINDOWPOS_ISCENTERED(window->y)) &&
               (window->y & 0xFFFF)) {
        displayID = (SDL_DisplayID)(window->y & 0xFFFF);
    }
    if (!displayID || !SDLOP_GetDisplay(displayID)) {
        displayID = SDL_GetPrimaryDisplay();
    }
    memset(&bounds, 0, sizeof(bounds));
    SDL_GetDisplayUsableBounds(displayID, &bounds);
    if (window->w > bounds.w || window->h > bounds.h) {
        /* Larger than the space the desktop leaves: centre on the whole display. */
        SDL_GetDisplayBounds(displayID, &bounds);
    }
    if (SDL_WINDOWPOS_ISUNDEFINED(window->x) || SDL_WINDOWPOS_ISCENTERED(window->x)) {
        window->x = bounds.x + (bounds.w - window->w) / 2;
    }
    if (SDL_WINDOWPOS_ISUNDEFINED(window->y) || SDL_WINDOWPOS_ISCENTERED(window->y)) {
        window->y = bounds.y + (bounds.h - window->h) / 2;
    }
}

SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID props)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;
    SDL_Window *window;
    const char *title;
    int w, h;
    SDL_WindowFlags flags = 0;

    if (!driver) {
        SDL_SetError("Video subsystem is not initialized");
        return NULL;
    }
    if (!props) {
        SDL_InvalidParamError("props");
        return NULL;
    }

    w = (int)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 640);
    h = (int)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 480);
    title = SDL_GetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "");
    flags = (SDL_WindowFlags)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, 0);

    if (w <= 0 || h <= 0) {
        SDL_SetError("Invalid window size %dx%d", w, h);
        return NULL;
    }

    window = (SDL_Window *)SDLOP_Calloc(1, sizeof(*window));
    if (!window) {
        SDL_OutOfMemory();
        return NULL;
    }
    window->x = (int)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_UNDEFINED);
    window->y = (int)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_UNDEFINED);
    window->w = w;
    window->h = h;
    sdlop_resolve_window_position(window);
    window->pixel_w = w;
    window->pixel_h = h;
    window->flags = flags;
    window->display_scale = 1.0f;
    window->opacity = 1.0f;
    window->title = SDL_strdup(title ? title : "");
    window->props = SDL_CreateProperties();
    if (!window->title || !window->props) {
        SDL_OutOfMemory();
        SDLOP_Free(window->title);
        SDLOP_Free(window);
        return NULL;
    }

    /* creation properties that SDL3 exposes both as flags and as properties */
    sdlop_set_window_flag(window, SDL_WINDOW_BORDERLESS,
                          SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN,
                                                 (flags & SDL_WINDOW_BORDERLESS) != 0));
    sdlop_set_window_flag(window, SDL_WINDOW_RESIZABLE,
                          SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN,
                                                 (flags & SDL_WINDOW_RESIZABLE) != 0));
    sdlop_set_window_flag(window, SDL_WINDOW_ALWAYS_ON_TOP,
                          SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_ALWAYS_ON_TOP_BOOLEAN,
                                                 (flags & SDL_WINDOW_ALWAYS_ON_TOP) != 0));
    sdlop_set_window_flag(window, SDL_WINDOW_FULLSCREEN,
                          SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                                                 (flags & SDL_WINDOW_FULLSCREEN) != 0));
    sdlop_set_window_flag(window, SDL_WINDOW_HIDDEN,
                          SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN,
                                                 (flags & SDL_WINDOW_HIDDEN) != 0));
    sdlop_set_window_flag(window, SDL_WINDOW_MODAL,
                          SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_MODAL_BOOLEAN,
                                                 (flags & SDL_WINDOW_MODAL) != 0));
    sdlop_set_window_flag(window, SDL_WINDOW_NOT_FOCUSABLE, false);
    if (!SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, true)) {
        window->flags |= SDL_WINDOW_NOT_FOCUSABLE;
    }

    window->parent = (SDL_Window *)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_CREATE_PARENT_POINTER, NULL);

    SDLOP_AddWindow(window);
    if (!driver->create_window(window)) {
        SDLOP_RemoveWindow(window);
        SDL_DestroyProperties(window->props);
        SDLOP_Free(window->title);
        SDLOP_Free(window);
        return NULL;
    }

    window->display = SDL_GetDisplayForRect(&(SDL_Rect){ window->x, window->y, window->w, window->h });
    SDLOP_UpdateWindowScaleFactor(window);

    /* SDL3 tells the app the window exists before it is shown. */
    sdlop_push_window_event(window, SDL_EVENT_WINDOW_SHOWN, 0, 0);
    return window;
}

SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags)
{
    SDL_Window *window;
    SDL_PropertiesID props = SDL_CreateProperties();

    if (!props) {
        return NULL;
    }
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, (Sint64)flags);
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return window;
}

SDL_Window *SDL_CreatePopupWindow(SDL_Window *parent, int offset_x, int offset_y, int w, int h,
                                  SDL_WindowFlags flags)
{
    SDL_PropertiesID props;
    SDL_Window *window;

    if (!parent) {
        SDL_InvalidParamError("parent");
        return NULL;
    }
    props = SDL_CreateProperties();
    if (!props) {
        return NULL;
    }
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_PARENT_POINTER, parent);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, offset_x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, offset_y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                          (Sint64)(flags | SDL_WINDOW_POPUP_MENU));
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return window;
}

void SDL_DestroyWindow(SDL_Window *window)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return;
    }
    if (window->surface) {
        SDLOP_DestroyWindowSurface(window);
    }
    if (driver && driver->destroy_window) {
        driver->destroy_window(window);
    }
    SDLOP_RemoveWindow(window);
    sdlop_push_window_event(window, SDL_EVENT_WINDOW_DESTROYED, 0, 0);
    SDL_DestroyProperties(window->props);
    SDLOP_Free(window->title);
    SDLOP_Free(window);
}

SDL_WindowID SDL_GetWindowID(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return 0;
    }
    return window->id;
}

SDL_Window *SDL_GetWindowParent(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return NULL;
    }
    return window->parent;
}

SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return 0;
    }
    return window->props;
}

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return 0;
    }
    return window->flags;
}

bool SDL_SetWindowTitle(SDL_Window *window, const char *title)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;
    char *copy;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    copy = SDL_strdup(title ? title : "");
    if (!copy) {
        return SDLOP_OutOfMemory();
    }
    SDLOP_Free(window->title);
    window->title = copy;
    if (driver && driver->set_window_title) {
        return driver->set_window_title(window, window->title);
    }
    return true;
}

const char *SDL_GetWindowTitle(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return NULL;
    }
    return window->title;
}

bool SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (!icon) {
        return SDL_InvalidParamError("icon");
    }
    if (driver && driver->set_window_icon) {
        return driver->set_window_icon(window, icon);
    }
    return true;
}

bool SDL_SetWindowPosition(SDL_Window *window, int x, int y)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;
    SDL_Rect bounds = { 0, 0, 0, 0 };

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    /* Same rules as at creation, but the display can also be named in the low
       bits of the value (SDL_WINDOWPOS_CENTERED_DISPLAY(i)), and SDL3 resolves
       both before the backend sees them. An undefined axis keeps the window
       where it is. */
    if (SDL_WINDOWPOS_ISUNDEFINED(x)) {
        x = window->x;
    }
    if (SDL_WINDOWPOS_ISUNDEFINED(y)) {
        y = window->y;
    }
    if (SDL_WINDOWPOS_ISCENTERED(x) || SDL_WINDOWPOS_ISCENTERED(y)) {
        SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        if (SDL_WINDOWPOS_ISCENTERED(x) && (x & 0xFFFF)) {
            display = (SDL_DisplayID)(x & 0xFFFF);
        } else if (SDL_WINDOWPOS_ISCENTERED(y) && (y & 0xFFFF)) {
            display = (SDL_DisplayID)(y & 0xFFFF);
        }
        if (!display || !SDLOP_GetDisplay(display)) {
            display = SDL_GetPrimaryDisplay();
        }
        if (!SDL_GetDisplayUsableBounds(display, &bounds) || window->w > bounds.w ||
            window->h > bounds.h) {
            if (!SDL_GetDisplayBounds(display, &bounds)) {
                return false;
            }
        }
        if (SDL_WINDOWPOS_ISCENTERED(x)) {
            x = bounds.x + (bounds.w - window->w) / 2;
        }
        if (SDL_WINDOWPOS_ISCENTERED(y)) {
            y = bounds.y + (bounds.h - window->h) / 2;
        }
    }
    if (driver && driver->set_window_position) {
        return driver->set_window_position(window, x, y);
    }
    window->x = x;
    window->y = y;
    return true;
}

bool SDL_GetWindowPosition(SDL_Window *window, int *x, int *y)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (driver && driver->get_window_position) {
        if (!driver->get_window_position(window, x, y)) {
            return false;
        }
    }
    if (x) {
        *x = window->x;
    }
    if (y) {
        *y = window->y;
    }
    return true;
}

bool SDL_SetWindowSize(SDL_Window *window, int w, int h)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (w <= 0 || h <= 0) {
        return SDL_SetError("Invalid window size %dx%d", w, h);
    }
    if (driver && driver->set_window_size) {
        return driver->set_window_size(window, w, h);
    }
    /* No backend hook: the size change is immediate and the pixel size follows
       the display scale, so the window surface stays in sync. */
    SDLOP_OnWindowResized(window, w, h);
    if (window->display_scale > 1.0f) {
        SDLOP_OnWindowPixelSizeChanged(window, (int)(w * window->display_scale + 0.5f),
                                       (int)(h * window->display_scale + 0.5f));
    } else {
        SDLOP_OnWindowPixelSizeChanged(window, w, h);
    }
    SDLOP_ResizeWindowSurface(window, window->pixel_w, window->pixel_h);
    return true;
}

bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (w) {
        *w = window->w;
    }
    if (h) {
        *h = window->h;
    }
    return true;
}

bool SDL_GetWindowSafeArea(SDL_Window *window, SDL_Rect *rect)
{
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (!rect) {
        return SDL_InvalidParamError("rect");
    }
    rect->x = 0;
    rect->y = 0;
    rect->w = window->w;
    rect->h = window->h;
    return true;
}

bool SDL_SetWindowAspectRatio(SDL_Window *window, float min_aspect, float max_aspect)
{
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    window->min_aspect = min_aspect;
    window->max_aspect = max_aspect;
    return true;
}

bool SDL_GetWindowAspectRatio(SDL_Window *window, float *min_aspect, float *max_aspect)
{
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (min_aspect) {
        *min_aspect = window->min_aspect;
    }
    if (max_aspect) {
        *max_aspect = window->max_aspect;
    }
    return true;
}

bool SDL_GetWindowBordersSize(SDL_Window *window, int *top, int *left, int *bottom, int *right)
{
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    /* Wayland sheds no light on decorations; X11 windows here are undecorated by
       us and decorated by the window manager, which does not report the frame. */
    if (top) {
        *top = 0;
    }
    if (left) {
        *left = 0;
    }
    if (bottom) {
        *bottom = 0;
    }
    if (right) {
        *right = 0;
    }
    return true;
}

bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h)
{
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (w) {
        *w = window->pixel_w;
    }
    if (h) {
        *h = window->pixel_h;
    }
    return true;
}

bool SDL_SetWindowMinimumSize(SDL_Window *window, int min_w, int min_h)
{
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    window->min_w = min_w;
    window->min_h = min_h;
    return true;
}

bool SDL_GetWindowMinimumSize(SDL_Window *window, int *w, int *h)
{
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (w) {
        *w = window->min_w;
    }
    if (h) {
        *h = window->min_h;
    }
    return true;
}

bool SDL_SetWindowMaximumSize(SDL_Window *window, int max_w, int max_h)
{
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    window->max_w = max_w;
    window->max_h = max_h;
    return true;
}

bool SDL_GetWindowMaximumSize(SDL_Window *window, int *w, int *h)
{
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (w) {
        *w = window->max_w;
    }
    if (h) {
        *h = window->max_h;
    }
    return true;
}

bool SDL_SetWindowBordered(SDL_Window *window, bool bordered)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    return driver && driver->set_window_bordered ? driver->set_window_bordered(window, bordered) : true;
}

bool SDL_SetWindowResizable(SDL_Window *window, bool resizable)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    sdlop_set_window_flag(window, SDL_WINDOW_RESIZABLE, resizable);
    return driver && driver->set_window_resizable ? driver->set_window_resizable(window, resizable) : true;
}

bool SDL_SetWindowAlwaysOnTop(SDL_Window *window, bool on_top)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    sdlop_set_window_flag(window, SDL_WINDOW_ALWAYS_ON_TOP, on_top);
    return driver && driver->set_window_always_on_top ? driver->set_window_always_on_top(window, on_top) : true;
}

bool SDL_ShowWindow(SDL_Window *window)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (!(window->flags & SDL_WINDOW_HIDDEN)) {
        return true;                   /* already shown */
    }
    sdlop_set_window_flag(window, SDL_WINDOW_HIDDEN, false);
    if (driver && driver->show_window && !driver->show_window(window)) {
        return false;
    }
    SDLOP_OnWindowShown(window, true);
    return true;
}

bool SDL_HideWindow(SDL_Window *window)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (window->flags & SDL_WINDOW_HIDDEN) {
        return true;                   /* already hidden */
    }
    sdlop_set_window_flag(window, SDL_WINDOW_HIDDEN, true);
    if (driver && driver->hide_window && !driver->hide_window(window)) {
        return false;
    }
    SDLOP_OnWindowShown(window, false);
    return true;
}

bool SDL_RaiseWindow(SDL_Window *window)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    return driver && driver->raise_window ? driver->raise_window(window) : true;
}

bool SDL_MaximizeWindow(SDL_Window *window)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    return driver && driver->maximize_window ? driver->maximize_window(window) : true;
}

bool SDL_MinimizeWindow(SDL_Window *window)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    return driver && driver->minimize_window ? driver->minimize_window(window) : true;
}

bool SDL_RestoreWindow(SDL_Window *window)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    return driver && driver->restore_window ? driver->restore_window(window) : true;
}

bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (fullscreen == ((window->flags & SDL_WINDOW_FULLSCREEN) != 0)) {
        return true;
    }
    if (!driver || !driver->set_window_fullscreen) {
        return SDL_Unsupported();
    }
    if (!driver->set_window_fullscreen(window, fullscreen)) {
        return false;
    }
    SDLOP_OnWindowFullscreenChanged(window, fullscreen);
    return true;
}

bool SDL_SyncWindow(SDL_Window *window)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    return driver && driver->sync_window ? driver->sync_window(window) : true;
}

bool SDL_SetWindowKeyboardGrab(SDL_Window *window, bool grabbed)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    sdlop_set_window_flag(window, SDL_WINDOW_KEYBOARD_GRABBED, grabbed);
    if (driver && driver->set_window_grab) {
        return driver->set_window_grab(window, grabbed, (window->flags & SDL_WINDOW_MOUSE_GRABBED) != 0);
    }
    return true;
}

bool SDL_SetWindowMouseGrab(SDL_Window *window, bool grabbed)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    sdlop_set_window_flag(window, SDL_WINDOW_MOUSE_GRABBED, grabbed);
    sdlop_grabbed_window = grabbed ? window : NULL;
    if (driver && driver->set_window_grab) {
        return driver->set_window_grab(window, (window->flags & SDL_WINDOW_KEYBOARD_GRABBED) != 0, grabbed);
    }
    return true;
}

bool SDL_GetWindowKeyboardGrab(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return false;
    }
    return (window->flags & SDL_WINDOW_KEYBOARD_GRABBED) != 0;
}

bool SDL_GetWindowMouseGrab(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return false;
    }
    return (window->flags & SDL_WINDOW_MOUSE_GRABBED) != 0;
}

SDL_Window *SDL_GetGrabbedWindow(void)
{
    return sdlop_grabbed_window;
}

bool SDL_SetWindowMouseRect(SDL_Window *window, const SDL_Rect *rect)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (rect) {
        window->mouse_rect = *rect;
        window->mouse_rect_set = true;
    } else {
        window->mouse_rect_set = false;
    }
    if (driver && driver->set_window_mouse_rect) {
        return driver->set_window_mouse_rect(window, rect);
    }
    return true;
}

const SDL_Rect *SDL_GetWindowMouseRect(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return NULL;
    }
    return window->mouse_rect_set ? &window->mouse_rect : NULL;
}

bool SDL_SetWindowOpacity(SDL_Window *window, float opacity)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    window->opacity = SDL_clamp(opacity, 0.0f, 1.0f);
    if (driver && driver->set_window_opacity) {
        return driver->set_window_opacity(window, window->opacity);
    }
    return true;
}

float SDL_GetWindowOpacity(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return 0.0f;
    }
    return window->opacity;
}

bool SDL_SetWindowParent(SDL_Window *window, SDL_Window *parent)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (parent == window) {
        return SDL_SetError("A window cannot be its own parent");
    }
    window->parent = parent;
    return driver && driver->set_window_parent ? driver->set_window_parent(window, parent) : true;
}

bool SDL_SetWindowModal(SDL_Window *window, bool modal)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    window->modal = modal;
    return driver && driver->set_window_modal ? driver->set_window_modal(window, modal ? window->parent : NULL) : true;
}

bool SDL_SetWindowFocusable(SDL_Window *window, bool focusable)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    sdlop_set_window_flag(window, SDL_WINDOW_NOT_FOCUSABLE, !focusable);
    return driver && driver->set_window_focusable ? driver->set_window_focusable(window, focusable) : true;
}

bool SDL_SetWindowHitTest(SDL_Window *window, SDL_HitTest callback, void *callback_data)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    window->hit_test = callback;
    window->hit_test_data = callback_data;
    if (driver && driver->set_window_hit_test) {
        return driver->set_window_hit_test(callback ? window : NULL, callback, callback_data);
    }
    return true;
}

bool SDL_FlashWindow(SDL_Window *window, SDL_FlashOperation operation)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (driver && driver->flash_window) {
        return driver->flash_window(window, operation);
    }
    /* nothing to do, but not an error: SDL3 treats this as best-effort */
    return true;
}

float SDL_GetWindowPixelDensity(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return 0.0f;
    }
    if (window->w > 0) {
        return (float)window->pixel_w / (float)window->w;
    }
    return 1.0f;
}

float SDL_GetWindowDisplayScale(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return 0.0f;
    }
    return window->display_scale;
}

bool SDL_SetWindowFullscreenMode(SDL_Window *window, const SDL_DisplayMode *mode)
{
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (mode) {
        window->fullscreen_mode = *mode;
        window->fullscreen_mode_set = true;
    } else {
        window->fullscreen_mode_set = false;
    }
    return true;
}

const SDL_DisplayMode *SDL_GetWindowFullscreenMode(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return NULL;
    }
    return window->fullscreen_mode_set ? &window->fullscreen_mode : NULL;
}

SDL_PixelFormat SDL_GetWindowPixelFormat(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return SDL_PIXELFORMAT_UNKNOWN;
    }
    return SDL_PIXELFORMAT_XRGB8888;
}

/* ------------------------------------------------------------------------- */
/* Software window surfaces                                                  */
/* ------------------------------------------------------------------------- */

bool SDL_WindowHasSurface(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return false;
    }
    return window->surface != NULL;
}

SDL_Surface *SDL_GetWindowSurface(SDL_Window *window)
{
    if (!window) {
        SDL_InvalidParamError("window");
        return NULL;
    }
    if (!window->surface) {
        if (!SDLOP_CreateWindowSurface(window, window->pixel_w, window->pixel_h,
                                       SDL_PIXELFORMAT_XRGB8888)) {
            return NULL;
        }
    }
    return window->surface;
}

bool SDL_UpdateWindowSurface(SDL_Window *window)
{
    return SDL_UpdateWindowSurfaceRects(window, NULL, 0);
}

bool SDL_UpdateWindowSurfaceRects(SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (!window->surface) {
        return SDL_SetError("Window has no surface");
    }
    if (!driver || !driver->present_surface) {
        return SDL_Unsupported();
    }
    return driver->present_surface(window, rects, numrects);
}

bool SDL_DestroyWindowSurface(SDL_Window *window)
{
    const SDLOP_VideoDriver *driver = sdlop_current_driver;

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (driver && driver->destroy_window_surface) {
        driver->destroy_window_surface(window);
    }
    SDLOP_DestroyWindowSurface(window);
    return true;
}

/* ------------------------------------------------------------------------- */
/* GL dispatch                                                               */
/* ------------------------------------------------------------------------- */

/* The GL context that is current, so SDL_GL_GetCurrentWindow()/Context() can
   answer without asking the driver. */
static SDL_Window *sdlop_gl_window;
static SDL_GLContext sdlop_gl_context;

static const SDLOP_GLDriver *sdlop_gl(void)
{
    /* NULL both when the video subsystem is not initialized and when the driver
       that is running simply has no GL support (offscreen, for instance). */
    return sdlop_current_driver ? sdlop_current_driver->gl : NULL;
}

/* Error for the "this driver has no GL" case, which is not the same thing as
   "SDL_Init(SDL_INIT_VIDEO) was never called". */
static bool sdlop_gl_not_supported(void)
{
    if (!sdlop_current_driver) {
        return SDL_SetError("Video subsystem is not initialized");
    }
    return SDL_SetError("OpenGL is not supported by the %s video driver",
                        sdlop_current_driver->name ? sdlop_current_driver->name : "(none)");
}

bool SDL_GL_LoadLibrary(const char *path)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (!gl) {
        return sdlop_gl_not_supported();
    }
    return gl->load_library(path);
}

SDL_FunctionPointer SDL_GL_GetProcAddress(const char *proc)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (!gl || !proc) {
        return NULL;
    }
    return gl->get_proc_address(proc);
}

void SDL_GL_UnloadLibrary(void)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (gl) {
        gl->unload_library();
    }
}

bool SDL_GL_ExtensionSupported(const char *extension)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (!gl || !extension) {
        return false;
    }
    return gl->extension_supported(extension);
}

void SDL_GL_ResetAttributes(void)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (gl) {
        gl->reset_attributes();
    }
}

bool SDL_GL_SetAttribute(SDL_GLAttr attr, int value)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (!gl) {
        return sdlop_gl_not_supported();
    }
    return gl->set_attribute(attr, value);
}

bool SDL_GL_GetAttribute(SDL_GLAttr attr, int *value)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (!gl) {
        return sdlop_gl_not_supported();
    }
    if (!value) {
        return SDL_InvalidParamError("value");
    }
    return gl->get_attribute(attr, value);
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    SDL_GLContext context;

    if (!window) {
        SDL_InvalidParamError("window");
        return NULL;
    }
    if (!gl) {
        sdlop_gl_not_supported();
        return NULL;
    }
    context = gl->create_context(window, NULL);
    if (context) {
        window->flags |= SDL_WINDOW_OPENGL;
    }
    return context;
}

bool SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (!gl) {
        return sdlop_gl_not_supported();
    }
    if (!gl->make_current(window, context)) {
        return false;
    }
    sdlop_gl_window = window;
    sdlop_gl_context = context;
    return true;
}

SDL_Window *SDL_GL_GetCurrentWindow(void)
{
    return sdlop_gl_window;
}

SDL_GLContext SDL_GL_GetCurrentContext(void)
{
    return sdlop_gl_context;
}

bool SDL_GL_SetSwapInterval(int interval)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (!gl) {
        return sdlop_gl_not_supported();
    }
    return gl->set_swap_interval(interval);
}

bool SDL_GL_GetSwapInterval(int *interval)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (!gl) {
        return sdlop_gl_not_supported();
    }
    if (!interval) {
        return SDL_InvalidParamError("interval");
    }
    *interval = gl->get_swap_interval();
    return true;
}

bool SDL_GL_SwapWindow(SDL_Window *window)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (!gl) {
        return sdlop_gl_not_supported();
    }
    return gl->swap_window(window);
}

bool SDL_GL_DestroyContext(SDL_GLContext context)
{
    const SDLOP_GLDriver *gl = sdlop_gl();
    if (!gl) {
        return sdlop_gl_not_supported();
    }
    if (!context) {
        return SDL_InvalidParamError("context");
    }
    if (context == sdlop_gl_context) {
        sdlop_gl_context = NULL;
        sdlop_gl_window = NULL;
    }
    gl->destroy_context(context);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Window state changes coming from the backends                             */
/* ------------------------------------------------------------------------- */

void SDLOP_UpdateWindowDisplay(SDL_Window *window, SDL_DisplayID display)
{
    if (!window || window->display == display) {
        return;
    }
    window->display = display;
    sdlop_push_window_event(window, SDL_EVENT_WINDOW_DISPLAY_CHANGED, (Sint32)display, 0);
}

void SDLOP_WindowIterate(void (*fn)(SDL_Window *window, void *userdata), void *userdata)
{
    SDL_Window *window = sdlop_windows;
    while (window) {
        SDL_Window *next = window->next;
        fn(window, userdata);
        window = next;
    }
}

void SDLOP_OnWindowMovedOnDisplay(SDL_Window *window, SDL_DisplayID display, int x, int y)
{
    if (!window) {
        return;
    }
    if (window->x != x || window->y != y) {
        window->x = x;
        window->y = y;
        sdlop_push_window_event(window, SDL_EVENT_WINDOW_MOVED, x, y);
    }
    SDLOP_UpdateWindowDisplay(window, display);
}

void SDLOP_OnWindowMoved(SDL_Window *window, int x, int y)
{
    if (!window || (window->x == x && window->y == y)) {
        return;
    }
    window->x = x;
    window->y = y;
    sdlop_push_window_event(window, SDL_EVENT_WINDOW_MOVED, x, y);
    SDLOP_UpdateWindowDisplay(window, SDL_GetDisplayForRect(&(SDL_Rect){ x, y, window->w, window->h }));
}

void SDLOP_OnWindowResized(SDL_Window *window, int w, int h)
{
    if (!window || (window->w == w && window->h == h)) {
        return;
    }
    window->w = w;
    window->h = h;
    sdlop_push_window_event(window, SDL_EVENT_WINDOW_RESIZED, w, h);
}

void SDLOP_OnWindowPixelSizeChanged(SDL_Window *window, int w, int h)
{
    if (!window || (window->pixel_w == w && window->pixel_h == h)) {
        return;
    }
    window->pixel_w = w;
    window->pixel_h = h;
    /* SDL3 hands the same window surface back, resized, once the drawable size
       changes - applications keep drawing into the surface they already have. */
    SDLOP_ResizeWindowSurface(window, w, h);
    sdlop_push_window_event(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, w, h);
}

void SDLOP_OnWindowShown(SDL_Window *window, bool shown)
{
    if (!window) {
        return;
    }
    sdlop_set_window_flag(window, SDL_WINDOW_HIDDEN, !shown);
    sdlop_push_window_event(window, shown ? SDL_EVENT_WINDOW_SHOWN : SDL_EVENT_WINDOW_HIDDEN, 0, 0);
}

void SDLOP_OnWindowClosed(SDL_Window *window)
{
    if (!window) {
        return;
    }
    sdlop_push_window_event(window, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
}

void SDLOP_OnWindowFocusGained(SDL_Window *window)
{
    if (!window) {
        return;
    }
    sdlop_set_window_flag(window, SDL_WINDOW_INPUT_FOCUS, true);
    if (sdlop_keyboard_focus != window) {
        sdlop_keyboard_focus = window;
        sdlop_push_window_event(window, SDL_EVENT_WINDOW_FOCUS_GAINED, 0, 0);
        SDLOP_SendWindowFocusEvents(window->id, true, false, SDL_GetTicksNS());
    }
}

void SDLOP_OnWindowFocusLost(SDL_Window *window)
{
    if (!window) {
        return;
    }
    sdlop_set_window_flag(window, SDL_WINDOW_INPUT_FOCUS, false);
    if (sdlop_keyboard_focus == window) {
        sdlop_keyboard_focus = NULL;
        sdlop_push_window_event(window, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
        SDLOP_SendWindowFocusEvents(window->id, false, false, SDL_GetTicksNS());
    }
    SDLOP_ResetKeyboardState();
}

void SDLOP_OnWindowMouseEnter(SDL_Window *window)
{
    if (!window) {
        return;
    }
    sdlop_mouse_focus = window;
    sdlop_set_window_flag(window, SDL_WINDOW_MOUSE_FOCUS, true);
    sdlop_push_window_event(window, SDL_EVENT_WINDOW_MOUSE_ENTER, 0, 0);
}

void SDLOP_OnWindowMouseLeave(SDL_Window *window)
{
    if (!window) {
        return;
    }
    if (sdlop_mouse_focus == window) {
        sdlop_mouse_focus = NULL;
        sdlop_set_window_flag(window, SDL_WINDOW_MOUSE_FOCUS, false);
    }
    sdlop_push_window_event(window, SDL_EVENT_WINDOW_MOUSE_LEAVE, 0, 0);
}

void SDLOP_OnWindowDisplayScaleChanged(SDL_Window *window, float scale)
{
    if (!window || scale <= 0.0f || window->display_scale == scale) {
        return;
    }
    window->display_scale = scale;
    /* SDL3 carries no payload for this event: applications call
       SDL_GetWindowDisplayScale() to read the new value. */
    sdlop_push_window_event(window, SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED, 0, 0);
}

void SDLOP_OnWindowOccluded(SDL_Window *window, bool occluded)
{
    if (!window) {
        return;
    }
    sdlop_set_window_flag(window, SDL_WINDOW_OCCLUDED, occluded);
    sdlop_push_window_event(window, occluded ? SDL_EVENT_WINDOW_OCCLUDED : SDL_EVENT_WINDOW_EXPOSED, 0, 0);
}

void SDLOP_OnWindowMaximized(SDL_Window *window, bool maximized)
{
    if (!window) {
        return;
    }
    sdlop_set_window_flag(window, SDL_WINDOW_MAXIMIZED, maximized);
    sdlop_push_window_event(window, maximized ? SDL_EVENT_WINDOW_MAXIMIZED : SDL_EVENT_WINDOW_RESTORED, 0, 0);
}

void SDLOP_OnWindowMinimized(SDL_Window *window, bool minimized)
{
    if (!window) {
        return;
    }
    sdlop_set_window_flag(window, SDL_WINDOW_MINIMIZED, minimized);
    sdlop_push_window_event(window, minimized ? SDL_EVENT_WINDOW_MINIMIZED : SDL_EVENT_WINDOW_RESTORED, 0, 0);
}

void SDLOP_OnWindowFullscreenChanged(SDL_Window *window, bool fullscreen)
{
    if (!window) {
        return;
    }
    sdlop_set_window_flag(window, SDL_WINDOW_FULLSCREEN, fullscreen);
    sdlop_push_window_event(window, fullscreen ? SDL_EVENT_WINDOW_ENTER_FULLSCREEN : SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 0, 0);
}

void SDLOP_OnWindowExposed(SDL_Window *window)
{
    if (window) {
        sdlop_push_window_event(window, SDL_EVENT_WINDOW_EXPOSED, 0, 0);
    }
}

bool SDLOP_WindowHasFocus(SDL_Window *window)
{
    return sdlop_keyboard_focus == window;
}

bool SDLOP_RelativeMouseModeActive(void)
{
    return sdlop_relative_mouse_mode;
}

void SDLOP_SetRelativeMouseMode(bool enabled)
{
    sdlop_relative_mouse_mode = enabled;
}

bool SDLOP_MouseCaptureActive(void)
{
    return sdlop_mouse_capture;
}

void SDLOP_SetMouseCapture(bool enabled)
{
    sdlop_mouse_capture = enabled;
}

const char *SDLOP_CurrentVideoDriverName(void)
{
    return sdlop_current_driver ? sdlop_current_driver->name : "none";
}
