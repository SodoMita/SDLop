/*
  SDLop - video / window management core.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include <strings.h>

static SDLop_VideoDevice *const video_drivers[] = {
#if SDLop_VIDEO_WAYLAND
    &SDLop_wayland_device,
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
    if (sdlop.video && sdlop.video->SetWindowPosition) {
        return sdlop.video->SetWindowPosition(sdlop.video, window);
    }
    return true;
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
