/*
  SDLop - logging (SDL_log.h).

  Port of SDL3's src/SDL_log.c (release-3.4.20) for POSIX: per-category
  priorities, SDL_HINT_LOGGING parsing ("app=info,input=debug,*=warn"),
  priority prefixes, replaceable output function. Messages go to stderr as
  "<prefix><message>\n" exactly like stock SDL3 on Linux.

  SDLop extra: when SDL_LOGGING is unset, $SDLOP_LOG_PRIORITY (a single
  priority name) is honoured as "*=<priority>" for compatibility with
  earlier SDLop releases.

  Copyright (C) 1997-2026 Sam Lantinga (SDL3 parts), zlib license.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include <stdarg.h>

#define SDL_MAX_LOG_MESSAGE_STACK 256
#define DEFAULT_CATEGORY -1

typedef struct SDL_LogLevel
{
    int category;
    SDL_LogPriority priority;
    struct SDL_LogLevel *next;
} SDL_LogLevel;

static void SDLCALL SDL_LogOutput(void *userdata, int category, SDL_LogPriority priority, const char *message);

static pthread_mutex_t SDL_log_lock;
static pthread_mutex_t SDL_log_function_lock;
static pthread_once_t log_locks_once = PTHREAD_ONCE_INIT;
static _Atomic int SDL_log_initialized;
static SDL_LogLevel *SDL_loglevels;
static SDL_LogPriority SDL_log_priorities[SDL_LOG_CATEGORY_CUSTOM];
static SDL_LogPriority SDL_log_default_priority;
static SDL_LogOutputFunction SDL_log_function = SDL_LogOutput;
static void *SDL_log_userdata = NULL;
static char *SDL_priority_prefixes[SDL_LOG_PRIORITY_COUNT];

static const char *const SDL_priority_names[] = {
    NULL, "TRACE", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"
};

/* If this list changes, update the documentation for SDL_HINT_LOGGING */
static const char *const SDL_category_names[] = {
    "APP", "ERROR", "ASSERT", "SYSTEM", "AUDIO", "VIDEO", "RENDER", "INPUT", "TEST", "GPU"
};
SDL_COMPILE_TIME_ASSERT(category_names, SDL_arraysize(SDL_category_names) == SDL_LOG_CATEGORY_RESERVED2);

static void init_locks(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&SDL_log_lock, &attr);
    pthread_mutex_init(&SDL_log_function_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void SDLCALL SDL_LoggingChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    (void)userdata;
    (void)name;
    (void)oldValue;
    (void)hint;
    SDL_ResetLogPriorities();
}

static void SDL_CheckInitLog(void)
{
    pthread_once(&log_locks_once, init_locks);
    if (SDL_log_initialized) {
        return;
    }
    pthread_mutex_lock(&SDL_log_lock);
    if (!SDL_log_initialized) {
        SDL_log_initialized = 1;
        /* calls SDL_ResetLogPriorities() right away with the current value */
        SDL_AddHintCallback(SDL_HINT_LOGGING, SDL_LoggingChanged, NULL);
    }
    pthread_mutex_unlock(&SDL_log_lock);
}

static void CleanupLogPriorities(void)
{
    while (SDL_loglevels) {
        SDL_LogLevel *entry = SDL_loglevels;
        SDL_loglevels = entry->next;
        SDL_free(entry);
    }
}

static void CleanupLogPrefixes(void)
{
    for (int i = 0; i < (int)SDL_arraysize(SDL_priority_prefixes); ++i) {
        SDL_free(SDL_priority_prefixes[i]);
        SDL_priority_prefixes[i] = NULL;
    }
}

/* SDL_Quit(): forget priorities/prefixes; the next log call re-reads the
 * hint (hint callbacks are dropped by SDL_Quit as well) */
void SDLOP_QuitLog(void)
{
    pthread_once(&log_locks_once, init_locks);
    pthread_mutex_lock(&SDL_log_lock);
    if (SDL_log_initialized) {
        SDL_RemoveHintCallback(SDL_HINT_LOGGING, SDL_LoggingChanged, NULL);
        CleanupLogPriorities();
        SDL_log_initialized = 0;
    }
    pthread_mutex_unlock(&SDL_log_lock);
    pthread_mutex_lock(&SDL_log_function_lock);
    CleanupLogPrefixes();
    pthread_mutex_unlock(&SDL_log_function_lock);
}

void SDL_SetLogPriorities(SDL_LogPriority priority)
{
    SDL_CheckInitLog();
    pthread_mutex_lock(&SDL_log_lock);
    CleanupLogPriorities();
    SDL_log_default_priority = priority;
    for (int i = 0; i < (int)SDL_arraysize(SDL_log_priorities); ++i) {
        SDL_log_priorities[i] = priority;
    }
    pthread_mutex_unlock(&SDL_log_lock);
}

void SDL_SetLogPriority(int category, SDL_LogPriority priority)
{
    SDL_CheckInitLog();
    pthread_mutex_lock(&SDL_log_lock);
    if (category >= 0 && category < (int)SDL_arraysize(SDL_log_priorities)) {
        SDL_log_priorities[category] = priority;
    } else {
        SDL_LogLevel *entry;
        for (entry = SDL_loglevels; entry; entry = entry->next) {
            if (entry->category == category) {
                entry->priority = priority;
                break;
            }
        }
        if (!entry) {
            entry = (SDL_LogLevel *)SDL_malloc(sizeof(*entry));
            if (entry) {
                entry->category = category;
                entry->priority = priority;
                entry->next = SDL_loglevels;
                SDL_loglevels = entry;
            }
        }
    }
    pthread_mutex_unlock(&SDL_log_lock);
}

SDL_LogPriority SDL_GetLogPriority(int category)
{
    SDL_CheckInitLog();
    if (category >= 0 && category < (int)SDL_arraysize(SDL_log_priorities)) {
        return SDL_log_priorities[category];
    }
    SDL_LogPriority priority = SDL_LOG_PRIORITY_INVALID;
    pthread_mutex_lock(&SDL_log_lock);
    for (SDL_LogLevel *entry = SDL_loglevels; entry; entry = entry->next) {
        if (entry->category == category) {
            priority = entry->priority;
            break;
        }
    }
    if (priority == SDL_LOG_PRIORITY_INVALID) {
        priority = SDL_log_default_priority;
    }
    pthread_mutex_unlock(&SDL_log_lock);
    return priority;
}

static bool ParseLogCategory(const char *string, size_t length, int *category)
{
    if (SDL_isdigit((unsigned char)*string)) {
        *category = SDL_atoi(string);
        return true;
    }
    if (*string == '*') {
        *category = DEFAULT_CATEGORY;
        return true;
    }
    for (int i = 0; i < (int)SDL_arraysize(SDL_category_names); ++i) {
        if (SDL_strncasecmp(string, SDL_category_names[i], length) == 0) {
            *category = i;
            return true;
        }
    }
    return false;
}

static bool ParseLogPriority(const char *string, size_t length, SDL_LogPriority *priority)
{
    if (SDL_isdigit((unsigned char)*string)) {
        int i = SDL_atoi(string);
        if (i == 0) {
            *priority = SDL_LOG_PRIORITY_COUNT; /* 0 = disable this category */
            return true;
        }
        if (i > SDL_LOG_PRIORITY_INVALID && i < SDL_LOG_PRIORITY_COUNT) {
            *priority = (SDL_LogPriority)i;
            return true;
        }
        return false;
    }
    if (SDL_strncasecmp(string, "quiet", length) == 0) {
        *priority = SDL_LOG_PRIORITY_COUNT;
        return true;
    }
    for (int i = SDL_LOG_PRIORITY_INVALID + 1; i < SDL_LOG_PRIORITY_COUNT; ++i) {
        if (SDL_strncasecmp(string, SDL_priority_names[i], length) == 0) {
            *priority = (SDL_LogPriority)i;
            return true;
        }
    }
    return false;
}

static void ParseLogPriorities(const char *hint)
{
    int category = DEFAULT_CATEGORY;
    SDL_LogPriority priority = SDL_LOG_PRIORITY_INVALID;

    if (SDL_strchr(hint, '=') == NULL) {
        if (ParseLogPriority(hint, SDL_strlen(hint), &priority)) {
            SDL_SetLogPriorities(priority);
        }
        return;
    }

    const char *next;
    for (const char *name = hint; name; name = next) {
        const char *sep = SDL_strchr(name, '=');
        if (!sep) {
            break;
        }
        next = SDL_strchr(sep, ',');
        if (next) {
            ++next;
        }
        if (ParseLogCategory(name, (size_t)(sep - name), &category)) {
            const char *value = sep + 1;
            size_t len = next ? (size_t)(next - value - 1) : SDL_strlen(value);
            if (ParseLogPriority(value, len, &priority)) {
                if (category == DEFAULT_CATEGORY) {
                    for (int i = 0; i < (int)SDL_arraysize(SDL_log_priorities); ++i) {
                        if (SDL_log_priorities[i] == SDL_LOG_PRIORITY_INVALID) {
                            SDL_log_priorities[i] = priority;
                        }
                    }
                    SDL_log_default_priority = priority;
                } else {
                    SDL_SetLogPriority(category, priority);
                }
            }
        }
    }
}

void SDL_ResetLogPriorities(void)
{
    SDL_CheckInitLog();
    pthread_mutex_lock(&SDL_log_lock);

    const char *env = SDL_getenv("DEBUG_INVOCATION");
    bool debug = (env && *env && *env != '0');

    CleanupLogPriorities();
    SDL_log_default_priority = SDL_LOG_PRIORITY_INVALID;
    for (int i = 0; i < (int)SDL_arraysize(SDL_log_priorities); ++i) {
        SDL_log_priorities[i] = SDL_LOG_PRIORITY_INVALID;
    }

    const char *hint = SDL_GetHint(SDL_HINT_LOGGING);
    if (hint) {
        ParseLogPriorities(hint);
    } else {
        const char *legacy = SDL_getenv("SDLOP_LOG_PRIORITY");
        SDL_LogPriority p;
        if (legacy && *legacy && ParseLogPriority(legacy, SDL_strlen(legacy), &p)) {
            SDL_log_default_priority = p;
            for (int i = 0; i < (int)SDL_arraysize(SDL_log_priorities); ++i) {
                SDL_log_priorities[i] = p;
            }
        }
    }

    if (SDL_log_default_priority == SDL_LOG_PRIORITY_INVALID) {
        SDL_log_default_priority = SDL_LOG_PRIORITY_ERROR;
    }
    for (int i = 0; i < (int)SDL_arraysize(SDL_log_priorities); ++i) {
        if (SDL_log_priorities[i] != SDL_LOG_PRIORITY_INVALID) {
            continue;
        }
        switch (i) {
        case SDL_LOG_CATEGORY_APPLICATION:
            SDL_log_priorities[i] = debug ? SDL_LOG_PRIORITY_DEBUG : SDL_LOG_PRIORITY_INFO;
            break;
        case SDL_LOG_CATEGORY_ASSERT:
            SDL_log_priorities[i] = SDL_LOG_PRIORITY_WARN;
            break;
        case SDL_LOG_CATEGORY_TEST:
            SDL_log_priorities[i] = SDL_LOG_PRIORITY_VERBOSE;
            break;
        default:
            SDL_log_priorities[i] = debug ? SDL_LOG_PRIORITY_DEBUG : SDL_LOG_PRIORITY_ERROR;
            break;
        }
    }
    pthread_mutex_unlock(&SDL_log_lock);
}

static const char *GetLogPriorityPrefix(SDL_LogPriority priority)
{
    if (priority <= SDL_LOG_PRIORITY_INVALID || priority >= SDL_LOG_PRIORITY_COUNT) {
        return "";
    }
    if (SDL_priority_prefixes[priority]) {
        return SDL_priority_prefixes[priority];
    }
    switch (priority) {
    case SDL_LOG_PRIORITY_WARN:
        return "WARNING: ";
    case SDL_LOG_PRIORITY_ERROR:
    case SDL_LOG_PRIORITY_CRITICAL:
        return "ERROR: ";
    default:
        return "";
    }
}

bool SDL_SetLogPriorityPrefix(SDL_LogPriority priority, const char *prefix)
{
    if (priority <= SDL_LOG_PRIORITY_INVALID || priority >= SDL_LOG_PRIORITY_COUNT) {
        return SDL_InvalidParamError("priority");
    }
    char *prefix_copy = SDL_strdup((!prefix || !*prefix) ? "" : prefix);
    if (!prefix_copy) {
        return false;
    }
    pthread_once(&log_locks_once, init_locks);
    pthread_mutex_lock(&SDL_log_function_lock);
    SDL_free(SDL_priority_prefixes[priority]);
    SDL_priority_prefixes[priority] = prefix_copy;
    pthread_mutex_unlock(&SDL_log_function_lock);
    return true;
}

void SDL_LogMessageV(int category, SDL_LogPriority priority, const char *fmt, va_list ap)
{
    char stack_buf[SDL_MAX_LOG_MESSAGE_STACK];
    char *message;
    va_list aq;

    if (!SDL_log_function) {
        return;
    }
    if (priority < SDL_GetLogPriority(category)) {
        return;
    }

    va_copy(aq, ap);
    int len = SDL_vsnprintf(stack_buf, sizeof(stack_buf), fmt, aq);
    va_end(aq);
    if (len < 0) {
        return;
    }
    if ((size_t)len >= sizeof(stack_buf)) {
        message = (char *)SDL_malloc((size_t)len + 1);
        if (!message) {
            return;
        }
        va_copy(aq, ap);
        len = SDL_vsnprintf(message, (size_t)len + 1, fmt, aq);
        va_end(aq);
    } else {
        message = stack_buf;
    }

    /* chop off final endline ("\n" or "\r\n") */
    if (len > 0 && message[len - 1] == '\n') {
        message[--len] = '\0';
        if (len > 0 && message[len - 1] == '\r') {
            message[--len] = '\0';
        }
    }

    pthread_mutex_lock(&SDL_log_function_lock);
    SDL_log_function(SDL_log_userdata, category, priority, message);
    pthread_mutex_unlock(&SDL_log_function_lock);

    if (message != stack_buf) {
        SDL_free(message);
    }
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

#define SDLOP_LOG_FN(name, prio)                              \
    void name(int category, const char *fmt, ...)             \
    {                                                         \
        va_list ap;                                           \
        va_start(ap, fmt);                                    \
        SDL_LogMessageV(category, prio, fmt, ap);             \
        va_end(ap);                                           \
    }

SDLOP_LOG_FN(SDL_LogTrace, SDL_LOG_PRIORITY_TRACE)
SDLOP_LOG_FN(SDL_LogVerbose, SDL_LOG_PRIORITY_VERBOSE)
SDLOP_LOG_FN(SDL_LogDebug, SDL_LOG_PRIORITY_DEBUG)
SDLOP_LOG_FN(SDL_LogInfo, SDL_LOG_PRIORITY_INFO)
SDLOP_LOG_FN(SDL_LogWarn, SDL_LOG_PRIORITY_WARN)
SDLOP_LOG_FN(SDL_LogError, SDL_LOG_PRIORITY_ERROR)
SDLOP_LOG_FN(SDL_LogCritical, SDL_LOG_PRIORITY_CRITICAL)

static void SDLCALL SDL_LogOutput(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    (void)userdata;
    (void)category;
    (void)fprintf(stderr, "%s%s\n", GetLogPriorityPrefix(priority), message);
}

SDL_LogOutputFunction SDL_GetDefaultLogOutputFunction(void)
{
    return SDL_LogOutput;
}

void SDL_GetLogOutputFunction(SDL_LogOutputFunction *callback, void **userdata)
{
    pthread_once(&log_locks_once, init_locks);
    pthread_mutex_lock(&SDL_log_function_lock);
    if (callback) {
        *callback = SDL_log_function;
    }
    if (userdata) {
        *userdata = SDL_log_userdata;
    }
    pthread_mutex_unlock(&SDL_log_function_lock);
}

void SDL_SetLogOutputFunction(SDL_LogOutputFunction callback, void *userdata)
{
    pthread_once(&log_locks_once, init_locks);
    pthread_mutex_lock(&SDL_log_function_lock);
    SDL_log_function = callback;
    SDL_log_userdata = userdata;
    pthread_mutex_unlock(&SDL_log_function_lock);
}
