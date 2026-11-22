/* Callback-list allocation/lifetime regression through the real worker.
 * Link with --wrap=calloc/free and the pipe discovery wrappers.
 * zlib license, see LICENSE. */
#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "uinput_synth.h"

static void *allocations[16384];
static size_t allocated, freed;
static bool tracking, fail_next;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool entered, released;
static atomic_int delivered;

extern void *__real_calloc(size_t, size_t);
extern void __real_free(void *);
void *__wrap_calloc(size_t n, size_t size)
{
    if (tracking && fail_next) { fail_next = false; errno = ENOMEM; return NULL; }
    void *p = __real_calloc(n, size);
    if (tracking && p) {
        assert(allocated < sizeof(allocations) / sizeof(*allocations));
        allocations[allocated++] = p;
    }
    return p;
}

void __wrap_free(void *p)
{
    if (p) {
        for (size_t i = allocated; i > 0; i--) {
            if (allocations[i - 1] == p) {
                allocations[i - 1] = NULL;
                freed++;
                break;
            }
        }
    }
    __real_free(p);
}

static void blocked(const SDLop_RawEvent *event, void *userdata)
{
    (void)event; (void)userdata;
    pthread_mutex_lock(&gate);
    entered = true;
    pthread_cond_broadcast(&condition);
    while (!released) { assert(pthread_cond_wait(&condition, &gate) == 0); }
    pthread_mutex_unlock(&gate);
}

static void on_raw(const SDLop_RawEvent *event, void *userdata)
{
    (void)event; (void)userdata;
    atomic_fetch_add(&delivered, 1);
}

static void absent(const SDLop_RawEvent *event, void *userdata)
{
    (void)event; (void)userdata;
}

static void extra(const SDLop_RawEvent *event, void *userdata)
{
    (void)event; (void)userdata;
}

static Uint64 cpu_now(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0);
    return (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
}

static int compare(const void *a, const void *b)
{
    Uint64 x = *(const Uint64 *)a, y = *(const Uint64 *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_RAW_INPUT", "1", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "0", 1);
    UInputDevice *device = uinput_synth_create("callback-lifetime");
    assert(device && SDL_Init(SDL_INIT_VIDEO) && SDLop_RawInputAvailable());
    tracking = true;
    assert(SDLop_RegisterRawEventCallback(blocked, NULL));
    tracking = false;
    assert(uinput_synth_rel(device, SDLop_REL_X, 1));
    pthread_mutex_lock(&gate);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    while (!entered) { assert(pthread_cond_timedwait(&condition, &gate, &deadline) == 0); }
    pthread_mutex_unlock(&gate);

    /* Worker holds the old list while main replaces it. Nothing may be freed
     * yet, and absent unregister calls must not allocate replacement lists. */
    tracking = true;
    SDLop_UnregisterRawEventCallback(blocked);
    assert(SDLop_RegisterRawEventCallback(on_raw, NULL));
    size_t before = allocated;
    Uint64 times[1000];
    for (int i = 0; i < 1000; i++) {
        Uint64 start = cpu_now();
        for (int j = 0; j < 10; j++) { SDLop_UnregisterRawEventCallback(absent); }
        times[i] = cpu_now() - start;
    }
    size_t unnecessary = allocated - before;
    SDLop_UnregisterRawEventCallback(NULL);
    assert(!freed);
    tracking = false;
    pthread_mutex_lock(&gate);
    released = true;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&gate);

    /* List churn races native dispatch; userdata stays alive through join. */
    for (int i = 0; i < 500; i++) {
        tracking = true;
        assert(SDLop_RegisterRawEventCallback(extra, NULL));
        SDLop_UnregisterRawEventCallback(extra);
        tracking = false;
        assert(uinput_synth_rel(device, SDLop_REL_X, i + 1));
    }
    tracking = true;
    for (int i = 0; i < 7; i++) { assert(SDLop_RegisterRawEventCallback(extra, NULL)); }
    assert(!SDLop_RegisterRawEventCallback(extra, NULL)); /* 8 active maximum */
    SDLop_UnregisterRawEventCallback(extra); /* removes every matching entry */
    fail_next = true;
    assert(!SDLop_RegisterRawEventCallback(extra, NULL));
    assert(!fail_next);
    fail_next = true;
    SDLop_UnregisterRawEventCallback(on_raw); /* failed allocation preserves it */
    assert(!fail_next);
    tracking = false;
    assert(uinput_synth_rel(device, SDLop_REL_X, 501));
    Uint64 until = SDL_GetTicksNS() + 3000000000ull;
    while (atomic_load(&delivered) != 501) {
        assert(SDL_GetTicksNS() < until);
        SDL_Delay(1);
    }
    tracking = true;
    SDLop_UnregisterRawEventCallback(on_raw);
    before = allocated;
    SDLop_UnregisterRawEventCallback(on_raw); /* empty list */
    size_t empty_allocations = allocated - before;
    tracking = false;
    assert(!freed); /* old reader snapshots reclaimed only after worker join */
    SDL_Quit();
    assert(SDLop_GetRawEventCount() == 502); /* held reader + 501 persistent deliveries */
    /* Reinitialization must not carry a dangling ownership chain forward. */
    for (int cycle = 0; cycle < 3; cycle++) {
        assert(SDL_Init(SDL_INIT_VIDEO) && SDLop_RawInputAvailable());
        tracking = true;
        assert(SDLop_RegisterRawEventCallback(extra, NULL));
        SDLop_UnregisterRawEventCallback(extra);
        tracking = false;
        SDL_Quit();
    }
    uinput_synth_destroy(device);
    qsort(times, 1000, sizeof(*times), compare);
    printf("absent unregister: allocations=%zu/10000, thread-CPU block-average p50=%.1f p95=%.1f ns/call\n",
           unnecessary, (double)times[500] / 10, (double)times[950] / 10);
    printf("callback snapshots: allocated=%zu freed_after_join=%zu; empty_unregister_allocations=%zu; native delivery=501/501\n",
           allocated, freed, empty_allocations);
    bool ok = !unnecessary && !empty_allocations && allocated == freed;
    printf("test_evdev_callbacks: %s (no-op allocation, held reader, churn, max count, OOM, shutdown reclamation)\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
