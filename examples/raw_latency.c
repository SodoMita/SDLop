/*
  SDLop example: measure raw input latency (kernel timestamp -> callback),
  like asyncinput's read_keys / benchmark_asyncinput examples.

  Move the mouse / type keys; Ctrl+C (or ESC) stops and prints stats.
  --seconds N bounds the run. --drain continuously drains both event APIs
  on the main thread while callbacks are measured on the worker.
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SAMPLES 8192

static atomic_bool running = true;
static Uint64 samples[MAX_SAMPLES];
static atomic_int nsamples;
static volatile sig_atomic_t interrupted;

static void on_interrupt(int signum)
{
    (void)signum;
    interrupted = 1;
}

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
    if (ev->timestamp_ns && t >= ev->timestamp_ns) {
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

int main(int argc, char **argv)
{
    bool drain = false;
    long seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--drain")) {
            drain = true;
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            char *end;
            seconds = strtol(argv[++i], &end, 10);
            if (*end || seconds < 1 || seconds > 3600) {
                fprintf(stderr, "--seconds must be between 1 and 3600\n");
                return 1;
            }
        } else {
            bool help = !strcmp(argv[i], "--help");
            fprintf(help ? stdout : stderr,
                    "Usage: %s [--seconds N] [--drain]\n", argv[0]);
            return help ? 0 : 1;
        }
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDLop_RawInputAvailable()) {
        fprintf(stderr, "raw input unavailable: %s\n", SDL_GetError());
        fprintf(stderr, "(need access to /dev/input: root or the 'input' group;\n"
                        " on X11/Wayland also SDLOP_RAW_INPUT=1)\n");
        SDL_Quit();
        return 2;
    }

    if (!SDLop_RegisterRawEventCallback(on_raw_event, NULL)) {
        fprintf(stderr, "register callback failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_interrupt;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    printf("waiting for input events... ESC/Ctrl+C quits (main thread: %s)\n",
           drain ? "continuously draining" : "sleeping");

    const Uint64 deadline = seconds ? now_ns() + (Uint64)seconds * 1000000000ull : 0;
    while (atomic_load(&running) && !interrupted && (!deadline || now_ns() < deadline)) {
        if (drain) {
            SDL_Event event;
            /* Bound both drains: a continuous source must not prevent the
             * outer loop from observing --seconds, ESC or SIGINT. */
            for (int i = 0; i < 256 && SDL_PollEvent(&event); i++) {
            }
            SDLop_RawEvent events[128];
            for (int i = 0; i < 4 && SDLop_PollRawEvents(events, 128) > 0; i++) {
            }
        } else {
            SDL_Delay(10);
        }
    }

    /* Callback unregister is not a quiescence barrier. Join the worker before
     * sorting its samples, including when interrupted mid-batch. */
    SDL_Quit();
    int n = atomic_load(&nsamples);
    if (n > 0) {
        qsort(samples, (size_t)n, sizeof(samples[0]), cmp_u64);
        double sum = 0;
        for (int i = 0; i < n; i++) {
            sum += (double)samples[i];
        }
        printf("events: %d\n", n);
        printf("kernel->callback latency:\n");
        printf("  min: %8.1f us\n", (double)samples[0] / 1e3);
        printf("  avg: %8.1f us\n", (double)sum / (double)n / 1e3);
        printf("  p50: %8.1f us\n", (double)samples[n / 2] / 1e3);
        printf("  p95: %8.1f us\n", (double)samples[n * 95 / 100] / 1e3);
        printf("  p99: %8.1f us\n", (double)samples[n * 99 / 100] / 1e3);
        printf("  max: %8.1f us\n", (double)samples[n - 1] / 1e3);
        if (n == MAX_SAMPLES) {
            printf("  sample buffer full; statistics cover the first %d events\n", MAX_SAMPLES);
        }
    } else {
        printf("no events captured\n");
    }

    return 0;
}
