/* Direct regression of the real worker drain, no production hooks/devices.
 * Complements the threaded worker tests with exact read/service boundaries.
 * zlib license, see LICENSE. */
#include "../src/internal/sdlop_evdev.c"
#include <assert.h>
#include <stdio.h>

#define TEST_FD (-123)
#define SDLOP_MAX_EVENTS_PER_SERVICE (SDLOP_MAX_READS_PER_DEVICE * SDLOP_MAX_EVENTS_PER_READ)
_Static_assert(SDLOP_MAX_EVENTS_PER_SERVICE == 2048, "preserve the existing service bound");
enum Mode { FULL, SHORT, INTERRUPT, FINITE, SCANS, END, ERROR };
static enum Mode mode;
static unsigned calls, native_count, callbacks;

extern ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *buffer, size_t bytes)
{
    if (fd != TEST_FD) { return __real_read(fd, buffer, bytes); }
    assert(bytes && bytes % sizeof(struct input_event) == 0);
    assert(bytes <= SDLOP_MAX_EVENTS_PER_READ * sizeof(struct input_event));
    calls++;
    if (mode == INTERRUPT) { errno = EINTR; return -1; }
    if (mode == END) { return 0; }
    if (mode == ERROR) { errno = EIO; return -1; }
    if (mode == FINITE && native_count == 300) { errno = EAGAIN; return -1; }
    unsigned n = (unsigned)(bytes / sizeof(struct input_event));
    if (mode == SHORT) { n = 1; }
    if (mode == FINITE && n > 300 - native_count) { n = 300 - native_count; }
    struct input_event *events = buffer;
    for (unsigned i = 0; i < n; i++) {
        events[i] = (struct input_event){ 0 };
        events[i].input_event_sec = 1;
        events[i].input_event_usec = 234;
        events[i].type = EV_REL;
        events[i].code = REL_X;
        events[i].value = (int)++native_count;
        if (mode == SCANS && native_count % 4 == 0) {
            events[i].type = EV_MSC;
            events[i].code = MSC_SCAN;
        }
    }
    return (ssize_t)(n * sizeof(*events));
}

static void callback(const SDLop_RawEvent *event, void *userdata)
{
    (void)userdata;
    assert(event->device == 7 && event->type == EV_REL && event->code == REL_X);
    assert(event->timestamp_ns == 1000234000ull);
    SDLop_RawInputStats stats;
    assert(SDLop_GetRawEventCount() == callbacks);
    assert(SDLop_GetRawInputStats(&stats) && stats.events_seen == callbacks);
    /* Both rings must already expose this record, even inside the callback. */
    assert(SDLOP_RingCount(&raw.sdl_ring) == callbacks + 1);
    assert(SDLOP_RingCount(&raw.poll_ring) == callbacks + 1);
    callbacks++;
}

int main(void)
{
    SDLOP_CallbackSet set = { .count = 1, .entries = { { callback, NULL } } };
    raw.callbacks = &set;
    SDLOP_RawDevice dev = { .fd = TEST_FD, .index = 7, .clock_monotonic = true };
    struct input_event batch[SDLOP_MAX_EVENTS_PER_READ];
    for (mode = FULL; mode <= ERROR; mode++) {
        calls = native_count = callbacks = 0;
        atomic_store(&raw.event_count, 0);
        SDLOP_RingInit(&raw.sdl_ring, raw.sdl_ring_storage, SDLOP_SDL_RING_SIZE, sizeof(SDLop_RawEvent));
        SDLOP_RingInit(&raw.poll_ring, raw.poll_ring_storage, SDLOP_POLL_RING_SIZE, sizeof(SDLop_RawEvent));
        bool drained = drain_device_events(&dev, batch, 2000000000ull, 0);
        assert(drained == (mode == FINITE)); /* budgets/EOF/errors aren't EAGAIN */
        assert(calls <= SDLOP_MAX_READS_PER_DEVICE);
        if (mode == FULL || mode == SCANS) {
            assert(native_count == SDLOP_MAX_EVENTS_PER_SERVICE);
            assert(calls == (SDLOP_MAX_EVENTS_PER_SERVICE + SDLOP_MAX_EVENTS_PER_READ - 1) / SDLOP_MAX_EVENTS_PER_READ);
        }
        if (mode == SHORT) { assert(calls == 32 && native_count == 32); }
        if (mode == INTERRUPT) { assert(calls == 32 && !native_count); }
        if (mode == FINITE) { assert(native_count == 300); }
        if (mode == END || mode == ERROR) { assert(calls == 1 && !native_count); }
        assert(SDLop_GetRawEventCount() == callbacks);
        assert(callbacks == native_count - (mode == SCANS ? native_count / 4 : 0));
        assert(!raw.sdl_ring.dropped && !raw.poll_ring.dropped);
        for (unsigned i = 1; i <= native_count; i++) {
            if (mode == SCANS && i % 4 == 0) { continue; }
            SDLop_RawEvent a, b;
            assert(SDLOP_RingPop(&raw.sdl_ring, &a) && SDLOP_RingPop(&raw.poll_ring, &b));
            assert(a.value == (int)i && b.value == (int)i);
            assert(a.timestamp_ns == b.timestamp_ns && a.device == b.device);
        }
        assert(SDLOP_RingEmpty(&raw.sdl_ring) && SDLOP_RingEmpty(&raw.poll_ring));
    }
    puts("test_evdev_dispatch: PASS (2048-record fairness, short/EINTR limits, EAGAIN boundary, ordering/publication/counts)");
    return 0;
}
