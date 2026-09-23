/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Logging (subset of <SDL3/SDL_log.h>).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_log_h_
#define SDL_log_h_

#include <SDL3/SDL_stdinc.h>

/** Log categories (subset, identical values to SDL3). */
#define SDL_LOG_CATEGORY_APPLICATION 0
#define SDL_LOG_CATEGORY_ERROR       1
#define SDL_LOG_CATEGORY_ASSERT      2
#define SDL_LOG_CATEGORY_SYSTEM      3
#define SDL_LOG_CATEGORY_AUDIO       4
#define SDL_LOG_CATEGORY_VIDEO       5
#define SDL_LOG_CATEGORY_RENDER      6
#define SDL_LOG_CATEGORY_INPUT       7
#define SDL_LOG_CATEGORY_TEST        8

/** Log priorities (subset, identical values to SDL3). */
typedef enum SDL_LogPriority
{
    SDL_LOG_PRIORITY_INVALID,
    SDL_LOG_PRIORITY_TRACE,
    SDL_LOG_PRIORITY_VERBOSE,
    SDL_LOG_PRIORITY_DEBUG,
    SDL_LOG_PRIORITY_INFO,
    SDL_LOG_PRIORITY_WARN,
    SDL_LOG_PRIORITY_ERROR,
    SDL_LOG_PRIORITY_CRITICAL,
    SDL_LOG_PRIORITY_COUNT
} SDL_LogPriority;

/**
 * Log a message with the application category and INFO priority.
 */
extern void SDL_Log(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/**
 * Log a message with the given category and priority.
 */
extern void SDL_LogMessage(int category, SDL_LogPriority priority, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

extern void SDL_LogVerbose(int category, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
extern void SDL_LogDebug(int category, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
extern void SDL_LogInfo(int category, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
extern void SDL_LogWarn(int category, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
extern void SDL_LogError(int category, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#endif /* SDL_log_h_ */
