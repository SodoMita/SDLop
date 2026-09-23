/*
  Benchmark: event queue push/poll throughput.

  Compiled twice: once against SDLop, once against real SDL3
  (-DSDLOP_BENCH_SDL3), using the identical source and the dummy/offscreen
  video driver. Measures SDL_PushEvent + SDL_PollEvent cost.

  Consumer side drains with PollEvent-until-false (the standard frame
  pattern) and validates ordering of the user events actually returned.
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef SDLOP_BENCH_SDL3
#define BENCH_NAME "SDLop"
#else
#define BENCH_NAME "SDL3 "
#endif

#define ITERATIONS 1000000

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    Uint32 usertype = SDL_RegisterEvents(1);
    if (!usertype) {
        fprintf(stderr, "SDL_RegisterEvents failed\n");
        return 1;
    }

    SDL_Event ev;
    SDL_memset(&ev, 0, sizeof(ev));

    /* warmup */
    for (int i = 0; i < 10000; i++) {
        ev.type = usertype;
        ev.user.code = i;
        SDL_PushEvent(&ev);
    }
    while (SDL_PollEvent(&ev)) {
    }

    /* batched: push N, then drain (typical frame pattern) */
    const int batch = 64;
    double t0 = now_sec();
    long long got = 0;
    int expected = 0;
    int order_violations = 0;
    for (int i = 0; i < ITERATIONS / batch; i++) {
        for (int j = 0; j < batch; j++) {
            ev.type = usertype;
            ev.user.code = i * batch + j;
            while (!SDL_PushEvent(&ev)) {
                /* queue full: drain a bit and retry */
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == usertype) {
                        if (ev.user.code != expected) {
                            order_violations++;
                        }
                        expected = ev.user.code + 1;
                        got++;
                    }
                }
            }
        }
        while (SDL_PollEvent(&ev)) {
            if (ev.type != usertype) {
                continue; /* skip engine-internal events */
            }
            if (ev.user.code != expected) {
                order_violations++;
                expected = ev.user.code; /* resync */
            }
            expected++;
            got++;
        }
    }
    /* drain leftovers */
    while (SDL_PollEvent(&ev)) {
        if (ev.type == usertype) {
            if (ev.user.code != expected) {
                order_violations++;
                expected = ev.user.code;
            }
            expected++;
            got++;
        }
    }
    double t1 = now_sec();
    printf("%s push+poll: %8.1f ns/push+drain  (%.2f M events pushed/s, %lld returned%s)\n",
           BENCH_NAME, (t1 - t0) / (double)ITERATIONS * 1e9,
           ITERATIONS / (t1 - t0) / 1e6, got,
           order_violations ? " (ORDER VIOLATIONS!)" : "");
    if (order_violations) {
        return 1;
    }
    if (got != ITERATIONS) {
        printf("%s WARNING: only %lld/%d events returned by poll\n", BENCH_NAME, got, ITERATIONS);
    }

    /* SDL_PumpEvents cost with an empty queue (called once per frame) */
    const int pumps = 200000;
    t0 = now_sec();
    for (int i = 0; i < pumps; i++) {
        SDL_PumpEvents();
    }
    t1 = now_sec();
    printf("%s empty pump: %8.1f ns/call\n", BENCH_NAME, (t1 - t0) / (double)pumps * 1e9);

    SDL_Quit();
    return 0;
}
