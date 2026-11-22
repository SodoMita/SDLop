/*
  SDLop test: Wayland pointer warps (stock SDL3).

  tests/mini_compositor.c with MINI_WARP_LOG logs warp requests. Modes:
    warp - wp_pointer_warp_v1: SDL_WarpMouseInWindow warps with the enter
           serial, clamped to the window (a protocol error otherwise);
           SDL_WarpMouseGlobal warps inside the pointer's window only
    hint - no wp_pointer_warp_v1: a oneshot pointer lock with a cursor
           position hint (pointer-constraints), released again
  Either way the motion event is synthesised (the compositor sends none).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
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

static char logpath[256];

static int count_lines(const char *prefix, char *last, size_t last_size)
{
    FILE *f = fopen(logpath, "r");
    if (last) {
        last[0] = '\0';
    }
    if (!f) {
        return 0;
    }
    char buf[128];
    int n = 0;
    while (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = '\0';
        if (strncmp(buf, prefix, strlen(prefix)) == 0) {
            n++;
            if (last) {
                snprintf(last, last_size, "%s", buf);
            }
        }
    }
    fclose(f);
    return n;
}

/* pump until the last line starting with prefix is want */
static bool wait_last(const char *prefix, const char *want)
{
    char last[128];
    const Uint64 end = SDL_GetTicks() + 2000;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        count_lines(prefix, last, sizeof(last));
        if (strcmp(last, want) == 0) {
            return true;
        }
        SDL_Delay(5);
    }
    fprintf(stderr, "  last '%s' line: '%s', want '%s'\n", prefix, last, want);
    return false;
}

/* the latest motion event of the queue */
static bool last_motion(SDL_WindowID id, float *x, float *y)
{
    SDL_PumpEvents();
    SDL_Event ev;
    bool got = false;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_MOUSE_MOTION && ev.motion.windowID == id) {
            *x = ev.motion.x;
            *y = ev.motion.y;
            got = true;
        }
    }
    return got;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor warp|hint\n", argv[0]);
        return 2;
    }
    const char *mode = argv[2];
    const bool warp = strcmp(mode, "warp") == 0;

    char xdg[] = "/tmp/sdlop-mini-warp-XXXXXX";
    if (!mkdtemp(xdg)) {
        perror("mkdtemp");
        return 2;
    }
    char sockpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/wayland-mini", xdg);
    snprintf(logpath, sizeof(logpath), "%s/warp.log", xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDL_TOUCH_MOUSE_EVENTS", "0", 1);

    pid_t server = fork();
    if (server == 0) {
        setenv("MINI_WARP_LOG", logpath, 1);
        if (!warp) {
            setenv("MINI_POINTER_WARP", "0", 1);
        }
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
    SDL_Window *w = SDL_CreateWindow("warp", 320, 240, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (!w) {
        kill(server, SIGKILL);
        return 1;
    }
    const SDL_WindowID id = SDL_GetWindowID(w);
    SDL_Surface *s = SDL_GetWindowSurface(w);
    if (s) {
        SDL_FillSurfaceRect(s, NULL, 0xff000000);
        SDL_UpdateWindowSurface(w);
    }
    const Uint64 end = SDL_GetTicks() + 2000;
    while (SDL_GetMouseFocus() != w && SDL_GetTicks() < end) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
    CHECK(SDL_GetMouseFocus() == w, "pointer entered");
    float mx = -1, my = -1;
    last_motion(id, &mx, &my); /* drain */

    SDL_WarpMouseInWindow(w, 50.5f, 60.25f);
    if (warp) {
        CHECK(wait_last("warp", "warp 50.5,60.25 serial=ok"), "wp_pointer_warp_v1 with the enter serial");
    } else {
        CHECK(wait_last("hint", "hint 50.5,60.25") && count_lines("lock", NULL, 0) == 1,
              "oneshot lock + cursor position hint");
    }
    mx = my = -1;
    CHECK(last_motion(id, &mx, &my) && mx == 50.5f && my == 60.25f, "synthesised motion to the warp (%g,%g)", mx, my);
    SDL_GetMouseState(&mx, &my);
    CHECK(mx == 50.5f && my == 60.25f, "mouse state follows (%g,%g)", mx, my);

    /* outside the window */
    SDL_WarpMouseInWindow(w, 500.0f, -10.0f);
    if (warp) {
        CHECK(wait_last("warp", "warp 320,0 serial=ok"), "clamped to the surface");
    } else {
        CHECK(wait_last("hint", "hint 500,-10"), "hint not clamped (stock)");
    }

    if (warp) {
        int wx = 0, wy = 0;
        SDL_Rect b = { 0, 0, 0, 0 };
        SDL_GetWindowPosition(w, &wx, &wy);
        SDL_GetDisplayBounds(SDL_GetDisplayForWindow(w), &b);
        CHECK(SDL_WarpMouseGlobal((float)(b.x + wx + 30), (float)(b.y + wy + 40)), "global warp");
        CHECK(wait_last("warp", "warp 30,40 serial=ok"), "global warp inside the window");
        mx = my = -1;
        CHECK(last_motion(id, &mx, &my) && mx == 30.0f && my == 40.0f, "global warp motion (%g,%g)", mx, my);
        CHECK(SDL_WarpMouseGlobal((float)(b.x + wx + 1000), (float)(b.y + wy + 40)), "global warp outside");
        SDL_Delay(50);
        SDL_PumpEvents();
        CHECK(count_lines("warp", NULL, 0) == 3, "no warp outside the pointer's window (%d)",
              count_lines("warp", NULL, 0));
        CHECK(count_lines("lock", NULL, 0) == 0, "no locks with wp_pointer_warp_v1");
    }

    SDL_DestroyWindow(w);
    SDL_Quit();
    int status = 0;
    waitpid(server, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "compositor exit status %d", status);
    unlink(logpath);
    rmdir(xdg);
    if (failures) {
        fprintf(stderr, "test_wayland_warp (%s): %d failure(s)\n", mode, failures);
        return 1;
    }
    printf("test_wayland_warp (%s): OK\n", mode);
    return 0;
}
