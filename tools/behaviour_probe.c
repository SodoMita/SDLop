/*
  Behaviour probe: one SDL3 program, compiled against both libraries, driven by
  the same external input, printing a normalized event trace.

      make behaviour-check                       # SDLop vs the system SDL3

  The trace is the deliverable. Whoever is diffing two SDL3-compatible libraries
  (SDLop and, say, a sibling implementation) runs this against each of them with
  the *same* script:

      gcc -Iinclude tools/behaviour_probe.c -o probe_mine build/libSDLop.a <libs>
      gcc $(pkg-config --cflags --libs sdl3) tools/behaviour_probe.c -o probe_stock
      ./probe_mine  --width 320 --height 240 > mine.txt      &
      ./probe_stock --width 320 --height 240 > stock.txt     &
      # then drive both with tests/behaviour_input.sh (or xdotool by hand)

  What is normalized, on purpose:

    * window ids are printed as W - they are allocated by the server
    * timestamps are not printed at all
    * modifiers are printed by name (SHIFT, CTRL, ...), not as bit values
    * window flags are decoded to names, so a difference is a difference in
      meaning rather than in a bit that means nothing

  What is NOT normalized: the order and the count of events. That is the point of
  the probe - the sequence of SDL_EVENT_* an application sees for a given script
  is the part of the contract a test suite is least likely to notice and the part
  two independent implementations most often disagree about.

  The probe drives its own window operations (resize, move, hide, show) before
  the input phase, so those events have a deterministic position in the trace and
  no window manager is needed. It prints READY-INPUT when it is waiting for the
  driver, and DONE when the run is over.
*/
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_width = 320, g_height = 240;
static float g_seconds = 3.0f;
static const char *g_title = "sdlop-behaviour";
static const char *g_phase = "phase1";

static const struct { SDL_WindowFlags flag; const char *name; } g_flags[] = {
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
    for (size_t i = 0; i < SDL_arraysize(g_flags); i++) {
        if (flags & g_flags[i].flag) {
            printf("%s%s", first ? "" : "|", g_flags[i].name);
            first = false;
        }
    }
    if (first) {
        printf("none");
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
    /* every window event the SDL3 3.2.10 headers declare, so a trace names
       what it shows instead of printing a number */
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

static void mods(char *out, size_t outlen, SDL_Keymod mod)
{
    out[0] = '\0';
#define M(f, n) if (mod & (f)) { if (out[0]) strncat(out, "|", outlen - strlen(out) - 1); strncat(out, n, outlen - strlen(out) - 1); }
    M(SDL_KMOD_LSHIFT, "LSHIFT") M(SDL_KMOD_RSHIFT, "RSHIFT")
    M(SDL_KMOD_LCTRL, "LCTRL") M(SDL_KMOD_RCTRL, "RCTRL")
    M(SDL_KMOD_LALT, "LALT") M(SDL_KMOD_RALT, "RALT")
    M(SDL_KMOD_LGUI, "LGUI") M(SDL_KMOD_RGUI, "RGUI")
    M(SDL_KMOD_NUM, "NUM") M(SDL_KMOD_CAPS, "CAPS") M(SDL_KMOD_MODE, "MODE")
#undef M
    if (!out[0]) {
        strncat(out, "none", outlen - 1);
    }
}

static void print_event(const SDL_Event *e)
{
    const char *name = event_name(e->type);
    char m[64];
    const char *sc, *kn;

    printf("EVENT %s ", g_phase);
    if (!name) {
        printf("OTHER type=0x%x\n", (unsigned)e->type);
        return;
    }
    printf("%s", name);

    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        mods(m, sizeof m, e->key.mod);
        sc = SDL_GetScancodeName(e->key.scancode);
        kn = SDL_GetKeyName(e->key.key);
        printf(" scancode=%s", (sc && sc[0]) ? sc : "?");
        printf(" key=%s", (kn && kn[0]) ? kn : "?");
        printf(" mod=%s down=%d repeat=%d", m, (int)e->key.down, (int)e->key.repeat);
        break;
    case SDL_EVENT_TEXT_INPUT:
        printf(" text='%s'", e->text.text);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        printf(" x=%.1f y=%.1f xrel=%.1f yrel=%.1f", e->motion.x, e->motion.y,
               e->motion.xrel, e->motion.yrel);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        printf(" button=%u clicks=%u x=%.1f y=%.1f", (unsigned)e->button.button,
               (unsigned)e->button.clicks, e->button.x, e->button.y);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        printf(" x=%.1f y=%.1f flipped=%d", e->wheel.x, e->wheel.y,
               (int)(e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED));
        break;
    case SDL_EVENT_QUIT:
        printf(" quit");
        break;
    default:
        if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST) {
            printf(" data1=%d data2=%d", e->window.data1, e->window.data2);
        }
        break;
    }
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    SDL_Window *window;
    SDL_Event e;
    Uint64 start;
    int i;

    for (i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            g_width = atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            g_height = atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            g_seconds = (float)atof(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            g_title = argv[++i];
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("FATAL init: %s\n", SDL_GetError());
        return 1;
    }
    printf("PROBE driver=%s version=%d\n", SDL_GetCurrentVideoDriver(), SDL_GetVersion());
    {
        int count = 0;
        SDL_DisplayID *displays = SDL_GetDisplays(&count);
        printf("DISPLAYS count=%d\n", count);
        SDL_free(displays);
    }

    /* The name tables are behaviour too: an application prints them, compares
       them and feeds them back through SDL_GetKeyFromName(), so a difference
       here is a difference an application sees. This block is compared by the
       trace diff like everything else. */
    {
        static const SDL_Keycode keys[] = {
            SDLK_A, SDLK_Z, SDLK_1, SDLK_SPACE, SDLK_RETURN, SDLK_ESCAPE, SDLK_TAB,
            SDLK_BACKSPACE, SDLK_DELETE, SDLK_LEFT, SDLK_F12, SDLK_LSHIFT, SDLK_CAPSLOCK,
            SDLK_EXCLAIM, SDLK_PLUSMINUS, SDLK_NUMLOCKCLEAR, SDLK_KP_1, SDLK_KP_LEFTPAREN,
            SDLK_MEDIA_PLAY, SDLK_LEFT_TAB, SDLK_LMETA, SDLK_CALL, SDLK_UNDO,
            (SDL_Keycode)0x80, (SDL_Keycode)0x100, (SDL_Keycode)0x0
        };
        /* Names whose answer cannot depend on the keyboard layout: letters,
           digits, keys that have names of their own. These are compared against
           stock SDL3 exactly. */
        static const char *names[] = { "A", "a", "Z", "z", "Space", "space", "Left Shift",
                                       "Return", "F1", "Keypad 1", "MediaPlay", "LeftTab",
                                       "CapsLock", "bogus", "" };
        /* Symbols whose answer *does* depend on the layout: stock SDL3 looks the
           character up in the active keymap and answers with the key that types
           it there (on a German layout "?" is shift plus the key whose unshifted
           keycode is 'ß', so stock answers 0xdf), and answers with the character
           itself when no key types it without a modifier. SDLop's name tables are
           the US ones on every layout, so these differ on any layout that is not
           US - reported, not compared. */
        static const char *symbols[] = { "!", "@", "_", "+", "{", "~", "?", "<" };

        printf("NAMES");
        for (size_t i = 0; i < SDL_arraysize(keys); i++) {
            printf(" [%s]", SDL_GetKeyName(keys[i]));
        }
        printf("\n");
        printf("SCANNAMES [%s] [%s] [%s] [%s] [%s]\n", SDL_GetScancodeName(SDL_SCANCODE_A),
               SDL_GetScancodeName(SDL_SCANCODE_BACKSLASH), SDL_GetScancodeName(SDL_SCANCODE_UNKNOWN),
               SDL_GetScancodeName(SDL_SCANCODE_KP_LEFTPAREN), SDL_GetScancodeName(SDL_SCANCODE_MEDIA_PLAY));
        printf("FROMNAMES");
        for (size_t i = 0; i < SDL_arraysize(names); i++) {
            printf(" [%s=0x%x]", names[i], (unsigned)SDL_GetKeyFromName(names[i]));
        }
        printf("\n");
        printf("FROMNAMES-SYMBOLS");
        for (size_t i = 0; i < SDL_arraysize(symbols); i++) {
            printf(" [%s=0x%x]", symbols[i], (unsigned)SDL_GetKeyFromName(symbols[i]));
        }
        printf("\n");
    }

    window = SDL_CreateWindow(g_title, g_width, g_height, SDL_WINDOW_RESIZABLE);
    if (!window) {
        printf("FATAL window: %s\n", SDL_GetError());
        return 1;
    }
    printf("WINDOW created\n");
    SDL_ShowWindow(window);
    printf("STATE flags=");
    print_flags(SDL_GetWindowFlags(window));
    printf("\n");
    {
        int w = 0, h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(window, &w, &h);
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        printf("STATE size=%dx%d pixels=%dx%d display-scale=%.2f\n", w, h, pw, ph,
               SDL_GetWindowDisplayScale(window));
    }

    /* ---- phase 1: the window operations this program drives itself ---------- */
    SDL_PumpEvents();                       /* let the creation events settle */
    while (SDL_PollEvent(&e)) { print_event(&e); }

    SDL_SetWindowSize(window, g_width + 80, g_height + 60);
    SDL_SetWindowPosition(window, 120, 96);
    SDL_Delay(120);
    while (SDL_PollEvent(&e)) { print_event(&e); }

    SDL_HideWindow(window);
    SDL_Delay(120);
    while (SDL_PollEvent(&e)) { print_event(&e); }

    SDL_ShowWindow(window);
    SDL_Delay(120);
    while (SDL_PollEvent(&e)) { print_event(&e); }

    /* ---- phase 2: the driver sends keys and pointer input ------------------- */
    g_phase = "input";
    printf("READY-INPUT\n");
    fflush(stdout);

    start = SDL_GetTicks();
    while (SDL_GetTicks() - start < (Uint64)(g_seconds * 1000.0f)) {
        while (SDL_PollEvent(&e)) {
            print_event(&e);
            if (e.type == SDL_EVENT_QUIT) {
                goto done;
            }
        }
        SDL_Delay(5);
    }

done:
    g_phase = "end";
    SDL_DestroyWindow(window);
    printf("DONE\n");
    SDL_Quit();
    return 0;
}
