/*
  SDLop core tests: error handling, properties, hints, rectangles, pixel
  formats, timers, the event queue and threads. No video device is needed, so
  this test runs anywhere.

      make check        (or: ./build/tests/test_core)
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <pthread.h>

static int failures;
static int checks;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        checks++;                                                               \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

static void test_version(void)
{
    int version = SDL_GetVersion();
    SDL_SDLopVersion sdlop;

    CHECK(version > 0, "SDL_GetVersion() returned %d", version);
    SDL_GetSDLopVersion(&sdlop);
    CHECK(sdlop.major == SDL_SDLOP_MAJOR_VERSION, "SDLOp version major %d", sdlop.major);
    printf("  SDL3 API version %d, SDLop %d.%d.%d\n", version, sdlop.major, sdlop.minor, sdlop.micro);
}

static void test_error(void)
{
    CHECK(SDL_GetError() != NULL, "SDL_GetError() returned NULL");
    CHECK(!SDL_SetError("failure %d", 42), "SDL_SetError() must return false");
    CHECK(SDL_strcmp(SDL_GetError(), "failure 42") == 0, "error text: '%s'", SDL_GetError());
    CHECK(SDL_ClearError(), "SDL_ClearError() failed");
    CHECK(SDL_GetError()[0] == '\0', "error string not cleared");
    SDL_ClearError();
    CHECK(!SDL_InvalidParamError("window"), "SDL_InvalidParamError() must return false");
    CHECK(SDL_strstr(SDL_GetError(), "window") != NULL, "error text: '%s'", SDL_GetError());
    SDL_ClearError();
}

static void test_properties(void)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_CleanupPropertyCallback cleanup_called = NULL;
    void *cleaned = NULL;

    CHECK(props != 0, "SDL_CreateProperties() failed");
    CHECK(SDL_SetStringProperty(props, "name", "leandsl3"), "set string");
    CHECK(SDL_SetNumberProperty(props, "count", 7), "set number");
    CHECK(SDL_SetBooleanProperty(props, "flag", true), "set boolean");
    CHECK(SDL_SetFloatProperty(props, "scale", 1.5f), "set float");
    CHECK(SDL_strcmp(SDL_GetStringProperty(props, "name", ""), "leandsl3") == 0, "get string");
    CHECK(SDL_GetNumberProperty(props, "count", 0) == 7, "get number");
    CHECK(SDL_GetBooleanProperty(props, "flag", false) == true, "get boolean");
    CHECK(SDL_GetFloatProperty(props, "scale", 0.0f) == 1.5f, "get float");
    CHECK(SDL_GetNumberProperty(props, "missing", -1) == -1, "default value");
    CHECK(SDL_GetPropertyType(props, "count") == SDL_PROPERTY_TYPE_NUMBER, "property type");
    CHECK(!SDL_HasProperty(props, "missing"), "SDL_HasProperty() lied");
    CHECK(SDL_SetPointerProperty(props, "pointer", &checks), "set pointer");
    CHECK(SDL_GetPointerProperty(props, "pointer", NULL) == &checks, "get pointer");
    CHECK(SDL_ClearProperty(props, "count"), "clear property");
    CHECK(!SDL_HasProperty(props, "count"), "property survived SDL_ClearProperty()");

    CHECK(SDL_SetPointerPropertyWithCleanup(props, "cleaned", &checks,
                                            (SDL_CleanupPropertyCallback)NULL, NULL), "set pointer");
    (void)cleanup_called;
    (void)cleaned;

    /* global properties are shared, and that is where app metadata lives */
    CHECK(SDL_SetAppMetadata("SDLop test", "1.0", "org.sdlop.test"), "SDL_SetAppMetadata()");
    CHECK(SDL_strcmp(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING), "SDLop test") == 0,
          "app metadata name");
    CHECK(SDL_GetGlobalProperties() != 0, "SDL_GetGlobalProperties()");

    SDL_DestroyProperties(props);
}

static void hint_callback(void *userdata, const char *name, const char *old_value,
                          const char *new_value)
{
    int *count = (int *)userdata;
    if (SDL_strcmp(name, "SDLOP_TEST_HINT") == 0) {
        (*count)++;
    }
    (void)old_value;
    (void)new_value;
}

static void test_hints(void)
{
    int calls = 0;

    SDL_SetHint("SDLOP_TEST_HINT", "first");
    CHECK(SDL_strcmp(SDL_GetHint("SDLOP_TEST_HINT"), "first") == 0, "hint value");
    CHECK(SDL_AddHintCallback("SDLOP_TEST_HINT", hint_callback, &calls), "add hint callback");
    CHECK(calls == 1, "adding a callback must call it once (calls=%d)", calls);
    SDL_SetHint("SDLOP_TEST_HINT", "second");
    CHECK(calls == 2, "callback on change (calls=%d)", calls);
    SDL_SetHint("SDLOP_TEST_HINT", "second");
    CHECK(calls == 2, "no callback when the value does not change (calls=%d)", calls);
    CHECK(SDL_GetHintBoolean("SDLOP_TEST_HINT", false) == true, "SDL_GetHintBoolean()");
    SDL_RemoveHintCallback("SDLOP_TEST_HINT", hint_callback, &calls);
    SDL_ResetHint("SDLOP_TEST_HINT");
    CHECK(SDL_GetHint("SDLOP_TEST_HINT") == NULL, "hint not reset");
}

static void test_rect(void)
{
    SDL_Rect a = { 0, 0, 10, 10 };
    SDL_Rect b = { 5, 5, 10, 10 };
    SDL_Rect c = { 20, 20, 5, 5 };
    SDL_Rect r;
    SDL_FRect fa = { 0.0f, 0.0f, 10.0f, 10.0f };
    SDL_FRect fb = { 5.0f, 5.0f, 10.0f, 10.0f };
    SDL_FRect fr;

    CHECK(SDL_HasRectIntersection(&a, &b), "rects overlap");
    CHECK(!SDL_HasRectIntersection(&a, &c), "rects do not overlap");
    CHECK(SDL_GetRectIntersection(&a, &b, &r) && r.x == 5 && r.y == 5 && r.w == 5 && r.h == 5,
          "intersection = %d,%d %dx%d", r.x, r.y, r.w, r.h);
    CHECK(SDL_GetRectUnion(&a, &c, &r) && r.x == 0 && r.y == 0 && r.w == 25 && r.h == 25,
          "union = %d,%d %dx%d", r.x, r.y, r.w, r.h);
    CHECK(SDL_HasRectIntersectionFloat(&fa, &fb), "float rects overlap");
    CHECK(SDL_GetRectIntersectionFloat(&fa, &fb, &fr) && fr.w == 5.0f, "float intersection");
    {
        float x1 = -5.0f, y1 = 5.0f, x2 = 15.0f, y2 = 5.0f;
        CHECK(SDL_GetRectAndLineIntersectionFloat(&fa, &x1, &y1, &x2, &y2), "line clip");
        CHECK(x1 >= 0.0f && x1 <= 10.0f && x2 >= 0.0f && x2 <= 10.0f, "line clipped to %.2f..%.2f", x1, x2);
    }
}

static void test_pixels(void)
{
    const SDL_PixelFormatDetails *details = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_ARGB8888);
    Uint8 r, g, b, a;
    Uint32 pixel;
    int bpp;
    Uint32 rmask, gmask, bmask, amask;

    CHECK(details != NULL, "SDL_GetPixelFormatDetails(ARGB8888)");
    CHECK(details->bytes_per_pixel == 4, "ARGB8888 bpp = %d", details->bytes_per_pixel);
    CHECK(details->Rmask == 0x00FF0000 && details->Amask == 0xFF000000,
          "ARGB8888 masks %08x %08x", details->Rmask, details->Amask);
    pixel = SDL_MapRGBA(details, NULL, 0x11, 0x22, 0x33, 0x44);
    CHECK(pixel == 0x44112233, "SDL_MapRGBA() = %08x", pixel);
    SDL_GetRGBA(pixel, details, NULL, &r, &g, &b, &a);
    CHECK(r == 0x11 && g == 0x22 && b == 0x33 && a == 0x44, "round trip %02x %02x %02x %02x", r, g, b, a);
    CHECK(SDL_strcmp(SDL_GetPixelFormatName(SDL_PIXELFORMAT_XRGB8888), "XRGB8888") == 0,
          "SDL_GetPixelFormatName()");
    CHECK(SDL_GetMasksForPixelFormat(SDL_PIXELFORMAT_RGB565, &bpp, &rmask, &gmask, &bmask, &amask),
          "SDL_GetMasksForPixelFormat()");
    CHECK(bpp == 16, "RGB565 bpp = %d", bpp);
    CHECK(SDL_GetPixelFormatForMasks(bpp, rmask, gmask, bmask, amask) == SDL_PIXELFORMAT_RGB565,
          "SDL_GetPixelFormatForMasks() round trip");
}

static void test_timer(void)
{
    Uint64 start = SDL_GetTicksNS();
    Uint64 frequency = SDL_GetPerformanceFrequency();

    CHECK(frequency > 0, "performance frequency = %llu", (unsigned long long)frequency);
    /* the performance counter is a raw monotonic clock, ticks are relative to
       SDL_Init(), so only their *rates* are comparable */
    CHECK(frequency >= 1000000, "performance frequency = %llu", (unsigned long long)frequency);
    CHECK(SDL_GetPerformanceCounter() != SDL_GetPerformanceCounter(), "the performance counter is frozen");
    SDL_Delay(20);
    {
        Uint64 elapsed = SDL_GetTicksNS() - start;
        CHECK(elapsed >= 15000000, "SDL_Delay(20) slept only %llu ns", (unsigned long long)elapsed);
    }
    CHECK(SDL_GetTicks() > 0, "SDL_GetTicks()");
}

static Uint32 timer_fired;

static Uint32 SDLCALL timer_callback(void *userdata, SDL_TimerID id, Uint32 interval)
{
    Uint32 *counter = (Uint32 *)userdata;
    (*counter)++;
    (void)id;
    return interval;                   /* keep firing */
}

/* pushes one event after a short delay, so the main thread has to block and be
   woken up by the push */
static void *push_event_thread(void *arg)
{
    SDL_Event event;
    Uint32 type = (Uint32)(uintptr_t)arg;

    SDL_Delay(20);
    SDL_zero(event);
    event.type = type;
    event.user.code = 3;
    SDL_PushEvent(&event);
    return NULL;
}

static void test_events(void)
{
    SDL_Event event;
    SDL_Event pushed;
    Uint32 user_type;
    SDL_TimerID timer;

    CHECK(SDL_Init(SDL_INIT_EVENTS), "SDL_Init(SDL_INIT_EVENTS): %s", SDL_GetError());
    CHECK(SDL_WasInit(SDL_INIT_EVENTS) == SDL_INIT_EVENTS, "SDL_WasInit()");
    CHECK(SDL_EventEnabled(SDL_EVENT_KEY_DOWN), "key events are enabled by default");
    SDL_SetEventEnabled(SDL_EVENT_KEY_DOWN, false);
    CHECK(!SDL_EventEnabled(SDL_EVENT_KEY_DOWN), "SDL_SetEventEnabled(false)");
    SDL_SetEventEnabled(SDL_EVENT_KEY_DOWN, true);

    user_type = SDL_RegisterEvents(4);
    CHECK(user_type >= SDL_EVENT_USER, "SDL_RegisterEvents() = %u", user_type);

    SDL_zero(pushed);
    pushed.type = user_type;
    pushed.user.code = 1;
    pushed.user.data1 = (void *)0x1234;
    CHECK(SDL_PushEvent(&pushed), "SDL_PushEvent(): %s", SDL_GetError());
    CHECK(SDL_HasEvent(user_type), "SDL_HasEvent()");
    SDL_zero(event);
    CHECK(SDL_PollEvent(&event), "SDL_PollEvent()");
    CHECK(event.type == user_type && event.user.code == 1 && event.user.data1 == (void *)0x1234,
          "user event round trip");
    CHECK(!SDL_PollEvent(&event), "queue should be empty");

    /* a flush must drop the events of that type */
    CHECK(SDL_PushEvent(&pushed), "push for flush");
    SDL_FlushEvent(user_type);
    CHECK(!SDL_HasEvent(user_type), "SDL_FlushEvent()");

    /* waits must time out rather than hang */
    {
        Uint64 start = SDL_GetTicks();
        CHECK(!SDL_WaitEventTimeout(&event, 30), "SDL_WaitEventTimeout() should time out");
        CHECK(SDL_GetTicks() - start >= 20, "SDL_WaitEventTimeout() returned too early");
    }

    /* a wait must be woken by an event pushed from another thread */
    {
        pthread_t thread;
        pthread_create(&thread, NULL, push_event_thread, (void *)(uintptr_t)user_type);
        CHECK(SDL_WaitEventTimeout(&event, 2000), "SDL_WaitEventTimeout() was not woken by the push");
        CHECK(event.type == user_type && event.user.code == 3, "wrong event delivered");
        pthread_join(thread, NULL);
    }

    /* timer callbacks are serviced by the event pump */
    timer = SDL_AddTimer(5, timer_callback, &timer_fired);
    CHECK(timer != 0, "SDL_AddTimer(): %s", SDL_GetError());
    {
        Uint64 deadline = SDL_GetTicks() + 500;
        while (timer_fired < 3 && SDL_GetTicks() < deadline) {
            SDL_PumpEvents();
            SDL_Delay(1);
        }
    }
    CHECK(timer_fired >= 3, "timer fired %u times", timer_fired);
    CHECK(SDL_RemoveTimer(timer), "SDL_RemoveTimer()");

    SDL_QuitSubSystem(SDL_INIT_EVENTS);
}

static void *thread_main(void *arg)
{
    int *value = (int *)arg;
    *value = 1;
    SDL_RunOnMainThread(NULL, NULL, false);            /* must fail, not crash */
    *value += SDL_IsMainThread() ? 100 : 0;
    return NULL;
}

static int main_thread_calls;

static void SDLCALL main_thread_callback(void *userdata)
{
    (void)userdata;
    main_thread_calls++;
}

static void test_threads(void)
{
    pthread_t thread;
    int thread_value = 0;

    CHECK(SDL_Init(SDL_INIT_EVENTS), "SDL_Init for threads");
    CHECK(SDL_IsMainThread(), "SDL_IsMainThread() on the main thread");
    SDL_RunOnMainThread(main_thread_callback, NULL, false);
    CHECK(main_thread_calls == 1, "SDL_RunOnMainThread() ran inline on the main thread");
    SDL_PumpEvents();
    CHECK(main_thread_calls == 1, "callback ran twice");

    pthread_create(&thread, NULL, thread_main, &thread_value);
    pthread_join(thread, NULL);
    CHECK(thread_value >= 1, "thread body did not run");
    CHECK((thread_value & 100) == 0, "SDL_IsMainThread() is true on a worker thread");
}

static void test_strings(void)
{
    char buffer[16];
    char *copy;

    CHECK(SDL_strlcpy(buffer, "hello", sizeof(buffer)) == 5, "SDL_strlcpy() return value");
    CHECK(SDL_strcmp(buffer, "hello") == 0, "SDL_strlcpy() result");
    CHECK(SDL_strlcat(buffer, " world", sizeof(buffer)) == 11, "SDL_strlcat()");
    CHECK(SDL_strcmp(buffer, "hello world") == 0, "SDL_strlcat() result");
    copy = SDL_strdup("copy");
    CHECK(copy && SDL_strcmp(copy, "copy") == 0, "SDL_strdup()");
    SDL_free(copy);
    CHECK(SDL_strcasecmp("ABC", "abc") == 0, "SDL_strcasecmp()");
    CHECK(SDL_snprintf(buffer, sizeof(buffer), "%d", 1234) == 4, "SDL_snprintf()");
    CHECK(SDL_atoi("  -42xyz") == -42, "SDL_atoi()");
    CHECK(SDL_strtol("0x10", NULL, 16) == 16, "SDL_strtol()");
    {
        Uint64 state = 0;
        Uint32 first, second;
        SDL_srand(1234);
        first = SDL_rand_bits();
        second = SDL_rand_bits();
        CHECK(first != second, "SDL_rand_bits() repeats itself");
        SDL_srand(1234);
        CHECK(SDL_rand_bits() == first, "SDL_srand() must make the sequence repeatable");
        CHECK(SDL_rand(10) >= 0 && SDL_rand(10) < 10, "SDL_rand() range");
        state = 1234;
        CHECK(SDL_rand_r(&state, 100) < 100, "SDL_rand_r() range");
        CHECK(SDL_randf() >= 0.0f && SDL_randf() < 1.0f, "SDL_randf() range");
    }
}

static void test_keyboard_names(void)
{
    CHECK(SDL_strcmp(SDL_GetScancodeName(SDL_SCANCODE_A), "A") == 0, "scancode name A = '%s'",
          SDL_GetScancodeName(SDL_SCANCODE_A));
    CHECK(SDL_GetScancodeFromName("A") == SDL_SCANCODE_A, "SDL_GetScancodeFromName(\"A\")");
    CHECK(SDL_GetScancodeFromName("Left Shift") == SDL_SCANCODE_LSHIFT, "SDL_GetScancodeFromName(\"Left Shift\")");
    CHECK(SDL_GetScancodeFromName("Space") == SDL_SCANCODE_SPACE, "space");
    CHECK(SDL_GetScancodeFromName("F12") == SDL_SCANCODE_F12, "F12");
    CHECK(SDL_GetScancodeFromName("Keypad 1") == SDL_SCANCODE_KP_1, "keypad 1");
    CHECK(SDL_strcmp(SDL_GetKeyName(SDLK_ESCAPE), "Escape") == 0, "SDL_GetKeyName(ESCAPE)");
    CHECK(SDL_GetKeyFromName("Escape") == SDLK_ESCAPE, "SDL_GetKeyFromName(\"Escape\")");
    CHECK(SDL_GetKeyFromName("a") == SDLK_A, "SDL_GetKeyFromName(\"a\")");

    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_NONE, false) == SDLK_A, "A without shift");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_SHIFT, false) == 'A', "A with shift = %x",
          SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_SHIFT, false));
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_1, SDL_KMOD_NONE, false) == '1', "1");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_1, SDL_KMOD_SHIFT, false) == '!', "shift+1 = %c",
          (char)SDL_GetKeyFromScancode(SDL_SCANCODE_1, SDL_KMOD_SHIFT, false));
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_RETURN, SDL_KMOD_NONE, false) == SDLK_RETURN, "return");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_KP_1, SDL_KMOD_NONE, false) == SDLK_KP_1, "keypad 1");
    {
        SDL_Keymod modstate = SDL_KMOD_NONE;
        CHECK(SDL_GetScancodeFromKey('A', &modstate) == SDL_SCANCODE_A, "scancode from 'A'");
        CHECK((modstate & SDL_KMOD_SHIFT) != 0, "uppercase implies shift");
        CHECK(SDL_GetScancodeFromKey('1', NULL) == SDL_SCANCODE_1, "scancode from '1'");
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("test_core (SDLop)\n");
    test_version();
    test_error();
    test_properties();
    test_hints();
    test_rect();
    test_pixels();
    test_timer();
    test_strings();
    test_keyboard_names();
    test_events();
    test_threads();

    printf("%d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
