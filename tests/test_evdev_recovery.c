/*
  Real-worker SDL recovery and public drop-statistics contracts. Two private
  pipe devices and mocked kernel EVIOCGKEY state; no production hooks/root.
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

#define __wrap_ioctl pipe_ioctl
#define __wrap_open pipe_open
#define __wrap_open64 pipe_open64
#include "../bench/raw_pipe.c"
#undef __wrap_ioctl
#undef __wrap_open
#undef __wrap_open64

#define WORDS (KEY_MAX / (8 * sizeof(unsigned long)) + 1)
static UInputDevice second;
static unsigned long kernel_keys[2][WORDS];
static pthread_mutex_t kernel_lock = PTHREAD_MUTEX_INITIALIZER;
static bool fail_query, interrupt_query;
static atomic_int callbacks, queries, idle_records, idle_queries;
static atomic_int raw_markers, callback_stats_errors;
static int sent;
static int nested_pumps;
static bool reentering, saw_reordered_motion;

static bool reentrant_watch(void *userdata, SDL_Event *event)
{
    (void)userdata;
    if (!reentering && (event->type == SDL_EVENT_KEY_UP ||
                       event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)) {
        reentering = true;
        bool had_motion = SDL_HasEvent(SDL_EVENT_MOUSE_MOTION);
        SDL_PumpEvents();
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && !had_motion &&
            SDL_HasEvent(SDL_EVENT_MOUSE_MOTION)) {
            saw_reordered_motion = true;
        }
        nested_pumps++;
        reentering = false;
    }
    return true;
}

static bool second_fd(int fd)
{
    struct stat a, b;
    return second.read_fd >= 0 && fstat(fd, &a) == 0 && fstat(second.read_fd, &b) == 0 &&
           a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

static int open_device(const char *path, int flags, mode_t mode, bool large)
{
    if (active && !strcmp(path, "/dev/input/event1")) {
        return fcntl(second.read_fd, F_DUPFD_CLOEXEC, 0);
    }
    return large ? pipe_open64(path, flags, mode) : pipe_open(path, flags, mode);
}

#define OPEN_WRAPPER(name, large) \
int name(const char *path, int flags, ...) \
{ \
    mode_t mode = 0; \
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) { \
        va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args); \
    } \
    return open_device(path, flags, mode, large); \
}
OPEN_WRAPPER(__wrap_open, false)
OPEN_WRAPPER(__wrap_open64, true)

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    bool other = second_fd(fd);
    if ((other || synthetic_fd(fd)) && _IOC_TYPE(request) == 'E' && _IOC_NR(request) == 0x18) {
        pthread_mutex_lock(&kernel_lock);
        bool fail = fail_query;
        if (interrupt_query) {
            interrupt_query = false;
            pthread_mutex_unlock(&kernel_lock);
            errno = EINTR;
            return -1;
        }
        if (!fail) {
            assert(_IOC_SIZE(request) == sizeof(kernel_keys[0]));
            memcpy(arg, kernel_keys[other ? 1 : 0], sizeof(kernel_keys[0]));
        }
        pthread_mutex_unlock(&kernel_lock);
        atomic_fetch_add(&queries, 1);
        if (fail) { errno = EIO; return -1; }
        return sizeof(kernel_keys[0]);
    }
    return pipe_ioctl(other ? active->read_fd : fd, request, arg);
}

extern int __real_epoll_wait(int, struct epoll_event *, int, int);
int __wrap_epoll_wait(int fd, struct epoll_event *events, int count, int timeout)
{
    /* Entry means prior batch, callbacks, query publication and stats stores
     * have all completed. Main waits on these instead of arbitrary sleeps. */
    atomic_store_explicit(&idle_records, atomic_load(&callbacks), memory_order_release);
    atomic_store_explicit(&idle_queries, atomic_load(&queries), memory_order_release);
    return __real_epoll_wait(fd, events, count, timeout);
}

static void on_raw(const SDLop_RawEvent *event, void *userdata)
{
    (void)userdata;
    SDLop_RawInputStats stats;
    if (!SDLop_GetRawInputStats(&stats) ||
        stats.version != SDLOP_RAW_INPUT_STATS_VERSION || stats.size != sizeof(stats)) {
        atomic_fetch_add(&callback_stats_errors, 1);
    }
    if (event->type == EV_SYN && event->code == SYN_DROPPED) { atomic_fetch_add(&raw_markers, 1); }
    atomic_fetch_add(&callbacks, 1);
}

static void key_state(int device, unsigned int code, bool down)
{
    unsigned int word = code / (8 * sizeof(unsigned long));
    unsigned long mask = 1ul << (code % (8 * sizeof(unsigned long)));
    pthread_mutex_lock(&kernel_lock);
    if (down) { kernel_keys[device][word] |= mask; } else { kernel_keys[device][word] &= ~mask; }
    pthread_mutex_unlock(&kernel_lock);
}

static void send_event(int device, unsigned short type, unsigned short code, int value)
{
    assert(emit(device ? &second : active, type, code, value));
    sent++;
}

static void wait_idle(void)
{
    Uint64 deadline = SDL_GetTicksNS() + 3000000000ull;
    while (atomic_load_explicit(&idle_records, memory_order_acquire) < sent) {
        assert(SDL_GetTicksNS() < deadline);
        SDL_Delay(1);
    }
}

static void recover_to(int expected_queries)
{
    Uint64 deadline = SDL_GetTicksNS() + 3000000000ull;
    while (atomic_load_explicit(&idle_queries, memory_order_acquire) < expected_queries) {
        assert(SDL_GetTicksNS() < deadline);
        SDL_PumpEvents();
        SDL_Delay(1);
    }
    SDL_PumpEvents();
}

static void dropped(int device)
{
    send_event(device, EV_SYN, SYN_DROPPED, 0);
    send_event(device, EV_SYN, SYN_REPORT, 0);
    wait_idle();
}

typedef struct Seen {
    int keydown[SDL_SCANCODE_COUNT], keyup[SDL_SCANCODE_COUNT];
    int down, up, motions, wheels;
    float wheel_y;
} Seen;

static Seen collect(void)
{
    Seen seen = { 0 };
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_KEY_DOWN) { seen.keydown[event.key.scancode]++; }
        if (event.type == SDL_EVENT_KEY_UP) { seen.keyup[event.key.scancode]++; }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { seen.down++; }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) { seen.up++; }
        if (event.type == SDL_EVENT_MOUSE_MOTION) { seen.motions++; }
        if (event.type == SDL_EVENT_MOUSE_WHEEL) { seen.wheels++; seen.wheel_y += event.wheel.y; }
    }
    return seen;
}

static int drain_raw(void)
{
    SDLop_RawEvent events[127];
    int n, total = 0;
    while ((n = SDLop_PollRawEvents(events, 127)) > 0) { total += n; }
    return total;
}

int main(void)
{
    second.read_fd = second.write_fd = -1;
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_RAW_INPUT", "1", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "0", 1);
    setenv("SDLOP_LIB_XKB", "none", 1);
    SDLop_RawInputStats initial;
    assert(SDLop_GetRawInputStats(&initial));
    assert(initial.version == SDLOP_RAW_INPUT_STATS_VERSION && initial.size == sizeof(initial));
    assert(!initial.events_seen && !initial.sdl_ring_dropped && !initial.poll_ring_dropped);
    assert(!SDLop_GetRawInputStats(NULL) && *SDL_GetError());
    SDL_ClearError();
    UInputDevice *first = uinput_synth_create("recovery");
    assert(first);
    int fds[2];
    assert(pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
    second.read_fd = fds[0]; second.write_fd = fds[1];
    char path[128];
    snprintf(path, sizeof(path), "%s/event1", first->directory);
    int file = __real_open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    assert(file >= 0); close(file);
    assert(SDL_Init(SDL_INIT_VIDEO));
    assert(SDL_CreateWindow("recovery", 320, 200, 0));
    /* Test keyboard focus gating using the dummy backend's focused window,
     * without claiming ownership of a real console or compositor. */
    SDLOP_RawInputQuit();
    sdlop.video->window_system_input = true;
    assert(SDLOP_RawInputInit());
    assert(SDLop_RegisterRawEventCallback(on_raw, NULL));
    SDL_FlushEvents(0, 0xFFFFFFFFu);

    assert(SDL_AddEventWatch(reentrant_watch, NULL));
    key_state(0, KEY_A, true); key_state(0, BTN_LEFT, true);
    send_event(0, EV_KEY, KEY_A, 1); send_event(0, EV_KEY, BTN_LEFT, 1);
    send_event(0, EV_REL, REL_X, 5);
    send_event(0, EV_SYN, SYN_REPORT, 0); wait_idle();
    Seen seen = collect();
    assert(seen.keydown[SDL_SCANCODE_A] == 1 && seen.down == 1 && seen.motions == 1);
    assert(!saw_reordered_motion);
    puts("PASS: nested SDL_PumpEvents cannot reorder a pending motion ahead of its button");

    /* Damage a partial wheel frame; invalid packet keys/motion must not leak.
     * The kernel snapshot contains D down, for which no native down was seen. */
    pthread_mutex_lock(&kernel_lock); interrupt_query = true; pthread_mutex_unlock(&kernel_lock);
    send_event(0, EV_REL, REL_WHEEL_HI_RES, 60);
    send_event(0, EV_SYN, SYN_DROPPED, 0);
    send_event(0, EV_REL, REL_X, 777);
    send_event(0, EV_KEY, KEY_W, 1);
    send_event(0, EV_KEY, KEY_A, 0);
    send_event(0, EV_KEY, BTN_LEFT, 0);
    key_state(0, KEY_A, false); key_state(0, BTN_LEFT, false); key_state(0, KEY_D, true);
    send_event(0, EV_SYN, SYN_REPORT, 0); wait_idle();
    recover_to(1); seen = collect();
    assert(seen.keyup[SDL_SCANCODE_A] == 1 && seen.up == 1);
    assert(seen.keydown[SDL_SCANCODE_D] == 1 && !seen.keydown[SDL_SCANCODE_W]);
    assert(!seen.motions && !seen.wheels);
    assert(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A] && SDL_GetKeyboardState(NULL)[SDL_SCANCODE_D]);
    assert(!(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK));
    assert(drain_raw() == sent); /* no synthesized raw D down */
    send_event(0, EV_REL, REL_WHEEL_HI_RES, 60); send_event(0, EV_SYN, SYN_REPORT, 0);
    wait_idle(); seen = collect();
    assert(seen.wheels == 1 && seen.wheel_y == 0.5f);
    puts("PASS: SYN_DROPPED discard, key/button diff, partial-wheel reset, native raw stream");

    /* Two keyboards: recovering device 0 must not release device 1's D. */
    key_state(1, KEY_D, true); send_event(1, EV_KEY, KEY_D, 1); wait_idle();
    seen = collect(); assert(!seen.keydown[SDL_SCANCODE_D]);
    key_state(0, KEY_D, false); dropped(0); recover_to(2); seen = collect();
    assert(!seen.keyup[SDL_SCANCODE_D] && SDL_GetKeyboardState(NULL)[SDL_SCANCODE_D]);
    key_state(1, KEY_D, false); send_event(1, EV_KEY, KEY_D, 0); wait_idle();
    seen = collect(); assert(seen.keyup[SDL_SCANCODE_D] == 1);
    puts("PASS: per-device recovery preserves another keyboard's held key");

    /* A press filtered on an unfocused device must not suppress a fresh
     * focused press on the other keyboard. Still keep union release rules. */
    SDL_Window *focus = sdlop.keyboard_focus;
    sdlop.keyboard_focus = NULL;
    key_state(0, KEY_A, true); send_event(0, EV_KEY, KEY_A, 1); wait_idle();
    seen = collect(); assert(!seen.keydown[SDL_SCANCODE_A]);
    sdlop.keyboard_focus = focus;
    key_state(1, KEY_A, true); send_event(1, EV_KEY, KEY_A, 1); wait_idle();
    seen = collect(); assert(seen.keydown[SDL_SCANCODE_A] == 1);
    key_state(0, KEY_A, false); send_event(0, EV_KEY, KEY_A, 0); wait_idle();
    seen = collect(); assert(!seen.keyup[SDL_SCANCODE_A]);
    key_state(1, KEY_A, false); send_event(1, EV_KEY, KEY_A, 0); wait_idle();
    seen = collect(); assert(seen.keyup[SDL_SCANCODE_A] == 1);
    puts("PASS: focus-filtered keys do not hide another keyboard's fresh press");

    /* Query failure is not an all-up state. It must retry without new input. */
    key_state(0, KEY_A, true); send_event(0, EV_KEY, KEY_A, 1); wait_idle(); collect();
    pthread_mutex_lock(&kernel_lock); fail_query = true; pthread_mutex_unlock(&kernel_lock);
    dropped(0); recover_to(3); seen = collect();
    assert(!seen.keyup[SDL_SCANCODE_A] && SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A]);
    send_event(0, EV_REL, REL_X, 999); send_event(0, EV_SYN, SYN_REPORT, 0); wait_idle();
    seen = collect(); assert(!seen.motions);
    key_state(0, KEY_A, false);
    pthread_mutex_lock(&kernel_lock); fail_query = false; pthread_mutex_unlock(&kernel_lock);
    recover_to(4); seen = collect(); assert(seen.keyup[SDL_SCANCODE_A] == 1);
    puts("PASS: failed query preserves state and retries on later pumps");

    /* State recovery must finish even with no queue capacity. The state query
     * reflects the snapshot immediately; its SDL transition notifications may
     * be dropped rather than replayed later as a stale backlog. */
    key_state(0, KEY_A, true); key_state(0, KEY_W, true); key_state(0, BTN_LEFT, true);
    send_event(0, EV_KEY, KEY_A, 1); send_event(0, EV_KEY, KEY_W, 1);
    send_event(0, EV_KEY, BTN_LEFT, 1); wait_idle(); collect();
    Uint32 user = SDL_RegisterEvents(1);
    SDL_Event filler = { .type = user };
    int filled = 0;
    while (filled < 100000 && SDL_PushEvent(&filler)) { filled++; }
    assert(filled > 3 && filled < 100000); SDL_ClearError();
    key_state(0, KEY_A, false); key_state(0, KEY_W, false);
    key_state(0, BTN_LEFT, false); key_state(0, KEY_D, true);
    int queries_before = atomic_load(&queries);
    dropped(0);
    SDL_PumpEvents(); /* process damage while the event queue is full */
    recover_to(queries_before + 1);
    assert(SDLOP_EventQueueFree() == 0);
    assert(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A]);
    assert(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_W]);
    assert(SDL_GetKeyboardState(NULL)[SDL_SCANCODE_D]);
    assert(!(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK));
    SDL_Event sentinels[113]; int n, total = 0;
    while ((n = SDL_PeepEvents(sentinels, 113, SDL_GETEVENT, user, user)) > 0) { total += n; }
    assert(total == filled);
    SDL_PumpEvents(); seen = collect();
    assert(!seen.keyup[SDL_SCANCODE_A] && !seen.keyup[SDL_SCANCODE_W]);
    assert(!seen.keydown[SDL_SCANCODE_D] && !seen.up);
    assert(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A] &&
           !SDL_GetKeyboardState(NULL)[SDL_SCANCODE_W] && SDL_GetKeyboardState(NULL)[SDL_SCANCODE_D]);
    key_state(0, KEY_D, false); send_event(0, EV_KEY, KEY_D, 0); wait_idle(); collect();
    drain_raw();
    puts("PASS: full SDL queue never freezes key/button state or replays dropped transitions");

    /* Lose final release records in both full rings, then recover without any
     * further native input (control eventfd must not shut the worker down). */
    key_state(0, BTN_LEFT, true); key_state(1, KEY_A, true);
    send_event(0, EV_KEY, BTN_LEFT, 1); send_event(1, EV_KEY, KEY_A, 1); wait_idle(); collect(); drain_raw();
    SDLop_RawInputStats before, after;
    assert(SDLop_GetRawInputStats(&before));
    enum { BURST = 40000, CAPACITY = 32768 };
    for (int i = 0; i < BURST; i++) { send_event(0, EV_REL, REL_X, 1); }
    key_state(0, BTN_LEFT, false); key_state(1, KEY_A, false);
    send_event(0, EV_KEY, BTN_LEFT, 0); send_event(0, EV_SYN, SYN_REPORT, 0);
    send_event(1, EV_KEY, KEY_A, 0); send_event(1, EV_SYN, SYN_REPORT, 0);
    wait_idle(); assert(SDLop_GetRawInputStats(&after));
    assert(after.sdl_ring_dropped - before.sdl_ring_dropped == BURST + 4 - CAPACITY);
    assert(after.poll_ring_dropped - before.poll_ring_dropped == BURST + 4 - CAPACITY);
    assert(drain_raw() == CAPACITY);
    recover_to(7); seen = collect();
    assert(!seen.motions && seen.up == 1 && seen.keyup[SDL_SCANCODE_A] == 1);
    assert(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A] && !(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK));
    assert(SDLop_RawInputAvailable());
    printf("PASS: exact independent ring-drop counts (%d each) and lost-release recovery\n", BURST + 4 - CAPACITY);

    /* Overflow only the raw polling ring while draining SDL regularly. */
    int query_before = atomic_load(&queries);
    assert(SDLop_GetRawInputStats(&before));
    for (int i = 0; i < BURST; i++) {
        send_event(0, EV_REL, REL_X, 1);
        if (i % 128 == 127) { wait_idle(); collect(); }
    }
    wait_idle(); collect();
    assert(SDLop_GetRawInputStats(&after));
    assert(after.sdl_ring_dropped == before.sdl_ring_dropped);
    assert(after.poll_ring_dropped - before.poll_ring_dropped == BURST - CAPACITY);
    assert(atomic_load(&queries) == query_before);
    assert(drain_raw() == CAPACITY);
    puts("PASS: poll-only overflow does not trigger SDL recovery");

    /* Conversely overflow only SDL's raw ring, while raw polling keeps up. */
    before = after;
    for (int i = 0; i < BURST; i++) {
        send_event(0, EV_REL, REL_X, 1);
        if (i % 128 == 127) { wait_idle(); drain_raw(); }
    }
    wait_idle(); drain_raw();
    assert(SDLop_GetRawInputStats(&after));
    assert(after.sdl_ring_dropped - before.sdl_ring_dropped == BURST - CAPACITY);
    assert(after.poll_ring_dropped == before.poll_ring_dropped);
    recover_to(query_before + 2); seen = collect();
    assert(!seen.motions);
    assert(after.events_seen == (Uint64)sent && SDLop_GetRawEventCount() == (Uint64)sent);
    assert(atomic_load(&callbacks) == sent && !atomic_load(&callback_stats_errors));
    assert(atomic_load(&raw_markers) == 4);
    puts("PASS: SDL-only overflow recovers; callbacks remain per native event");
    assert(nested_pumps >= 3 && !saw_reordered_motion);
    SDL_RemoveEventWatch(reentrant_watch, NULL);

    SDL_Quit();
    SDLop_RawInputStats final;
    assert(SDLop_GetRawInputStats(&final));
    assert(!memcmp(&after, &final, sizeof(final)));
    assert(SDL_Init(SDL_INIT_VIDEO));
    assert(SDLop_RawInputAvailable() && SDLop_GetRawInputStats(&final));
    assert(!final.events_seen && !final.sdl_ring_dropped && !final.poll_ring_dropped);
    puts("PASS: counters survive quit and reset on worker reinitialization");

    assert(SDL_CreateWindow("removal", 320, 200, 0));
    SDLOP_RawInputQuit();
    sdlop.video->window_system_input = true;
    assert(SDLOP_RawInputInit());
    sent = 0;
    atomic_store(&callbacks, 0); atomic_store(&idle_records, 0);
    assert(SDLop_RegisterRawEventCallback(on_raw, NULL));
    SDL_FlushEvents(0, SDL_EVENT_LAST);
    send_event(0, EV_KEY, KEY_A, 1); send_event(1, EV_KEY, KEY_A, 1);
    send_event(0, EV_KEY, KEY_B, 1); send_event(0, EV_KEY, BTN_LEFT, 1);
    wait_idle(); collect();
    assert(SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A] && SDL_GetKeyboardState(NULL)[SDL_SCANCODE_B]);
    filler.type = SDL_EVENT_USER;
    while (SDL_PushEvent(&filler)) { }
    close(first->write_fd); first->write_fd = -1; /* real pipe EOF/HUP */
    SDL_Delay(20);
    SDL_PumpEvents(); /* state releases complete even though notifications cannot fit */
    assert(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_B]);
    assert(!(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK));
    SDL_FlushEvents(0, SDL_EVENT_LAST);
    SDL_PumpEvents();
    seen = collect();
    assert(SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A]); /* surviving device owns it */
    assert(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_B] && !seen.keyup[SDL_SCANCODE_B]);
    assert(!(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) && !seen.up);
    assert(SDLOP_RawInputIsSource()); /* other combo still covers both classes */
    assert(SDLop_GetRawEventCount() == (Uint64)sent && atomic_load(&callbacks) == sent);
    send_event(1, EV_KEY, KEY_A, 0); send_event(1, EV_KEY, KEY_C, 1); wait_idle(); collect();
    assert(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A] && SDL_GetKeyboardState(NULL)[SDL_SCANCODE_C]);
    close(second.write_fd); second.write_fd = -1;
    Uint64 deadline = SDL_GetTicks() + 3000;
    while (SDLOP_RawInputIsSource() && SDL_GetTicks() < deadline) { SDL_PumpEvents(); SDL_Delay(1); }
    assert(!SDLOP_RawInputIsSource() && !SDL_GetKeyboardState(NULL)[SDL_SCANCODE_C]);
    assert(SDLop_GetRawEventCount() == (Uint64)sent && atomic_load(&callbacks) == sent);
    puts("PASS: terminal EOF retires devices, queue-safe releases preserve union; last removal enables seat fallback without synthetic raw callbacks");
    SDL_Quit();
    close(second.read_fd); close(second.write_fd); second.read_fd = -1;
    unlink(path); uinput_synth_destroy(first);
    puts("test_evdev_recovery: PASS");
    return 0;
}
