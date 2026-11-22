/*
  SDLop test: touchpad pinch gestures on Wayland (zwp_pointer_gestures_v1).

  mini_compositor (MINI_GESTURES=<version>) offers the gestures global and
  MINI_POINTER_CMD scripts drive the pointer's pinch object. Checked:
    - begin on our surface -> SDL_EVENT_PINCH_BEGIN (scale 0, windowID)
    - updates -> SDL_EVENT_PINCH_UPDATE with the protocol's scale
    - end and cancelled end -> SDL_EVENT_PINCH_END; updates after the end
      are dropped
    - SDL_SetEventEnabled(SDL_EVENT_PINCH_UPDATE, false) filters updates
  Modes: "v3" and "v1" (gestures global version), "off" sets
  SDLOP_WAYLAND_NO_POINTER_GESTURES=1: no pinch events at all.

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
static SDL_Event evs[256];
static int nev;

/* run tablet input in the compositor, collect what it caused */
static void run(const char *cmds)
{
    FILE *f = fopen(cmdpath, "w");
    if (f) {
        fputs(cmds, f);
        fclose(f);
    }
    kill(server, SIGUSR2);
    nev = 0;
    const Uint64 end = SDL_GetTicks() + 250;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (nev < (int)SDL_arraysize(evs)) {
                evs[nev++] = ev;
            }
        }
        SDL_Delay(5);
    }
}

static int count_pinch(void)
{
    int n = 0;
    for (int i = 0; i < nev; i++) {
        n += evs[i].type >= SDL_EVENT_PINCH_BEGIN && evs[i].type <= SDL_EVENT_PINCH_END;
    }
    return n;
}

static const SDL_Event *nth_pinch(int k)
{
    for (int i = 0; i < nev; i++) {
        if (evs[i].type >= SDL_EVENT_PINCH_BEGIN && evs[i].type <= SDL_EVENT_PINCH_END && k-- == 0) {
            return &evs[i];
        }
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor v3|v1|off\n", argv[0]);
        return 2;
    }
    const bool on = strcmp(argv[2], "off") != 0;
    char xdg[] = "/tmp/sdlop-mini-pinch-XXXXXX";
    if (!mkdtemp(xdg)) {
        perror("mkdtemp");
        return 2;
    }
    char sockpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/wayland-mini", xdg);
    snprintf(cmdpath, sizeof(cmdpath), "%s/pinch.cmd", xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    if (!on) {
        setenv("SDLOP_WAYLAND_NO_POINTER_GESTURES", "1", 1);
    }
    server = fork();
    if (server == 0) {
        setenv("MINI_POINTER_CMD", cmdpath, 1);
        setenv("MINI_GESTURES", strcmp(argv[2], "v1") == 0 ? "1" : "3", 1);
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
    SDL_Window *w = SDL_CreateWindow("pinch", 320, 240, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    SDL_Surface *s = w ? SDL_GetWindowSurface(w) : NULL;
    if (s) {
        SDL_FillSurfaceRect(s, NULL, 0xff000000);
        SDL_UpdateWindowSurface(w);
    }
    run("");
    const SDL_WindowID wid = w ? SDL_GetWindowID(w) : 0;

    run("pinch_begin 2\npinch_update 1.25\npinch_update 1.5\npinch_end\n");
    if (!on) {
        CHECK(count_pinch() == 0, "pinch events with gestures off (%d)", count_pinch());
        goto done;
    }
    CHECK(count_pinch() == 4, "begin, 2 updates, end: got %d pinch events", count_pinch());
    const SDL_Event *e = nth_pinch(0);
    CHECK(e && e->type == SDL_EVENT_PINCH_BEGIN && e->pinch.scale == 0.0f && e->pinch.windowID == wid, "PINCH_BEGIN");
    e = nth_pinch(1);
    CHECK(e && e->type == SDL_EVENT_PINCH_UPDATE && fabsf(e->pinch.scale - 1.25f) < 0.01f && e->pinch.windowID == wid,
          "PINCH_UPDATE 1.25 (%g)", e ? e->pinch.scale : -1.0);
    e = nth_pinch(2);
    CHECK(e && e->type == SDL_EVENT_PINCH_UPDATE && fabsf(e->pinch.scale - 1.5f) < 0.01f, "PINCH_UPDATE 1.5");
    e = nth_pinch(3);
    CHECK(e && e->type == SDL_EVENT_PINCH_END && e->pinch.windowID == wid, "PINCH_END");

    /* updates after the end have no focus */
    run("pinch_update 2.0\n");
    CHECK(count_pinch() == 0, "update without an active pinch: %d events", count_pinch());

    /* cancelled gesture still ends; disabled updates are filtered */
    SDL_SetEventEnabled(SDL_EVENT_PINCH_UPDATE, false);
    run("pinch_begin 2\npinch_update 0.5\npinch_end 1\n");
    CHECK(count_pinch() == 2 && nth_pinch(0)->type == SDL_EVENT_PINCH_BEGIN &&
              nth_pinch(1)->type == SDL_EVENT_PINCH_END,
          "cancelled pinch with updates disabled: %d events", count_pinch());
    SDL_SetEventEnabled(SDL_EVENT_PINCH_UPDATE, true);

done:
    SDL_DestroyWindow(w);
    SDL_Quit();
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    if (failures) {
        fprintf(stderr, "test_wayland_pinch (%s): %d failure(s)\n", argv[2], failures);
        return 1;
    }
    printf("test_wayland_pinch (%s): OK\n", argv[2]);
    return 0;
}
