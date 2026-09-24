/*
  SDLop -- SDL_log.h.

  Formatting and the priority table are the same as SDL3's; the default output
  function writes one line per message to stderr, which is what SDL's default
  does on Linux.
*/

#include "../sdlop_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define SDLOP_LOG_CATEGORY_COUNT 20
#define SDLOP_LOG_MESSAGE_LEN 4096

static SDL_LogPriority sdlop_log_priority[SDLOP_LOG_CATEGORY_COUNT] = {
    SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO,
    SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO,
    SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO,
    SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO,
    SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO,
    SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO,
    SDL_LOG_PRIORITY_INFO, SDL_LOG_PRIORITY_INFO
};

static const char *sdlop_log_prefix[SDL_LOG_PRIORITY_COUNT] = {
    "", "", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"
};

static const char *sdlop_log_prefix_override[SDL_LOG_PRIORITY_COUNT];

static SDL_LogOutputFunction sdlop_log_output;
static void *sdlop_log_output_userdata;
static pthread_mutex_t sdlop_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void SDLCALL sdlop_default_log_output(void *userdata, int category,
                                             SDL_LogPriority priority, const char *message)
{
    const char *prefix = sdlop_log_prefix_override[priority];
    const char *category_name;
    (void)userdata;

    if (!prefix) {
        prefix = sdlop_log_prefix[priority];
    }
    switch ((SDL_LogCategory)category) {
        case SDL_LOG_CATEGORY_APPLICATION: category_name = "APP"; break;
        case SDL_LOG_CATEGORY_ERROR:       category_name = "ERROR"; break;
        case SDL_LOG_CATEGORY_ASSERT:      category_name = "ASSERT"; break;
        case SDL_LOG_CATEGORY_SYSTEM:      category_name = "SYSTEM"; break;
        case SDL_LOG_CATEGORY_AUDIO:       category_name = "AUDIO"; break;
        case SDL_LOG_CATEGORY_VIDEO:       category_name = "VIDEO"; break;
        case SDL_LOG_CATEGORY_RENDER:      category_name = "RENDER"; break;
        case SDL_LOG_CATEGORY_INPUT:       category_name = "INPUT"; break;
        case SDL_LOG_CATEGORY_TEST:        category_name = "TEST"; break;
        default:                           category_name = "CUSTOM"; break;
    }
    if (prefix[0]) {
        fprintf(stderr, "%s: %s: %s\n", prefix, category_name, message);
    } else {
        fprintf(stderr, "%s: %s\n", category_name, message);
    }
    fflush(stderr);
}

void SDL_LogMessageV(int category, SDL_LogPriority priority, const char *fmt, va_list ap)
{
    char message[SDLOP_LOG_MESSAGE_LEN];
    SDL_LogOutputFunction output;
    void *userdata;

    if (category < 0 || category >= SDLOP_LOG_CATEGORY_COUNT) {
        return;
    }
    /* priority values grow with severity (TRACE=1 .. CRITICAL=7), so a message is
       printed when it is at least as severe as the category's threshold. */
    if (priority == SDL_LOG_PRIORITY_INVALID || priority < sdlop_log_priority[category]) {
        return;
    }

    vsnprintf(message, sizeof(message), fmt ? fmt : "", ap);

    pthread_mutex_lock(&sdlop_log_lock);
    output = sdlop_log_output ? sdlop_log_output : sdlop_default_log_output;
    userdata = sdlop_log_output ? sdlop_log_output_userdata : NULL;
    pthread_mutex_unlock(&sdlop_log_lock);
    output(userdata, category, priority, message);
}

void SDL_LogMessage(int category, SDL_LogPriority priority, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(category, priority, fmt, ap);
    va_end(ap);
}

void SDL_Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO, fmt, ap);
    va_end(ap);
}

void SDL_LogTrace(int category, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(category, SDL_LOG_PRIORITY_TRACE, fmt, ap);
    va_end(ap);
}

void SDL_LogVerbose(int category, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(category, SDL_LOG_PRIORITY_VERBOSE, fmt, ap);
    va_end(ap);
}

void SDL_LogDebug(int category, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(category, SDL_LOG_PRIORITY_DEBUG, fmt, ap);
    va_end(ap);
}

void SDL_LogInfo(int category, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(category, SDL_LOG_PRIORITY_INFO, fmt, ap);
    va_end(ap);
}

void SDL_LogWarn(int category, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(category, SDL_LOG_PRIORITY_WARN, fmt, ap);
    va_end(ap);
}

void SDL_LogError(int category, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(category, SDL_LOG_PRIORITY_ERROR, fmt, ap);
    va_end(ap);
}

void SDL_LogCritical(int category, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(category, SDL_LOG_PRIORITY_CRITICAL, fmt, ap);
    va_end(ap);
}

void SDL_SetLogPriorities(SDL_LogPriority priority)
{
    int i;
    for (i = 0; i < SDLOP_LOG_CATEGORY_COUNT; i++) {
        sdlop_log_priority[i] = priority;
    }
}

void SDL_SetLogPriority(int category, SDL_LogPriority priority)
{
    if (category >= 0 && category < SDLOP_LOG_CATEGORY_COUNT) {
        sdlop_log_priority[category] = priority;
    }
}

SDL_LogPriority SDL_GetLogPriority(int category)
{
    if (category >= 0 && category < SDLOP_LOG_CATEGORY_COUNT) {
        return sdlop_log_priority[category];
    }
    return SDL_LOG_PRIORITY_INVALID;
}

void SDL_ResetLogPriorities(void)
{
    int i;
    for (i = 0; i < SDLOP_LOG_CATEGORY_COUNT; i++) {
        sdlop_log_priority[i] = SDL_LOG_PRIORITY_INFO;
    }
}

bool SDL_SetLogPriorityPrefix(SDL_LogPriority priority, const char *prefix)
{
    if (priority <= SDL_LOG_PRIORITY_INVALID || priority >= SDL_LOG_PRIORITY_COUNT) {
        return SDL_InvalidParamError("priority");
    }
    sdlop_log_prefix_override[priority] = prefix;
    return true;
}

SDL_LogOutputFunction SDL_GetDefaultLogOutputFunction(void)
{
    return sdlop_default_log_output;
}

void SDL_GetLogOutputFunction(SDL_LogOutputFunction *callback, void **userdata)
{
    pthread_mutex_lock(&sdlop_log_lock);
    if (callback) {
        *callback = sdlop_log_output ? sdlop_log_output : sdlop_default_log_output;
    }
    if (userdata) {
        *userdata = sdlop_log_output ? sdlop_log_output_userdata : NULL;
    }
    pthread_mutex_unlock(&sdlop_log_lock);
}

void SDL_SetLogOutputFunction(SDL_LogOutputFunction callback, void *userdata)
{
    pthread_mutex_lock(&sdlop_log_lock);
    sdlop_log_output = callback;
    sdlop_log_output_userdata = userdata;
    pthread_mutex_unlock(&sdlop_log_lock);
}
