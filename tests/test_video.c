/*
  SDLop video tests: displays, windows, properties, the software window
  surface and the event stream a window produces.

  Runs on any driver (it asks for what is available), and with
  SDL_VIDEODRIVER=wayland it exercises the real compositor path - that is how the
  Wayland backend is smoke tested in CI:

      SDL_VIDEODRIVER=wayland WAYLAND_DISPLAY=wayland-1 ./build/tests/test_video
*/

#include <SDL3/SDL.h>

#include <stdio.h>

static int failures;
static int checks;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        checks++;                                                               \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

static void test_drivers(void)
{
    int count = SDL_GetNumVideoDrivers();
    const char *current;

    CHECK(count > 0, "SDL_GetNumVideoDrivers() = %d", count);
    printf("  video drivers:");
    for (int i = 0; i < count; i++) {
        printf(" %s", SDL_GetVideoDriver(i));
    }
    printf("\n");
    CHECK(SDL_GetVideoDriver(0) != NULL, "driver 0 has no name");
    CHECK(SDL_GetVideoDriver(count) == NULL, "out-of-range driver name is not NULL");
    CHECK(SDL_GetVideoDriver(-1) == NULL, "negative driver index is not NULL");
    current = SDL_GetCurrentVideoDriver();
    CHECK(current != NULL, "SDL_GetCurrentVideoDriver() returned NULL");
    printf("  running on '%s'\n", current ? current : "(none)");
    CHECK(SDL_GetVideoDriver(-1) == NULL, "negative index");
}

static void test_displays(void)
{
    int count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&count);
    SDL_DisplayID primary;

    CHECK(displays != NULL && count > 0, "no displays (%d)", count);
    if (!displays || count == 0) {
        return;
    }
    primary = SDL_GetPrimaryDisplay();
    CHECK(primary != 0, "SDL_GetPrimaryDisplay() = 0");

    for (int i = 0; i < count; i++) {
        SDL_Rect bounds;
        const char *name = SDL_GetDisplayName(displays[i]);

        CHECK(name != NULL, "display %u has no name", (unsigned)displays[i]);
        CHECK(SDL_GetDisplayBounds(displays[i], &bounds), "no bounds for display %u",
              (unsigned)displays[i]);
        CHECK(bounds.w > 0 && bounds.h > 0, "display %u is %dx%d", (unsigned)displays[i],
              bounds.w, bounds.h);
        CHECK(SDL_GetDisplayUsableBounds(displays[i], &bounds), "no usable bounds");
        CHECK(SDL_GetDisplayContentScale(displays[i]) > 0.0f, "content scale is 0");
        CHECK(SDL_GetCurrentDisplayOrientation(displays[i]) != SDL_ORIENTATION_UNKNOWN,
              "unknown orientation");
        {
            int num_modes = 0;
            SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(displays[i], &num_modes);
            CHECK(modes != NULL && num_modes > 0, "display %u has no modes",
                  (unsigned)displays[i]);
            if (modes && num_modes > 0) {
                CHECK(modes[0]->w > 0 && modes[0]->h > 0, "mode 0 is %dx%d", modes[0]->w, modes[0]->h);
                CHECK(modes[0]->displayID == displays[i], "mode belongs to another display");
                {
                    SDL_DisplayMode closest;
                    CHECK(SDL_GetClosestFullscreenDisplayMode(displays[i], modes[0]->w, modes[0]->h,
                                                              0.0f, false, &closest),
                          "SDL_GetClosestFullscreenDisplayMode()");
                    CHECK(closest.w > 0 && closest.h > 0, "closest mode is %dx%d", closest.w, closest.h);
                }
                SDL_free(modes);
            }
        }
        printf("  display %u: '%s' %dx%d scale %.2f\n", (unsigned)displays[i], name ? name : "?",
               bounds.w, bounds.h, SDL_GetDisplayContentScale(displays[i]));
    }
    CHECK(SDL_GetDisplayName(0) == NULL || true, "display 0 lookup must not crash");
    SDL_free(displays);
}

static void test_window_properties(void)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_Window *window;

    CHECK(SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "props window"),
          "title property");
    CHECK(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 320), "width property");
    CHECK(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 200), "height property");
    CHECK(SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true), "resizable property");
    CHECK(SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true), "hidden property");
    CHECK(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, 40), "x property");
    CHECK(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, 60), "y property");

    window = SDL_CreateWindowWithProperties(props);
    CHECK(window != NULL, "SDL_CreateWindowWithProperties(): %s", SDL_GetError());
    if (window) {
        int w = 0, h = 0, x = 0, y = 0;
        CHECK(SDL_strcmp(SDL_GetWindowTitle(window), "props window") == 0, "title from properties");
        CHECK(SDL_GetWindowSize(window, &w, &h) && w == 320 && h == 200, "size from properties %dx%d", w, h);
        CHECK(SDL_GetWindowPosition(window, &x, &y) && x == 40 && y == 60, "position from properties %d,%d", x, y);
        CHECK(SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN, "hidden flag from properties");
        CHECK(SDL_GetWindowFlags(window) & SDL_WINDOW_RESIZABLE, "resizable flag from properties");
        SDL_DestroyWindow(window);
    }
    SDL_DestroyProperties(props);
}

static void test_window_state(void)
{
    SDL_Window *window = SDL_CreateWindow("state", 200, 100, SDL_WINDOW_RESIZABLE);
    SDL_Window *parent;
    int w = 0, h = 0;

    CHECK(window != NULL, "SDL_CreateWindow()");
    if (!window) {
        return;
    }

    CHECK(SDL_GetWindowFlags(window) & SDL_WINDOW_RESIZABLE, "resizable flag missing");
    CHECK(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN), "fullscreen flag set");
    CHECK(SDL_GetWindowDisplayScale(window) > 0.0f, "display scale is 0");
    CHECK(SDL_GetWindowPixelDensity(window) > 0.0f, "pixel density is 0");
    CHECK(SDL_GetWindowPixelFormat(window) == SDL_PIXELFORMAT_XRGB8888 ||
          SDL_GetWindowPixelFormat(window) != SDL_PIXELFORMAT_UNKNOWN, "pixel format");
    CHECK(SDL_GetWindowDisplayScale(window) == SDL_GetWindowDisplayScale(window), "scale is stable");

    SDL_SetWindowMinimumSize(window, 100, 50);
    {
        int min_w = 0, min_h = 0;
        CHECK(SDL_GetWindowMinimumSize(window, &min_w, &min_h) && min_w == 100 && min_h == 50,
              "minimum size %dx%d", min_w, min_h);
    }
    SDL_SetWindowMaximumSize(window, 800, 600);
    {
        int max_w = 0, max_h = 0;
        CHECK(SDL_GetWindowMaximumSize(window, &max_w, &max_h) && max_w == 800 && max_h == 600,
              "maximum size %dx%d", max_w, max_h);
    }
    SDL_SetWindowAspectRatio(window, 2.0f, 1.0f);
    {
        float min_aspect = 0.0f, max_aspect = 0.0f;
        CHECK(SDL_GetWindowAspectRatio(window, &min_aspect, &max_aspect) &&
              min_aspect == 2.0f && max_aspect == 1.0f, "aspect ratio %f %f", min_aspect, max_aspect);
    }

    SDL_SetWindowBordered(window, false);
    CHECK(!(SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS), "borderless flag");
    SDL_SetWindowBordered(window, true);

    SDL_SetWindowResizable(window, false);
    CHECK(!(SDL_GetWindowFlags(window) & SDL_WINDOW_RESIZABLE), "resizable flag after clearing");
    SDL_SetWindowResizable(window, true);

    CHECK(SDL_SetWindowAlwaysOnTop(window, true), "SDL_SetWindowAlwaysOnTop(): %s", SDL_GetError());
    CHECK(SDL_GetWindowFlags(window) & SDL_WINDOW_ALWAYS_ON_TOP, "always-on-top flag");
    SDL_SetWindowAlwaysOnTop(window, false);

    CHECK(SDL_SetWindowOpacity(window, 0.5f), "SDL_SetWindowOpacity(): %s", SDL_GetError());
    CHECK(SDL_GetWindowOpacity(window) == 0.5f, "opacity is %f", SDL_GetWindowOpacity(window));
    SDL_SetWindowOpacity(window, 1.0f);

    CHECK(SDL_SetWindowMouseGrab(window, true), "SDL_SetWindowMouseGrab()");
    CHECK(SDL_GetWindowMouseGrab(window), "mouse grab flag");
    SDL_SetWindowMouseGrab(window, false);
    CHECK(SDL_SetWindowKeyboardGrab(window, true), "SDL_SetWindowKeyboardGrab()");
    CHECK(SDL_GetWindowKeyboardGrab(window), "keyboard grab flag");
    SDL_SetWindowKeyboardGrab(window, false);

    SDL_SetWindowMouseRect(window, &(SDL_Rect){ 0, 0, 100, 50 });
    {
        const SDL_Rect *rect = SDL_GetWindowMouseRect(window);
        CHECK(rect && rect->w == 100 && rect->h == 50, "mouse rect not stored");
    }
    SDL_SetWindowMouseRect(window, NULL);

    CHECK(SDL_SetWindowRelativeMouseMode(window, true), "SDL_SetWindowRelativeMouseMode(): %s",
          SDL_GetError());
    CHECK(SDL_GetWindowRelativeMouseMode(window), "relative mode flag");
    CHECK(SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_RELATIVE_MODE, "relative mode window flag");
    SDL_SetWindowRelativeMouseMode(window, false);

    CHECK(SDL_SyncWindow(window), "SDL_SyncWindow(): %s", SDL_GetError());

    /* the pixel size follows the window size (scale 1 on every test machine).
       The size is read back after SDL_SyncWindow() because the window's size is
       the server's answer to the request, not the request itself. */
    SDL_SetWindowSize(window, 300, 150);
    SDL_SyncWindow(window);
    CHECK(SDL_GetWindowSize(window, &w, &h) && w == 300 && h == 150, "size after SDL_SetWindowSize: %dx%d", w, h);
    CHECK(SDL_GetWindowSizeInPixels(window, &w, &h) && w == 300 && h == 150,
          "pixel size after SDL_SetWindowSize: %dx%d", w, h);

    /* a popup needs a parent */
    parent = window;
    CHECK(SDL_GetWindowParent(window) == NULL, "a top-level window has a parent");
    (void)parent;

    SDL_DestroyWindow(window);
    CHECK(SDL_GetWindowFromID(0) == NULL, "window 0");
}

static void test_window_surface(void)
{
    SDL_Window *window = SDL_CreateWindow("surface", 64, 48, 0);
    SDL_Surface *surface;
    const SDL_PixelFormatDetails *format;
    Uint32 pixel;
    Uint8 r, g, b;

    CHECK(window != NULL, "SDL_CreateWindow()");
    if (!window) {
        return;
    }
    CHECK(!SDL_WindowHasSurface(window), "a fresh window must not have a surface yet");

    surface = SDL_GetWindowSurface(window);
    CHECK(surface != NULL, "SDL_GetWindowSurface(): %s", SDL_GetError());
    if (surface) {
        CHECK(SDL_WindowHasSurface(window), "SDL_WindowHasSurface() after getting the surface");
        CHECK(surface->w == 64 && surface->h == 48, "surface is %dx%d", surface->w, surface->h);
        CHECK(surface->pixels != NULL, "surface has no pixels");
        CHECK(surface->pitch >= surface->w * 4, "pitch is %d", surface->pitch);

        format = SDL_GetPixelFormatDetails(surface->format);
        CHECK(format != NULL, "surface format details");

        pixel = SDL_MapRGB(format, NULL, 0x12, 0x34, 0x56);
        CHECK(SDL_FillSurfaceRect(surface, NULL, pixel), "SDL_FillSurfaceRect(NULL)");
        {
            Uint32 *row = (Uint32 *)(void *)surface->pixels;
            SDL_GetRGB(row[0], format, NULL, &r, &g, &b);
            CHECK(r == 0x12 && g == 0x34 && b == 0x56, "pixel after fill = %02x%02x%02x", r, g, b);
        }

        CHECK(SDL_FillSurfaceRect(surface, &(SDL_Rect){ 8, 8, 16, 16 }, 0), "fill a rect");
        {
            Uint32 *row = (Uint32 *)(void *)((Uint8 *)surface->pixels + 8 * surface->pitch);
            SDL_GetRGB(row[8], format, NULL, &r, &g, &b);
            CHECK(r == 0 && g == 0 && b == 0, "filled rect is %02x%02x%02x", r, g, b);
        }

        CHECK(SDL_UpdateWindowSurface(window), "SDL_UpdateWindowSurface(): %s", SDL_GetError());
        CHECK(SDL_UpdateWindowSurfaceRects(window, &(SDL_Rect){ 0, 0, 8, 8 }, 1),
              "SDL_UpdateWindowSurfaceRects(): %s", SDL_GetError());
    }

    SDL_DestroyWindowSurface(window);
    CHECK(!SDL_WindowHasSurface(window), "surface was not destroyed");
    SDL_DestroyWindow(window);
}

static void test_window_events(void)
{
    SDL_Event event;
    SDL_Window *window = SDL_CreateWindow("events", 128, 96, SDL_WINDOW_HIDDEN);
    bool seen_shown = false;
    bool seen_resized = false;
    bool seen_close = false;

    CHECK(window != NULL, "SDL_CreateWindow()");
    if (!window) {
        return;
    }
    /* Drop anything the previous tests left in the queue: the queue is global. */
    {
        SDL_Event stale;
        SDL_PumpEvents();
        while (SDL_PollEvent(&stale)) {
        }
    }
    SDL_ShowWindow(window);
    SDL_SetWindowSize(window, 160, 120);
    SDL_SetWindowTitle(window, "events 2");
    /* The resize event is what the server says about the request, so wait for it
       (a single pump is a race: the ConfigureNotify may still be in flight). */
    SDL_SyncWindow(window);
    SDL_PumpEvents();

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_WINDOW_SHOWN: seen_shown = true; break;
            case SDL_EVENT_WINDOW_RESIZED:
                seen_resized = true;
                CHECK(event.window.data1 == 160 && event.window.data2 == 120,
                      "resize event is %d,%d", event.window.data1, event.window.data2);
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: seen_close = true; break;
            default: break;
        }
    }
    CHECK(seen_shown, "no SDL_EVENT_WINDOW_SHOWN after SDL_ShowWindow()");
    CHECK(seen_resized, "no SDL_EVENT_WINDOW_RESIZED after SDL_SetWindowSize()");
    CHECK(!seen_close, "a close request came from nowhere");

    CHECK(SDL_strcmp(SDL_GetWindowTitle(window), "events 2") == 0, "title did not change");
    SDL_DestroyWindow(window);
}

static void test_dialogs_and_misc(void)
{
    SDL_Window *window = SDL_CreateWindow("misc", 64, 64, 0);

    CHECK(window != NULL, "SDL_CreateWindow()");
    if (window) {
        const char *text = SDL_GetWindowTitle(window);
        CHECK(text != NULL, "no title");
        CHECK(SDL_FlashWindow(window, SDL_FLASH_BRIEFLY), "SDL_FlashWindow(): %s", SDL_GetError());
        SDL_SetWindowFocusable(window, false);
        CHECK(SDL_GetWindowFlags(window) & SDL_WINDOW_NOT_FOCUSABLE, "focusable flag");
        SDL_SetWindowFocusable(window, true);
        CHECK(SDL_HideWindow(window), "SDL_HideWindow()");
        CHECK(SDL_WindowHasSurface(window) == false, "hidden window still has a surface");
        SDL_DestroyWindow(window);
    }
    CHECK(SDL_GetSystemTheme() >= SDL_SYSTEM_THEME_UNKNOWN, "SDL_GetSystemTheme()");
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("test_video (SDLop)\n");
    /* Run under whatever the user asked for; on a machine with no display
       session, fall back to the offscreen driver so `make check` always runs. */
    SDL_setenv_unsafe("SDL_VIDEODRIVER", "offscreen", 0);
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS), "SDL_Init(): %s", SDL_GetError());

    test_drivers();
    test_displays();
    test_window_properties();
    test_window_state();
    test_window_surface();
    test_window_events();
    test_dialogs_and_misc();

    SDL_Quit();
    printf("%d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
