/*
  Headless Wayland pointer test client.

  Prints one machine-readable line per interesting event, so a shell driver
  (tests/wayland_input.sh) can assert on it. It exists because a compositor with
  no input devices cannot exercise the pointer path at all -- wl_inject supplies
  the events (through wlroots' virtual pointer), this supplies the window.

  Usage: wayland_input [--relative] [--motion-only] [--seconds N] [--title T]
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void paint(SDL_Window *window)
{
    SDL_Surface *surface = SDL_GetWindowSurface(window);
    if (surface) {
        SDL_FillSurfaceRect(surface, NULL, SDL_MapSurfaceRGB(surface, 20, 40, 80));
        SDL_UpdateWindowSurface(window);
    }
}

int main(int argc, char **argv)
{
    SDL_Window *window;
    SDL_Event event;
    Uint64 start, deadline;
    bool relative = false;
    double seconds = 6.0;
    const char *title = "sdlop-input";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--relative") == 0) {
            relative = true;
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = SDL_atof(argv[++i]);
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            title = argv[++i];
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "wayland_input: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow(title, 400, 300, 0);
    if (!window) {
        fprintf(stderr, "wayland_input: create window: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_SetWindowRelativeMouseMode(window, relative)) {
        fprintf(stderr, "wayland_input: relative mode: %s\n", SDL_GetError());
        return 1;
    }
    printf("READY driver=%s relative=%d size=%dx%d\n", SDL_GetCurrentVideoDriver(),
           relative ? 1 : 0, 400, 300);
    fflush(stdout);

    start = SDL_GetTicks();
    deadline = start + (Uint64)(seconds * 1000.0);
    while (SDL_GetTicks() < deadline) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_MOUSE_MOTION:
                printf("EVENT motion x=%.1f y=%.1f xrel=%.1f yrel=%.1f rel=%d\n",
                       (double)event.motion.x, (double)event.motion.y,
                       (double)event.motion.xrel, (double)event.motion.yrel,
                       (event.motion.xrel != 0.0f || event.motion.yrel != 0.0f) ? 1 : 0);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                printf("EVENT button down=%d button=%d x=%.1f y=%.1f\n", 1,
                       (int)event.button.button, (double)event.button.x, (double)event.button.y);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                printf("EVENT button down=%d button=%d\n", 0, (int)event.button.button);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                printf("EVENT wheel x=%.1f y=%.1f\n", (double)event.wheel.x,
                       (double)event.wheel.y);
                break;
            case SDL_EVENT_WINDOW_MOUSE_ENTER:
                printf("EVENT enter\n");
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                printf("EVENT leave\n");
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                printf("EVENT focus=1\n");
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                printf("EVENT focus=0\n");
                break;
            case SDL_EVENT_KEY_DOWN:
                printf("EVENT key down=1 scancode=%d key=%u\n", (int)event.key.scancode,
                       (unsigned)event.key.key);
                break;
            case SDL_EVENT_KEY_UP:
                printf("EVENT key down=0 scancode=%d key=%u\n", (int)event.key.scancode,
                       (unsigned)event.key.key);
                break;
            default:
                break;
            }
            fflush(stdout);
        }
        paint(window);
        SDL_Delay(8);
    }

    {
        int pw = 0, ph = 0, ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        SDL_GetWindowSize(window, &ww, &wh);
        printf("DONE relative=%d size=%dx%d pixels=%dx%d\n",
               SDL_GetWindowRelativeMouseMode(window) ? 1 : 0, ww, wh, pw, ph);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
