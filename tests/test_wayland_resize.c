/*
  SDLop test: Wayland interactive resizes and the window surface.

  tests/mini_compositor.c with MINI_RESIZE_LOG holds frame callbacks until
  the next commit and logs configures, acks, commits, pools and buffers.

    - a burst of configures in the resizing state is throttled the stock
      way: one is applied (acked) per frame callback and the newest wins, so
      an application that draws on every RESIZED sees two sizes for a burst
      of 20, not 20; each applied size comes with RESIZED and EXPOSED
    - the configure that ends the resize is applied right away
    - the window surface stays valid across resizes (SDLop): the same
      SDL_Surface pointer, resized in place by SDL_GetWindowSurface
    - SDL_UpdateWindowSurface on a surface drawn before the resize reached
      it presents that frame at its size, succeeds, and resizes the surface
      afterwards
    - resizing within the shm buffers' headroom maps no new memory: no new
      wl_shm_pool, only new wl_buffers
    - an application that stops drawing during a resize does not hold the
      compositor up: the newest configure is acked and committed (clear
      colour) after a short stall

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
static char cmdpath[256];
static pid_t server;

/* number of log lines starting with prefix; the last one in last */
static int count_lines(const char *prefix, char *last, size_t last_size)
{
    FILE *f = fopen(logpath, "r");
    if (last) {
        last[0] = '\0';
    }
    if (!f) {
        return 0;
    }
    char buf[256];
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

static void send_cmd(const char *cmd)
{
    FILE *f = fopen(cmdpath, "w");
    if (f) {
        fputs(cmd, f);
        fclose(f);
    }
    kill(server, SIGUSR2);
}

typedef struct
{
    int resized;
    int exposed;
    int last_w, last_h;
} Counts;

/* an application loop: on every RESIZED (or always, with draw_always) it
 * fetches the window surface, checks it, draws and presents */
static void run_app(SDL_Window *w, SDL_Surface *surface, Uint32 ms, bool draw, Counts *c)
{
    const Uint64 end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        bool redraw = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_WINDOW_RESIZED) {
                c->resized++;
                c->last_w = ev.window.data1;
                c->last_h = ev.window.data2;
                redraw = true;
            } else if (ev.type == SDL_EVENT_WINDOW_EXPOSED) {
                c->exposed++;
            }
        }
        if (draw && redraw) {
            SDL_Surface *s = SDL_GetWindowSurface(w);
            CHECK(s == surface, "the window surface pointer stays the same across resizes");
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(w, &pw, &ph);
            CHECK(s && s->w == pw && s->h == ph, "surface %dx%d follows the window %dx%d", s ? s->w : -1,
                  s ? s->h : -1, pw, ph);
            if (s) {
                SDL_FillSurfaceRect(s, NULL, 0xff336699);
                CHECK(SDL_UpdateWindowSurface(w), "present: %s", SDL_GetError());
            }
        }
        SDL_Delay(2);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor\n", argv[0]);
        return 2;
    }
    char xdg[] = "/tmp/sdlop-mini-resize-XXXXXX";
    if (!mkdtemp(xdg)) {
        perror("mkdtemp");
        return 2;
    }
    char sockpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/wayland-mini", xdg);
    snprintf(logpath, sizeof(logpath), "%s/resize.log", xdg);
    snprintf(cmdpath, sizeof(cmdpath), "%s/resize.cmd", xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    server = fork();
    if (server == 0) {
        setenv("MINI_POINTER_CMD", cmdpath, 1);
        setenv("MINI_RESIZE_LOG", logpath, 1);
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
    SDL_Window *w = SDL_CreateWindow("resize", 200, 150, SDL_WINDOW_RESIZABLE);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (!w) {
        kill(server, SIGKILL);
        return 1;
    }
    SDL_Surface *surface = SDL_GetWindowSurface(w);
    CHECK(surface && surface->w == 200 && surface->h == 150, "initial window surface: %s", SDL_GetError());
    if (!surface) {
        kill(server, SIGKILL);
        return 1;
    }
    SDL_FillSurfaceRect(surface, NULL, 0xff000000);
    SDL_UpdateWindowSurface(w);
    Counts c = { 0, 0, 0, 0 };
    run_app(w, surface, 150, true, &c);
    char last[256];

    /* 1. a burst of 20 resizing configures, growing beyond the buffers */
    send_cmd("rburst 20 300 200 10 5\n");
    memset(&c, 0, sizeof(c));
    run_app(w, surface, 400, true, &c);
    CHECK(count_lines("configure ", NULL, 0) == 20, "20 configures sent");
    const int acks1 = count_lines("ack ", last, sizeof(last));
    CHECK(strcmp(last, "ack 490x295 resizing=1") == 0, "the newest configure is acked last: '%s'", last);
    const int burst_acks = count_lines("ack 3", NULL, 0) + count_lines("ack 4", NULL, 0);
    CHECK(burst_acks >= 1 && burst_acks <= 3, "throttled: %d of 20 configures applied", burst_acks);
    CHECK(c.resized >= 1 && c.resized <= 3, "throttled: %d RESIZED for 20 configures", c.resized);
    CHECK(c.last_w == 490 && c.last_h == 295, "newest size wins: %dx%d", c.last_w, c.last_h);
    CHECK(c.exposed >= c.resized, "EXPOSED with each applied size (%d, %d)", c.exposed, c.resized);
    CHECK(count_lines("commit ", last, sizeof(last)) > 0 && strcmp(last, "commit 490x295") == 0,
          "the application's frame at the newest size is committed: '%s'", last);
    CHECK(surface->w == 490 && surface->h == 295, "surface resized in place: %dx%d", surface->w, surface->h);

    /* 2. sizes within the headroom: new wl_buffers on the memory there is.
     * A buffer of the cycle that was not used at the larger size grows
     * once, so allow a pool per extra buffer - but most new sizes must be
     * served without one (each used to cost a new pool per buffer) */
    const int pools_before = count_lines("pool ", NULL, 0);
    const int buffers_before = count_lines("buffer ", NULL, 0);
    send_cmd("rburst 10 480 290 -4 -2\n");
    memset(&c, 0, sizeof(c));
    run_app(w, surface, 300, true, &c);
    CHECK(count_lines("ack ", last, sizeof(last)) > acks1 && strcmp(last, "ack 444x272 resizing=1") == 0,
          "second burst: newest acked last: '%s'", last);
    for (int i = 0; i < 4; i++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "rend %d %d\n", 470 - i * 10, 280 - i * 5);
        send_cmd(cmd);
        run_app(w, surface, 60, true, &c);
    }
    const int pools_added = count_lines("pool ", NULL, 0) - pools_before;
    const int buffers_added = count_lines("buffer ", NULL, 0) - buffers_before;
    CHECK(buffers_added >= 6 && buffers_added - pools_added >= 4,
          "within the headroom: %d new wl_buffers but %d new pools", buffers_added, pools_added);

    /* 3. the end of the resize is applied immediately */
    send_cmd("rend 400 250\n");
    memset(&c, 0, sizeof(c));
    run_app(w, surface, 150, true, &c);
    CHECK(count_lines("ack ", last, sizeof(last)) > 0 && strcmp(last, "ack 400x250 resizing=0") == 0,
          "resize end acked: '%s'", last);
    CHECK(c.resized == 1 && c.last_w == 400 && c.last_h == 250, "one RESIZED to 400x250 (%d, %dx%d)", c.resized,
          c.last_w, c.last_h);

    /* 4. a frame drawn before the resize reached the surface */
    send_cmd("rend 420 260\n");
    memset(&c, 0, sizeof(c));
    run_app(w, surface, 150, false, &c);
    CHECK(c.resized == 1, "RESIZED to 420x260");
    CHECK(surface->w == 400 && surface->h == 250, "untouched until the application uses it: %dx%d", surface->w,
          surface->h);
    SDL_FillSurfaceRect(surface, NULL, 0xff00ff00);
    CHECK(SDL_UpdateWindowSurface(w), "stale surface still presents: %s", SDL_GetError());
    run_app(w, surface, 50, false, &c);
    CHECK(count_lines("commit ", last, sizeof(last)) > 0 && strcmp(last, "commit 400x250") == 0,
          "presented at the size it was drawn at: '%s'", last);
    CHECK(surface->w == 420 && surface->h == 260, "resized after that present: %dx%d", surface->w, surface->h);
    CHECK(SDL_GetWindowSurface(w) == surface, "same surface");

    /* 5. an application that stops drawing mid-resize */
    send_cmd("rburst 5 500 300 2 2\n");
    memset(&c, 0, sizeof(c));
    run_app(w, surface, 400, false, &c);
    CHECK(count_lines("ack ", last, sizeof(last)) > 0 && strcmp(last, "ack 508x308 resizing=1") == 0,
          "stalled resize: newest acked anyway: '%s'", last);
    CHECK(count_lines("commit ", last, sizeof(last)) > 0 && strcmp(last, "commit 508x308") == 0,
          "stalled resize: committed at the newest size: '%s'", last);

    SDL_DestroyWindow(w);
    SDL_Quit();
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    if (failures || getenv("SDLOP_TEST_VERBOSE")) {
        FILE *f = fopen(logpath, "r");
        char buf[256];
        while (f && fgets(buf, sizeof(buf), f)) {
            fprintf(stderr, "  log: %s", buf);
        }
        if (f) {
            fclose(f);
        }
    }
    unlink(logpath);
    unlink(cmdpath);
    rmdir(xdg);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_wayland_resize: OK\n");
    return 0;
}
