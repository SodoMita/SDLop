/*
  SDLop test: X11 input method (XIM) integration.

  A fake XIM server (xcb-imdkit, forked child, XMODIFIERS=@im=sdloptest)
  stands in for ibus/fcitx. It logs what the client sets up and, for the
  'a' key (keycode 38), plays an input method:
    - mode "root" (default stock style, PreeditNothing): commits a long
      string -> SDL_EVENT_TEXT_INPUT, no key events for 'a'
    - mode "composition" (SDL_HINT_IME_IMPLEMENTED_UI=composition,
      on-the-spot PreeditCallbacks): preedit "日本語" with the second
      character highlighted -> SDL_EVENT_TEXT_EDITING(start 1, length 1),
      then the commit (an empty TEXT_EDITING ends the composition)
  Every other key is handed back and must behave like a normal key. Also
  checked: the IC gets focus only while text input is active, the spot
  location follows SDL_SetTextInputArea, SDL_StopTextInput resets the IC and
  keys no longer reach the IM.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <xcb/xcb.h>
#include <xcb/xtest.h>
#include <xcb-imdkit/encoding.h>
#include <xcb-imdkit/imdkit.h>
#include <xcb-imdkit/ximproto.h>

#include <signal.h>
#include <stdarg.h>
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

#define COMMIT "Привет, мир! Привет, мир! Привет, мир! Привет, мир!"
#define PREEDIT "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" /* 日本語 */
#define KEY_A 38                                       /* evdev KEY_A + 8 */
#define KEY_B 56                                       /* evdev KEY_B + 8 */

/* ------------------------------------------------------------------ */
/* fake XIM server (child process)                                     */
/* ------------------------------------------------------------------ */

static FILE *server_log;

static void slog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(server_log, fmt, ap);
    va_end(ap);
    fputc('\n', server_log);
    fflush(server_log);
}

static void log_spot(xcb_im_input_context_t *ic)
{
    if (xcb_im_input_context_get_preedit_attr_mask(ic) & XCB_XIM_XNSpotLocation_MASK) {
        const xcb_im_preedit_attr_t *a = xcb_im_input_context_get_preedit_attr(ic);
        slog("spot=%d,%d", a->spot_location.x, a->spot_location.y);
    }
}

static void preedit_draw(xcb_im_t *im, xcb_im_input_context_t *ic, const char *utf8, uint32_t *feedback,
                         uint32_t nchars, uint32_t delete_chars, uint32_t caret)
{
    xcb_im_preedit_draw_fr_t frame;
    memset(&frame, 0, sizeof(frame));
    size_t len = 0;
    char *ct = utf8 ? xcb_utf8_to_compound_text(utf8, strlen(utf8), &len) : NULL;
    frame.caret = caret;
    frame.chg_first = 0;
    frame.chg_length = delete_chars;
    frame.status = ct ? 0 : 3; /* bit 0: no string, bit 1: no feedback */
    frame.length_of_preedit_string = (uint16_t)len;
    frame.preedit_string = (uint8_t *)ct;
    frame.feedback_array.size = ct ? nchars : 0;
    frame.feedback_array.items = ct ? feedback : NULL;
    xcb_im_preedit_draw_callback(im, ic, &frame);
    free(ct);
}

static void server_callback(xcb_im_t *im, xcb_im_client_t *client, xcb_im_input_context_t *ic,
                            const xcb_im_packet_header_fr_t *hdr, void *frame, void *arg, void *user_data)
{
    (void)client;
    (void)frame;
    (void)user_data;
    switch (hdr->major_opcode) {
    case XCB_XIM_CREATE_IC:
        slog("create style=0x%x", xcb_im_input_context_get_input_style(ic));
        log_spot(ic);
        break;
    case XCB_XIM_SET_IC_VALUES:
        log_spot(ic);
        break;
    case XCB_XIM_SET_IC_FOCUS:
        slog("focus");
        break;
    case XCB_XIM_UNSET_IC_FOCUS:
        slog("unfocus");
        break;
    case XCB_XIM_RESET_IC:
        slog("reset");
        break;
    case XCB_XIM_FORWARD_EVENT: {
        xcb_key_press_event_t *ev = (xcb_key_press_event_t *)arg;
        const bool press = (ev->response_type & 0x7f) == XCB_KEY_PRESS;
        if (ev->detail != KEY_A) {
            xcb_im_forward_event(im, ic, ev); /* not ours: hand it back */
            break;
        }
        if (!press) {
            break; /* the release of a consumed key is consumed too */
        }
        slog("ime key");
        const bool callbacks = (xcb_im_input_context_get_input_style(ic) & XCB_IM_PreeditCallbacks) != 0;
        if (callbacks) {
            uint32_t fb[3] = { XCB_XIM_UNDERLINE, XCB_XIM_REVERSE, XCB_XIM_UNDERLINE };
            xcb_im_preedit_start_callback(im, ic);
            preedit_draw(im, ic, PREEDIT, fb, 3, 0, 2);
        }
        size_t len = 0;
        char *ct = xcb_utf8_to_compound_text(COMMIT, strlen(COMMIT), &len);
        xcb_im_commit_string(im, ic, XCB_XIM_LOOKUP_CHARS, ct, (uint32_t)len, 0);
        free(ct);
        if (callbacks) {
            preedit_draw(im, ic, NULL, NULL, 0, 3, 0);
            xcb_im_preedit_done_callback(im, ic);
        }
        break;
    }
    default:
        break;
    }
}

static int run_server(const char *logpath)
{
    server_log = fopen(logpath, "a");
    if (!server_log) {
        return 2;
    }
    xcb_compound_text_init();
    int screen_num = 0;
    xcb_connection_t *c = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(c)) {
        slog("server: no X connection");
        return 2;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    xcb_screen_t *screen = it.data;
    xcb_window_t w = xcb_generate_id(c);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, 0, NULL);
    static uint32_t styles_arr[] = {
        XCB_IM_PreeditCallbacks | XCB_IM_StatusNothing,
        XCB_IM_PreeditNothing | XCB_IM_StatusNothing,
        XCB_IM_PreeditNothing | XCB_IM_StatusNone,
    };
    static char *encodings_arr[] = { "COMPOUND_TEXT" };
    xcb_im_styles_t styles = { 3, styles_arr };
    xcb_im_encodings_t encodings = { 1, encodings_arr };
    /* no trigger keys: static event flow, every key event is forwarded */
    xcb_im_t *im = xcb_im_create(c, screen_num, w, "sdloptest", XCB_IM_ALL_LOCALES, &styles, NULL, NULL, &encodings,
                                 XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE, server_callback, NULL);
    if (!im || !xcb_im_open_im(im)) {
        slog("server: open failed");
        return 2;
    }
    xcb_flush(c);
    slog("ready");
    xcb_generic_event_t *ev;
    while ((ev = xcb_wait_for_event(c))) {
        xcb_im_filter_event(im, ev);
        free(ev);
        xcb_flush(c);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* client side                                                         */
/* ------------------------------------------------------------------ */

static xcb_connection_t *obs;
static const char *logpath;

static bool log_has(const char *line)
{
    FILE *f = fopen(logpath, "r");
    if (!f) {
        return false;
    }
    char buf[256];
    bool found = false;
    while (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = '\0';
        found = found || strcmp(buf, line) == 0;
    }
    fclose(f);
    return found;
}

static int log_count(const char *line)
{
    FILE *f = fopen(logpath, "r");
    if (!f) {
        return 0;
    }
    char buf[256];
    int n = 0;
    while (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = '\0';
        n += strcmp(buf, line) == 0;
    }
    fclose(f);
    return n;
}

static bool wait_log(const char *line, int count)
{
    const Uint64 end = SDL_GetTicks() + 3000;
    while (SDL_GetTicks() < end) {
        if (log_count(line) >= count) {
            return true;
        }
        SDL_PumpEvents();
        SDL_Delay(5);
    }
    return false;
}

static void dump_log(void)
{
    FILE *f = fopen(logpath, "r");
    if (!f) {
        return;
    }
    char buf[256];
    fprintf(stderr, "  server log:\n");
    while (fgets(buf, sizeof(buf), f)) {
        fprintf(stderr, "    %s", buf);
    }
    fclose(f);
}

static void fake_key(uint8_t keycode, bool down)
{
    xcb_test_fake_input(obs, down ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
    xcb_flush(obs);
}

static void tap(uint8_t keycode)
{
    fake_key(keycode, true);
    fake_key(keycode, false);
}

typedef struct Collected
{
    char text[512];
    int text_events;
    int key_down_a, key_down_b, key_up_b;
    int edits;
    bool got_preedit, cleared_after_preedit;
    char bad_edit[128];
} Collected;

static void collect(Collected *c, int ms, bool (*done)(const Collected *))
{
    const Uint64 end = SDL_GetTicks() + (Uint64)ms;
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_TEXT_INPUT:
                c->text_events++;
                SDL_strlcat(c->text, e.text.text, sizeof(c->text));
                break;
            case SDL_EVENT_TEXT_EDITING:
                c->edits++;
                if (strcmp(e.edit.text, PREEDIT) == 0 && e.edit.start == 1 && e.edit.length == 1 &&
                    c->text[0] == '\0') {
                    c->got_preedit = true;
                } else if (e.edit.text[0] == '\0' && c->got_preedit) {
                    c->cleared_after_preedit = true;
                } else if (e.edit.text[0] != '\0') {
                    snprintf(c->bad_edit, sizeof(c->bad_edit), "'%s' %d+%d", e.edit.text, e.edit.start,
                             e.edit.length);
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                c->key_down_a += e.key.scancode == SDL_SCANCODE_A;
                c->key_down_b += e.key.scancode == SDL_SCANCODE_B;
                break;
            case SDL_EVENT_KEY_UP:
                c->key_up_b += e.key.scancode == SDL_SCANCODE_B;
                break;
            default:
                break;
            }
        }
        if (done && done(c)) {
            break;
        }
        SDL_Delay(5);
    }
}

static bool commit_done(const Collected *c)
{
    return strcmp(c->text, COMMIT) == 0;
}

static bool b_done(const Collected *c)
{
    return c->key_up_b > 0 && strcmp(c->text, "b") == 0;
}

static bool a_down(const Collected *c)
{
    return c->key_down_a > 0;
}

int main(int argc, char *argv[])
{
    if (argc >= 3 && strcmp(argv[1], "--server") == 0) {
        return run_server(argv[2]);
    }
    const bool composition = argc >= 2 && strcmp(argv[1], "composition") == 0;
    if (!getenv("DISPLAY")) {
        printf("test_x11_xim: SKIP (no DISPLAY)\n");
        return 0;
    }
    char dir[] = "/tmp/sdlop-xim-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    static char path[300];
    snprintf(path, sizeof(path), "%s/server.log", dir);
    logpath = path;

    pid_t server = fork();
    if (server == 0) {
        char *const server_argv[] = { argv[0], "--server", (char *)logpath, NULL };
        execvp(argv[0], server_argv);
        _exit(127);
    }
    const Uint64 ready_end = SDL_GetTicks() + 5000;
    while (!log_has("ready") && SDL_GetTicks() < ready_end) {
        SDL_Delay(10);
    }
    if (!log_has("ready")) {
        fprintf(stderr, "fake XIM server did not start\n");
        dump_log();
        kill(server, SIGKILL);
        return 1;
    }

    setenv("XMODIFIERS", "@im=sdloptest", 1);
    setenv("LC_ALL", "C.UTF-8", 1);
    setenv("SDL_VIDEODRIVER", "x11", 1);
    if (composition) {
        SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
    }
    obs = xcb_connect(NULL, NULL);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        kill(server, SIGKILL);
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("xim", 320, 240, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (!w) {
        kill(server, SIGKILL);
        return 1;
    }
    SDL_RaiseWindow(w);
    const xcb_window_t xid = (xcb_window_t)SDL_GetNumberProperty(SDL_GetWindowProperties(w),
                                                                  SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    const Uint64 start = SDL_GetTicks();
    bool asked_focus = false;
    while (SDL_GetKeyboardFocus() != w && SDL_GetTicks() < start + 3000) {
        SDL_PumpEvents();
        if (!asked_focus && SDL_GetTicks() > start + 500) {
            /* nobody focused it (no WM): do it from outside */
            xcb_set_input_focus(obs, XCB_INPUT_FOCUS_PARENT, xid, XCB_CURRENT_TIME);
            xcb_flush(obs);
            asked_focus = true;
        }
        SDL_Delay(5);
    }
    CHECK(SDL_GetKeyboardFocus() == w, "keyboard focus");

    CHECK(!log_has("focus"), "IC focused before SDL_StartTextInput");
    const SDL_Rect area = { 10, 20, 100, 30 };
    SDL_SetTextInputArea(w, &area, 5);
    CHECK(SDL_StartTextInput(w), "start text input");
    CHECK(wait_log(composition ? "create style=0x402" : "create style=0x408", 1), "IC created with the %s style",
          composition ? "PreeditCallbacks|StatusNothing" : "PreeditNothing|StatusNothing");
    CHECK(wait_log("focus", 1), "IC focused");
    CHECK(wait_log("spot=15,50", 1), "spot location = area x + cursor, area bottom");

    /* the IME key: composition + commit, no key events */
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    Collected c;
    memset(&c, 0, sizeof(c));
    tap(KEY_A);
    collect(&c, 3000, commit_done);
    collect(&c, 100, NULL); /* anything trailing */
    CHECK(log_has("ime key"), "key reached the IM");
    CHECK(strcmp(c.text, COMMIT) == 0, "committed '%s' (want '%s')", c.text, COMMIT);
    CHECK(c.text_events >= 2, "long commit split into several TEXT_INPUT events (%d)", c.text_events);
    CHECK(c.key_down_a == 0, "no KEY_DOWN for the key the IM consumed (%d)", c.key_down_a);
    if (composition) {
        CHECK(c.got_preedit, "TEXT_EDITING '" PREEDIT "' 1+1 before the commit (%d editing events)", c.edits);
        CHECK(c.cleared_after_preedit, "empty TEXT_EDITING ends the composition");
    } else {
        CHECK(c.edits == 0, "no TEXT_EDITING with the root style (%d)", c.edits);
    }
    CHECK(c.bad_edit[0] == '\0', "unexpected TEXT_EDITING %s", c.bad_edit);

    /* a key the IM hands back behaves normally */
    memset(&c, 0, sizeof(c));
    tap(KEY_B);
    collect(&c, 3000, b_done);
    CHECK(c.key_down_b == 1 && c.key_up_b == 1, "handed-back key: down %d up %d", c.key_down_b, c.key_up_b);
    CHECK(strcmp(c.text, "b") == 0, "handed-back key text '%s'", c.text);

    /* moving the area updates the spot */
    const SDL_Rect area2 = { 40, 60, 100, 20 };
    SDL_SetTextInputArea(w, &area2, 7);
    CHECK(wait_log("spot=47,80", 1), "spot follows SDL_SetTextInputArea");

    /* stopping: IC reset + unfocused, keys bypass the IM */
    const int resets = log_count("reset");
    CHECK(SDL_StopTextInput(w), "stop");
    CHECK(wait_log("reset", resets + 1), "IC reset on stop");
    CHECK(wait_log("unfocus", 1), "IC unfocused on stop");
    const int ime_keys = log_count("ime key");
    memset(&c, 0, sizeof(c));
    tap(KEY_A);
    collect(&c, 2000, a_down);
    collect(&c, 100, NULL);
    CHECK(c.key_down_a == 1, "KEY_DOWN A without text input (%d)", c.key_down_a);
    CHECK(c.text[0] == '\0', "no text without text input ('%s')", c.text);
    CHECK(log_count("ime key") == ime_keys, "IM saw a key while text input was stopped");

    /* and restarting brings it back */
    CHECK(SDL_StartTextInput(w), "restart");
    CHECK(wait_log("focus", 2), "IC refocused");
    memset(&c, 0, sizeof(c));
    tap(KEY_A);
    collect(&c, 3000, commit_done);
    CHECK(strcmp(c.text, COMMIT) == 0, "commit after restart '%s'", c.text);

    if (failures) {
        dump_log();
    }
    SDL_DestroyWindow(w);
    SDL_Quit();
    xcb_disconnect(obs);
    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
    unlink(logpath);
    rmdir(dir);
    if (failures) {
        fprintf(stderr, "test_x11_xim (%s): %d failure(s)\n", composition ? "composition" : "root", failures);
        return 1;
    }
    printf("test_x11_xim (%s): OK\n", composition ? "composition" : "root");
    return 0;
}
