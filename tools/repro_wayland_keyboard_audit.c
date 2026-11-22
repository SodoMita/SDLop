/* Isolated actual Wayland keyboard listener callbacks, no display server.
 * Compile with generated protocol headers, function/data sections and linker
 * garbage collection (see docs/input-followup-audit.md). No production hooks.
 * Exit 1 = selected gap reproduced, 0 = not seen, assert = setup failed.
 * zlib license, see LICENSE. */
#include "../src/video/wayland/sdlop_wayland.c"
#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 2) { return 2; }
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("XKB_DEFAULT_LAYOUT", "us", 1);
    assert(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *window = SDL_CreateWindow("keyboard audit", 320, 200, 0);
    assert(window && SDLOP_KeyboardSetDefaultKeymap());
    SDL_FlushEvents(0, SDL_EVENT_LAST);
    bool reproduced = false;
    if (!strcmp(argv[1], "repeat-info")) {
        sdlop.repeat_delay_ms = 500;
        sdlop.repeat_interval_ms = 33;
        keyboard_repeat_info(NULL, NULL, 0, 500); /* protocol: disable repeat */
        SDLOP_SendKeyboardKey(true, false, SDL_SCANCODE_A, 30, 0);
        sdlop.repeat_next_ns = SDLOP_MonotonicNS() - 1;
        SDLOP_KeyboardProcessRepeats();
        SDL_Event e;
        int repeats = 0;
        while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, 0, SDL_EVENT_LAST) == 1) {
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.repeat) { repeats++; }
        }
        SDLOP_SendKeyboardKey(false, false, SDL_SCANCODE_A, 30, 0);
        keyboard_repeat_info(NULL, NULL, 20, 0); /* legal immediate start */
        printf("repeat-info: rate0_repeats=%d rate20_delay0_stored_delay=%u interval=%u (wanted 0/50)\n",
               repeats, sdlop.repeat_delay_ms, sdlop.repeat_interval_ms);
        reproduced = repeats > 0 && sdlop.repeat_delay_ms == 500 && sdlop.repeat_interval_ms == 33;
    } else if (!strcmp(argv[1], "enter-held")) {
        WaylandWindowData wd = { 0 };
        void *saved = window->driverdata;
        wd.surface = (struct wl_surface *)(uintptr_t)1; /* identity only, never dereferenced */
        window->driverdata = &wd;
        sdlop.keyboard_focus = NULL;
        uint32_t held[] = { 42 }; /* evdev LEFTSHIFT, held before focus enter */
        struct wl_array keys = { .size = sizeof(held), .alloc = sizeof(held), .data = held };
        keyboard_enter(&wl_data, NULL, 1, wd.surface, &keys);
        bool focus = sdlop.keyboard_focus == window;
        bool shift = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_LSHIFT];
        printf("enter-held: focus_gained=%d advertised_held_shift=1 SDL_LSHIFT_down=%d\n", focus, shift);
        window->driverdata = saved;
        reproduced = focus && !shift;
    } else { return 2; }
    SDL_Quit();
    printf("%s: %s\n", argv[1], reproduced ? "REPRODUCED" : "not reproduced");
    return reproduced ? 1 : 0;
}
