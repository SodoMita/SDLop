/*
  SDLop test: mouse wheel / touchpad scrolling on Wayland.

  mini_compositor's MINI_POINTER_CMD scripts send wl_pointer axis frames the
  way libinput-based compositors do (a wheel detent = axis 15 units plus
  value120 120 for wl_pointer v8+, or discrete 1 for v5-7). Checked, against
  stock SDL3's semantics:
    - one detent down -> exactly one SDL_EVENT_MOUSE_WHEEL with y = -1.0
      (the continuous value of the same frame must not be added on top)
    - up / horizontal signs, two detents in one frame, a 1/4 hi-res step
    - touchpad scrolling (continuous only): value / 10
    - natural scrolling (relative direction inverted, v9) -> direction
      SDL_MOUSEWHEEL_FLIPPED with unchanged values
  Modes: "v9", "v7" (discrete), "v4" (no source/discrete/value120: the
  continuous value / 10 is all there is).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            failures++;                                          \
        }                                                        \
    } while (0)

static pid_t server;
static char cmdpath[256];
static SDL_MouseWheelEvent wheels[64];
static int nwheel;

/* run pointer input in the compositor, collect the wheel events */
static void run(const char *cmds)
{
    FILE *f = fopen(cmdpath, "w");
    if (f) {
        fputs(cmds, f);
        fclose(f);
    }
    kill(server, SIGUSR2);
    nwheel = 0;
    const Uint64 end = SDL_GetTicks() + 250;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_MOUSE_WHEEL && nwheel < (int)SDL_arraysize(wheels)) {
                wheels[nwheel++] = ev.wheel;
            }
        }
        SDL_Delay(5);
    }
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.001f;
}

/* exactly one wheel event with these values */
static void expect(const char *what, float x, float y, SDL_MouseWheelDirection dir)
{
    CHECK(nwheel == 1, "%s: %d wheel events (want 1)", what, nwheel);
    if (nwheel >= 1) {
        CHECK(near(wheels[0].x, x) && near(wheels[0].y, y), "%s: x=%g y=%g (want %g %g)", what, wheels[0].x,
              wheels[0].y, x, y);
        CHECK(wheels[0].direction == dir, "%s: direction %d (want %d)", what, (int)wheels[0].direction, (int)dir);
        CHECK(wheels[0].windowID != 0, "%s: windowID set", what);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor v9|v7|v4\n", argv[0]);
        return 2;
    }
    const int seat_version = atoi(argv[2] + 1);
    char xdg[] = "/tmp/sdlop-mini-wheel-XXXXXX";
    if (!mkdtemp(xdg)) {
        perror("mkdtemp");
        return 2;
    }
    char sockpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/wayland-mini", xdg);
    snprintf(cmdpath, sizeof(cmdpath), "%s/wheel.cmd", xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    server = fork();
    if (server == 0) {
        setenv("MINI_POINTER_CMD", cmdpath, 1);
        setenv("MINI_SEAT_VERSION", argv[2] + 1, 1);
        execl(argv[1], "mini_compositor", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 200 && access(sockpath, F_OK) != 0; i++) {
        SDL_Delay(10);
    }
    if (access(sockpath, F_OK) != 0 || !SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "setup failed: %s\n", SDL_GetError());
        kill(server, SIGKILL);
        return 2;
    }
    SDL_Window *w = SDL_CreateWindow("wheel", 320, 240, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    SDL_Surface *s = w ? SDL_GetWindowSurface(w) : NULL;
    if (s) {
        SDL_FillSurfaceRect(s, NULL, 0xff000000);
        SDL_UpdateWindowSurface(w);
    }
    run("");
    run("");
    CHECK(SDL_GetMouseFocus() == w, "pointer entered the window");

    if (seat_version >= 5) {
        /* the frame carries steps: they win over the continuous 15 units */
        run("wheel 0 120\n");
        expect("one detent down", 0.0f, -1.0f, SDL_MOUSEWHEEL_NORMAL);
        run("wheel 0 -120\n");
        expect("one detent up", 0.0f, 1.0f, SDL_MOUSEWHEEL_NORMAL);
        run("wheel 1 120\n");
        expect("one detent right", 1.0f, 0.0f, SDL_MOUSEWHEEL_NORMAL);
        run("wheel 0 240\n");
        expect("two detents in one frame", 0.0f, -2.0f, SDL_MOUSEWHEEL_NORMAL);
    } else {
        /* wl_pointer v4: only "axis 15" per detent -> 15 / 10 like stock */
        run("wheel 0 120\n");
        expect("v4 detent down (continuous only)", 0.0f, -1.5f, SDL_MOUSEWHEEL_NORMAL);
    }
    if (seat_version >= 8) {
        run("wheel 0 30\n");
        expect("hi-res quarter step", 0.0f, -0.25f, SDL_MOUSEWHEEL_NORMAL);
    }
    if (seat_version >= 9) {
        run("natural 0 120\n");
        expect("natural scrolling", 0.0f, -1.0f, SDL_MOUSEWHEEL_FLIPPED);
        /* the direction is per frame */
        run("wheel 0 120\n");
        expect("normal again", 0.0f, -1.0f, SDL_MOUSEWHEEL_NORMAL);
    }
    run("finger 0 5\n");
    expect("touchpad scroll (continuous / 10)", 0.0f, -0.5f, SDL_MOUSEWHEEL_NORMAL);
    run("finger 1 -2.5\n");
    expect("touchpad scroll left", -0.25f, 0.0f, SDL_MOUSEWHEEL_NORMAL);

    SDL_DestroyWindow(w);
    SDL_Quit();
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    char path[300];
    snprintf(path, sizeof(path), "%s", cmdpath);
    unlink(path);
    rmdir(xdg);
    printf("test_wayland_wheel (%s): %s (%d failures)\n", argv[2], failures ? "FAIL" : "ok", failures);
    return failures ? 1 : 0;
}
