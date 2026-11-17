/*
  SDLop - standard utility functions: memory, ctype, strings, number
  parsing/formatting, printf (ported from SDL3 3.2.10 src/stdlib/, using
  the libc-backed variants SDL3 uses on Linux).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* the macros must not rewrite the definitions below (SDL3 does the same
   #undef dance in its SDL_memcpy.c/SDL_memset.c) */
#undef SDL_memcpy
#undef SDL_memmove
#undef SDL_memset

/* ------------------------------------------------------------------ */
/* Memory functions (SDL3: src/stdlib/SDL_malloc.c, libc build)        */
/* ------------------------------------------------------------------ */

static void * SDLCALL real_malloc(size_t s) { return malloc(s); }
static void * SDLCALL real_calloc(size_t n, size_t s) { return calloc(n, s); }
static void * SDLCALL real_realloc(void *p, size_t s) { return realloc(p, s); }
static void   SDLCALL real_free(void *p) { free(p); }

/* Memory functions used by SDLop that can be replaced by the application */
static struct
{
    SDL_malloc_func malloc_func;
    SDL_calloc_func calloc_func;
    SDL_realloc_func realloc_func;
    SDL_free_func free_func;
} s_mem = {
    real_malloc, real_calloc, real_realloc, real_free
};

void SDL_GetOriginalMemoryFunctions(SDL_malloc_func *malloc_func,
                                    SDL_calloc_func *calloc_func,
                                    SDL_realloc_func *realloc_func,
                                    SDL_free_func *free_func)
{
    if (malloc_func) {
        *malloc_func = real_malloc;
    }
    if (calloc_func) {
        *calloc_func = real_calloc;
    }
    if (realloc_func) {
        *realloc_func = real_realloc;
    }
    if (free_func) {
        *free_func = real_free;
    }
}

void SDL_GetMemoryFunctions(SDL_malloc_func *malloc_func,
                            SDL_calloc_func *calloc_func,
                            SDL_realloc_func *realloc_func,
                            SDL_free_func *free_func)
{
    if (malloc_func) {
        *malloc_func = s_mem.malloc_func;
    }
    if (calloc_func) {
        *calloc_func = s_mem.calloc_func;
    }
    if (realloc_func) {
        *realloc_func = s_mem.realloc_func;
    }
    if (free_func) {
        *free_func = s_mem.free_func;
    }
}

bool SDL_SetMemoryFunctions(SDL_malloc_func malloc_func,
                            SDL_calloc_func calloc_func,
                            SDL_realloc_func realloc_func,
                            SDL_free_func free_func)
{
    if (!malloc_func) {
        return SDL_InvalidParamError("malloc_func");
    }
    if (!calloc_func) {
        return SDL_InvalidParamError("calloc_func");
    }
    if (!realloc_func) {
        return SDL_InvalidParamError("realloc_func");
    }
    if (!free_func) {
        return SDL_InvalidParamError("free_func");
    }

    s_mem.malloc_func = malloc_func;
    s_mem.calloc_func = calloc_func;
    s_mem.realloc_func = realloc_func;
    s_mem.free_func = free_func;
    return true;
}

int SDL_GetNumAllocations(void)
{
    /* SDL3 returns -1 unless built with SDL_TRACK_ALLOCATION_COUNT */
    return -1;
}

void *SDL_malloc(size_t size)
{
    void *mem;

    if (!size) {
        size = 1;
    }

    mem = s_mem.malloc_func(size);
    if (!mem) {
        SDL_OutOfMemory();
    }

    return mem;
}

void *SDL_calloc(size_t nmemb, size_t size)
{
    void *mem;

    if (!nmemb || !size) {
        nmemb = 1;
        size = 1;
    }

    mem = s_mem.calloc_func(nmemb, size);
    if (!mem) {
        SDL_OutOfMemory();
    }

    return mem;
}

void *SDL_realloc(void *ptr, size_t size)
{
    void *mem;

    if (!size) {
        size = 1;
    }

    mem = s_mem.realloc_func(ptr, size);
    if (!mem) {
        SDL_OutOfMemory();
    }

    return mem;
}

void SDL_free(void *ptr)
{
    if (!ptr) {
        return;
    }

    s_mem.free_func(ptr);
}

/* SDL3: src/stdlib/SDL_stdlib.c */
void *SDL_aligned_alloc(size_t alignment, size_t size)
{
    size_t padding;
    Uint8 *result = NULL;
    size_t requested_size = size;

    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    padding = (alignment - (size % alignment));

    /* size + alignment + sizeof(void *) + padding, with overflow checks */
    if (size <= SDL_SIZE_MAX - alignment &&
        (size + alignment) <= SDL_SIZE_MAX - sizeof(void *) &&
        (size + alignment + sizeof(void *)) <= SDL_SIZE_MAX - padding) {
        size += alignment + sizeof(void *) + padding;
        void *original = SDL_malloc(size);
        if (original) {
            /* Make sure we have enough space to store the original pointer */
            result = (Uint8 *)original + sizeof(original);

            /* Align the pointer we're going to return */
            result += alignment - (((size_t)result) % alignment);

            /* Store the original pointer right before the returned value */
            memcpy(result - sizeof(original), &original, sizeof(original));

            /* Initialize the padding to zero */
            if (padding > 0) {
                memset(result + requested_size, 0, padding);
            }
        }
    }
    return result;
}

void SDL_aligned_free(void *mem)
{
    if (mem) {
        void *original;
        memcpy(&original, ((Uint8 *)mem - sizeof(original)), sizeof(original));
        SDL_free(original);
    }
}

/* ------------------------------------------------------------------ */
/* ctype (SDL3: locale-independent ASCII; identical implementations)   */
/* ------------------------------------------------------------------ */

int SDL_abs(int x) { return abs(x); }
int SDL_isalpha(int x) { return (SDL_isupper(x)) || (SDL_islower(x)); }
int SDL_isalnum(int x) { return (SDL_isalpha(x)) || (SDL_isdigit(x)); }
int SDL_isdigit(int x) { return ((x) >= '0') && ((x) <= '9'); }
int SDL_isxdigit(int x) { return (((x) >= 'A') && ((x) <= 'F')) || (((x) >= 'a') && ((x) <= 'f')) || (SDL_isdigit(x)); }
int SDL_ispunct(int x) { return (SDL_isgraph(x)) && (!SDL_isalnum(x)); }
int SDL_isspace(int x) { return ((x) == ' ') || ((x) == '\t') || ((x) == '\r') || ((x) == '\n') || ((x) == '\f') || ((x) == '\v'); }
int SDL_isupper(int x) { return ((x) >= 'A') && ((x) <= 'Z'); }
int SDL_islower(int x) { return ((x) >= 'a') && ((x) <= 'z'); }
int SDL_isprint(int x) { return ((x) >= ' ') && ((x) < '\x7f'); }
int SDL_isgraph(int x) { return (SDL_isprint(x)) && ((x) != ' '); }
int SDL_iscntrl(int x) { return (((x) >= '\0') && ((x) <= '\x1f')) || ((x) == '\x7f'); }
int SDL_toupper(int x) { return ((x) >= 'a') && ((x) <= 'z') ? ('A' + ((x) - 'a')) : (x); }
int SDL_tolower(int x) { return ((x) >= 'A') && ((x) <= 'Z') ? ('a' + ((x) - 'A')) : (x); }
int SDL_isblank(int x) { return ((x) == ' ') || ((x) == '\t'); }

/* ------------------------------------------------------------------ */
/* Memory blocks                                                       */
/* ------------------------------------------------------------------ */

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

/* Note that memset() is a byte assignment and this is a 32-bit assignment,
   so they're not directly equivalent. */
void *SDL_memset4(void *dst, Uint32 val, size_t dwords)
{
    size_t _n = (dwords + 3) / 4;
    Uint32 *_p = (Uint32 *)dst;
    Uint32 _val = (val);
    if (dwords == 0) {
        return dst;
    }
    switch (dwords % 4) {
    case 0:
        do {
            *_p++ = _val;
            /* fall through */
        case 3:
            *_p++ = _val;
            /* fall through */
        case 2:
            *_p++ = _val;
            /* fall through */
        case 1:
            *_p++ = _val;
        } while (--_n);
    }
    return dst;
}

int SDL_memcmp(const void *s1, const void *s2, size_t len)
{
    return memcmp(s1, s2, len);
}

/* ------------------------------------------------------------------ */
/* UTF-8 stepping (SDL3: src/stdlib/SDL_string.c, verbatim semantics)  */
/* ------------------------------------------------------------------ */

static Uint32 SDLOP_StepUTF8(const char **_str, const size_t slen)
{
    /*
     * From rfc3629, the UTF-8 spec:
     *  https://www.ietf.org/rfc/rfc3629.txt
     */
    const Uint8 *str = (const Uint8 *)*_str;
    const Uint32 octet = (Uint32)(slen ? *str : 0);

    if (octet == 0) {  /* null terminator, end of string. */
        return 0;      /* don't advance `*_str`. */
    } else if ((octet & 0x80) == 0) {  /* 0xxxxxxx: one byte codepoint. */
        (*_str)++;
        return octet;
    } else if (((octet & 0xE0) == 0xC0) && (slen >= 2)) {  /* 110xxxxx 10xxxxxx: two byte codepoint. */
        const Uint8 str1 = str[1];
        if ((str1 & 0xC0) == 0x80) {  /* If trailing bytes aren't 10xxxxxx, sequence is bogus. */
            const Uint32 result = ((octet & 0x1F) << 6) | (str1 & 0x3F);
            if (result >= 0x0080) {  /* rfc3629 says you can't use overlong sequences for smaller values. */
                *_str += 2;
                return result;
            }
        }
    } else if (((octet & 0xF0) == 0xE0) && (slen >= 3)) {  /* 1110xxxx 10xxxxxx 10xxxxxx: three byte codepoint. */
        const Uint8 str1 = str[1];
        const Uint8 str2 = str[2];
        if (((str1 & 0xC0) == 0x80) && ((str2 & 0xC0) == 0x80)) {
            const Uint32 octet2 = ((Uint32)(str1 & 0x3F)) << 6;
            const Uint32 octet3 = ((Uint32)(str2 & 0x3F));
            const Uint32 result = ((octet & 0x0F) << 12) | octet2 | octet3;
            if (result >= 0x800) {
                if ((result < 0xD800) || (result > 0xDFFF)) {  /* UTF-16 surrogate values are illegal in UTF-8. */
                    *_str += 3;
                    return result;
                }
            }
        }
    } else if (((octet & 0xF8) == 0xF0) && (slen >= 4)) {  /* 11110xxx 10xxxxxx ...: four byte codepoint. */
        const Uint8 str1 = str[1];
        const Uint8 str2 = str[2];
        const Uint8 str3 = str[3];
        if (((str1 & 0xC0) == 0x80) && ((str2 & 0xC0) == 0x80) && ((str3 & 0xC0) == 0x80)) {
            const Uint32 octet2 = ((Uint32)(str1 & 0x1F)) << 12;
            const Uint32 octet3 = ((Uint32)(str2 & 0x3F)) << 6;
            const Uint32 octet4 = ((Uint32)(str3 & 0x3F));
            const Uint32 result = ((octet & 0x07) << 18) | octet2 | octet3 | octet4;
            if (result >= 0x10000) {
                *_str += 4;
                return result;
            }
        }
    }

    /* bogus byte, skip ahead, return a REPLACEMENT CHARACTER. */
    (*_str)++;
    return SDL_INVALID_UNICODE_CODEPOINT;
}

Uint32 SDL_StepUTF8(const char **pstr, size_t *pslen)
{
    if (!pslen) {
        return SDLOP_StepUTF8(pstr, 4);  /* 4 == max codepoint size. */
    }
    const char *origstr = *pstr;
    const Uint32 result = SDLOP_StepUTF8(pstr, *pslen);
    *pslen -= (size_t)(*pstr - origstr);
    return result;
}

Uint32 SDL_StepBackUTF8(const char *start, const char **pstr)
{
    if (!pstr || *pstr <= start) {
        return 0;
    }

    /* Step back over the previous UTF-8 character */
    const char *str = *pstr;
    do {
        if (str == start) {
            break;
        }
        --str;
    } while ((*str & 0xC0) == 0x80);

    const size_t length = (size_t)(*pstr - str);
    *pstr = str;
    return SDLOP_StepUTF8(&str, length);
}

char *SDL_UCS4ToUTF8(Uint32 codepoint, char *dst)
{
    if (!dst) {
        return NULL;
    } else if (codepoint > 0x10FFFF) {  /* Outside the range of Unicode codepoints. */
        codepoint = SDL_INVALID_UNICODE_CODEPOINT;
    } else if ((codepoint >= 0xD800) && (codepoint <= 0xDFFF)) {  /* UTF-16 surrogates are illegal in UTF-8. */
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

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

size_t SDL_strlen(const char *string)
{
    return strlen(string);
}

size_t SDL_strnlen(const char *string, size_t maxlen)
{
    return strnlen(string, maxlen);
}

size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen)
{
    size_t srclen = SDL_strlen(src);
    if (maxlen > 0) {
        size_t len = SDL_min(srclen, maxlen - 1);
        SDL_memcpy(dst, src, len);
        dst[len] = '\0';
    }
    return srclen;
}

/* copies as many *whole codepoints* as fit (SDL3 semantics) */
#define SDLOP_UTF8_IsLeadByte(c) ((c) >= 0xC0 && (c) <= 0xF4)
#define SDLOP_UTF8_IsTrailingByte(c) ((c) >= 0x80 && (c) <= 0xBF)
static size_t SDLOP_UTF8_GetTrailingBytes(unsigned char c)
{
    if (c >= 0xF0 && c <= 0xF4) {
        return 3;
    } else if (c >= 0xE0 && c <= 0xEF) {
        return 2;
    } else if (c >= 0xC0 && c <= 0xDF) {
        return 1;
    } else {
        return 0;
    }
}

size_t SDL_utf8strlcpy(char *dst, const char *src, size_t dst_bytes)
{
    size_t bytes = 0;

    if (dst_bytes > 0) {
        size_t src_bytes = SDL_strlen(src);
        size_t i = 0;
        size_t trailing_bytes = 0;

        bytes = SDL_min(src_bytes, dst_bytes - 1);
        if (bytes) {
            unsigned char c = (unsigned char)src[bytes - 1];
            if (SDLOP_UTF8_IsLeadByte(c)) {
                --bytes;
            } else if (SDLOP_UTF8_IsTrailingByte(c)) {
                for (i = bytes - 1; i != 0; --i) {
                    c = (unsigned char)src[i];
                    trailing_bytes = SDLOP_UTF8_GetTrailingBytes(c);
                    if (trailing_bytes) {
                        if ((bytes - i) != (trailing_bytes + 1)) {
                            bytes = i;
                        }

                        break;
                    }
                }
            }
            SDL_memcpy(dst, src, bytes);
        }
        dst[bytes] = '\0';
    }

    return bytes;
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

size_t SDL_strlcat(char *dst, const char *src, size_t maxlen)
{
    size_t dstlen = SDL_strlen(dst);
    size_t srclen = SDL_strlen(src);
    if (dstlen < maxlen) {
        SDL_strlcpy(dst + dstlen, src, maxlen - dstlen);
    }
    return dstlen + srclen;
}

char *SDL_strdup(const char *string)
{
    size_t len = SDL_strlen(string) + 1;
    char *newstr = (char *)SDL_malloc(len);
    if (newstr) {
        SDL_memcpy(newstr, string, len);
    }
    return newstr;
}

char *SDL_strndup(const char *string, size_t maxlen)
{
    size_t len = SDL_strnlen(string, maxlen);
    char *newstr = (char *)SDL_malloc(len + 1);
    if (newstr) {
        SDL_memcpy(newstr, string, len);
        newstr[len] = '\0';
    }
    return newstr;
}

char *SDL_strrev(char *string)
{
    size_t len = SDL_strlen(string);
    char *a = &string[0];
    char *b = &string[len - 1];
    len /= 2;
    while (len--) {
        const char c = *a;
        *a++ = *b;
        *b-- = c;
    }
    return string;
}

char *SDL_strupr(char *string)
{
    char *bufp = string;
    while (*bufp) {
        *bufp = (char)SDL_toupper((unsigned char)*bufp);
        ++bufp;
    }
    return string;
}

char *SDL_strlwr(char *string)
{
    char *bufp = string;
    while (*bufp) {
        *bufp = (char)SDL_tolower((unsigned char)*bufp);
        ++bufp;
    }
    return string;
}

char *SDL_strchr(const char *string, int c)
{
    return (char *)strchr(string, c);
}

char *SDL_strrchr(const char *string, int c)
{
    return (char *)strrchr(string, c);
}

char *SDL_strnstr(const char *haystack, const char *needle, size_t maxlen)
{
    size_t length = SDL_strlen(needle);
    if (length == 0) {
        return (char *)haystack;
    }
    while (maxlen >= length && *haystack) {
        if (SDL_strncmp(haystack, needle, length) == 0) {
            return (char *)haystack;
        }
        ++haystack;
        --maxlen;
    }
    return NULL;
}

char *SDL_strstr(const char *haystack, const char *needle)
{
    return (char *)strstr(haystack, needle);
}

char *SDL_strcasestr(const char *haystack, const char *needle)
{
    const size_t length = SDL_strlen(needle);
    do {
        if (SDL_strncasecmp(haystack, needle, length) == 0) {
            return (char *)haystack;
        }
    } while (SDL_StepUTF8(&haystack, NULL));  /* move ahead by a full codepoint at a time, regardless of bytes. */

    return NULL;
}

/*
 * Adapted from _PDCLIB_strtok() of PDClib library (CC0), like SDL3's
 * SDL_strtokr.c.
 */
char *SDL_strtok_r(char *s1, const char *s2, char **ptr)
{
    const char *p = s2;

    if (!s2 || !ptr || (!s1 && !*ptr)) {
        return NULL;
    }

    if (s1 != NULL) {  /* new string */
        *ptr = s1;
    } else {           /* old string continued */
        if (*ptr == NULL) {
            /* No old string, no new string, nothing to do */
            return NULL;
        }
        s1 = *ptr;
    }

    /* skip leading s2 characters */
    while (*p && *s1) {
        if (*s1 == *p) {
            /* found separator; skip and start over */
            ++s1;
            p = s2;
            continue;
        }
        ++p;
    }

    if (!*s1) {  /* no more to parse */
        *ptr = s1;
        return NULL;
    }

    /* skipping non-s2 characters */
    *ptr = s1;
    while (**ptr) {
        p = s2;
        while (*p) {
            if (**ptr == *p++) {
                /* found separator; overwrite with '\0', position *ptr, return */
                *((*ptr)++) = '\0';
                return s1;
            }
        }
        ++(*ptr);
    }

    return s1;
}

int SDL_strcmp(const char *str1, const char *str2)
{
    return strcmp(str1, str2);
}

int SDL_strncmp(const char *str1, const char *str2, size_t maxlen)
{
    return strncmp(str1, str2, maxlen);
}

/* SDL3 folds full Unicode case here; SDLop folds ASCII only (case-less
   comparison of non-ASCII codepoints compares them unchanged). */
static int SDLOP_casecmp_n(const char *str1, const char *str2, size_t maxlen, bool bounded)
{
    size_t remaining = maxlen;
    while (true) {
        if (bounded && remaining == 0) {
            return 0;
        }
        unsigned char a = (unsigned char)*str1;
        unsigned char b = (unsigned char)*str2;
        int c1 = SDL_tolower(a);
        int c2 = SDL_tolower(b);
        if (c1 < c2) {
            return -1;
        } else if (c1 > c2) {
            return 1;
        } else if (c1 == 0) {
            break;  /* complete match */
        }
        ++str1;
        ++str2;
        --remaining;
    }
    return 0;
}

int SDL_strcasecmp(const char *str1, const char *str2)
{
    return SDLOP_casecmp_n(str1, str2, 0, false);
}

int SDL_strncasecmp(const char *str1, const char *str2, size_t maxlen)
{
    return SDLOP_casecmp_n(str1, str2, maxlen, true);
}

char *SDL_strpbrk(const char *str, const char *breakset)
{
    for (; *str; str++) {
        const char *p;
        for (p = breakset; *p; p++) {
            if (*str == *p) {
                return (char *)str;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Number formatting/parsing (SDL3 uses the libc spellings on Linux)   */
/* ------------------------------------------------------------------ */

static const char ntoa_table[] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
    'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
    'U', 'V', 'W', 'X', 'Y', 'Z'
};

char *SDL_itoa(int value, char *string, int radix)
{
    return SDL_ltoa((long)value, string, radix);
}

char *SDL_uitoa(unsigned int value, char *string, int radix)
{
    return SDL_ultoa((unsigned long)value, string, radix);
}

char *SDL_ltoa(long value, char *string, int radix)
{
    char *bufp = string;

    if (value < 0) {
        *bufp++ = '-';
        SDL_ultoa(-value, bufp, radix);
    } else {
        SDL_ultoa(value, bufp, radix);
    }

    return string;
}

char *SDL_ultoa(unsigned long value, char *string, int radix)
{
    char *bufp = string;

    if (value) {
        while (value > 0) {
            *bufp++ = ntoa_table[value % radix];
            value /= radix;
        }
    } else {
        *bufp++ = '0';
    }
    *bufp = '\0';

    /* The numbers went into the string backwards. :) */
    SDL_strrev(string);

    return string;
}

char *SDL_lltoa(long long value, char *string, int radix)
{
    char *bufp = string;

    if (value < 0) {
        *bufp++ = '-';
        SDL_ulltoa(-value, bufp, radix);
    } else {
        SDL_ulltoa(value, bufp, radix);
    }

    return string;
}

char *SDL_ulltoa(unsigned long long value, char *string, int radix)
{
    char *bufp = string;

    if (value) {
        while (value > 0) {
            *bufp++ = ntoa_table[value % radix];
            value /= radix;
        }
    } else {
        *bufp++ = '0';
    }
    *bufp = '\0';

    /* The numbers went into the string backwards. :) */
    SDL_strrev(string);

    return string;
}

int SDL_atoi(const char *string)
{
    return atoi(string);
}

double SDL_atof(const char *string)
{
    return atof(string);
}

long SDL_strtol(const char *string, char **endp, int base)
{
    return strtol(string, endp, base);
}

unsigned long SDL_strtoul(const char *string, char **endp, int base)
{
    return strtoul(string, endp, base);
}

long long SDL_strtoll(const char *string, char **endp, int base)
{
    return strtoll(string, endp, base);
}

unsigned long long SDL_strtoull(const char *string, char **endp, int base)
{
    return strtoull(string, endp, base);
}

double SDL_strtod(const char *string, char **endp)
{
    return strtod(string, endp);
}

/* ------------------------------------------------------------------ */
/* Formatted output                                                    */
/* ------------------------------------------------------------------ */

int SDL_snprintf(char *text, size_t maxlen, const char *fmt, ...)
{
    va_list ap;
    int result;

    va_start(ap, fmt);
    result = SDL_vsnprintf(text, maxlen, fmt, ap);
    va_end(ap);

    return result;
}

int SDL_vsnprintf(char *text, size_t maxlen, const char *fmt, va_list ap)
{
    if (!fmt) {
        fmt = "";
    }
    return vsnprintf(text, maxlen, fmt, ap);
}

int SDL_asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    int result;

    va_start(ap, fmt);
    result = SDL_vasprintf(strp, fmt, ap);
    va_end(ap);

    return result;
}

int SDL_vasprintf(char **strp, const char *fmt, va_list ap)
{
    int result;
    int size = 100; /* Guess we need no more than 100 bytes */
    char *p, *np;
    va_list aq;

    *strp = NULL;

    p = (char *)SDL_malloc(size);
    if (!p) {
        return -1;
    }

    while (1) {
        /* Try to print in the allocated space */
        va_copy(aq, ap);
        result = SDL_vsnprintf(p, size, fmt, aq);
        va_end(aq);

        /* Check error code */
        if (result < 0) {
            SDL_free(p);
            return result;
        }

        /* If that worked, return the string */
        if (result < size) {
            *strp = p;
            return result;
        }

        /* Else try again with more space */
        size = result + 1; /* Precisely what is needed */

        np = (char *)SDL_realloc(p, size);
        if (!np) {
            SDL_free(p);
            return -1;
        } else {
            p = np;
        }
    }
}
