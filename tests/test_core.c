/*
  SDLop test: SDL3 core services, checked against SDL 3.4.20 semantics.

    - hints: priorities, env override, callbacks, SDL_ResetHint(s)
    - properties: typed get/set conversions, cleanup, copy, enumerate
    - log: per-category priorities, SDL_LOGGING parsing, output function,
      prefixes, trailing-newline chop
    - timers: periodic, rescheduling, cancel from callback, SDL_RemoveTimer
    - events: filter + watchers order, SDL_PeepEvents add/peek/get,
      SDL_SetEventEnabled flush/drop, cross-thread SDL_PushEvent,
      SDL_RunOnMainThread, SDL_GetEventDescription
    - clipboard: in-process data/text/primary selection + update events
    - misc: version, platform, app metadata, SDL_SetErrorV

  Runs on the dummy video driver, no display required.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                    \
            fprintf(stderr, "  [%s]\n", #cond);              \
            failures++;                                      \
        }                                                    \
    } while (0)

/* ------------------------------------------------------------------ */

static int hint_calls;
static char hint_last[64];

static void SDLCALL hint_cb(void *userdata, const char *name, const char *oldValue, const char *newValue)
{
    (void)userdata;
    (void)name;
    (void)oldValue;
    hint_calls++;
    snprintf(hint_last, sizeof(hint_last), "%s", newValue ? newValue : "(null)");
}

static void test_hints(void)
{
    const char *H = "SDLOP_TEST_HINT";
    CHECK(SDL_GetHint(H) == NULL, "unset hint");
    CHECK(SDL_SetHint(H, "1"), "set");
    CHECK(strcmp(SDL_GetHint(H), "1") == 0, "get");
    CHECK(SDL_GetHintBoolean(H, false) == true, "bool");
    CHECK(SDL_SetHintWithPriority(H, "0", SDL_HINT_OVERRIDE), "override");
    CHECK(!SDL_SetHintWithPriority(H, "2", SDL_HINT_NORMAL), "lower priority rejected");
    CHECK(strcmp(SDL_GetHint(H), "0") == 0, "override kept");

    hint_calls = 0;
    CHECK(SDL_AddHintCallback(H, hint_cb, NULL), "add cb");
    CHECK(hint_calls == 1 && strcmp(hint_last, "0") == 0, "callback fires on add: %d %s", hint_calls, hint_last);
    SDL_SetHintWithPriority(H, "0", SDL_HINT_OVERRIDE);
    CHECK(hint_calls == 1, "no callback when unchanged");
    SDL_SetHintWithPriority(H, "abc", SDL_HINT_OVERRIDE);
    CHECK(hint_calls == 2 && strcmp(hint_last, "abc") == 0, "callback on change");
    CHECK(SDL_ResetHint(H), "reset");
    CHECK(SDL_GetHint(H) == NULL, "reset -> NULL");
    CHECK(hint_calls == 3 && strcmp(hint_last, "(null)") == 0, "callback on reset");
    SDL_RemoveHintCallback(H, hint_cb, NULL);
    SDL_SetHint(H, "x");
    CHECK(hint_calls == 3, "removed callback silent");

    /* environment wins over normal priority */
    SDL_setenv_unsafe("SDLOP_TEST_ENV_HINT", "env", 1); /* libc setenv is invisible to SDL3 after first use */
    CHECK(strcmp(SDL_GetHint("SDLOP_TEST_ENV_HINT"), "env") == 0, "env visible");
    CHECK(!SDL_SetHint("SDLOP_TEST_ENV_HINT", "app"), "env beats normal");
    CHECK(strcmp(SDL_GetHint("SDLOP_TEST_ENV_HINT"), "env") == 0, "env kept");
    CHECK(SDL_SetHintWithPriority("SDLOP_TEST_ENV_HINT", "app", SDL_HINT_OVERRIDE), "override beats env");
    CHECK(strcmp(SDL_GetHint("SDLOP_TEST_ENV_HINT"), "app") == 0, "override value");
    SDL_ResetHints();
    CHECK(strcmp(SDL_GetHint("SDLOP_TEST_ENV_HINT"), "env") == 0, "reset back to env");
    SDL_unsetenv_unsafe("SDLOP_TEST_ENV_HINT");
}

/* ------------------------------------------------------------------ */

static int cleanup_calls;
static void SDLCALL prop_cleanup(void *userdata, void *value)
{
    (void)userdata;
    (void)value;
    cleanup_calls++;
}

static int enum_count;
static void SDLCALL enum_cb(void *userdata, SDL_PropertiesID props, const char *name)
{
    (void)userdata;
    (void)props;
    (void)name;
    enum_count++;
}

static void test_properties(void)
{
    SDL_PropertiesID p = SDL_CreateProperties();
    CHECK(p != 0, "create");
    CHECK(SDL_SetNumberProperty(p, "n", 42), "num");
    CHECK(SDL_SetFloatProperty(p, "f", 1.5f), "float");
    CHECK(SDL_SetBooleanProperty(p, "b", true), "bool");
    CHECK(SDL_SetStringProperty(p, "s", "hello"), "str");

    CHECK(SDL_GetNumberProperty(p, "n", 0) == 42, "num get");
    CHECK(SDL_GetFloatProperty(p, "n", 0) == 42.0f, "num as float");
    CHECK(strcmp(SDL_GetStringProperty(p, "n", ""), "42") == 0, "num as string");
    CHECK(SDL_GetNumberProperty(p, "f", 0) == 2, "float as num (rounded)");
    CHECK(SDL_GetBooleanProperty(p, "n", false) == true, "num as bool");
    CHECK(SDL_GetPropertyType(p, "s") == SDL_PROPERTY_TYPE_STRING, "type");
    CHECK(SDL_GetPropertyType(p, "zz") == SDL_PROPERTY_TYPE_INVALID, "missing type");
    CHECK(SDL_HasProperty(p, "b"), "has");

    CHECK(SDL_SetPointerPropertyWithCleanup(p, "ptr", &cleanup_calls, prop_cleanup, NULL), "ptr+cleanup");
    cleanup_calls = 0;
    SDL_SetPointerProperty(p, "ptr", NULL); /* NULL clears */
    CHECK(cleanup_calls == 1, "cleanup on clear");
    CHECK(!SDL_HasProperty(p, "ptr"), "cleared");

    SDL_SetStringProperty(p, "s", NULL);
    CHECK(!SDL_HasProperty(p, "s"), "NULL string clears");

    SDL_PropertiesID q = SDL_CreateProperties();
    CHECK(SDL_CopyProperties(p, q), "copy");
    CHECK(SDL_GetNumberProperty(q, "n", 0) == 42, "copied num");
    enum_count = 0;
    SDL_EnumerateProperties(q, enum_cb, NULL);
    CHECK(enum_count == 3, "enumerate %d", enum_count);

    SDL_SetPointerPropertyWithCleanup(q, "ptr2", &cleanup_calls, prop_cleanup, NULL);
    cleanup_calls = 0;
    SDL_DestroyProperties(q);
    CHECK(cleanup_calls == 1, "cleanup on destroy");
    SDL_DestroyProperties(p);
    CHECK(SDL_GetGlobalProperties() != 0, "global props");
}

/* ------------------------------------------------------------------ */

static char log_buf[512];
static int log_calls;
static SDL_LogPriority log_prio;

static void SDLCALL log_cb(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    (void)userdata;
    (void)category;
    log_calls++;
    log_prio = priority;
    snprintf(log_buf, sizeof(log_buf), "%s", message);
}

static void test_log(void)
{
    SDL_LogOutputFunction prev;
    void *prev_ud;
    SDL_GetLogOutputFunction(&prev, &prev_ud);
    CHECK(prev == SDL_GetDefaultLogOutputFunction(), "default output");
    SDL_SetLogOutputFunction(log_cb, NULL);

    SDL_ResetLogPriorities();
    CHECK(SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION) == SDL_LOG_PRIORITY_INFO ||
              SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION) == SDL_LOG_PRIORITY_DEBUG,
          "app default");
    CHECK(SDL_GetLogPriority(SDL_LOG_CATEGORY_TEST) == SDL_LOG_PRIORITY_VERBOSE, "test default");
    CHECK(SDL_GetLogPriority(SDL_LOG_CATEGORY_ASSERT) == SDL_LOG_PRIORITY_WARN, "assert default");

    log_calls = 0;
    SDL_Log("hello %d\n", 7);
    CHECK(log_calls == 1 && strcmp(log_buf, "hello 7") == 0, "newline chopped: '%s'", log_buf);
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "filtered");
    CHECK(log_calls == 1, "video debug filtered");

    /* long message > stack buffer */
    char big[400];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    SDL_Log("%s", big);
    CHECK(strlen(log_buf) == sizeof(big) - 1, "long message %zu", strlen(log_buf));

    SDL_SetHint(SDL_HINT_LOGGING, "video=debug,*=warn");
    CHECK(SDL_GetLogPriority(SDL_LOG_CATEGORY_VIDEO) == SDL_LOG_PRIORITY_DEBUG, "hint video");
    CHECK(SDL_GetLogPriority(SDL_LOG_CATEGORY_AUDIO) == SDL_LOG_PRIORITY_WARN, "hint default *");
    CHECK(SDL_GetLogPriority(SDL_LOG_CATEGORY_CUSTOM + 5) == SDL_LOG_PRIORITY_WARN, "custom gets *");
    SDL_SetHint(SDL_HINT_LOGGING, "quiet");
    CHECK(SDL_GetLogPriority(SDL_LOG_CATEGORY_ERROR) == SDL_LOG_PRIORITY_COUNT, "quiet");
    SDL_ResetHint(SDL_HINT_LOGGING);

    SDL_SetLogPriority(SDL_LOG_CATEGORY_CUSTOM + 1, SDL_LOG_PRIORITY_TRACE);
    log_calls = 0;
    SDL_LogTrace(SDL_LOG_CATEGORY_CUSTOM + 1, "t");
    CHECK(log_calls == 1 && log_prio == SDL_LOG_PRIORITY_TRACE, "custom trace");
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_CRITICAL);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "no");
    CHECK(log_calls == 1, "all critical");
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "yes");
    CHECK(log_calls == 2, "critical passes");

    CHECK(!SDL_SetLogPriorityPrefix(SDL_LOG_PRIORITY_INVALID, "x"), "invalid prefix prio");
    CHECK(SDL_SetLogPriorityPrefix(SDL_LOG_PRIORITY_WARN, "W: "), "prefix");
    SDL_ResetLogPriorities();
    SDL_SetLogOutputFunction(prev, prev_ud);
}

/* ------------------------------------------------------------------ */

static SDL_AtomicInt timer_ticks;
static SDL_AtomicInt timer_once;

static Uint32 SDLCALL periodic(void *userdata, SDL_TimerID id, Uint32 interval)
{
    (void)userdata;
    (void)id;
    SDL_AddAtomicInt(&timer_ticks, 1);
    return interval;
}

static Uint32 SDLCALL one_shot(void *userdata, SDL_TimerID id, Uint32 interval)
{
    (void)userdata;
    (void)id;
    (void)interval;
    SDL_AddAtomicInt(&timer_once, 1);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_USER + 7;
    SDL_PushEvent(&e); /* from the timer thread */
    return 0;
}

static Uint64 SDLCALL ns_timer(void *userdata, SDL_TimerID id, Uint64 interval)
{
    (void)id;
    int *n = (int *)userdata;
    return (++*n < 3) ? interval : 0;
}

static void test_timers(void)
{
    SDL_SetAtomicInt(&timer_ticks, 0);
    SDL_SetAtomicInt(&timer_once, 0);
    SDL_TimerID a = SDL_AddTimer(5, periodic, NULL);
    SDL_TimerID b = SDL_AddTimer(10, one_shot, NULL);
    int nsn = 0;
    SDL_TimerID c = SDL_AddTimerNS(2000000, ns_timer, &nsn);
    CHECK(a && b && c && a != b, "ids");
    SDL_Delay(80);
    CHECK(SDL_GetAtomicInt(&timer_ticks) >= 5, "periodic ticks %d", SDL_GetAtomicInt(&timer_ticks));
    CHECK(SDL_GetAtomicInt(&timer_once) == 1, "one shot");
    CHECK(nsn == 3, "ns timer stopped after 3: %d", nsn);
    CHECK(SDL_RemoveTimer(a), "remove");
    int t = SDL_GetAtomicInt(&timer_ticks);
    SDL_Delay(30);
    CHECK(SDL_GetAtomicInt(&timer_ticks) <= t + 1, "no ticks after remove");
    CHECK(!SDL_RemoveTimer(b), "finished timer not found");

    SDL_Event e;
    bool got = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_USER + 7) {
            got = true;
            CHECK(e.common.timestamp != 0, "timestamp filled");
        }
    }
    CHECK(got, "event pushed from timer thread delivered");

    Uint64 t0 = SDL_GetTicksNS();
    SDL_DelayPrecise(3000000);
    Uint64 dt = SDL_GetTicksNS() - t0;
    CHECK(dt >= 3000000 && dt < 20000000, "DelayPrecise %llu", (unsigned long long)dt);
}

/* ------------------------------------------------------------------ */

static int order[8];
static int norder;

static bool SDLCALL filter_cb(void *userdata, SDL_Event *event)
{
    (void)userdata;
    order[norder++ & 7] = 1;
    return event->type != SDL_EVENT_USER + 1; /* drop USER+1 */
}

static bool SDLCALL watch_cb(void *userdata, SDL_Event *event)
{
    (void)event;
    order[norder++ & 7] = (int)(intptr_t)userdata;
    return true;
}

static int main_cb_ran;
static void SDLCALL main_cb(void *userdata)
{
    (void)userdata;
    main_cb_ran = SDL_IsMainThread() ? 1 : -1;
}

static void *thread_main(void *arg)
{
    (void)arg;
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_USER + 3;
    SDL_PushEvent(&e);
    SDL_RunOnMainThread(main_cb, NULL, true);
    return NULL;
}

static void test_events(void)
{
    SDL_Event e, buf[8];
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

    /* filter then watchers, in order */
    SDL_SetEventFilter(filter_cb, NULL);
    SDL_AddEventWatch(watch_cb, (void *)2);
    SDL_AddEventWatch(watch_cb, (void *)3);
    norder = 0;
    SDL_zero(e);
    e.type = SDL_EVENT_USER;
    CHECK(SDL_PushEvent(&e), "push");
    CHECK(norder == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3, "filter->watch order %d", norder);
    norder = 0;
    e.type = SDL_EVENT_USER + 1;
    CHECK(!SDL_PushEvent(&e), "filtered push returns false");
    CHECK(norder == 1, "watchers skipped for filtered");
    SDL_EventFilter f;
    void *ud;
    CHECK(SDL_GetEventFilter(&f, &ud) && f == filter_cb, "get filter");
    SDL_RemoveEventWatch(watch_cb, (void *)2);
    SDL_RemoveEventWatch(watch_cb, (void *)3);
    SDL_SetEventFilter(NULL, NULL);
    CHECK(!SDL_GetEventFilter(&f, &ud), "no filter");

    /* PeepEvents */
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    for (int i = 0; i < 4; i++) {
        SDL_zero(buf[i]);
        buf[i].type = SDL_EVENT_USER + (Uint32)i;
    }
    CHECK(SDL_PeepEvents(buf, 4, SDL_ADDEVENT, 0, 0) == 4, "add 4");
    CHECK(SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_EVENT_USER, SDL_EVENT_USER + 1) == 2, "count range");
    CHECK(SDL_PeepEvents(buf, 1, SDL_GETEVENT, SDL_EVENT_USER + 2, SDL_EVENT_USER + 2) == 1 &&
              buf[0].type == SDL_EVENT_USER + 2,
          "get from the middle");
    CHECK(SDL_PeepEvents(buf, 8, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 3, "3 left");
    CHECK(buf[0].type == SDL_EVENT_USER && buf[1].type == SDL_EVENT_USER + 1 && buf[2].type == SDL_EVENT_USER + 3,
          "order kept");
    CHECK(SDL_HasEvent(SDL_EVENT_USER + 3) && !SDL_HasEvent(SDL_EVENT_USER + 2), "HasEvent");

    /* enable/disable */
    CHECK(SDL_EventEnabled(SDL_EVENT_USER), "enabled by default");
    SDL_SetEventEnabled(SDL_EVENT_USER, false);
    CHECK(!SDL_EventEnabled(SDL_EVENT_USER), "disabled");
    CHECK(!SDL_HasEvent(SDL_EVENT_USER), "disable flushes");
    SDL_SetEventEnabled(SDL_EVENT_USER, true);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

    /* cross-thread push + RunOnMainThread */
    pthread_t th;
    main_cb_ran = 0;
    CHECK(!SDL_IsMainThread() == false, "this is the main thread");
    pthread_create(&th, NULL, thread_main, NULL);
    bool got = false;
    for (int i = 0; i < 200 && (!got || !main_cb_ran); i++) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_USER + 3) {
                got = true;
            }
        }
        SDL_Delay(5);
    }
    pthread_join(th, NULL);
    CHECK(got, "threaded push");
    CHECK(main_cb_ran == 1, "RunOnMainThread on main thread: %d", main_cb_ran);

    Uint32 base = SDL_RegisterEvents(3);
    CHECK(base >= SDL_EVENT_USER, "register");

    char desc[256];
    SDL_zero(e);
    e.type = SDL_EVENT_QUIT;
    CHECK(SDL_GetEventDescription(&e, desc, sizeof(desc)) > 0 && strstr(desc, "SDL_EVENT_QUIT"), "desc '%s'", desc);
}

/* ------------------------------------------------------------------ */

static const char *const my_types[] = { "text/x-test", "application/x-blob" };
static const void *SDLCALL clip_cb(void *userdata, const char *mime_type, size_t *size)
{
    (void)userdata;
    if (strcmp(mime_type, "application/x-blob") == 0) {
        *size = 3;
        return "\x01\x02\x03";
    }
    *size = 4;
    return "test";
}

static void test_clipboard(void)
{
    SDL_Event e;
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

    CHECK(!SDL_HasClipboardText(), "empty");
    CHECK(SDL_SetClipboardText("hello"), "set text");
    CHECK(SDL_HasClipboardText(), "has text");
    char *t = SDL_GetClipboardText();
    CHECK(t && strcmp(t, "hello") == 0, "get text");
    SDL_free(t);

    CHECK(SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_CLIPBOARD_UPDATE, SDL_EVENT_CLIPBOARD_UPDATE) == 1, "update event");
    /* the text mime types are the driver's (stock: 1 generic, 5 on X11 and
     * Wayland); text/plain;charset=utf-8 is always among them */
    bool has_utf8_text = false;
    for (int i = 0; i < e.clipboard.num_mime_types; i++) {
        has_utf8_text = has_utf8_text || strcmp(e.clipboard.mime_types[i], "text/plain;charset=utf-8") == 0;
    }
    CHECK(e.clipboard.owner && e.clipboard.num_mime_types >= 1 && has_utf8_text, "update event content");

    CHECK(SDL_SetClipboardData(clip_cb, NULL, NULL, my_types, 2), "set data");
    size_t n = 0;
    char **types = SDL_GetClipboardMimeTypes(&n);
    CHECK(types && n == 2 && strcmp(types[1], "application/x-blob") == 0 && types[2] == NULL, "mime types");
    SDL_free(types);
    size_t sz = 0;
    Uint8 *blob = (Uint8 *)SDL_GetClipboardData("application/x-blob", &sz);
    CHECK(blob && sz == 3 && blob[2] == 3, "blob");
    SDL_free(blob);
    CHECK(!SDL_HasClipboardData("image/png"), "not offered");
    CHECK(!SDL_SetClipboardData(clip_cb, NULL, NULL, NULL, 0), "invalid params");
    CHECK(SDL_ClearClipboardData(), "clear");
    CHECK(!SDL_HasClipboardData("text/x-test"), "cleared");

    CHECK(SDL_SetPrimarySelectionText("sel"), "primary");
    CHECK(SDL_HasPrimarySelectionText(), "has primary");
    t = SDL_GetPrimarySelectionText();
    CHECK(t && strcmp(t, "sel") == 0, "get primary");
    SDL_free(t);

    while (SDL_PollEvent(&e)) {
    }
    SDL_PumpEvents(); /* frees the polled events' temporary memory */
}

/* ------------------------------------------------------------------ */

static SDL_TLSID tls_id;
static SDL_AtomicInt tls_dtor_calls;
static SDL_Mutex *mtx;
static SDL_Condition *cnd;
static SDL_Semaphore *sem;
static int shared_counter;

static void SDLCALL tls_dtor(void *value)
{
    (void)value;
    SDL_AddAtomicInt(&tls_dtor_calls, 1);
}

static int SDLCALL worker(void *data)
{
    int id = (int)(intptr_t)data;
    SDL_SetTLS(&tls_id, (void *)(intptr_t)(id + 100), tls_dtor);
    for (int i = 0; i < 10000; i++) {
        SDL_LockMutex(mtx);
        SDL_LockMutex(mtx); /* recursive */
        shared_counter++;
        SDL_UnlockMutex(mtx);
        SDL_UnlockMutex(mtx);
    }
    if ((intptr_t)SDL_GetTLS(&tls_id) != id + 100) {
        return -1;
    }
    SDL_SignalSemaphore(sem);
    return id;
}

static int SDLCALL cond_waiter(void *data)
{
    (void)data;
    SDL_LockMutex(mtx);
    while (shared_counter != -1) {
        SDL_WaitCondition(cnd, mtx);
    }
    SDL_UnlockMutex(mtx);
    return 7;
}

static void test_threads(void)
{
    SDL_AtomicInt a;
    SDL_SetAtomicInt(&a, 5);
    CHECK(SDL_AddAtomicInt(&a, 3) == 5 && SDL_GetAtomicInt(&a) == 8, "atomic add");
    CHECK(SDL_CompareAndSwapAtomicInt(&a, 8, 1) && !SDL_CompareAndSwapAtomicInt(&a, 8, 2), "cas");
    void *p = NULL;
    CHECK(SDL_CompareAndSwapAtomicPointer(&p, NULL, &a) && SDL_GetAtomicPointer(&p) == &a, "cas ptr");
    SDL_SpinLock sl = 0;
    CHECK(SDL_TryLockSpinlock(&sl) && !SDL_TryLockSpinlock(&sl), "spinlock");
    SDL_UnlockSpinlock(&sl);

    mtx = SDL_CreateMutex();
    cnd = SDL_CreateCondition();
    sem = SDL_CreateSemaphore(0);
    CHECK(mtx && cnd && sem, "create sync objects");
    CHECK(SDL_TryLockMutex(NULL), "NULL mutex trylock");
    CHECK(!SDL_WaitSemaphoreTimeout(sem, 10), "sem timeout");
    SDL_LockMutex(mtx);
    CHECK(!SDL_WaitConditionTimeout(cnd, mtx, 5), "cond times out");
    SDL_UnlockMutex(mtx);

    SDL_SetAtomicInt(&tls_dtor_calls, 0);
    SDL_SetTLS(&tls_id, (void *)1, NULL);
    shared_counter = 0;
    SDL_Thread *t[4];
    for (int i = 0; i < 4; i++) {
        t[i] = SDL_CreateThread(worker, "sdlop-worker-with-a-long-name", (void *)(intptr_t)i);
        CHECK(t[i] != NULL, "create thread");
    }
    CHECK(strcmp(SDL_GetThreadName(t[0]), "sdlop-worker-with-a-long-name") == 0, "thread name");
    CHECK(SDL_GetThreadID(t[0]) != 0 && SDL_GetThreadID(t[0]) != SDL_GetCurrentThreadID(), "thread id");
    for (int i = 0; i < 4; i++) {
        int status = -5;
        SDL_WaitThread(t[i], &status);
        CHECK(status == i, "status %d", status);
    }
    CHECK(shared_counter == 40000, "mutex counter %d", shared_counter);
    CHECK(SDL_GetSemaphoreValue(sem) == 4, "sem value");
    CHECK(SDL_GetAtomicInt(&tls_dtor_calls) == 4, "TLS destructors %d", SDL_GetAtomicInt(&tls_dtor_calls));
    CHECK((intptr_t)SDL_GetTLS(&tls_id) == 1, "main TLS untouched");

    SDL_Thread *w = SDL_CreateThread(cond_waiter, "cond", NULL);
    SDL_Delay(10);
    SDL_LockMutex(mtx);
    shared_counter = -1;
    SDL_BroadcastCondition(cnd);
    SDL_UnlockMutex(mtx);
    int st;
    SDL_WaitThread(w, &st);
    CHECK(st == 7, "cond waiter");

    while (SDL_TryWaitSemaphore(sem)) {
        /* drain the workers' posts */
    }
    SDL_Thread *d = SDL_CreateThread(worker, "detached", (void *)9);
    SDL_DetachThread(d);
    CHECK(SDL_WaitSemaphoreTimeout(sem, 5000), "detached thread ran");
    SDL_Delay(10); /* let it finish exiting before its mutex goes away */

    SDL_InitState is;
    SDL_zero(is);
    CHECK(SDL_ShouldInit(&is), "should init");
    SDL_SetInitialized(&is, true);
    CHECK(!SDL_ShouldInit(&is), "already init");
    CHECK(SDL_ShouldQuit(&is), "should quit");
    SDL_SetInitialized(&is, false);

    SDL_DestroySemaphore(sem);
    SDL_DestroyCondition(cnd);
    SDL_DestroyMutex(mtx);
}

/* ------------------------------------------------------------------ */

static bool set_error_v(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool r = SDL_SetErrorV(fmt, ap);
    va_end(ap);
    return r;
}

static void test_misc(void)
{
    CHECK(SDL_GetVersion() == SDL_VERSION, "version");
    CHECK(SDL_VERSIONNUM_MAJOR(SDL_GetVersion()) == 3 && SDL_VERSIONNUM_MINOR(SDL_GetVersion()) == 4, "3.4");
    CHECK(SDL_GetRevision() && *SDL_GetRevision(), "revision");
    CHECK(strcmp(SDL_GetPlatform(), "Linux") == 0, "platform");
    CHECK(!set_error_v("err %d", 5) && strcmp(SDL_GetError(), "err 5") == 0, "SetErrorV");

    CHECK(strcmp(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING), "application") == 0, "type default");
    CHECK(strcmp(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING), "test_core") == 0, "exe name default");
    CHECK(SDL_SetAppMetadata("Core", "1.0", "org.sdlop.core"), "set metadata");
    CHECK(strcmp(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING), "Core") == 0, "name");
    SDL_SetHint(SDL_HINT_APP_NAME, "Hinted");
    CHECK(strcmp(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING), "Hinted") == 0, "hint wins");
    CHECK(!SDL_SetAppMetadataProperty("SDL.app.metadata.bogus", "x"), "bogus rejected");
}

int main(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    test_hints();
    test_properties();
    test_log();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    test_timers();
    test_events();
    test_clipboard();
    test_misc();
    test_threads();
    SDL_Quit();

    /* everything is usable again after SDL_Quit */
    CHECK(SDL_GetHint(SDL_HINT_APP_NAME) == NULL, "hints cleared by SDL_Quit");
    CHECK(SDL_Init(SDL_INIT_EVENTS), "re-init");
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_core: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_core: all checks passed\n");
    return 0;
}
