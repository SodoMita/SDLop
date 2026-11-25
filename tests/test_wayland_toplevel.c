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
      committed right away; the opaque region is dropped while translucent
    - SDL_WINDOW_TRANSPARENT: ARGB8888 surface and shm buffers, no opaque
      region, cleared fully transparent until SDLop_SetWindowClearColor,
      SDL_GL_ALPHA_SIZE 8 when it was 0; opaque windows keep XRGB8888 and
      an opaque region
    - SDL_SetWindowHitTest: a left press on a draggable / resize area
      starts xdg_toplevel move / resize with the press serial and is not
      passed on, resize areas show resize cursors (cursor-shape), normal
      areas behave normally (pointer input via MINI_POINTER_CMD)
    - SDL_ShowWindowSystemMenu: xdg_toplevel show_window_menu
    - the compositor's close button (MINI_POINTER_CMD "close") sends
      xdg_toplevel.close, which SDLop turns into SDL_EVENT_WINDOW_CLOSE_REQUESTED
    - xdg-foreign: toplevels are exported, the handle is
      SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_EXPORT_HANDLE_STRING, dropped
      when hidden, a new one when shown again
    - without the protocols SDL_SetWindowIcon / SDL_SetWindowOpacity fail
      cleanly, SDL_SetWindowModal still succeeds and there is no handle

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
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
static char cursorlog[256];
static pid_t server;

/* number of log lines starting with prefix; the last one in last */
static int count_lines_in(const char *path, const char *prefix, char *last, size_t last_size)
{
    FILE *f = fopen(path, "r");
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

static int count_lines(const char *prefix, char *last, size_t last_size)
{
    return count_lines_in(logpath, prefix, last, last_size);
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

/* pump until the last line of path starting with prefix is want */
static bool wait_last_in(const char *path, const char *prefix, const char *want)
{
    char last[640];
    const Uint64 end = SDL_GetTicks() + 2000;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        count_lines_in(path, prefix, last, sizeof(last));
        if (strcmp(last, want) == 0) {
            return true;
        }
        SDL_Delay(5);
    }
    fprintf(stderr, "  last '%s' line: '%s', want '%s'\n", prefix, last, want);
    return false;
}

static bool wait_last(const char *prefix, const char *want)
{
    return wait_last_in(logpath, prefix, want);
}

/* pump until the window's xdg-foreign export handle is want (NULL: none) */
static bool wait_handle(SDL_Window *w, const char *want)
{
    const Uint64 end = SDL_GetTicks() + 2000;
    const char *h = NULL;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        h = SDL_GetStringProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_EXPORT_HANDLE_STRING,
                                  NULL);
        if (want ? (h && strcmp(h, want) == 0) : !h) {
            return true;
        }
        SDL_Delay(5);
    }
    fprintf(stderr, "  export handle '%s', want '%s'\n", h ? h : "(null)", want ? want : "(null)");
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

/* run pointer input in the compositor (MINI_POINTER_CMD), then collect
 * the events it caused */
static void pointer_cmd(const char *cmds, int *downs, int *ups)
{
    FILE *f = fopen(cmdpath, "w");
    if (f) {
        fputs(cmds, f);
        fclose(f);
    }
    kill(server, SIGUSR2);
    *downs = *ups = 0;
    const Uint64 end = SDL_GetTicks() + 200;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                (*downs)++;
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                (*ups)++;
            }
        }
        SDL_Delay(5);
    }
}

static SDL_HitTestResult hit_test(SDL_Window *win, const SDL_Point *p, void *data)
{
    (void)win;
    (void)data;
    if (p->y < 20) {
        return SDL_HITTEST_DRAGGABLE;
    }
    if (p->x >= 300 && p->y >= 220) {
        return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    }
    return SDL_HITTEST_NORMAL;
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
    snprintf(cmdpath, sizeof(cmdpath), "%s/pointer.cmd", xdg);
    snprintf(cursorlog, sizeof(cursorlog), "%s/cursor.log", xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDL_TOUCH_MOUSE_EVENTS", "0", 1);

    server = fork();
    if (server == 0) {
        setenv("MINI_POINTER_CMD", cmdpath, 1);
        if (icons) {
            setenv("MINI_TOPLEVEL_LOG", logpath, 1);
            setenv("MINI_CURSOR_LOG", cursorlog, 1);
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
        CHECK(s->format == SDL_PIXELFORMAT_XRGB8888, "opaque window surface: %s", SDL_GetPixelFormatName(s->format));
        SDL_FillSurfaceRect(s, NULL, 0xff000000);
        SDL_UpdateWindowSurface(w);
    }
    pump_for(100);
    CHECK(wait_handle(w, icons ? "mini-1" : NULL), "main window export handle");

    /* the compositor's close button -> xdg_toplevel.close ->
     * SDL_EVENT_WINDOW_CLOSE_REQUESTED (same event the other backends send) */
    {
        FILE *f = fopen(cmdpath, "w");
        CHECK(f != NULL, "open pointer command file for close");
        if (f) {
            fputs("close\n", f);
            fclose(f);
        }
        kill(server, SIGUSR2);
        bool closed = false;
        const Uint64 end = SDL_GetTicks() + 2000;
        while (SDL_GetTicks() < end && !closed) {
            SDL_PumpEvents();
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                    e.window.windowID == SDL_GetWindowID(w)) {
                    closed = true;
                }
            }
            if (!closed) {
                SDL_Delay(5);
            }
        }
        CHECK(closed, "compositor close button -> WINDOW_CLOSE_REQUESTED");
    }

    if (icons) {
        char last[640];
        CHECK(wait_last("opaque=", "opaque=set"), "opaque window: opaque region");
        CHECK(count_lines("attach ", last, sizeof(last)) > 0 && strncmp(last, "attach XRGB ", 12) == 0,
              "opaque window: XRGB8888 buffers ('%s')", last);
    }

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
        CHECK(wait_last("opaque=", "opaque=none"), "translucent: no opaque region");
        CHECK(SDL_SetWindowOpacity(w, 1.0f) && wait_last("alpha=", "alpha=4294967295"), "full opacity");
        CHECK(wait_last("opaque=", "opaque=set"), "opaque again: opaque region");
        CHECK(SDL_GetWindowOpacity(w) == 1.0f, "opacity reads back");
    } else {
        CHECK(!op, "SDL_SetWindowOpacity fails without wp_alpha_modifier_v1");
        CHECK(SDL_GetWindowOpacity(w) == 1.0f, "opacity unchanged (%g)", SDL_GetWindowOpacity(w));
    }

    /* hit test */
    {
        const Uint64 until = SDL_GetTicks() + 2000;
        while (SDL_GetMouseFocus() != w && SDL_GetTicks() < until) {
            SDL_PumpEvents();
            SDL_Delay(5);
        }
        CHECK(SDL_GetMouseFocus() == w, "pointer entered");
        CHECK(SDL_SetWindowHitTest(w, hit_test, NULL), "SDL_SetWindowHitTest: %s", SDL_GetError());
        int downs, ups;
        pointer_cmd("motion 50 10\npress 272\nrelease 272\n", &downs, &ups);
        CHECK(downs == 0, "a press on a draggable area is not passed on (%d downs)", downs);
        if (icons) {
            CHECK(wait_last("move", "move serial=ok"), "xdg_toplevel.move with the press serial");
        }
        pointer_cmd("motion 310 230\n", &downs, &ups);
        if (icons) {
            CHECK(wait_last_in(cursorlog, "shape=", "shape=23"), "se-resize cursor over the resize corner");
        }
        pointer_cmd("press 272\nrelease 272\n", &downs, &ups);
        CHECK(downs == 0, "a press on a resize area is not passed on (%d downs)", downs);
        if (icons) {
            CHECK(wait_last("resize", "resize edge=10 serial=ok"), "xdg_toplevel.resize bottom-right");
        }
        pointer_cmd("motion 150 120\npress 272\nrelease 272\n", &downs, &ups);
        CHECK(downs == 1 && ups == 1, "normal area: down %d up %d", downs, ups);
        if (icons) {
            CHECK(count_lines("move", NULL, 0) == 1 && count_lines("resize", NULL, 0) == 1,
                  "no move/resize from the normal area");
            CHECK(wait_last_in(cursorlog, "shape=", "shape=1"), "default cursor again");
        }
        /* right button: no hit testing */
        pointer_cmd("motion 50 10\npress 273\nrelease 273\n", &downs, &ups);
        CHECK(downs == 1, "right press on the draggable area reaches the app (%d)", downs);

        CHECK(SDL_ShowWindowSystemMenu(w, 40, 50), "SDL_ShowWindowSystemMenu");
        if (icons) {
            CHECK(wait_last("menu", "menu 40,50 serial=ok"), "show_window_menu with the press serial");
        }
        CHECK(SDL_SetWindowHitTest(w, NULL, NULL), "hit test off");
        pointer_cmd("motion 50 10\npress 272\nrelease 272\n", &downs, &ups);
        CHECK(downs == 1, "hit test off: presses reach the app (%d)", downs);
        if (icons) {
            CHECK(count_lines("move", NULL, 0) == 1, "no move with the hit test off");
        }
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

    /* SDL_WINDOW_TRANSPARENT */
    {
        int alpha = -1;
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
        SDL_Window *t = SDL_CreateWindow("transparent", 64, 48, SDL_WINDOW_TRANSPARENT);
        CHECK(t != NULL, "transparent window: %s", SDL_GetError());
        CHECK(t && (SDL_GetWindowFlags(t) & SDL_WINDOW_TRANSPARENT), "SDL_WINDOW_TRANSPARENT kept");
        CHECK(SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &alpha) && alpha == 8, "GL alpha size %d", alpha);
        if (icons && t) {
            CHECK(wait_last("opaque=", "opaque=none"), "transparent: no opaque region");
            CHECK(wait_last("attach ", "attach ARGB pixel=00000000"), "transparent: cleared to transparent");
        }
        SDL_Surface *ts = t ? SDL_GetWindowSurface(t) : NULL;
        CHECK(ts && ts->format == SDL_PIXELFORMAT_ARGB8888, "transparent surface: %s",
              ts ? SDL_GetPixelFormatName(ts->format) : SDL_GetError());
        if (ts) {
            SDL_FillSurfaceRect(ts, NULL, 0x80402010);
            CHECK(SDL_UpdateWindowSurface(t), "update transparent: %s", SDL_GetError());
            if (icons) {
                CHECK(wait_last("attach ", "attach ARGB pixel=80402010"), "transparent: alpha reaches the compositor");
            }
            SDLop_SetWindowClearColor(t, 1, 2, 3);
            if (icons) {
                CHECK(wait_last("attach ", "attach ARGB pixel=ff010203"), "explicit clear colour is opaque");
            }
        }
        if (t && icons) {
            const int before = count_lines("opaque=", NULL, 0);
            CHECK(SDL_SetWindowOpacity(t, 0.5f) && wait_last("alpha=", "alpha=2147483647"), "transparent opacity");
            CHECK(SDL_SetWindowOpacity(t, 1.0f) && wait_last("alpha=", "alpha=4294967295"), "transparent opacity 1");
            CHECK(wait_count("opaque=", before + 2, NULL, 0) && wait_last("opaque=", "opaque=none"),
                  "transparent: still no opaque region at full opacity");
        }
        if (t && icons) {
            /* a hidden window's toplevel goes, and its handle with it */
            char last[640];
            CHECK(count_lines("export ", last, sizeof(last)) > 0, "transparent window exported");
            CHECK(wait_handle(t, last + 7), "transparent window handle '%s'", last + 7);
            SDL_HideWindow(t);
            CHECK(wait_handle(t, NULL), "hidden: no export handle");
            char unexported[sizeof(last) + 16]; /* "unexport " + the handle */
            snprintf(unexported, sizeof(unexported), "unexport %s", last + 7);
            CHECK(wait_last("unexport ", unexported), "hidden: export destroyed");
            SDL_ShowWindow(t);
            const int n = atoi(last + 12);
            char want[32];
            snprintf(want, sizeof(want), "mini-%d", n + 1);
            CHECK(wait_handle(t, want), "re-shown: a new handle");
        }
        if (t) {
            SDL_DestroyWindow(t);
        }
    }

    SDL_DestroyWindow(w);
    SDL_Quit();
    int status = 0;
    waitpid(server, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "compositor exit status %d", status);
    unlink(logpath);
    unlink(cmdpath);
    unlink(cursorlog);
    rmdir(xdg);
    if (failures) {
        fprintf(stderr, "test_wayland_toplevel (%s): %d failure(s)\n", mode, failures);
        return 1;
    }
    printf("test_wayland_toplevel (%s): OK\n", mode);
    return 0;
}
