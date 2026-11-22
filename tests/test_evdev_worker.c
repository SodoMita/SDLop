/*
  Real-worker wakeup/clock contracts, using process-local pipe evdev and
  linker wrappers to force the consumer-drains-mid-batch interleaving.
  No production hooks, devices, privileges or display server required.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include "internal/sdlop_internal.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/epoll.h>

/* Reuse discovery/transport, but intercept clock setup in this test. */
#define __wrap_ioctl pipe_ioctl
#include "../bench/raw_pipe.c"
#undef __wrap_ioctl

static bool accept_clock;
static int clock_requests;
static atomic_int records;
static Uint64 callback_stamp;
static pthread_t main_thread;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int epoll_entries;
static bool pause_read, read_paused, release_on_poll;
static int poll_woke;
static bool interrupt_next_wake;
static int interrupted_writes;
static atomic_int reads_before_clock_setup;
static atomic_bool continuously_readable;
static bool interrupt_shutdown;
static int shutdown_interrupted;

static void wait_locked(bool *condition)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    while (!*condition) {
        assert(pthread_cond_timedwait(&gate_cond, &gate_lock, &deadline) == 0);
    }
}

static void wait_epoll_locked(int target)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    while (epoll_entries < target) {
        assert(pthread_cond_timedwait(&gate_cond, &gate_lock, &deadline) == 0);
    }
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (synthetic_fd(fd) && request == EVIOCSCLOCKID) {
        assert(*(int *)arg == CLOCK_MONOTONIC);
        clock_requests++;
        if (accept_clock) { return 0; }
        errno = EINVAL;
        return -1;
    }
    return pipe_ioctl(fd, request, arg);
}

extern int __real_epoll_wait(int, struct epoll_event *, int, int);
int __wrap_epoll_wait(int fd, struct epoll_event *events, int count, int timeout)
{
    pthread_mutex_lock(&gate_lock);
    epoll_entries++;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
    return __real_epoll_wait(fd, events, count, timeout);
}

extern ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
    if (!pthread_equal(pthread_self(), main_thread) && synthetic_fd(fd)) {
        if (clock_requests != 1) { atomic_fetch_add(&reads_before_clock_setup, 1); }
        pthread_mutex_lock(&gate_lock);
        if (pause_read) {
            read_paused = true;
            pthread_cond_broadcast(&gate_cond);
            while (pause_read) { assert(pthread_cond_wait(&gate_cond, &gate_lock) == 0); }
            read_paused = false;
        }
        pthread_mutex_unlock(&gate_lock);
        if (atomic_load_explicit(&continuously_readable, memory_order_acquire)) {
            /* Keep returning full native reads until main releases the source.
             * A worker that drains forever cannot signal or service shutdown. */
            memset(buffer, 0, size);
            return size; /* EV_SYN/SYN_REPORT records */
        }
    }
    return __real_read(fd, buffer, size);
}

extern ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *buffer, size_t size)
{
    if (pthread_equal(pthread_self(), main_thread) && interrupt_shutdown &&
        fd != sdlop.wake_fd && active && fd != active->write_fd) {
        interrupt_shutdown = false;
        shutdown_interrupted++;
        errno = EINTR;
        return -1;
    }
    if (!pthread_equal(pthread_self(), main_thread) && fd == sdlop.wake_fd) {
        pthread_mutex_lock(&gate_lock);
        bool interrupt = interrupt_next_wake;
        interrupt_next_wake = false;
        if (interrupt) { interrupted_writes++; }
        pthread_mutex_unlock(&gate_lock);
        if (interrupt) {
            errno = EINTR;
            return -1;
        }
    }
    return __real_write(fd, buffer, size);
}

extern int __real_poll(struct pollfd *, nfds_t, int);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    bool capture = false;
    if (pthread_equal(pthread_self(), main_thread)) {
        pthread_mutex_lock(&gate_lock);
        if (release_on_poll) {
            /* SDL_WaitEvent has just pumped an empty ring. Release the
             * producer, then let it finish its epoll batch before entering
             * poll. Readiness is deterministic, not a sub-ms timing guess. */
            int next = epoll_entries + 1;
            release_on_poll = false;
            pause_read = false;
            pthread_cond_broadcast(&gate_cond);
            wait_epoll_locked(next);
            capture = true;
        }
        pthread_mutex_unlock(&gate_lock);
    }
    int result = __real_poll(fds, count, timeout);
    if (capture) {
        for (nfds_t i = 0; i < count; i++) {
            if (fds[i].fd == sdlop.wake_fd && (fds[i].revents & POLLIN)) { poll_woke++; }
        }
    }
    return result;
}

static Uint64 clock_ns(clockid_t clock)
{
    struct timespec now;
    assert(clock_gettime(clock, &now) == 0);
    return (Uint64)now.tv_sec * 1000000000ull + (Uint64)now.tv_nsec;
}

static void on_raw(const SDLop_RawEvent *event, void *userdata)
{
    (void)userdata;
    callback_stamp = event->timestamp_ns;
    atomic_fetch_add_explicit(&records, 1, memory_order_release);
}

static void clear_wake(void)
{
    uint64_t count;
    while (read(sdlop.wake_fd, &count, sizeof(count)) == sizeof(count)) { }
    assert(errno == EAGAIN);
}

static void drain_raw(void)
{
    SDLop_RawEvent events[32];
    while (SDLop_PollRawEvents(events, 32) > 0) { }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    bool wake_test = !strcmp(argv[1], "wake");
    bool busy_test = !strcmp(argv[1], "busy");
    bool realtime = !strcmp(argv[1], "fallback-realtime");
    bool future = !strcmp(argv[1], "negotiated-authoritative");
    accept_clock = wake_test || busy_test || future || !strcmp(argv[1], "monotonic");
    assert(wake_test || busy_test || realtime || future || !strcmp(argv[1], "monotonic") ||
           !strcmp(argv[1], "fallback-monotonic"));
    main_thread = pthread_self();
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_RAW_INPUT", "1", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "0", 1);
    UInputDevice *device = uinput_synth_create("worker-contract");
    assert(device && SDL_Init(SDL_INIT_VIDEO));
    assert(SDLop_RawInputAvailable());
    assert(SDL_CreateWindow("worker", 320, 200, 0));
    assert(SDLop_RegisterRawEventCallback(on_raw, NULL));
    SDL_FlushEvents(0, 0xFFFFFFFFu);
    pthread_mutex_lock(&gate_lock);
    wait_epoll_locked(1);
    pthread_mutex_unlock(&gate_lock);

    int failures = 0;
    if (busy_test) {
        clear_wake();
        atomic_store_explicit(&continuously_readable, true, memory_order_release);
        assert(uinput_synth_rel(device, REL_X, 1));
        struct pollfd fd = { .fd = sdlop.wake_fd, .events = POLLIN };
        int ready = __real_poll(&fd, 1, 100);
        bool notified = ready > 0 && (fd.revents & POLLIN);
        /* Keep the source busy through SDL_Quit to also exercise bounded
         * shutdown. Release it on the old-library failure path for a clean repro. */
        if (!notified) { atomic_store_explicit(&continuously_readable, false, memory_order_release); }
        printf("continuously readable fd: notification_before_EAGAIN=%s\n", notified ? "yes" : "no");
        if (!notified) { failures++; }
    } else if (wake_test) {
        enum { REPEATS = 100 };
        Uint64 times[REPEATS];
        for (int i = 0; i < REPEATS; i++) {
            /* Leave an old record in the ring before the next epoll batch. */
            pthread_mutex_lock(&gate_lock);
            int next = epoll_entries + 1;
            uinput_synth_rel(device, REL_X, 1);
            wait_epoll_locked(next);
            pause_read = true;
            uinput_synth_rel(device, REL_X, 2);
            wait_locked(&read_paused);
            pthread_mutex_unlock(&gate_lock);

            SDL_Event event;
            assert(SDL_PollEvent(&event) && event.type == SDL_EVENT_MOUSE_MOTION && event.motion.xrel == 1);
            assert(!SDL_PollEvent(&event));
            drain_raw();
            clear_wake();
            pthread_mutex_lock(&gate_lock);
            release_on_poll = true;
            interrupt_next_wake = (i == 0); /* retry the first notification */
            pthread_mutex_unlock(&gate_lock);
            Uint64 start = clock_ns(CLOCK_MONOTONIC);
            assert(SDL_WaitEventTimeout(&event, 50));
            times[i] = clock_ns(CLOCK_MONOTONIC) - start;
            assert(event.type == SDL_EVENT_MOUSE_MOTION && event.motion.xrel == 2);
            drain_raw();
            clear_wake();
        }
        Uint64 max = 0, sum = 0;
        for (int i = 0; i < REPEATS; i++) { sum += times[i]; if (times[i] > max) { max = times[i]; } }
        printf("forced mid-batch drain: eventfd wakes=%d/%d SDL_WaitEvent avg=%.1fus max=%.1fus\n",
               poll_woke, REPEATS, (double)sum / REPEATS / 1000., (double)max / 1000.);
        if (poll_woke != REPEATS || interrupted_writes != 1) { failures++; }
        /* No new input, no producer notification or periodic wake writes. */
        struct pollfd fd = { .fd = sdlop.wake_fd, .events = POLLIN };
        assert(__real_poll(&fd, 1, 20) == 0);
        assert(atomic_load_explicit(&records, memory_order_acquire) == REPEATS * 2);
    } else {
        Uint64 expected = clock_ns(CLOCK_MONOTONIC);
        /* Artificial realtime-looking value with negotiated monotonic clock:
         * verifies that success bypasses heuristics, not just that ordinary
         * monotonic numbers happen to survive the existing heuristic. */
        Uint64 stamp = (realtime || future) ? clock_ns(CLOCK_REALTIME) : expected;
        struct input_event native;
        memset(&native, 0, sizeof(native));
        native.type = EV_REL;
        native.code = REL_X;
        native.value = 7;
        native.input_event_sec = stamp / 1000000000ull;
        native.input_event_usec = stamp % 1000000000ull / 1000;
        stamp = stamp / 1000 * 1000;
        pthread_mutex_lock(&gate_lock);
        int next = epoll_entries + 1;
        assert(write(device->write_fd, &native, sizeof(native)) == sizeof(native));
        wait_epoll_locked(next);
        pthread_mutex_unlock(&gate_lock);
        assert(atomic_load_explicit(&records, memory_order_acquire) == 1);
        SDLop_RawEvent raw;
        assert(SDLop_PollRawEvents(&raw, 1) == 1);
        assert(raw.timestamp_ns == callback_stamp);
        bool stamp_ok = realtime ? (raw.timestamp_ns > expected - 5000000ull && raw.timestamp_ns < expected + 5000000ull)
                                 : raw.timestamp_ns == stamp;
        printf("clock mode=%s setup_requests=%d callback/poll_stamp=%llu timestamp_ok=%s\n",
               argv[1], clock_requests, (unsigned long long)raw.timestamp_ns, stamp_ok ? "yes" : "no");
        if (clock_requests != 1 || !stamp_ok || atomic_load(&reads_before_clock_setup)) { failures++; }
    }
    interrupt_shutdown = busy_test; /* first control write must retry EINTR */
    SDL_Quit();
    if (busy_test) { assert(shutdown_interrupted == 1); }
    uinput_synth_destroy(device);
    printf("test_evdev_worker %s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
