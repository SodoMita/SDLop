/*
  SDLop - thread management (see <SDL3/SDL_thread.h>).

  Ported from SDL3 3.2.10 (src/thread/SDL_thread.c + the pthread backend),
  with SDLop simplifications:

  - validity tracking is a small mutex-protected pointer list instead of
    SDL3's object hash (same observable semantics for SDL_WaitThread /
    SDL_DetachThread / SDL_GetThreadState / SDL_GetThreadID / SDL_GetThreadName);
  - thread priority on Linux is the setpriority(nice) mapping, like SDL3 on
    a system without rtkit/dbus (SCHED_OTHER only; realtime policies fail
    cleanly with an error);
  - TLS storage rides a single pthread key (no generic fallback list).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <pthread.h>
#include <signal.h>
#include <errno.h>
#ifndef __EMSCRIPTEN__
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Thread object                                                       */
/* ------------------------------------------------------------------ */

struct SDL_Thread
{
    SDL_ThreadID threadid;  /* set by the thread itself at startup */
    pthread_t handle;
    int status;
    SDL_AtomicInt state;    /* SDL_ThreadState */
    char *name;             /* owned copy */
    size_t stacksize;       /* 0 for default, >0 for user-specified stack size */
    SDL_ThreadFunction userfunc;
    void *userdata;
    struct SDL_Thread *registry_next;
};

/* ------------------------------------------------------------------ */
/* Validity registry                                                   */
/*                                                                     */
/* SDL3 keeps a global hash of valid objects so a stale SDL_Thread* is */
/* rejected instead of dereferenced. SDLop keeps a linked list of live */
/* pointers under a mutex; lookups compare pointer values only, never  */
/* dereference.                                                        */
/* ------------------------------------------------------------------ */

static pthread_mutex_t thread_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_Thread *thread_registry; /* linked via ->registry_next */

static bool thread_is_valid(SDL_Thread *thread)
{
    bool found = false;
    pthread_mutex_lock(&thread_registry_lock);
    for (SDL_Thread *t = thread_registry; t; t = t->registry_next) {
        if (t == thread) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&thread_registry_lock);
    return found;
}

static void thread_register(SDL_Thread *thread)
{
    pthread_mutex_lock(&thread_registry_lock);
    thread->registry_next = thread_registry;
    thread_registry = thread;
    pthread_mutex_unlock(&thread_registry_lock);
}

static void thread_unregister(SDL_Thread *thread)
{
    pthread_mutex_lock(&thread_registry_lock);
    SDL_Thread **link = &thread_registry;
    while (*link) {
        if (*link == thread) {
            *link = thread->registry_next;
            break;
        }
        link = &(*link)->registry_next;
    }
    pthread_mutex_unlock(&thread_registry_lock);
}

/* ------------------------------------------------------------------ */
/* Thread-local storage (SDL3: src/thread/SDL_thread.c + pthread key)  */
/* ------------------------------------------------------------------ */

/* This is the system-independent thread local storage structure */
typedef struct SDL_TLSData
{
    int limit;
    struct
    {
        void *data;
        SDL_TLSDestructorCallback destructor;
    } array[1];
} SDL_TLSData;

/* This is how many TLS entries we allocate at once */
#define TLS_ALLOC_CHUNKSIZE 4

/* The storage is local to the thread, but the IDs are global for the process */
static SDL_AtomicInt SDL_tls_allocated;
static SDL_AtomicInt SDL_tls_id;

static pthread_key_t sdlop_tls_key;
static bool sdlop_tls_key_valid;
static pthread_once_t sdlop_tls_once = PTHREAD_ONCE_INIT;

static void sdlop_make_tls_key(void)
{
    sdlop_tls_key_valid = (pthread_key_create(&sdlop_tls_key, NULL) == 0);
}

void SDL_InitTLSData(void)
{
    pthread_once(&sdlop_tls_once, sdlop_make_tls_key);
}

static SDL_TLSData *sys_get_tls_data(void)
{
    if (!sdlop_tls_key_valid) {
        return NULL;
    }
    return (SDL_TLSData *)pthread_getspecific(sdlop_tls_key);
}

static bool sys_set_tls_data(SDL_TLSData *data)
{
    if (!sdlop_tls_key_valid) {
        return SDL_SetError("TLS unavailable (pthread_key_create failed)");
    }
    if (pthread_setspecific(sdlop_tls_key, data) != 0) {
        return SDL_SetError("pthread_setspecific() failed");
    }
    return true;
}

void *SDL_GetTLS(SDL_TLSID *id)
{
    if (id == NULL) {
        SDL_InvalidParamError("id");
        return NULL;
    }

    int storage_index = SDL_GetAtomicInt(id) - 1;
    SDL_TLSData *storage = sys_get_tls_data();
    if (!storage || storage_index < 0 || storage_index >= storage->limit) {
        return NULL;
    }
    return storage->array[storage_index].data;
}

bool SDL_SetTLS(SDL_TLSID *id, const void *value, SDL_TLSDestructorCallback destructor)
{
    if (id == NULL) {
        return SDL_InvalidParamError("id");
    }

    /* Make sure TLS is initialized.
     * There's a race condition here if you are calling this from non-SDL threads
     * and haven't called SDL_Init() on your main thread, but such is life.
     * (SDL3 notes the same race.) */
    SDL_InitTLSData();

    /* Get the storage index associated with the ID in a thread-safe way */
    int storage_index = SDL_GetAtomicInt(id) - 1;
    if (storage_index < 0) {
        int new_id = (SDL_AtomicIncRef(&SDL_tls_id) + 1);

        SDL_CompareAndSwapAtomicInt(id, 0, new_id);

        /* If there was a race condition we'll have wasted an ID, but every thread
         * will have the same storage index for this id.
         */
        storage_index = SDL_GetAtomicInt(id) - 1;
    }

    /* Get the storage for the current thread */
    SDL_TLSData *storage = sys_get_tls_data();
    if (!storage || storage_index >= storage->limit) {
        unsigned int i, oldlimit, newlimit;
        SDL_TLSData *new_storage;

        oldlimit = storage ? (unsigned int)storage->limit : 0;
        newlimit = (unsigned int)(storage_index + TLS_ALLOC_CHUNKSIZE);
        new_storage = (SDL_TLSData *)SDL_realloc(storage, sizeof(*storage) + (newlimit - 1) * sizeof(storage->array[0]));
        if (!new_storage) {
            return false;
        }
        storage = new_storage;
        storage->limit = (int)newlimit;
        for (i = oldlimit; i < newlimit; ++i) {
            storage->array[i].data = NULL;
            storage->array[i].destructor = NULL;
        }
        if (!sys_set_tls_data(storage)) {
            SDL_free(storage);
            return false;
        }
        SDL_AtomicIncRef(&SDL_tls_allocated);
    }

    storage->array[storage_index].data = (void *)value;
    storage->array[storage_index].destructor = destructor;
    return true;
}

void SDL_CleanupTLS(void)
{
    /* Cleanup the storage for the current thread */
    SDL_TLSData *storage = sys_get_tls_data();
    if (storage) {
        int i;
        for (i = 0; i < storage->limit; ++i) {
            if (storage->array[i].destructor) {
                storage->array[i].destructor(storage->array[i].data);
            }
        }
        sys_set_tls_data(NULL);
        SDL_free(storage);
        (void)SDL_AtomicDecRef(&SDL_tls_allocated);
    }
}

/* internal: SDL_Quit() hook (SDL3: SDL_QuitTLSData) */
void SDLOP_ThreadsQuit(void)
{
    SDL_CleanupTLS();

    if (SDL_GetAtomicInt(&SDL_tls_allocated) == 0) {
        if (sdlop_tls_key_valid) {
            pthread_key_delete(sdlop_tls_key);
            sdlop_tls_key_valid = false;
            /* allow re-init after SDL_Quit */
            pthread_once_t fresh = PTHREAD_ONCE_INIT;
            sdlop_tls_once = fresh;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Persistent strings (per-thread intern pool, SDL3: SDL_GetPersistentString) */
/* ------------------------------------------------------------------ */

typedef struct SDLOP_PStringNode
{
    struct SDLOP_PStringNode *next;
    char str[];
} SDLOP_PStringNode;

static SDL_TLSID pstring_tls;

static void SDLCALL SDLOP_FreePersistentStrings(void *value)
{
    SDLOP_PStringNode *node = (SDLOP_PStringNode *)value;
    while (node) {
        SDLOP_PStringNode *next = node->next;
        SDL_free(node);
        node = next;
    }
}

/* internal, SDL3-equivalent SDL_GetPersistentString */
const char *SDLOP_GetPersistentString(const char *string)
{
    if (!string) {
        return NULL;
    }
    if (!*string) {
        return "";
    }

    SDLOP_PStringNode *list = (SDLOP_PStringNode *)SDL_GetTLS(&pstring_tls);
    for (SDLOP_PStringNode *node = list; node; node = node->next) {
        if (SDL_strcmp(node->str, string) == 0) {
            return node->str;
        }
    }

    size_t len = SDL_strlen(string) + 1;
    SDLOP_PStringNode *node = (SDLOP_PStringNode *)SDL_malloc(sizeof(*node) + len);
    if (!node) {
        return NULL;
    }
    SDL_memcpy(node->str, string, len);
    node->next = list;
    SDL_SetTLS(&pstring_tls, node, SDLOP_FreePersistentStrings);
    return node->str;
}

/* ------------------------------------------------------------------ */
/* Thread startup / teardown                                           */
/* ------------------------------------------------------------------ */

/* List of signals to mask in the subthreads (SDL3 does the same) */
static const int sig_list[] = {
    SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGALRM, SIGTERM, SIGCHLD, SIGWINCH,
    SIGVTALRM, SIGPROF, 0
};

static void SDLOP_SetupThread(const char *name)
{
    int i;
    sigset_t mask;

#ifndef __EMSCRIPTEN__
    if (name) {
        if (pthread_setname_np(pthread_self(), name) == ERANGE) {
            char namebuf[16]; /* Limited to 16 chars */
            SDL_strlcpy(namebuf, name, sizeof(namebuf));
            (void)pthread_setname_np(pthread_self(), namebuf);
        }
    }
#else
    (void)name;
#endif

    /* Mask asynchronous signals for this thread */
    sigemptyset(&mask);
    for (i = 0; sig_list[i]; ++i) {
        sigaddset(&mask, sig_list[i]);
    }
    pthread_sigmask(SIG_BLOCK, &mask, 0);

#ifdef PTHREAD_CANCEL_ASYNCHRONOUS
    /* Allow ourselves to be asynchronously cancelled */
    {
        int oldstate;
        pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldstate);
    }
#endif
}

/* SDL3: SDL_RunThread() */
static void SDLOP_RunThread(SDL_Thread *thread)
{
    void *userdata = thread->userdata;
    int (SDLCALL *userfunc)(void *) = thread->userfunc;

    int *statusloc = &thread->status;

    /* Perform any system-dependent setup - this function may not fail */
    SDLOP_SetupThread(thread->name);

    /* Get the thread id */
    thread->threadid = SDL_GetCurrentThreadID();

    /* Run the function */
    *statusloc = userfunc(userdata);

    /* Clean up thread-local storage */
    SDL_CleanupTLS();

    /* Mark us as ready to be joined (or detached) */
    if (!SDL_CompareAndSwapAtomicInt(&thread->state, SDL_THREAD_ALIVE, SDL_THREAD_COMPLETE)) {
        /* Clean up if something already detached us. Read the state
           directly: the object was invalidated by SDL_DetachThread(), so a
           validity-gated getter could not see DETACHED here. */
        if (SDL_GetAtomicInt(&thread->state) == SDL_THREAD_DETACHED) {
            SDL_free(thread->name); /* Can't free later, we've already cleaned up TLS */
            SDL_free(thread);
        }
    }
}

static void *SDLOP_ThreadLauncher(void *data)
{
    SDLOP_RunThread((SDL_Thread *)data);
    return NULL;
}

SDL_Thread *SDL_CreateThreadWithPropertiesRuntime(SDL_PropertiesID props,
                                                  SDL_FunctionPointer pfnBeginThread,
                                                  SDL_FunctionPointer pfnEndThread)
{
    /* Only Windows/GDK use the C runtime entry points in SDL3. */
    if (pfnBeginThread || pfnEndThread) {
        SDL_SetError("_beginthreadex/_endthreadex not supported on this platform");
        return NULL;
    }

    SDL_ThreadFunction fn = (SDL_ThreadFunction)SDL_GetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, NULL);
    const char *name = SDL_GetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, NULL);
    const size_t stacksize = (size_t)SDL_GetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 0);
    void *userdata = SDL_GetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, NULL);

    if (!fn) {
        SDL_SetError("Thread entry function is NULL");
        return NULL;
    }

    SDL_Thread *thread = (SDL_Thread *)SDL_calloc(1, sizeof(*thread));
    if (!thread) {
        return NULL;
    }
    thread->status = -1;
    SDL_SetAtomicInt(&thread->state, SDL_THREAD_ALIVE);

    /* Set up the arguments for the thread */
    if (name) {
        thread->name = SDL_strdup(name);
        if (!thread->name) {
            SDL_free(thread);
            return NULL;
        }
    }

    thread->userfunc = fn;
    thread->userdata = userdata;
    thread->stacksize = stacksize;

    thread_register(thread);

    /* Create the thread and go! */
    pthread_attr_t attr;
    bool attr_ok = (pthread_attr_init(&attr) == 0);
    if (attr_ok) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        /* Set caller-requested stack size. Otherwise: use the system default. */
        if (thread->stacksize) {
            pthread_attr_setstacksize(&attr, thread->stacksize);
        }
    }
    if (!attr_ok || pthread_create(&thread->handle, &attr, SDLOP_ThreadLauncher, thread) != 0) {
        /* Oops, failed. Gotta free everything */
        thread_unregister(thread);
        SDL_free(thread->name);
        SDL_free(thread);
        if (attr_ok) {
            pthread_attr_destroy(&attr);
            SDL_SetError("Not enough resources to create thread");
        } else {
            SDL_SetError("Couldn't initialize pthread attributes");
        }
        return NULL;
    }
    pthread_attr_destroy(&attr);

    /* Everything is running now */
    return thread;
}

SDL_Thread *SDL_CreateThreadRuntime(SDL_ThreadFunction fn,
                                    const char *name, void *userdata,
                                    SDL_FunctionPointer pfnBeginThread,
                                    SDL_FunctionPointer pfnEndThread)
{
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)fn);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, name);
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, userdata);
    SDL_Thread *thread = SDL_CreateThreadWithPropertiesRuntime(props, pfnBeginThread, pfnEndThread);
    SDL_DestroyProperties(props);
    return thread;
}

SDL_ThreadID SDL_GetCurrentThreadID(void)
{
    return (SDL_ThreadID)pthread_self();
}

SDL_ThreadID SDL_GetThreadID(SDL_Thread *thread)
{
    SDL_ThreadID id = 0;

    if (thread) {
        if (thread_is_valid(thread)) {
            id = thread->threadid;
        }
    } else {
        id = SDL_GetCurrentThreadID();
    }
    return id;
}

const char *SDL_GetThreadName(SDL_Thread *thread)
{
    if (thread_is_valid(thread)) {
        return SDLOP_GetPersistentString(thread->name);
    } else {
        return NULL;
    }
}

/* SDL3: pthread backend + Linux threadprio (no-dbus path) */
bool SDL_SetCurrentThreadPriority(SDL_ThreadPriority priority)
{
#ifdef __EMSCRIPTEN__
    /* No schedulers to program on the web; accept and ignore. */
    (void)priority;
    return true;
#else
    struct sched_param sched;
    int policy;
    int pri_policy;
    pthread_t thread = pthread_self();
    const char *policyhint = SDL_getenv_unsafe(SDL_HINT_THREAD_PRIORITY_POLICY);
    const char *rtcritical = SDL_getenv_unsafe(SDL_HINT_THREAD_FORCE_REALTIME_TIME_CRITICAL);
    const bool timecritical_realtime_hint =
        rtcritical && (SDL_strcmp(rtcritical, "1") == 0 ||
                       SDL_strcasecmp(rtcritical, "true") == 0 ||
                       SDL_strcasecmp(rtcritical, "yes") == 0);

    if (pthread_getschedparam(thread, &policy, &sched) != 0) {
        return SDL_SetError("pthread_getschedparam() failed");
    }

    /* Higher priority levels may require changing the pthread scheduler policy
     * for the thread. SDL will make such changes by default but there is
     * also a hint allowing that behavior to be overridden. */
    switch (priority) {
    case SDL_THREAD_PRIORITY_LOW:
    case SDL_THREAD_PRIORITY_NORMAL:
        pri_policy = SCHED_OTHER;
        break;
    case SDL_THREAD_PRIORITY_HIGH:
    case SDL_THREAD_PRIORITY_TIME_CRITICAL:
        /* Linux (unlike Apple) stays on SCHED_OTHER for high priorities */
        pri_policy = SCHED_OTHER;
        break;
    default:
        pri_policy = policy;
        break;
    }

    if (timecritical_realtime_hint && priority == SDL_THREAD_PRIORITY_TIME_CRITICAL) {
        pri_policy = SCHED_RR;
    }

    if (policyhint) {
        if (SDL_strcmp(policyhint, "current") == 0) {
            /* Leave current thread scheduler policy unchanged */
        } else if (SDL_strcmp(policyhint, "other") == 0) {
            policy = SCHED_OTHER;
        } else if (SDL_strcmp(policyhint, "rr") == 0) {
            policy = SCHED_RR;
        } else if (SDL_strcmp(policyhint, "fifo") == 0) {
            policy = SCHED_FIFO;
        } else {
            policy = pri_policy;
        }
    } else {
        policy = pri_policy;
    }

    if (policy == SCHED_RR || policy == SCHED_FIFO) {
        /* SDL3 reaches realtime policies through rtkit over dbus; SDLop is
           lean and doesn't talk to dbus, so - exactly like SDL3 on a system
           without rtkit - this fails cleanly. */
        return SDL_SetError("setpriority() failed (realtime policy needs rtkit or root)");
    }

    int nice_level;
    switch (priority) {
    case SDL_THREAD_PRIORITY_LOW:
        nice_level = 19;
        break;
    case SDL_THREAD_PRIORITY_HIGH:
        nice_level = -10;
        break;
    case SDL_THREAD_PRIORITY_TIME_CRITICAL:
        nice_level = -20;
        break;
    default:
        nice_level = 0;
        break;
    }

    const pid_t linuxTid = (pid_t)syscall(SYS_gettid);
    if (setpriority(PRIO_PROCESS, (id_t)linuxTid, nice_level) == 0) {
        return true;
    }

    return SDL_SetError("setpriority() failed");
#endif
}

void SDL_WaitThread(SDL_Thread *thread, int *status)
{
    if (!thread_is_valid(thread)) {
        if (status) {
            *status = -1;
        }
        return;
    }

    pthread_join(thread->handle, 0);
    if (status) {
        *status = thread->status;
    }
    thread_unregister(thread);
    SDL_free(thread->name);
    SDL_free(thread);
}

SDL_ThreadState SDL_GetThreadState(SDL_Thread *thread)
{
    if (!thread_is_valid(thread)) {
        return SDL_THREAD_UNKNOWN;
    }

    return (SDL_ThreadState)SDL_GetAtomicInt(&thread->state);
}

void SDL_DetachThread(SDL_Thread *thread)
{
    if (!thread_is_valid(thread)) {
        return;
    }

    /* The thread may vanish at any time, it's no longer valid */
    thread_unregister(thread);

    /* Grab dibs if the state is alive+joinable. */
    if (SDL_CompareAndSwapAtomicInt(&thread->state, SDL_THREAD_ALIVE, SDL_THREAD_DETACHED)) {
        pthread_detach(thread->handle);
    } else {
        /* all other states are pretty final, see where we landed. */
        SDL_ThreadState thread_state = (SDL_ThreadState)SDL_GetAtomicInt(&thread->state);
        if (thread_state == SDL_THREAD_DETACHED) {
            return; /* already detached (you shouldn't call this twice!) */
        } else if (thread_state == SDL_THREAD_COMPLETE) {
            /* already done, clean it up (thread->status already final). */
            pthread_join(thread->handle, 0);
            SDL_free(thread->name);
            SDL_free(thread);
        }
    }
}
