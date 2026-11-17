/*
  SDLop test: stdinc utilities - memory functions, environment, ctype,
  strings, number parsing, printf. Headless; no video needed.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>

/* custom allocator hooks for SDL_SetMemoryFunctions */
static int custom_mallocs, custom_callocs, custom_reallocs, custom_frees;
static void *custom_malloc(size_t s) { custom_mallocs++; return malloc(s); }
static void *custom_calloc(size_t n, size_t s) { custom_callocs++; return calloc(n, s); }
static void *custom_realloc(void *p, size_t s) { custom_reallocs++; return realloc(p, s); }
static void custom_free(void *p) { custom_frees++; free(p); }

int main(void)
{
    /* --- memory --- */
    void *p = SDL_malloc(64);
    assert(p);
    memset(p, 0xA5, 64);
    p = SDL_realloc(p, 256);
    assert(p);
    assert(((unsigned char *)p)[0] == 0xA5); /* contents preserved */
    SDL_free(p);
    SDL_free(NULL); /* must be a no-op */

    p = SDL_calloc(16, 4);
    assert(p);
    for (int i = 0; i < 64; i++) {
        assert(((unsigned char *)p)[i] == 0);
    }
    SDL_free(p);

    /* zero-size is rounded up, never NULL on a working allocator */
    p = SDL_malloc(0);
    assert(p);
    SDL_free(p);

    /* aligned allocations */
    for (size_t align = 8; align <= 4096; align *= 2) {
        void *a = SDL_aligned_alloc(align, 100);
        assert(a);
        assert(((uintptr_t)a % align) == 0);
        memset(a, 0x5A, 100); /* may touch the requested size */
        SDL_aligned_free(a);
    }

    /* custom memory functions are honored by SDL_malloc & friends */
    assert(SDL_SetMemoryFunctions(custom_malloc, custom_calloc, custom_realloc, custom_free));
    SDL_malloc_func mf; SDL_calloc_func cf; SDL_realloc_func rf; SDL_free_func ff;
    SDL_GetMemoryFunctions(&mf, &cf, &rf, &ff);
    assert(mf == custom_malloc && cf == custom_calloc && rf == custom_realloc && ff == custom_free);
    char *s = SDL_strdup("custom"); /* strdup goes through SDL_malloc */
    assert(s && strcmp(s, "custom") == 0);
    assert(custom_mallocs > 0);
    SDL_free(s);
    assert(custom_frees > 0);
    /* restore the real allocator */
    SDL_malloc_func omf; SDL_calloc_func ocf; SDL_realloc_func orf; SDL_free_func off;
    SDL_GetOriginalMemoryFunctions(&omf, &ocf, &orf, &off);
    assert(SDL_SetMemoryFunctions(omf, ocf, orf, off));
    SDL_GetMemoryFunctions(&mf, &cf, &rf, &ff);
    assert(mf == omf && cf == ocf && rf == orf && ff == off);
    /* invalid arguments rejected */
    assert(!SDL_SetMemoryFunctions(NULL, ocf, orf, off));
    assert(SDL_GetNumAllocations() == -1); /* not tracked, like stock SDL3 */

    /* --- ctype --- */
    assert(SDL_isdigit('5') && !SDL_isdigit('x'));
    assert(SDL_isalpha('a') && SDL_isalpha('Z') && !SDL_isalpha('0'));
    assert(SDL_isalnum('9') && !SDL_isalnum('-'));
    assert(SDL_isspace(' ') && SDL_isspace('\t') && !SDL_isspace('a'));
    assert(SDL_isxdigit('f') && SDL_isxdigit('F') && SDL_isxdigit('9') && !SDL_isxdigit('g'));
    assert(SDL_isupper('A') && !SDL_isupper('a'));
    assert(SDL_islower('a') && !SDL_islower('A'));
    assert(SDL_isblank(' ') && SDL_isblank('\t') && !SDL_isblank('\n'));
    assert(SDL_isprint('!') && !SDL_isprint('\n'));
    assert(SDL_isgraph('!') && !SDL_isgraph(' '));
    assert(SDL_iscntrl('\0') && SDL_iscntrl('\x7f') && !SDL_iscntrl('a'));
    assert(SDL_ispunct('.') && !SDL_ispunct('a') && !SDL_ispunct(' '));
    assert(SDL_toupper('a') == 'A' && SDL_toupper('A') == 'A' && SDL_toupper('1') == '1');
    assert(SDL_tolower('A') == 'a' && SDL_tolower('a') == 'a');
    assert(SDL_abs(-7) == 7 && SDL_abs(7) == 7 && SDL_abs(0) == 0);

    /* --- strings --- */
    assert(SDL_strlen("hello") == 5);
    assert(SDL_strnlen("hello", 3) == 3);
    assert(SDL_strnlen("hello", 10) == 5);

    char buf[64];
    assert(SDL_strlcpy(buf, "hello", sizeof(buf)) == 5);
    assert(strcmp(buf, "hello") == 0);
    assert(SDL_strlcpy(buf, "way too long", 5) == 12); /* returns full length */
    assert(strcmp(buf, "way ") == 0);                  /* truncated + NUL */
    SDL_strlcpy(buf, "", sizeof(buf));
    assert(SDL_strlcat(buf, "foo", sizeof(buf)) == 3);
    assert(SDL_strlcat(buf, "bar", sizeof(buf)) == 6);
    assert(strcmp(buf, "foobar") == 0);
    /* strlcat past-end: returns what it would have needed */
    strcpy(buf, "0123456789");
    assert(SDL_strlcat(buf, "abc", 4) == 13);

    s = SDL_strdup("dup me");
    assert(s && strcmp(s, "dup me") == 0);
    SDL_free(s);
    s = SDL_strndup("truncate me", 8);
    assert(s && strcmp(s, "truncate") == 0);
    SDL_free(s);

    strcpy(buf, "abcdef");
    assert(SDL_strrev(buf) == buf && strcmp(buf, "fedcba") == 0);
    strcpy(buf, "aBc");
    SDL_strupr(buf);
    assert(strcmp(buf, "ABC") == 0);
    SDL_strlwr(buf);
    assert(strcmp(buf, "abc") == 0);

    assert(SDL_strchr("hello", 'l') != NULL && *SDL_strchr("hello", 'l') == 'l');
    assert(SDL_strchr("hello", 'z') == NULL);
    assert(SDL_strchr("hello", '\0') != NULL); /* finds the terminator */
    assert(SDL_strrchr("hello", 'l') == SDL_strchr("hello", 'l') + 1);
    assert(strcmp(SDL_strstr("needle in a haystack", "haystack"), "haystack") == 0);
    assert(SDL_strstr("short", "longer needle") == NULL);
    assert(SDL_strnstr("abcabc", "ca", 6) != NULL);
    assert(SDL_strnstr("abcabc", "ca", 3) == NULL); /* doesn't fit in maxlen */
    assert(SDL_strcasestr("aBcDeF", "bcd") != NULL); /* matches "BcD" across case */
    assert(strcmp(SDL_strcasestr("aBcDeF", "BCD"), "BcDeF") == 0);
    assert(SDL_strcasestr("aBcDeF", "xyz") == NULL);

    assert(SDL_strcmp("a", "b") < 0);
    assert(SDL_strcmp("b", "a") > 0);
    assert(SDL_strcmp("a", "a") == 0);
    assert(SDL_strncmp("abc", "abd", 2) == 0);
    assert(SDL_strncmp("abc", "abd", 3) < 0);
    assert(SDL_strcasecmp("AbC", "aBc") == 0);
    assert(SDL_strcasecmp("abc", "abd") < 0);
    assert(SDL_strncasecmp("AbCd", "aBcX", 3) == 0);
    assert(SDL_strncasecmp("AbCd", "aBcX", 4) < 0);

    assert(SDL_strpbrk("hello", "ol") != NULL);
    assert(*SDL_strpbrk("hello", "ol") == 'l');
    assert(SDL_strpbrk("hello", "xyz") == NULL);

    /* strtok_r */
    {
        char text[] = "alpha, beta,,gamma";
        char *save = NULL;
        char *tok = SDL_strtok_r(text, ", ", &save);
        assert(tok && strcmp(tok, "alpha") == 0);
        tok = SDL_strtok_r(NULL, ", ", &save);
        assert(tok && strcmp(tok, "beta") == 0);
        tok = SDL_strtok_r(NULL, ", ", &save);
        assert(tok && strcmp(tok, "gamma") == 0);
        tok = SDL_strtok_r(NULL, ", ", &save);
        assert(tok == NULL);
    }

    /* --- UTF-8 helpers --- */
    {
        const char *text = "a\xC3\xA9\xE2\x82\xAC"; /* a, é, € */
        const char *p = text;
        assert(SDL_StepUTF8(&p, NULL) == 'a');
        assert(SDL_StepUTF8(&p, NULL) == 0xE9);
        assert(SDL_StepUTF8(&p, NULL) == 0x20AC);
        assert(SDL_StepUTF8(&p, NULL) == 0);
        assert(SDL_utf8strlen(text) == 3);
        /* 2 bytes cover 'a' plus a truncated é lead byte -> counts as U+FFFD */
        assert(SDL_utf8strnlen(text, 2) == 2);
        assert(SDL_utf8strnlen(text, 1) == 1);
        assert(SDL_utf8strnlen(text, 6) == 3);
        /* invalid bytes step one at a time and produce U+FFFD */
        const char *bad = "a\xFF""b";
        const char *bp = bad;
        assert(SDL_StepUTF8(&bp, NULL) == 'a');
        assert(SDL_StepUTF8(&bp, NULL) == 0xFFFD);
        assert(SDL_StepUTF8(&bp, NULL) == 'b');
        /* stepping back lands on sequence starts */
        const char *mid = text + 3; /* second byte of é */
        const char *back = mid;
        assert(SDL_StepBackUTF8(text, &back) == 0xE9);
        assert(back == text + 1);
        /* UCS4 -> UTF-8 */
        char utf8[8];
        char *end = SDL_UCS4ToUTF8(0x20AC, utf8);
        assert((end - utf8) == 3);
        assert(memcmp(utf8, "\xE2\x82\xAC", 3) == 0);
        end = SDL_UCS4ToUTF8(0x41, utf8);
        assert((end - utf8) == 1 && utf8[0] == 'A');
        assert(SDL_UCS4ToUTF8(0x110000, utf8) != NULL); /* clamps to U+FFFD */
        assert((unsigned char)utf8[0] == 0xEF);         /* U+FFFD = EF BF BD */
        /* utf8strlcpy never splits a codepoint: 3-byte budget fits 'a' + é
           exactly (the é lead byte's full sequence fits), returns 3 */
        char small[4];
        assert(SDL_utf8strlcpy(small, text, sizeof(small)) == 3);
        assert(memcmp(small, "a\xC3\xA9", 4) == 0);
        /* 2-byte budget must drop the é entirely rather than split it */
        char tiny[3];
        assert(SDL_utf8strlcpy(tiny, text, sizeof(tiny)) == 1);
        assert(strcmp(tiny, "a") == 0);
    }

    /* --- number formatting --- */
    assert(SDL_itoa(1234, buf, 10) == buf && strcmp(buf, "1234") == 0);
    SDL_itoa(-1234, buf, 10);
    assert(strcmp(buf, "-1234") == 0);
    SDL_itoa(0, buf, 10);
    assert(strcmp(buf, "0") == 0);
    SDL_uitoa(255, buf, 16);
    assert(strcmp(buf, "FF") == 0);
    SDL_ltoa(-1234567890L, buf, 10);
    assert(strcmp(buf, "-1234567890") == 0);
    SDL_ultoa(18446744073709551615ULL, buf, 16);
    assert(strcmp(buf, "FFFFFFFFFFFFFFFF") == 0);
    SDL_lltoa(-9223372036854775807LL - 1, buf, 10);
    assert(strcmp(buf, "-9223372036854775808") == 0);
    SDL_ulltoa(18446744073709551615ULL, buf, 10);
    assert(strcmp(buf, "18446744073709551615") == 0);
    SDL_itoa(35, buf, 36);
    assert(strcmp(buf, "Z") == 0);

    /* --- number parsing --- */
    assert(SDL_atoi("42") == 42);
    assert(SDL_atoi("  -17xyz") == -17);
    assert(SDL_atof("2.5") == 2.5);
    assert(SDL_strtol("0x1F", NULL, 16) == 31);
    assert(SDL_strtol("777", NULL, 8) == 0777);
    assert(SDL_strtol("101", NULL, 2) == 5);
    char *endp = NULL;
    assert(SDL_strtol("12ab", &endp, 10) == 12 && endp && *endp == 'a');
    assert(SDL_strtol("-9223372036854775808", NULL, 10) == LONG_MIN);
    assert(SDL_strtoul("4294967295", NULL, 10) == 4294967295UL);
    assert(SDL_strtoull("18446744073709551615", NULL, 10) == 18446744073709551615ULL);
    assert(SDL_strtoll("-9223372036854775808", NULL, 10) == (-9223372036854775807LL - 1));
    double d = SDL_strtod("3.14159e2", &endp);
    assert(d > 314.158 && d < 314.160);
    assert(endp && *endp == '\0');
    assert(SDL_strtod("  -0.5x", &endp) == -0.5 && *endp == 'x');

    /* --- printf --- */
    assert(SDL_snprintf(buf, sizeof(buf), "%d %s", 42, "x") == 4);
    assert(strcmp(buf, "42 x") == 0);
    assert(SDL_snprintf(buf, 4, "%d %s", 42, "xyz") == 6); /* would-be length */
    assert(strcmp(buf, "42 ") == 0);                      /* truncated + NUL */
    assert(SDL_snprintf(buf, sizeof(buf), NULL) == 0);    /* NULL fmt -> "" */
    {
        char *astr = NULL;
        int len = SDL_asprintf(&astr, "%s=%d", "answer", 42);
        assert(len == 9);
        assert(astr && strcmp(astr, "answer=42") == 0);
        SDL_free(astr);
    }

    /* --- memset4 / memcmp / memcpy exports --- */
    {
        Uint32 block[8];
        SDL_memset4(block, 0xDEADBEEF, 8);
        for (int i = 0; i < 8; i++) {
            assert(block[i] == 0xDEADBEEF);
        }
        Uint32 other[8];
        SDL_memcpy(other, block, sizeof(other));
        assert(SDL_memcmp(block, other, sizeof(block)) == 0);
        other[3] = 0;
        assert(SDL_memcmp(block, other, sizeof(block)) != 0);
        SDL_memmove(block, (char *)block + 4, sizeof(block) - 4);
        assert(block[0] == 0xDEADBEEF);
    }

    /* --- environment --- */
    {
        /* set before first SDL_GetEnvironment(): must show up in the
           populated global environment */
        setenv("SDLOP_TEST_PRESET", "preset", 1);

        SDL_Environment *env = SDL_GetEnvironment();
        assert(env);
        assert(SDL_GetEnvironment() == env); /* same global object */

        assert(SDL_SetEnvironmentVariable(env, "SDLOP_TEST_VAR", "one", true));
        assert(strcmp(SDL_GetEnvironmentVariable(env, "SDLOP_TEST_VAR"), "one") == 0);
        /* no overwrite -> success but value kept */
        assert(SDL_SetEnvironmentVariable(env, "SDLOP_TEST_VAR", "two", false));
        assert(strcmp(SDL_GetEnvironmentVariable(env, "SDLOP_TEST_VAR"), "one") == 0);
        /* overwrite works */
        assert(SDL_SetEnvironmentVariable(env, "SDLOP_TEST_VAR", "two", true));
        assert(strcmp(SDL_GetEnvironmentVariable(env, "SDLOP_TEST_VAR"), "two") == 0);
        /* the returned string survives later changes (persistent copy) */
        const char *snapshot = SDL_GetEnvironmentVariable(env, "SDLOP_TEST_VAR");
        assert(SDL_SetEnvironmentVariable(env, "SDLOP_TEST_VAR", "three", true));
        assert(strcmp(snapshot, "two") == 0);
        assert(strcmp(SDL_GetEnvironmentVariable(env, "SDLOP_TEST_VAR"), "three") == 0);

        /* invalid names rejected */
        assert(!SDL_SetEnvironmentVariable(env, "BAD=NAME", "x", true));
        assert(!SDL_SetEnvironmentVariable(env, "", "x", true));
        assert(!SDL_SetEnvironmentVariable(env, "OK", NULL, true));
        assert(SDL_GetEnvironmentVariable(env, "SDLOP_DEFINITELY_NOT_SET") == NULL);

        /* the process environment was populated into the global env */
        assert(strcmp(SDL_GetEnvironmentVariable(env, "SDLOP_TEST_PRESET"), "preset") == 0);

        /* enumerate */
        char **vars = SDL_GetEnvironmentVariables(env);
        assert(vars);
        bool found = false;
        for (int i = 0; vars[i]; i++) {
            if (strcmp(vars[i], "SDLOP_TEST_VAR=three") == 0) {
                found = true;
            }
        }
        assert(found);
        SDL_free(vars);

        /* unset */
        assert(SDL_UnsetEnvironmentVariable(env, "SDLOP_TEST_VAR"));
        assert(SDL_GetEnvironmentVariable(env, "SDLOP_TEST_VAR") == NULL);
        assert(SDL_UnsetEnvironmentVariable(env, "SDLOP_TEST_VAR")); /* missing: success */

        /* env==NULL rejected */
        assert(!SDL_SetEnvironmentVariable(NULL, "X", "y", true));
        assert(SDL_GetEnvironmentVariables(NULL) == NULL);

        /* custom environment, not populated */
        SDL_Environment *custom = SDL_CreateEnvironment(false);
        assert(custom);
        assert(SDL_GetEnvironmentVariable(custom, "PATH") == NULL);
        assert(SDL_SetEnvironmentVariable(custom, "MINE", "1", true));
        assert(strcmp(SDL_GetEnvironmentVariable(custom, "MINE"), "1") == 0);
        SDL_DestroyEnvironment(custom);
        /* destroying the global one is refused */
        SDL_DestroyEnvironment(env);
        assert(SDL_GetEnvironment() == env);
    }

    /* --- unsafe process-env variants --- */
    assert(SDL_setenv_unsafe("SDLOP_UNSAFE_VAR", "hello", 1) == 0);
    assert(strcmp(SDL_getenv_unsafe("SDLOP_UNSAFE_VAR"), "hello") == 0);
    /* mirrored into the SDL environment too */
    assert(strcmp(SDL_getenv("SDLOP_UNSAFE_VAR"), "hello") == 0);
    assert(SDL_setenv_unsafe("SDLOP_UNSAFE_VAR", "nope", 0) == 0);
    assert(strcmp(SDL_getenv_unsafe("SDLOP_UNSAFE_VAR"), "hello") == 0);
    assert(SDL_unsetenv_unsafe("SDLOP_UNSAFE_VAR") == 0);
    assert(SDL_getenv_unsafe("SDLOP_UNSAFE_VAR") == NULL);
    assert(SDL_getenv("SDLOP_UNSAFE_VAR") == NULL);
    assert(SDL_setenv_unsafe("BAD=NAME", "x", 1) == -1);
    assert(SDL_getenv_unsafe("") == NULL);

    printf("test_stdinc: OK\n");
    return 0;
}
