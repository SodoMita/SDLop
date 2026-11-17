/*
  SDLop - environment variables (see <SDL3/SDL_stdinc.h>).

  Ported from SDL3 3.2.10 (src/stdlib/SDL_getenv.c), with a linked list +
  pthread mutex instead of SDL3's hash table + SDL_Mutex.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <stdlib.h>

extern char **environ;

/* one allocation per variable: { next, "name\0value\0" } */
typedef struct SDLOP_EnvVar
{
    struct SDLOP_EnvVar *next;
    char name[];
} SDLOP_EnvVar;

struct SDL_Environment
{
    pthread_mutex_t lock;
    SDLOP_EnvVar *vars;
};

static SDL_Environment *SDL_environment;
static pthread_once_t sdlop_env_once = PTHREAD_ONCE_INIT;

static SDLOP_EnvVar *find_var(SDL_Environment *env, const char *name)
{
    for (SDLOP_EnvVar *var = env->vars; var; var = var->next) {
        if (SDL_strcmp(var->name, name) == 0) {
            return var;
        }
    }
    return NULL;
}

static const char *var_value(const SDLOP_EnvVar *var)
{
    return var->name + SDL_strlen(var->name) + 1;
}

/* lock NOT held (used by population, before the env is shared) */
static bool add_var(SDL_Environment *env, const char *name, size_t name_len, const char *value)
{
    size_t value_len = SDL_strlen(value);
    SDLOP_EnvVar *var = (SDLOP_EnvVar *)SDL_malloc(sizeof(*var) + name_len + 1 + value_len + 1);
    if (!var) {
        return false;
    }
    SDL_memcpy(var->name, name, name_len);
    var->name[name_len] = '\0';
    SDL_memcpy(var->name + name_len + 1, value, value_len + 1);
    var->next = env->vars;
    env->vars = var;
    return true;
}

SDL_Environment *SDL_CreateEnvironment(bool populated)
{
    SDL_Environment *env = (SDL_Environment *)SDL_calloc(1, sizeof(*env));
    if (!env) {
        return NULL;
    }

    pthread_mutex_init(&env->lock, NULL);

    if (populated) {
        char **strings = environ;
        if (strings) {
            for (int i = 0; strings[i]; ++i) {
                const char *equal = SDL_strchr(strings[i], '=');
                if (!equal || equal == strings[i]) {
                    continue;
                }
                if (!add_var(env, strings[i], (size_t)(equal - strings[i]), equal + 1)) {
                    break; /* OOM: keep what we have */
                }
            }
        }
    }

    return env;
}

static void sdlop_init_environment(void)
{
    SDL_environment = SDL_CreateEnvironment(true);
}

SDL_Environment *SDL_GetEnvironment(void)
{
    pthread_once(&sdlop_env_once, sdlop_init_environment);
    return SDL_environment;
}

/* internal: SDL_Quit() hook (SDL3: SDL_QuitEnvironment) */
void SDLOP_EnvironmentQuit(void)
{
    SDL_Environment *env = SDL_environment;

    if (env) {
        SDL_environment = NULL;

        /* SDL_DestroyEnvironment refuses to destroy the global env (it
           compares against the global pointer), so destroy it by hand here. */
        SDLOP_EnvVar *var = env->vars;
        while (var) {
            SDLOP_EnvVar *next = var->next;
            SDL_free(var);
            var = next;
        }
        pthread_mutex_destroy(&env->lock);
        SDL_free(env);

        /* allow re-init after SDL_Quit */
        pthread_once_t fresh = PTHREAD_ONCE_INIT;
        sdlop_env_once = fresh;
    }
}

const char *SDL_GetEnvironmentVariable(SDL_Environment *env, const char *name)
{
    const char *result = NULL;

    if (!env) {
        return NULL;
    } else if (!name || *name == '\0') {
        return NULL;
    }

    pthread_mutex_lock(&env->lock);
    {
        SDLOP_EnvVar *var = find_var(env, name);
        if (var) {
            /* return a copy that stays valid after the env is changed
               (SDL3: SDL_GetPersistentString) */
            result = SDLOP_GetPersistentString(var_value(var));
        }
    }
    pthread_mutex_unlock(&env->lock);

    return result;
}

char **SDL_GetEnvironmentVariables(SDL_Environment *env)
{
    char **result = NULL;

    if (!env) {
        SDL_InvalidParamError("env");
        return NULL;
    }

    pthread_mutex_lock(&env->lock);
    {
        /* First pass, get the size we need for all the strings */
        size_t count = 0;
        size_t length = 0;
        for (SDLOP_EnvVar *var = env->vars; var; var = var->next) {
            length += SDL_strlen(var->name) + 1 + SDL_strlen(var_value(var)) + 1;
            count++;
        }

        /* Allocate memory for the strings */
        result = (char **)SDL_malloc((count + 1) * sizeof(*result) + length);
        if (result) {
            /* Second pass, copy the strings */
            char *string = (char *)(result + count + 1);
            size_t i = 0;
            for (SDLOP_EnvVar *var = env->vars; var; var = var->next) {
                size_t len;

                result[i++] = string;

                len = SDL_strlen(var->name);
                SDL_memcpy(string, var->name, len);
                string += len;
                *(string++) = '=';

                const char *value = var_value(var);
                len = SDL_strlen(value);
                SDL_memcpy(string, value, len);
                string += len;
                *(string++) = '\0';
            }
            result[count] = NULL;
        }
    }
    pthread_mutex_unlock(&env->lock);

    return result;
}

bool SDL_SetEnvironmentVariable(SDL_Environment *env, const char *name, const char *value, bool overwrite)
{
    bool result = false;

    if (!env) {
        return SDL_InvalidParamError("env");
    } else if (!name || *name == '\0' || SDL_strchr(name, '=') != NULL) {
        return SDL_InvalidParamError("name");
    } else if (!value) {
        return SDL_InvalidParamError("value");
    }

    pthread_mutex_lock(&env->lock);
    {
        SDLOP_EnvVar *existing = find_var(env, name);
        if (existing && !overwrite) {
            result = true; /* it already existed, and we refused to overwrite it. Call it success. */
        } else {
            size_t name_len = SDL_strlen(name);
            size_t value_len = SDL_strlen(value);
            SDLOP_EnvVar *var = (SDLOP_EnvVar *)SDL_malloc(sizeof(*var) + name_len + 1 + value_len + 1);
            if (var) {
                SDL_memcpy(var->name, name, name_len + 1);
                SDL_memcpy(var->name + name_len + 1, value, value_len + 1);
                if (existing) {
                    /* replace in place (keeps list order) */
                    var->next = existing->next;
                    /* find the link to existing and splice */
                    SDLOP_EnvVar **link = &env->vars;
                    while (*link && *link != existing) {
                        link = &(*link)->next;
                    }
                    if (*link) {
                        *link = var;
                    }
                    SDL_free(existing);
                } else {
                    var->next = env->vars;
                    env->vars = var;
                }
                result = true;
            }
        }
    }
    pthread_mutex_unlock(&env->lock);

    return result;
}

bool SDL_UnsetEnvironmentVariable(SDL_Environment *env, const char *name)
{
    bool result = false;

    if (!env) {
        return SDL_InvalidParamError("env");
    } else if (!name || *name == '\0' || SDL_strchr(name, '=') != NULL) {
        return SDL_InvalidParamError("name");
    }

    pthread_mutex_lock(&env->lock);
    {
        SDLOP_EnvVar **link = &env->vars;
        while (*link) {
            if (SDL_strcmp((*link)->name, name) == 0) {
                SDLOP_EnvVar *var = *link;
                *link = var->next;
                SDL_free(var);
                break;
            }
            link = &(*link)->next;
        }
        result = true; /* SDL3: removing a missing variable succeeds */
    }
    pthread_mutex_unlock(&env->lock);

    return result;
}

void SDL_DestroyEnvironment(SDL_Environment *env)
{
    if (!env || env == SDL_environment) {
        return;
    }

    SDLOP_EnvVar *var = env->vars;
    while (var) {
        SDLOP_EnvVar *next = var->next;
        SDL_free(var);
        var = next;
    }
    pthread_mutex_destroy(&env->lock);
    SDL_free(env);
}

/* Put a variable into the environment
   Note: Name may not contain a '=' character. (Reference: http://www.unix.com/man-page/Linux/3/setenv/) */
int SDL_setenv_unsafe(const char *name, const char *value, int overwrite)
{
    /* Input validation */
    if (!name || *name == '\0' || SDL_strchr(name, '=') != NULL || !value) {
        return -1;
    }

    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name, value, (overwrite != 0));

    return setenv(name, value, overwrite);
}

int SDL_unsetenv_unsafe(const char *name)
{
    /* Input validation */
    if (!name || *name == '\0' || SDL_strchr(name, '=') != NULL) {
        return -1;
    }

    SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), name);

    return unsetenv(name);
}

/* Retrieve a variable named "name" from the environment */
const char *SDL_getenv_unsafe(const char *name)
{
    /* Input validation */
    if (!name || *name == '\0') {
        return NULL;
    }

    return getenv(name);
}

const char *SDL_getenv(const char *name)
{
    return SDL_GetEnvironmentVariable(SDL_GetEnvironment(), name);
}
