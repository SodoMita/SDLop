/*
  SDLop - video / window management core.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include <strings.h>
#include <math.h>

static SDLop_Display *display_by_id(SDL_DisplayID displayID);
static void finalize_display_mode(SDL_DisplayMode *mode);

static SDLop_VideoDevice *const video_drivers[] = {
#if SDLop_VIDEO_EMSCRIPTEN
    &SDLop_emscripten_device,
#endif
#if SDLop_VIDEO_WAYLAND
    &SDLop_wayland_device,
#endif
#if SDLop_VIDEO_KMSDRM
    &SDLop_kmsdrm_device,
#endif
    &SDLop_dummy_device,
};

#define SDLOP_NUM_VIDEO_DRIVERS ((int)SDL_arraysize(video_drivers))

static const char *default_driver_order(void)
{
    const char *hint = getenv("SDL_VIDEODRIVER");
    return hint;
}

bool SDLOP_VideoInit(void)
{
    const char *driver = default_driver_order();

    if (driver && driver[0]) {
        for (int i = 0; i < SDLOP_NUM_VIDEO_DRIVERS; i++) {
            if (strcasecmp(video_drivers[i]->name, driver) == 0) {
                if (video_drivers[i]->Init(video_drivers[i])) {
                    sdlop.video = video_drivers[i];
                    return true;
                }
                return false;
            }
        }
        return SDL_SetError("%s not available", driver);
    }

    /* try drivers in order */
    for (int i = 0; i < SDLOP_NUM_VIDEO_DRIVERS; i++) {
        if (video_drivers[i]->Init(video_drivers[i])) {
            sdlop.video = video_drivers[i];
            return true;
        }
    }
    return SDL_SetError("No available video device");
}

void SDLOP_VideoQuit(void)
{
    /* destroy remaining windows first */
    while (sdlop.windows) {
        SDL_DestroyWindow(sdlop.windows);
    }
    if (sdlop.video) {
        sdlop.video->Quit(sdlop.video);
        sdlop.video = NULL;
    }
    /* drivers are responsible for DelDisplay at Quit; make sure nothing leaks */
    for (int i = 0; i < sdlop.num_displays; i++) {
        SDLop_Display *d = &sdlop.displays[i];
        free(d->fullscreen_modes);
        d->fullscreen_modes = NULL;
        d->num_fullscreen_modes = 0;
        if (d->props) {
            SDL_DestroyProperties(d->props);
            d->props = 0;
        }
        free(d->name);
        d->name = NULL;
    }
    memset(sdlop.displays, 0, sizeof(sdlop.displays));
    sdlop.num_displays = 0;
}

void SDLOP_AddWindow(SDL_Window *window)
{
    window->next = sdlop.windows;
    sdlop.windows = window;
    sdlop.num_windows++;
}

void SDLOP_RemoveWindow(SDL_Window *window)
{
    SDL_Window **pp = &sdlop.windows;
    while (*pp) {
        if (*pp == window) {
            *pp = window->next;
            sdlop.num_windows--;
            break;
        }
        pp = &(*pp)->next;
    }
    if (sdlop.keyboard_focus == window) {
        sdlop.keyboard_focus = NULL;
    }
    if (sdlop.mouse_focus == window) {
        sdlop.mouse_focus = NULL;
        sdlop.mouse_has_position = false;
    }
    if (sdlop.relative_mode_window == window) {
        sdlop.relative_mode_window = NULL;
    }
}

SDL_WindowID SDLOP_FocusWindowID(void)
{
    if (sdlop.keyboard_focus) {
        return sdlop.keyboard_focus->id;
    }
    return sdlop.windows ? sdlop.windows->id : 0;
}

SDL_Window *SDLOP_GetFocusedKeyboardWindow(void)
{
    return sdlop.keyboard_focus ? sdlop.keyboard_focus : sdlop.windows;
}

/* ------------------------------------------------------------------ */
/* Display list                                                       */
/* ------------------------------------------------------------------ */

/* SDL3's cmpmodes: widest first, then tallest, then deepest, then fastest. */
static int cmp_display_modes(const void *A, const void *B)
{
    const SDL_DisplayMode *a = (const SDL_DisplayMode *)A;
    const SDL_DisplayMode *b = (const SDL_DisplayMode *)B;
    int a_refresh = a->refresh_rate_numerator > 0 && a->refresh_rate_denominator > 0
                        ? a->refresh_rate_numerator * 100 / a->refresh_rate_denominator
                        : (int)(a->refresh_rate * 100);
    int b_refresh = b->refresh_rate_numerator > 0 && b->refresh_rate_denominator > 0
                        ? b->refresh_rate_numerator * 100 / b->refresh_rate_denominator
                        : (int)(b->refresh_rate * 100);
    int a_bpp = SDLOP_BytesPerPixel(a->format);
    int b_bpp = SDLOP_BytesPerPixel(b->format);
    int a_density = (int)(a->pixel_density * 100);
    int b_density = (int)(b->pixel_density * 100);

    if (a->w != b->w) {
        return b->w - a->w;
    } else if (a->h != b->h) {
        return b->h - a->h;
    } else if (a_bpp != b_bpp) {
        return b_bpp - a_bpp;
    } else if (a->format != b->format) {
        return b->format > a->format ? 1 : -1;
    } else if (a_refresh != b_refresh) {
        return b_refresh - a_refresh;
    } else if (a_density != b_density) {
        return a_density - b_density;
    }
    return 0;
}

static void finalize_display_mode(SDL_DisplayMode *mode)
{
    if (mode->pixel_density <= 0.0f) {
        mode->pixel_density = 1.0f;
    }
    if (mode->refresh_rate_numerator > 0) {
        if (mode->refresh_rate_denominator <= 0) {
            mode->refresh_rate_denominator = 1;
        }
        mode->refresh_rate = (float)mode->refresh_rate_numerator / (float)mode->refresh_rate_denominator;
    }
}

bool SDLOP_AddFullscreenDisplayMode(SDL_DisplayID displayID, const SDL_DisplayMode *mode)
{
    SDLop_Display *d = display_by_id(displayID);
    if (!d || !mode) {
        return false;
    }
    SDL_DisplayMode new_mode = *mode;
    new_mode.displayID = displayID;
    new_mode.internal = NULL;
    finalize_display_mode(&new_mode);

    /* dedupe */
    for (int i = 0; i < d->num_fullscreen_modes; i++) {
        if (cmp_display_modes(&new_mode, &d->fullscreen_modes[i]) == 0) {
            return false;
        }
    }
    SDL_DisplayMode *modes = (SDL_DisplayMode *)realloc(d->fullscreen_modes,
                                                        (size_t)(d->num_fullscreen_modes + 1) * sizeof(SDL_DisplayMode));
    if (!modes) {
        SDL_OutOfMemory();
        return false;
    }
    d->fullscreen_modes = modes;
    d->fullscreen_modes[d->num_fullscreen_modes++] = new_mode;
    qsort(d->fullscreen_modes, (size_t)d->num_fullscreen_modes, sizeof(SDL_DisplayMode), cmp_display_modes);
    return true;
}

void SDLOP_UpdateWindowDisplay(SDL_Window *window)
{
    if (!window || sdlop.num_displays == 0) {
        return;
    }
    SDL_Rect bounds = { window->x, window->y, window->w, window->h };
    SDL_DisplayID new_id = 0;
    if ((window->flags & SDL_WINDOW_FULLSCREEN) && window->has_fullscreen_mode
        && window->fullscreen_mode.displayID) {
        new_id = window->fullscreen_mode.displayID;
    } else {
        new_id = SDL_GetDisplayForRect(&bounds);
    }
    if (new_id && new_id != window->displayID) {
        window->displayID = new_id;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_DISPLAY_CHANGED, 0, 0);
    }
}

int SDLOP_GetDisplayIndexById(SDL_DisplayID displayID)
{
    for (int i = 0; i < sdlop.num_displays; i++) {
        if (sdlop.displays[i].id == displayID) {
            return i;
        }
    }
    return -1;
}

SDL_DisplayID SDLOP_GetDisplayIdAtIndex(int index)
{
    if (index < 0 || index >= sdlop.num_displays) {
        return 0;
    }
    return sdlop.displays[index].id;
}

static SDLop_Display *display_by_id(SDL_DisplayID displayID)
{
    int i = SDLOP_GetDisplayIndexById(displayID);
    return i >= 0 ? &sdlop.displays[i] : NULL;
}

static void SDLOP_ReflowDisplayBounds(void)
{
    int x = 0;
    for (int i = 0; i < sdlop.num_displays; i++) {
        SDLop_Display *d = &sdlop.displays[i];
        d->bounds.x = x;
        d->bounds.y = 0;
        x += d->bounds.w;
    }
}

bool SDLOP_GetDisplayBoundsById(SDL_DisplayID displayID, SDL_Rect *rect)
{
    SDLop_Display *d = display_by_id(displayID);
    if (!d || !rect) {
        return false;
    }
    *rect = d->bounds;
    return true;
}

SDL_DisplayID SDLOP_AddDisplay(const char *name, int w, int h, int refresh_mhz,
                               SDL_DisplayOrientation natural_orientation,
                               void *internal, bool send_event)
{
    if (sdlop.num_displays >= SDLOP_MAX_DISPLAYS) {
        SDL_SetError("SDLOP_MAX_DISPLAYS reached");
        return 0;
    }
    if (w <= 0 || h <= 0) {
        SDL_SetError("Invalid display mode");
        return 0;
    }
    SDLop_Display *d = &sdlop.displays[sdlop.num_displays++];
    memset(d, 0, sizeof(*d));
    d->id = ++sdlop.next_display_id;
    d->name = strdup(name && *name ? name : "display");
    d->content_scale = 1.0f;
    d->natural_orientation = natural_orientation;
    d->current_orientation = natural_orientation;
    d->internal = internal;
    d->bounds.w = w;
    d->bounds.h = h;
    d->desktop_mode.displayID = d->id;
    d->desktop_mode.format = SDL_PIXELFORMAT_ARGB8888;
    d->desktop_mode.w = w;
    d->desktop_mode.h = h;
    d->desktop_mode.pixel_density = 1.0f;
    if (refresh_mhz > 0) {
        d->desktop_mode.refresh_rate = refresh_mhz / 1000.0f;
        d->desktop_mode.refresh_rate_numerator = refresh_mhz;
        d->desktop_mode.refresh_rate_denominator = 1000;
    } else {
        d->desktop_mode.refresh_rate = 0.0f;
        d->desktop_mode.refresh_rate_numerator = 0;
        d->desktop_mode.refresh_rate_denominator = 0;
    }
    d->desktop_mode.internal = NULL;
    d->fullscreen_modes = NULL;
    d->num_fullscreen_modes = 0;
    d->props = 0;
    SDLOP_ReflowDisplayBounds();
    /* the desktop mode is always an available exclusive mode; backends add
     * their full list on top of this */
    SDLOP_AddFullscreenDisplayMode(d->id, &d->desktop_mode);
    if (send_event) {
        SDLOP_SendDisplayEvent(d->id, SDL_EVENT_DISPLAY_ADDED, 0, 0);
    }
    return d->id;
}

void SDLOP_DelDisplay(SDL_DisplayID displayID, bool send_event)
{
    int i = SDLOP_GetDisplayIndexById(displayID);
    if (i < 0) {
        return;
    }
    SDLop_Display *d = &sdlop.displays[i];
    if (send_event) {
        SDLOP_SendDisplayEvent(displayID, SDL_EVENT_DISPLAY_REMOVED, 0, 0);
    }
    free(d->name);
    free(d->fullscreen_modes);
    d->fullscreen_modes = NULL;
    d->num_fullscreen_modes = 0;
    if (d->props) {
        SDL_DestroyProperties(d->props);
        d->props = 0;
    }
    memset(d, 0, sizeof(*d));
    if (i < sdlop.num_displays - 1) {
        memmove(&sdlop.displays[i], &sdlop.displays[i + 1],
                (size_t)(sdlop.num_displays - i - 1) * sizeof(SDLop_Display));
    }
    sdlop.num_displays--;
    memset(&sdlop.displays[sdlop.num_displays], 0, sizeof(SDLop_Display));
    SDLOP_ReflowDisplayBounds();
}

/* ------------------------------------------------------------------ */
/* Public display API                                                  */
/* ------------------------------------------------------------------ */

SDL_DisplayID *SDL_GetDisplays(int *count)
{
    if (!sdlop.video) {
        if (count) {
            *count = 0;
        }
        (void)SDL_SetError("Video subsystem not initialized");
        return NULL;
    }
    SDL_DisplayID *ids = (SDL_DisplayID *)calloc((size_t)sdlop.num_displays + 1, sizeof(SDL_DisplayID));
    if (!ids) {
        if (count) {
            *count = 0;
        }
        SDL_OutOfMemory();
        return NULL;
    }
    for (int i = 0; i < sdlop.num_displays; i++) {
        ids[i] = sdlop.displays[i].id;
    }
    ids[sdlop.num_displays] = 0;
    if (count) {
        *count = sdlop.num_displays;
    }
    return ids; /* caller frees with free() (SDLop has no SDL_free) */
}

SDL_DisplayID SDL_GetPrimaryDisplay(void)
{
    if (!sdlop.video || sdlop.num_displays == 0) {
        (void)SDL_SetError("Video subsystem not initialized");
        return 0;
    }
    return sdlop.displays[0].id;
}

SDL_PropertiesID SDL_GetDisplayProperties(SDL_DisplayID displayID)
{
    SDLop_Display *d = display_by_id(displayID);
    if (!d) {
        SDL_SetError("Invalid display");
        return 0;
    }
    if (d->props == 0) {
        d->props = SDL_CreateProperties();
        if (d->props) {
            SDL_SetBooleanProperty(d->props, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
        }
    }
    return d->props;
}

const char *SDL_GetDisplayName(SDL_DisplayID displayID)
{
    SDLop_Display *d = display_by_id(displayID);
    return d ? d->name : NULL;
}

bool SDL_GetDisplayBounds(SDL_DisplayID displayID, SDL_Rect *rect)
{
    SDLop_Display *d = display_by_id(displayID);
    if (!d) {
        SDL_SetError("Invalid display");
        return false;
    }
    if (!rect) {
        SDL_SetError("rect is NULL");
        return false;
    }
    *rect = d->bounds;
    return true;
}

bool SDL_GetDisplayUsableBounds(SDL_DisplayID displayID, SDL_Rect *rect)
{
    /* SDLop has no chrome; usable == entire display bounds */
    return SDL_GetDisplayBounds(displayID, rect);
}

SDL_DisplayOrientation SDL_GetNaturalDisplayOrientation(SDL_DisplayID displayID)
{
    SDLop_Display *d = display_by_id(displayID);
    return d ? d->natural_orientation : SDL_ORIENTATION_UNKNOWN;
}

SDL_DisplayOrientation SDL_GetCurrentDisplayOrientation(SDL_DisplayID displayID)
{
    SDLop_Display *d = display_by_id(displayID);
    return d ? d->current_orientation : SDL_ORIENTATION_UNKNOWN;
}

float SDL_GetDisplayContentScale(SDL_DisplayID displayID)
{
    SDLop_Display *d = display_by_id(displayID);
    return d ? d->content_scale : 1.0f;
}

SDL_DisplayMode **SDL_GetFullscreenDisplayModes(SDL_DisplayID displayID, int *count)
{
    if (count) {
        *count = 0;
    }
    SDLop_Display *d = display_by_id(displayID);
    if (!d) {
        SDL_SetError("Invalid display");
        return NULL;
    }
    int num_modes = d->num_fullscreen_modes;
    SDL_DisplayMode **result = (SDL_DisplayMode **)malloc(
        (size_t)(num_modes + 1) * sizeof(*result) + (size_t)num_modes * sizeof(**result));
    if (!result) {
        SDL_OutOfMemory();
        return NULL;
    }
    SDL_DisplayMode *modes = (SDL_DisplayMode *)((Uint8 *)result + (size_t)(num_modes + 1) * sizeof(*result));
    if (num_modes) {
        memcpy(modes, d->fullscreen_modes, (size_t)num_modes * sizeof(*modes));
    }
    for (int i = 0; i < num_modes; i++) {
        result[i] = &modes[i];
    }
    result[num_modes] = NULL;
    if (count) {
        *count = num_modes;
    }
    return result;
}

bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID displayID, int w, int h, float refresh_rate, bool include_high_density_modes, SDL_DisplayMode *result)
{
    if (!result) {
        return SDL_SetError("Invalid parameter 'closest'");
    }
    memset(result, 0, sizeof(*result));

    SDLop_Display *d = display_by_id(displayID);
    if (!d) {
        return SDL_SetError("Invalid display");
    }

    float aspect_ratio;
    if (h > 0) {
        aspect_ratio = (float)w / (float)h;
    } else {
        aspect_ratio = 1.0f;
    }

    if (refresh_rate == 0.0f) {
        refresh_rate = d->desktop_mode.refresh_rate;
    }

    const SDL_DisplayMode *mode, *closest = NULL;
    for (int i = 0; i < d->num_fullscreen_modes; i++) {
        mode = &d->fullscreen_modes[i];

        if (w > mode->w) {
            /* modes are sorted largest-first: no larger modes remain */
            break;
        }
        if (h > mode->h) {
            /* wider, but not tall enough; closer modes may still follow */
            continue;
        }
        if (mode->pixel_density > 1.0f && !include_high_density_modes) {
            continue;
        }
        if (closest) {
            float current_aspect_ratio = (float)mode->w / (float)mode->h;
            float closest_aspect_ratio = (float)closest->w / (float)closest->h;
            if (fabsf(aspect_ratio - closest_aspect_ratio) < fabsf(aspect_ratio - current_aspect_ratio)) {
                /* the mode we already found has a better aspect ratio match */
                continue;
            }
            if (mode->w == closest->w && mode->h == closest->h) {
                if (fabsf(closest->refresh_rate - refresh_rate) < fabsf(mode->refresh_rate - refresh_rate)) {
                    continue;
                }
                if (SDLOP_BytesPerPixel(closest->format) > SDLOP_BytesPerPixel(mode->format)) {
                    /* prefer the highest color depth */
                    continue;
                }
            }
        }
        closest = mode;
    }
    if (!closest) {
        return SDL_SetError("Couldn't find any matching video modes");
    }
    *result = *closest;
    return true;
}

const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID displayID)
{
    SDLop_Display *d = display_by_id(displayID);
    return d ? &d->desktop_mode : NULL;
}

const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID displayID)
{
    /* SDLop presents in the display's desktop mode; there is no separate
     * current-mode tracking yet */
    SDLop_Display *d = display_by_id(displayID);
    return d ? &d->desktop_mode : NULL;
}

SDL_DisplayID SDL_GetDisplayForPoint(const SDL_Point *point)
{
    if (!point) {
        return SDL_GetPrimaryDisplay();
    }
    for (int i = 0; i < sdlop.num_displays; i++) {
        SDLop_Display *d = &sdlop.displays[i];
        if (point->x >= d->bounds.x && point->x < d->bounds.x + d->bounds.w &&
            point->y >= d->bounds.y && point->y < d->bounds.y + d->bounds.h) {
            return d->id;
        }
    }
    return SDL_GetPrimaryDisplay();
}

SDL_DisplayID SDL_GetDisplayForRect(const SDL_Rect *rect)
{
    if (!rect) {
        return SDL_GetPrimaryDisplay();
    }
    SDL_DisplayID best = 0;
    int64_t best_overlap = 0;
    for (int i = 0; i < sdlop.num_displays; i++) {
        SDLop_Display *d = &sdlop.displays[i];
        int x1 = rect->x > d->bounds.x ? rect->x : d->bounds.x;
        int y1 = rect->y > d->bounds.y ? rect->y : d->bounds.y;
        int x2 = rect->x + rect->w < d->bounds.x + d->bounds.w ? rect->x + rect->w : d->bounds.x + d->bounds.w;
        int y2 = rect->y + rect->h < d->bounds.y + d->bounds.h ? rect->y + rect->h : d->bounds.y + d->bounds.h;
        if (x2 > x1 && y2 > y1) {
            int64_t overlap = (int64_t)(x2 - x1) * (int64_t)(y2 - y1);
            if (overlap > best_overlap) {
                best_overlap = overlap;
                best = d->id;
            }
        }
    }
    if (best) {
        return best;
    }
    SDL_Point center = { rect->x + rect->w / 2, rect->y + rect->h / 2 };
    return SDL_GetDisplayForPoint(&center);
}

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window)
{
    if (!window) {
        return 0;
    }
    /* an explicit fullscreen display overrides all */
    if ((window->flags & SDL_WINDOW_FULLSCREEN) && window->has_fullscreen_mode
        && window->fullscreen_mode.displayID) {
        return window->fullscreen_mode.displayID;
    }
    SDL_Rect wr = { window->x, window->y, window->w, window->h };
    return SDL_GetDisplayForRect(&wr);
}

float SDL_GetWindowPixelDensity(SDL_Window *window)
{
    (void)window;
    return 1.0f; /* SDLop has no high-density path yet */
}

/* ------------------------------------------------------------------ */
/* Public window API                                                   */
/* ------------------------------------------------------------------ */

SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags)
{
    if (!sdlop.init_done || !sdlop.video) {
        SDL_SetError("Video subsystem not initialized");
        return NULL;
    }
    if (w <= 0) {
        w = 640;
    }
    if (h <= 0) {
        h = 480;
    }

    SDL_Window *window = (SDL_Window *)calloc(1, sizeof(*window));
    if (!window) {
        SDL_OutOfMemory();
        return NULL;
    }
    window->id = ++sdlop.next_window_id;
    window->title = strdup(title ? title : "");
    window->w = w;
    window->h = h;
    window->x = 0;
    window->y = 0;
    window->display_scale = 1.0f;
    window->flags = flags | SDL_WINDOW_HIDDEN; /* shown below unless HIDDEN */
    window->clear_r = 0x20;
    window->clear_g = 0x20;
    window->clear_b = 0x28;

    if (!sdlop.video->CreateWindow(sdlop.video, window)) {
        free(window->title);
        free(window);
        return NULL; /* driver sets error */
    }

    SDLOP_AddWindow(window);
    SDLOP_UpdateWindowDisplay(window);

    if (!(flags & SDL_WINDOW_HIDDEN)) {
        SDL_ShowWindow(window);
    }
    return window;
}

void SDL_DestroyWindow(SDL_Window *window)
{
    if (!window) {
        return;
    }
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_DESTROYED, 0, 0);
    if (window->surface) {
        /* drivers that own the pixel storage null this out themselves;
         * otherwise (dummy RAM buffers) free it here */
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
    if (sdlop.video) {
        sdlop.video->DestroyWindow(sdlop.video, window);
    }
    SDLOP_RemoveWindow(window);
    free(window->title);
    free(window);
}

SDL_WindowID SDL_GetWindowID(SDL_Window *window)
{
    return window ? window->id : 0;
}

SDL_Window *SDL_GetWindowFromID(SDL_WindowID id)
{
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        if (w->id == id) {
            return w;
        }
    }
    return NULL;
}

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window)
{
    return window ? window->flags : 0;
}

bool SDL_SetWindowTitle(SDL_Window *window, const char *title)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    char *dup = strdup(title ? title : "");
    if (!dup) {
        return SDL_OutOfMemory();
    }
    free(window->title);
    window->title = dup;
    if (sdlop.video && sdlop.video->SetWindowTitle) {
        return sdlop.video->SetWindowTitle(sdlop.video, window);
    }
    return true;
}

const char *SDL_GetWindowTitle(SDL_Window *window)
{
    return window ? window->title : "";
}

bool SDL_SetWindowSize(SDL_Window *window, int w, int h)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (w <= 0 || h <= 0) {
        return SDL_SetError("Invalid window size");
    }
    window->w = w;
    window->h = h;
    if (sdlop.video && sdlop.video->SetWindowSize) {
        return sdlop.video->SetWindowSize(sdlop.video, window);
    }
    return true;
}

bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (w) {
        *w = window->w;
    }
    if (h) {
        *h = window->h;
    }
    return true;
}

bool SDL_SetWindowPosition(SDL_Window *window, int x, int y)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (SDL_WINDOWPOS_ISUNDEFINED(x) || SDL_WINDOWPOS_ISUNDEFINED(y) ||
        SDL_WINDOWPOS_ISCENTERED(x) || SDL_WINDOWPOS_ISCENTERED(y)) {
        return true; /* compositor decides placement */
    }
    window->x = x;
    window->y = y;
    bool ok = true;
    if (sdlop.video && sdlop.video->SetWindowPosition) {
        ok = sdlop.video->SetWindowPosition(sdlop.video, window);
    }
    /* keep window->displayID and the DISPLAY_CHANGED event in sync with
     * where the window actually landed */
    SDLOP_UpdateWindowDisplay(window);
    return ok;
}

bool SDL_GetWindowPosition(SDL_Window *window, int *x, int *y)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (x) {
        *x = window->x;
    }
    if (y) {
        *y = window->y;
    }
    return true;
}

bool SDL_ShowWindow(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (window->flags & SDL_WINDOW_HIDDEN) {
        window->flags &= ~SDL_WINDOW_HIDDEN;
        if (sdlop.video && sdlop.video->ShowWindow) {
            sdlop.video->ShowWindow(sdlop.video, window);
        }
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_SHOWN, 0, 0);
    }
    return true;
}

bool SDL_HideWindow(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (!(window->flags & SDL_WINDOW_HIDDEN)) {
        window->flags |= SDL_WINDOW_HIDDEN;
        if (sdlop.video && sdlop.video->HideWindow) {
            sdlop.video->HideWindow(sdlop.video, window);
        }
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_HIDDEN, 0, 0);
    }
    return true;
}

bool SDL_MinimizeWindow(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (sdlop.video && sdlop.video->MinimizeWindow) {
        sdlop.video->MinimizeWindow(sdlop.video, window);
    }
    return true;
}

bool SDL_MaximizeWindow(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (sdlop.video && sdlop.video->MaximizeWindow) {
        sdlop.video->MaximizeWindow(sdlop.video, window);
    }
    return true;
}

bool SDL_RestoreWindow(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (sdlop.video && sdlop.video->RestoreWindow) {
        sdlop.video->RestoreWindow(sdlop.video, window);
    }
    return true;
}

bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    bool want = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
    if (fullscreen == want) {
        return true; /* already in the requested state */
    }
    if (fullscreen) {
        window->flags |= SDL_WINDOW_FULLSCREEN;
    } else {
        window->flags &= ~SDL_WINDOW_FULLSCREEN;
    }
    if (sdlop.video && sdlop.video->SetWindowFullscreen) {
        if (!sdlop.video->SetWindowFullscreen(sdlop.video, window)) {
            /* revert on failure */
            if (fullscreen) {
                window->flags &= ~SDL_WINDOW_FULLSCREEN;
            } else {
                window->flags |= SDL_WINDOW_FULLSCREEN;
            }
            return false;
        }
    }
    return true;
}

bool SDL_SetWindowFullscreenMode(SDL_Window *window, const SDL_DisplayMode *mode)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (mode) {
        if (mode->w <= 0 || mode->h <= 0) {
            /* w/h <= 0 means borderless fullscreen desktop mode */
            window->has_fullscreen_mode = false;
            memset(&window->fullscreen_mode, 0, sizeof(window->fullscreen_mode));
        } else {
            SDL_DisplayMode stored = *mode;
            if (stored.displayID == 0) {
                stored.displayID = (window->flags & SDL_WINDOW_FULLSCREEN) && window->displayID
                    ? window->displayID
                    : SDL_GetPrimaryDisplay();
            }
            finalize_display_mode(&stored);
            window->fullscreen_mode = stored;
            window->has_fullscreen_mode = true;
        }
    } else {
        window->has_fullscreen_mode = false;
        memset(&window->fullscreen_mode, 0, sizeof(window->fullscreen_mode));
    }

    /* if fullscreen is in effect, re-apply so the mode takes hold now */
    if ((window->flags & SDL_WINDOW_FULLSCREEN) != 0 && sdlop.video && sdlop.video->SetWindowFullscreen) {
        if (!sdlop.video->SetWindowFullscreen(sdlop.video, window)) {
            return false; /* driver sets error */
        }
        SDLOP_UpdateWindowDisplay(window);
    }
    return true;
}

const SDL_DisplayMode *SDL_GetWindowFullscreenMode(SDL_Window *window)
{
    if (!window || !window->has_fullscreen_mode) {
        return NULL;
    }
    return &window->fullscreen_mode;
}

bool SDL_RaiseWindow(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (sdlop.video && sdlop.video->RaiseWindow) {
        sdlop.video->RaiseWindow(sdlop.video, window);
    }
    return true;
}

const char *SDL_GetCurrentVideoDriver(void)
{
    return sdlop.video ? sdlop.video->name : NULL;
}

int SDL_GetNumVideoDrivers(void)
{
    return SDLOP_NUM_VIDEO_DRIVERS;
}

const char *SDL_GetVideoDriver(int index)
{
    if (index >= 0 && index < SDLOP_NUM_VIDEO_DRIVERS) {
        return video_drivers[index]->name;
    }
    return NULL;
}

void SDLop_SetWindowClearColor(SDL_Window *window, Uint8 r, Uint8 g, Uint8 b)
{
    if (!window) {
        return;
    }
    window->clear_r = r;
    window->clear_g = g;
    window->clear_b = b;
    if (sdlop.video && sdlop.video->SetWindowClearColor) {
        sdlop.video->SetWindowClearColor(sdlop.video, window);
    }
}

float SDLCALL SDL_GetWindowDisplayScale(SDL_Window *window)
{
    if (!window) {
        SDL_SetError("Invalid window");
        return 1.0f;
    }
    return window->display_scale;
}
