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
#if SDLop_VIDEO_X11
    &SDLop_x11_device,
#endif
#if SDLop_VIDEO_KMSDRM
    &SDLop_kmsdrm_device,
#endif
#if SDLop_VIDEO_FBDEV
    &SDLop_fbdev_device,
#endif
    &SDLop_dummy_device,
};

#define SDLOP_NUM_VIDEO_DRIVERS ((int)SDL_arraysize(video_drivers))

static const char *default_driver_order(void)
{
    const char *hint = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    return hint;
}

static void video_post_init(void)
{
    sdlop.grabbed_window = NULL;
    sdlop.suspend_screensaver = false;
    /* stock SDL3 inhibits the screensaver unless the app opts in */
    if (!SDL_GetHintBoolean(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, false)) {
        sdlop.suspend_screensaver = true;
        if (sdlop.video->SuspendScreenSaver) {
            sdlop.video->SuspendScreenSaver(sdlop.video);
        }
    }
}

bool SDLOP_VideoInit(void)
{
    const char *driver = default_driver_order();

    if (driver && driver[0]) {
        for (int i = 0; i < SDLOP_NUM_VIDEO_DRIVERS; i++) {
            if (strcasecmp(video_drivers[i]->name, driver) == 0) {
                if (video_drivers[i]->Init(video_drivers[i])) {
                    sdlop.video = video_drivers[i];
                    video_post_init();
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
            SDL_ClearError(); /* drop errors of drivers tried before */
            video_post_init();
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
    SDLOP_GL_Quit();
    SDLOP_Vulkan_Quit();
    SDLOP_MouseQuit(); /* cursors hold driver resources */
    sdlop.grabbed_window = NULL;
    sdlop.suspend_screensaver = false;
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

static struct
{
    const char *property_name;
    SDL_WindowFlags flag;
    bool invert_value;
} window_flag_props[] = {
    { SDL_PROP_WINDOW_CREATE_ALWAYS_ON_TOP_BOOLEAN, SDL_WINDOW_ALWAYS_ON_TOP, false },
    { SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, SDL_WINDOW_BORDERLESS, false },
    { SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, SDL_WINDOW_NOT_FOCUSABLE, true },
    { SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, SDL_WINDOW_FULLSCREEN, false },
    { SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, SDL_WINDOW_HIDDEN, false },
    { SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, SDL_WINDOW_HIGH_PIXEL_DENSITY, false },
    { SDL_PROP_WINDOW_CREATE_MAXIMIZED_BOOLEAN, SDL_WINDOW_MAXIMIZED, false },
    { SDL_PROP_WINDOW_CREATE_MENU_BOOLEAN, SDL_WINDOW_POPUP_MENU, false },
    { SDL_PROP_WINDOW_CREATE_METAL_BOOLEAN, SDL_WINDOW_METAL, false },
    { SDL_PROP_WINDOW_CREATE_MINIMIZED_BOOLEAN, SDL_WINDOW_MINIMIZED, false },
    { SDL_PROP_WINDOW_CREATE_MODAL_BOOLEAN, SDL_WINDOW_MODAL, false },
    { SDL_PROP_WINDOW_CREATE_MOUSE_GRABBED_BOOLEAN, SDL_WINDOW_MOUSE_GRABBED, false },
    { SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, SDL_WINDOW_OPENGL, false },
    { SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, SDL_WINDOW_RESIZABLE, false },
    { SDL_PROP_WINDOW_CREATE_TRANSPARENT_BOOLEAN, SDL_WINDOW_TRANSPARENT, false },
    { SDL_PROP_WINDOW_CREATE_TOOLTIP_BOOLEAN, SDL_WINDOW_TOOLTIP, false },
    { SDL_PROP_WINDOW_CREATE_UTILITY_BOOLEAN, SDL_WINDOW_UTILITY, false },
    { SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, SDL_WINDOW_VULKAN, false },
};

static SDL_WindowFlags window_flags_from_props(SDL_PropertiesID props)
{
    SDL_WindowFlags flags = (SDL_WindowFlags)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, 0);
    for (size_t i = 0; i < SDL_arraysize(window_flag_props); ++i) {
        if (window_flag_props[i].invert_value) {
            if (!SDL_GetBooleanProperty(props, window_flag_props[i].property_name, true)) {
                flags |= window_flag_props[i].flag;
            }
        } else if (SDL_GetBooleanProperty(props, window_flag_props[i].property_name, false)) {
            flags |= window_flag_props[i].flag;
        }
    }
    return flags;
}

bool SDLOP_WindowValid(SDL_Window *window)
{
    if (window) {
        for (SDL_Window *w = sdlop.windows; w; w = w->next) {
            if (w == window) {
                return true;
            }
        }
    }
    return false;
}

/* CHECK_WINDOW_MAGIC */
#define CHECK_WINDOW(window, retval)                     \
    if (!sdlop.video) {                                  \
        SDL_SetError("Video subsystem has not been initialized"); \
        return retval;                                   \
    }                                                    \
    if (!SDLOP_WindowValid(window)) {                    \
        SDL_SetError("Invalid window");                  \
        return retval;                                   \
    }
#define CHECK_WINDOW_NOT_POPUP(window, retval)           \
    if (SDLOP_WINDOW_IS_POPUP(window)) {                 \
        SDL_SetError("Operation invalid on popup windows"); \
        return retval;                                   \
    }

SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID props)
{
    if (!sdlop.init_done || !sdlop.video) {
        SDL_SetError("Video subsystem not initialized");
        return NULL;
    }
    const char *title = SDL_GetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, NULL);
    int x = (int)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_UNDEFINED);
    int y = (int)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_UNDEFINED);
    int w = (int)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 0);
    int h = (int)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 0);
    SDL_Window *parent = (SDL_Window *)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_CREATE_PARENT_POINTER, NULL);
    SDL_WindowFlags flags = window_flags_from_props(props);
    bool undefined_x = false, undefined_y = false;

    if ((flags & SDL_WINDOW_MODAL) && !SDLOP_WindowValid(parent)) {
        SDL_SetError("Modal windows must specify a parent window");
        return NULL;
    }
    bool popup = (flags & (SDL_WINDOW_TOOLTIP | SDL_WINDOW_POPUP_MENU)) != 0;
    if (popup) {
        if (!sdlop.video->supports_popups) {
            SDL_Unsupported();
            return NULL;
        }
        if (!SDLOP_WindowValid(parent)) {
            SDL_SetError("Tooltip and popup menu windows must specify a parent window");
            return NULL;
        }
        flags &= ~(SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS);
    }
    SDL_WindowFlags type_flags = flags & (SDL_WINDOW_UTILITY | SDL_WINDOW_TOOLTIP | SDL_WINDOW_POPUP_MENU | SDL_WINDOW_MODAL);
    if (type_flags & (type_flags - 1)) {
        SDL_SetError("Conflicting window type flags specified: 0x%.8x", (unsigned int)type_flags);
        return NULL;
    }
    SDL_WindowFlags graphics_flags = flags & (SDL_WINDOW_OPENGL | SDL_WINDOW_METAL | SDL_WINDOW_VULKAN);
    if (graphics_flags & (graphics_flags - 1)) {
        SDL_SetError("Conflicting window graphics flags specified: 0x%.8x", (unsigned int)graphics_flags);
        return NULL;
    }
    if (flags & SDL_WINDOW_METAL) {
        SDL_SetError("Metal support is either not configured in SDL or not available in current SDL video driver (%s) or platform", sdlop.video->name);
        return NULL;
    }
    if ((flags & SDL_WINDOW_OPENGL) && !SDLOP_GL_CanCreateWindow()) {
        SDL_SetError("OpenGL support is either not configured in SDL or not available in current SDL video driver (%s) or platform", sdlop.video->name);
        return NULL;
    }
    if ((flags & SDL_WINDOW_VULKAN) && !SDLOP_Vulkan_CanCreateWindow()) {
        SDL_SetError("Vulkan support is either not configured in SDL or not available in current SDL video driver (%s) or platform", sdlop.video->name);
        return NULL;
    }

    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    int popup_x = x, popup_y = y;
    if (popup) {
        /* x/y are relative to the parent */
        x = parent->x + popup_x;
        y = parent->y + popup_y;
    } else if (SDL_WINDOWPOS_ISUNDEFINED(x) || SDL_WINDOWPOS_ISUNDEFINED(y) ||
               SDL_WINDOWPOS_ISCENTERED(x) || SDL_WINDOWPOS_ISCENTERED(y)) {
        SDL_DisplayID displayID = 0;
        SDL_Rect bounds;
        if ((SDL_WINDOWPOS_ISUNDEFINED(x) || SDL_WINDOWPOS_ISCENTERED(x)) && (x & 0xFFFF)) {
            displayID = (SDL_DisplayID)(x & 0xFFFF);
        } else if ((SDL_WINDOWPOS_ISUNDEFINED(y) || SDL_WINDOWPOS_ISCENTERED(y)) && (y & 0xFFFF)) {
            displayID = (SDL_DisplayID)(y & 0xFFFF);
        }
        if (displayID == 0 || SDLOP_GetDisplayIndexById(displayID) < 0) {
            displayID = SDL_GetPrimaryDisplay();
        }
        SDL_zero(bounds);
        SDL_GetDisplayUsableBounds(displayID, &bounds);
        if (w > bounds.w || h > bounds.h) {
            SDL_GetDisplayBounds(displayID, &bounds);
        }
        if (SDL_WINDOWPOS_ISCENTERED(x) || SDL_WINDOWPOS_ISUNDEFINED(x)) {
            undefined_x = SDL_WINDOWPOS_ISUNDEFINED(x);
            x = bounds.x + (bounds.w - w) / 2;
        }
        if (SDL_WINDOWPOS_ISCENTERED(y) || SDL_WINDOWPOS_ISUNDEFINED(y)) {
            undefined_y = SDL_WINDOWPOS_ISUNDEFINED(y);
            y = bounds.y + (bounds.h - h) / 2;
        }
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
    window->x = x;
    window->y = y;
    window->undefined_x = undefined_x;
    window->undefined_y = undefined_y;
    window->popup_offset_x = popup_x;
    window->popup_offset_y = popup_y;
    window->display_scale = 1.0f;
    window->opacity = 1.0f;
    window->parent = parent;
    /* grabs are applied once the window is shown (SDL_FinishWindowCreation) */
    window->pending_flags = flags & (SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED);
    flags &= ~(SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED);
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
    SDL_ClearError();
    return window;
}

SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    if (title && *title) {
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    }
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, (Sint64)flags);
    SDL_Window *window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return window;
}

SDL_Window *SDL_CreatePopupWindow(SDL_Window *parent, int offset_x, int offset_y, int w, int h, SDL_WindowFlags flags)
{
    if (!(flags & (SDL_WINDOW_TOOLTIP | SDL_WINDOW_POPUP_MENU))) {
        SDL_SetError("Popup windows must specify either the 'SDL_WINDOW_TOOLTIP' or the 'SDL_WINDOW_POPUP_MENU' flag");
        return NULL;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_PARENT_POINTER, parent);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, offset_x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, offset_y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, (Sint64)flags);
    SDL_Window *window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return window;
}

void SDL_DestroyWindow(SDL_Window *window)
{
    if (!SDLOP_WindowValid(window)) {
        return;
    }
    window->is_destroying = true;

    /* children (popups, transient windows) go first */
    for (;;) {
        SDL_Window *child = NULL;
        for (SDL_Window *w = sdlop.windows; w; w = w->next) {
            if (w->parent == window) {
                child = w;
                break;
            }
        }
        if (!child) {
            break;
        }
        SDL_DestroyWindow(child);
    }

    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_DESTROYED, 0, 0);
    SDLOP_GL_NotifyWindowDestroyed(window);
    SDLOP_Vulkan_NotifyWindowDestroyed(window);
    if (window->surface) {
        /* drivers that own the pixel storage null this out themselves;
         * otherwise (dummy RAM buffers) free it here */
        SDL_DestroySurface(window->surface);
        window->surface = NULL;
    }
    if (sdlop.video && sdlop.video->SetWindowModal && (window->flags & SDL_WINDOW_MODAL)) {
        sdlop.video->SetWindowModal(sdlop.video, window, false);
    }
    if (sdlop.grabbed_window == window) {
        sdlop.grabbed_window = NULL;
    }
    if (sdlop.video) {
        sdlop.video->DestroyWindow(sdlop.video, window);
    }
    SDLOP_RemoveWindow(window);
    SDL_DestroyProperties(window->props);
    if (window->text_input_props) {
        SDL_DestroyProperties(window->text_input_props);
    }
    SDL_DestroySurface(window->icon);
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
    if (w <= 0) {
        return SDL_InvalidParamError("w");
    }
    if (h <= 0) {
        return SDL_InvalidParamError("h");
    }
    /* aspect first, then the size limits win (stock order) */
    float new_aspect = w / (float)h;
    if (window->max_aspect > 0.0f && new_aspect > window->max_aspect) {
        w = (int)SDL_roundf(h * window->max_aspect);
    } else if (window->min_aspect > 0.0f && new_aspect < window->min_aspect) {
        h = (int)SDL_roundf(w / window->min_aspect);
    }
    if (window->min_w && w < window->min_w) {
        w = window->min_w;
    }
    if (window->max_w && w > window->max_w) {
        w = window->max_w;
    }
    if (window->min_h && h < window->min_h) {
        h = window->min_h;
    }
    if (window->max_h && h > window->max_h) {
        h = window->max_h;
    }
    window->w = w;
    window->h = h;
    bool ok = true;
    if (sdlop.video && sdlop.video->SetWindowSize) {
        ok = sdlop.video->SetWindowSize(sdlop.video, window);
    }
    if (ok) {
        SDLOP_GL_NotifyWindowResized(window);
    }
    return ok;
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
    if (SDLOP_WINDOW_IS_POPUP(window) && window->parent) {
        /* popup coordinates are relative to the parent (stock) */
        window->popup_offset_x = x;
        window->popup_offset_y = y;
        x += window->parent->x;
        y += window->parent->y;
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
    int px = 0, py = 0;
    if (SDLOP_WINDOW_IS_POPUP(window) && window->parent) {
        px = window->parent->x;
        py = window->parent->y;
    }
    if (x) {
        *x = window->x - px;
    }
    if (y) {
        *y = window->y - py;
    }
    return true;
}

void SDLOP_OnWindowMoved(SDL_Window *window)
{
    if (SDLOP_WINDOW_IS_POPUP(window) && window->parent) {
        window->popup_offset_x = window->x - window->parent->x;
        window->popup_offset_y = window->y - window->parent->y;
    }
    /* popups follow their parent (stock X11/Wayland behaviour) */
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        if (w->parent == window && SDLOP_WINDOW_IS_POPUP(w) && !w->is_destroying) {
            const int nx = window->x + w->popup_offset_x;
            const int ny = window->y + w->popup_offset_y;
            if (nx != w->x || ny != w->y) {
                w->x = nx;
                w->y = ny;
                if (sdlop.video && sdlop.video->SetWindowPosition) {
                    sdlop.video->SetWindowPosition(sdlop.video, w);
                }
                SDLOP_OnWindowMoved(w);
            }
        }
    }
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
        if ((window->flags & SDL_WINDOW_MODAL) && sdlop.video && sdlop.video->SetWindowModal) {
            sdlop.video->SetWindowModal(sdlop.video, window, true);
        }
        /* grabs requested while hidden take effect now */
        SDL_WindowFlags pending = window->pending_flags;
        window->pending_flags = 0;
        if (pending & SDL_WINDOW_MOUSE_GRABBED) {
            SDL_SetWindowMouseGrab(window, true);
        }
        if (pending & SDL_WINDOW_KEYBOARD_GRABBED) {
            SDL_SetWindowKeyboardGrab(window, true);
        }
    }
    return true;
}

bool SDL_HideWindow(SDL_Window *window)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (!(window->flags & SDL_WINDOW_HIDDEN)) {
        /* grabs come back when the window is shown again */
        window->pending_flags |= window->flags & (SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED);
        if (window->flags & (SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED)) {
            window->flags &= ~(SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED);
            SDLOP_UpdateWindowGrab(window);
        }
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

/* ------------------------------------------------------------------ */
/* SDL 3.4 window management                                           */
/* ------------------------------------------------------------------ */

SDL_Window **SDLCALL SDL_GetWindows(int *count)
{
    if (count) {
        *count = 0;
    }
    if (!sdlop.video) {
        SDL_SetError("Video subsystem has not been initialized");
        return NULL;
    }
    int n = 0;
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        ++n;
    }
    SDL_Window **list = (SDL_Window **)SDL_malloc((size_t)(n + 1) * sizeof(*list));
    if (!list) {
        return NULL;
    }
    int i = 0;
    for (SDL_Window *w = sdlop.windows; w && i < n; w = w->next) {
        list[i++] = w;
    }
    list[i] = NULL;
    if (count) {
        *count = i;
    }
    return list;
}

SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *window)
{
    CHECK_WINDOW(window, 0);
    if (window->props == 0) {
        window->props = SDL_CreateProperties();
    }
    return window->props;
}

SDL_Window *SDL_GetWindowParent(SDL_Window *window)
{
    CHECK_WINDOW(window, NULL);
    return window->parent;
}

SDL_PixelFormat SDL_GetWindowPixelFormat(SDL_Window *window)
{
    CHECK_WINDOW(window, SDL_PIXELFORMAT_UNKNOWN);
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    return mode ? mode->format : SDL_PIXELFORMAT_UNKNOWN;
}

bool SDL_GetWindowSafeArea(SDL_Window *window, SDL_Rect *rect)
{
    if (rect) {
        SDL_zerop(rect);
    }
    CHECK_WINDOW(window, false);
    if (rect) {
        if (SDL_RectEmpty(&window->safe_rect)) {
            rect->w = window->w;
            rect->h = window->h;
        } else {
            *rect = window->safe_rect;
        }
    }
    return true;
}

void *SDL_GetWindowICCProfile(SDL_Window *window, size_t *size)
{
    CHECK_WINDOW(window, NULL);
    if (!sdlop.video->GetWindowICCProfile) {
        SDL_Unsupported();
        return NULL;
    }
    return sdlop.video->GetWindowICCProfile(sdlop.video, window, size);
}

bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h)
{
    int filter;
    CHECK_WINDOW(window, false);
    if (!w) {
        w = &filter;
    }
    if (!h) {
        h = &filter;
    }
    SDL_GetWindowSize(window, w, h);
    const SDL_DisplayMode *mode;
    SDL_DisplayID displayID = SDL_GetDisplayForWindow(window);
    if ((window->flags & SDL_WINDOW_FULLSCREEN) && SDL_GetWindowFullscreenMode(window)) {
        mode = SDL_GetCurrentDisplayMode(displayID);
    } else {
        mode = SDL_GetDesktopDisplayMode(displayID);
    }
    if (mode) {
        *w = (int)SDL_ceilf(*w * mode->pixel_density);
        *h = (int)SDL_ceilf(*h * mode->pixel_density);
    }
    return true;
}

bool SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon)
{
    CHECK_WINDOW(window, false);
    if (!icon) {
        return SDL_InvalidParamError("icon");
    }
    SDL_DestroySurface(window->icon);
    window->icon = SDL_ConvertSurface(icon, SDL_PIXELFORMAT_ARGB8888);
    if (!window->icon) {
        return false;
    }
    if (!sdlop.video->SetWindowIcon) {
        return SDL_Unsupported();
    }
    return sdlop.video->SetWindowIcon(sdlop.video, window, window->icon);
}

static bool toggle_flag(SDL_Window *window, SDL_WindowFlags flag, bool on)
{
    bool have = (window->flags & flag) != 0;
    if (have == on) {
        return false;
    }
    if (on) {
        window->flags |= flag;
    } else {
        window->flags &= ~flag;
    }
    return true;
}

bool SDL_SetWindowBordered(SDL_Window *window, bool bordered)
{
    CHECK_WINDOW(window, false);
    CHECK_WINDOW_NOT_POPUP(window, false);
    if (!sdlop.video->SetWindowBordered) {
        return SDL_Unsupported();
    }
    if (toggle_flag(window, SDL_WINDOW_BORDERLESS, !bordered)) {
        sdlop.video->SetWindowBordered(sdlop.video, window, bordered);
    }
    return true;
}

bool SDL_SetWindowResizable(SDL_Window *window, bool resizable)
{
    CHECK_WINDOW(window, false);
    CHECK_WINDOW_NOT_POPUP(window, false);
    if (!sdlop.video->SetWindowResizable) {
        return SDL_Unsupported();
    }
    if (toggle_flag(window, SDL_WINDOW_RESIZABLE, resizable)) {
        sdlop.video->SetWindowResizable(sdlop.video, window, resizable);
    }
    return true;
}

bool SDL_SetWindowAlwaysOnTop(SDL_Window *window, bool on_top)
{
    CHECK_WINDOW(window, false);
    CHECK_WINDOW_NOT_POPUP(window, false);
    if (!sdlop.video->SetWindowAlwaysOnTop) {
        return SDL_Unsupported();
    }
    if (toggle_flag(window, SDL_WINDOW_ALWAYS_ON_TOP, on_top)) {
        sdlop.video->SetWindowAlwaysOnTop(sdlop.video, window, on_top);
    }
    return true;
}

bool SDL_SetWindowAspectRatio(SDL_Window *window, float min_aspect, float max_aspect)
{
    CHECK_WINDOW(window, false);
    window->min_aspect = min_aspect;
    window->max_aspect = max_aspect;
    if (sdlop.video->SetWindowAspectRatio) {
        sdlop.video->SetWindowAspectRatio(sdlop.video, window);
    }
    return SDL_SetWindowSize(window, window->w, window->h);
}

bool SDL_GetWindowAspectRatio(SDL_Window *window, float *min_aspect, float *max_aspect)
{
    CHECK_WINDOW(window, false);
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
    int dummy = 0;
    if (!top) {
        top = &dummy;
    }
    if (!left) {
        left = &dummy;
    }
    if (!right) {
        right = &dummy;
    }
    if (!bottom) {
        bottom = &dummy;
    }
    *top = *left = *bottom = *right = 0;
    CHECK_WINDOW(window, false);
    if (!sdlop.video->GetWindowBordersSize) {
        return SDL_Unsupported();
    }
    return sdlop.video->GetWindowBordersSize(sdlop.video, window, top, left, bottom, right);
}

bool SDL_SetWindowMinimumSize(SDL_Window *window, int min_w, int min_h)
{
    CHECK_WINDOW(window, false);
    if (min_w < 0) {
        return SDL_InvalidParamError("min_w");
    }
    if (min_h < 0) {
        return SDL_InvalidParamError("min_h");
    }
    if ((window->max_w && min_w > window->max_w) || (window->max_h && min_h > window->max_h)) {
        return SDL_SetError("SDL_SetWindowMinimumSize(): Tried to set minimum size larger than maximum size");
    }
    window->min_w = min_w;
    window->min_h = min_h;
    if (sdlop.video->SetWindowMinimumSize) {
        sdlop.video->SetWindowMinimumSize(sdlop.video, window);
    }
    return SDL_SetWindowSize(window, window->w, window->h);
}

bool SDL_GetWindowMinimumSize(SDL_Window *window, int *min_w, int *min_h)
{
    CHECK_WINDOW(window, false);
    if (min_w) {
        *min_w = window->min_w;
    }
    if (min_h) {
        *min_h = window->min_h;
    }
    return true;
}

bool SDL_SetWindowMaximumSize(SDL_Window *window, int max_w, int max_h)
{
    CHECK_WINDOW(window, false);
    if (max_w < 0) {
        return SDL_InvalidParamError("max_w");
    }
    if (max_h < 0) {
        return SDL_InvalidParamError("max_h");
    }
    if ((max_w && max_w < window->min_w) || (max_h && max_h < window->min_h)) {
        return SDL_SetError("SDL_SetWindowMaximumSize(): Tried to set maximum size smaller than minimum size");
    }
    window->max_w = max_w;
    window->max_h = max_h;
    if (sdlop.video->SetWindowMaximumSize) {
        sdlop.video->SetWindowMaximumSize(sdlop.video, window);
    }
    return SDL_SetWindowSize(window, window->w, window->h);
}

bool SDL_GetWindowMaximumSize(SDL_Window *window, int *max_w, int *max_h)
{
    CHECK_WINDOW(window, false);
    if (max_w) {
        *max_w = window->max_w;
    }
    if (max_h) {
        *max_h = window->max_h;
    }
    return true;
}

bool SDL_SyncWindow(SDL_Window *window)
{
    CHECK_WINDOW(window, false);
    if (sdlop.video->SyncWindow) {
        return sdlop.video->SyncWindow(sdlop.video, window);
    }
    return true;
}

bool SDL_SetWindowSurfaceVSync(SDL_Window *window, int vsync)
{
    CHECK_WINDOW(window, false);
    if (!sdlop.video->SetWindowFramebufferVSync) {
        return SDL_Unsupported();
    }
    return sdlop.video->SetWindowFramebufferVSync(sdlop.video, window, vsync);
}

bool SDL_GetWindowSurfaceVSync(SDL_Window *window, int *vsync)
{
    CHECK_WINDOW(window, false);
    if (!sdlop.video->GetWindowFramebufferVSync) {
        return SDL_Unsupported();
    }
    return sdlop.video->GetWindowFramebufferVSync(sdlop.video, window, vsync);
}

bool SDL_SetWindowOpacity(SDL_Window *window, float opacity)
{
    CHECK_WINDOW(window, false);
    if (!sdlop.video->SetWindowOpacity) {
        return SDL_Unsupported();
    }
    if (opacity < 0.0f) {
        opacity = 0.0f;
    } else if (opacity > 1.0f) {
        opacity = 1.0f;
    }
    bool result = sdlop.video->SetWindowOpacity(sdlop.video, window, opacity);
    if (result) {
        window->opacity = opacity;
    }
    return result;
}

float SDL_GetWindowOpacity(SDL_Window *window)
{
    CHECK_WINDOW(window, -1.0f);
    return window->opacity;
}

bool SDL_SetWindowParent(SDL_Window *window, SDL_Window *parent)
{
    CHECK_WINDOW(window, false);
    CHECK_WINDOW_NOT_POPUP(window, false);
    if (parent) {
        CHECK_WINDOW(parent, false);
        CHECK_WINDOW_NOT_POPUP(parent, false);
    }
    if (window == parent) {
        return SDL_SetError("Cannot set the parent of a window to itself.");
    }
    if (!sdlop.video->SetWindowParent) {
        return SDL_Unsupported();
    }
    if (window->flags & SDL_WINDOW_MODAL) {
        return SDL_SetError("Modal windows cannot change parents; call SDL_SetWindowModal() to clear modal status first.");
    }
    if (window->parent == parent) {
        return true;
    }
    const bool ret = sdlop.video->SetWindowParent(sdlop.video, window, parent);
    window->parent = ret ? parent : NULL;
    return ret;
}

bool SDL_SetWindowModal(SDL_Window *window, bool modal)
{
    CHECK_WINDOW(window, false);
    CHECK_WINDOW_NOT_POPUP(window, false);
    if (!sdlop.video->SetWindowModal) {
        return SDL_Unsupported();
    }
    if (modal) {
        if (!window->parent) {
            return SDL_SetError("Window must have a parent to enable the modal state; use SDL_SetWindowParent() to set the parent first.");
        }
        window->flags |= SDL_WINDOW_MODAL;
    } else if (window->flags & SDL_WINDOW_MODAL) {
        window->flags &= ~SDL_WINDOW_MODAL;
    } else {
        return true;
    }
    if (window->flags & SDL_WINDOW_HIDDEN) {
        return true;
    }
    return sdlop.video->SetWindowModal(sdlop.video, window, modal);
}

bool SDL_SetWindowFocusable(SDL_Window *window, bool focusable)
{
    CHECK_WINDOW(window, false);
    const bool have = !(window->flags & SDL_WINDOW_NOT_FOCUSABLE);
    if (focusable != have && sdlop.video->SetWindowFocusable) {
        toggle_flag(window, SDL_WINDOW_NOT_FOCUSABLE, !focusable);
        if (!sdlop.video->SetWindowFocusable(sdlop.video, window, focusable)) {
            return false;
        }
    }
    return true;
}

bool SDL_SetWindowFillDocument(SDL_Window *window, bool fill)
{
    CHECK_WINDOW(window, false);
    /* emscripten-only in stock SDL3; a no-op everywhere else */
    (void)fill;
    return true;
}

void SDLOP_UpdateWindowGrab(SDL_Window *window)
{
    bool mouse_grabbed = false, keyboard_grabbed = false;
    if (window->flags & SDL_WINDOW_INPUT_FOCUS) {
        mouse_grabbed = (window->flags & (SDL_WINDOW_MOUSE_RELATIVE_MODE | SDL_WINDOW_MOUSE_GRABBED)) != 0;
        keyboard_grabbed = (window->flags & SDL_WINDOW_KEYBOARD_GRABBED) != 0;
    }
    SDLop_VideoDevice *dev = sdlop.video;
    if (!dev) {
        return;
    }
    if (mouse_grabbed || keyboard_grabbed) {
        if (sdlop.grabbed_window && sdlop.grabbed_window != window) {
            /* stealing a grab from another window */
            SDL_Window *old = sdlop.grabbed_window;
            old->flags &= ~(SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED);
            if (dev->SetWindowMouseGrab) {
                dev->SetWindowMouseGrab(dev, old, false);
            }
            if (dev->SetWindowKeyboardGrab) {
                dev->SetWindowKeyboardGrab(dev, old, false);
            }
        }
        sdlop.grabbed_window = window;
    } else if (sdlop.grabbed_window == window) {
        sdlop.grabbed_window = NULL;
    }
    if (dev->SetWindowMouseGrab && !dev->SetWindowMouseGrab(dev, window, mouse_grabbed)) {
        window->flags &= ~SDL_WINDOW_MOUSE_GRABBED;
    }
    if (dev->SetWindowKeyboardGrab && !dev->SetWindowKeyboardGrab(dev, window, keyboard_grabbed)) {
        window->flags &= ~SDL_WINDOW_KEYBOARD_GRABBED;
    }
    if (sdlop.grabbed_window &&
        !(sdlop.grabbed_window->flags & (SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED))) {
        sdlop.grabbed_window = NULL;
    }
}

static bool set_grab(SDL_Window *window, SDL_WindowFlags flag, bool grabbed)
{
    CHECK_WINDOW(window, false);
    CHECK_WINDOW_NOT_POPUP(window, false);
    if (window->flags & SDL_WINDOW_HIDDEN) {
        if (grabbed) {
            window->pending_flags |= flag;
        } else {
            window->pending_flags &= ~flag;
        }
        return true;
    }
    if (!toggle_flag(window, flag, grabbed)) {
        return true;
    }
    SDLOP_UpdateWindowGrab(window);
    if (grabbed && !(window->flags & flag)) {
        return false;
    }
    return true;
}

bool SDL_SetWindowKeyboardGrab(SDL_Window *window, bool grabbed)
{
    return set_grab(window, SDL_WINDOW_KEYBOARD_GRABBED, grabbed);
}

bool SDL_SetWindowMouseGrab(SDL_Window *window, bool grabbed)
{
    return set_grab(window, SDL_WINDOW_MOUSE_GRABBED, grabbed);
}

bool SDL_GetWindowKeyboardGrab(SDL_Window *window)
{
    CHECK_WINDOW(window, false);
    return window == sdlop.grabbed_window && (window->flags & SDL_WINDOW_KEYBOARD_GRABBED);
}

bool SDL_GetWindowMouseGrab(SDL_Window *window)
{
    CHECK_WINDOW(window, false);
    return window == sdlop.grabbed_window && (window->flags & SDL_WINDOW_MOUSE_GRABBED);
}

SDL_Window *SDL_GetGrabbedWindow(void)
{
    if (sdlop.grabbed_window &&
        (sdlop.grabbed_window->flags & (SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED))) {
        return sdlop.grabbed_window;
    }
    return NULL;
}

bool SDL_SetWindowMouseRect(SDL_Window *window, const SDL_Rect *rect)
{
    CHECK_WINDOW(window, false);
    if (!sdlop.video->SetWindowMouseRect) {
        return SDL_Unsupported();
    }
    SDL_Rect zero = { 0, 0, 0, 0 };
    if (!rect) {
        rect = &zero;
    }
    if (SDL_memcmp(&window->mouse_rect, rect, sizeof(*rect)) == 0) {
        return true;
    }
    window->mouse_rect = *rect;
    return sdlop.video->SetWindowMouseRect(sdlop.video, window);
}

const SDL_Rect *SDL_GetWindowMouseRect(SDL_Window *window)
{
    CHECK_WINDOW(window, NULL);
    return SDL_RectEmpty(&window->mouse_rect) ? NULL : &window->mouse_rect;
}

bool SDL_FlashWindow(SDL_Window *window, SDL_FlashOperation operation)
{
    CHECK_WINDOW(window, false);
    CHECK_WINDOW_NOT_POPUP(window, false);
    if (sdlop.video->FlashWindow) {
        return sdlop.video->FlashWindow(sdlop.video, window, operation);
    }
    return SDL_Unsupported();
}

bool SDL_SetWindowProgressState(SDL_Window *window, SDL_ProgressState state)
{
    CHECK_WINDOW(window, false);
    CHECK_WINDOW_NOT_POPUP(window, false);
    if (state < SDL_PROGRESS_STATE_NONE || state > SDL_PROGRESS_STATE_ERROR) {
        return SDL_InvalidParamError("state");
    }
    window->progress_state = state;
    return true; /* stock SDL3 forwards this over DBus (Unity launcher API) */
}

SDL_ProgressState SDL_GetWindowProgressState(SDL_Window *window)
{
    CHECK_WINDOW(window, SDL_PROGRESS_STATE_INVALID);
    CHECK_WINDOW_NOT_POPUP(window, SDL_PROGRESS_STATE_INVALID);
    return window->progress_state;
}

bool SDL_SetWindowProgressValue(SDL_Window *window, float value)
{
    CHECK_WINDOW(window, false);
    CHECK_WINDOW_NOT_POPUP(window, false);
    window->progress_value = SDL_clamp(value, 0.0f, 1.0f);
    return true;
}

float SDL_GetWindowProgressValue(SDL_Window *window)
{
    CHECK_WINDOW(window, -1.0f);
    CHECK_WINDOW_NOT_POPUP(window, -1.0f);
    return window->progress_value;
}

bool SDL_ShowWindowSystemMenu(SDL_Window *window, int x, int y)
{
    CHECK_WINDOW(window, false);
    CHECK_WINDOW_NOT_POPUP(window, false);
    if (sdlop.video->ShowWindowSystemMenu) {
        sdlop.video->ShowWindowSystemMenu(sdlop.video, window, x, y);
        return true;
    }
    return SDL_Unsupported();
}

bool SDL_SetWindowHitTest(SDL_Window *window, SDL_HitTest callback, void *callback_data)
{
    CHECK_WINDOW(window, false);
    if (!sdlop.video->SetWindowHitTest) {
        return SDL_Unsupported();
    }
    window->hit_test = callback;
    window->hit_test_data = callback_data;
    return sdlop.video->SetWindowHitTest(sdlop.video, window, callback != NULL);
}

static void SDLCALL cleanup_surface_property(void *userdata, void *value)
{
    (void)userdata;
    SDL_DestroySurface((SDL_Surface *)value);
}

bool SDL_SetWindowShape(SDL_Window *window, SDL_Surface *shape)
{
    CHECK_WINDOW(window, false);
    if (!(window->flags & SDL_WINDOW_TRANSPARENT)) {
        return SDL_SetError("Window must be created with SDL_WINDOW_TRANSPARENT");
    }
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props) {
        return false;
    }
    SDL_Surface *surface = NULL;
    if (shape) {
        surface = SDL_ConvertSurface(shape, SDL_PIXELFORMAT_ARGB32);
        if (!surface) {
            return false;
        }
    }
    if (!SDL_SetPointerPropertyWithCleanup(props, SDL_PROP_WINDOW_SHAPE_POINTER, surface, cleanup_surface_property, NULL)) {
        return false;
    }
    if (sdlop.video->UpdateWindowShape) {
        if (!sdlop.video->UpdateWindowShape(sdlop.video, window, surface)) {
            return false;
        }
    }
    return true;
}

bool SDL_ScreenSaverEnabled(void)
{
    if (!sdlop.video) {
        return true;
    }
    return !sdlop.suspend_screensaver;
}

static bool set_screensaver(bool suspend)
{
    if (!sdlop.video) {
        return SDL_SetError("Video subsystem has not been initialized");
    }
    if (sdlop.suspend_screensaver == suspend) {
        return true;
    }
    sdlop.suspend_screensaver = suspend;
    if (sdlop.video->SuspendScreenSaver) {
        return sdlop.video->SuspendScreenSaver(sdlop.video);
    }
    return SDL_Unsupported();
}

bool SDL_EnableScreenSaver(void)
{
    return set_screensaver(false);
}

bool SDL_DisableScreenSaver(void)
{
    return set_screensaver(true);
}

SDL_SystemTheme SDL_GetSystemTheme(void)
{
    if (sdlop.video && sdlop.video->GetSystemTheme) {
        return sdlop.video->GetSystemTheme(sdlop.video);
    }
    return SDL_SYSTEM_THEME_UNKNOWN;
}
