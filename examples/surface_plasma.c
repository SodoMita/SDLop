/*
  SDLop example: software rendering via the window surface.
  The surface maps directly onto the Wayland shm buffer (zero copy).

  Usage: surface_plasma [frames]   (pass a frame count to benchmark)
*/

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void render(SDL_Surface *s, Uint64 t_ns)
{
    double t = (double)t_ns / 1e9;
    Uint32 *pix = (Uint32 *)s->pixels;
    int pitch32 = s->pitch / 4;
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            double v = sin(x / 32.0 + t) + sin(y / 24.0 + t * 1.3) +
                       sin((x + y) / 48.0 + t * 0.7) + sin(sqrt((double)(x * x + y * y)) / 40.0 - t);
            Uint32 r = (Uint32)(127 + 127 * sin(v * 3.14159));
            Uint32 g = (Uint32)(127 + 127 * sin(v * 3.14159 + 2.0));
            Uint32 b = (Uint32)(127 + 127 * sin(v * 3.14159 + 4.0));
            pix[(size_t)y * (size_t)pitch32 + x] = (r << 16) | (g << 8) | b; /* XRGB8888 */
        }
    }
}

int main(int argc, char *argv[])
{
    int bench_frames = 0;
    if (argc > 1) {
        bench_frames = atoi(argv[1]);
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("SDLop - software surface plasma", 640, 480, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Surface *surface = SDL_GetWindowSurface(window);
    if (!surface) {
        fprintf(stderr, "SDL_GetWindowSurface failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    printf("software surface: %dx%d pitch=%d format=0x%08x (XRGB8888=%d)\n",
           surface->w, surface->h, surface->pitch, (unsigned)surface->format,
           surface->format == SDL_PIXELFORMAT_XRGB8888);

    bool done = false;
    int frames = 0;
    Uint64 t0 = SDL_GetTicksNS();
    while (!done && (!bench_frames || frames < bench_frames)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                done = true;
            }
            if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                surface = SDL_GetWindowSurface(window); /* follows the resize */
            }
        }
        if (surface) {
            render(surface, SDL_GetTicksNS());
            SDL_UpdateWindowSurface(window);
            frames++;
        }
        if (!bench_frames) {
            SDL_Delay(16);
        }
    }
    Uint64 dt = SDL_GetTicksNS() - t0;
    if (bench_frames) {
        printf("software surface: %d frames in %.3f s => %.1f fps (%.1f MPix/s)\n",
               frames, dt / 1e9, frames / (dt / 1e9),
               (double)frames * surface->w * surface->h / (dt / 1e9) / 1e6);
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
