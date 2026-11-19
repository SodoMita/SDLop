/*
  SDLop - SDL_stdinc.h runtime: memory, strings, ctype, math, environment,
  sorting, UTF-8, iconv, pseudo-random numbers and checksums.

  Thin wrappers over the C library wherever libc already has SDL3's exact
  semantics; the rest (ASCII-only ctype, strl*, UTF-8 stepping, the LCG of
  SDL_rand, crc16/crc32/murmur3, SDL_Environment) follows SDL3's
  src/stdlib so results are bit-identical to stock SDL3.

  Parts adapted from SDL3 (src/stdlib/SDL_string.c, SDL_random.c,
  SDL_crc16.c, SDL_crc32.c, SDL_murmur3.c, SDL_iconv.c),
  Copyright (C) 1997-2026 Sam Lantinga, zlib license.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "internal/sdlop_internal.h"

#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <wchar.h>
#include <wctype.h>

/* SDL_stdinc.h maps these to the libc names; we define the real symbols */
#undef SDL_memcpy
#undef SDL_memmove
#undef SDL_memset

extern char **environ;

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

static void *SDLCALL real_malloc(size_t size) { return malloc(size); }
static void *SDLCALL real_calloc(size_t n, size_t size) { return calloc(n, size); }
static void *SDLCALL real_realloc(void *mem, size_t size) { return realloc(mem, size); }
static void SDLCALL real_free(void *mem) { free(mem); }

static struct
{
    SDL_malloc_func malloc_func;
    SDL_calloc_func calloc_func;
    SDL_realloc_func realloc_func;
    SDL_free_func free_func;
} s_mem = { real_malloc, real_calloc, real_realloc, real_free };

void *SDL_malloc(size_t size)
{
    if (!size) {
        size = 1; /* SDL3: never returns NULL for 0 bytes */
    }
    void *mem = s_mem.malloc_func(size);
    if (!mem) {
        SDL_OutOfMemory();
    }
    return mem;
}

void *SDL_calloc(size_t nmemb, size_t size)
{
    if (!nmemb || !size) {
        nmemb = size = 1;
    }
    void *mem = s_mem.calloc_func(nmemb, size);
    if (!mem) {
        SDL_OutOfMemory();
    }
    return mem;
}

void *SDL_realloc(void *mem, size_t size)
{
    if (!size) {
        size = 1;
    }
    void *r = s_mem.realloc_func(mem, size);
    if (!r) {
        SDL_OutOfMemory();
    }
    return r;
}

void SDL_free(void *mem)
{
    if (mem) {
        s_mem.free_func(mem);
    }
}

void SDL_GetOriginalMemoryFunctions(SDL_malloc_func *malloc_func, SDL_calloc_func *calloc_func,
                                    SDL_realloc_func *realloc_func, SDL_free_func *free_func)
{
    if (malloc_func) *malloc_func = real_malloc;
    if (calloc_func) *calloc_func = real_calloc;
    if (realloc_func) *realloc_func = real_realloc;
    if (free_func) *free_func = real_free;
}

void SDL_GetMemoryFunctions(SDL_malloc_func *malloc_func, SDL_calloc_func *calloc_func,
                            SDL_realloc_func *realloc_func, SDL_free_func *free_func)
{
    if (malloc_func) *malloc_func = s_mem.malloc_func;
    if (calloc_func) *calloc_func = s_mem.calloc_func;
    if (realloc_func) *realloc_func = s_mem.realloc_func;
    if (free_func) *free_func = s_mem.free_func;
}

bool SDL_SetMemoryFunctions(SDL_malloc_func malloc_func, SDL_calloc_func calloc_func,
                            SDL_realloc_func realloc_func, SDL_free_func free_func)
{
    if (!malloc_func) return SDL_InvalidParamError("malloc_func");
    if (!calloc_func) return SDL_InvalidParamError("calloc_func");
    if (!realloc_func) return SDL_InvalidParamError("realloc_func");
    if (!free_func) return SDL_InvalidParamError("free_func");
    s_mem.malloc_func = malloc_func;
    s_mem.calloc_func = calloc_func;
    s_mem.realloc_func = realloc_func;
    s_mem.free_func = free_func;
    return true;
}

int SDL_GetNumAllocations(void)
{
    return -1; /* allocation tracking is not compiled in (same as SDL3 release builds) */
}

void *SDL_aligned_alloc(size_t alignment, size_t size)
{
    /* SDL3 layout: over-allocate through SDL_malloc, store the original
     * pointer just below the aligned block (so SDL_SetMemoryFunctions
     * allocators work and SDL_aligned_free pairs with them). */
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    if ((alignment & (alignment - 1)) != 0) {
        SDL_SetError("alignment must be a power of two");
        return NULL;
    }
    size_t padding = alignment - (size % alignment);
    if (padding == alignment) {
        padding = 0;
    }
    size_t total;
    if (__builtin_add_overflow(size, padding, &total) ||
        __builtin_add_overflow(total, alignment + sizeof(void *), &total)) {
        SDL_OutOfMemory();
        return NULL;
    }
    Uint8 *original = (Uint8 *)SDL_malloc(total);
    if (!original) {
        return NULL;
    }
    uintptr_t addr = ((uintptr_t)original + sizeof(void *) + alignment - 1) & ~(uintptr_t)(alignment - 1);
    ((void **)addr)[-1] = original;
    return (void *)addr;
}

void SDL_aligned_free(void *mem)
{
    if (mem) {
        SDL_free(((void **)mem)[-1]);
    }
}

void *SDL_memcpy(void *dst, const void *src, size_t len)
{
    return memcpy(dst, src, len);
}

void *SDL_memmove(void *dst, const void *src, size_t len)
{
    return memmove(dst, src, len);
}

void *SDL_memset(void *dst, int c, size_t len)
{
    return memset(dst, c, len);
}

void *SDL_memset4(void *dst, Uint32 val, size_t dwords)
{
    Uint32 *p = (Uint32 *)dst;
    for (size_t i = 0; i < dwords; i++) {
        p[i] = val;
    }
    return dst;
}

/* ------------------------------------------------------------------ */
/* ctype: ASCII only, locale independent (SDL3 semantics)              */
/* ------------------------------------------------------------------ */

int SDL_isalpha(int x) { return ((x >= 'A') && (x <= 'Z')) || ((x >= 'a') && (x <= 'z')); }
int SDL_isdigit(int x) { return (x >= '0') && (x <= '9'); }
int SDL_isalnum(int x) { return SDL_isalpha(x) || SDL_isdigit(x); }
int SDL_isxdigit(int x) { return SDL_isdigit(x) || ((x >= 'A') && (x <= 'F')) || ((x >= 'a') && (x <= 'f')); }
int SDL_isblank(int x) { return (x == ' ') || (x == '\t'); }
int SDL_iscntrl(int x) { return ((x >= 0) && (x <= 31)) || (x == 127); }
int SDL_isspace(int x) { return (x == ' ') || ((x >= '\t') && (x <= '\r')); }
int SDL_isupper(int x) { return (x >= 'A') && (x <= 'Z'); }
int SDL_islower(int x) { return (x >= 'a') && (x <= 'z'); }
int SDL_isprint(int x) { return (x >= ' ') && (x < '\x7f'); }
int SDL_isgraph(int x) { return SDL_isprint(x) && (x != ' '); }
int SDL_ispunct(int x) { return SDL_isgraph(x) && !SDL_isalnum(x); }
int SDL_toupper(int x) { return SDL_islower(x) ? (x - 'a' + 'A') : x; }
int SDL_tolower(int x) { return SDL_isupper(x) ? (x - 'A' + 'a') : x; }

/* ------------------------------------------------------------------ */
/* libc forwarders (identical semantics)                               */
/* ------------------------------------------------------------------ */

int SDL_abs(int x)
{
    return abs(x);
}

int SDL_memcmp(const void *s1, const void *s2, size_t len)
{
    return memcmp(s1, s2, len);
}

size_t SDL_wcslen(const wchar_t *wstr)
{
    return wcslen(wstr);
}

size_t SDL_wcsnlen(const wchar_t *wstr, size_t maxlen)
{
    return wcsnlen(wstr, maxlen);
}

int SDL_wcscmp(const wchar_t *str1, const wchar_t *str2)
{
    return wcscmp(str1, str2);
}

int SDL_wcsncmp(const wchar_t *str1, const wchar_t *str2, size_t maxlen)
{
    return wcsncmp(str1, str2, maxlen);
}

int SDL_wcscasecmp(const wchar_t *str1, const wchar_t *str2)
{
    return wcscasecmp(str1, str2);
}

int SDL_wcsncasecmp(const wchar_t *str1, const wchar_t *str2, size_t maxlen)
{
    return wcsncasecmp(str1, str2, maxlen);
}

long SDL_wcstol(const wchar_t *str, wchar_t **endp, int base)
{
    return wcstol(str, endp, base);
}

size_t SDL_strlen(const char *str)
{
    return strlen(str);
}

size_t SDL_strnlen(const char *str, size_t maxlen)
{
    return strnlen(str, maxlen);
}

char * SDL_strstr(const char *haystack, const char *needle)
{
    return (char *)strstr(haystack, needle);
}

char * SDL_strcasestr(const char *haystack, const char *needle)
{
    return (char *)strcasestr(haystack, needle);
}

char * SDL_strtok_r(char *str, const char *delim, char **saveptr)
{
    return strtok_r(str, delim, saveptr);
}

int SDL_atoi(const char *str)
{
    return atoi(str);
}

double SDL_atof(const char *str)
{
    return atof(str);
}

long SDL_strtol(const char *str, char **endp, int base)
{
    return strtol(str, endp, base);
}

unsigned long SDL_strtoul(const char *str, char **endp, int base)
{
    return strtoul(str, endp, base);
}

long long SDL_strtoll(const char *str, char **endp, int base)
{
    return strtoll(str, endp, base);
}

unsigned long long SDL_strtoull(const char *str, char **endp, int base)
{
    return strtoull(str, endp, base);
}

double SDL_strtod(const char *str, char **endp)
{
    return strtod(str, endp);
}

int SDL_strcmp(const char *str1, const char *str2)
{
    return strcmp(str1, str2);
}

int SDL_strncmp(const char *str1, const char *str2, size_t maxlen)
{
    return strncmp(str1, str2, maxlen);
}

int SDL_strcasecmp(const char *str1, const char *str2)
{
    return strcasecmp(str1, str2);
}

int SDL_strncasecmp(const char *str1, const char *str2, size_t maxlen)
{
    return strncasecmp(str1, str2, maxlen);
}

char * SDL_strpbrk(const char *str, const char *breakset)
{
    return (char *)strpbrk(str, breakset);
}

int SDL_vsscanf(const char *text, const char *fmt, va_list ap)
{
    return vsscanf(text, fmt, ap);
}

double SDL_acos(double x)
{
    return acos(x);
}

float SDL_acosf(float x)
{
    return acosf(x);
}

double SDL_asin(double x)
{
    return asin(x);
}

float SDL_asinf(float x)
{
    return asinf(x);
}

double SDL_atan(double x)
{
    return atan(x);
}

float SDL_atanf(float x)
{
    return atanf(x);
}

double SDL_atan2(double y, double x)
{
    return atan2(y, x);
}

float SDL_atan2f(float y, float x)
{
    return atan2f(y, x);
}

double SDL_ceil(double x)
{
    return ceil(x);
}

float SDL_ceilf(float x)
{
    return ceilf(x);
}

double SDL_copysign(double x, double y)
{
    return copysign(x, y);
}

float SDL_copysignf(float x, float y)
{
    return copysignf(x, y);
}

double SDL_cos(double x)
{
    return cos(x);
}

float SDL_cosf(float x)
{
    return cosf(x);
}

double SDL_exp(double x)
{
    return exp(x);
}

float SDL_expf(float x)
{
    return expf(x);
}

double SDL_fabs(double x)
{
    return fabs(x);
}

float SDL_fabsf(float x)
{
    return fabsf(x);
}

double SDL_floor(double x)
{
    return floor(x);
}

float SDL_floorf(float x)
{
    return floorf(x);
}

double SDL_trunc(double x)
{
    return trunc(x);
}

float SDL_truncf(float x)
{
    return truncf(x);
}

double SDL_fmod(double x, double y)
{
    return fmod(x, y);
}

float SDL_fmodf(float x, float y)
{
    return fmodf(x, y);
}

double SDL_log(double x)
{
    return log(x);
}

float SDL_logf(float x)
{
    return logf(x);
}

double SDL_log10(double x)
{
    return log10(x);
}

float SDL_log10f(float x)
{
    return log10f(x);
}

double SDL_modf(double x, double *y)
{
    return modf(x, y);
}

float SDL_modff(float x, float *y)
{
    return modff(x, y);
}

double SDL_pow(double x, double y)
{
    return pow(x, y);
}

float SDL_powf(float x, float y)
{
    return powf(x, y);
}

double SDL_round(double x)
{
    return round(x);
}

float SDL_roundf(float x)
{
    return roundf(x);
}

long SDL_lround(double x)
{
    return lround(x);
}

long SDL_lroundf(float x)
{
    return lroundf(x);
}

double SDL_scalbn(double x, int n)
{
    return scalbn(x, n);
}

float SDL_scalbnf(float x, int n)
{
    return scalbnf(x, n);
}

double SDL_sin(double x)
{
    return sin(x);
}

float SDL_sinf(float x)
{
    return sinf(x);
}

double SDL_sqrt(double x)
{
    return sqrt(x);
}

float SDL_sqrtf(float x)
{
    return sqrtf(x);
}

double SDL_tan(double x)
{
    return tan(x);
}

float SDL_tanf(float x)
{
    return tanf(x);
}

int SDL_isinf(double x) { return isinf(x); }
int SDL_isinff(float x) { return isinf(x); }
int SDL_isnan(double x) { return isnan(x); }
int SDL_isnanf(float x) { return isnan(x); }

char *SDL_strchr(const char *str, int c) { return (char *)strchr(str, c); }
char *SDL_strrchr(const char *str, int c) { return (char *)strrchr(str, c); }

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen)
{
    size_t srclen = strlen(src);
    if (maxlen > 0) {
        size_t len = SDL_min(srclen, maxlen - 1);
        memcpy(dst, src, len);
        dst[len] = '\0';
    }
    return srclen;
}

size_t SDL_strlcat(char *dst, const char *src, size_t maxlen)
{
    size_t dstlen = strlen(dst);
    size_t srclen = strlen(src);
    if (dstlen < maxlen) {
        SDL_strlcpy(dst + dstlen, src, maxlen - dstlen);
    }
    return dstlen + srclen;
}

#define UTF8_IsLeadByte(c)     ((c) >= 0xC0 && (c) <= 0xF4)
#define UTF8_IsTrailingByte(c) ((c) >= 0x80 && (c) <= 0xBF)

static size_t UTF8_GetTrailingBytes(unsigned char c)
{
    if (c >= 0xC0 && c <= 0xDF) {
        return 1;
    } else if (c >= 0xE0 && c <= 0xEF) {
        return 2;
    } else if (c >= 0xF0 && c <= 0xF4) {
        return 3;
    }
    return 0;
}

size_t SDL_utf8strlcpy(char *dst, const char *src, size_t dst_bytes)
{
    size_t bytes = 0;
    if (dst_bytes > 0) {
        size_t src_bytes = strlen(src);
        bytes = SDL_min(src_bytes, dst_bytes - 1);
        if (bytes) {
            unsigned char c = (unsigned char)src[bytes - 1];
            if (UTF8_IsLeadByte(c)) {
                --bytes;
            } else if (UTF8_IsTrailingByte(c)) {
                for (size_t i = bytes - 1; i != 0; --i) {
                    c = (unsigned char)src[i];
                    size_t trailing_bytes = UTF8_GetTrailingBytes(c);
                    if (trailing_bytes) {
                        if ((bytes - i) != (trailing_bytes + 1)) {
                            bytes = i;
                        }
                        break;
                    }
                }
            }
            memcpy(dst, src, bytes);
        }
        dst[bytes] = '\0';
    }
    return bytes;
}

char *SDL_strdup(const char *str)
{
    size_t len = strlen(str) + 1;
    char *s = (char *)SDL_malloc(len);
    if (s) {
        memcpy(s, str, len);
    }
    return s;
}

char *SDL_strndup(const char *str, size_t maxlen)
{
    size_t len = strnlen(str, maxlen);
    char *s = (char *)SDL_malloc(len + 1);
    if (s) {
        memcpy(s, str, len);
        s[len] = '\0';
    }
    return s;
}

char *SDL_strrev(char *str)
{
    size_t len = strlen(str);
    char *a = str, *b = str + len - 1;
    if (len) {
        while (a < b) {
            char c = *a;
            *a++ = *b;
            *b-- = c;
        }
    }
    return str;
}

char *SDL_strupr(char *str)
{
    for (char *s = str; *s; s++) {
        *s = (char)SDL_toupper((unsigned char)*s);
    }
    return str;
}

char *SDL_strlwr(char *str)
{
    for (char *s = str; *s; s++) {
        *s = (char)SDL_tolower((unsigned char)*s);
    }
    return str;
}

char *SDL_strnstr(const char *haystack, const char *needle, size_t maxlen)
{
    size_t length = strlen(needle);
    if (length == 0) {
        return (char *)haystack;
    }
    while (maxlen >= length && *haystack) {
        if (strncmp(haystack, needle, length) == 0) {
            return (char *)haystack;
        }
        ++haystack;
        --maxlen;
    }
    return NULL;
}

static char *ulltoa_impl(unsigned long long value, char *str, int radix)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char *p = str;
    if (radix < 2 || radix > 36) {
        radix = 10;
    }
    do {
        *p++ = digits[value % (unsigned)radix];
        value /= (unsigned)radix;
    } while (value);
    *p = '\0';
    return SDL_strrev(str);
}

static char *lltoa_impl(long long value, char *str, int radix)
{
    if (value < 0) {
        *str = '-';
        ulltoa_impl(0ULL - (unsigned long long)value, str + 1, radix);
        return str;
    }
    return ulltoa_impl((unsigned long long)value, str, radix);
}

char *SDL_itoa(int value, char *str, int radix) { return lltoa_impl(value, str, radix); }
char *SDL_uitoa(unsigned int value, char *str, int radix) { return ulltoa_impl(value, str, radix); }
char *SDL_ltoa(long value, char *str, int radix) { return lltoa_impl(value, str, radix); }
char *SDL_ultoa(unsigned long value, char *str, int radix) { return ulltoa_impl(value, str, radix); }
char *SDL_lltoa(long long value, char *str, int radix) { return lltoa_impl(value, str, radix); }
char *SDL_ulltoa(unsigned long long value, char *str, int radix) { return ulltoa_impl(value, str, radix); }

/* wide strings */
size_t SDL_wcslcpy(wchar_t *dst, const wchar_t *src, size_t maxlen)
{
    size_t srclen = wcslen(src);
    if (maxlen > 0) {
        size_t len = SDL_min(srclen, maxlen - 1);
        memcpy(dst, src, len * sizeof(wchar_t));
        dst[len] = L'\0';
    }
    return srclen;
}

size_t SDL_wcslcat(wchar_t *dst, const wchar_t *src, size_t maxlen)
{
    size_t dstlen = wcslen(dst);
    size_t srclen = wcslen(src);
    if (dstlen < maxlen) {
        SDL_wcslcpy(dst + dstlen, src, maxlen - dstlen);
    }
    return dstlen + srclen;
}

wchar_t *SDL_wcsdup(const wchar_t *wstr)
{
    size_t size = (wcslen(wstr) + 1) * sizeof(wchar_t);
    wchar_t *s = (wchar_t *)SDL_malloc(size);
    if (s) {
        memcpy(s, wstr, size);
    }
    return s;
}

wchar_t *SDL_wcsstr(const wchar_t *haystack, const wchar_t *needle)
{
    return (wchar_t *)wcsstr(haystack, needle);
}

wchar_t *SDL_wcsnstr(const wchar_t *haystack, const wchar_t *needle, size_t maxlen)
{
    size_t length = wcslen(needle);
    if (length == 0) {
        return (wchar_t *)haystack;
    }
    while (maxlen >= length && *haystack) {
        if (wcsncmp(haystack, needle, length) == 0) {
            return (wchar_t *)haystack;
        }
        ++haystack;
        --maxlen;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

char *SDL_UCS4ToUTF8(Uint32 codepoint, char *dst)
{
    if (!dst) {
        return NULL;
    }
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        codepoint = SDL_INVALID_UNICODE_CODEPOINT;
    }
    Uint8 *p = (Uint8 *)dst;
    if (codepoint <= 0x7F) {
        *p = (Uint8)codepoint;
        ++dst;
    } else if (codepoint <= 0x7FF) {
        p[0] = 0xC0 | (Uint8)((codepoint >> 6) & 0x1F);
        p[1] = 0x80 | (Uint8)(codepoint & 0x3F);
        dst += 2;
    } else if (codepoint <= 0xFFFF) {
        p[0] = 0xE0 | (Uint8)((codepoint >> 12) & 0x0F);
        p[1] = 0x80 | (Uint8)((codepoint >> 6) & 0x3F);
        p[2] = 0x80 | (Uint8)(codepoint & 0x3F);
        dst += 3;
    } else {
        p[0] = 0xF0 | (Uint8)((codepoint >> 18) & 0x07);
        p[1] = 0x80 | (Uint8)((codepoint >> 12) & 0x3F);
        p[2] = 0x80 | (Uint8)((codepoint >> 6) & 0x3F);
        p[3] = 0x80 | (Uint8)(codepoint & 0x3F);
        dst += 4;
    }
    return dst;
}

static Uint32 StepUTF8(const char **_str, const size_t slen)
{
    const Uint8 *str = (const Uint8 *)*_str;
    const Uint32 octet = (Uint32)(slen ? *str : 0);

    if (octet == 0) {
        return 0;
    } else if ((octet & 0x80) == 0) {
        (*_str)++;
        return octet;
    } else if (((octet & 0xE0) == 0xC0) && (slen >= 2)) {
        const Uint8 str1 = str[1];
        if ((str1 & 0xC0) == 0x80) {
            const Uint32 result = ((octet & 0x1F) << 6) | (str1 & 0x3F);
            if (result >= 0x0080) {
                *_str += 2;
                return result;
            }
        }
    } else if (((octet & 0xF0) == 0xE0) && (slen >= 3)) {
        const Uint8 str1 = str[1];
        const Uint8 str2 = str[2];
        if (((str1 & 0xC0) == 0x80) && ((str2 & 0xC0) == 0x80)) {
            const Uint32 result = ((octet & 0x0F) << 12) | ((Uint32)(str1 & 0x3F) << 6) | (Uint32)(str2 & 0x3F);
            if (result >= 0x800 && (result < 0xD800 || result > 0xDFFF)) {
                *_str += 3;
                return result;
            }
        }
    } else if (((octet & 0xF8) == 0xF0) && (slen >= 4)) {
        const Uint8 str1 = str[1];
        const Uint8 str2 = str[2];
        const Uint8 str3 = str[3];
        if (((str1 & 0xC0) == 0x80) && ((str2 & 0xC0) == 0x80) && ((str3 & 0xC0) == 0x80)) {
            const Uint32 result = ((octet & 0x07) << 18) | ((Uint32)(str1 & 0x3F) << 12) |
                                  ((Uint32)(str2 & 0x3F) << 6) | (Uint32)(str3 & 0x3F);
            if (result >= 0x10000) {
                *_str += 4;
                return result;
            }
        }
    }
    (*_str)++;
    return SDL_INVALID_UNICODE_CODEPOINT;
}

Uint32 SDL_StepUTF8(const char **pstr, size_t *pslen)
{
    if (!pslen) {
        return StepUTF8(pstr, 4);
    }
    const char *origstr = *pstr;
    const Uint32 result = StepUTF8(pstr, *pslen);
    *pslen -= (size_t)(*pstr - origstr);
    return result;
}

Uint32 SDL_StepBackUTF8(const char *start, const char **pstr)
{
    if (!pstr || *pstr <= start) {
        return 0;
    }
    const char *str = *pstr;
    do {
        if (str == start) {
            break;
        }
        --str;
    } while ((*str & 0xC0) == 0x80);
    size_t length = (size_t)(*pstr - str);
    *pstr = str;
    return StepUTF8(&str, length);
}

size_t SDL_utf8strlen(const char *str)
{
    size_t result = 0;
    while (SDL_StepUTF8(&str, NULL)) {
        result++;
    }
    return result;
}

size_t SDL_utf8strnlen(const char *str, size_t bytes)
{
    size_t result = 0;
    while (SDL_StepUTF8(&str, &bytes)) {
        result++;
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* printf / scanf                                                      */
/* ------------------------------------------------------------------ */

int SDL_sscanf(const char *text, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(text, fmt, ap);
    va_end(ap);
    return r;
}

int SDL_vsnprintf(char *text, size_t maxlen, const char *fmt, va_list ap)
{
    if (!fmt) {
        fmt = "";
    }
    return vsnprintf(text, maxlen, fmt, ap);
}

int SDL_snprintf(char *text, size_t maxlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = SDL_vsnprintf(text, maxlen, fmt, ap);
    va_end(ap);
    return r;
}

int SDL_vswprintf(wchar_t *text, size_t maxlen, const wchar_t *fmt, va_list ap)
{
    if (!fmt) {
        fmt = L"";
    }
    return vswprintf(text, maxlen, fmt, ap);
}

int SDL_swprintf(wchar_t *text, size_t maxlen, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = SDL_vswprintf(text, maxlen, fmt, ap);
    va_end(ap);
    return r;
}

int SDL_vasprintf(char **strp, const char *fmt, va_list ap)
{
    if (!strp) {
        return SDL_InvalidParamError("strp"), -1;
    }
    *strp = NULL;
    va_list aq;
    va_copy(aq, ap);
    int len = SDL_vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (len < 0) {
        return -1;
    }
    char *s = (char *)SDL_malloc((size_t)len + 1);
    if (!s) {
        return -1;
    }
    SDL_vsnprintf(s, (size_t)len + 1, fmt, ap);
    *strp = s;
    return len;
}

int SDL_asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = SDL_vasprintf(strp, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ */
/* Sorting                                                             */
/* ------------------------------------------------------------------ */

void SDL_qsort(void *base, size_t nmemb, size_t size, SDL_CompareCallback compare)
{
    if (base && compare && size) {
        qsort(base, nmemb, size, compare);
    }
}

typedef struct
{
    SDL_CompareCallback_r compare;
    void *userdata;
} QsortCtx;

static int qsort_r_tramp(const void *a, const void *b, void *arg)
{
    QsortCtx *ctx = (QsortCtx *)arg;
    return ctx->compare(ctx->userdata, a, b);
}

void SDL_qsort_r(void *base, size_t nmemb, size_t size, SDL_CompareCallback_r compare, void *userdata)
{
    if (base && compare && size) {
        QsortCtx ctx = { compare, userdata };
        qsort_r(base, nmemb, size, qsort_r_tramp, &ctx);
    }
}

void *SDL_bsearch_r(const void *key, const void *base, size_t nmemb, size_t size,
                    SDL_CompareCallback_r compare, void *userdata)
{
    if (!key || !base || !compare || !size) {
        return NULL;
    }
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void *elem = (const char *)base + mid * size;
        int r = compare(userdata, key, elem);
        if (r < 0) {
            hi = mid;
        } else if (r > 0) {
            lo = mid + 1;
        } else {
            return (void *)elem;
        }
    }
    return NULL;
}

static int SDLCALL bsearch_tramp(void *userdata, const void *a, const void *b)
{
    return ((SDL_CompareCallback)(SDL_FunctionPointer)userdata)(a, b);
}

void *SDL_bsearch(const void *key, const void *base, size_t nmemb, size_t size, SDL_CompareCallback compare)
{
    if (!compare) {
        return NULL;
    }
    return SDL_bsearch_r(key, base, nmemb, size, bsearch_tramp, (void *)(SDL_FunctionPointer)compare);
}

/* ------------------------------------------------------------------ */
/* Pseudo-random numbers (SDL3's LCG: same sequences for same seeds)   */
/* ------------------------------------------------------------------ */

static Uint64 SDL_rand_state;
static bool SDL_rand_initialized;

void SDL_srand(Uint64 seed)
{
    if (!seed) {
        seed = SDL_GetPerformanceCounter();
    }
    SDL_rand_state = seed;
    SDL_rand_initialized = true;
}

Uint32 SDL_rand_bits_r(Uint64 *state)
{
    if (!state) {
        return 0;
    }
    *state = *state * 0xff1cd035ul + 0x05;
    return (Uint32)(*state >> 32);
}

Sint32 SDL_rand_r(Uint64 *state, Sint32 n)
{
    if (n < 0) {
        return 0;
    }
    Uint64 val = (Uint64)SDL_rand_bits_r(state) * (Uint64)n;
    return (Sint32)(val >> 32);
}

float SDL_randf_r(Uint64 *state)
{
    return (float)(SDL_rand_bits_r(state) >> (32 - 24)) * 0x1p-24f;
}

Sint32 SDL_rand(Sint32 n)
{
    if (!SDL_rand_initialized) {
        SDL_srand(0);
    }
    return SDL_rand_r(&SDL_rand_state, n);
}

float SDL_randf(void)
{
    if (!SDL_rand_initialized) {
        SDL_srand(0);
    }
    return SDL_randf_r(&SDL_rand_state);
}

Uint32 SDL_rand_bits(void)
{
    if (!SDL_rand_initialized) {
        SDL_srand(0);
    }
    return SDL_rand_bits_r(&SDL_rand_state);
}

/* ------------------------------------------------------------------ */
/* Checksums / hashes (SDL3 algorithms, do not change)                 */
/* ------------------------------------------------------------------ */

static Uint16 crc16_for_byte(Uint8 r)
{
    Uint16 crc = 0;
    for (int i = 0; i < 8; ++i) {
        crc = (Uint16)((((crc ^ r) & 1) ? 0xA001 : 0) ^ (crc >> 1));
        r >>= 1;
    }
    return crc;
}

Uint16 SDL_crc16(Uint16 crc, const void *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc = (Uint16)(crc16_for_byte((Uint8)crc ^ ((const Uint8 *)data)[i]) ^ (crc >> 8));
    }
    return crc;
}

static Uint32 crc32_for_byte(Uint32 r)
{
    for (int i = 0; i < 8; ++i) {
        r = ((r & 1) ? 0 : (Uint32)0xEDB88320L) ^ (r >> 1);
    }
    return r ^ (Uint32)0xFF000000L;
}

Uint32 SDL_crc32(Uint32 crc, const void *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc = crc32_for_byte((Uint8)crc ^ ((const Uint8 *)data)[i]) ^ (crc >> 8);
    }
    return crc;
}

static inline Uint32 murmur_32_scramble(Uint32 k)
{
    k *= 0xcc9e2d51;
    k = (k << 15) | (k >> 17);
    k *= 0x1b873593;
    return k;
}

Uint32 SDL_murmur3_32(const void *data, size_t len, Uint32 seed)
{
    const Uint8 *bytes = (const Uint8 *)data;
    Uint32 hash = seed;
    Uint32 k;
    for (size_t i = len >> 2; i--;) {
        memcpy(&k, bytes, sizeof(Uint32));
        k = SDL_Swap32LE(k);
        bytes += sizeof(Uint32);
        hash ^= murmur_32_scramble(k);
        hash = (hash << 13) | (hash >> 19);
        hash = hash * 5 + 0xe6546b64;
    }
    size_t left = (len & 3);
    if (left) {
        k = 0;
        for (size_t i = left; i--;) {
            k <<= 8;
            k |= bytes[i];
        }
        hash ^= murmur_32_scramble(k);
    }
    hash ^= (Uint32)len;
    hash ^= hash >> 16;
    hash *= 0x85ebca6b;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35;
    hash ^= hash >> 16;
    return hash;
}

/* ------------------------------------------------------------------ */
/* iconv (glibc's, through the SDL3 API)                               */
/* ------------------------------------------------------------------ */

SDL_iconv_t SDL_iconv_open(const char *tocode, const char *fromcode)
{
    iconv_t cd = iconv_open(tocode, fromcode);
    if (cd == (iconv_t)-1) {
        SDL_SetError("Unsupported iconv conversion: %s -> %s", fromcode, tocode);
        return (SDL_iconv_t)-1;
    }
    return (SDL_iconv_t)cd;
}

int SDL_iconv_close(SDL_iconv_t cd)
{
    if (cd == (SDL_iconv_t)-1) {
        return -1;
    }
    return iconv_close((iconv_t)cd);
}

size_t SDL_iconv(SDL_iconv_t cd, const char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft)
{
    if (cd == (SDL_iconv_t)-1) {
        return SDL_ICONV_ERROR;
    }
    size_t r = iconv((iconv_t)cd, (char **)inbuf, inbytesleft, outbuf, outbytesleft);
    if (r == (size_t)-1) {
        switch (errno) {
        case E2BIG:
            return SDL_ICONV_E2BIG;
        case EILSEQ:
            return SDL_ICONV_EILSEQ;
        case EINVAL:
            return SDL_ICONV_EINVAL;
        default:
            return SDL_ICONV_ERROR;
        }
    }
    return r;
}

char *SDL_iconv_string(const char *tocode, const char *fromcode, const char *inbuf, size_t inbytesleft)
{
    if (!tocode || !*tocode) {
        tocode = "UTF-8";
    }
    if (!fromcode || !*fromcode) {
        fromcode = "UTF-8";
    }
    SDL_iconv_t cd = SDL_iconv_open(tocode, fromcode);
    if (cd == (SDL_iconv_t)-1) {
        return NULL;
    }
    size_t stringsize = inbytesleft;
    char *string = (char *)SDL_malloc(stringsize + sizeof(Uint32));
    if (!string) {
        SDL_iconv_close(cd);
        return NULL;
    }
    char *outbuf = string;
    size_t outbytesleft = stringsize;
    memset(outbuf, 0, sizeof(Uint32));

    while (inbytesleft > 0) {
        const size_t oldinbytesleft = inbytesleft;
        size_t retCode = SDL_iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
        if (retCode == SDL_ICONV_E2BIG) {
            const ptrdiff_t diff = outbuf - string;
            char *oldstring = string;
            stringsize = stringsize ? stringsize * 2 : 16;
            string = (char *)SDL_realloc(string, stringsize + sizeof(Uint32));
            if (!string) {
                SDL_free(oldstring);
                SDL_iconv_close(cd);
                return NULL;
            }
            outbuf = string + diff;
            outbytesleft = stringsize - (size_t)diff;
            memset(outbuf, 0, sizeof(Uint32));
            continue;
        } else if (retCode == SDL_ICONV_EILSEQ) {
            ++inbuf;
            --inbytesleft;
        } else if (retCode == SDL_ICONV_EINVAL || retCode == SDL_ICONV_ERROR) {
            inbytesleft = 0;
        }
        if (oldinbytesleft == inbytesleft) {
            break;
        }
    }
    memset(outbuf, 0, sizeof(Uint32));
    SDL_iconv_close(cd);
    return string;
}

/* ------------------------------------------------------------------ */
/* Environment                                                         */
/* ------------------------------------------------------------------ */

struct SDL_Environment
{
    pthread_mutex_t lock;
    char **vars;   /* "name=value", owned */
    int count, capacity;
};

static SDL_Environment *SDL_environment;
static pthread_once_t env_once = PTHREAD_ONCE_INIT;

static int env_find(SDL_Environment *env, const char *name, size_t namelen)
{
    for (int i = 0; i < env->count; i++) {
        if (strncmp(env->vars[i], name, namelen) == 0 && env->vars[i][namelen] == '=') {
            return i;
        }
    }
    return -1;
}

static bool env_put(SDL_Environment *env, const char *name, const char *value, bool overwrite)
{
    size_t namelen = strlen(name);
    int idx = env_find(env, name, namelen);
    if (idx >= 0 && !overwrite) {
        return true;
    }
    size_t vlen = strlen(value);
    char *s = (char *)SDL_malloc(namelen + vlen + 2);
    if (!s) {
        return false;
    }
    memcpy(s, name, namelen);
    s[namelen] = '=';
    memcpy(s + namelen + 1, value, vlen + 1);
    if (idx >= 0) {
        SDL_free(env->vars[idx]);
        env->vars[idx] = s;
        return true;
    }
    if (env->count == env->capacity) {
        int cap = env->capacity ? env->capacity * 2 : 64;
        char **nv = (char **)SDL_realloc(env->vars, (size_t)cap * sizeof(char *));
        if (!nv) {
            SDL_free(s);
            return false;
        }
        env->vars = nv;
        env->capacity = cap;
    }
    env->vars[env->count++] = s;
    return true;
}

SDL_Environment *SDL_CreateEnvironment(bool populated)
{
    SDL_Environment *env = (SDL_Environment *)SDL_calloc(1, sizeof(*env));
    if (!env) {
        return NULL;
    }
    pthread_mutex_init(&env->lock, NULL);
    if (populated && environ) {
        for (char **e = environ; *e; e++) {
            const char *eq = strchr(*e, '=');
            if (!eq || eq == *e) {
                continue;
            }
            char *name = SDL_strndup(*e, (size_t)(eq - *e));
            if (name) {
                env_put(env, name, eq + 1, false);
                SDL_free(name);
            }
        }
    }
    return env;
}

static void env_init_once(void)
{
    SDL_environment = SDL_CreateEnvironment(true);
}

SDL_Environment *SDL_GetEnvironment(void)
{
    pthread_once(&env_once, env_init_once);
    return SDL_environment;
}

const char *SDL_GetEnvironmentVariable(SDL_Environment *env, const char *name)
{
    if (!env) {
        SDL_InvalidParamError("env");
        return NULL;
    }
    if (!name || !*name) {
        return NULL;
    }
    const char *result = NULL;
    pthread_mutex_lock(&env->lock);
    int idx = env_find(env, name, strlen(name));
    if (idx >= 0) {
        /* SDL3: valid until the variable changes (we keep the string alive
         * in a per-thread-unsafe way, same as SDL3's documented contract) */
        result = env->vars[idx] + strlen(name) + 1;
    }
    pthread_mutex_unlock(&env->lock);
    return result;
}

char **SDL_GetEnvironmentVariables(SDL_Environment *env)
{
    if (!env) {
        SDL_InvalidParamError("env");
        return NULL;
    }
    pthread_mutex_lock(&env->lock);
    size_t size = (size_t)(env->count + 1) * sizeof(char *);
    for (int i = 0; i < env->count; i++) {
        size += strlen(env->vars[i]) + 1;
    }
    char **result = (char **)SDL_malloc(size);
    if (result) {
        char *strings = (char *)(result + env->count + 1);
        for (int i = 0; i < env->count; i++) {
            size_t len = strlen(env->vars[i]) + 1;
            memcpy(strings, env->vars[i], len);
            result[i] = strings;
            strings += len;
        }
        result[env->count] = NULL;
    }
    pthread_mutex_unlock(&env->lock);
    return result;
}

bool SDL_SetEnvironmentVariable(SDL_Environment *env, const char *name, const char *value, bool overwrite)
{
    if (!env) {
        return SDL_InvalidParamError("env");
    }
    if (!name || !*name || strchr(name, '=')) {
        return SDL_InvalidParamError("name");
    }
    if (!value) {
        return SDL_InvalidParamError("value");
    }
    pthread_mutex_lock(&env->lock);
    bool ok = env_put(env, name, value, overwrite);
    pthread_mutex_unlock(&env->lock);
    return ok;
}

bool SDL_UnsetEnvironmentVariable(SDL_Environment *env, const char *name)
{
    if (!env) {
        return SDL_InvalidParamError("env");
    }
    if (!name || !*name || strchr(name, '=')) {
        return SDL_InvalidParamError("name");
    }
    pthread_mutex_lock(&env->lock);
    int idx = env_find(env, name, strlen(name));
    if (idx >= 0) {
        SDL_free(env->vars[idx]);
        env->vars[idx] = env->vars[--env->count];
    }
    pthread_mutex_unlock(&env->lock);
    return true;
}

void SDL_DestroyEnvironment(SDL_Environment *env)
{
    if (!env || env == SDL_environment) {
        return;
    }
    for (int i = 0; i < env->count; i++) {
        SDL_free(env->vars[i]);
    }
    SDL_free(env->vars);
    pthread_mutex_destroy(&env->lock);
    SDL_free(env);
}

const char *SDL_getenv(const char *name)
{
    return SDL_GetEnvironmentVariable(SDL_GetEnvironment(), name);
}

const char *SDL_getenv_unsafe(const char *name)
{
    if (!name || !*name) {
        return NULL;
    }
    return getenv(name);
}

int SDL_setenv_unsafe(const char *name, const char *value, int overwrite)
{
    if (!name || !*name || strchr(name, '=') || !value) {
        return -1;
    }
    if (SDL_environment) {
        SDL_SetEnvironmentVariable(SDL_environment, name, value, overwrite != 0);
    }
    return setenv(name, value, overwrite);
}

int SDL_unsetenv_unsafe(const char *name)
{
    if (!name || !*name || strchr(name, '=')) {
        return -1;
    }
    if (SDL_environment) {
        SDL_UnsetEnvironmentVariable(SDL_environment, name);
    }
    return unsetenv(name);
}
