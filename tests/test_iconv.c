/*
  SDLop test: SDL_iconv_string() grows its output buffer correctly.

  The output can be wider than the input (UTF-8 to UTF-16 is 2x, UTF-32 is
  4x), so the converter runs out of room part-way through and has to realloc
  and resume. Two bugs lived here:

    - the return code was kept in an int although SDL_iconv() returns size_t
      and the sentinels are (size_t)-1..-4;
    - when the output buffer was full, SDL_iconv() had already consumed the
      input code point but returned E2BIG without rewinding *inbuf, so the
      caller's retry skipped that character entirely.

  The second one silently dropped a character at every buffer boundary, which
  showed up as "AAAA" -> "AAA". This test walks every input length through the
  growth path and also checks a wide conversion, so neither can come back.

  Portable and headless: runs on both platforms.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

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

/* every input length from 1 to N goes through the grow-and-resume path at
 * least once, because the initial buffer is sized from the input length */
static void sweep_utf16(int max_len)
{
    for (int n = 1; n <= max_len; n++) {
        char src[512 + 1];
        Uint16 *out;

        for (int i = 0; i < n; i++) {
            src[i] = 'A';
        }
        src[n] = '\0';

        out = (Uint16 *)SDL_iconv_string("UTF-16LE", "UTF-8", src, (size_t)n);
        if (!out) {
            CHECK(0, "n=%d: SDL_iconv_string returned NULL (%s)", n, SDL_GetError());
            continue;
        }
        CHECK(out[n] == 0, "n=%d: result is not NUL-terminated", n);
        for (int i = 0; i < n; i++) {
            if (out[i] != 'A') {
                CHECK(0, "n=%d: character %d of %d is missing (dropped at a buffer boundary)",
                      n, i, n);
                break;
            }
        }
        SDL_free(out);
    }
}

/* a wide conversion: 4 bytes of output per ASCII byte on the input */
static void sweep_utf32(int max_len)
{
    for (int n = 1; n <= max_len; n++) {
        char src[256 + 1];
        Uint32 *out;

        for (int i = 0; i < n; i++) {
            src[i] = 'Z';
        }
        src[n] = '\0';

        out = (Uint32 *)SDL_iconv_string("UTF-32LE", "UTF-8", src, (size_t)n);
        if (!out) {
            CHECK(0, "utf32 n=%d: SDL_iconv_string returned NULL (%s)", n, SDL_GetError());
            continue;
        }
        CHECK(out[n] == 0, "utf32 n=%d: result is not NUL-terminated", n);
        for (int i = 0; i < n; i++) {
            if (out[i] != 'Z') {
                CHECK(0, "utf32 n=%d: character %d of %d is missing", n, i, n);
                break;
            }
        }
        SDL_free(out);
    }
}

int main(void)
{
    /* the exact case that used to truncate: one ASCII byte per 2 output bytes */
    {
        const char src[] = "AAAA";
        Uint16 *out = (Uint16 *)SDL_iconv_string("UTF-16LE", "UTF-8", src, 4);
        CHECK(out != NULL, "AAAA: NULL result");
        if (out) {
            CHECK(out[0] == 'A' && out[1] == 'A' && out[2] == 'A' && out[3] == 'A',
                  "AAAA -> %04X %04X %04X %04X, want four 'A'", out[0], out[1], out[2], out[3]);
            CHECK(out[4] == 0, "AAAA: missing terminator");
            SDL_free(out);
        }
    }

    /* multi-byte input, so growth happens inside a character */
    {
        const char *snow = "\xE2\x98\x83\xE2\x98\x83"; /* two U+2603 */
        Uint16 *out = (Uint16 *)SDL_iconv_string("UTF-16LE", "UTF-8", snow, strlen(snow));
        CHECK(out != NULL, "snowmen: NULL result");
        if (out) {
            CHECK(out[0] == 0x2603 && out[1] == 0x2603,
                  "snowmen -> %04X %04X, want 2603 2603", out[0], out[1]);
            SDL_free(out);
        }
    }

    /* a surrogate pair (U+1F389) round-trips through UTF-16 */
    {
        const char *party = "\xF0\x9F\x8E\x89";
        Uint16 *out = (Uint16 *)SDL_iconv_string("UTF-16LE", "UTF-8", party, strlen(party));
        CHECK(out != NULL, "U+1F389: NULL result");
        if (out) {
            CHECK(out[0] == 0xD83C && out[1] == 0xDF89,
                  "U+1F389 -> %04X %04X, want D83C DF89", out[0], out[1]);
            SDL_free(out);
        }
    }

    /* identity conversion still works */
    {
        const char msg[] = "hello, world";
        char *out = SDL_iconv_string("UTF-8", "UTF-8", msg, strlen(msg));
        CHECK(out != NULL, "identity: NULL result");
        if (out) {
            CHECK(strcmp(out, msg) == 0, "identity round-trip changed the text");
            SDL_free(out);
        }
    }

    sweep_utf16(400);
    sweep_utf32(200);

    if (failures) {
        fprintf(stderr, "test_iconv: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_iconv: growth path, widths and round-trips OK\n");
    return 0;
}
