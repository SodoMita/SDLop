/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.

  Minimal stand-in for <SDL3/SDL_stdinc.h>: the basic typedefs and macros
  used by the SDL3 API subset implemented by SDLop.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_stdinc_h_
#define SDL_stdinc_h_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

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
typedef Uint32 SDL_TouchID;
typedef Uint32 SDL_PropertiesID;
typedef Uint32 SDL_MouseButtonFlags;
typedef Uint32 SDL_InitFlags;

#define SDL_arraysize(array) (sizeof(array) / sizeof(array[0]))
#define SDL_memset memset
#define SDL_memcpy memcpy
#define SDL_zero(x) SDL_memset(&(x), 0, sizeof(x))
#define SDL_min(x, y) (((x) < (y)) ? (x) : (y))
#define SDL_max(x, y) (((x) > (y)) ? (x) : (y))
#define SDL_clamp(x, a, b) (((x) < (a)) ? (a) : (((x) > (b)) ? (b) : (x)))

#define SDL_UINT64_C(c) UINT64_C(c)

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

#endif /* SDL_stdinc_h_ */
