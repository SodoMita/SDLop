/*
  SDLop - logging.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL_log.h>
#include <stdio.h>
#include <stdarg.h>
#include <strings.h>
#include <stdlib.h>

static const char *priority_names[] = {
    "TRACE", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"
};

static SDL_LogPriority min_priority = SDL_LOG_PRIORITY_INVALID; /* lazy: from $SDLOP_LOG_PRIORITY, else INFO */

static SDL_LogPriority sdlop_default_priority(void)
{
    const char *v = getenv("SDLOP_LOG_PRIORITY");
    if (!v || !*v) {
        return SDL_LOG_PRIORITY_INFO;
    }
    struct { const char *name; SDL_LogPriority prio; } map[] = {
        { "trace", SDL_LOG_PRIORITY_TRACE },
        { "verbose", SDL_LOG_PRIORITY_VERBOSE },
        { "debug", SDL_LOG_PRIORITY_DEBUG },
        { "info", SDL_LOG_PRIORITY_INFO },
        { "warn", SDL_LOG_PRIORITY_WARN },
        { "error", SDL_LOG_PRIORITY_ERROR },
        { "critical", SDL_LOG_PRIORITY_CRITICAL },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcasecmp(v, map[i].name) == 0) {
            return map[i].prio;
        }
    }
    return SDL_LOG_PRIORITY_INFO;
}

void SDL_SetLogPriority(int category, SDL_LogPriority priority)
{
    (void)category; /* SDLop uses a single global threshold */
    if (priority >= SDL_LOG_PRIORITY_TRACE && priority < SDL_LOG_PRIORITY_COUNT) {
        min_priority = priority;
    }
}

static SDL_LogPriority sdlop_min_priority(void)
{
    if (min_priority == SDL_LOG_PRIORITY_INVALID) {
        min_priority = sdlop_default_priority();
    }
    return min_priority;
}

void SDL_LogMessage(int category, SDL_LogPriority priority, const char *fmt, ...)
{
    if (priority < sdlop_min_priority()) {
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
        if ((prio) < sdlop_min_priority()) {            \
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
