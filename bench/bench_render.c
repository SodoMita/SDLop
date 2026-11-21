/*
  Benchmark: software surface fill vs OpenGL clear at 640x480, through
  whatever video driver SDL picks (SDL_VIDEODRIVER=wayland/x11/kmsdrm to
  choose). "present" is the time spent in SDL_UpdateWindowSurface: on
  Wayland it includes waiting for the compositor to release a buffer, so
  the software rate is bounded by the compositor, as for any conforming
  client.

  Usage: bench_render [frames-per-test]
*/

#include <SDL3/SDL.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double fps_for(Uint64 dt_ns, int frames)
{
    return frames / ((double)dt_ns / 1e9);
}

int main(int argc, char *argv[])
{
    const int frames = argc > 1 ? atoi(argv[1]) : 300;
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    printf("video driver: %s, %d frames per test\n", SDL_GetCurrentVideoDriver(), frames);

    /* ---- software surface: full-window fill per frame ---- */
    {
        SDL_Window *w = SDL_CreateWindow("bench sw", 640, 480, 0);
        SDL_Surface *s = SDL_GetWindowSurface(w);
        if (!s) {
            fprintf(stderr, "no window surface: %s\n", SDL_GetError());
        } else {
            Uint32 color = 0x0030A040;
            Uint64 present = 0;
            Uint64 t0 = SDL_GetTicksNS();
            for (int i = 0; i < frames; i++) {
                color ^= 0x000000FF;
                SDL_FillSurfaceRect(s, NULL, color);
                Uint64 p0 = SDL_GetTicksNS();
                SDL_UpdateWindowSurface(w);
                present += SDL_GetTicksNS() - p0;
                SDL_PumpEvents();
            }
            Uint64 dt = SDL_GetTicksNS() - t0;
            printf("software surface fill+present: %6.1f fps  (%.1f us/frame, present %.1f us, %.1f MPix/s)\n",
                   fps_for(dt, frames), (double)dt / frames / 1e3, (double)present / frames / 1e3,
                   (double)frames * s->w * s->h / ((double)dt / 1e9) / 1e6);
        }
        SDL_DestroyWindow(w);
    }

    /* ---- OpenGL: glClear per frame via llvmpipe ---- */
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_Window *w = SDL_CreateWindow("bench gl", 640, 480, SDL_WINDOW_OPENGL);
        SDL_GLContext ctx = w ? SDL_GL_CreateContext(w) : NULL;
        if (!ctx) {
            fprintf(stderr, "no GL context: %s\n", SDL_GetError());
        } else {
            SDL_GL_SetSwapInterval(0);
            const char *rend = (const char *)glGetString(GL_RENDERER);
            Uint64 t0 = SDL_GetTicksNS();
            for (int i = 0; i < frames; i++) {
                glClearColor((float)(i % 255) / 255.0f, 0.2f, 0.6f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                SDL_GL_SwapWindow(w);
                SDL_PumpEvents();
            }
            Uint64 dt = SDL_GetTicksNS() - t0;
            printf("GL glClear+swap (%s): %6.1f fps  (%.1f us/frame, %.1f MPix/s)\n",
                   rend ? rend : "?", fps_for(dt, frames), (double)dt / frames / 1e3,
                   (double)frames * 640 * 480 / ((double)dt / 1e9) / 1e6);
            SDL_GL_DestroyContext(ctx);
        }
        SDL_DestroyWindow(w);
    }

    SDL_Quit();
    return 0;
}
