/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  SDL_stdinc.h: the "standard library" part of the SDL3 API.

  SDLop is written from scratch, but its declarations must match SDL3 exactly,
  so the signatures below are the same as SDL3's (SDL3 is zlib licensed,
  Copyright (C) 1997-2025 Sam Lantinga and SDL contributors).  Unlike SDL3, this
  header does not reimplement the C runtime: the SDLop build delegates to libc and
  only implements what libc does not have (SDL_strlcpy, SDL_strlcat, SDL_rand...).
  Functions upstream provides that SDLop does not implement (SDL_iconv,
  SDL_GetEnvironment, SIMD helpers, ...) are absent; see tools/dropped.txt.
*/

#ifndef SDL_stdinc_h_
#define SDL_stdinc_h_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifndef __cplusplus
#include <alloca.h>
#endif

#ifndef __cplusplus
#include <stdbool.h>
#endif

/* ------------------------------------------------------------------------- */
/* Attribute / decoration macros                                             */
/*                                                                            */
/* All of the SDL_* annotation macros upstream uses appear here, either with  */
/* their real meaning or as documented no-ops, so that upstream-style code    */
/* compiles unchanged.                                                        */
/* ------------------------------------------------------------------------- */

#ifndef SDL_INLINE
#ifdef __cplusplus
#define SDL_INLINE inline
#elif defined(_MSC_VER)
#define SDL_INLINE __inline
#else
#define SDL_INLINE inline
#endif
#endif

#ifndef SDL_FORCE_INLINE
#if defined(_MSC_VER)
#define SDL_FORCE_INLINE static __forceinline
#elif defined(__GNUC__)
#define SDL_FORCE_INLINE static __inline__ __attribute__((always_inline))
#else
#define SDL_FORCE_INLINE static SDL_INLINE
#endif
#endif

#ifndef SDL_NODISCARD
#if defined(__GNUC__) && (__GNUC__ >= 4)
#define SDL_NODISCARD __attribute__((warn_unused_result))
#else
#define SDL_NODISCARD
#endif
#endif

#ifndef SDL_MALLOC
#if defined(__GNUC__)
#define SDL_MALLOC __attribute__((malloc))
#else
#define SDL_MALLOC
#endif
#endif

#ifndef SDL_ALLOC_SIZE
#if defined(__GNUC__)
#define SDL_ALLOC_SIZE(x) __attribute__((alloc_size(x)))
#else
#define SDL_ALLOC_SIZE(x)
#endif
#endif

#ifndef SDL_ALLOC_SIZE2
#if defined(__GNUC__)
#define SDL_ALLOC_SIZE2(x, y) __attribute__((alloc_size(x, y)))
#else
#define SDL_ALLOC_SIZE2(x, y)
#endif
#endif

#ifndef SDL_PRINTF_VARARG_FUNC
#if defined(__GNUC__)
#define SDL_PRINTF_VARARG_FUNC(n) __attribute__((format(printf, n, n + 1)))
#define SDL_PRINTF_VARARG_FUNCV(n) __attribute__((format(printf, n, 0)))
#define SDL_SCANF_VARARG_FUNC(n) __attribute__((format(scanf, n, n + 1)))
#define SDL_SCANF_VARARG_FUNCV(n) __attribute__((format(scanf, n, 0)))
#else
#define SDL_PRINTF_VARARG_FUNC(n)
#define SDL_PRINTF_VARARG_FUNCV(n)
#define SDL_SCANF_VARARG_FUNC(n)
#define SDL_SCANF_VARARG_FUNCV(n)
#endif
#endif

/* Buffer-size documentation macros: no-ops, as in SDL3. */
#define SDL_OUT_BYTECAP(x)
#define SDL_IN_BYTECAP(x)
#define SDL_OUT_Z_BYTECAP(x)
#define SDL_OUT_CAP(x)
#define SDL_IN_CAP(x)
#define SDL_OUT_Z_CAP(x)
#define SDL_PRINTF_FORMAT_STRING
#define SDL_SCANF_FORMAT_STRING
#define SDL_ANALYZER_NORETURN
#define SDL_ATTRIBUTE_UNUSED
#define SDL_ATTRIBUTE_NORETURN
#define SDL_ATTRIBUTE_NONNULL(x)
#define SDL_ATTRIBUTE_FORMAT(x)
#define SDL_ATTRIBUTE_ALIGNED(x)

#ifndef SDL_FALLTHROUGH
#if defined(__has_attribute)
#if __has_attribute(fallthrough)
#define SDL_FALLTHROUGH __attribute__((fallthrough))
#endif
#endif
#ifndef SDL_FALLTHROUGH
#define SDL_FALLTHROUGH
#endif
#endif

/* SDLop targets normal ELF shared objects on the platforms supported today. */
#ifndef SDL_DECLSPEC
#define SDL_DECLSPEC
#endif
#ifndef SDLCALL
#define SDLCALL
#endif

#ifdef __cplusplus
#define SDL_reinterpret_cast(type, expression) reinterpret_cast<type>(expression)
#define SDL_static_cast(type, expression) static_cast<type>(expression)
#define SDL_const_cast(type, expression) const_cast<type>(expression)
#else
#define SDL_reinterpret_cast(type, expression) ((type)(expression))
#define SDL_static_cast(type, expression) ((type)(expression))
#define SDL_const_cast(type, expression) ((type)(expression))
#endif

#ifdef __cplusplus
#define SDL_COMPILE_TIME_ASSERT(name, x) static_assert(x, #x)
#define SDL_CPP_COMPILE_TIME_ASSERT(name, x) static_assert(x, #x)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define SDL_COMPILE_TIME_ASSERT(name, x) _Static_assert(x, #x)
#define SDL_CPP_COMPILE_TIME_ASSERT(name, x)
#else
#define SDL_COMPILE_TIME_ASSERT(name, x) typedef int SDL_compile_time_assert_##name[(x) * 2 - 1]
#define SDL_CPP_COMPILE_TIME_ASSERT(name, x)
#endif

/* ------------------------------------------------------------------------- */
/* Types                                                                     */
/* ------------------------------------------------------------------------- */

typedef int8_t Sint8;
typedef uint8_t Uint8;
typedef int16_t Sint16;
typedef uint16_t Uint16;
typedef int32_t Sint32;
typedef uint32_t Uint32;
typedef int64_t Sint64;
typedef uint64_t Uint64;

typedef intptr_t SDL_intptr;
typedef void (*SDL_FunctionPointer)(void);

/** SDLop runs on unix-like platforms today; this is the only platform macro
    it defines for now. */
#if !defined(SDL_PLATFORM_UNIX)
#define SDL_PLATFORM_UNIX 1
#endif

/* Byte order: needed by SDL_pixels.h and used by a lot of ported code. */
#if !defined(SDL_BYTEORDER)
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define SDL_BYTEORDER 4321
#else
#define SDL_BYTEORDER 1234
#endif
#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#endif

#define SDL_ICONV_ERROR     (size_t)-1
#define SDL_ICONV_E2BIG     (size_t)-2
#define SDL_ICONV_EILSEQ    (size_t)-3
#define SDL_ICONV_EINVAL    (size_t)-4
#define SDL_INVALID_UNICODE_CODEPOINT 0xFFFD
#define SDL_MAX_TIME ((Sint64)INT64_MAX)
#define SDL_MIN_TIME ((Sint64)INT64_MIN)
#define SDL_PI_D 3.141592653589793238462643383279502884
#define SDL_PI_F 3.141592653589793238462643383279502884f

#define SDL_INT8_C(c)   c
#define SDL_UINT8_C(c)  c
#define SDL_INT16_C(c)  c
#define SDL_UINT16_C(c) c
#define SDL_INT32_C(c)  c
#define SDL_UINT32_C(c) c##U
#define SDL_INT64_C(c)  c##LL
#define SDL_UINT64_C(c) c##ULL

#define SDL_SINT8_C(c)  c
#define SDL_SINT16_C(c) c
#define SDL_SINT32_C(c) c
#define SDL_SINT64_C(c) c##LL
#define SDL_MIN_SINT8   ((Sint8)-128)
#define SDL_MAX_SINT8   ((Sint8)127)
#define SDL_MAX_UINT8   ((Uint8)255)
#define SDL_MIN_UINT8   ((Uint8)0)
#define SDL_MIN_SINT16  ((Sint16)-32768)
#define SDL_MAX_SINT16  ((Sint16)32767)
#define SDL_MAX_UINT16  ((Uint16)65535)
#define SDL_MIN_UINT16  ((Uint16)0)
#define SDL_MIN_SINT32  ((Sint32)(-2147483647-1))
#define SDL_MAX_SINT32  ((Sint32)2147483647)
#define SDL_MAX_UINT32  ((Uint32)4294967295u)
#define SDL_MIN_UINT32  ((Uint32)0)
#define SDL_MIN_SINT64  (~((Sint64)9223372036854775807))
#define SDL_MAX_SINT64  ((Sint64)9223372036854775807ll)
#define SDL_MAX_UINT64  ((Uint64)18446744073709551615ull)
#define SDL_MIN_UINT64  ((Uint64)0)
#define SDL_SIZE_MAX    SIZE_MAX
#define SDL_FLT_EPSILON 1.1920928955078125e-07F

/* ------------------------------------------------------------------------- */
/* Macros                                                                    */
/* ------------------------------------------------------------------------- */

/* printf() length modifiers for the fixed-width types, as upstream spells them. */
#define SDL_PRIx32 "x"
#define SDL_PRIX32 "X"
#define SDL_PRIu32 "u"
#define SDL_PRIs32 "d"
#define SDL_PRId32 "d"
#define SDL_PRIx64 "llx"
#define SDL_PRIX64 "llX"
#define SDL_PRIu64 "llu"
#define SDL_PRIs64 "lld"
#define SDL_PRId64 "lld"
#define SDL_PRILL_PREFIX "ll"
#define SDL_PRILLd "lld"
#define SDL_PRILLu "llu"
#define SDL_PRILLx "llx"
#define SDL_PRILLX "llX"
#define SDL_PRILL_PREFIX_DEPRECATED SDL_PRILL_PREFIX

#ifndef SDL_NORETURN
#if defined(__GNUC__)
#define SDL_NORETURN __attribute__((noreturn))
#else
#define SDL_NORETURN
#endif
#endif

#ifndef SDL_DEPRECATED
#if defined(__GNUC__)
#define SDL_DEPRECATED __attribute__((deprecated))
#else
#define SDL_DEPRECATED
#endif
#endif

#ifndef SDL_UNUSED
#if defined(__GNUC__)
#define SDL_UNUSED __attribute__((unused))
#else
#define SDL_UNUSED
#endif
#endif

#ifndef SDL_HAS_BUILTIN
#define SDL_HAS_BUILTIN(x) 0
#endif

#define SDL_FOURCC(a, b, c, d) \
    ((SDL_static_cast(Uint32, SDL_static_cast(Uint8, (a))) << 0) | \
     (SDL_static_cast(Uint32, SDL_static_cast(Uint8, (b))) << 8) | \
     (SDL_static_cast(Uint32, SDL_static_cast(Uint8, (c))) << 16) | \
     (SDL_static_cast(Uint32, SDL_static_cast(Uint8, (d))) << 24))

#define SDL_size_add_check_overflow(a, b, ret) SDL_sdlop_size_add_check_overflow(a, b, ret)
#define SDL_size_mul_check_overflow(a, b, ret) SDL_sdlop_size_mul_check_overflow(a, b, ret)

SDL_FORCE_INLINE bool SDL_sdlop_size_add_check_overflow(size_t a, size_t b, size_t *ret)
{
    if (b > SDL_SIZE_MAX - a) {
        return false;
    }
    *ret = a + b;
    return true;
}

SDL_FORCE_INLINE bool SDL_sdlop_size_mul_check_overflow(size_t a, size_t b, size_t *ret)
{
    if (a != 0 && b > SDL_SIZE_MAX / a) {
        return false;
    }
    *ret = a * b;
    return true;
}

#define SDL_arraysize(array) (sizeof(array)/sizeof(array[0]))
#define SDL_TABLESIZE(table) SDL_arraysize(table)
#define SDL_STRINGIFY_ARG(arg) #arg

#ifdef __cplusplus
#define SDL_stack_alloc(type, count) ((type *)alloca(sizeof(type) * (count)))
#define SDL_stack_free(data) ((void)0)
#else
#define SDL_stack_alloc(type, count) ((type *)alloca(sizeof(type) * (count)))
#define SDL_stack_free(data) ((void)0)
#endif

#define SDL_zero(x) SDL_memset(&(x), 0, sizeof((x)))
#define SDL_zerop(x) SDL_memset((x), 0, sizeof(*(x)))
#define SDL_zeron(x, n) SDL_memset((x), 0, sizeof(*(x)) * (n))
#define SDL_zeroa(x) SDL_memset((x), 0, sizeof(x))
#define SDL_copyp(dst, src) { SDL_COMPILE_TIME_ASSERT(SDL_copyp, sizeof (*(dst)) == sizeof (*(src))); SDL_memcpy((dst), (src), sizeof (*(src))); }

#define SDL_min(x, y) (((x) < (y)) ? (x) : (y))
#define SDL_max(x, y) (((x) > (y)) ? (x) : (y))
#define SDL_clamp(x, a, b) (((x) < (a)) ? (a) : (((x) > (b)) ? (b) : (x)))

#define SDL_va_copy(dst, src) va_copy(dst, src)

/* SDL_FUNCTION/SDL_FILE/SDL_LINE/SDL_NULL_WHILE_LOOP_CONDITION live in
   SDL_assert.h upstream (copied verbatim by the generator), so they are not
   redefined here. */

/* SDL3's debugger-break helper. SDLop uses the compiler builtin instead of
   upstream's per-architecture inline assembly. */
#ifndef SDL_TriggerBreakpoint
#if defined(__GNUC__) || defined(__clang__)
#define SDL_TriggerBreakpoint() __builtin_trap()
#elif defined(_MSC_VER)
#define SDL_TriggerBreakpoint() __debugbreak()
#else
#define SDL_TriggerBreakpoint() ((void)0)
#endif
#endif

/* ------------------------------------------------------------------------- */
/* Functions                                                                 */
/* ------------------------------------------------------------------------- */

#ifdef __cplusplus
extern "C" {
#endif

/* Memory */
extern SDL_DECLSPEC SDL_MALLOC void * SDLCALL SDL_malloc(size_t size);
extern SDL_DECLSPEC SDL_MALLOC SDL_ALLOC_SIZE2(1, 2) void * SDLCALL SDL_calloc(size_t nmemb, size_t size);
extern SDL_DECLSPEC SDL_ALLOC_SIZE(2) void * SDLCALL SDL_realloc(void *mem, size_t size);
extern SDL_DECLSPEC void SDLCALL SDL_free(void *mem);

typedef void *(SDLCALL *SDL_malloc_func)(size_t size);
typedef void *(SDLCALL *SDL_calloc_func)(size_t nmemb, size_t size);
typedef void *(SDLCALL *SDL_realloc_func)(void *mem, size_t size);
typedef void (SDLCALL *SDL_free_func)(void *mem);

extern SDL_DECLSPEC void SDLCALL SDL_GetMemoryFunctions(SDL_malloc_func *malloc_func,
                                                        SDL_calloc_func *calloc_func,
                                                        SDL_realloc_func *realloc_func,
                                                        SDL_free_func *free_func);
extern SDL_DECLSPEC bool SDLCALL SDL_SetMemoryFunctions(SDL_malloc_func malloc_func,
                                                        SDL_calloc_func calloc_func,
                                                        SDL_realloc_func realloc_func,
                                                        SDL_free_func free_func);
extern SDL_DECLSPEC void SDLCALL SDL_GetOriginalMemoryFunctions(SDL_malloc_func *malloc_func,
                                                                SDL_calloc_func *calloc_func,
                                                                SDL_realloc_func *realloc_func,
                                                                SDL_free_func *free_func);

extern SDL_DECLSPEC void * SDLCALL SDL_memset(SDL_OUT_BYTECAP(len) void *dst, int c, size_t len);
#define SDL_memset4(dst, val, len) SDL_memset(dst, val, (len) * 4)
extern SDL_DECLSPEC void * SDLCALL SDL_memcpy(SDL_OUT_BYTECAP(len) void *dst, SDL_IN_BYTECAP(len) const void *src, size_t len);
extern SDL_DECLSPEC void * SDLCALL SDL_memmove(SDL_OUT_BYTECAP(len) void *dst, SDL_IN_BYTECAP(len) const void *src, size_t len);
extern SDL_DECLSPEC int SDLCALL SDL_memcmp(const void *s1, const void *s2, size_t len);

/* Strings */
extern SDL_DECLSPEC size_t SDLCALL SDL_strlen(const char *str);
extern SDL_DECLSPEC size_t SDLCALL SDL_strnlen(const char *str, size_t maxlen);
extern SDL_DECLSPEC size_t SDLCALL SDL_strlcpy(char *dst, const char *src, size_t maxlen);
extern SDL_DECLSPEC size_t SDLCALL SDL_utf8strlcpy(char *dst, const char *src, size_t dst_bytes);
extern SDL_DECLSPEC size_t SDLCALL SDL_utf8strlen(const char *str);
extern SDL_DECLSPEC size_t SDLCALL SDL_utf8strnlen(const char *str, size_t bytes);
extern SDL_DECLSPEC size_t SDLCALL SDL_strlcat(char *dst, const char *src, size_t maxlen);
extern SDL_DECLSPEC SDL_MALLOC char * SDLCALL SDL_strdup(const char *str);
extern SDL_DECLSPEC SDL_MALLOC char * SDLCALL SDL_strndup(const char *str, size_t maxlen);
extern SDL_DECLSPEC char * SDLCALL SDL_strrev(char *str);
extern SDL_DECLSPEC char * SDLCALL SDL_strupr(char *str);
extern SDL_DECLSPEC char * SDLCALL SDL_strlwr(char *str);
extern SDL_DECLSPEC char * SDLCALL SDL_strchr(const char *str, int c);
extern SDL_DECLSPEC char * SDLCALL SDL_strrchr(const char *str, int c);
extern SDL_DECLSPEC char * SDLCALL SDL_strstr(const char *haystack, const char *needle);
extern SDL_DECLSPEC char * SDLCALL SDL_strcasestr(const char *haystack, const char *needle);
extern SDL_DECLSPEC char * SDLCALL SDL_strtok_r(char *str, const char *delim, char **saveptr);
extern SDL_DECLSPEC size_t SDLCALL SDL_strspn(const char *str, const char *accept);
extern SDL_DECLSPEC size_t SDLCALL SDL_strcspn(const char *str, const char *reject);
extern SDL_DECLSPEC char * SDLCALL SDL_strpbrk(const char *str, const char *breakset);
extern SDL_DECLSPEC int SDLCALL SDL_strcmp(const char *str1, const char *str2);
extern SDL_DECLSPEC int SDLCALL SDL_strncmp(const char *str1, const char *str2, size_t maxlen);
extern SDL_DECLSPEC int SDLCALL SDL_strcasecmp(const char *str1, const char *str2);
extern SDL_DECLSPEC int SDLCALL SDL_strncasecmp(const char *str1, const char *str2, size_t maxlen);

extern SDL_DECLSPEC int SDLCALL SDL_sscanf(const char *text, SDL_SCANF_FORMAT_STRING const char *fmt, ...) SDL_SCANF_VARARG_FUNC(2);
extern SDL_DECLSPEC int SDLCALL SDL_vsscanf(const char *text, const char *fmt, va_list ap) SDL_SCANF_VARARG_FUNCV(2);
extern SDL_DECLSPEC int SDLCALL SDL_snprintf(SDL_OUT_Z_CAP(maxlen) char *text, size_t maxlen, SDL_PRINTF_FORMAT_STRING const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(3);
extern SDL_DECLSPEC int SDLCALL SDL_vsnprintf(SDL_OUT_Z_CAP(maxlen) char *text, size_t maxlen, const char *fmt, va_list ap) SDL_PRINTF_VARARG_FUNCV(3);
extern SDL_DECLSPEC int SDLCALL SDL_asprintf(char **strp, SDL_PRINTF_FORMAT_STRING const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(2);
extern SDL_DECLSPEC int SDLCALL SDL_vasprintf(char **strp, const char *fmt, va_list ap) SDL_PRINTF_VARARG_FUNCV(2);

/* Floating point: thin wrappers over libm, exactly the SDL3 surface. */
extern SDL_DECLSPEC double SDLCALL SDL_acos(double x);
extern SDL_DECLSPEC float SDLCALL SDL_acosf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_asin(double x);
extern SDL_DECLSPEC float SDLCALL SDL_asinf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_atan(double x);
extern SDL_DECLSPEC float SDLCALL SDL_atanf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_atan2(double y, double x);
extern SDL_DECLSPEC float SDLCALL SDL_atan2f(float y, float x);
extern SDL_DECLSPEC double SDLCALL SDL_ceil(double x);
extern SDL_DECLSPEC float SDLCALL SDL_ceilf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_copysign(double x, double y);
extern SDL_DECLSPEC float SDLCALL SDL_copysignf(float x, float y);
extern SDL_DECLSPEC double SDLCALL SDL_cos(double x);
extern SDL_DECLSPEC float SDLCALL SDL_cosf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_exp(double x);
extern SDL_DECLSPEC float SDLCALL SDL_expf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_fabs(double x);
extern SDL_DECLSPEC float SDLCALL SDL_fabsf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_floor(double x);
extern SDL_DECLSPEC float SDLCALL SDL_floorf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_fmod(double x, double y);
extern SDL_DECLSPEC float SDLCALL SDL_fmodf(float x, float y);
extern SDL_DECLSPEC double SDLCALL SDL_log(double x);
extern SDL_DECLSPEC float SDLCALL SDL_logf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_log10(double x);
extern SDL_DECLSPEC float SDLCALL SDL_log10f(float x);
extern SDL_DECLSPEC double SDLCALL SDL_pow(double x, double y);
extern SDL_DECLSPEC float SDLCALL SDL_powf(float x, float y);
extern SDL_DECLSPEC double SDLCALL SDL_round(double x);
extern SDL_DECLSPEC float SDLCALL SDL_roundf(float x);
extern SDL_DECLSPEC long SDLCALL SDL_lround(double x);
extern SDL_DECLSPEC long SDLCALL SDL_lroundf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_scalbn(double x, int n);
extern SDL_DECLSPEC float SDLCALL SDL_scalbnf(float x, int n);
extern SDL_DECLSPEC double SDLCALL SDL_sin(double x);
extern SDL_DECLSPEC float SDLCALL SDL_sinf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_sqrt(double x);
extern SDL_DECLSPEC float SDLCALL SDL_sqrtf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_tan(double x);
extern SDL_DECLSPEC float SDLCALL SDL_tanf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_trunc(double x);
extern SDL_DECLSPEC float SDLCALL SDL_truncf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_cbrt(double x);
extern SDL_DECLSPEC float SDLCALL SDL_cbrtf(float x);
extern SDL_DECLSPEC double SDLCALL SDL_hypot(double x, double y);
extern SDL_DECLSPEC float SDLCALL SDL_hypotf(float x, float y);
extern SDL_DECLSPEC double SDLCALL SDL_fma(double x, double y, double z);
extern SDL_DECLSPEC float SDLCALL SDL_fmaf(float x, float y, float z);
extern SDL_DECLSPEC int SDLCALL SDL_abs(int x);

/* Conversion */
extern SDL_DECLSPEC int SDLCALL SDL_atoi(const char *str);
extern SDL_DECLSPEC double SDLCALL SDL_atof(const char *str);
extern SDL_DECLSPEC long SDLCALL SDL_strtol(const char *str, char **endp, int base);
extern SDL_DECLSPEC unsigned long SDLCALL SDL_strtoul(const char *str, char **endp, int base);
extern SDL_DECLSPEC long long SDLCALL SDL_strtoll(const char *str, char **endp, int base);
extern SDL_DECLSPEC unsigned long long SDLCALL SDL_strtoull(const char *str, char **endp, int base);
extern SDL_DECLSPEC double SDLCALL SDL_strtod(const char *str, char **endp);
extern SDL_DECLSPEC int SDLCALL SDL_itoa(int value, char *str, int radix);
extern SDL_DECLSPEC int SDLCALL SDL_uitoa(unsigned int value, char *str, int radix);
extern SDL_DECLSPEC int SDLCALL SDL_ltoa(long value, char *str, int radix);
extern SDL_DECLSPEC int SDLCALL SDL_ultoa(unsigned long value, char *str, int radix);
extern SDL_DECLSPEC int SDLCALL SDL_lltoa(long long value, char *str, int radix);
extern SDL_DECLSPEC int SDLCALL SDL_ulltoa(unsigned long long value, char *str, int radix);

/* Environment. SDL3 wraps these in SDL_Environment objects (3.2); SDLop
   exposes the process environment directly. */
extern SDL_DECLSPEC const char * SDLCALL SDL_getenv(const char *name);
extern SDL_DECLSPEC int SDLCALL SDL_setenv_unsafe(const char *name, const char *value, int overwrite);
extern SDL_DECLSPEC int SDLCALL SDL_unsetenv_unsafe(const char *name);

/* Sorting. SDL3's SDL_qsort_r uses its own compare convention. */
typedef int (SDLCALL *SDL_CompareCallback)(const void *a, const void *b);
typedef int (SDLCALL *SDL_CompareCallback_r)(void *userdata, const void *a, const void *b);
extern SDL_DECLSPEC void SDLCALL SDL_qsort(void *base, size_t nmemb, size_t size, SDL_CompareCallback compare);
extern SDL_DECLSPEC void SDLCALL SDL_qsort_r(void *base, size_t nmemb, size_t size,
                                             SDL_CompareCallback_r compare, void *userdata);

/* Ctype. SDL3 has these as macros/inline wrappers around the C library. */
extern SDL_DECLSPEC int SDLCALL SDL_isalpha(int x);
extern SDL_DECLSPEC int SDLCALL SDL_isalnum(int x);
extern SDL_DECLSPEC int SDLCALL SDL_isblank(int x);
extern SDL_DECLSPEC int SDLCALL SDL_iscntrl(int x);
extern SDL_DECLSPEC int SDLCALL SDL_isdigit(int x);
extern SDL_DECLSPEC int SDLCALL SDL_isxdigit(int x);
extern SDL_DECLSPEC int SDLCALL SDL_ispunct(int x);
extern SDL_DECLSPEC int SDLCALL SDL_isspace(int x);
extern SDL_DECLSPEC int SDLCALL SDL_isupper(int x);
extern SDL_DECLSPEC int SDLCALL SDL_islower(int x);
extern SDL_DECLSPEC int SDLCALL SDL_isprint(int x);
extern SDL_DECLSPEC int SDLCALL SDL_isgraph(int x);
extern SDL_DECLSPEC int SDLCALL SDL_toupper(int x);
extern SDL_DECLSPEC int SDLCALL SDL_tolower(int x);
/* Stdlib: PRNG. SDL3 exposes a thread-safe global PRNG plus an explicit-state
   variant; SDLop implements both with xoshiro256**. */
extern SDL_DECLSPEC int SDLCALL SDL_rand(int n);
extern SDL_DECLSPEC float SDLCALL SDL_randf(void);
extern SDL_DECLSPEC void SDLCALL SDL_srand(Uint64 seed);
extern SDL_DECLSPEC Sint32 SDLCALL SDL_rand_r(Uint64 *state, Sint32 n);
extern SDL_DECLSPEC float SDLCALL SDL_randf_r(Uint64 *state);
extern SDL_DECLSPEC Uint32 SDLCALL SDL_rand_bits(void);
extern SDL_DECLSPEC Uint32 SDLCALL SDL_rand_bits_r(Uint64 *state);

#ifdef __cplusplus
}
#endif

/* SDLop-internal: the active allocator, so SDL_SetMemoryFunctions() works for
   library-internal allocations too. Not part of the SDL3 API. */
extern SDL_malloc_func SDL_sdlop_malloc;
extern SDL_calloc_func SDL_sdlop_calloc;
extern SDL_realloc_func SDL_sdlop_realloc;
extern SDL_free_func SDL_sdlop_free;

#endif /* SDL_stdinc_h_ */
