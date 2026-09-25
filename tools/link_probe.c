/*
  Link-level compatibility probe: a program written against **stock** SDL3's
  headers, linked against SDLop's library.

  The other direction is checked elsewhere - tools/check_api.py proves SDLop's
  headers declare what SDL3's do, tools/abi_probe.c proves the layouts and the
  constants match - but neither of them catches a *symbol* problem: a function
  whose declaration is fine and whose ABI is fine, but which the library does not
  export, or exports under a different name. A program built against real SDL3
  headers and linked against libSDLop.so is the only thing that can.

  So this file is deliberately written the way an application would be, with no
  SDLop-specific calls and no #ifdefs, and it is compiled twice:

      make link-check

  once against the system's SDL3 headers, linked against libSDLop.so, and once
  against SDLop's own headers, linked against libSDLop.a. Both must build, both
  must run, and their output must be identical - which is the promise the whole
  project rests on ("the same API, only faster").

  It exercises a spread of the documented core surface rather than everything:
  the point is that the calls it makes resolve at link time and behave the same,
  not to be another test suite (tests/ are that).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

/* A surface, a blit and a present, on whatever driver the environment picks:
   this is the "draw something" path an application actually uses. */
static void draw(SDL_Window *window)
{
    SDL_Surface *surface = SDL_GetWindowSurface(window);

    if (!surface) {
        printf("  (no window surface: %s)\n", SDL_GetError());
        return;
    }
    SDL_FillSurfaceRect(surface, NULL, SDL_MapSurfaceRGB(surface, 20, 40, 80));
    SDL_UpdateWindowSurface(window);
}

int main(int argc, char *argv[])
{
    SDL_Window *window;
    SDL_Event event;
    SDL_PropertiesID props;
    const char *driver;
    int width = 320, height = 240;
    bool quit = false;

    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("FAIL SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    printf("driver=%s subsystem=%s\n", SDL_GetCurrentVideoDriver(),
           SDL_GetCurrentVideoDriver());

    /* Version and error handling: the two things every application touches. */
    printf("version=%d.%d.%d\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
           SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
    SDL_SetError("probe error %d", 42);
    check(strcmp(SDL_GetError(), "probe error 42") == 0, "SDL_SetError/SDL_GetError round trip");
    SDL_ClearError();
    check(SDL_GetError()[0] == '\0', "SDL_ClearError");

    {
        Uint32 extensions = 0;
        const char *const *names = SDL_Vulkan_GetInstanceExtensions(&extensions);
        printf("drivers=%d vulkan_extensions=%u\n", SDL_GetNumVideoDrivers(),
               names ? extensions : 0u);
    }
    driver = SDL_GetVideoDriver(0);
    check(driver != NULL && SDL_GetNumVideoDrivers() > 0, "SDL_GetVideoDriver(0)");

    window = SDL_CreateWindow("sdlop link probe", width, height,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!window) {
        printf("FAIL SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    check(SDL_GetWindowID(window) != 0, "SDL_GetWindowID");
    check(SDL_GetWindowFromID(SDL_GetWindowID(window)) == window, "SDL_GetWindowFromID");

    /* Properties: the SDL3 way to attach data to an object. */
    props = SDL_GetWindowProperties(window);
    check(props != 0, "SDL_GetWindowProperties");
    check(SDL_SetPointerProperty(props, "probe.pointer", window), "SDL_SetPointerProperty");
    check(SDL_GetPointerProperty(props, "probe.pointer", NULL) == window,
          "SDL_GetPointerProperty");
    check(SDL_SetNumberProperty(props, "probe.number", 7), "SDL_SetNumberProperty");
    check(SDL_GetNumberProperty(props, "probe.number", 0) == 7, "SDL_GetNumberProperty");
    check(SDL_SetBooleanProperty(props, "probe.bool", true), "SDL_SetBooleanProperty");
    check(SDL_GetBooleanProperty(props, "probe.bool", false), "SDL_GetBooleanProperty");

    SDL_ShowWindow(window);
    draw(window);
    SDL_SetWindowTitle(window, "sdlop link probe (renamed)");
    SDL_SetWindowPosition(window, 10, 20);
    SDL_SetWindowSize(window, 200, 150);
    SDL_SyncWindow(window);

    {
        int x = 0, y = 0, w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        SDL_GetWindowPosition(window, &x, &y);
        check(w > 0 && h > 0, "window has a size after SDL_SyncWindow");
        /* A tiling compositor decides the size and ignores the position, so on
           Wayland the numbers are the session's, not the program's - and they can
           differ between two runs of the same binary. The verdict is what is
           compared there; the exact geometry is compared where the program's
           request is what wins (offscreen and X11). */
        if (strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
            printf("geometry=compositor-chosen\n");
        } else {
            printf("geometry=%dx%d+%d+%d\n", w, h, x, y);
        }
    }

    /* Keyboard and mouse queries, event pumping, timers: the parts of the core
       API a game loop reaches for on every frame. */
    printf("keyname=%s scancode=%d keycode=%d\n",
           SDL_GetKeyName(SDLK_A), (int)SDL_GetScancodeFromKey(SDLK_A, NULL),
           (int)SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_NONE, false));
    check(strcmp(SDL_GetKeyName(SDLK_A), "A") == 0, "SDL_GetKeyName");
    check(SDL_GetScancodeFromName("Left Shift") == SDL_SCANCODE_LSHIFT,
          "SDL_GetScancodeFromName(\"Left Shift\")");
    check(SDL_GetScancodeFromKey(SDLK_A, NULL) == SDL_SCANCODE_A, "SDL_GetScancodeFromKey");
    check(SDL_GetKeyFromScancode(SDL_SCANCODE_SPACE, SDL_KMOD_NONE, false) == SDLK_SPACE,
          "SDL_GetKeyFromScancode");
    SDL_PumpEvents();
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            quit = true;
        }
    }
    {
        /* The timer has to advance across a sleep; its absolute value at startup
           is not something an application can depend on (offscreen starts at 0). */
        Uint64 before = SDL_GetTicks();
        SDL_Delay(10);
        Uint64 after = SDL_GetTicks();
        /* Printed as a verdict, not as numbers: the absolute value and the exact
           delta differ between runs, and the gate below compares two runs. */
        printf("pump=%d ticks=%s\n", quit ? 1 : 0,
               (after > before) ? "advancing" : "stuck");
        check(after >= before && after - before < 500, "SDL_Delay/SDL_GetTicks");
        check(SDL_GetTicksNS() > 0, "SDL_GetTicksNS");
    }

    /* The display family: names, modes and the current one. */
    {
        SDL_DisplayID *displays = SDL_GetDisplays(NULL);
        int count = 0;

        if (displays) {
            SDL_free(displays);
            count = SDL_GetDisplays(NULL) ? 1 : 0;
        }
        printf("displays=%d primary=%d\n", count,
               SDL_GetPrimaryDisplay() != 0 ? 1 : 0);
        if (SDL_GetPrimaryDisplay() != 0) {
            const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
            check(mode != NULL && mode->w > 0 && mode->h > 0, "SDL_GetCurrentDisplayMode");
            printf("display=%s %dx%d\n", SDL_GetDisplayName(SDL_GetPrimaryDisplay()),
                   mode->w, mode->h);
        }
    }

    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_Quit();

    printf("link-probe: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
