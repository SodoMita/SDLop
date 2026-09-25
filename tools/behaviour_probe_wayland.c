/*
  SDL3 behaviour probe for a real Wayland compositor.

  This program deliberately does not synthesize input.  It creates one visible
  SDL window, prints a normalized trace, and waits at READY-INPUT while an
  external driver (for example tests/test_virtual_input on sway) sends the same
  key and pointer script to it.  Build and run the same source against SDLop
  and stock SDL3, then diff the traces:

      cmake --build build --target behaviour_probe_wayland
      SDLOP_DISABLE_RAW_INPUT=1 WAYLAND_DISPLAY=wayland-0 \
        ./build/behaviour_probe_wayland --seconds 8 > sdlop-wayland.txt

  The trace intentionally omits timestamps, window IDs, and compositor-specific
  coordinates that cannot be made stable between clients.  It preserves event
  order and the values an application normally consumes: key names/modifiers,
  pointer position and deltas, buttons, wheel direction, text, and window data.

  The input phase begins only after the window has been shown and its initial
  configure events have been pumped.  An injector can wait for READY-INPUT on
  stdout before sending its script.  Set SDLOP_DISABLE_RAW_INPUT=1 for both
  implementations when comparing Wayland seat semantics; otherwise SDLop may
  prefer /dev/input and the trace is no longer a Wayland-vs-Wayland comparison.

  This software is provided 'as-is', without any express or implied warranty
  (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int probe_width = 320;
static int probe_height = 240;
static double input_seconds = 8.0;
static const char *probe_title = "sdlop-wayland-behaviour";
static const char *phase = "lifecycle";
static int relative_mode;

static const struct
{
    SDL_WindowFlags value;
    const char *name;
} window_flags[] = {
    { SDL_WINDOW_FULLSCREEN, "FULLSCREEN" },
    { SDL_WINDOW_OPENGL, "OPENGL" },
    { SDL_WINDOW_HIDDEN, "HIDDEN" },
    { SDL_WINDOW_BORDERLESS, "BORDERLESS" },
    { SDL_WINDOW_RESIZABLE, "RESIZABLE" },
    { SDL_WINDOW_MINIMIZED, "MINIMIZED" },
    { SDL_WINDOW_MAXIMIZED, "MAXIMIZED" },
    { SDL_WINDOW_MOUSE_GRABBED, "MOUSE_GRABBED" },
    { SDL_WINDOW_INPUT_FOCUS, "INPUT_FOCUS" },
    { SDL_WINDOW_MOUSE_FOCUS, "MOUSE_FOCUS" },
    { SDL_WINDOW_MOUSE_CAPTURE, "MOUSE_CAPTURE" },
    { SDL_WINDOW_ALWAYS_ON_TOP, "ALWAYS_ON_TOP" },
    { SDL_WINDOW_HIGH_PIXEL_DENSITY, "HIGH_PIXEL_DENSITY" },
    { SDL_WINDOW_VULKAN, "VULKAN" },
    { SDL_WINDOW_NOT_FOCUSABLE, "NOT_FOCUSABLE" },
};

static void print_flags(SDL_WindowFlags flags)
{
    bool first = true;
    for (size_t i = 0; i < SDL_arraysize(window_flags); ++i) {
        if ((flags & window_flags[i].value) != 0) {
            printf("%s%s", first ? "" : "|", window_flags[i].name);
            first = false;
        }
    }
    if (first) {
        fputs("none", stdout);
    }
}

static const char *event_name(Uint32 type)
{
    switch (type) {
    case SDL_EVENT_QUIT: return "QUIT";
    case SDL_EVENT_KEY_DOWN: return "KEYDOWN";
    case SDL_EVENT_KEY_UP: return "KEYUP";
    case SDL_EVENT_TEXT_EDITING: return "TEXTEDITING";
    case SDL_EVENT_TEXT_INPUT: return "TEXT";
    case SDL_EVENT_KEYMAP_CHANGED: return "KEYMAP_CHANGED";
    case SDL_EVENT_MOUSE_MOTION: return "MOTION";
    case SDL_EVENT_MOUSE_BUTTON_DOWN: return "BUTTONDOWN";
    case SDL_EVENT_MOUSE_BUTTON_UP: return "BUTTONUP";
    case SDL_EVENT_MOUSE_WHEEL: return "WHEEL";
    case SDL_EVENT_FINGER_DOWN: return "FINGERDOWN";
    case SDL_EVENT_FINGER_UP: return "FINGERUP";
    case SDL_EVENT_FINGER_MOTION: return "FINGERMOTION";
    case SDL_EVENT_FINGER_CANCELED: return "FINGERCANCELED";
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return "CLOSE_REQUESTED";
    case SDL_EVENT_WINDOW_DESTROYED: return "DESTROYED";
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: return "DISPLAY_CHANGED";
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: return "DISPLAY_SCALE_CHANGED";
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: return "ENTER_FULLSCREEN";
    case SDL_EVENT_WINDOW_EXPOSED: return "EXPOSED";
    case SDL_EVENT_WINDOW_FOCUS_GAINED: return "FOCUS_GAINED";
    case SDL_EVENT_WINDOW_FOCUS_LOST: return "FOCUS_LOST";
    case SDL_EVENT_WINDOW_HIDDEN: return "HIDDEN";
    case SDL_EVENT_WINDOW_HIT_TEST: return "HIT_TEST";
    case SDL_EVENT_WINDOW_ICCPROF_CHANGED: return "ICCPROF_CHANGED";
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: return "LEAVE_FULLSCREEN";
    case SDL_EVENT_WINDOW_MAXIMIZED: return "MAXIMIZED";
    case SDL_EVENT_WINDOW_METAL_VIEW_RESIZED: return "METAL_VIEW_RESIZED";
    case SDL_EVENT_WINDOW_MINIMIZED: return "MINIMIZED";
    case SDL_EVENT_WINDOW_MOUSE_ENTER: return "MOUSE_ENTER";
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: return "MOUSE_LEAVE";
    case SDL_EVENT_WINDOW_MOVED: return "MOVED";
    case SDL_EVENT_WINDOW_OCCLUDED: return "OCCLUDED";
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: return "PIXEL_SIZE_CHANGED";
    case SDL_EVENT_WINDOW_RESIZED: return "RESIZED";
    case SDL_EVENT_WINDOW_RESTORED: return "RESTORED";
    case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED: return "SAFE_AREA_CHANGED";
    case SDL_EVENT_WINDOW_SHOWN: return "SHOWN";
    case SDL_EVENT_WINDOW_HDR_STATE_CHANGED: return "HDR_STATE_CHANGED";
    default: return NULL;
    }
}

static void append_modifier(char *out, size_t outlen, SDL_Keymod mod,
                            SDL_Keymod bit, const char *name)
{
    size_t used;
    if ((mod & bit) == 0 || outlen == 0) {
        return;
    }
    used = strlen(out);
    if (used != 0 && used + 1 < outlen) {
        out[used++] = '|';
        out[used] = '\0';
    }
    if (used < outlen - 1) {
        snprintf(out + used, outlen - used, "%s", name);
    }
}

static void print_modifiers(SDL_Keymod mod)
{
    char text[96] = "";
    append_modifier(text, sizeof text, mod, SDL_KMOD_LSHIFT, "LSHIFT");
    append_modifier(text, sizeof text, mod, SDL_KMOD_RSHIFT, "RSHIFT");
    append_modifier(text, sizeof text, mod, SDL_KMOD_LCTRL, "LCTRL");
    append_modifier(text, sizeof text, mod, SDL_KMOD_RCTRL, "RCTRL");
    append_modifier(text, sizeof text, mod, SDL_KMOD_LALT, "LALT");
    append_modifier(text, sizeof text, mod, SDL_KMOD_RALT, "RALT");
    append_modifier(text, sizeof text, mod, SDL_KMOD_LGUI, "LGUI");
    append_modifier(text, sizeof text, mod, SDL_KMOD_RGUI, "RGUI");
    append_modifier(text, sizeof text, mod, SDL_KMOD_NUM, "NUM");
    append_modifier(text, sizeof text, mod, SDL_KMOD_CAPS, "CAPS");
    append_modifier(text, sizeof text, mod, SDL_KMOD_MODE, "MODE");
    printf("%s", text[0] ? text : "none");
}

static const char *safe_name(const char *text)
{
    return (text && text[0]) ? text : "?";
}

static void print_event(const SDL_Event *event)
{
    const char *name = event_name(event->type);

    printf("EVENT %s ", phase);
    if (!name) {
        printf("OTHER type=0x%x\n", (unsigned)event->type);
        fflush(stdout);
        return;
    }
    printf("%s", name);

    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        printf(" scancode=%s key=%s mod=",
               safe_name(SDL_GetScancodeName(event->key.scancode)),
               safe_name(SDL_GetKeyName(event->key.key)));
        print_modifiers(event->key.mod);
        printf(" down=%d repeat=%d", event->key.down ? 1 : 0,
               event->key.repeat ? 1 : 0);
        break;
    case SDL_EVENT_TEXT_EDITING:
        printf(" text='%s' start=%d length=%d",
               event->edit.text ? event->edit.text : "",
               event->edit.start, event->edit.length);
        break;
    case SDL_EVENT_TEXT_INPUT:
        printf(" text='%s'", event->text.text ? event->text.text : "");
        break;
    case SDL_EVENT_MOUSE_MOTION:
        printf(" x=%.1f y=%.1f xrel=%.1f yrel=%.1f",
               event->motion.x, event->motion.y,
               event->motion.xrel, event->motion.yrel);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        printf(" button=%u clicks=%u x=%.1f y=%.1f",
               (unsigned)event->button.button,
               (unsigned)event->button.clicks,
               event->button.x, event->button.y);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        printf(" x=%.1f y=%.1f flipped=%d",
               event->wheel.x, event->wheel.y,
               event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? 1 : 0);
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
        printf(" x=%.3f y=%.3f dx=%.3f dy=%.3f pressure=%.3f",
               event->tfinger.x, event->tfinger.y,
               event->tfinger.dx, event->tfinger.dy,
               event->tfinger.pressure);
        break;
    case SDL_EVENT_QUIT:
        fputs(" quit", stdout);
        break;
    default:
        if (event->type >= SDL_EVENT_WINDOW_FIRST &&
            event->type <= SDL_EVENT_WINDOW_LAST) {
            printf(" data1=%d data2=%d", event->window.data1,
                   event->window.data2);
        }
        break;
    }
    putchar('\n');
    fflush(stdout);
}

static void drain_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        print_event(&event);
    }
}

static void pump_for(Uint32 milliseconds)
{
    Uint64 end = SDL_GetTicks() + milliseconds;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        drain_events();
        SDL_Delay(5);
    }
}

/* Get + fill + update in one step. SDL3's SDL_UpdateWindowSurface() refuses
   to run without a current SDL_GetWindowSurface() (and invalidates the
   surface on resize), while SDLop maps during the configure handshake; doing
   all three here is what makes stock SDL3 actually commit a buffer and map
   the window, so both implementations reach the input phase comparable. */
static void draw_window(SDL_Window *window)
{
    SDL_Surface *surface = SDL_GetWindowSurface(window);
    if (!surface) {
        fprintf(stderr, "draw: GetWindowSurface: %s\n", SDL_GetError());
        return;
    }
    SDL_FillSurfaceRect(surface, NULL, 0xFF006E);
    if (!SDL_UpdateWindowSurface(window)) {
        fprintf(stderr, "draw: UpdateWindowSurface: %s\n", SDL_GetError());
    }
}

static int parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            probe_width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            probe_height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            input_seconds = atof(argv[++i]);
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            probe_title = argv[++i];
        } else if (strcmp(argv[i], "--relative") == 0) {
            relative_mode = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [--width W] [--height H] [--seconds S] "
                   "[--title TITLE] [--relative]\n", argv[0]);
            return 1;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    if (probe_width <= 0 || probe_height <= 0 || input_seconds < 0.0) {
        fprintf(stderr, "invalid probe dimensions or duration\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    SDL_Window *window;
    SDL_Event event;
    Uint64 deadline;
    int parse_result = parse_args(argc, argv);

    if (parse_result != 0) {
        return parse_result < 0 ? 2 : 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "FATAL init: %s\n", SDL_GetError());
        return 1;
    }

    printf("PROBE driver=%s\n", safe_name(SDL_GetCurrentVideoDriver()));
    window = SDL_CreateWindow(probe_title, probe_width, probe_height,
                              SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "FATAL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    printf("WINDOW created\n");
    printf("STATE flags=");
    print_flags(SDL_GetWindowFlags(window));
    putchar('\n');

    {
        int width = 0;
        int height = 0;
        if (SDL_GetWindowSize(window, &width, &height)) {
            printf("STATE size=%dx%d display-scale=%.2f\n", width, height,
                   SDL_GetWindowDisplayScale(window));
        }
    }

    /* Explicitly show the window, then let Wayland deliver configure/focus
       events before the external injector starts. */
    SDL_ShowWindow(window);
    pump_for(250);
    /* Draw once so the surface actually maps: stock SDL3 only commits a
       buffer from the render/update path, so an undrawn window never maps
       on Wayland (no view, no focus, no input - the stock trace comes back
       with device-added events only). SDLop maps in its configure handler;
       this call is what makes the two implementations comparable. */
    draw_window(window);
    pump_for(120);

    /* A resize is supported by both SDLop's and stock SDL3's Wayland driver;
       positioning is intentionally omitted because Wayland has no global
       window-position API. */
    if (SDL_SetWindowSize(window, probe_width + 80, probe_height + 60)) {
        pump_for(180);
        draw_window(window);
    }

    /* Hide/show gives the lifecycle trace a deterministic operation without
       depending on compositor-specific maximize/fullscreen policy. */
    SDL_HideWindow(window);
    pump_for(120);
    SDL_ShowWindow(window);
    pump_for(180);
    draw_window(window);
    pump_for(120);

    if (relative_mode) {
        if (!SDL_SetWindowRelativeMouseMode(window, true)) {
            printf("RELATIVE unsupported='%s'\n", SDL_GetError());
        } else {
            printf("RELATIVE enabled\n");
        }
    }

    phase = "input";
    printf("READY-INPUT\n");
    fflush(stdout);

    deadline = SDL_GetTicks() + (Uint64)(input_seconds * 1000.0);
    while (SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        while (SDL_PollEvent(&event)) {
            print_event(&event);
            if (event.type == SDL_EVENT_QUIT) {
                deadline = 0;
                break;
            }
        }
        SDL_Delay(5);
    }

    if (relative_mode && SDL_GetWindowRelativeMouseMode(window)) {
        SDL_SetWindowRelativeMouseMode(window, false);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    puts("DONE");
    return 0;
}
