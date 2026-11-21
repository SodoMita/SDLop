/*
  SDLop test: Wayland clipboard (wl_data_device) and primary selection
  (zwp_primary_selection_v1) between two SDL processes.

  Runs against tests/mini_compositor.c with MINI_SELECTION=1. This process
  ("A") re-executes itself as a peer ("B"); both are SDLop applications on
  the same compositor and take turns through a pipe:
    1. A offers text + a binary custom type and PRIMARY text; B sees the
       foreign SDL_EVENT_CLIPBOARD_UPDATE and reads everything back.
       A reading its own offer must short-circuit (no pipe to itself).
    2. A offers 2 MiB of text; B reads it intact (pipe streaming).
    3. B takes CLIPBOARD and PRIMARY; A's offer is cancelled (cleanup runs),
       A gets the update and reads B's data.
    4. B clears the clipboard; A sees it empty.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
static const char *who = "A";

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL [%s] %s:%d: ", who, __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                                  \
            fprintf(stderr, "\n");                                         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static int rd_fd = -1, wr_fd = -1;

static void send_token(char t)
{
    if (write(wr_fd, &t, 1) != 1) {
        perror("write token");
    }
}

/* wait for token `t` from the other process, pumping SDL meanwhile (our
 * sources must be served while the other side reads) */
static bool wait_token(char t, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        struct pollfd pfd = { rd_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 5) > 0) {
            char c = 0;
            if (read(rd_fd, &c, 1) != 1) {
                return false;
            }
            if (c == t) {
                return true;
            }
            fprintf(stderr, "[%s] unexpected token '%c' (wanted '%c')\n", who, c, t);
            return false;
        }
    }
    fprintf(stderr, "[%s] timeout waiting for token '%c'\n", who, t);
    return false;
}

/* pump until a foreign-owner clipboard update arrives; optionally require
 * a mime type in it */
static bool wait_foreign_update(const char *mime, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type != SDL_EVENT_CLIPBOARD_UPDATE || e.clipboard.owner) {
                continue;
            }
            if (!mime) {
                return true;
            }
            for (int i = 0; i < e.clipboard.num_mime_types; i++) {
                if (strcmp(e.clipboard.mime_types[i], mime) == 0) {
                    return true;
                }
            }
        }
        SDL_Delay(5);
    }
    return false;
}

static char *big_text(size_t n)
{
    char *s = (char *)malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        s[i] = (char)('A' + (i * 13 + i / 4096) % 26);
    }
    s[n] = '\0';
    return s;
}

#define BIG_SIZE (2u * 1024u * 1024u)
static const char a_text[] = "from A \xc3\xa9";
static const unsigned char a_binary[] = { 0, 1, 2, 0xfe, 0xff, 0, 'z' };
static int a_cleanups = 0;
static char *a_big;

static const void *SDLCALL a_cb(void *userdata, const char *mime, size_t *size)
{
    (void)userdata;
    if (strcmp(mime, "application/x-sdlop-test") == 0) {
        *size = sizeof(a_binary);
        return a_binary;
    }
    if (strcmp(mime, "text/plain;charset=utf-8") == 0) {
        *size = strlen(a_text);
        return a_text;
    }
    *size = 0;
    return NULL;
}

static const void *SDLCALL a_big_cb(void *userdata, const char *mime, size_t *size)
{
    (void)mime;
    *size = BIG_SIZE;
    return userdata;
}

static void SDLCALL a_cleanup(void *userdata)
{
    (void)userdata;
    a_cleanups++;
}

static int run_peer(void)
{
    who = "B";
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[B] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* 1 */
    CHECK(wait_token('1', 10000), "step 1 token");
    CHECK(wait_foreign_update("application/x-sdlop-test", 2000), "foreign update listing A's custom type");
    CHECK(SDL_HasClipboardText(), "SDL_HasClipboardText");
    char *t = SDL_GetClipboardText();
    CHECK(t && strcmp(t, a_text) == 0, "SDL_GetClipboardText = '%s'", t ? t : "(null)");
    SDL_free(t);
    CHECK(SDL_HasClipboardData("application/x-sdlop-test"), "custom type offered");
    size_t len = 0;
    void *d = SDL_GetClipboardData("application/x-sdlop-test", &len);
    CHECK(d && len == sizeof(a_binary) && memcmp(d, a_binary, len) == 0, "binary data (len %zu)", len);
    SDL_free(d);
    CHECK(!SDL_HasClipboardData("image/png"), "type A doesn't offer");
    d = SDL_GetClipboardData("image/png", &len);
    CHECK(!d, "no data for a type A doesn't offer");
    SDL_free(d);
    size_t nm = 0;
    char **ml = SDL_GetClipboardMimeTypes(&nm);
    CHECK(ml && nm == 2, "SDL_GetClipboardMimeTypes: %zu types", nm);
    SDL_free(ml);
    CHECK(SDL_HasPrimarySelectionText(), "SDL_HasPrimarySelectionText");
    char *p = SDL_GetPrimarySelectionText();
    CHECK(p && strcmp(p, "A primary") == 0, "SDL_GetPrimarySelectionText = '%s'", p ? p : "(null)");
    SDL_free(p);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    send_token('1');

    /* 2 */
    CHECK(wait_token('2', 10000), "step 2 token");
    CHECK(wait_foreign_update(NULL, 2000), "foreign update for the big offer");
    char *expect = big_text(BIG_SIZE);
    Uint64 t0 = SDL_GetTicksNS();
    t = SDL_GetClipboardText();
    Uint64 t1 = SDL_GetTicksNS();
    CHECK(t && strlen(t) == BIG_SIZE && memcmp(t, expect, BIG_SIZE) == 0, "2 MiB read intact (len %zu)",
          t ? strlen(t) : 0);
    printf("[B] 2 MiB clipboard read in %.1f ms\n", (double)(t1 - t0) / 1e6);
    SDL_free(t);
    free(expect);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    send_token('2');

    /* 3 */
    CHECK(wait_token('3', 10000), "step 3 token");
    CHECK(SDL_SetClipboardText("from B"), "SDL_SetClipboardText: %s", SDL_GetError());
    CHECK(SDL_SetPrimarySelectionText("B primary"), "SDL_SetPrimarySelectionText: %s", SDL_GetError());
    t = SDL_GetClipboardText();
    CHECK(t && strcmp(t, "from B") == 0, "own text readable while A is alive");
    SDL_free(t);
    send_token('3');
    CHECK(wait_token('4', 10000), "step 3 done");

    /* 4 */
    CHECK(SDL_ClearClipboardData(), "SDL_ClearClipboardData");
    send_token('4');
    CHECK(wait_token('5', 10000), "step 4 done");

    SDL_Quit();
    return failures ? 1 : 0;
}

int main(int argc, char *argv[])
{
    if (argc >= 4 && strcmp(argv[1], "--peer") == 0) {
        rd_fd = atoi(argv[2]);
        wr_fd = atoi(argv[3]);
        return run_peer();
    }
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor\n", argv[0]);
        return 2;
    }

    char xdg[] = "/tmp/sdlop-mini-clip-XXXXXX";
    if (!mkdtemp(xdg)) {
        perror("mkdtemp");
        return 2;
    }
    char sockpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/wayland-mini", xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    pid_t server = fork();
    if (server == 0) {
        setenv("MINI_SELECTION", "1", 1);
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

    /* the peer connects only after A's first offer exists, so it gets the
     * selection when its data device is created (as on keyboard focus) */
    const char *mimes[] = { "text/plain;charset=utf-8", "application/x-sdlop-test" };
    CHECK(SDL_SetClipboardData(a_cb, a_cleanup, NULL, mimes, 2), "SDL_SetClipboardData: %s", SDL_GetError());
    CHECK(SDL_SetPrimarySelectionText("A primary"), "SDL_SetPrimarySelectionText: %s", SDL_GetError());
    Uint64 t0 = SDL_GetTicksNS();
    char *own = SDL_GetClipboardText();
    CHECK(own && strcmp(own, a_text) == 0, "own offer readable: '%s'", own ? own : "(null)");
    CHECK(SDL_GetTicksNS() - t0 < 200000000ull, "own offer read without a pipe round trip");
    SDL_free(own);

    int to_peer[2], from_peer[2];
    if (pipe(to_peer) < 0 || pipe(from_peer) < 0) {
        perror("pipe");
        return 2;
    }
    pid_t peer = fork();
    if (peer == 0) {
        close(to_peer[1]);
        close(from_peer[0]);
        char r[16], w[16];
        snprintf(r, sizeof(r), "%d", to_peer[0]);
        snprintf(w, sizeof(w), "%d", from_peer[1]);
        execl(argv[0], argv[0], "--peer", r, w, (char *)NULL);
        _exit(127);
    }
    close(to_peer[0]);
    close(from_peer[1]);
    wr_fd = to_peer[1];
    rd_fd = from_peer[0];

    /* 1 */
    send_token('1');
    CHECK(wait_token('1', 10000), "peer step 1");

    /* 2 */
    a_big = big_text(BIG_SIZE);
    const char *text_mime[] = { "text/plain;charset=utf-8" };
    CHECK(SDL_SetClipboardData(a_big_cb, a_cleanup, a_big, text_mime, 1), "SDL_SetClipboardData (big)");
    CHECK(a_cleanups == 1, "replacing our offer ran its cleanup (%d)", a_cleanups);
    send_token('2');
    CHECK(wait_token('2', 15000), "peer step 2");

    /* 3 */
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    send_token('3');
    CHECK(wait_token('3', 10000), "peer step 3");
    CHECK(wait_foreign_update("text/plain;charset=utf-8", 2000), "foreign update for B's offer");
    CHECK(a_cleanups == 2, "losing the clipboard cancelled our offer (%d)", a_cleanups);
    char *t = SDL_GetClipboardText();
    CHECK(t && strcmp(t, "from B") == 0, "SDL_GetClipboardText (B) = '%s'", t ? t : "(null)");
    SDL_free(t);
    char *p = SDL_GetPrimarySelectionText();
    CHECK(p && strcmp(p, "B primary") == 0, "SDL_GetPrimarySelectionText (B) = '%s'", p ? p : "(null)");
    SDL_free(p);
    send_token('4');

    /* 4 */
    CHECK(wait_token('4', 10000), "peer step 4");
    Uint64 deadline = SDL_GetTicks() + 2000;
    while (SDL_HasClipboardText() && SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
    CHECK(!SDL_HasClipboardText(), "clipboard empty after B cleared it");
    send_token('5');

    int status = 0;
    if (waitpid(peer, &status, 0) != peer || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: peer exited with status 0x%x\n", status);
        failures++;
    }
    SDL_Quit();
    free(a_big);
    if (waitpid(server, &status, 0) != server || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: mini compositor exited with status 0x%x\n", status);
        failures++;
    }
    if (failures) {
        fprintf(stderr, "test_wayland_clipboard: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_wayland_clipboard: PASS\n");
    return 0;
}
