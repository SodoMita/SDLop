/*
  Headless Wayland display test client.

  Prints one machine-readable line per display and per display event, so
  tests/wayland_display.sh can assert geometry, HiDPI scale and hotplug
  behaviour against a compositor whose outputs are configured out of band.

  Usage: wayland_display [--seconds N] [--watch]
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dump_displays(const char *tag)
{
    SDL_DisplayID *ids;
    int count = 0, i;

    ids = SDL_GetDisplays(&count);
    for (i = 0; i < count; i++) {
        SDL_Rect bounds;
        const SDL_DisplayMode *mode;
        const char *name;
        float content_scale;

        if (!SDL_GetDisplayBounds(ids[i], &bounds)) {
            bounds.x = bounds.y = bounds.w = bounds.h = -1;
        }
        mode = SDL_GetCurrentDisplayMode(ids[i]);
        name = SDL_GetDisplayName(ids[i]);
        content_scale = SDL_GetDisplayContentScale(ids[i]);
        printf("DISPLAY %s id=%u name=%s bounds=%d,%d,%d,%d mode=%dx%d density=%.2f content=%.2f\n",
               tag, (unsigned)ids[i], name ? name : "?", bounds.x, bounds.y, bounds.w, bounds.h,
               mode ? mode->w : -1, mode ? mode->h : -1,
               mode ? (double)mode->pixel_density : -1.0, (double)content_scale);
    }
    if (count == 0) {
        printf("DISPLAY %s none\n", tag);
    }
    SDL_free(ids);
}

int main(int argc, char **argv)
{
    SDL_Window *window = NULL;
    SDL_Event event;
    Uint64 deadline, start;
    double seconds = 8.0;
    bool watch = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = SDL_atof(argv[++i]);
        } else if (strcmp(argv[i], "--watch") == 0) {
            watch = true;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "wayland_display: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    dump_displays("start");

    if (!watch) {
        SDL_Quit();
        return 0;
    }

    /* A window on the second display if there is one, so the hotplug test also
       shows how windows are re-homed. */
    {
        int count = 0;
        SDL_DisplayID *ids = SDL_GetDisplays(&count);
        window = SDL_CreateWindow("display-test", 200, 150, SDL_WINDOW_HIDDEN);
        if (!window) {
            fprintf(stderr, "wayland_display: create window: %s\n", SDL_GetError());
            return 1;
        }
        if (count > 1) {
            SDL_Rect bounds;
            SDL_GetDisplayBounds(ids[count - 1], &bounds);
            SDL_SetWindowPosition(window, bounds.x + 40, bounds.y + 40);
        }
        SDL_free(ids);
        SDL_ShowWindow(window);
        {
            SDL_DisplayID d = SDL_GetDisplayForWindow(window);
            int w = 0, h = 0, pw = 0, ph = 0;
            SDL_GetWindowSize(window, &w, &h);
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            printf("WINDOW start display=%u scale=%.2f size=%dx%d pixels=%dx%d\n", (unsigned)d,
                   (double)SDL_GetWindowDisplayScale(window), w, h, pw, ph);
        }
        printf("READY\n");
        fflush(stdout);
    }

    start = SDL_GetTicks();
    deadline = start + (Uint64)(seconds * 1000.0);
    while (SDL_GetTicks() < deadline) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_DISPLAY_ADDED:
                printf("EVENT display added=%u\n", (unsigned)event.display.displayID);
                dump_displays("after-added");
                break;
            case SDL_EVENT_DISPLAY_REMOVED:
                printf("EVENT display removed=%u\n", (unsigned)event.display.displayID);
                dump_displays("after-removed");
                break;
            case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
                printf("EVENT content-scale display=%u\n", (unsigned)event.display.displayID);
                break;
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
                printf("EVENT window display=%d\n", event.window.data1);
                break;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                printf("EVENT window scale=%.2f\n", (double)SDL_GetWindowDisplayScale(window));
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                printf("EVENT window pixels=%dx%d\n", event.window.data1, event.window.data2);
                break;
            default:
                break;
            }
            fflush(stdout);
        }
        {
            SDL_Surface *surface = SDL_GetWindowSurface(window);
            if (surface) {
                SDL_FillSurfaceRect(surface, NULL, SDL_MapSurfaceRGB(surface, 30, 30, 90));
                SDL_UpdateWindowSurface(window);
            }
        }
        SDL_Delay(10);
    }
    {
        int w = 0, h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(window, &w, &h);
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        printf("WINDOW end display=%u scale=%.2f size=%dx%d pixels=%dx%d\n",
               (unsigned)SDL_GetDisplayForWindow(window),
               (double)SDL_GetWindowDisplayScale(window), w, h, pw, ph);
    }
    dump_displays("end");
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
