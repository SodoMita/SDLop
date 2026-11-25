/*
  Synthetic kernel-timestamp -> callback / raw poll / SDL poll / SDL wait
  latency benchmark. The uinput build needs device access; the pipe build
  uses a rootless, process-local userspace transport with the same real worker.

  A separate producer allows the main thread to drain while the worker is
  publishing. --stress injects bursts rather than one paced event at a time.
  No changes to the library's worker hot path are needed for instrumentation.
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <linux/input-event-codes.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "uinput_synth.h"

/* Sequence numbers are encoded in float xrel downstream. Keep all supported
 * values exactly representable and the discovery marker outside that range. */
#define MAX_REPORTS 4000000
#define DISCOVERY_MARKER INT_MAX

static struct {
    int reports;
    int rate;       /* reports/second; zero means saturation */
    int burst;      /* reports per scheduled batch */
    int axes;       /* REL_X probe, optionally REL_Y load, then SYN_REPORT */
} config = { 160000, 16000, 1, 2 };

#ifdef SDLOP_BENCH_PIPE
#define SOURCE_LABEL "pipe stamp"
#else
#define SOURCE_LABEL "kernel"
#endif

/* Only the worker writes callback samples; the main thread reads them after
 * SDL_Quit has joined it. The other sample sets have only a main-thread writer. */
typedef struct Samples
{
    Uint64 *latency;
    unsigned char *seen;
    int expected;
    int count;
    int invalid;
} Samples;

/* Do not make producer/consumer accounting counters share cache lines. */
_Alignas(64) static Samples callbacks;
_Alignas(64) static Samples raw_poll;
_Alignas(64) static Samples sdl_events;
/* Worker-owned source timestamps and main-owned receipt timestamps are paired
 * only after SDL_Quit joins the worker. This avoids relying on the SDL event
 * and SDL_GetTicksNS clocks having the same origin (currently they do not). */
static Uint64 *source_timestamps, *sdl_receipts;
_Alignas(64) static Uint64 native_callbacks;
_Alignas(64) static Uint64 native_polled, sdl_motions;
_Alignas(64) static bool callback_started;
_Alignas(64) static bool raw_started;
_Alignas(64) static int sdl_received;
static Uint32 device;
static atomic_bool producer_done;

static Uint64 now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
}

static void record(Samples *samples, int sequence, Uint64 timestamp, Uint64 now)
{
    if (sequence < 1 || sequence > samples->expected || samples->seen[sequence - 1]) {
        return;
    }
    samples->seen[sequence - 1] = true;
    if (!timestamp || now < timestamp) {
        samples->invalid++;
        return;
    }
    samples->latency[samples->count++] = now - timestamp;
}

static void on_raw(const SDLop_RawEvent *ev, void *userdata)
{
    (void)userdata;
    if (ev->device != device) {
        return;
    }
    if (ev->type == SDLop_EV_REL && ev->code == REL_X &&
        ev->value >= 1 && ev->value <= config.reports) {
        callback_started = true;
        const Uint64 received = now_ns();
        if (!callbacks.seen[ev->value - 1]) {
            source_timestamps[ev->value - 1] = ev->timestamp_ns;
        }
        record(&callbacks, ev->value, ev->timestamp_ns, received);
    }
    if (callback_started) {
        native_callbacks++;
    }
}

static int cmp_u64(const void *a, const void *b)
{
    Uint64 x = *(const Uint64 *)a, y = *(const Uint64 *)b;
    return (x > y) - (x < y);
}

static bool report(const char *name, Samples *samples)
{
    const int n = samples->count;
    printf("%-30s n=%d/%d missing=%d invalid=%d", name, n, samples->expected,
           samples->expected - n - samples->invalid, samples->invalid);
    if (!n) {
        printf("  no samples\n");
        return false;
    }
    qsort(samples->latency, (size_t)n, sizeof(samples->latency[0]), cmp_u64);
    double sum = 0;
    for (int i = 0; i < n; i++) {
        sum += (double)samples->latency[i];
    }
    printf("  min=%.1fus avg=%.1fus p50=%.1fus p95=%.1fus p99=%.1fus max=%.1fus\n",
           (double)samples->latency[0] / 1e3, sum / n / 1e3,
           (double)samples->latency[n / 2] / 1e3,
           (double)samples->latency[n * 95 / 100] / 1e3,
           (double)samples->latency[n * 99 / 100] / 1e3,
           (double)samples->latency[n - 1] / 1e3);
    return n == samples->expected && !samples->invalid;
}

/* Allocate/prefault outside the worker. No per-event allocation, locks or
 * additional worker syscalls are needed when collecting millions of samples. */
static void prefault(void *memory, size_t bytes)
{
    volatile unsigned char *p = memory;
    for (size_t i = 0; i < bytes; i += 4096) {
        p[i] = 0;
    }
}

static void release_samples(Samples *samples)
{
    free(samples->latency);
    free(samples->seen);
    memset(samples, 0, sizeof(*samples));
}

static bool allocate_samples(Samples *samples, int expected)
{
    memset(samples, 0, sizeof(*samples));
    samples->expected = expected;
    samples->latency = calloc((size_t)expected, sizeof(*samples->latency));
    samples->seen = calloc((size_t)expected, sizeof(*samples->seen));
    if (!samples->latency || !samples->seen) {
        release_samples(samples);
        return false;
    }
    prefault(samples->latency, (size_t)expected * sizeof(*samples->latency));
    prefault(samples->seen, (size_t)expected * sizeof(*samples->seen));
    return true;
}

static void release_trial(void)
{
    release_samples(&callbacks);
    release_samples(&raw_poll);
    release_samples(&sdl_events);
    free(source_timestamps);
    free(sdl_receipts);
    source_timestamps = sdl_receipts = NULL;
}

static bool allocate_trial(void)
{
    if (!allocate_samples(&callbacks, config.reports) ||
        !allocate_samples(&raw_poll, config.reports) ||
        !allocate_samples(&sdl_events, config.reports)) {
        release_trial();
        return false;
    }
    source_timestamps = calloc((size_t)config.reports, sizeof(*source_timestamps));
    sdl_receipts = calloc((size_t)config.reports, sizeof(*sdl_receipts));
    if (!source_timestamps || !sdl_receipts) {
        release_trial();
        return false;
    }
    prefault(source_timestamps, (size_t)config.reports * sizeof(*source_timestamps));
    prefault(sdl_receipts, (size_t)config.reports * sizeof(*sdl_receipts));
    return true;
}

static void reset_trial(void)
{
    Samples *sets[] = { &callbacks, &raw_poll, &sdl_events };
    for (int i = 0; i < 3; i++) {
        sets[i]->count = sets[i]->invalid = 0;
        memset(sets[i]->seen, 0, (size_t)sets[i]->expected * sizeof(*sets[i]->seen));
    }
    memset(source_timestamps, 0, (size_t)config.reports * sizeof(*source_timestamps));
    memset(sdl_receipts, 0, (size_t)config.reports * sizeof(*sdl_receipts));
    sdl_received = 0;
    native_callbacks = native_polled = sdl_motions = 0;
    callback_started = raw_started = false;
    atomic_store(&producer_done, false);
}

/* Integer arithmetic avoids drift and remains bounded for all accepted CLI
 * values. Schedule batches against an absolute clock, not relative sleeps. */
static Uint64 report_deadline(Uint64 start, int reports, int rate)
{
    return start + (Uint64)reports * 1000000000ull / (Uint64)rate;
}

static bool sleep_until(Uint64 deadline)
{
    struct timespec target = { (time_t)(deadline / 1000000000ull),
                               (long)(deadline % 1000000000ull) };
    int result;
    do {
        result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL);
    } while (result == EINTR);
    return result == 0;
}

typedef struct Producer
{
    UInputDevice *synth;
    int sent;
    Uint64 start, end;
    Uint64 late_batches, max_lateness;
    bool failed;
} Producer;

static void *produce(void *userdata)
{
    Producer *p = userdata;
    SDL_Delay(20); /* allow the main thread to enter drain/wait */
    p->start = now_ns();
    for (int sent = 0; sent < config.reports;) {
        int batch = SDL_min(config.burst, config.reports - sent);
        if (config.rate) {
            Uint64 deadline = report_deadline(p->start, sent, config.rate);
            Uint64 now = now_ns();
            if (now < deadline && !sleep_until(deadline)) {
                p->failed = true;
                break;
            }
            now = now_ns();
            Uint64 late = now > deadline ? now - deadline : 0;
            Uint64 period = (Uint64)batch * 1000000000ull / (Uint64)config.rate;
            if (late > period) {
                p->late_batches++;
            }
            if (late > p->max_lateness) {
                p->max_lateness = late;
            }
        }
        for (int j = 0; j < batch; j++) {
            /* Count complete writes, not attempted reports. A kernel/pipe
             * transport error must not masquerade as a library ring drop. */
            if (!uinput_synth_rel(p->synth, REL_X, sent + 1) ||
                (config.axes == 2 && !uinput_synth_rel(p->synth, REL_Y, -1)) ||
                !uinput_synth_syn(p->synth)) {
                fprintf(stderr, "input transport failed after %d complete reports: %s\n",
                        sent, strerror(errno));
                p->failed = true;
                break;
            }
            sent++;
        }
        p->sent = sent;
        if (p->failed) {
            break;
        }
    }
    /* Include the final report interval in the requested duration. */
    if (config.rate && !p->failed) {
        Uint64 end = report_deadline(p->start, config.reports, config.rate);
        if (now_ns() < end && !sleep_until(end)) {
            p->failed = true;
        }
    }
    p->end = now_ns();
    atomic_store_explicit(&producer_done, true, memory_order_release);
    return NULL;
}

/* Discover our source by a marker outside the measured sequence range. Do not
 * accidentally benchmark another keyboard/mouse just because it is readable. */
static bool discover(UInputDevice *synth)
{
    const Uint64 deadline = now_ns() + 1000000000ull;
    bool found = false;
    /* Send one frame, then wait for its completion. Reinjecting on every
     * retry can leave extra discovery frames behind if the worker is delayed. */
    if (!uinput_synth_rel(synth, REL_X, DISCOVERY_MARKER) || !uinput_synth_syn(synth)) {
        fprintf(stderr, "discovery transport failed: %s\n", strerror(errno));
        return false;
    }
    do {
        SDL_Delay(5);
        SDL_PumpEvents();
        SDLop_RawEvent events[128];
        int n;
        while ((n = SDLop_PollRawEvents(events, 128)) > 0) {
            for (int i = 0; i < n; i++) {
                if (events[i].type == SDLop_EV_REL && events[i].code == REL_X &&
                    events[i].value == DISCOVERY_MARKER) {
                    device = events[i].device;
                    found = true;
                }
                if (found && events[i].device == device &&
                    events[i].type == SDLop_EV_SYN && events[i].code == SYN_REPORT) {
                    /* The whole discovery frame has now been published to
                     * both rings. Do not leave its SYN behind in an API-cost
                     * trial that expects empty queues. */
                    SDL_PumpEvents();
                    SDL_FlushEvents(0, 0xFFFFFFFFu);
                    return true;
                }
            }
        }
    } while (now_ns() < deadline);
    return false;
}

static void drain_raw(void)
{
    SDLop_RawEvent events[128];
    int n;
    while ((n = SDLop_PollRawEvents(events, 128)) > 0) {
        const Uint64 now = now_ns();
        for (int i = 0; i < n; i++) {
            if (events[i].device != device) {
                continue;
            }
            if (events[i].type == SDLop_EV_REL && events[i].code == REL_X &&
                events[i].value >= 1 && events[i].value <= config.reports) {
                raw_started = true;
                record(&raw_poll, events[i].value, events[i].timestamp_ns, now);
            }
            if (raw_started) {
                native_polled++;
            }
        }
    }
}

static void collect_sdl(const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        sdl_motions++;
    }
    if (ev->type == SDL_EVENT_MOUSE_MOTION && ev->motion.xrel >= 1 &&
        ev->motion.xrel <= config.reports && ev->motion.yrel == 0) {
        int sequence = (int)ev->motion.xrel;
        if (ev->motion.xrel == (float)sequence && !sdl_receipts[sequence - 1]) {
            sdl_receipts[sequence - 1] = now_ns();
            sdl_received++;
        }
    }
}

static void finalize_sdl_samples(void)
{
    for (int i = 0; i < config.reports; i++) {
        if (sdl_receipts[i]) {
            record(&sdl_events, i + 1, source_timestamps[i], sdl_receipts[i]);
        }
    }
}

/* These are direct call costs, not transport or scheduling latency. Taking
 * one clock pair per block amortizes timer overhead; the printed distribution
 * is of block averages, not individual calls. Nothing is subtracted. */
static bool measure_api_overhead(void)
{
    enum { BLOCKS = 1000, CALLS = 1000, WARMUP_BLOCKS = 10 };
    Uint64 elapsed[BLOCKS];
    SDLop_RawEvent raws[128];
    SDL_Event ev, pushed;
    memset(&pushed, 0, sizeof(pushed));
    pushed.type = SDL_RegisterEvents(1);
    if (!pushed.type) {
        return false;
    }
    printf("\nDirect API overhead: ns/call (percentiles of %d block averages, %d calls/block).\n"
           "Includes loop/check costs; timer costs amortized, no baseline subtracted.\n",
           BLOCKS, CALLS);
#define MEASURE_API(label, expression) do { \
        for (int block = -WARMUP_BLOCKS; block < BLOCKS; block++) { \
            Uint64 start = now_ns(); \
            for (int call = 0; call < CALLS; call++) { \
                if (!(expression)) { \
                    fprintf(stderr, "unexpected result while measuring %s\n", label); \
                    return false; \
                } \
            } \
            Uint64 duration = now_ns() - start; \
            if (block >= 0) { elapsed[block] = duration; } \
        } \
        qsort(elapsed, BLOCKS, sizeof(elapsed[0]), cmp_u64); \
        printf("%-32s p50=%7.1f p95=%7.1f p99=%7.1f max=%7.1f\n", label, \
               (double)elapsed[BLOCKS / 2] / CALLS, \
               (double)elapsed[BLOCKS * 95 / 100] / CALLS, \
               (double)elapsed[BLOCKS * 99 / 100] / CALLS, \
               (double)elapsed[BLOCKS - 1] / CALLS); \
    } while (0)

    MEASURE_API("SDLop_PollRawEvents (empty)", SDLop_PollRawEvents(raws, 128) == 0);
    MEASURE_API("SDL_PollEvent (empty)", !SDL_PollEvent(&ev));
    MEASURE_API("SDL_WaitEventTimeout (empty, 0)", !SDL_WaitEventTimeout(&ev, 0));
    MEASURE_API("SDL_PushEvent + SDL_PollEvent", SDL_PushEvent(&pushed) &&
                SDL_PollEvent(&ev) && ev.type == pushed.type);
#undef MEASURE_API
    return true;
}

static bool native_delivery_complete(void)
{
    return native_callbacks == (Uint64)config.reports * (config.axes + 1) &&
           native_polled == (Uint64)config.reports * (config.axes + 1) &&
           sdl_motions == (Uint64)config.reports * config.axes;
}

static bool meets_target_rate(int reports, Uint64 duration_ns, int rate)
{
    return duration_ns && (!rate || (double)reports * 1e9 / duration_ns >= rate * 0.95);
}

static int run(UInputDevice *synth, bool wait, bool api_only)
{
    reset_trial();
    const Uint64 expected_native = (Uint64)config.reports * (config.axes + 1);
    const Uint64 expected_motion = (Uint64)config.reports * config.axes;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDLop_RawInputAvailable()) {
#ifdef SDLOP_BENCH_PIPE
        fprintf(stderr, "bench_raw_latency_pipe: worker setup failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
#else
        printf("bench_raw_latency: SKIP (raw input unavailable: %s)\n", SDL_GetError());
        SDL_Quit();
        return 77;
#endif
    }
    SDL_Window *window = SDL_CreateWindow("raw latency", 320, 200, 0);
    if (!window || !discover(synth) || !SDLop_RegisterRawEventCallback(on_raw, NULL)) {
        fprintf(stderr, "benchmark setup failed (window/device/callback): %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (api_only) {
        bool ok = measure_api_overhead();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return ok ? 0 : 1;
    }

    Producer producer = { .synth = synth };
    pthread_t thread;
    if (pthread_create(&thread, NULL, produce, &producer) != 0) {
        fprintf(stderr, "could not start injection thread\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    Uint64 finished_at = 0;
    for (;;) {
        SDL_Event ev;
        /* Timeout bounds the run even if a ring drops every event. This calls
         * the same implementation as SDL_WaitEvent (which passes -1). */
        if (wait) {
            if (SDL_WaitEventTimeout(&ev, 50)) {
                collect_sdl(&ev);
            }
        } else if (SDL_PollEvent(&ev)) {
            collect_sdl(&ev);
        }
        drain_raw();
        if (atomic_load_explicit(&producer_done, memory_order_acquire)) {
            if (!finished_at) {
                finished_at = now_ns();
            }
            if ((raw_poll.count + raw_poll.invalid == config.reports &&
                 sdl_received == config.reports && native_polled >= expected_native &&
                 sdl_motions >= expected_motion) ||
                now_ns() - finished_at >= 250000000ull) {
                break;
            }
        }
    }
    pthread_join(thread, NULL);
    SDL_DestroyWindow(window);
    /* Unregister alone is not a worker quiescence barrier. Join via Quit
     * before sorting callback-owned memory or starting the next trial. */
    SDL_Quit();
    finalize_sdl_samples();

    double seconds = (double)(producer.end - producer.start) / 1e9;
    printf("\nmode=%s reports=%d target_rate=%d reports/s burst=%d axes=%d\n",
           wait ? "wait" : "poll", config.reports, config.rate, config.burst, config.axes);
    printf("injection: sent=%d/%d duration=%.6fs achieved=%.1f reports/s native_rate=%.1f records/s\n",
           producer.sent, config.reports, seconds, producer.sent / seconds,
           (double)producer.sent * (config.axes + 1) / seconds);
    printf("pacing: late_batches=%llu max_lateness=%.1fus (0 target_rate means saturation)\n",
           (unsigned long long)producer.late_batches, (double)producer.max_lateness / 1e3);
    printf("native records: callback=%llu/%llu raw_poll=%llu/%llu; SDL motions=%llu/%llu\n",
           (unsigned long long)native_callbacks, (unsigned long long)expected_native,
           (unsigned long long)native_polled, (unsigned long long)expected_native,
           (unsigned long long)sdl_motions, (unsigned long long)expected_motion);
    bool complete = report(SOURCE_LABEL " -> worker callback", &callbacks);
    complete = report(SOURCE_LABEL " -> SDLop_PollRawEvents", &raw_poll) && complete;
    complete = report(wait ? SOURCE_LABEL " -> SDL_WaitEventTimeout" : SOURCE_LABEL " -> SDL_PollEvent",
                      &sdl_events) && complete;
    complete = complete && !producer.failed && native_delivery_complete();
    bool rate_met = meets_target_rate(producer.sent, producer.end - producer.start, config.rate);
    printf("target_rate_met=%s (mean rate, 5%% shortfall tolerance; see pacing jitter above)\n",
           rate_met ? "yes" : "no");
    if (!rate_met) {
        fprintf(stderr, "UNDER-RATE mode=%s: injection did not sustain requested mean load; not a valid rate gate\n",
                wait ? "wait" : "poll");
    }
    if (!complete) {
        fprintf(stderr, "INCOMPLETE mode=%s: lost/invalid probes or native records; do not use as a latency gate\n",
                wait ? "wait" : "poll");
    }
    return complete && rate_met ? 0 : 1;
}

static bool parse_uint_option(const char *text, int minimum, int maximum, int *out)
{
    if (!*text || strspn(text, "0123456789") != strlen(text)) {
        return false;
    }
    errno = 0;
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value < (unsigned long)minimum || value > (unsigned long)maximum) {
        return false;
    }
    *out = (int)value;
    return true;
}

int main(int argc, char **argv)
{
    bool stress = false;
    bool api_only = false;
    bool explicit_rate = false, explicit_burst = false;
    const char *mode = "all";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stress")) {
            stress = true;
        } else if (!strcmp(argv[i], "--api-overhead")) {
            api_only = true;
        } else if ((!strcmp(argv[i], "--reports") || !strcmp(argv[i], "--events") ||
                    !strcmp(argv[i], "--rate") ||
                    !strcmp(argv[i], "--burst") || !strcmp(argv[i], "--axes")) && i + 1 < argc) {
            const char *option = argv[i++];
            int *value = NULL, minimum = 1, maximum = MAX_REPORTS;
            if (!strcmp(option, "--reports") || !strcmp(option, "--events")) {
                value = &config.reports;
            } else if (!strcmp(option, "--rate")) {
                value = &config.rate;
                minimum = 0;
                maximum = 1000000;
                explicit_rate = true;
            } else if (!strcmp(option, "--burst")) {
                value = &config.burst;
                maximum = 4096;
                explicit_burst = true;
            } else {
                value = &config.axes;
                maximum = 2;
            }
            if (!parse_uint_option(argv[i], minimum, maximum, value)) {
                fprintf(stderr, "%s must be an integer in [%d, %d]\n", option, minimum, maximum);
                return 1;
            }
        } else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            mode = argv[++i];
            if (!strcmp(mode, "poll") || !strcmp(mode, "wait") || !strcmp(mode, "all")) {
                continue;
            }
            fprintf(stderr, "invalid mode: %s\n", mode);
            return 1;
        } else {
            bool help = !strcmp(argv[i], "--help");
            fprintf(help ? stdout : stderr,
                    "Usage: %s [--mode all|poll|wait] [--api-overhead] [--stress]\n"
                    "  --reports N report/probe count, 1..4000000 (default 160000; --events is an alias)\n"
                    "  --rate Hz   reports/second, 0..1000000; 0=saturation (default 16000)\n"
                    "  --burst N   reports per batch, 1..4096 (default 1)\n"
                    "  --axes 1|2  REL axes per report before SYN_REPORT (default 2)\n"
                    "  --stress    defaults to --rate 0 --burst 64 unless explicitly set\n",
                    argv[0]);
            return help ? 0 : 1;
        }
    }
    if (stress) {
        if (!explicit_rate) { config.rate = 0; }
        if (!explicit_burst) { config.burst = 64; }
    }
    if (!allocate_trial()) {
        fprintf(stderr, "could not allocate sample buffers for %d reports\n", config.reports);
        return 1;
    }
    setenv("SDL_VIDEODRIVER", "dummy", 1);
#ifdef SDLOP_BENCH_PIPE
    /* This controlled benchmark must exercise the worker, not accidentally
     * measure its disabled/error path because of the caller's environment. */
    setenv("SDLOP_RAW_INPUT", "1", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "0", 1);
#endif
    UInputDevice *synth = uinput_synth_create("sdlop-latency");
    if (!synth) {
        release_trial();
#ifdef SDLOP_BENCH_PIPE
        perror("bench_raw_latency_pipe: transport setup failed");
        return 1;
#else
        printf("bench_raw_latency: SKIP (/dev/uinput not accessible)\n");
        return 77;
#endif
    }
#ifdef SDLOP_BENCH_PIPE
    printf("ROOTLESS userspace pipe transport: real SDLop worker/APIs, NO evdev or hardware timing.\n");
#else
    printf("Kernel event timestamp -> consumer. Keep physical input idle.\n");
#endif
    int result = 0;
    if (api_only) {
        result = run(synth, false, true);
    } else {
        printf("Latency in microseconds. Wait uses a 50ms timeout; raw polling there includes sleep.\n");
        if (strcmp(mode, "wait")) {
            result = run(synth, false, false);
        }
        if (result != 77 && strcmp(mode, "poll")) {
            /* An overload in poll mode must not hide the independent wait
             * trial. Preserve any failure while still collecting both. */
            int wait_result = run(synth, true, false);
            if (wait_result) { result = wait_result; }
        }
    }
    uinput_synth_destroy(synth);
    release_trial();
    return result;
}
