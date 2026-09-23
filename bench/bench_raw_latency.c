/*
  Benchmark: raw input path latency.

  Injects synthetic events via /dev/uinput with kernel timestamps and
  measures:
    - kernel event time -> worker-thread callback
    - kernel event time -> main-thread SDL_PumpEvents consumption

  This is the asyncinput-style path: no window system involved.
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <linux/input-event-codes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "uinput_synth.h"

#define EVENTS 2000
#define GAP_US 200

static atomic_bool stop_cb;
static Uint64 cb_latencies[EVENTS];
static atomic_int cb_n;
static Uint64 pump_latencies[EVENTS];
static int pump_n;
static Uint64 inject_times[EVENTS]; /* our CLOCK_MONOTONIC at write() */

static void on_raw(const SDLop_RawEvent *ev, void *userdata)
{
    (void)userdata;
    if (stop_cb) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    Uint64 now = (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
    if (ev->type == SDLop_EV_REL && ev->code == REL_X && ev->timestamp_ns > 0 && now > ev->timestamp_ns) {
        int n = atomic_load(&cb_n);
        if (n < EVENTS) {
            cb_latencies[n] = now - ev->timestamp_ns;
            atomic_store(&cb_n, n + 1);
        }
    }
}

static int cmp_u64(const void *a, const void *b)
{
    Uint64 x = *(const Uint64 *)a, y = *(const Uint64 *)b;
    return (x > y) - (x < y);
}

static void report(const char *name, Uint64 *lat, int n)
{
    if (n == 0) {
        printf("%-28s no samples\n", name);
        return;
    }
    qsort(lat, (size_t)n, sizeof(lat[0]), cmp_u64);
    Uint64 sum = 0;
    for (int i = 0; i < n; i++) {
        sum += lat[i];
    }
    printf("%-28s n=%d  min=%6.1fus  avg=%6.1fus  p50=%6.1fus  p95=%6.1fus  max=%6.1fus\n",
           name, n,
           (double)lat[0] / 1e3,
           (double)sum / (double)n / 1e3,
           (double)lat[n / 2] / 1e3,
           (double)lat[(int)(n * 0.95)] / 1e3,
           (double)lat[n - 1] / 1e3);
}

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);

    UInputDevice *synth = uinput_synth_create("sdlop-latency");
    if (!synth) {
        printf("bench_raw_latency: SKIP (/dev/uinput not accessible)\n");
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        uinput_synth_destroy(synth);
        return 1;
    }
    if (!SDLop_RawInputAvailable()) {
        printf("bench_raw_latency: SKIP (raw input unavailable: %s)\n", SDL_GetError());
        SDL_ClearError();
        SDL_Quit();
        uinput_synth_destroy(synth);
        return 0;
    }

    SDLop_RegisterRawEventCallback(on_raw, NULL);

    /* inject REL_X events with pacing */
    for (int i = 0; i < EVENTS; i++) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        inject_times[i] = (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
        uinput_synth_rel(synth, REL_X, 1);
        uinput_synth_syn(synth);

        /* consume on the "main thread" until our REL_X shows up in the
         * raw ring (this is what a game frame's PumpEvents would do) */
        bool found = false;
        Uint64 deadline = inject_times[i] + 10000000ull; /* 10ms safety */
        while (!found) {
            SDL_PumpEvents();
            SDLop_RawEvent raws[16];
            int n = SDLop_PollRawEvents(raws, 16);
            for (int j = 0; j < n; j++) {
                if (raws[j].type == SDLop_EV_REL && raws[j].code == REL_X) {
                    found = true;
                }
            }
            struct timespec nowts;
            clock_gettime(CLOCK_MONOTONIC, &nowts);
            Uint64 now = (Uint64)nowts.tv_sec * 1000000000ull + (Uint64)nowts.tv_nsec;
            if (!found && now > deadline) {
                break;
            }
        }
        struct timespec nowts;
        clock_gettime(CLOCK_MONOTONIC, &nowts);
        Uint64 now = (Uint64)nowts.tv_sec * 1000000000ull + (Uint64)nowts.tv_nsec;
        pump_latencies[pump_n++] = now - inject_times[i];
        SDL_FlushEvents(0, 0xFFFFFFFFu); /* keep the SDL queue drained */

        struct timespec gap = { 0, GAP_US * 1000 };
        nanosleep(&gap, NULL);
    }
    atomic_store(&stop_cb, true);
    SDL_Delay(50);

    report("uinput write -> callback", cb_latencies, atomic_load(&cb_n));
    report("uinput write -> SDL_PumpEvent", pump_latencies, pump_n);
    printf("note: includes synthetic uinput->evdev kernel overhead\n");

    SDL_Quit();
    uinput_synth_destroy(synth);
    return 0;
}
