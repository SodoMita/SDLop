/*
  SDLop test: Wayland toplevel extras (stock SDL3).

  tests/mini_compositor.c with MINI_TOPLEVEL_LOG offers
  xdg_toplevel_icon_manager_v1 and xdg_wm_dialog_v1 and logs what is set
  (mode "all"; "none" offers neither):
    - SDL_SetWindowIcon: one square buffer per integer scale of the icon's
      alternate images (smaller ones skipped), non-square images centred on
      a transparent square, premultiplied ARGB8888
    - the icon is set again on the new toplevel of a hidden + shown window
    - modal windows (SDL_WINDOW_MODAL, SDL_SetWindowModal) get an
      xdg_dialog_v1 with set_modal / unset_modal, a fresh one when re-shown
    - SDL_SetWindowOpacity: wp_alpha_modifier_surface_v1 multiplier,
      committed right away
    - without the protocols SDL_SetWindowIcon / SDL_SetWindowOpacity fail
      cleanly and SDL_SetWindowModal still succeeds

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
    char buf[640];
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

static bool wait_count(const char *prefix, int want, char *last, size_t last_size)
{
    const Uint64 end = SDL_GetTicks() + 2000;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        if (count_lines(prefix, last, last_size) >= want) {
            return true;
        }
        SDL_Delay(5);
    }
    return false;
}

/* pump until the last line starting with prefix is want */
static bool wait_last(const char *prefix, const char *want)
{
    char last[640];
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

static void pump_for(Uint32 ms)
{
    const Uint64 end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
}

static SDL_Surface *solid(int w, int h, Uint32 argb)
{
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    if (s) {
        SDL_FillSurfaceRect(s, NULL, argb);
    }
    return s;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor all|none\n", argv[0]);
        return 2;
    }
    const char *mode = argv[2];
    const bool icons = strcmp(mode, "all") == 0;

    char xdg[] = "/tmp/sdlop-mini-toplevel-XXXXXX";
    if (!mkdtemp(xdg)) {
        perror("mkdtemp");
        return 2;
    }
    char sockpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/wayland-mini", xdg);
    snprintf(logpath, sizeof(logpath), "%s/toplevel.log", xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDL_TOUCH_MOUSE_EVENTS", "0", 1);

    pid_t server = fork();
    if (server == 0) {
        if (icons) {
            setenv("MINI_TOPLEVEL_LOG", logpath, 1);
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
    SDL_Window *w = SDL_CreateWindow("toplevel", 320, 240, 0);
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
    pump_for(100);

    /* 32x16 half-transparent red, 2x green, a too-small blue (skipped) */
    SDL_Surface *icon = solid(32, 16, 0x80ff0000);
    SDL_Surface *big = solid(64, 32, 0xff00ff00);
    SDL_Surface *small = solid(16, 8, 0xff0000ff);
    CHECK(icon && big && small && SDL_AddSurfaceAlternateImage(icon, big) && SDL_AddSurfaceAlternateImage(icon, small),
          "icon surfaces");
    SDL_DestroySurface(big);
    SDL_DestroySurface(small);
    const bool set = SDL_SetWindowIcon(w, icon);
    if (icons) {
        const char *want = "icon 32x32@1:80800000/00000000 64x64@2:ff00ff00/00000000";
        char last[640];
        CHECK(set, "SDL_SetWindowIcon: %s", SDL_GetError());
        CHECK(wait_count("icon", 1, last, sizeof(last)), "icon set");
        CHECK(strcmp(last, want) == 0, "icon '%s' (want '%s')", last, want);

        /* hide + show: a new toplevel gets the same icon */
        SDL_HideWindow(w);
        pump_for(100);
        SDL_ShowWindow(w);
        CHECK(wait_count("icon", 2, last, sizeof(last)), "icon set again on the re-shown toplevel");
        CHECK(strcmp(last, want) == 0, "re-shown icon '%s'", last);

        /* a new icon replaces it */
        SDL_Surface *blue = solid(24, 24, 0xff0000ff);
        CHECK(blue && SDL_SetWindowIcon(w, blue), "second icon");
        CHECK(wait_count("icon", 3, last, sizeof(last)), "second icon set");
        CHECK(strcmp(last, "icon 24x24@1:ff0000ff/ff0000ff") == 0, "second icon '%s'", last);
        SDL_DestroySurface(blue);
    } else {
        CHECK(!set, "SDL_SetWindowIcon fails without xdg_toplevel_icon_manager_v1");
        pump_for(50);
    }
    SDL_DestroySurface(icon);

    /* opacity */
    const bool op = SDL_SetWindowOpacity(w, 0.5f);
    if (icons) {
        CHECK(op, "SDL_SetWindowOpacity: %s", SDL_GetError());
        CHECK(wait_last("alpha=", "alpha=2147483647"), "half opacity committed");
        CHECK(SDL_SetWindowOpacity(w, 1.0f) && wait_last("alpha=", "alpha=4294967295"), "full opacity");
        CHECK(SDL_GetWindowOpacity(w) == 1.0f, "opacity reads back");
    } else {
        CHECK(!op, "SDL_SetWindowOpacity fails without wp_alpha_modifier_v1");
        CHECK(SDL_GetWindowOpacity(w) == 1.0f, "opacity unchanged (%g)", SDL_GetWindowOpacity(w));
    }

    /* modal dialogs */
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_PARENT_POINTER, w);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_MODAL_BOOLEAN, true);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 100);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 80);
    SDL_Window *dlg = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    CHECK(dlg && (SDL_GetWindowFlags(dlg) & SDL_WINDOW_MODAL), "modal window: %s", SDL_GetError());
    if (dlg) {
        if (icons) {
            CHECK(wait_count("dialog", 1, NULL, 0) && wait_count("modal=1", 1, NULL, 0),
                  "modal window gets an xdg_dialog_v1 + set_modal");
        }
        CHECK(SDL_SetWindowModal(dlg, false), "unset modal: %s", SDL_GetError());
        if (icons) {
            CHECK(wait_last("modal=", "modal=0"), "unset_modal");
        }
        CHECK(SDL_SetWindowModal(dlg, true), "set modal again");
        if (icons) {
            CHECK(wait_last("modal=", "modal=1"), "set_modal");
            CHECK(count_lines("dialog", NULL, 0) == 1, "one xdg_dialog_v1 per toplevel");
        }
        SDL_HideWindow(dlg);
        if (icons) {
            CHECK(wait_count("dialog destroyed", 1, NULL, 0), "hidden: the dialog goes with the toplevel");
        }
        SDL_ShowWindow(dlg);
        if (icons) {
            /* "dialog" also prefixes "dialog destroyed" */
            CHECK(wait_count("dialog", 3, NULL, 0), "re-shown: a new xdg_dialog_v1");
            CHECK(wait_last("dialog", "dialog") && wait_last("modal=", "modal=1"), "re-shown modal");
        }
        SDL_DestroyWindow(dlg);
        if (icons) {
            CHECK(wait_count("dialog destroyed", 2, NULL, 0), "destroyed with the window");
        }
    }

    SDL_DestroyWindow(w);
    SDL_Quit();
    int status = 0;
    waitpid(server, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "compositor exit status %d", status);
    unlink(logpath);
    rmdir(xdg);
    if (failures) {
        fprintf(stderr, "test_wayland_toplevel (%s): %d failure(s)\n", mode, failures);
        return 1;
    }
    printf("test_wayland_toplevel (%s): OK\n", mode);
    return 0;
}
