/*
  SDLop example: measure raw input latency (kernel timestamp -> callback),
  like asyncinput's read_keys / benchmark_asyncinput examples.

  Move the mouse / type keys; Ctrl+C (or ESC) stops and prints stats.
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_SAMPLES 8192

static atomic_bool running = true;
static Uint64 samples[MAX_SAMPLES];
static atomic_int nsamples;

static Uint64 now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
}

static void on_raw_event(const SDLop_RawEvent *ev, void *userdata)
{
    (void)userdata;
    if (ev->type == SDLop_EV_KEY && ev->code == SDLop_KEY_ESC && ev->value) {
        atomic_store(&running, false);
        return;
    }
    if (ev->type == SDLop_EV_SYN) {
        return;
    }
    Uint64 t = now_ns();
    if (t >= ev->timestamp_ns) {
        int n = atomic_load(&nsamples);
        if (n < MAX_SAMPLES) {
            samples[n] = t - ev->timestamp_ns;
            atomic_store(&nsamples, n + 1);
        }
    }
}

static int cmp_u64(const void *a, const void *b)
{
    Uint64 x = *(const Uint64 *)a, y = *(const Uint64 *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDLop_RawInputAvailable()) {
        fprintf(stderr, "raw input unavailable: %s\n", SDL_GetError());
        fprintf(stderr, "(need access to /dev/input: root or the 'input' group)\n");
        SDL_Quit();
        return 2;
    }

    SDLop_RegisterRawEventCallback(on_raw_event, NULL);
    printf("waiting for input events... ESC quits\n");

    while (atomic_load(&running)) {
        SDL_Delay(10);
    }

    int n = atomic_load(&nsamples);
    if (n > 0) {
        qsort(samples, (size_t)n, sizeof(samples[0]), cmp_u64);
        Uint64 sum = 0;
        for (int i = 0; i < n; i++) {
            sum += samples[i];
        }
        printf("events: %d\n", n);
        printf("kernel->callback latency:\n");
        printf("  min: %8.1f us\n", (double)samples[0] / 1e3);
        printf("  avg: %8.1f us\n", (double)sum / (double)n / 1e3);
        printf("  p50: %8.1f us\n", (double)samples[n / 2] / 1e3);
        printf("  p95: %8.1f us\n", (double)samples[(int)(n * 0.95)] / 1e3);
        printf("  max: %8.1f us\n", (double)samples[n - 1] / 1e3);
    } else {
        printf("no events captured\n");
    }

    SDL_Quit();
    return 0;
}
