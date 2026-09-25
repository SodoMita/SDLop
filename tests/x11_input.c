/*
  X11 test client.

  Prints one machine-readable line per interesting event, so a shell driver
  (tests/x11_input.sh) can drive it with xdotool and assert on what arrived. It
  exists because the X11 backend's input fallback is only exercised by real X
  events: the evdev worker is not the input source here, so this is what proves
  the translation of the X event stream (scancodes, keycodes, text, modifiers,
  repeat, pointer motion, buttons, the wheel, window geometry) against a running
  X server.

  Usage: x11_input [--seconds N] [--title T] [--width W] [--height H]
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
    double seconds = 8.0;
    const char *title = "sdlop-x11";
    int width = 320, height = 240, wx = 0, wy = 0, ww = 0, wh = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = SDL_atof(argv[++i]);
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            title = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = SDL_atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = SDL_atoi(argv[++i]);
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "x11_input: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow(title, width, height, 0);
    if (!window) {
        fprintf(stderr, "x11_input: create window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowPosition(window, 120, 90);
    SDL_ShowWindow(window);
    SDL_StartTextInput(window);

    /* Where the window is and how big it is: the driver computes the pointer
       coordinates it injects from this, so nothing has to be assumed about the X
       server (there is no window manager in a test setup, and a window manager
       would decide the position anyway). */
    SDL_GetWindowPosition(window, &wx, &wy);
    SDL_GetWindowSize(window, &ww, &wh);
    printf("READY driver=%s x=%d y=%d w=%d h=%d id=%u x11=%lld\n", SDL_GetCurrentVideoDriver(),
           wx, wy, ww, wh, (unsigned)SDL_GetWindowID(window),
           (long long)SDL_GetNumberProperty(SDL_GetWindowProperties(window),
                                            SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));

    /* The displays this backend found, so the driver can compare them with what
       the X server itself reports (xrandr). */
    {
        int count = 0;
        SDL_DisplayID *displays = SDL_GetDisplays(&count);
        SDL_DisplayID primary = SDL_GetPrimaryDisplay();

        for (int i = 0; i < count; i++) {
            SDL_Rect bounds;
            const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(displays[i]);

            if (!SDL_GetDisplayBounds(displays[i], &bounds)) {
                continue;
            }
            printf("DISPLAY id=%u name=%s x=%d y=%d w=%d h=%d mode=%dx%d@%.2f primary=%d\n",
                   (unsigned)displays[i], SDL_GetDisplayName(displays[i]), bounds.x, bounds.y,
                   bounds.w, bounds.h, mode ? mode->w : 0, mode ? mode->h : 0,
                   mode ? (double)mode->refresh_rate : 0.0, displays[i] == primary ? 1 : 0);
        }
        SDL_free(displays);
    }
    fflush(stdout);

    start = SDL_GetTicks();
    deadline = start + (Uint64)(seconds * 1000.0);
    while (SDL_GetTicks() < deadline) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_MOUSE_MOTION:
                printf("EVENT motion x=%.1f y=%.1f xrel=%.1f yrel=%.1f\n", (double)event.motion.x,
                       (double)event.motion.y, (double)event.motion.xrel, (double)event.motion.yrel);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                printf("EVENT button down=1 button=%d x=%.1f y=%.1f\n", (int)event.button.button,
                       (double)event.button.x, (double)event.button.y);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                printf("EVENT button down=0 button=%d\n", (int)event.button.button);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                printf("EVENT wheel x=%.1f y=%.1f\n", (double)event.wheel.x, (double)event.wheel.y);
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
            case SDL_EVENT_WINDOW_RESIZED:
                printf("EVENT resized w=%d h=%d\n", event.window.data1, event.window.data2);
                break;
            case SDL_EVENT_WINDOW_MOVED:
                printf("EVENT moved x=%d y=%d\n", event.window.data1, event.window.data2);
                break;
            case SDL_EVENT_WINDOW_EXPOSED:
                printf("EVENT exposed\n");
                break;
            case SDL_EVENT_KEY_DOWN:
                printf("EVENT key down=1 scancode=%d key=0x%x mod=0x%x repeat=%d\n",
                       (int)event.key.scancode, (unsigned)event.key.key, (unsigned)event.key.mod,
                       event.key.repeat ? 1 : 0);
                break;
            case SDL_EVENT_KEY_UP:
                printf("EVENT key down=0 scancode=%d key=0x%x mod=0x%x repeat=%d\n",
                       (int)event.key.scancode, (unsigned)event.key.key, (unsigned)event.key.mod,
                       event.key.repeat ? 1 : 0);
                break;
            case SDL_EVENT_TEXT_INPUT:
                printf("EVENT text \"%s\"\n", event.text.text);
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                printf("EVENT close\n");
                break;
            default:
                break;
            }
            fflush(stdout);
        }
        paint(window);
        SDL_Delay(5);
    }

    SDL_StopTextInput(window);
    SDL_GetWindowPosition(window, &wx, &wy);
    SDL_GetWindowSize(window, &ww, &wh);
    {
        int pw = 0, ph = 0;

        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        printf("DONE x=%d y=%d w=%d h=%d pixel=%dx%d focus=%d\n", wx, wy, ww, wh, pw, ph,
               (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
