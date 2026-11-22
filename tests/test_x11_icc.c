/*
  SDLop test: X11 SDL_GetWindowICCProfile (_ICC_PROFILE on the root).

    - no _ICC_PROFILE: NULL, "Screen is not calibrated."
    - setting the property sends SDL_EVENT_WINDOW_ICCPROF_CHANGED and the
      profile bytes come back unchanged (embedded NULs included)
    - changing and deleting it send the event again
  A profile the session already had is restored at the end. Skips cleanly
  when DISPLAY is unset.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static xcb_atom_t A_icc;

static bool wait_icc_event(SDL_WindowID id)
{
    const Uint64 end = SDL_GetTicks() + 2000;
    while (SDL_GetTicks() < end) {
        SDL_Event ev;
        while (SDL_WaitEventTimeout(&ev, 10)) {
            if (ev.type == SDL_EVENT_WINDOW_ICCPROF_CHANGED && ev.window.windowID == id) {
                return true;
            }
        }
    }
    return false;
}

static bool profile_is(SDL_Window *w, const void *want, size_t want_len)
{
    size_t size = 0;
    void *p = SDL_GetWindowICCProfile(w, &size);
    const bool ok = p && size == want_len && memcmp(p, want, want_len) == 0;
    SDL_free(p);
    return ok;
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_x11_icc: SKIP (DISPLAY not set)\n");
        return 0;
    }
    setenv("SDL_VIDEODRIVER", "x11", 1);
    int scr = 0;
    obs = xcb_connect(NULL, &scr);
    if (xcb_connection_has_error(obs)) {
        fprintf(stderr, "xcb_connect failed\n");
        return 1;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(obs));
    for (int i = 0; i < scr; i++) {
        xcb_screen_next(&it);
    }
    root = it.data->root;
    char name[32];
    if (scr > 0) {
        snprintf(name, sizeof(name), "_ICC_PROFILE_%d", scr);
    } else {
        snprintf(name, sizeof(name), "_ICC_PROFILE");
    }
    xcb_intern_atom_reply_t *ar = xcb_intern_atom_reply(obs, xcb_intern_atom(obs, 0, (uint16_t)strlen(name), name), NULL);
    A_icc = ar ? ar->atom : XCB_NONE;
    free(ar);
    /* keep whatever the session has */
    xcb_get_property_reply_t *saved =
        xcb_get_property_reply(obs, xcb_get_property(obs, 0, root, A_icc, XCB_GET_PROPERTY_TYPE_ANY, 0, 1u << 24), NULL);
    xcb_delete_property(obs, root, A_icc);
    xcb_flush(obs);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("icc", 64, 48, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (!w) {
        return 1;
    }
    const SDL_WindowID id = SDL_GetWindowID(w);
    size_t size = 123;
    void *none = SDL_GetWindowICCProfile(w, &size);
    CHECK(none == NULL && strstr(SDL_GetError(), "not calibrated"), "no profile: NULL, \"%s\"", SDL_GetError());
    SDL_free(none);

    static const uint8_t prof1[] = { 'a', 'c', 's', 'p', 0, 1, 2, 3, 0, 0xff, 0x80, 'x' };
    xcb_change_property(obs, XCB_PROP_MODE_REPLACE, root, A_icc, XCB_ATOM_CARDINAL, 8, sizeof(prof1), prof1);
    xcb_flush(obs);
    CHECK(wait_icc_event(id), "SDL_EVENT_WINDOW_ICCPROF_CHANGED on set");
    CHECK(profile_is(w, prof1, sizeof(prof1)), "profile bytes returned unchanged");

    uint8_t prof2[3000];
    for (size_t i = 0; i < sizeof(prof2); i++) {
        prof2[i] = (uint8_t)(i * 7);
    }
    xcb_change_property(obs, XCB_PROP_MODE_REPLACE, root, A_icc, XCB_ATOM_CARDINAL, 8, sizeof(prof2), prof2);
    xcb_flush(obs);
    CHECK(wait_icc_event(id), "event on change");
    CHECK(profile_is(w, prof2, sizeof(prof2)), "changed profile returned");

    xcb_delete_property(obs, root, A_icc);
    xcb_flush(obs);
    CHECK(wait_icc_event(id), "event on delete");
    none = SDL_GetWindowICCProfile(w, &size);
    CHECK(none == NULL, "profile gone after delete");
    SDL_free(none);

    SDL_DestroyWindow(w);
    SDL_Quit();
    if (saved && saved->type != XCB_NONE && saved->format == 8) {
        xcb_change_property(obs, XCB_PROP_MODE_REPLACE, root, A_icc, saved->type, 8,
                            (uint32_t)xcb_get_property_value_length(saved), xcb_get_property_value(saved));
        xcb_flush(obs);
    }
    free(saved);
    xcb_disconnect(obs);
    if (failures) {
        fprintf(stderr, "test_x11_icc: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_x11_icc: OK\n");
    return 0;
}
