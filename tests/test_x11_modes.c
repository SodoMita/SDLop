/*
  SDLop test: X11 exclusive fullscreen display modes through XRandR.

  An 800x600 mode is added to the first lit output from a raw xcb
  connection (Xvfb offers just its screen size), then:
    - it is listed among the fullscreen modes, the desktop mode unchanged
    - fullscreen with it as the window's fullscreen mode switches the crtc,
      SDL_GetCurrentDisplayMode reports it, CURRENT_MODE_CHANGED is sent,
      the display bounds follow, and the desktop mode stays put
    - leaving fullscreen, destroying the window and SDL_Quit each give the
      desktop mode back (checked on the crtc itself)
  The test mode is removed again at the end. Skips cleanly when DISPLAY is
  unset or the server cannot take a user mode.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

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

static xcb_connection_t *obs;
static xcb_window_t root;
static xcb_randr_output_t output;
static xcb_randr_crtc_t crtc;
static xcb_randr_mode_t test_mode;

static void crtc_size(int *w, int *h)
{
    *w = *h = -1;
    xcb_randr_get_crtc_info_reply_t *ci =
        xcb_randr_get_crtc_info_reply(obs, xcb_randr_get_crtc_info(obs, crtc, XCB_CURRENT_TIME), NULL);
    if (ci) {
        *w = ci->width;
        *h = ci->height;
        free(ci);
    }
}

static void root_size(int *w, int *h)
{
    *w = *h = -1;
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(obs, xcb_get_geometry(obs, root), NULL);
    if (g) {
        *w = g->width;
        *h = g->height;
        free(g);
    }
}

static bool wm_running(void)
{
    xcb_intern_atom_reply_t *a = xcb_intern_atom_reply(
        obs, xcb_intern_atom(obs, 0, strlen("_NET_SUPPORTING_WM_CHECK"), "_NET_SUPPORTING_WM_CHECK"), NULL);
    if (!a) {
        return false;
    }
    xcb_get_property_reply_t *p = xcb_get_property_reply(
        obs, xcb_get_property(obs, 0, root, a->atom, XCB_ATOM_WINDOW, 0, 1), NULL);
    const bool yes = p && xcb_get_property_value_length(p) >= 4;
    free(p);
    free(a);
    return yes;
}

/* first output with a crtc; adds an 800x600@60 mode to it */
static bool add_test_mode(void)
{
    xcb_randr_query_version_reply_t *v =
        xcb_randr_query_version_reply(obs, xcb_randr_query_version(obs, 1, 5), NULL);
    if (!v) {
        return false;
    }
    free(v);
    xcb_randr_get_screen_resources_current_reply_t *res = xcb_randr_get_screen_resources_current_reply(
        obs, xcb_randr_get_screen_resources_current(obs, root), NULL);
    if (!res) {
        return false;
    }
    xcb_randr_output_t *outs = xcb_randr_get_screen_resources_current_outputs(res);
    for (int i = 0; i < xcb_randr_get_screen_resources_current_outputs_length(res) && !crtc; i++) {
        xcb_randr_get_output_info_reply_t *oi = xcb_randr_get_output_info_reply(
            obs, xcb_randr_get_output_info(obs, outs[i], res->config_timestamp), NULL);
        if (oi && oi->crtc != XCB_NONE) {
            output = outs[i];
            crtc = oi->crtc;
        }
        free(oi);
    }
    free(res);
    if (!crtc) {
        return false;
    }
    static const char name[] = "sdlop-test-800x600";
    xcb_randr_mode_info_t mi;
    memset(&mi, 0, sizeof(mi));
    mi.width = 800;
    mi.height = 600;
    mi.dot_clock = 40000000; /* VESA 800x600@60 */
    mi.hsync_start = 840;
    mi.hsync_end = 968;
    mi.htotal = 1056;
    mi.vsync_start = 601;
    mi.vsync_end = 605;
    mi.vtotal = 628;
    mi.name_len = (uint16_t)strlen(name);
    mi.mode_flags = XCB_RANDR_MODE_FLAG_HSYNC_POSITIVE | XCB_RANDR_MODE_FLAG_VSYNC_POSITIVE;
    xcb_randr_create_mode_reply_t *cm = xcb_randr_create_mode_reply(
        obs, xcb_randr_create_mode(obs, root, mi, (uint32_t)strlen(name), name), NULL);
    if (!cm) {
        return false;
    }
    test_mode = cm->mode;
    free(cm);
    xcb_generic_error_t *err = xcb_request_check(obs, xcb_randr_add_output_mode_checked(obs, output, test_mode));
    if (err) {
        free(err);
        xcb_randr_destroy_mode(obs, test_mode);
        xcb_flush(obs);
        test_mode = 0;
        return false;
    }
    return true;
}

static void remove_test_mode(void)
{
    if (test_mode) {
        free(xcb_request_check(obs, xcb_randr_delete_output_mode_checked(obs, output, test_mode)));
        free(xcb_request_check(obs, xcb_randr_destroy_mode_checked(obs, test_mode)));
    }
}

static bool drain_for(Uint32 type, Uint64 ms)
{
    bool seen = false;
    const Uint64 end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        SDL_Event ev;
        while (SDL_WaitEventTimeout(&ev, 10)) {
            seen = seen || ev.type == type;
        }
    }
    return seen;
}

static bool wait_window_size(SDL_Window *w, int want_w, int want_h)
{
    const Uint64 end = SDL_GetTicks() + 3000;
    int ww = 0, wh = 0;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        SDL_GetWindowSize(w, &ww, &wh);
        if (ww == want_w && wh == want_h) {
            return true;
        }
        SDL_Delay(10);
    }
    fprintf(stderr, "window is %dx%d, want %dx%d\n", ww, wh, want_w, want_h);
    return false;
}

static bool find_800(SDL_DisplayID disp, SDL_DisplayMode *out)
{
    int n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(disp, &n);
    bool found = false;
    for (int i = 0; modes && i < n && !found; i++) {
        if (modes[i]->w == 800 && modes[i]->h == 600) {
            *out = *modes[i];
            found = true;
        }
    }
    SDL_free(modes);
    return found;
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("SKIP: no DISPLAY\n");
        return 0;
    }
    obs = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(obs)) {
        printf("SKIP: cannot connect\n");
        return 0;
    }
    root = xcb_setup_roots_iterator(xcb_get_setup(obs)).data->root;
    if (!add_test_mode()) {
        printf("SKIP: server takes no user RandR mode\n");
        xcb_disconnect(obs);
        return 0;
    }
    int desk_w, desk_h, root_w, root_h;
    crtc_size(&desk_w, &desk_h);
    root_size(&root_w, &root_h);
    const bool no_wm = !wm_running();
    printf("crtc %dx%d, screen %dx%d, %s\n", desk_w, desk_h, root_w, root_h, no_wm ? "no WM" : "WM");

    SDL_setenv_unsafe("SDL_VIDEO_DRIVER", "x11", 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        remove_test_mode();
        return 1;
    }
    const SDL_DisplayID disp = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(disp);
    CHECK(desk && desk->w == desk_w && desk->h == desk_h, "desktop mode %dx%d", desk ? desk->w : -1,
          desk ? desk->h : -1);
    const SDL_DisplayMode *cur = SDL_GetCurrentDisplayMode(disp);
    CHECK(cur && cur->w == desk_w && cur->h == desk_h, "current mode before switching");

    SDL_DisplayMode m800;
    if (!find_800(disp, &m800)) {
        CHECK(false, "800x600 not among the fullscreen modes");
        SDL_Quit();
        remove_test_mode();
        return 1;
    }
    CHECK(m800.refresh_rate > 59.0f && m800.refresh_rate < 61.0f, "800x600 refresh %g", (double)m800.refresh_rate);

    /* --- 1. enter / leave ------------------------------------------- */
    SDL_Window *w = SDL_CreateWindow("modes", 320, 240, 0);
    CHECK(w != NULL, "create: %s", SDL_GetError());
    drain_for(0, 200);
    CHECK(SDL_SetWindowFullscreenMode(w, &m800), "SetWindowFullscreenMode: %s", SDL_GetError());
    int cw, ch;
    crtc_size(&cw, &ch);
    CHECK(cw == desk_w && ch == desk_h, "mode applied while windowed: crtc %dx%d", cw, ch);
    CHECK(SDL_SetWindowFullscreen(w, true), "fullscreen: %s", SDL_GetError());
    crtc_size(&cw, &ch);
    CHECK(cw == 800 && ch == 600, "crtc after entering: %dx%d", cw, ch);
    cur = SDL_GetCurrentDisplayMode(disp);
    CHECK(cur && cur->w == 800 && cur->h == 600, "current mode %dx%d", cur ? cur->w : -1, cur ? cur->h : -1);
    desk = SDL_GetDesktopDisplayMode(disp);
    CHECK(desk && desk->w == desk_w && desk->h == desk_h, "desktop mode moved to %dx%d", desk ? desk->w : -1,
          desk ? desk->h : -1);
    SDL_Rect b;
    CHECK(SDL_GetDisplayBounds(disp, &b) && b.w == 800 && b.h == 600, "bounds %dx%d", b.w, b.h);
    CHECK(drain_for(SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED, 300), "no CURRENT_MODE_CHANGED on entering");
    /* the RandR notify must not turn the fullscreen mode into the desktop one */
    desk = SDL_GetDesktopDisplayMode(disp);
    CHECK(desk && desk->w == desk_w && desk->h == desk_h, "desktop mode after the notify %dx%d",
          desk ? desk->w : -1, desk ? desk->h : -1);
    cur = SDL_GetCurrentDisplayMode(disp);
    CHECK(cur && cur->w == 800, "current mode after the notify %d", cur ? cur->w : -1);
    if (no_wm) {
        CHECK(wait_window_size(w, 800, 600), "window not fitted to the mode");
    }

    CHECK(SDL_SetWindowFullscreen(w, false), "leave: %s", SDL_GetError());
    crtc_size(&cw, &ch);
    CHECK(cw == desk_w && ch == desk_h, "crtc after leaving: %dx%d", cw, ch);
    int rw, rh;
    root_size(&rw, &rh);
    CHECK(rw == root_w && rh == root_h, "screen after leaving: %dx%d", rw, rh);
    cur = SDL_GetCurrentDisplayMode(disp);
    CHECK(cur && cur->w == desk_w && cur->h == desk_h, "current mode after leaving");
    CHECK(drain_for(SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED, 300), "no CURRENT_MODE_CHANGED on leaving");

    /* desktop-sized fullscreen mode: no switch at all */
    SDL_SetWindowFullscreenMode(w, NULL);
    CHECK(SDL_SetWindowFullscreen(w, true), "borderless fullscreen");
    crtc_size(&cw, &ch);
    CHECK(cw == desk_w && ch == desk_h, "borderless fullscreen switched the crtc: %dx%d", cw, ch);
    SDL_SetWindowFullscreen(w, false);
    drain_for(0, 200);

    /* --- 2. destroy restores ---------------------------------------- */
    SDL_SetWindowFullscreenMode(w, &m800);
    SDL_SetWindowFullscreen(w, true);
    crtc_size(&cw, &ch);
    CHECK(cw == 800, "crtc before destroy: %dx%d", cw, ch);
    SDL_DestroyWindow(w);
    crtc_size(&cw, &ch);
    CHECK(cw == desk_w && ch == desk_h, "crtc after destroy: %dx%d", cw, ch);
    drain_for(0, 200);

    /* --- 3. SDL_Quit restores --------------------------------------- */
    w = SDL_CreateWindow("modes2", 320, 240, SDL_WINDOW_FULLSCREEN);
    SDL_SetWindowFullscreenMode(w, &m800);
    crtc_size(&cw, &ch);
    CHECK(cw == 800, "crtc for a window created fullscreen: %dx%d", cw, ch);
    SDL_Quit();
    crtc_size(&cw, &ch);
    CHECK(cw == desk_w && ch == desk_h, "crtc after SDL_Quit: %dx%d", cw, ch);
    root_size(&rw, &rh);
    CHECK(rw == root_w && rh == root_h, "screen after SDL_Quit: %dx%d", rw, rh);

    remove_test_mode();
    xcb_disconnect(obs);
    if (failures) {
        fprintf(stderr, "test_x11_modes: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_x11_modes: OK\n");
    return 0;
}
