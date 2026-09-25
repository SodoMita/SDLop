/*
  SDLop test: pointer-constraints / relative-pointer integration, verified
  against a purpose-built mini compositor (tests/mini_compositor.c) that
  implements wl_seat, zwp_pointer_constraints_v1 and
  zwp_relative_pointer_manager_v1 and scripts the events:

    1. pointer enter at (100,100) once the window is mapped
    2. on lock_pointer: locked event, then relative_motion(10.5, -3.25)
    3. after the client unlocks: absolute motion to (600,450)

  The client asserts SDL semantics at each step.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                    \
            fprintf(stderr, "\n");                           \
            failures++;                                      \
        }                                                    \
    } while (0)

/* pump until an event matching a predicate shows up, or timeout */
typedef bool (*event_pred)(const SDL_Event *e, void *user);

static bool pump_until(event_pred pred, void *user, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (pred(&e, user)) {
                return true;
            }
        }
        SDL_Delay(2);
    }
    return false;
}

static bool pred_mouse_enter(const SDL_Event *e, void *user)
{
    (void)user;
    return e->type == SDL_EVENT_WINDOW_MOUSE_ENTER;
}

typedef struct
{
    float want_xrel, want_yrel;
    float want_x, want_y;
    bool match_rel;
    bool match_pos;
    bool matched;
} motion_expect;

static bool pred_motion(const SDL_Event *e, void *user)
{
    motion_expect *m = (motion_expect *)user;
    if (e->type != SDL_EVENT_MOUSE_MOTION) {
        return false;
    }
    bool rel_ok = !m->match_rel ||
                  (fabsf(e->motion.xrel - m->want_xrel) < 0.001f &&
                   fabsf(e->motion.yrel - m->want_yrel) < 0.001f);
    bool pos_ok = !m->match_pos ||
                  (fabsf(e->motion.x - m->want_x) < 0.001f && fabsf(e->motion.y - m->want_y) < 0.001f);
    if (rel_ok && pos_ok) {
        m->matched = true;
        return true;
    }
    return false;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_pointer_lock <mini_compositor path>\n");
        return 2;
    }

    /* private runtime dir + socket name for the mini compositor */
    char xdg[] = "/tmp/sdlop-mini-XXXXXX";
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
        execl(argv[1], "mini_compositor", (char *)NULL);
        _exit(127);
    }
    if (server < 0) {
        perror("fork");
        return 2;
    }

    /* wait for the socket */
    for (int i = 0; i < 200; i++) {
        if (access(sockpath, F_OK) == 0) {
            break;
        }
        SDL_Delay(10);
    }
    if (access(sockpath, F_OK) != 0) {
        fprintf(stderr, "mini compositor did not create %s\n", sockpath);
        kill(server, SIGKILL);
        return 2;
    }

    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        kill(server, SIGKILL);
        return 1;
    }

    SDL_Window *w = SDL_CreateWindow("lock test", 320, 240, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (w) {
        /* map the window: surface + commit makes the compositor send the
         * initial configure and then the pointer enter */
        SDL_Surface *s = SDL_GetWindowSurface(w);
        CHECK(s != NULL, "window surface: %s", SDL_GetError());
        if (s) {
            CHECK(SDL_FillSurfaceRect(s, NULL, 0x00204060), "fill");
            CHECK(SDL_UpdateWindowSurface(w), "update: %s", SDL_GetError());
        }

        /* step 1: enter */
        CHECK(pump_until(pred_mouse_enter, NULL, 3000), "no WINDOW_MOUSE_ENTER");
        float mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        CHECK(fabsf(mx - 100.0f) < 0.01f && fabsf(my - 100.0f) < 0.01f,
              "enter position (%f,%f) != (100,100)", mx, my);

        /* step 2: arm the lock */
        CHECK(SDL_SetWindowRelativeMouseMode(w, true),
              "relative mode on: %s", SDL_GetError());

        /* the mini compositor answers with locked + relative_motion(10.5,-3.25);
         * stock SDL3 accumulates relative motion into the reported position */
        motion_expect rel = { 10.5f, -3.25f, 110.5f, 96.75f, true, true, false };
        CHECK(pump_until(pred_motion, &rel, 3000),
              "no relative MOUSE_MOTION (xrel=10.5, yrel=-3.25, pos 110.5,96.75)");

        /* relative state reflects the accumulated deltas */
        float rx = 0, ry = 0;
        SDL_GetRelativeMouseState(&rx, &ry);
        CHECK(fabsf(rx - 10.5f) < 0.01f && fabsf(ry + 3.25f) < 0.01f,
              "relative state (%f,%f) != (10.5,-3.25)", rx, ry);

        /* stock accumulates relative motion into the absolute position too */
        SDL_GetMouseState(&mx, &my);
        CHECK(fabsf(mx - 110.5f) < 0.01f && fabsf(my - 96.75f) < 0.01f,
              "position after relative motion (%f,%f) != (110.5,96.75)", mx, my);

        /* step 3: unlock; the compositor then sends absolute motion */
        CHECK(SDL_SetWindowRelativeMouseMode(w, false),
              "relative mode off: %s", SDL_GetError());
        /* the rig injects (600,450), far outside the 320x240 window: stock
         * clamps the reported position into the window (w-1, h-1) */
        motion_expect abs_ = { 0.0f, 0.0f, 319.0f, 239.0f, false, true, false };
        CHECK(pump_until(pred_motion, &abs_, 3000),
              "no absolute MOUSE_MOTION clamped to (319,239) after unlock");

        SDL_DestroyWindowSurface(w);
        SDL_DestroyWindow(w);
    }

    SDL_Quit();

    /* the compositor exits on client disconnect */
    int status = 0;
    for (int i = 0; i < 200; i++) {
        if (waitpid(server, &status, WNOHANG) == server) {
            break;
        }
        SDL_Delay(10);
    }
    if (!WIFEXITED(status)) {
        kill(server, SIGKILL);
        waitpid(server, &status, 0);
    }

    if (failures) {
        printf("test_pointer_lock: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_pointer_lock: PASS\n");
    return 0;
}
