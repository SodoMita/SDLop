/*
  SDLop test: Wayland HiDPI (stock SDL3 scale model).

  tests/mini_compositor.c with MINI_SCALE gives the window a preferred
  scale, MINI_SCALE_CHANGE a second one on SIGUSR1; every committed buffer
  geometry is logged and checked against the window. Modes:
    fractional - wp_fractional_scale_v1 1.5 -> 2.0 with wp_viewporter:
                 HIGH_PIXEL_DENSITY window 320x240 renders 480x360 then
                 640x480 pixels, shown at 320x240 by the viewport
    lowdpi     - the same without HIGH_PIXEL_DENSITY: display scale 1.5 ->
                 2.0, still 320x240 pixels (compositor upscales)
    buffer     - wl_surface.preferred_buffer_scale 2 -> 3, no viewporter:
                 640x480 at buffer scale 2, then 960x720 at 3
    output     - a wl_output of scale 2, entered by the window, viewporter
    noviewport - fractional 1.5 offered but no viewporter: not used (stock),
                 the window stays at scale 1
  Pointer coordinates stay logical in all modes.

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

typedef struct Mode
{
    const char *name;
    const char *scale, *change; /* MINI_SCALE, MINI_SCALE_CHANGE */
    bool viewporter, hidpi;
    float scale1, scale2;   /* expected display scale before / after the change */
    int pw1, ph1, pw2, ph2; /* expected pixel size before / after */
    const char *geo1, *geo2; /* expected committed geometry before / after */
} Mode;

static const Mode modes[] = {
    { "fractional", "fractional:180", "240", true, true, 1.5f, 2.0f, 480, 360, 640, 480,
      "buffer=480x360 scale=1 dest=320x240", "buffer=640x480 scale=1 dest=320x240" },
    { "lowdpi", "fractional:180", "240", true, false, 1.5f, 2.0f, 320, 240, 320, 240,
      "buffer=320x240 scale=1 dest=-1x-1", "buffer=320x240 scale=1 dest=-1x-1" },
    { "buffer", "buffer:2", "3", false, true, 2.0f, 3.0f, 640, 480, 960, 720, "buffer=640x480 scale=2 dest=-1x-1",
      "buffer=960x720 scale=3 dest=-1x-1" },
    { "output", "output:2", NULL, true, true, 2.0f, 2.0f, 640, 480, 640, 480,
      "buffer=640x480 scale=1 dest=320x240", "buffer=640x480 scale=1 dest=320x240" },
    { "noviewport", "fractional:180", NULL, false, true, 1.0f, 1.0f, 320, 240, 320, 240,
      "buffer=320x240 scale=1 dest=-1x-1", "buffer=320x240 scale=1 dest=-1x-1" },
};

static bool log_has(const char *path, const char *line, bool last_only)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    char buf[256], last[256] = "";
    bool found = false;
    while (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = '\0';
        found = found || strcmp(buf, line) == 0;
        snprintf(last, sizeof(last), "%s", buf);
    }
    fclose(f);
    if (last_only && strcmp(last, line) != 0) {
        fprintf(stderr, "  last commit: '%s'\n", last);
    }
    return last_only ? strcmp(last, line) == 0 : found;
}

static void present(SDL_Window *w, int pw, int ph)
{
    SDL_Surface *s = SDL_GetWindowSurface(w);
    CHECK(s && s->w == pw && s->h == ph, "window surface %dx%d (want %dx%d)", s ? s->w : -1, s ? s->h : -1, pw, ph);
    if (s) {
        SDL_FillSurfaceRect(s, NULL, 0xff204080);
        CHECK(SDL_UpdateWindowSurface(w), "update: %s", SDL_GetError());
    }
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor <mode>\n", argv[0]);
        return 2;
    }
    const Mode *m = NULL;
    for (size_t i = 0; i < SDL_arraysize(modes); i++) {
        if (strcmp(argv[2], modes[i].name) == 0) {
            m = &modes[i];
        }
    }
    if (!m) {
        fprintf(stderr, "unknown mode %s\n", argv[2]);
        return 2;
    }

    char xdg[] = "/tmp/sdlop-mini-hidpi-XXXXXX";
    if (!mkdtemp(xdg)) {
        perror("mkdtemp");
        return 2;
    }
    char sockpath[256], logpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/wayland-mini", xdg);
    snprintf(logpath, sizeof(logpath), "%s/geometry.log", xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDL_TOUCH_MOUSE_EVENTS", "0", 1); /* the compositor's touch script would move the mouse */

    pid_t server = fork();
    if (server == 0) {
        setenv("MINI_SCALE", m->scale, 1);
        if (m->change) {
            setenv("MINI_SCALE_CHANGE", m->change, 1);
        }
        if (m->viewporter) {
            setenv("MINI_VIEWPORTER", "1", 1);
        }
        setenv("MINI_GEOMETRY_LOG", logpath, 1);
        execl(argv[1], "mini_compositor", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 200 && access(sockpath, F_OK) != 0; i++) {
        SDL_Delay(10);
    }
    if (access(sockpath, F_OK) != 0) {
        int st = 0;
        if (waitpid(server, &st, WNOHANG) == server && WIFEXITED(st) && WEXITSTATUS(st) == 77) {
            printf("test_wayland_hidpi (%s): SKIP (libwayland-server too old)\n", m->name);
            return 0;
        }
        fprintf(stderr, "mini compositor did not create %s\n", sockpath);
        kill(server, SIGKILL);
        return 2;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        kill(server, SIGKILL);
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("hidpi", 320, 240, m->hidpi ? SDL_WINDOW_HIGH_PIXEL_DENSITY : 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (!w) {
        kill(server, SIGKILL);
        return 1;
    }

    /* first scale: wait until it arrived and the window is mapped */
    int scale_events = 0, pixel_events = 0, last_pw = 0, last_ph = 0;
    Uint64 deadline = SDL_GetTicks() + 2000;
    while (SDL_GetTicks() < deadline) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                scale_events++;
            } else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                pixel_events++;
                last_pw = e.window.data1;
                last_ph = e.window.data2;
            }
        }
        if (SDL_GetWindowDisplayScale(w) == m->scale1 && SDL_GetTicks() + 1800 > deadline) {
            break; /* scale reached and the handshake had 200 ms */
        }
        SDL_Delay(5);
    }
    int ww = 0, wh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    CHECK(ww == 320 && wh == 240, "logical size %dx%d", ww, wh);
    CHECK(SDL_GetWindowDisplayScale(w) == m->scale1, "display scale %g (want %g)", SDL_GetWindowDisplayScale(w),
          m->scale1);
    CHECK(pw == m->pw1 && ph == m->ph1, "pixel size %dx%d (want %dx%d)", pw, ph, m->pw1, m->ph1);
    const float density = (float)m->pw1 / 320.0f;
    CHECK(SDL_GetWindowPixelDensity(w) == density, "pixel density %g (want %g)", SDL_GetWindowPixelDensity(w),
          density);
    if (pixel_events) {
        CHECK(last_pw == pw && last_ph == ph, "PIXEL_SIZE_CHANGED carries %dx%d", last_pw, last_ph);
    }
    present(w, m->pw1, m->ph1);

    /* the pointer entered at (100,100) surface-local: logical coordinates */
    deadline = SDL_GetTicks() + 300;
    while (SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
    float mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    CHECK(mx == 100.0f && my == 100.0f, "pointer at %g,%g (logical 100,100)", mx, my);
    CHECK(log_has(logpath, m->geo1, false), "committed '%s'", m->geo1);

    if (m->change) {
        scale_events = pixel_events = 0;
        kill(server, SIGUSR1); /* MINI_SCALE_CHANGE */
        deadline = SDL_GetTicks() + 2000;
        while (SDL_GetTicks() < deadline && SDL_GetWindowDisplayScale(w) != m->scale2) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                    scale_events++;
                } else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    pixel_events++;
                    last_pw = e.window.data1;
                    last_ph = e.window.data2;
                }
            }
            SDL_Delay(5);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                pixel_events++;
                last_pw = e.window.data1;
                last_ph = e.window.data2;
            } else if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                scale_events++;
            }
        }
        CHECK(SDL_GetWindowDisplayScale(w) == m->scale2, "display scale after change %g (want %g)",
              SDL_GetWindowDisplayScale(w), m->scale2);
        CHECK(scale_events == 1, "one DISPLAY_SCALE_CHANGED (%d)", scale_events);
        SDL_GetWindowSizeInPixels(w, &pw, &ph);
        CHECK(pw == m->pw2 && ph == m->ph2, "pixel size after change %dx%d (want %dx%d)", pw, ph, m->pw2, m->ph2);
        if (m->pw2 != m->pw1) {
            CHECK(pixel_events == 1 && last_pw == m->pw2 && last_ph == m->ph2, "PIXEL_SIZE_CHANGED %dx%d (%d events)",
                  last_pw, last_ph, pixel_events);
        } else {
            CHECK(pixel_events == 0, "no PIXEL_SIZE_CHANGED without a pixel size change (%d)", pixel_events);
        }
        SDL_GetWindowSize(w, &ww, &wh);
        CHECK(ww == 320 && wh == 240, "logical size unchanged %dx%d", ww, wh);
        present(w, m->pw2, m->ph2);
        deadline = SDL_GetTicks() + 100;
        while (SDL_GetTicks() < deadline) {
            SDL_PumpEvents();
            SDL_Delay(5);
        }
    }
    CHECK(log_has(logpath, m->geo2, true), "last commit '%s'", m->geo2);

    SDL_DestroyWindow(w);
    SDL_Quit();
    int status = 0;
    if (waitpid(server, &status, 0) != server || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: mini compositor exited with status 0x%x\n", status);
        failures++;
    }
    if (failures) {
        fprintf(stderr, "test_wayland_hidpi (%s): %d failure(s)\n", m->name, failures);
        return 1;
    }
    printf("test_wayland_hidpi (%s): PASS\n", m->name);
    return 0;
}
