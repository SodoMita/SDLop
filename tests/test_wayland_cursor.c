/*
  SDLop test: Wayland cursors through wp_cursor_shape_v1 (stock SDL3).

  tests/mini_compositor.c with MINI_CURSOR_LOG logs every cursor request.
  Modes:
    shape   - the compositor offers wp_cursor_shape_manager_v1: system
              cursors are set by shape (default 1, pointer 4, move ->
              all_scroll 32), custom cursors and hiding still use
              wl_pointer.set_cursor
    noshape - no cursor-shape manager: system cursors are surfaces
    theme   - offered, but SDLOP_WAYLAND_NO_CURSOR_SHAPE=1: surfaces only

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
        if (!prefix || strncmp(buf, prefix, strlen(prefix)) == 0) {
            n++;
        }
        if (last) {
            snprintf(last, last_size, "%s", buf);
        }
    }
    fclose(f);
    return n;
}

/* pump until the latest cursor request is `want` */
static bool wait_last(const char *want)
{
    char last[128];
    const Uint64 end = SDL_GetTicks() + 2000;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        count_lines(NULL, last, sizeof(last));
        if (strcmp(last, want) == 0) {
            return true;
        }
        SDL_Delay(5);
    }
    fprintf(stderr, "  last cursor request '%s', want '%s'\n", last, want);
    return false;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor shape|noshape|theme\n", argv[0]);
        return 2;
    }
    const char *mode = argv[2];
    const bool shapes = strcmp(mode, "shape") == 0;

    char xdg[] = "/tmp/sdlop-mini-cursor-XXXXXX";
    if (!mkdtemp(xdg)) {
        perror("mkdtemp");
        return 2;
    }
    char sockpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/wayland-mini", xdg);
    snprintf(logpath, sizeof(logpath), "%s/cursor.log", xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDL_TOUCH_MOUSE_EVENTS", "0", 1);
    if (strcmp(mode, "theme") == 0) {
        setenv("SDLOP_WAYLAND_NO_CURSOR_SHAPE", "1", 1);
    }

    pid_t server = fork();
    if (server == 0) {
        setenv("MINI_CURSOR_LOG", logpath, 1);
        if (strcmp(mode, "noshape") == 0) {
            setenv("MINI_CURSOR_SHAPE", "0", 1);
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
    SDL_Window *w = SDL_CreateWindow("cursor", 320, 240, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (!w) {
        kill(server, SIGKILL);
        return 1;
    }
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

    /* the default cursor on enter */
    CHECK(wait_last(shapes ? "shape=1" : "cursor=surface"), "default cursor on enter");

    SDL_Cursor *hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    CHECK(hand && SDL_SetCursor(hand), "set pointer cursor: %s", SDL_GetError());
    CHECK(wait_last(shapes ? "shape=4" : "cursor=surface"), "pointer cursor");

    SDL_Cursor *move = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
    CHECK(move && SDL_SetCursor(move), "set move cursor");
    CHECK(wait_last(shapes ? "shape=32" : "cursor=surface"), "move cursor -> all_scroll (stock)");

    /* custom cursors are always surfaces */
    SDL_Surface *img = SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_ARGB8888);
    SDL_FillSurfaceRect(img, NULL, 0xffff0000);
    SDL_Cursor *custom = SDL_CreateColorCursor(img, 1, 1);
    SDL_DestroySurface(img);
    CHECK(custom && SDL_SetCursor(custom), "set custom cursor");
    CHECK(wait_last("cursor=surface"), "custom cursor is a surface");

    CHECK(SDL_HideCursor(), "hide");
    CHECK(wait_last("cursor=null"), "hidden cursor");
    CHECK(SDL_SetCursor(hand) && SDL_ShowCursor(), "show again");
    CHECK(wait_last(shapes ? "shape=4" : "cursor=surface"), "shown again");

    if (!shapes) {
        CHECK(count_lines("shape=", NULL, 0) == 0, "no shape requests in mode %s", mode);
    }

    SDL_DestroyCursor(hand);
    SDL_DestroyCursor(move);
    SDL_DestroyCursor(custom);
    SDL_DestroyWindow(w);
    SDL_Quit();
    int status = 0;
    waitpid(server, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "compositor exit status %d", status);
    unlink(logpath);
    rmdir(xdg);
    if (failures) {
        fprintf(stderr, "test_wayland_cursor (%s): %d failure(s)\n", mode, failures);
        return 1;
    }
    printf("test_wayland_cursor (%s): OK\n", mode);
    return 0;
}
