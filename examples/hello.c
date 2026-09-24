/*
  The smallest useful SDLop program: open a window, paint it, react to input.

  Build (from the project root):
      make examples && ./build/examples/hello

  It runs on Wayland, X11 or offscreen:

      SDL_VIDEODRIVER=wayland ./build/examples/hello
      SDL_VIDEODRIVER=offscreen ./build/examples/hello --frames 3
*/

#include <SDL3/SDL.h>
#include <stdio.h>

int main(int argc, char *argv[])
{
    SDL_Window *window;
    SDL_Surface *surface;
    SDL_Event event;
    Uint64 start;
    int frames = 0;
    int max_frames = 0;
    bool running = true;

    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = SDL_atoi(argv[++i]);
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    {
        int version = SDL_GetVersion();
        printf("SDLop %d.%d.%d, tracking SDL3 API %d.%d.%d, on video driver '%s'\n",
               SDL_SDLOP_MAJOR_VERSION, SDL_SDLOP_MINOR_VERSION, SDL_SDLOP_MICRO_VERSION,
               version / 1000000, (version / 1000) % 1000, version % 1000,
               SDL_GetCurrentVideoDriver());
    }

    window = SDL_CreateWindow("SDLop", 640, 360, SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    start = SDL_GetTicks();
    while (running) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    running = false;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    printf("key down: scancode %d, key '%s'\n", (int)event.key.scancode,
                           SDL_GetKeyName(event.key.key));
                    if (event.key.key == SDLK_ESCAPE) {
                        running = false;
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    printf("mouse: %.0f,%.0f\n", event.motion.x, event.motion.y);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    printf("mouse button %d\n", event.button.button);
                    break;
                case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                    printf("display scale: %.2f\n", SDL_GetWindowDisplayScale(window));
                    break;
                default:
                    break;
            }
        }

        /* Paint the window surface: a moving colour ramp plus a corner box, so a
           screenshot shows both a gradient and hard edges. */
        surface = SDL_GetWindowSurface(window);
        if (surface) {
            const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(surface->format);
            Uint64 ticks = SDL_GetTicks();
            for (int y = 0; y < surface->h; y++) {
                Uint32 *row = (Uint32 *)(void *)((Uint8 *)surface->pixels + (size_t)y * surface->pitch);
                Uint8 g = (Uint8)((y + ticks / 8) & 0xFF);
                for (int x = 0; x < surface->w; x++) {
                    Uint8 r = (Uint8)((x + ticks / 16) & 0xFF);
                    row[x] = SDL_MapRGB(format, NULL, r, g, 64);
                }
            }
            SDL_FillSurfaceRect(surface, &(SDL_Rect){ 8, 8, 64, 64 },
                                SDL_MapRGB(format, NULL, 255, 255, 255));
            SDL_UpdateWindowSurface(window);
        }

        frames++;
        if (max_frames && frames >= max_frames) {
            running = false;
        }
        SDL_Delay(1);
    }

    printf("%d frames in %llu ms\n", frames, (unsigned long long)(SDL_GetTicks() - start));
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
