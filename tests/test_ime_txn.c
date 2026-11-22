/*
  text-input-v3 transaction application (sdlop_wayland_ime_txn.h), without
  a compositor: the listeners record, `done` emits in protocol order.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>
#include "video/wayland/sdlop_wayland_ime_txn.h"

#include <stdio.h>
#include <string.h>

static char log_buf[1024];
static int failures;

#define CHECK(cond, ...)                \
    do {                                \
        if (!(cond)) {                  \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");               \
            failures++;                 \
        }                               \
    } while (0)

static void edit(const char *text, int start, int length)
{
    char e[128];
    snprintf(e, sizeof(e), "EDIT:%s@%d+%d|", text, start, length);
    strcat(log_buf, e);
}

static void text(const char *t)
{
    strcat(log_buf, "COMMIT:");
    strcat(log_buf, t);
    strcat(log_buf, "|");
}

static void expect(const char *what, const char *want)
{
    CHECK(strcmp(log_buf, want) == 0, "%s: got '%s', want '%s'", what, log_buf, want);
    log_buf[0] = '\0';
}

int main(void)
{
    SDLOP_IMETxn t;
    SDL_zero(t);

    /* a preedit alone; a pump (= nothing) before done emits nothing */
    SDLOP_IMETxnSetPreedit(&t, "old", 3, 3);
    expect("before done", "");
    SDLOP_IMETxnDone(&t, edit, text);
    expect("preedit", "EDIT:old@3+0|");

    /* commit + new preedit, both wire orders: old cleared, commit, then new */
    SDLOP_IMETxnSetPreedit(&t, "next", 4, 4);
    SDLOP_IMETxnSetCommit(&t, "accepted");
    expect("preedit, commit before done", "");
    SDLOP_IMETxnDone(&t, edit, text);
    expect("preedit, commit, done", "EDIT:@0+0|COMMIT:accepted|EDIT:next@4+0|");

    SDLOP_IMETxnSetCommit(&t, "second");
    SDLOP_IMETxnSetPreedit(&t, "third", 0, 5);
    expect("commit, preedit before done", "");
    SDLOP_IMETxnDone(&t, edit, text);
    expect("commit, preedit, done", "EDIT:@0+0|COMMIT:second|EDIT:third@0+5|");

    /* commit only: preedit gone afterwards (pending state reset at done) */
    SDLOP_IMETxnSetCommit(&t, "last");
    SDLOP_IMETxnDone(&t, edit, text);
    expect("commit only", "EDIT:@0+0|COMMIT:last|EDIT:@0+0|");

    /* commit with nothing visible: no clear before it */
    SDLOP_IMETxnSetCommit(&t, "x");
    SDLOP_IMETxnDone(&t, edit, text);
    expect("commit, nothing visible", "COMMIT:x|EDIT:@0+0|");

    /* empty done: empty editing (stock) */
    SDLOP_IMETxnDone(&t, edit, text);
    expect("empty done", "EDIT:@0+0|");

    /* cursor bytes -> characters (UTF-8), hidden cursor stays -1 */
    SDLOP_IMETxnSetPreedit(&t, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", 3, 6);
    SDLOP_IMETxnDone(&t, edit, text);
    expect("utf-8 cursor", "EDIT:\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e@1+1|");
    SDLOP_IMETxnSetPreedit(&t, "ab", -1, -1);
    SDLOP_IMETxnDone(&t, edit, text);
    expect("hidden cursor", "EDIT:ab@-1+-1|");

    /* a repeated event replaces (last one wins), reset drops pending */
    SDLOP_IMETxnSetCommit(&t, "a");
    SDLOP_IMETxnSetCommit(&t, "b");
    SDLOP_IMETxnSetPreedit(&t, "p", 1, 1);
    SDLOP_IMETxnReset(&t);
    SDLOP_IMETxnDone(&t, edit, text);
    expect("reset", "EDIT:@0+0|");
    SDLOP_IMETxnSetCommit(&t, "a");
    SDLOP_IMETxnSetCommit(&t, "b");
    SDLOP_IMETxnDone(&t, edit, text);
    expect("last commit wins", "COMMIT:b|EDIT:@0+0|");
    SDLOP_IMETxnReset(&t);

    printf("test_ime_txn: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
