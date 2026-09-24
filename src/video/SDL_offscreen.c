/*
  SDLop -- the "offscreen" video driver.

  Windows exist (with properties, IDs, focus tracking, surfaces and the whole
  core API) but nothing is displayed: no compositor, no X server, no GPU. Its two
  jobs are running the test suite and CI (there is no display in a container) and
  being the fallback that keeps SDL_GetWindowSurface() working for tools that only
  need a canvas.

  Using it is explicit: SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen") or the
  SDL_VIDEODRIVER environment variable.
*/

#include "../sdlop_internal.h"

#include <string.h>

static bool sdlop_offscreen_init(void)
{
    struct SDLOP_Display *display = SDLOP_AddDisplay(1);

    if (!display) {
        return false;
    }
    SDLOP_SetDisplayName(display, "Offscreen");
    SDLOP_SetDisplayBounds(display, &(SDL_Rect){ 0, 0, 1920, 1080 });
    display->usable = display->bounds;
    SDLOP_SetDisplayScale(display, 1.0f);
    {
        SDL_DisplayMode mode;
        memset(&mode, 0, sizeof(mode));
        mode.displayID = display->id;
        mode.format = SDL_PIXELFORMAT_XRGB8888;
        mode.w = 1920;
        mode.h = 1080;
        mode.pixel_density = 1.0f;
        mode.refresh_rate = 60.0f;
        mode.refresh_rate_numerator = 60000;
        mode.refresh_rate_denominator = 1000;
        SDLOP_AddDisplayMode(display, &mode);
        SDLOP_SetDisplayCurrentMode(display, &mode);
    }
    return true;
}

static void sdlop_offscreen_quit(void)
{
}

static bool sdlop_offscreen_create_window(SDL_Window *window)
{
    /* the window's pixel size equals its logical size at scale 1 */
    SDLOP_OnWindowPixelSizeChanged(window, window->w, window->h);
    /* There is no compositor to hand focus around, so the first window behaves
       like a focused window: input records then have somewhere to go. */
    if (!SDLOP_GetKeyboardFocusWindow()) {
        SDLOP_OnWindowFocusGained(window);
    }
    if (!SDLOP_GetMouseFocusWindow()) {
        SDLOP_SetMouseFocusWindow(window);
    }
    return true;
}

static bool sdlop_offscreen_present(SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    /* Nothing to present to: the pixels stay in the surface, which is exactly
       what an offscreen consumer (a test, a screenshot tool) wants. */
    (void)window;
    (void)rects;
    (void)numrects;
    return true;
}

static bool sdlop_offscreen_create_vulkan_surface(SDL_Window *window, VkInstance instance,
                                                  const struct VkAllocationCallbacks *allocator,
                                                  VkSurfaceKHR *surface)
{
    (void)window; (void)instance; (void)allocator; (void)surface;
    return SDL_SetError("Vulkan is not available on the offscreen driver");
}

static const char *const *sdlop_offscreen_vulkan_extensions(Uint32 *count)
{
    if (count) {
        *count = 0;
    }
    return NULL;
}

const SDLOP_VideoDriver SDLOP_OffscreenVideoDriver = {
    .name = "offscreen",
    .init = sdlop_offscreen_init,
    .quit = sdlop_offscreen_quit,
    .create_window = sdlop_offscreen_create_window,
    .present_surface = sdlop_offscreen_present,
    .create_vulkan_surface = sdlop_offscreen_create_vulkan_surface,
    .get_vulkan_instance_extensions = sdlop_offscreen_vulkan_extensions,
    .get_event_fd = NULL,          /* SDL_WaitEvent() waits on the event condvar */
};
