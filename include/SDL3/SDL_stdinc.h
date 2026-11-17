/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.

  Stand-in for <SDL3/SDL_stdinc.h>: the basic typedefs and macros used by
  the SDL3 API subset implemented by SDLop, plus the standard utility
  functions (memory, environment, ctype, strings, number parsing and
  printf) ported from SDL3 3.2.10.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_stdinc_h_
#define SDL_stdinc_h_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>

#include <SDL3/SDL_begin_code.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int8_t   Sint8;
typedef uint8_t  Uint8;
typedef int16_t  Sint16;
typedef uint16_t Uint16;
typedef int32_t  Sint32;
typedef uint32_t Uint32;
typedef int64_t  Sint64;
typedef uint64_t Uint64;

typedef Uint32 SDL_WindowID;
typedef Uint32 SDL_MouseID;
typedef Uint32 SDL_KeyboardID;
typedef Uint64 SDL_TouchID;  /* SDL3: SDL_touch.h - Uint64 */
typedef Uint64 SDL_FingerID; /* SDL3: SDL_touch.h - Uint64 */
typedef Uint32 SDL_PropertiesID;
typedef Uint32 SDL_MouseButtonFlags;
typedef Uint32 SDL_InitFlags;

#define SDL_arraysize(array) (sizeof(array) / sizeof(array[0]))
#define SDL_zero(x) SDL_memset(&(x), 0, sizeof(x))
#define SDL_zerop(x) SDL_memset((x), 0, sizeof(*(x)))
#define SDL_zeroa(x) SDL_memset((x), 0, sizeof((x)))
#define SDL_min(x, y) (((x) < (y)) ? (x) : (y))
#define SDL_max(x, y) (((x) > (y)) ? (x) : (y))
#define SDL_clamp(x, a, b) (((x) < (a)) ? (a) : (((x) > (b)) ? (b) : (x)))

#define SDL_UINT64_C(c) UINT64_C(c)

#ifndef SDL_SIZE_MAX
#define SDL_SIZE_MAX SIZE_MAX
#endif

/** Byte-order detection (mirrors SDL_endian.h for the common case). */
#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define SDL_BYTEORDER SDL_BIG_ENDIAN
#else
#define SDL_BYTEORDER SDL_LIL_ENDIAN
#endif

/** Generic function pointer (identical to SDL3's SDL_FunctionPointer). */
typedef void (*SDL_FunctionPointer)(void);

/** SDL3: an invalid Unicode codepoint (U+FFFD REPLACEMENT CHARACTER). */
#define SDL_INVALID_UNICODE_CODEPOINT 0xFFFD

/* ------------------------------------------------------------------ */
/* Memory management (SDL3-identical semantics: the allocator is       */
/* replaceable through SDL_SetMemoryFunctions)                         */
/* ------------------------------------------------------------------ */

typedef void *(SDLCALL *SDL_malloc_func)(size_t size);
typedef void *(SDLCALL *SDL_calloc_func)(size_t nmemb, size_t size);
typedef void *(SDLCALL *SDL_realloc_func)(void *mem, size_t size);
typedef void (SDLCALL *SDL_free_func)(void *mem);

extern void * SDLCALL SDL_malloc(size_t size);
extern void * SDLCALL SDL_calloc(size_t nmemb, size_t size);
extern void * SDLCALL SDL_realloc(void *mem, size_t size);
extern void SDLCALL SDL_free(void *mem);

extern void SDLCALL SDL_GetOriginalMemoryFunctions(SDL_malloc_func *malloc_func,
                                                   SDL_calloc_func *calloc_func,
                                                   SDL_realloc_func *realloc_func,
                                                   SDL_free_func *free_func);
extern void SDLCALL SDL_GetMemoryFunctions(SDL_malloc_func *malloc_func,
                                           SDL_calloc_func *calloc_func,
                                           SDL_realloc_func *realloc_func,
                                           SDL_free_func *free_func);
extern bool SDLCALL SDL_SetMemoryFunctions(SDL_malloc_func malloc_func,
                                           SDL_calloc_func calloc_func,
                                           SDL_realloc_func realloc_func,
                                           SDL_free_func free_func);

extern void * SDLCALL SDL_aligned_alloc(size_t alignment, size_t size);
extern void SDLCALL SDL_aligned_free(void *mem);
extern int SDLCALL SDL_GetNumAllocations(void);

/* ------------------------------------------------------------------ */
/* Environment variables                                               */
/* ------------------------------------------------------------------ */

typedef struct SDL_Environment SDL_Environment;

extern SDL_Environment * SDLCALL SDL_GetEnvironment(void);
extern SDL_Environment * SDLCALL SDL_CreateEnvironment(bool populated);
extern const char * SDLCALL SDL_GetEnvironmentVariable(SDL_Environment *env, const char *name);
extern char ** SDLCALL SDL_GetEnvironmentVariables(SDL_Environment *env);
extern bool SDLCALL SDL_SetEnvironmentVariable(SDL_Environment *env, const char *name, const char *value, bool overwrite);
extern bool SDLCALL SDL_UnsetEnvironmentVariable(SDL_Environment *env, const char *name);
extern void SDLCALL SDL_DestroyEnvironment(SDL_Environment *env);
extern const char * SDLCALL SDL_getenv(const char *name);
extern const char * SDLCALL SDL_getenv_unsafe(const char *name);
extern int SDLCALL SDL_setenv_unsafe(const char *name, const char *value, int overwrite);
extern int SDLCALL SDL_unsetenv_unsafe(const char *name);

/* ------------------------------------------------------------------ */
/* Character classification (locale-independent, ASCII - like SDL3)    */
/* ------------------------------------------------------------------ */

extern int SDLCALL SDL_abs(int x);
extern int SDLCALL SDL_isalpha(int x);
extern int SDLCALL SDL_isalnum(int x);
extern int SDLCALL SDL_isblank(int x);
extern int SDLCALL SDL_iscntrl(int x);
extern int SDLCALL SDL_isdigit(int x);
extern int SDLCALL SDL_isxdigit(int x);
extern int SDLCALL SDL_ispunct(int x);
extern int SDLCALL SDL_isspace(int x);
extern int SDLCALL SDL_isupper(int x);
extern int SDLCALL SDL_islower(int x);
extern int SDLCALL SDL_isprint(int x);
extern int SDLCALL SDL_isgraph(int x);
extern int SDLCALL SDL_toupper(int x);
extern int SDLCALL SDL_tolower(int x);

/* ------------------------------------------------------------------ */
/* Memory blocks. SDL3 declares real functions and then re-#defines    */
/* them to the libc spelling for compiler optimizations; SDLop does    */
/* the same so either spelling links.                                  */
/* ------------------------------------------------------------------ */

extern void * SDLCALL SDL_memcpy(void *dst, const void *src, size_t len);
/* Take advantage of compiler optimizations for memcpy */
#ifndef SDL_SLOW_MEMCPY
#ifdef SDL_memcpy
#undef SDL_memcpy
#endif
#define SDL_memcpy  memcpy
#endif

extern void * SDLCALL SDL_memmove(void *dst, const void *src, size_t len);
#ifndef SDL_SLOW_MEMMOVE
#ifdef SDL_memmove
#undef SDL_memmove
#endif
#define SDL_memmove memmove
#endif

extern void * SDLCALL SDL_memset(void *dst, int c, size_t len);
#ifndef SDL_SLOW_MEMSET
#ifdef SDL_memset
#undef SDL_memset
#endif
#define SDL_memset  memset
#endif

extern void * SDLCALL SDL_memset4(void *dst, Uint32 val, size_t dwords);
extern int SDLCALL SDL_memcmp(const void *s1, const void *s2, size_t len);

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

extern size_t SDLCALL SDL_strlen(const char *str);
extern size_t SDLCALL SDL_strnlen(const char *str, size_t maxlen);
extern size_t SDLCALL SDL_strlcpy(char *dst, const char *src, size_t maxlen);
extern size_t SDLCALL SDL_utf8strlcpy(char *dst, const char *src, size_t dst_bytes);
extern size_t SDLCALL SDL_strlcat(char *dst, const char *src, size_t maxlen);
extern char * SDLCALL SDL_strdup(const char *str);
extern char * SDLCALL SDL_strndup(const char *str, size_t maxlen);
extern char * SDLCALL SDL_strrev(char *str);
extern char * SDLCALL SDL_strupr(char *str);
extern char * SDLCALL SDL_strlwr(char *str);
extern char * SDLCALL SDL_strchr(const char *str, int c);
extern char * SDLCALL SDL_strrchr(const char *str, int c);
extern char * SDLCALL SDL_strstr(const char *haystack, const char *needle);
extern char * SDLCALL SDL_strnstr(const char *haystack, const char *needle, size_t maxlen);
extern char * SDLCALL SDL_strcasestr(const char *haystack, const char *needle);
extern char * SDLCALL SDL_strtok_r(char *str, const char *delim, char **saveptr);
extern size_t SDLCALL SDL_utf8strlen(const char *str);
extern size_t SDLCALL SDL_utf8strnlen(const char *str, size_t bytes);
extern int SDLCALL SDL_strcmp(const char *str1, const char *str2);
extern int SDLCALL SDL_strncmp(const char *str1, const char *str2, size_t maxlen);
extern int SDLCALL SDL_strcasecmp(const char *str1, const char *str2);
extern int SDLCALL SDL_strncasecmp(const char *str1, const char *str2, size_t maxlen);
extern char * SDLCALL SDL_strpbrk(const char *str, const char *breakset);
extern Uint32 SDLCALL SDL_StepUTF8(const char **pstr, size_t *pslen);
extern Uint32 SDLCALL SDL_StepBackUTF8(const char *start, const char **pstr);
extern char * SDLCALL SDL_UCS4ToUTF8(Uint32 codepoint, char *dst);

/* ------------------------------------------------------------------ */
/* Number formatting/parsing                                           */
/* ------------------------------------------------------------------ */

extern char * SDLCALL SDL_itoa(int value, char *str, int radix);
extern char * SDLCALL SDL_uitoa(unsigned int value, char *str, int radix);
extern char * SDLCALL SDL_ltoa(long value, char *str, int radix);
extern char * SDLCALL SDL_ultoa(unsigned long value, char *str, int radix);
extern char * SDLCALL SDL_lltoa(long long value, char *str, int radix);
extern char * SDLCALL SDL_ulltoa(unsigned long long value, char *str, int radix);
extern int SDLCALL SDL_atoi(const char *str);
extern double SDLCALL SDL_atof(const char *str);
extern long SDLCALL SDL_strtol(const char *str, char **endp, int base);
extern unsigned long SDLCALL SDL_strtoul(const char *str, char **endp, int base);
extern long long SDLCALL SDL_strtoll(const char *str, char **endp, int base);
extern unsigned long long SDLCALL SDL_strtoull(const char *str, char **endp, int base);
extern double SDLCALL SDL_strtod(const char *str, char **endp);

/* ------------------------------------------------------------------ */
/* Formatted output                                                    */
/* ------------------------------------------------------------------ */

extern int SDLCALL SDL_snprintf(char *text, size_t maxlen, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
extern int SDLCALL SDL_vsnprintf(char *text, size_t maxlen, const char *fmt, va_list ap);
extern int SDLCALL SDL_asprintf(char **strp, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
extern int SDLCALL SDL_vasprintf(char **strp, const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#include <SDL3/SDL_close_code.h>

#endif /* SDL_stdinc_h_ */
