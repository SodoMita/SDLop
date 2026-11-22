/*
  Rootless CPU-work benchmark for the real native publication/callback loop.
  Includes the private implementation so no production testing hooks are needed.
  A fake read returns full batches and simulates immediate consumer reclamation;
  this deliberately EXCLUDES syscalls, scheduling and cross-core contention.
  Results are block-average CPU work, NOT input latency or a replacement for
  bench_raw_latency_pipe's concurrent-consumer regression gate.

  zlib license, see LICENSE.
*/
#ifndef SDLOP_EVDEV_SOURCE
#define SDLOP_EVDEV_SOURCE "../src/internal/sdlop_evdev.c"
#endif
#include SDLOP_EVDEV_SOURCE
#include <assert.h>
#include <stdio.h>
#include <time.h>

#define FAKE_FD (-123)
#define BLOCKS 1000
#define PASSES 10

static struct input_event native[SDLOP_MAX_EVENTS_PER_READ];
static uint64_t callback_sum;

extern ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
    if (fd != FAKE_FD) { return __real_read(fd, buffer, size); }
    assert(size == sizeof(native));
    /* The simulated consumers reclaim the previous read's publications. */
    atomic_store_explicit(&raw.sdl_ring.tail, atomic_load_explicit(&raw.sdl_ring.head, memory_order_relaxed), memory_order_release);
    atomic_store_explicit(&raw.poll_ring.tail, atomic_load_explicit(&raw.poll_ring.head, memory_order_relaxed), memory_order_release);
    memcpy(buffer, native, sizeof(native));
    return sizeof(native);
}

static void callback(const SDLop_RawEvent *event, void *userdata)
{
    (void)userdata;
    callback_sum += (uint64_t)event->value;
}

static Uint64 now(void)
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
    SDLOP_RingInit(&raw.sdl_ring, raw.sdl_ring_storage, SDLOP_SDL_RING_SIZE, sizeof(SDLop_RawEvent));
    SDLOP_RingInit(&raw.poll_ring, raw.poll_ring_storage, SDLOP_POLL_RING_SIZE, sizeof(SDLop_RawEvent));
    SDLOP_CallbackSet callbacks = { .count = 1, .entries = { { callback, NULL } } };
    raw.callbacks = &callbacks;
    SDLOP_RawDevice dev = { .fd = FAKE_FD, .clock_monotonic = true };
    for (int i = 0; i < SDLOP_MAX_EVENTS_PER_READ; i++) {
        native[i].type = EV_REL;
        native[i].code = REL_X;
        native[i].value = 1;
        native[i].input_event_sec = 1;
    }
    struct input_event batch[SDLOP_MAX_EVENTS_PER_READ];
    Uint64 samples[BLOCKS];
    const Uint64 per_block = PASSES * SDLOP_MAX_READS_PER_DEVICE * SDLOP_MAX_EVENTS_PER_READ;
    for (int i = 0; i < BLOCKS; i++) {
        Uint64 start = now();
        for (int j = 0; j < PASSES; j++) {
            assert(!drain_device_events(&dev, batch, 1000000000ull, 0));
        }
        samples[i] = now() - start;
    }
    assert(SDLop_GetRawEventCount() == BLOCKS * per_block);
    assert(callback_sum == BLOCKS * per_block);
    assert(!raw.sdl_ring.dropped && !raw.poll_ring.dropped);
    qsort(samples, BLOCKS, sizeof(*samples), compare);
    printf("Native publication + callback CPU-work proxy, %llu events, no drops\n", (unsigned long long)(BLOCKS * per_block));
    printf("ns/event, percentiles of %llu-event block averages: p50=%.2f p95=%.2f p99=%.2f max=%.2f\n",
           (unsigned long long)per_block, (double)samples[BLOCKS / 2] / per_block,
           (double)samples[BLOCKS * 95 / 100] / per_block,
           (double)samples[BLOCKS * 99 / 100] / per_block,
           (double)samples[BLOCKS - 1] / per_block);
    return 0;
}
