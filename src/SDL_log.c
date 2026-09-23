/*
  SDLop - logging.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL_log.h>
#include <stdio.h>
#include <stdarg.h>

static const char *priority_names[] = {
    "TRACE", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"
};

static SDL_LogPriority min_priority = SDL_LOG_PRIORITY_INFO;

void SDL_LogMessage(int category, SDL_LogPriority priority, const char *fmt, ...)
{
    if (priority < min_priority) {
        return;
    }
    va_list ap;
    fprintf(stderr, "[%s] ", priority_names[priority - 1]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    (void)category;
}

void SDL_Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[INFO] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define SDLOP_LOG_FN(name, prio)                        \
    void name(int category, const char *fmt, ...)       \
    {                                                   \
        if ((prio) < min_priority) {                    \
            return;                                     \
        }                                               \
        va_list ap;                                     \
        fprintf(stderr, "[%s] ", priority_names[(prio) - 1]); \
        va_start(ap, fmt);                              \
        vfprintf(stderr, fmt, ap);                      \
        va_end(ap);                                     \
        fputc('\n', stderr);                            \
        (void)category;                                 \
    }

SDLOP_LOG_FN(SDL_LogVerbose, SDL_LOG_PRIORITY_VERBOSE)
SDLOP_LOG_FN(SDL_LogDebug, SDL_LOG_PRIORITY_DEBUG)
SDLOP_LOG_FN(SDL_LogInfo, SDL_LOG_PRIORITY_INFO)
SDLOP_LOG_FN(SDL_LogWarn, SDL_LOG_PRIORITY_WARN)
SDLOP_LOG_FN(SDL_LogError, SDL_LOG_PRIORITY_ERROR)
