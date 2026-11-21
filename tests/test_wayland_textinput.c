/*
  SDLop test: Wayland IME through zwp_text_input_v3 (stock SDL3 behaviour).

  tests/mini_compositor.c with MINI_TEXT_INPUT focuses the window and logs
  every text-input state commit; once enabled its "IME" sends a preedit and
  then commits a long string. Checks:
    - text input is enabled only while SDL text input is active, with the
      content type of SDL_PROP_TEXTINPUT_* and the cursor rectangle of
      SDL_SetTextInputArea (x clamped to the area as in stock)
    - preedit -> SDL_EVENT_TEXT_EDITING with the cursor in characters
    - commit_string -> SDL_EVENT_TEXT_INPUT, split into <= 63-byte events
      at UTF-8 boundaries, followed by an empty TEXT_EDITING
    - SDL_StopTextInput disables it

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

/* 22 bytes per repetition, mostly two-byte characters: 63 bytes would end
 * inside one */
#define COMMIT "Привет, мир! Привет, мир! Привет, мир! Привет, мир!"
#define PREEDIT "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" /* 日本語 */

static bool last_line_is(const char *path, const char *want)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    char buf[256], last[256] = "";
    while (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = '\0';
        snprintf(last, sizeof(last), "%s", buf);
    }
    fclose(f);
    return strcmp(last, want) == 0;
}

static int count_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    char buf[256];
    int n = 0;
    while (fgets(buf, sizeof(buf), f)) {
        n++;
    }
    fclose(f);
    return n;
}

/* pump events until the compositor logged `want` as its latest state */
static bool wait_state(const char *path, const char *want)
{
    const Uint64 deadline = SDL_GetTicks() + 2000;
    while (SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        if (last_line_is(path, want)) {
            return true;
        }
        SDL_Delay(5);
    }
    FILE *f = fopen(path, "r");
    fprintf(stderr, "  waiting for '%s', log:\n", want);
    if (f) {
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {
            fprintf(stderr, "    %s", buf);
        }
        fclose(f);
    }
    return false;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor\n", argv[0]);
        return 2;
    }
    char xdg[] = "/tmp/sdlop-mini-ti-XXXXXX";
    if (!mkdtemp(xdg)) {
        perror("mkdtemp");
        return 2;
    }
    char sockpath[256], logpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/wayland-mini", xdg);
    snprintf(logpath, sizeof(logpath), "%s/text-input.log", xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDL_TOUCH_MOUSE_EVENTS", "0", 1);

    pid_t server = fork();
    if (server == 0) {
        setenv("MINI_TEXT_INPUT", logpath, 1);
        setenv("MINI_TI_COMMIT", COMMIT, 1);
        execl(argv[1], "mini_compositor", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 200 && access(sockpath, F_OK) != 0; i++) {
        SDL_Delay(10);
    }
    if (access(sockpath, F_OK) != 0) {
        fprintf(stderr, "mini compositor did not create %s\n", sockpath);
        kill(server, SIGKILL);
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        kill(server, SIGKILL);
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("text input", 320, 240, 0);
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

    bool focused = false;
    Uint64 deadline = SDL_GetTicks() + 2000;
    while (!focused && SDL_GetTicks() < deadline) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            focused = focused || e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        }
        SDL_Delay(5);
    }
    CHECK(focused, "keyboard focus");
    CHECK(SDL_GetKeyboardFocus() == w, "SDL_GetKeyboardFocus");
    CHECK(count_lines(logpath) == 0, "text input committed before SDL_StartTextInput");

    /* area first (common usage), then start as an e-mail field: purpose
     * email (6); hint completion|spellcheck|multiline (0x203, stock
     * defaults); cursor x = 10 + 5 */
    const SDL_Rect area = { 10, 20, 100, 30 };
    CHECK(SDL_SetTextInputArea(w, &area, 5), "set area: %s", SDL_GetError());
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT_EMAIL);
    CHECK(SDL_StartTextInputWithProperties(w, props), "start: %s", SDL_GetError());
    SDL_DestroyProperties(props);
    CHECK(SDL_TextInputActive(w), "active");
    CHECK(wait_state(logpath, "enabled=1 hint=515 purpose=6 rect=15,20,1,30"), "enabled as e-mail field");

    /* the IME script: preedit, then the commit */
    char text[512] = "";
    int edits = 0, inputs = 0;
    bool got_preedit = false, cleared = false;
    deadline = SDL_GetTicks() + 2000;
    while (SDL_GetTicks() < deadline && !(cleared && strcmp(text, COMMIT) == 0)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_TEXT_EDITING) {
                edits++;
                CHECK(e.edit.windowID == SDL_GetWindowID(w), "editing windowID");
                if (strcmp(e.edit.text, PREEDIT) == 0) {
                    CHECK(!got_preedit && text[0] == '\0', "preedit order");
                    CHECK(e.edit.start == 1 && e.edit.length == 1, "preedit cursor %d+%d (want 1+1 characters)",
                          e.edit.start, e.edit.length);
                    got_preedit = true;
                } else if (e.edit.text[0] == '\0') {
                    cleared = strcmp(text, COMMIT) == 0;
                } else {
                    CHECK(false, "unexpected editing text '%s'", e.edit.text);
                }
            } else if (e.type == SDL_EVENT_TEXT_INPUT) {
                inputs++;
                CHECK(strlen(e.text.text) <= 63, "text event of %zu bytes", strlen(e.text.text));
                const unsigned char last = (unsigned char)e.text.text[strlen(e.text.text) - 1];
                CHECK(last < 0x80 || (last & 0xC0) == 0x80, "chunk ends inside a UTF-8 sequence");
                SDL_strlcat(text, e.text.text, sizeof(text));
            }
        }
        SDL_Delay(5);
    }
    CHECK(got_preedit, "TEXT_EDITING '" PREEDIT "' (%d editing events)", edits);
    CHECK(strcmp(text, COMMIT) == 0, "committed text '%s' (want '%s')", text, COMMIT);
    CHECK(inputs >= 2, "long commit split into several TEXT_INPUT events (%d)", inputs);
    CHECK(cleared, "empty TEXT_EDITING after the commit");

    /* moving the cursor: x clamped to the area's right edge */
    const SDL_Rect area2 = { 50, 60, 80, 20 };
    CHECK(SDL_SetTextInputArea(w, &area2, 100), "set area 2");
    CHECK(wait_state(logpath, "enabled=1 hint=515 purpose=6 rect=130,60,1,20"), "cursor rectangle update");

    /* restarting applies new properties: plain text = auto-capitalisation */
    CHECK(SDL_StartTextInput(w), "restart");
    CHECK(wait_state(logpath, "enabled=1 hint=519 purpose=0 rect=130,60,1,20"), "restart with default properties");

    CHECK(SDL_ClearComposition(w), "clear composition");
    CHECK(SDL_StopTextInput(w), "stop");
    CHECK(!SDL_TextInputActive(w), "inactive");
    CHECK(wait_state(logpath, "enabled=0 hint=0 purpose=0 rect=0,0,0,0"), "disabled by SDL_StopTextInput");

    SDL_DestroyWindow(w);
    SDL_Quit();
    int status = 0;
    waitpid(server, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "compositor exit status %d", status);
    unlink(logpath);
    rmdir(xdg);
    if (failures) {
        fprintf(stderr, "test_wayland_textinput: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_wayland_textinput: OK\n");
    return 0;
}
