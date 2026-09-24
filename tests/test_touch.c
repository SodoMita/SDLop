/*
  SDLop test: wl_touch integration, verified against tests/mini_compositor.c
  which scripts the sequence on window map:

    touch down  id=7 at (50,60)   -> SDL_EVENT_FINGER_DOWN
    touch motion id=7 at (70,80)  -> SDL_EVENT_FINGER_MOTION (dx=20, dy=20)
    touch up    id=7              -> SDL_EVENT_FINGER_UP

  Coordinates are normalized by the window size (320x240 here), matching
  SDL3 semantics (x/y/dx/dy in 0...1, pressure 1.0 while down).

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
            fprintf(stderr, "\n");                                \
            failures++;                                          \
        }                                                        \
    } while (0)

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

typedef struct
{
    Uint32 type;
    float x, y, dx, dy, pressure;
    SDL_Event last;
} finger_expect;

static bool pred_finger(const SDL_Event *e, void *user)
{
    finger_expect *f = (finger_expect *)user;
    if (e->type != f->type) {
        return false;
    }
    f->last = *e;
    return fabsf(e->tfinger.x - f->x) < 0.001f &&
           fabsf(e->tfinger.y - f->y) < 0.001f &&
           fabsf(e->tfinger.dx - f->dx) < 0.001f &&
           fabsf(e->tfinger.dy - f->dy) < 0.001f &&
           fabsf(e->tfinger.pressure - f->pressure) < 0.001f;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor\n", argv[0]);
        return 2;
    }

    char xdg[] = "/tmp/sdlop-mini-touch-XXXXXX";
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

    SDL_Window *w = SDL_CreateWindow("touch test", 320, 240, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (w) {
        SDL_Surface *s = SDL_GetWindowSurface(w);
        CHECK(s != NULL, "window surface: %s", SDL_GetError());
        if (s) {
            SDL_FillSurfaceRect(s, NULL, 0x00204060);
            SDL_UpdateWindowSurface(w);
        }

        /* down at (50,60)/320x240 */
        finger_expect down = { SDL_EVENT_FINGER_DOWN, 50.0f / 320, 60.0f / 240, 0, 0, 1.0f, { 0 } };
        CHECK(pump_until(pred_finger, &down, 3000), "no FINGER_DOWN at (0.15625,0.25)");
        CHECK(down.last.tfinger.fingerID == 7, "fingerID %llu != 7",
              (unsigned long long)down.last.tfinger.fingerID);
        CHECK(down.last.tfinger.touchID != 0, "touchID is 0");
        CHECK(down.last.tfinger.windowID == SDL_GetWindowID(w), "windowID mismatch");

        /* second finger goes down while finger 7 is still active
         * (checks must follow wire order: pump_until consumes what it skips) */
        finger_expect down2 = { SDL_EVENT_FINGER_DOWN, 100.0f / 320, 120.0f / 240, 0, 0, 1.0f, { 0 } };
        CHECK(pump_until(pred_finger, &down2, 3000), "no FINGER_DOWN (finger 8) at (100,120)");
        CHECK(down2.last.tfinger.fingerID == 8, "second fingerID != 8");

        /* finger 7 moves while finger 8 is tracked independently:
         * dx=20/320, dy=20/240 */
        finger_expect motion = { SDL_EVENT_FINGER_MOTION, 70.0f / 320, 80.0f / 240,
                                 20.0f / 320, 20.0f / 240, 1.0f, { 0 } };
        CHECK(pump_until(pred_finger, &motion, 3000), "no FINGER_MOTION (finger 7) to (70,80) with dx=20,dy=20");
        CHECK(motion.last.tfinger.fingerID == 7, "motion fingerID != 7 (tracking by id broken)");

        /* finger 8 motion: dx=40/320, dy=40/240 */
        finger_expect motion2 = { SDL_EVENT_FINGER_MOTION, 140.0f / 320, 160.0f / 240,
                                  40.0f / 320, 40.0f / 240, 1.0f, { 0 } };
        CHECK(pump_until(pred_finger, &motion2, 3000), "no FINGER_MOTION (finger 8) to (140,160)");

        /* up at the last known position, pressure 0 */
        finger_expect up = { SDL_EVENT_FINGER_UP, 70.0f / 320, 80.0f / 240, 0, 0, 0.0f, { 0 } };
        CHECK(pump_until(pred_finger, &up, 3000), "no FINGER_UP at (70,80)");
        CHECK(up.last.tfinger.fingerID == 7, "up fingerID != 7");

        /* the compositor cancels the sequence: finger 8 gets FINGER_CANCELED
         * at its last known position, pressure 0 */
        finger_expect cancel = { SDL_EVENT_FINGER_CANCELED, 140.0f / 320, 160.0f / 240, 0, 0, 0.0f, { 0 } };
        CHECK(pump_until(pred_finger, &cancel, 3000), "no FINGER_CANCELED (finger 8)");
        CHECK(cancel.last.tfinger.fingerID == 8, "cancel fingerID != 8");

        SDL_DestroyWindowSurface(w);
        SDL_DestroyWindow(w);
    }

    SDL_Quit();

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
        printf("test_touch: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_touch: PASS\n");
    return 0;
}
