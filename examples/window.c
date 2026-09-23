/*
  SDLop example: classic SDL3-style window + event loop.

  This exact source compiles against real SDL3 unchanged.
*/

#include <SDL3/SDL.h>
#include <stdio.h>

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("SDLop - window example", 800, 600, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                done = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                done = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                printf("key down: %s (scancode %d, raw evdev %u)%s\n",
                       SDL_GetKeyName(event.key.key), (int)event.key.scancode,
                       (unsigned)event.key.raw, event.key.repeat ? " [repeat]" : "");
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    done = true;
                }
                break;
            case SDL_EVENT_KEY_UP:
                printf("key up:   %s\n", SDL_GetKeyName(event.key.key));
                break;
            case SDL_EVENT_TEXT_INPUT:
                printf("text:     \"%s\"\n", event.text.text);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                printf("motion:   %.1f, %.1f (rel %.1f, %.1f)\n",
                       event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                printf("button %s: %u at %.1f, %.1f\n",
                       event.button.down ? "down" : "up", (unsigned)event.button.button,
                       event.button.x, event.button.y);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                printf("wheel:    %.2f, %.2f\n", event.wheel.x, event.wheel.y);
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                printf("resized:  %d x %d\n", event.window.data1, event.window.data2);
                break;
            default:
                break;
            }
        }
        SDL_Delay(16); /* ~60 Hz idle */
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
