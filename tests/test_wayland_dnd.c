/*
  SDLop test: Wayland drag-and-drop target.

  tests/mini_compositor.c with MINI_DND drags data onto the window (see
  there); this checks the SDL_EVENT_DROP_* sequence:
    files    - text/uri-list with escapes, a comment line and a non-local
               URI -> BEGIN, POSITION(10.5,20.25), POSITION(30,40),
               FILE x2, COMPLETE; the compositor checks finish
    text     - text/plain;charset=utf-8 -> TEXT per line
    bigtext  - one 1 MiB line from a blocking-write source (larger than
               the pipe): our read end only is nonblocking -> intact TEXT
    refuse   - only image/png offered: nothing accepted, no events
    disabled - SDL_EVENT_DROP_FILE/TEXT disabled: nothing accepted

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

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/mini_compositor [files|text|refuse|disabled]\n", argv[0]);
        return 2;
    }
    const char *mode = argc > 2 ? argv[2] : "files";
    const bool files = strcmp(mode, "files") == 0;
    const bool bigtext = strcmp(mode, "bigtext") == 0;
    const bool text = strcmp(mode, "text") == 0;
    const bool expect_drop = files || text || bigtext;
    enum { BIG_REPEAT = 65536 }; /* x 16 bytes = 1 MiB */
    size_t big_len = 0;

    char xdg[] = "/tmp/sdlop-mini-dnd-XXXXXX";
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
        if (files || strcmp(mode, "disabled") == 0) {
            setenv("MINI_DND",
                   "# a comment\r\nfile:///tmp/a%20b.txt\r\nfile://localhost/tmp/c.txt\r\n"
                   "file://elsewhere.invalid/tmp/remote.txt\r\nhttp://example.com/x\r\n",
                   1);
            setenv("MINI_DND_MIMES", "text/plain,text/uri-list", 1);
        } else if (bigtext) {
            setenv("MINI_DND", "0123456789abcdef", 1);
            setenv("MINI_DND_REPEAT", "65536", 1);
            setenv("MINI_DND_MIMES", "text/plain;charset=utf-8", 1);
        } else if (text) {
            setenv("MINI_DND", "hello\nworld \xc3\xa9\n", 1);
            setenv("MINI_DND_MIMES", "text/plain;charset=utf-8", 1);
        } else {
            setenv("MINI_DND", "\x89PNG", 1);
            setenv("MINI_DND_MIMES", "image/png", 1);
        }
        if (!expect_drop) {
            setenv("MINI_DND_REFUSE", "1", 1);
        }
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
    if (strcmp(mode, "disabled") == 0) {
        SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, false);
        SDL_SetEventEnabled(SDL_EVENT_DROP_TEXT, false);
    }
    SDL_Window *w = SDL_CreateWindow("dnd test", 320, 240, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    SDL_Surface *s = w ? SDL_GetWindowSurface(w) : NULL;
    if (s) {
        SDL_FillSurfaceRect(s, NULL, 0xff336699);
        SDL_UpdateWindowSurface(w);
    }
    const SDL_WindowID wid = w ? SDL_GetWindowID(w) : 0;

    int begins = 0, positions = 0, completes = 0, nfiles = 0, ntexts = 0, wrong_window = 0;
    float px[4] = { 0 }, py[4] = { 0 };
    char *file[4] = { 0 }, *txt[4] = { 0 };
    bool order_ok = true;
    Uint64 deadline = SDL_GetTicks() + (bigtext ? 10000 : expect_drop ? 3000 : 1500);
    while (SDL_GetTicks() < deadline && completes == 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type < SDL_EVENT_DROP_FILE || e.type > SDL_EVENT_DROP_POSITION) {
                continue;
            }
            if (e.drop.windowID != wid) {
                wrong_window++;
            }
            switch (e.type) {
            case SDL_EVENT_DROP_BEGIN:
                order_ok = order_ok && begins == 0 && positions == 0;
                begins++;
                break;
            case SDL_EVENT_DROP_POSITION:
                order_ok = order_ok && begins == 1;
                if (positions < 4) {
                    px[positions] = e.drop.x;
                    py[positions] = e.drop.y;
                }
                positions++;
                break;
            case SDL_EVENT_DROP_FILE:
                order_ok = order_ok && begins == 1;
                if (nfiles < 4) {
                    file[nfiles] = SDL_strdup(e.drop.data);
                }
                nfiles++;
                break;
            case SDL_EVENT_DROP_TEXT:
                order_ok = order_ok && begins == 1;
                if (bigtext) {
                    big_len = strlen(e.drop.data);
                    for (size_t i = 0; i < big_len && order_ok; i += 16) {
                        order_ok = big_len - i >= 16 && memcmp(e.drop.data + i, "0123456789abcdef", 16) == 0;
                    }
                } else if (ntexts < 4) {
                    txt[ntexts] = SDL_strdup(e.drop.data);
                }
                ntexts++;
                break;
            case SDL_EVENT_DROP_COMPLETE:
                completes++;
                break;
            default:
                break;
            }
        }
        SDL_Delay(5);
    }

    if (expect_drop) {
        CHECK(begins == 1 && completes == 1 && order_ok, "BEGIN..COMPLETE sequence (begin %d complete %d order %d)",
              begins, completes, order_ok);
        CHECK(wrong_window == 0, "events carry the window id");
        CHECK(positions == 2 && px[0] == 10.5f && py[0] == 20.25f && px[1] == 30.0f && py[1] == 40.0f,
              "positions (%d): (%g,%g) (%g,%g)", positions, px[0], py[0], px[1], py[1]);
    }
    if (files) {
        CHECK(nfiles == 2 && ntexts == 0, "2 local files (got %d files, %d texts)", nfiles, ntexts);
        CHECK(file[0] && strcmp(file[0], "/tmp/a b.txt") == 0, "file 1 = '%s'", file[0] ? file[0] : "(null)");
        CHECK(file[1] && strcmp(file[1], "/tmp/c.txt") == 0, "file 2 = '%s'", file[1] ? file[1] : "(null)");
    } else if (bigtext) {
        CHECK(ntexts == 1 && nfiles == 0, "1 big text line (got %d texts, %d files)", ntexts, nfiles);
        CHECK(big_len == (size_t)BIG_REPEAT * 16, "big text intact: %zu of %zu bytes", big_len, (size_t)BIG_REPEAT * 16);
    } else if (text) {
        CHECK(ntexts == 2 && nfiles == 0, "2 text lines (got %d texts, %d files)", ntexts, nfiles);
        CHECK(txt[0] && strcmp(txt[0], "hello") == 0, "text 1 = '%s'", txt[0] ? txt[0] : "(null)");
        CHECK(txt[1] && strcmp(txt[1], "world \xc3\xa9") == 0, "text 2 = '%s'", txt[1] ? txt[1] : "(null)");
    } else {
        CHECK(begins == 0 && positions == 0 && nfiles == 0 && ntexts == 0 && completes == 0,
              "no drop events when refusing (begin %d pos %d files %d texts %d complete %d)", begins, positions, nfiles,
              ntexts, completes);
    }
    for (int i = 0; i < 4; i++) {
        SDL_free(file[i]);
        SDL_free(txt[i]);
    }

    if (!expect_drop) {
        /* let the compositor finish the cancelled drag before leaving */
        Uint64 until = SDL_GetTicks() + 300;
        while (SDL_GetTicks() < until) {
            SDL_PumpEvents();
            SDL_Delay(5);
        }
    }
    SDL_DestroyWindow(w);
    SDL_Quit();
    int status = 0;
    if (waitpid(server, &status, 0) != server || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: mini compositor exited with status 0x%x\n", status);
        failures++;
    }
    if (failures) {
        fprintf(stderr, "test_wayland_dnd (%s): %d failure(s)\n", mode, failures);
        return 1;
    }
    printf("test_wayland_dnd (%s): PASS\n", mode);
    return 0;
}
