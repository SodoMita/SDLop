/*
  SDLop test: Wayland software presentation follows the wl_buffer contract.

  Runs against tests/mini_compositor.c with MINI_BUFFER_CHECK=1, which holds
  committed buffers until its next repaint (and once stalls for 300 ms) and
  fails if SDLop attaches or writes a buffer before wl_buffer.release, or if
  a committed frame differs from what was drawn - full and partial
  (SDL_UpdateWindowSurfaceRects) updates, across two window resizes.
  The frame model is described in mini_compositor.c.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL: " __VA_ARGS__); \
            fprintf(stderr, "\n");                \
            failures++;                           \
        }                                         \
    } while (0)

#define FRAMES 120

static Uint32 frame_color(int k)
{
    return ((Uint32)k * 2654435761u) & 0xffffffu;
}

static void frame_band(int k, int W, int H, SDL_Rect *r)
{
    r->w = W / 2;
    r->h = 8;
    r->x = (k * 13) % (W / 2);
    r->y = (k * 37) % (H - 8);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor\n", argv[0]);
        return 2;
    }

    char xdg[] = "/tmp/sdlop-mini-buffers-XXXXXX";
    if (!mkdtemp(xdg)) {
        perror("mkdtemp");
        return 2;
    }
    char sockpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/wayland-mini", xdg);

    pid_t server = fork();
    if (server == 0) {
        setenv("XDG_RUNTIME_DIR", xdg, 1);
        setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
        setenv("MINI_BUFFER_CHECK", "1", 1);
        execl(argv[1], "mini_compositor", (char *)NULL);
        _exit(127);
    }
    if (server < 0) {
        perror("fork");
        return 2;
    }
    for (int i = 0; i < 200 && access(sockpath, F_OK) != 0; i++) {
        SDL_Delay(10);
    }
    if (access(sockpath, F_OK) != 0) {
        fprintf(stderr, "mini compositor did not create %s\n", sockpath);
        kill(server, SIGKILL);
        return 2;
    }

    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        kill(server, SIGKILL);
        return 1;
    }

    static const int sizes[][2] = { { 320, 240 }, { 200, 150 }, { 360, 200 } };
    SDL_Window *w = SDL_CreateWindow("buffer test", sizes[0][0], sizes[0][1], 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    Uint64 max_present = 0;
    for (int k = 0; w && k < FRAMES; k++) {
        if (k % 40 == 0 && k > 0) {
            CHECK(SDL_SetWindowSize(w, sizes[k / 40][0], sizes[k / 40][1]), "resize: %s", SDL_GetError());
            SDL_SyncWindow(w);
        }
        SDL_Surface *s = SDL_GetWindowSurface(w);
        CHECK(s != NULL, "frame %d: window surface: %s", k, SDL_GetError());
        if (!s) {
            break;
        }
        if (k % 40 == 0) {
            CHECK(s->w == sizes[k / 40][0] && s->h == sizes[k / 40][1], "frame %d: surface %dx%d", k, s->w, s->h);
        }
        void *pixels = s->pixels;
        SDL_Rect rects[2];
        int n;
        if (k % 40 == 0) {
            SDL_FillSurfaceRect(s, NULL, frame_color(k));
            n = 0;
        } else {
            frame_band(k, s->w, s->h, &rects[0]);
            SDL_FillSurfaceRect(s, &rects[0], frame_color(k));
            n = 2;
        }
        rects[1].x = rects[1].y = 0;
        rects[1].w = rects[1].h = 1;
        SDL_FillSurfaceRect(s, &rects[1], 0xA50000u | (Uint32)k);
        Uint64 t = SDL_GetTicksNS();
        bool ok = n ? SDL_UpdateWindowSurfaceRects(w, rects, n) : SDL_UpdateWindowSurface(w);
        t = SDL_GetTicksNS() - t;
        if (t > max_present) {
            max_present = t;
        }
        CHECK(ok, "frame %d: present: %s", k, SDL_GetError());
        /* SDL3: the surface pixels stay put between presents */
        CHECK(SDL_GetWindowSurface(w)->pixels == pixels, "frame %d: surface pixels moved", k);
        SDL_PumpEvents();
    }
    /* the 300 ms compositor stall must have blocked a present, not failed it */
    CHECK(max_present > 200 * 1000000ull, "no present waited for the stalled compositor (max %llu us)",
          (unsigned long long)(max_present / 1000));
    SDL_DestroyWindow(w);
    SDL_Quit();

    int status = 0;
    if (waitpid(server, &status, 0) != server || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: mini compositor buffer check (status %d)\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        failures++;
    }
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", xdg);
    if (system(cmd) != 0) {
        /* ignore */
    }
    if (failures) {
        fprintf(stderr, "test_wayland_buffers: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_wayland_buffers: PASS\n");
    return 0;
}
