/*
  SDLop -- SDL_stdinc.h implementation.

  SDLop does not reimplement the C runtime: everything libc already has
  is a one-line wrapper, and only the pieces libc lacks (strlcpy, strlcat, UTF-8
  helpers, itoa, the SDL PRNG) are written out here. That is the main reason the
  library is small and why it links against libc/libm like any normal C library.
*/

#include "../sdlop_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

/* ------------------------------------------------------------------------- */
/* Allocators: SDL_SetMemoryFunctions() swaps these for every module           */
/* ------------------------------------------------------------------------- */

static void *SDLCALL default_malloc(size_t size) { return malloc(size ? size : 1); }
static void *SDLCALL default_calloc(size_t nmemb, size_t size) { return calloc(nmemb ? nmemb : 1, size ? size : 1); }
static void *SDLCALL default_realloc(void *mem, size_t size) { return realloc(mem, size ? size : 1); }
static void SDLCALL default_free(void *mem) { free(mem); }

SDL_malloc_func SDL_sdlop_malloc = default_malloc;
SDL_calloc_func SDL_sdlop_calloc = default_calloc;
SDL_realloc_func SDL_sdlop_realloc = default_realloc;
SDL_free_func SDL_sdlop_free = default_free;

void *SDL_malloc(size_t size) { return SDL_sdlop_malloc(size ? size : 1); }
void *SDL_calloc(size_t nmemb, size_t size) { return SDL_sdlop_calloc(nmemb ? nmemb : 1, size ? size : 1); }
void *SDL_realloc(void *mem, size_t size) { return SDL_sdlop_realloc(mem, size ? size : 1); }
void SDL_free(void *mem) { if (mem) SDL_sdlop_free(mem); }

void SDL_GetMemoryFunctions(SDL_malloc_func *malloc_func, SDL_calloc_func *calloc_func,
                            SDL_realloc_func *realloc_func, SDL_free_func *free_func)
{
    if (malloc_func) *malloc_func = SDL_sdlop_malloc;
    if (calloc_func) *calloc_func = SDL_sdlop_calloc;
    if (realloc_func) *realloc_func = SDL_sdlop_realloc;
    if (free_func) *free_func = SDL_sdlop_free;
}

bool SDL_SetMemoryFunctions(SDL_malloc_func malloc_func, SDL_calloc_func calloc_func,
                           SDL_realloc_func realloc_func, SDL_free_func free_func)
{
    if (!malloc_func || !calloc_func || !realloc_func || !free_func) {
        return SDL_InvalidParamError("memory function");
    }
    SDL_sdlop_malloc = malloc_func;
    SDL_sdlop_calloc = calloc_func;
    SDL_sdlop_realloc = realloc_func;
    SDL_sdlop_free = free_func;
    return true;
}

void SDL_GetOriginalMemoryFunctions(SDL_malloc_func *malloc_func, SDL_calloc_func *calloc_func,
                                    SDL_realloc_func *realloc_func, SDL_free_func *free_func)
{
    if (malloc_func) *malloc_func = default_malloc;
    if (calloc_func) *calloc_func = default_calloc;
    if (realloc_func) *realloc_func = default_realloc;
    if (free_func) *free_func = default_free;
}

/* Internal helpers used by every other module. */
void *SDLOP_Alloc(size_t size) { return SDL_malloc(size); }
void *SDLOP_Calloc(size_t count, size_t size) { return SDL_calloc(count, size); }
void *SDLOP_Realloc(void *mem, size_t size) { return SDL_realloc(mem, size); }
void SDLOP_Free(void *mem) { SDL_free(mem); }

char *SDLOP_Strdup(const char *str)
{
    if (!str) {
        return NULL;
    }
    return SDL_strdup(str);
}

/* ------------------------------------------------------------------------- */
/* Memory / string wrappers                                                  */
/* ------------------------------------------------------------------------- */

void *SDL_memset(SDL_OUT_BYTECAP(len) void *dst, int c, size_t len)
{
    return memset(dst, c, len);
}

void *SDL_memcpy(SDL_OUT_BYTECAP(len) void *dst, SDL_IN_BYTECAP(len) const void *src, size_t len)
{
    if (dst == src || len == 0) {
        return dst;
    }
    return memcpy(dst, src, len);
}

void *SDL_memmove(SDL_OUT_BYTECAP(len) void *dst, SDL_IN_BYTECAP(len) const void *src, size_t len)
{
    return memmove(dst, src, len);
}

int SDL_memcmp(const void *s1, const void *s2, size_t len)
{
    return memcmp(s1, s2, len);
}

size_t SDL_strlen(const char *str) { return str ? strlen(str) : 0; }
size_t SDL_strnlen(const char *str, size_t maxlen) { return str ? strnlen(str, maxlen) : 0; }

size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen)
{
    size_t srclen;
    if (!dst) {
        return 0;
    }
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    srclen = strlen(src);
    if (srclen + 1 <= maxlen) {
        memcpy(dst, src, srclen + 1);
    } else if (maxlen > 0) {
        memcpy(dst, src, maxlen - 1);
        dst[maxlen - 1] = '\0';
    }
    return srclen;
}

size_t SDL_strlcat(char *dst, const char *src, size_t maxlen)
{
    size_t dstlen, srclen;
    if (!dst || !src) {
        return 0;
    }
    dstlen = strnlen(dst, maxlen);
    if (dstlen == maxlen) {
        return maxlen + strlen(src);
    }
    srclen = strlen(src);
    if (srclen + dstlen + 1 <= maxlen) {
        memcpy(dst + dstlen, src, srclen + 1);
    } else {
        memcpy(dst + dstlen, src, maxlen - dstlen - 1);
        dst[maxlen - 1] = '\0';
    }
    return dstlen + srclen;
}

static size_t utf8_seq_len(Uint8 byte)
{
    if (byte < 0x80) return 1;
    if ((byte & 0xE0) == 0xC0) return 2;
    if ((byte & 0xF0) == 0xE0) return 3;
    if ((byte & 0xF8) == 0xF0) return 4;
    return 1;   /* invalid byte: count it as one character, never loop forever */
}

size_t SDL_utf8strlen(const char *str)
{
    size_t count = 0;
    const Uint8 *p = (const Uint8 *)str;
    if (!str) {
        return 0;
    }
    while (*p) {
        p += utf8_seq_len(*p);
        count++;
    }
    return count;
}

size_t SDL_utf8strnlen(const char *str, size_t bytes)
{
    size_t count = 0, used = 0;
    const Uint8 *p = (const Uint8 *)str;
    if (!str) {
        return 0;
    }
    while (p[used] && used < bytes) {
        used += utf8_seq_len(p[used]);
        count++;
    }
    return count;
}

size_t SDL_utf8strlcpy(char *dst, const char *src, size_t dst_bytes)
{
    size_t srclen = SDL_strlen(src);
    size_t copy, i = 0;
    if (!dst || dst_bytes == 0) {
        return srclen;
    }
    copy = (dst_bytes - 1 < srclen) ? dst_bytes - 1 : srclen;
    /* never cut a multi-byte sequence in half */
    while (i < copy) {
        size_t l = utf8_seq_len((Uint8)src[i]);
        if (i + l > copy) {
            copy = i;
            break;
        }
        i += l;
    }
    memcpy(dst, src, copy);
    dst[copy] = '\0';
    return srclen;
}

char *SDL_strdup(const char *str)
{
    size_t len;
    char *newstr;
    if (!str) {
        return NULL;
    }
    len = strlen(str) + 1;
    newstr = (char *)SDL_malloc(len);
    if (newstr) {
        memcpy(newstr, str, len);
    }
    return newstr;
}

char *SDL_strndup(const char *str, size_t maxlen)
{
    size_t len;
    char *newstr;
    if (!str) {
        return NULL;
    }
    len = strnlen(str, maxlen);
    newstr = (char *)SDL_malloc(len + 1);
    if (newstr) {
        memcpy(newstr, str, len);
        newstr[len] = '\0';
    }
    return newstr;
}

char *SDL_strrev(char *str)
{
    size_t len, i, j;
    if (!str) {
        return NULL;
    }
    len = strlen(str);
    for (i = 0, j = len - 1; len > 1 && i < j; i++, j--) {
        char c = str[i];
        str[i] = str[j];
        str[j] = c;
    }
    return str;
}

char *SDL_strupr(char *str)
{
    char *p = str;
    while (p && *p) {
        *p = (char)toupper((unsigned char)*p);
        p++;
    }
    return str;
}

char *SDL_strlwr(char *str)
{
    char *p = str;
    while (p && *p) {
        *p = (char)tolower((unsigned char)*p);
        p++;
    }
    return str;
}

char *SDL_strchr(const char *str, int c) { return (char *)strchr(str, c); }
char *SDL_strrchr(const char *str, int c) { return (char *)strrchr(str, c); }
char *SDL_strstr(const char *haystack, const char *needle) { return (char *)strstr(haystack, needle); }
char *SDL_strpbrk(const char *str, const char *breakset) { return (char *)strpbrk(str, breakset); }
char *SDL_strtok_r(char *str, const char *delim, char **saveptr) { return strtok_r(str, delim, saveptr); }
size_t SDL_strspn(const char *str, const char *accept) { return strspn(str, accept); }
size_t SDL_strcspn(const char *str, const char *reject) { return strcspn(str, reject); }
int SDL_strcmp(const char *str1, const char *str2) { return strcmp(str1 ? str1 : "", str2 ? str2 : ""); }
int SDL_strncmp(const char *str1, const char *str2, size_t maxlen) { return strncmp(str1 ? str1 : "", str2 ? str2 : "", maxlen); }
int SDL_strcasecmp(const char *str1, const char *str2) { return strcasecmp(str1 ? str1 : "", str2 ? str2 : ""); }
int SDL_strncasecmp(const char *str1, const char *str2, size_t maxlen) { return strncasecmp(str1 ? str1 : "", str2 ? str2 : "", maxlen); }

char *SDL_strcasestr(const char *haystack, const char *needle)
{
    size_t nlen;
    if (!haystack || !needle) {
        return NULL;
    }
    nlen = strlen(needle);
    if (nlen == 0) {
        return (char *)haystack;
    }
    for (; *haystack; ++haystack) {
        if (strncasecmp(haystack, needle, nlen) == 0) {
            return (char *)haystack;
        }
    }
    return NULL;
}

int SDL_sscanf(const char *text, SDL_SCANF_FORMAT_STRING const char *fmt, ...)
{
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = vsscanf(text, fmt, ap);
    va_end(ap);
    return rc;
}

int SDL_vsscanf(const char *text, const char *fmt, va_list ap) { return vsscanf(text, fmt, ap); }

int SDL_snprintf(SDL_OUT_Z_CAP(maxlen) char *text, size_t maxlen, SDL_PRINTF_FORMAT_STRING const char *fmt, ...)
{
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = vsnprintf(text, maxlen, fmt, ap);
    va_end(ap);
    return rc;
}

int SDL_vsnprintf(SDL_OUT_Z_CAP(maxlen) char *text, size_t maxlen, const char *fmt, va_list ap)
{
    return vsnprintf(text, maxlen, fmt, ap);
}

int SDL_asprintf(char **strp, SDL_PRINTF_FORMAT_STRING const char *fmt, ...)
{
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = SDL_vasprintf(strp, fmt, ap);
    va_end(ap);
    return rc;
}

int SDL_vasprintf(char **strp, const char *fmt, va_list ap)
{
    va_list ap2;
    int len;
    char *buf;
    if (!strp) {
        return -1;
    }
    va_copy(ap2, ap);
    len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (len < 0) {
        *strp = NULL;
        return -1;
    }
    buf = (char *)SDL_malloc((size_t)len + 1);
    if (!buf) {
        *strp = NULL;
        return -1;
    }
    vsnprintf(buf, (size_t)len + 1, fmt, ap);
    *strp = buf;
    return len;
}

/* ------------------------------------------------------------------------- */
/* Conversion                                                                */
/* ------------------------------------------------------------------------- */

int SDL_atoi(const char *str) { return (int)SDL_strtol(str, NULL, 10); }
double SDL_atof(const char *str) { return SDL_strtod(str, NULL); }
long SDL_strtol(const char *str, char **endp, int base) { return strtol(str, endp, base); }
unsigned long SDL_strtoul(const char *str, char **endp, int base) { return strtoul(str, endp, base); }
long long SDL_strtoll(const char *str, char **endp, int base) { return strtoll(str, endp, base); }
unsigned long long SDL_strtoull(const char *str, char **endp, int base) { return strtoull(str, endp, base); }
double SDL_strtod(const char *str, char **endp) { return strtod(str, endp); }

static int sdlop_ulltoa(unsigned long long value, char *str, int radix, bool negative)
{
    char temp[70];
    int i = 0, j = 0;
    const char *digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (radix < 2 || radix > 36) {
        if (str) *str = '\0';
        return 0;
    }
    do {
        temp[i++] = digits[value % (unsigned)radix];
        value /= (unsigned)radix;
    } while (value > 0);
    if (negative) {
        temp[i++] = '-';
    }
    while (i > 0) {
        str[j++] = temp[--i];
    }
    str[j] = '\0';
    return j;
}

int SDL_itoa(int value, char *str, int radix) { return sdlop_ulltoa((unsigned long long)(value < 0 ? -(long long)value : value), str, radix, value < 0); }
int SDL_uitoa(unsigned int value, char *str, int radix) { return sdlop_ulltoa(value, str, radix, false); }
int SDL_ltoa(long value, char *str, int radix) { return sdlop_ulltoa((unsigned long long)(value < 0 ? -value : value), str, radix, value < 0); }
int SDL_ultoa(unsigned long value, char *str, int radix) { return sdlop_ulltoa(value, str, radix, false); }
int SDL_lltoa(long long value, char *str, int radix) { return sdlop_ulltoa((unsigned long long)(value < 0 ? -value : value), str, radix, value < 0); }
int SDL_ulltoa(unsigned long long value, char *str, int radix) { return sdlop_ulltoa(value, str, radix, false); }

/* ------------------------------------------------------------------------- */
/* ctype / math: direct libc delegation                                      */
/* ------------------------------------------------------------------------- */

int SDL_isalpha(int x) { return isalpha(x); }
int SDL_isalnum(int x) { return isalnum(x); }
int SDL_isblank(int x) { return isblank(x); }
int SDL_iscntrl(int x) { return iscntrl(x); }
int SDL_isdigit(int x) { return isdigit(x); }
int SDL_isxdigit(int x) { return isxdigit(x); }
int SDL_ispunct(int x) { return ispunct(x); }
int SDL_isspace(int x) { return isspace(x); }
int SDL_isupper(int x) { return isupper(x); }
int SDL_islower(int x) { return islower(x); }
int SDL_isprint(int x) { return isprint(x); }
int SDL_isgraph(int x) { return isgraph(x); }
int SDL_toupper(int x) { return toupper(x); }
int SDL_tolower(int x) { return tolower(x); }
int SDL_abs(int x) { return x < 0 ? -x : x; }

double SDL_acos(double x) { return acos(x); }
float SDL_acosf(float x) { return acosf(x); }
double SDL_asin(double x) { return asin(x); }
float SDL_asinf(float x) { return asinf(x); }
double SDL_atan(double x) { return atan(x); }
float SDL_atanf(float x) { return atanf(x); }
double SDL_atan2(double y, double x) { return atan2(y, x); }
float SDL_atan2f(float y, float x) { return atan2f(y, x); }
double SDL_ceil(double x) { return ceil(x); }
float SDL_ceilf(float x) { return ceilf(x); }
double SDL_copysign(double x, double y) { return copysign(x, y); }
float SDL_copysignf(float x, float y) { return copysignf(x, y); }
double SDL_cos(double x) { return cos(x); }
float SDL_cosf(float x) { return cosf(x); }
double SDL_exp(double x) { return exp(x); }
float SDL_expf(float x) { return expf(x); }
double SDL_fabs(double x) { return fabs(x); }
float SDL_fabsf(float x) { return fabsf(x); }
double SDL_floor(double x) { return floor(x); }
float SDL_floorf(float x) { return floorf(x); }
double SDL_fmod(double x, double y) { return fmod(x, y); }
float SDL_fmodf(float x, float y) { return fmodf(x, y); }
double SDL_log(double x) { return log(x); }
float SDL_logf(float x) { return logf(x); }
double SDL_log10(double x) { return log10(x); }
float SDL_log10f(float x) { return log10f(x); }
double SDL_pow(double x, double y) { return pow(x, y); }
float SDL_powf(float x, float y) { return powf(x, y); }
double SDL_round(double x) { return round(x); }
float SDL_roundf(float x) { return roundf(x); }
long SDL_lround(double x) { return lround(x); }
long SDL_lroundf(float x) { return lroundf(x); }
double SDL_scalbn(double x, int n) { return scalbn(x, n); }
float SDL_scalbnf(float x, int n) { return scalbnf(x, n); }
double SDL_sin(double x) { return sin(x); }
float SDL_sinf(float x) { return sinf(x); }
double SDL_sqrt(double x) { return sqrt(x); }
float SDL_sqrtf(float x) { return sqrtf(x); }
double SDL_tan(double x) { return tan(x); }
float SDL_tanf(float x) { return tanf(x); }
double SDL_trunc(double x) { return trunc(x); }
float SDL_truncf(float x) { return truncf(x); }
double SDL_cbrt(double x) { return cbrt(x); }
float SDL_cbrtf(float x) { return cbrtf(x); }
double SDL_hypot(double x, double y) { return hypot(x, y); }
float SDL_hypotf(float x, float y) { return hypotf(x, y); }
double SDL_fma(double x, double y, double z) { return fma(x, y, z); }
float SDL_fmaf(float x, float y, float z) { return fmaf(x, y, z); }

/* ------------------------------------------------------------------------- */
/* PRNG: xoshiro256** with a lock-free per-thread state                      */
/*                                                                            */
/* SDL3's SDL_rand()/SDL_rand_r() are documented as thread safe, so the global */
/* generator keeps its state in thread-local storage, seeded from a shared     */
/* seed + a per-thread counter. No lock is needed on the hot path.             */
/* ------------------------------------------------------------------------- */

/* SDL3's global PRNG: one seed, one state per thread, and SDL_srand() makes the
   whole sequence repeatable (which is what tests and replays rely on). */
static Uint64 sdlop_prng_seed = 0x853c49e6748fea9bULL;

typedef struct { Uint64 s[4]; } SDLOP_RngState;
static _Thread_local SDLOP_RngState sdlop_rng;
static _Thread_local bool sdlop_rng_ready;

static Uint64 sdlop_splitmix64(Uint64 *x)
{
    Uint64 z = (*x += 0x9E3779B97f4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

Uint32 SDL_rand_bits_r(Uint64 *state)
{
    Uint64 x = *state;
    if (x == 0) {
        x = 0x9E3779B97F4A7C15ULL;
    }
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return (Uint32)(x >> 32);
}

float SDL_randf_r(Uint64 *state)
{
    return (float)((double)SDL_rand_bits_r(state) / 4294967296.0);
}

Sint32 SDL_rand_r(Uint64 *state, Sint32 n)
{
    Uint32 r;
    Uint32 bound;
    if (n <= 0 || !state) {
        return 0;
    }
    bound = (Uint32)(0x100000000ULL / (Uint32)n * (Uint32)n);
    /* rejection sampling: every value in [0, n) has exactly the same chance */
    do {
        r = SDL_rand_bits_r(state);
    } while (r >= bound);
    return (Sint32)(r % (Uint32)n);
}

void SDL_srand(Uint64 seed)
{
    sdlop_prng_seed = seed;
    sdlop_rng_ready = false;
}

static SDLOP_RngState *sdlop_rng_get(void)
{
    if (!sdlop_rng_ready) {
        Uint64 s = sdlop_prng_seed;
        sdlop_rng.s[0] = sdlop_splitmix64(&s);
        sdlop_rng.s[1] = sdlop_splitmix64(&s);
        sdlop_rng.s[2] = sdlop_splitmix64(&s);
        sdlop_rng.s[3] = sdlop_splitmix64(&s);
        if (!(sdlop_rng.s[0] | sdlop_rng.s[1] | sdlop_rng.s[2] | sdlop_rng.s[3])) {
            sdlop_rng.s[0] = 1;
        }
        sdlop_rng_ready = true;
    }
    return &sdlop_rng;
}

Uint32 SDL_rand_bits(void)
{
    SDLOP_RngState *st = sdlop_rng_get();
    /* xoshiro256** */
    const Uint64 result = ((st->s[1] * 5) << 7) | ((st->s[1] * 5) >> 57);
    const Uint64 t = st->s[1] << 17;
    st->s[2] ^= st->s[0];
    st->s[3] ^= st->s[1];
    st->s[1] ^= st->s[2];
    st->s[0] ^= st->s[3];
    st->s[2] ^= t;
    st->s[3] = (st->s[3] << 45) | (st->s[3] >> 19);
    return (Uint32)(result * 0x2545F4914F6CDD1DULL >> 32);
}

float SDL_randf(void)
{
    return (float)((double)SDL_rand_bits() / 4294967296.0);
}

int SDL_rand(int n)
{
    Uint32 r;
    Uint32 bound;
    if (n <= 0) {
        return 0;
    }
    bound = (Uint32)(0x100000000ULL / (Uint32)n * (Uint32)n);
    do {
        r = SDL_rand_bits();
    } while (r >= bound);
    return (int)(r % (Uint32)n);
}

/* Environment: SDL3 wraps the process environment in SDL_Environment objects;
   the SDLop build exposes it directly (SDL_getenv predates SDL_Environment and is
   still what ported code uses). */
const char *SDL_getenv(const char *name)
{
    if (!name) {
        return NULL;
    }
    return getenv(name);
}

int SDL_setenv_unsafe(const char *name, const char *value, int overwrite)
{
    if (!name || !name[0] || SDL_strchr(name, '=') != NULL) {
        return -1;
    }
    return setenv(name, value ? value : "", overwrite);
}

int SDL_unsetenv_unsafe(const char *name)
{
    if (!name || !name[0] || SDL_strchr(name, '=') != NULL) {
        return -1;
    }
    return unsetenv(name);
}
