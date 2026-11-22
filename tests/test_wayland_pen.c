/*
  SDLop test: Wayland pens through zwp_tablet_manager_v2.

  mini_compositor (MINI_TABLET=1) offers a pen (pressure + tilt) and an
  eraser; MINI_POINTER_CMD scripts drive them. Checked:
    - proximity in/out, motion, pressure/tilt axes, barrel button, down/up
      with stock ordering (motion before a down, after an up)
    - the emulated mouse (which = SDL_PEN_MOUSEID) and finger
      (SDL_PEN_TOUCHID) follow the touching pen
    - the eraser reports eraser=true and is a separate pen; a removed tool
      is gone (SDL_GetPenDeviceType -> INVALID)
  Mode "off" sets SDLOP_WAYLAND_NO_TABLET=1: the same input gives no pen
  events at all.

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

static int find(Uint32 type, int from)
{
    for (int i = from; i < nev; i++) {
        if (evs[i].type == type) {
            return i;
        }
    }
    return -1;
}

static int count(Uint32 type)
{
    int n = 0;
    for (int i = 0; i < nev; i++) {
        n += evs[i].type == type;
    }
    return n;
}

static int count_pen_events(void)
{
    int n = 0;
    for (int i = 0; i < nev; i++) {
        n += evs[i].type >= SDL_EVENT_PEN_PROXIMITY_IN && evs[i].type <= SDL_EVENT_PEN_AXIS;
    }
    return n;
}

static bool axis_is(SDL_PenAxis axis, float want)
{
    for (int i = 0; i < nev; i++) {
        if (evs[i].type == SDL_EVENT_PEN_AXIS && evs[i].paxis.axis == axis) {
            return fabsf(evs[i].paxis.value - want) < 0.01f;
        }
    }
    return false;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor on|off\n", argv[0]);
        return 2;
    }
    const bool on = strcmp(argv[2], "on") == 0;
    char xdg[] = "/tmp/sdlop-mini-pen-XXXXXX";
    if (!mkdtemp(xdg)) {
        perror("mkdtemp");
        return 2;
    }
    char sockpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/wayland-mini", xdg);
    snprintf(cmdpath, sizeof(cmdpath), "%s/pen.cmd", xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    if (!on) {
        setenv("SDLOP_WAYLAND_NO_TABLET", "1", 1);
    }
    server = fork();
    if (server == 0) {
        setenv("MINI_POINTER_CMD", cmdpath, 1);
        setenv("MINI_TABLET", "1", 1);
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
    SDL_Window *w = SDL_CreateWindow("pen", 320, 240, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    SDL_Surface *s = w ? SDL_GetWindowSurface(w) : NULL;
    if (s) {
        SDL_FillSurfaceRect(s, NULL, 0xff000000);
        SDL_UpdateWindowSurface(w);
    }
    run("");

    /* --- hover in ----------------------------------------------------- */
    run("tsel pen\ntprox\ntmotion 50 60\ntframe\n");
    if (!on) {
        CHECK(count_pen_events() == 0, "pen events with the tablet protocol off (%d)", count_pen_events());
        run("tpressure 30000\ntdown\ntmotion 55 60\ntframe\ntup\ntout\ntframe\n");
        CHECK(count_pen_events() == 0, "pen events with the tablet protocol off (%d)", count_pen_events());
        goto done;
    }
    int i = find(SDL_EVENT_PEN_PROXIMITY_IN, 0);
    CHECK(i >= 0, "no PEN_PROXIMITY_IN");
    const SDL_PenID pen = i >= 0 ? evs[i].pproximity.which : 0;
    CHECK(pen != 0 && SDL_GetPenDeviceType(pen) == SDL_PEN_DEVICE_TYPE_UNKNOWN, "pen %u type %d", (unsigned)pen,
          (int)SDL_GetPenDeviceType(pen));
    i = find(SDL_EVENT_PEN_MOTION, 0);
    CHECK(i >= 0 && evs[i].pmotion.x == 50.0f && evs[i].pmotion.y == 60.0f && evs[i].pmotion.which == pen,
          "hover motion");
    CHECK(i >= 0 && (evs[i].pmotion.pen_state & SDL_PEN_INPUT_IN_PROXIMITY) && !(evs[i].pmotion.pen_state & SDL_PEN_INPUT_DOWN),
          "hover pen_state %x", i >= 0 ? evs[i].pmotion.pen_state : 0);
    i = find(SDL_EVENT_MOUSE_MOTION, 0);
    CHECK(i >= 0 && evs[i].motion.which == SDL_PEN_MOUSEID && evs[i].motion.x == 50.0f, "hover moves the pen mouse");
    CHECK(count(SDL_EVENT_MOUSE_BUTTON_DOWN) == 0 && count(SDL_EVENT_FINGER_DOWN) == 0, "hovering clicks");

    /* --- touch down with pressure, motion first ----------------------- */
    run("tpressure 32768\ntdown\ntmotion 55 60\ntframe\n");
    const int m = find(SDL_EVENT_PEN_MOTION, 0), d = find(SDL_EVENT_PEN_DOWN, 0);
    CHECK(m >= 0 && d > m, "motion %d must precede down %d", m, d);
    CHECK(d >= 0 && !evs[d].ptouch.eraser && evs[d].ptouch.x == 55.0f && evs[d].ptouch.which == pen, "pen down");
    CHECK(axis_is(SDL_PEN_AXIS_PRESSURE, 32768.0f / 65535.0f), "pressure axis");
    i = find(SDL_EVENT_MOUSE_BUTTON_DOWN, 0);
    CHECK(i >= 0 && evs[i].button.which == SDL_PEN_MOUSEID && evs[i].button.button == SDL_BUTTON_LEFT,
          "pen mouse left down");
    i = find(SDL_EVENT_FINGER_DOWN, 0);
    CHECK(i >= 0 && evs[i].tfinger.touchID == SDL_PEN_TOUCHID, "pen finger down");

    /* --- tilt and a barrel button while down -------------------------- */
    run("ttilt 10 -20\ntframe\ntbutton 331 1\ntframe\n");
    CHECK(axis_is(SDL_PEN_AXIS_XTILT, 10.0f), "xtilt");
    CHECK(axis_is(SDL_PEN_AXIS_YTILT, -20.0f), "ytilt");
    i = find(SDL_EVENT_PEN_BUTTON_DOWN, 0);
    CHECK(i >= 0 && evs[i].pbutton.button == 1 && evs[i].pbutton.which == pen, "BTN_STYLUS -> pen button 1");
    run("tbutton 331 0\ntframe\n");
    CHECK(find(SDL_EVENT_PEN_BUTTON_UP, 0) >= 0, "pen button up");

    /* --- lift: up before the motion ----------------------------------- */
    run("tup\ntmotion 60 60\ntframe\n");
    const int u = find(SDL_EVENT_PEN_UP, 0), m2 = find(SDL_EVENT_PEN_MOTION, 0);
    CHECK(u >= 0 && m2 > u, "up %d must precede motion %d", u, m2);
    i = find(SDL_EVENT_MOUSE_BUTTON_UP, 0);
    CHECK(i >= 0 && evs[i].button.which == SDL_PEN_MOUSEID && evs[i].button.button == SDL_BUTTON_LEFT,
          "pen mouse left up");
    CHECK(find(SDL_EVENT_FINGER_UP, 0) >= 0, "pen finger up");

    /* --- out ---------------------------------------------------------- */
    run("tout\ntframe\n");
    i = find(SDL_EVENT_PEN_PROXIMITY_OUT, 0);
    CHECK(i >= 0 && evs[i].pproximity.which == pen, "proximity out");

    /* --- the eraser --------------------------------------------------- */
    run("tsel eraser\ntprox\ntmotion 10 10\ntdown\ntframe\n");
    i = find(SDL_EVENT_PEN_DOWN, 0);
    const SDL_PenID eraser = i >= 0 ? evs[i].ptouch.which : 0;
    CHECK(i >= 0 && evs[i].ptouch.eraser && (evs[i].ptouch.pen_state & SDL_PEN_INPUT_ERASER_TIP), "eraser down");
    CHECK(eraser != 0 && eraser != pen, "eraser is its own pen (%u vs %u)", (unsigned)eraser, (unsigned)pen);
    run("tup\ntframe\ntremove\n");
    CHECK(find(SDL_EVENT_PEN_PROXIMITY_OUT, 0) >= 0, "removing an in-proximity tool sends proximity out");
    CHECK(SDL_GetPenDeviceType(eraser) == SDL_PEN_DEVICE_TYPE_INVALID, "removed eraser still known");
    CHECK(SDL_GetPenDeviceType(pen) == SDL_PEN_DEVICE_TYPE_UNKNOWN, "pen lost with the eraser");

done:
    SDL_DestroyWindow(w);
    SDL_Quit();
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    if (failures) {
        fprintf(stderr, "test_wayland_pen (%s): %d failure(s)\n", argv[2], failures);
        return 1;
    }
    printf("test_wayland_pen (%s): OK\n", argv[2]);
    return 0;
}
