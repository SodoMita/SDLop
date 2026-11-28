/*
  SDLop - asyncinput-style low-latency evdev input worker.

  A dedicated background thread:
    - opens every readable /dev/input/event* device
    - epoll_wait()s and batch-reads struct input_event
    - publishes each event (native codes, kernel timestamp) to:
        1. user callbacks (invoked right here - lowest latency)
        2. an SPSC lock-free ring drained by SDL_PumpEvents()
        3. an SPSC lock-free ring drained by SDLop_PollRawEvents()
    - signals an eventfd so SDL_WaitEvent() wakes up immediately

  No locks on the hot path, no allocation per event.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "internal/sdlop_ring.h"
#include "internal/scancode_evdev.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define SDLOP_MAX_DEVICES     32
#define SDLOP_MAX_EPOLL_EVENTS 16
/* Bounded burst headroom: 32K native records is ~171ms at 64K two-axis
 * reports/s, or ~20ms at 1.6M records/s. Init clears/prefaults this storage
 * before starting the worker; publication still does exactly the same work. */
#define SDLOP_SDL_RING_SIZE   32768 /* power of two */
#define SDLOP_POLL_RING_SIZE  32768 /* power of two */
#define SDLOP_MAX_CALLBACKS   8
#define SDLOP_MAX_EVENTS_PER_READ 64
#define SDLOP_MAX_READS_PER_DEVICE 32 /* bound wakeup/control starvation */
#define SDLOP_KEY_WORDS (KEY_MAX / (8 * sizeof(unsigned long)) + 1)

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct SDLOP_RawDevice
{
    int fd;
    Uint32 index;
    bool is_keyboard;
    bool is_pointer;
    bool clock_monotonic; /* EVIOCSCLOCKID succeeded for this fd */
    bool has_abs;
    bool wheel_highres_x, wheel_highres_y; /* immutable capabilities */
    bool dead_fd; /* worker only; fd kept open until join */
    atomic_bool disconnected; /* worker release -> main acquire */

    /* Main-thread SDL adapter state (never touched by the worker). */
    unsigned long keys[SDLOP_KEY_WORDS];
    float hi_res_x, hi_res_y;
    SDL_Window *wheel_focus; /* owner of a partial SDL wheel frame */
    bool removal_done; /* main-thread state release completed */
    int abs_min[2], abs_max[2];
    int abs_range[2]; /* abs_max - abs_min, kept for the abs_motion scaling */
    bool touch_is_left; /* BTN_TOUCH with no BTN_LEFT is this device's left button */
    bool abs_recovered;
    bool syncing, wait_report, requested;
    Uint64 retry_ns;
    unsigned int sync_code;
    uint64_t sync_generation, request_generation;

    /* Worker writes only after a request, then release-publishes ready.
     * Main does not request again until it has finished reading the snapshot.
     * The boundary is the SDL ring head after draining/querying this fd. */
    atomic_bool snapshot_ready;
    bool snapshot_ok;
    unsigned long snapshot_keys[SDLOP_KEY_WORDS];
    struct input_absinfo snapshot_abs[2];
    uint64_t snapshot_boundary, snapshot_dropped;
} SDLOP_RawDevice;

typedef struct SDLOP_CallbackSet
{
    int count;
    struct {
        SDLop_RawEventCallback cb;
        void *userdata;
    } entries[SDLOP_MAX_CALLBACKS];
    /* Immutable ownership chain. The worker never traverses it; reclaim all
     * published versions only after it has joined, without per-event epochs. */
    struct SDLOP_CallbackSet *previous;
} SDLOP_CallbackSet;

static struct
{
    _Atomic bool running; /* worker loop flag: written by main, read by worker */
    pthread_t thread;
    int epoll_fd;
    int control_fd; /* shutdown and slow-path state-query requests */
    SDLOP_RawDevice devices[SDLOP_MAX_DEVICES];
    int num_devices;
    bool have_keyboard;
    bool have_pointer;
    bool fallback_pending, source_lost; /* main-thread only, all-or-nothing */
    Uint16 alias_head[SDL_SCANCODE_COUNT], alias_next[SDLOP_EVDEV_KEY_MAX + 1];
    Uint16 delivered_code[SDL_SCANCODE_COUNT]; /* native code owning logical down */
    bool winsys; /* started for X11/Wayland: the only SDL input source there */

    /* rings: single producer (worker), single consumer (main thread) */
    _Alignas(SDLOP_CACHELINE) SDLop_RawEvent sdl_ring_storage[SDLOP_SDL_RING_SIZE];
    SDLOP_Ring sdl_ring;
    _Alignas(SDLOP_CACHELINE) SDLop_RawEvent poll_ring_storage[SDLOP_POLL_RING_SIZE];
    SDLOP_Ring poll_ring;

    _Alignas(SDLOP_CACHELINE) atomic_uint_least64_t event_count;
    /* Drop snapshots change only after a lossy batch. Keep these separately
     * cached from the per-event count and from main-thread recovery state. */
    _Alignas(SDLOP_CACHELINE) atomic_uint_least64_t sdl_dropped, poll_dropped;
    _Alignas(SDLOP_CACHELINE) atomic_uint resync_requests; /* main -> worker */
    _Alignas(SDLOP_CACHELINE) uint64_t observed_dropped; /* main thread */
    bool have_sync; /* main-thread slow path active */

    /* callbacks (RCU-style snapshot pointer swap) */
    _Alignas(SDLOP_CACHELINE) _Atomic(SDLOP_CallbackSet *) callbacks;
    pthread_mutex_t cb_mutex;
} raw;

/* Console keyboard ownership and the Ctrl+Alt+Fn chord.

   Kept apart from `raw` on purpose: a video driver claims the console during
   its own init. Keep ownership separate from the worker lifecycle. */
static struct
{
    bool kb_owns_off;        /* the console keyboard is in K_OFF because of us */
    int vt_fd;               /* console the chord applies to, or -1 */
    unsigned int modifiers; /* left/right Ctrl and Alt bits */
    Uint16 fn_swallowed;     /* consumed Fn code; swallow its release too */
} chord = { .vt_fd = -1 };

/* ------------------------------------------------------------------ */
/* Device discovery                                                    */
/* ------------------------------------------------------------------ */

static bool test_bit(const unsigned long *bits, unsigned int bit)
{
    return (bits[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1ul;
}

static int open_input_devices(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        return 0;
    }
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL && raw.num_devices < SDLOP_MAX_DEVICES) {
        if (strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue; /* no permission or busy */
        }

        /* Select the evdev clock before the first read. Older/non-evdev
         * sources may reject this; only those retain timestamp guessing. */
        int clock_id = CLOCK_MONOTONIC;
        bool clock_monotonic = ioctl(fd, EVIOCSCLOCKID, &clock_id) == 0;

        /* what can this device do? */
        unsigned long evbits[EV_MAX / (8 * sizeof(unsigned long)) + 1] = { 0 };
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
            close(fd);
            continue;
        }

        SDLOP_RawDevice *dev = &raw.devices[raw.num_devices];
        memset(dev, 0, sizeof(*dev)); /* a previous rejected fd may have used this slot */
        atomic_init(&dev->disconnected, false);
        atomic_init(&dev->snapshot_ready, false);
        dev->fd = fd;
        dev->index = (Uint32)raw.num_devices;
        dev->clock_monotonic = clock_monotonic;
        dev->is_keyboard = dev->is_pointer = false;

        bool pointer_button = false, touchpad = false, has_btn_left = false;
        if (test_bit(evbits, EV_KEY)) {
            unsigned long keybits[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = { 0 };
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
                pointer_button = test_bit(keybits, BTN_LEFT) || test_bit(keybits, BTN_TOOL_PEN);
                touchpad = test_bit(keybits, BTN_TOOL_FINGER);
                has_btn_left = test_bit(keybits, BTN_LEFT);
                if (test_bit(keybits, KEY_W) || test_bit(keybits, KEY_A)) {
                    dev->is_keyboard = true;
                }
            }
        }
        if (test_bit(evbits, EV_REL)) {
            unsigned long relbits[REL_MAX / (8 * sizeof(unsigned long)) + 1] = { 0 };
            if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) >= 0) {
                dev->wheel_highres_x = test_bit(relbits, REL_HWHEEL_HI_RES);
                dev->wheel_highres_y = test_bit(relbits, REL_WHEEL_HI_RES);
                if (test_bit(relbits, REL_X) && test_bit(relbits, REL_Y)) {
                    dev->is_pointer = true;
                }
            }
        }

        if (test_bit(evbits, EV_ABS) && pointer_button && !touchpad) {
            unsigned long props[INPUT_PROP_MAX / (8 * sizeof(unsigned long)) + 1] = { 0 };
            /* Legacy devices may not implement EVIOCGPROP. Direct touch
             * surfaces need a touch translator, not mouse emulation here. */
            (void)ioctl(fd, EVIOCGPROP(sizeof(props)), props);
            unsigned long bits[ABS_MAX / (8 * sizeof(unsigned long)) + 1] = { 0 };
            if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) >= 0 &&
                test_bit(bits, ABS_X) && test_bit(bits, ABS_Y) &&
                !test_bit(bits, ABS_MT_POSITION_X) && !test_bit(props, INPUT_PROP_DIRECT)) { /* not a multitouch translator */
                bool valid = true;
                for (int axis = 0; axis < 2; axis++) {
                    int result;
                    struct input_absinfo initial = { 0 };
                    struct input_absinfo *info = &initial;
                    do {
                        result = ioctl(fd, EVIOCGABS(ABS_X + axis), info);
                    } while (result < 0 && errno == EINTR);
                    if (result < 0 || info->maximum <= info->minimum) { valid = false; }
                    dev->abs_min[axis] = info->minimum;
                    dev->abs_max[axis] = info->maximum;
                    dev->abs_range[axis] = info->maximum - info->minimum;
                }
                dev->has_abs = valid;
                dev->is_pointer |= valid;
                /* a touchscreen with no BTN_LEFT reports contact as BTN_TOUCH;
                 * that contact is this device's left button. BTN_LEFT lives in
                 * the EV_KEY bitmap, not the EV_ABS one `bits` holds -- reading
                 * it from `bits` was out of bounds. */
                dev->touch_is_left = !has_btn_left;
            }
        }

        if (!dev->is_keyboard && !dev->is_pointer) {
            close(fd);
            continue; /* not interesting for windowing+input core */
        }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.u32 = dev->index;
        if (epoll_ctl(raw.epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            close(fd);
            continue;
        }
        raw.have_keyboard |= dev->is_keyboard;
        raw.have_pointer |= dev->is_pointer;
        raw.num_devices++;
        count++;
    }
    closedir(dir);
    return count;
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

static void dispatch_callbacks(const SDLop_RawEvent *event)
{
    SDLOP_CallbackSet *set = atomic_load_explicit(&raw.callbacks, memory_order_acquire);
    if (!set || set->count == 0) {
        return;
    }
    for (int i = 0; i < set->count; i++) {
        set->entries[i].cb(event, set->entries[i].userdata);
    }
}

/* Cold error path only: never change native publication/callback behavior.
 * Keep descriptors alive until join, so the main thread never races close/reuse. */
static void retire_device(SDLOP_RawDevice *dev)
{
    if (dev->fd < 0 || dev->dead_fd) { return; }
    (void)epoll_ctl(raw.epoll_fd, EPOLL_CTL_DEL, dev->fd, NULL);
    dev->dead_fd = true;
    atomic_store_explicit(&dev->disconnected, true, memory_order_release);
}

/* Drain only on the worker. Recovery reuses the same publication/callback
 * path before querying kernel state; queries never run in the event loop. */
static bool drain_device_events(SDLOP_RawDevice *dev, struct input_event *batch,
                                Uint64 mono_now, Uint64 rt_offset_ns)
{
    /* UINT64_MAX disables guessing without adding a clock-mode
     * branch to each event. Clock negotiation is per device. */
    const Uint64 device_rt_offset = dev->clock_monotonic ? UINT64_MAX : rt_offset_ns;
    /* The worker is the only writer until it joins at shutdown. Publish the
     * same per-event value as before, without a locked read-modify-write for
     * a counter no other thread increments. Callbacks still observe the count
     * of events completed before the current callback. */
    uint64_t event_count = atomic_load_explicit(&raw.event_count, memory_order_relaxed);
    /* Level-triggered epoll will report remaining input again. A permanently
     * readable source must not postpone wakeups/shutdown/other devices forever.
     * Recovery queries require a genuine EAGAIN boundary, so exhausting this
     * budget returns false and lets the adapter retry its snapshot later. */
    for (int reads = 0; reads < SDLOP_MAX_READS_PER_DEVICE; reads++) {
        ssize_t got = read(dev->fd, batch, SDLOP_MAX_EVENTS_PER_READ * sizeof(*batch));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            bool empty = got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
            if (!empty) { retire_device(dev); }
            return empty;
        }
        int count = (int)(got / (ssize_t)sizeof(struct input_event));
        for (int j = 0; j < count; j++) {
            const struct input_event *ie = &batch[j];
            if (ie->type == EV_MSC && ie->code == MSC_SCAN) {
                continue; /* redundant */
            }
            SDLop_RawEvent ev2;
            ev2.device = dev->index;
            ev2.type = ie->type;
            ev2.code = ie->code;
            ev2.value = ie->value;
            ev2.timestamp_ns = (Uint64)ie->input_event_sec * 1000000000ull + (Uint64)ie->input_event_usec * 1000ull;
            if (ev2.timestamp_ns > mono_now + 2000000000ull && ev2.timestamp_ns > device_rt_offset) {
                /* fallback realtime-stamped source: normalize */
                ev2.timestamp_ns -= device_rt_offset;
            }

            SDLOP_RingPush(&raw.sdl_ring, &ev2);
            SDLOP_RingPush(&raw.poll_ring, &ev2);
            dispatch_callbacks(&ev2);
            atomic_store_explicit(&raw.event_count, ++event_count, memory_order_relaxed);
        }
    }
    return false;
}

static void *worker_main(void *arg)
{
    (void)arg;

    /* The KMSDRM driver consumes its VT-switch signals via signalfd; block
     * them here as well so they are never delivered to (or default-acted on
     * by) this thread. They are published via sdlop.vt_signals before this
     * thread starts (video init runs before raw-input init). */
    if (sdlop.vt_signals[0] > 0 || sdlop.vt_signals[1] > 0) {
        sigset_t vt_mask;
        sigemptyset(&vt_mask);
        if (sdlop.vt_signals[0] > 0) {
            sigaddset(&vt_mask, sdlop.vt_signals[0]);
        }
        if (sdlop.vt_signals[1] > 0) {
            sigaddset(&vt_mask, sdlop.vt_signals[1]);
        }
        pthread_sigmask(SIG_BLOCK, &vt_mask, NULL);
    }
    struct epoll_event evs[SDLOP_MAX_EPOLL_EVENTS];
    struct input_event batch[SDLOP_MAX_EVENTS_PER_READ];

    /* Fallback for devices that rejected EVIOCSCLOCKID: preserve the old
     * realtime -> monotonic heuristic. Successfully negotiated devices
     * bypass it entirely. The offset is sampled only at worker startup. */
    Uint64 rt_offset_ns = 0;
    {
        struct timespec mono, real;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        clock_gettime(CLOCK_REALTIME, &real);
        Uint64 m = (Uint64)mono.tv_sec * 1000000000ull + (Uint64)mono.tv_nsec;
        Uint64 r = (Uint64)real.tv_sec * 1000000000ull + (Uint64)real.tv_nsec;
        rt_offset_ns = r - m;
    }

    uint64_t published_sdl_dropped = 0, published_poll_dropped = 0;
    while (raw.running) {
        int n = epoll_wait(raw.epoll_fd, evs, SDLOP_MAX_EPOLL_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        /* Only this thread writes head. Comparing publication snapshots
         * detects a successful push even if the consumer empties the ring
         * mid-batch. No extra operations on the per-event publication path. */
        uint64_t sdl_head_before = atomic_load_explicit(&raw.sdl_ring.head, memory_order_relaxed);
        bool quit_requested = false, recovered = false;
        Uint64 mono_now = SDLOP_MonotonicNS();
        for (int i = 0; i < n; i++) {
            if (evs[i].data.u32 == 0xFFFFFFFFu) {
                uint64_t control;
                ssize_t result;
                do {
                    result = read(raw.control_fd, &control, sizeof(control));
                } while (result < 0 && errno == EINTR);
                if (!raw.running) {
                    quit_requested = true;
                    break;
                }
                continue;
            }
            SDLOP_RawDevice *dev = &raw.devices[evs[i].data.u32];
            if (!dev->dead_fd) {
                bool drained = drain_device_events(dev, batch, mono_now, rt_offset_ns);
                if (drained && (evs[i].events & (EPOLLHUP | EPOLLERR))) { retire_device(dev); }
                recovered |= dev->dead_fd; /* wake even when no native record arrived */
            }
        }
        unsigned int requests = atomic_load_explicit(&raw.resync_requests, memory_order_acquire);
        if (requests && !quit_requested && raw.running) {
            requests = atomic_exchange_explicit(&raw.resync_requests, 0, memory_order_acquire);
            for (int i = 0; i < raw.num_devices; i++) {
                if (!(requests & (1u << i))) {
                    continue;
                }
                SDLOP_RawDevice *dev = &raw.devices[i];
                if (dev->dead_fd) { continue; } /* main handles terminal state */
                bool drained = drain_device_events(dev, batch, SDLOP_MonotonicNS(), rt_offset_ns);
                if (dev->dead_fd) { recovered = true; continue; }
                memset(dev->snapshot_keys, 0, sizeof(dev->snapshot_keys));
                int result = -1;
                if (drained) {
                    do {
                        result = ioctl(dev->fd, EVIOCGKEY(sizeof(dev->snapshot_keys)), dev->snapshot_keys);
                    } while (result < 0 && errno == EINTR);
                }
                if (result >= 0 && dev->has_abs) {
                    for (int axis = 0; axis < 2; axis++) {
                        int queried;
                        do {
                            queried = ioctl(dev->fd, EVIOCGABS(ABS_X + axis), &dev->snapshot_abs[axis]);
                        } while (queried < 0 && errno == EINTR);
                        if (queried < 0 || dev->snapshot_abs[axis].maximum <= dev->snapshot_abs[axis].minimum) {
                            result = -1;
                        }
                    }
                }
                dev->snapshot_ok = result >= 0;
                dev->snapshot_boundary = atomic_load_explicit(&raw.sdl_ring.head, memory_order_relaxed);
                dev->snapshot_dropped = atomic_load_explicit(&raw.sdl_ring.dropped, memory_order_relaxed);
                atomic_store_explicit(&dev->snapshot_ready, true, memory_order_release);
                recovered = true;
            }
        }
        /* Publish only changed counters at batch boundaries. Unconditional
         * zero stores would invalidate the consumer's cache on every pump. */
        if (atomic_load_explicit(&raw.sdl_ring.dropped, memory_order_relaxed) != published_sdl_dropped) {
            published_sdl_dropped = atomic_load_explicit(&raw.sdl_ring.dropped, memory_order_relaxed);
            atomic_store_explicit(&raw.sdl_dropped, published_sdl_dropped, memory_order_release);
        }
        if (atomic_load_explicit(&raw.poll_ring.dropped, memory_order_relaxed) != published_poll_dropped) {
            published_poll_dropped = atomic_load_explicit(&raw.poll_ring.dropped, memory_order_relaxed);
            atomic_store_explicit(&raw.poll_dropped, published_poll_dropped, memory_order_relaxed);
        }
        if (((atomic_load_explicit(&raw.sdl_ring.head, memory_order_relaxed) != sdl_head_before &&
              !SDLOP_RingEmpty(&raw.sdl_ring)) || recovered) && sdlop.wake_fd >= 0) {
            /* Notify after successful publication regardless of the INITIAL
             * empty state. If already empty now, the consumer has consumed
             * all published records and has no pending raw work to wake for.
             * This avoids unnecessary syscalls while actively polling.
             * EAGAIN means the eventfd is already readable; retry EINTR. */
            uint64_t one = 1;
            ssize_t result;
            do {
                result = write(sdlop.wake_fd, &one, sizeof(one));
            } while (result < 0 && errno == EINTR);
        }
        if (quit_requested || !raw.running) {
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Init / Quit                                                         */
/* ------------------------------------------------------------------ */

/* The video driver being initialised (sdlop.video is still NULL while a
   console driver probes the keyboard from its Init) has its own input. */
static bool video_has_window_system_input(void)
{
    return sdlop.video && sdlop.video->window_system_input;
}

/* Input source policy, read when the worker starts (SDL_Init):
     SDLOP_DISABLE_RAW_INPUT=1  never (kept for compatibility)
     SDLOP_RAW_INPUT=1|0        force on / off
     unset                      console drivers: on; X11/Wayland: off
   Empty values count as unset: `VAR=` is how a caller forwards an unset knob
   (the QEMU guest does exactly that). */
static bool raw_input_wanted(bool winsys)
{
    if (SDLOP_HintFlag("SDLOP_DISABLE_RAW_INPUT", false)) {
        return SDL_SetError("disabled by SDLOP_DISABLE_RAW_INPUT");
    }
    if (!SDLOP_HintFlag("SDLOP_RAW_INPUT", !winsys)) {
        return SDL_SetError(winsys ? "window-system input in use (set SDLOP_RAW_INPUT=1 for evdev)"
                                   : "disabled by SDLOP_RAW_INPUT");
    }
    return true;
}

bool SDLOP_RawInputInit(void)
{
    const bool winsys = video_has_window_system_input();
    if (raw.running) {
        if (raw.winsys == winsys) {
            return true;
        }
        /* a console driver probed the keyboard, then failed and a window
           system took over: re-decide under the window-system policy */
        SDLOP_RawInputQuit();
    }
    if (!raw_input_wanted(winsys)) {
        return false;
    }
    memset(&raw, 0, sizeof(raw));
    atomic_init(&raw.running, false);
    atomic_init(&raw.callbacks, NULL);
    atomic_init(&raw.event_count, 0);
    atomic_init(&raw.sdl_dropped, 0);
    atomic_init(&raw.poll_dropped, 0);
    atomic_init(&raw.resync_requests, 0);
    raw.winsys = winsys;
    for (unsigned code = 0; code <= SDLOP_EVDEV_KEY_MAX; code++) {
        SDL_Scancode sc = (SDL_Scancode)sdlop_evdev_to_scancode[code];
        if (sc != SDL_SCANCODE_UNKNOWN) {
            raw.alias_next[code] = raw.alias_head[sc];
            raw.alias_head[sc] = (Uint16)(code + 1);
        }
    }
    pthread_mutex_init(&raw.cb_mutex, NULL);

    SDLOP_RingInit(&raw.sdl_ring, raw.sdl_ring_storage, SDLOP_SDL_RING_SIZE, sizeof(SDLop_RawEvent));
    SDLOP_RingInit(&raw.poll_ring, raw.poll_ring_storage, SDLOP_POLL_RING_SIZE, sizeof(SDLop_RawEvent));

    if (sdlop.wake_fd < 0) {
        sdlop.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    }

    raw.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (raw.epoll_fd < 0) {
        pthread_mutex_destroy(&raw.cb_mutex);
        return SDL_SetError("epoll_create1 failed: %s", strerror(errno));
    }

    raw.control_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (raw.control_fd < 0) {
        close(raw.epoll_fd);
        raw.epoll_fd = -1;
        pthread_mutex_destroy(&raw.cb_mutex);
        return SDL_SetError("eventfd failed: %s", strerror(errno));
    }
    {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.u32 = 0xFFFFFFFFu; /* control channel */
        if (epoll_ctl(raw.epoll_fd, EPOLL_CTL_ADD, raw.control_fd, &ev) < 0) {
            close(raw.control_fd);
            close(raw.epoll_fd);
            raw.control_fd = raw.epoll_fd = -1;
            pthread_mutex_destroy(&raw.cb_mutex);
            return SDL_SetError("epoll_ctl failed: %s", strerror(errno));
        }
    }

    if (open_input_devices() == 0) {
        close(raw.control_fd);
        close(raw.epoll_fd);
        raw.control_fd = raw.epoll_fd = -1;
        pthread_mutex_destroy(&raw.cb_mutex);
        return SDL_SetError("No readable /dev/input devices (need root or the 'input' group); falling back to window-system input");
    }

    if (winsys && !(raw.have_keyboard && raw.have_pointer)) {
        /* never mix: evdev replaces the window system's keyboard *and* mouse
           or neither of them */
        const bool kb = raw.have_keyboard;
        for (int i = 0; i < raw.num_devices; i++) {
            close(raw.devices[i].fd);
        }
        close(raw.control_fd);
        close(raw.epoll_fd);
        raw.control_fd = raw.epoll_fd = -1;
        raw.num_devices = 0;
        pthread_mutex_destroy(&raw.cb_mutex);
        return SDL_SetError("SDLOP_RAW_INPUT needs a readable keyboard and pointer (no %s); using window-system input",
                            kb ? "pointer" : "keyboard");
    }

    raw.running = true;
    if (pthread_create(&raw.thread, NULL, worker_main, NULL) != 0) {
        raw.running = false;
        for (int i = 0; i < raw.num_devices; i++) {
            close(raw.devices[i].fd);
        }
        close(raw.control_fd);
        close(raw.epoll_fd);
        raw.control_fd = raw.epoll_fd = -1;
        raw.num_devices = 0;
        pthread_mutex_destroy(&raw.cb_mutex);
        return SDL_SetError("Could not start input worker thread");
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "SDLop raw input: %d device(s)%s%s%s",
                raw.num_devices,
                raw.have_keyboard ? " [keyboard]" : "",
                raw.have_pointer ? " [pointer]" : "",
                winsys ? ", replacing window-system keyboard and mouse" : "");
    return true;
}

void SDLOP_RawInputQuit(void)
{
    if (!raw.running) {
        return;
    }
    raw.running = false;
    /* wake the worker via its control eventfd, then join */
    if (raw.control_fd >= 0) {
        uint64_t one = 1;
        ssize_t result;
        do {
            result = write(raw.control_fd, &one, sizeof(one));
        } while (result < 0 && errno == EINTR);
    }
    pthread_join(raw.thread, NULL);
    for (int i = 0; i < raw.num_devices; i++) {
        close(raw.devices[i].fd);
    }
    if (raw.control_fd >= 0) {
        close(raw.control_fd);
        raw.control_fd = -1;
    }
    if (raw.epoll_fd >= 0) {
        close(raw.epoll_fd);
        raw.epoll_fd = -1;
    }
    raw.num_devices = 0;
    raw.winsys = false;
    SDLOP_CallbackSet *set = atomic_load_explicit(&raw.callbacks, memory_order_relaxed);
    while (set) {
        SDLOP_CallbackSet *previous = set->previous;
        free(set);
        set = previous;
    }
    atomic_store_explicit(&raw.callbacks, NULL, memory_order_relaxed);
    pthread_mutex_destroy(&raw.cb_mutex);
}

void SDLOP_RawInputAttachConsole(int console_fd)
{
    /* called once the console keyboard belongs to us (K_OFF was set because a
       raw keyboard exists): from here on key events are ours to deliver and
       Ctrl+Alt+Fn is ours to forward */
    chord.vt_fd = console_fd;
    chord.kb_owns_off = (console_fd >= 0);
    chord.modifiers = 0;
    chord.fn_swallowed = 0;
}

void SDLOP_RawInputDetachConsole(void)
{
    /* the console is not ours any more: no key events, no chord handling */
    if (chord.kb_owns_off) {
        SDL_ResetKeyboard(); /* key releases may happen on the other VT */
    }
    chord.kb_owns_off = false;
    chord.vt_fd = -1;
    chord.modifiers = 0;
}

/* Console drivers ask before setting K_OFF. Start the worker here so we
   only take keys away from the kernel after the actual input path works.
   The driver's VT signal mask is already installed and inherited by it. */
bool SDLOP_RawInputHasKeyboard(void)
{
    if (!raw.running && !SDLOP_RawInputInit()) {
        SDL_ClearError(); /* optional: leave the console keyboard usable */
        return false;
    }
    return raw.have_keyboard;
}

bool SDLOP_RawInputIsSource(void)
{
    /* only ever set with both a keyboard and a pointer (see Init) */
    return raw.running && raw.winsys && !raw.source_lost;
}

/* ------------------------------------------------------------------ */
/* Main-thread consumption                                             */
/* ------------------------------------------------------------------ */

static Uint8 mouse_button_from_btn(Uint16 code)
{
    switch (code) {
    case BTN_LEFT:
    case BTN_TOUCH:  return SDL_BUTTON_LEFT;
    case BTN_RIGHT:  return SDL_BUTTON_RIGHT;
    case BTN_MIDDLE: return SDL_BUTTON_MIDDLE;
    case BTN_SIDE:   return SDL_BUTTON_X1;
    case BTN_EXTRA:  return SDL_BUTTON_X2;
    default:         return 0;
    }
}

static bool raw_console_foreground(void)
{
    struct vt_stat state;
    unsigned int dev = 0;
    return chord.kb_owns_off && chord.vt_fd >= 0 &&
           ioctl(chord.vt_fd, TIOCGDEV, &dev) == 0 && major(dev) == 4 &&
           ioctl(chord.vt_fd, VT_GETSTATE, &state) == 0 && state.v_active == minor(dev);
}

/* Ctrl+Alt+Fn: the kernel's own VT switch chord, but the console keyboard is
   in K_OFF, so its events arrive here instead. Returns true when the event was
   the chord and must not surface as an SDL key event. */
static bool sdlop_vt_handle_chord(const SDLop_RawEvent *ev, bool allow_switch)
{
    if (!raw_console_foreground()) {
        return false;
    }
    unsigned int modifier = 0;
    switch (ev->code) {
    case KEY_LEFTCTRL:  modifier = 1; break;
    case KEY_RIGHTCTRL: modifier = 2; break;
    case KEY_LEFTALT:   modifier = 4; break;
    case KEY_RIGHTALT:  modifier = 8; break;
    default: break;
    }
    if (modifier) {
        if (ev->value) {
            chord.modifiers |= modifier;
        } else {
            chord.modifiers &= ~modifier;
        }
        return false;
    }
    if (chord.fn_swallowed == ev->code) {
        if (ev->value == 0) {
            chord.fn_swallowed = 0;
        }
        return true;
    }
    const int fn = (ev->code == KEY_F11) ? 11 : (ev->code == KEY_F12) ? 12 :
                   (ev->code >= KEY_F1 && ev->code <= KEY_F10) ? (int)ev->code - (int)KEY_F1 + 1 : 0;
    if (!allow_switch || !fn || !(chord.modifiers & 3) || !(chord.modifiers & 12) || ev->value != 1) {
        return false;
    }
    chord.fn_swallowed = ev->code;
    /* The kernel owns the switch protocol: we are in VT_PROCESS, so this
       request makes the kernel send the release signal to us and remember the
       target. The driver releases the console (master, mode, keyboard) and
       acknowledges; the switch then completes. */
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "SDLop raw input: Ctrl+Alt+F%d -> VT %d", fn, fn);
    if (ioctl(chord.vt_fd, VT_ACTIVATE, fn) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "SDLop raw input: VT_ACTIVATE(%d) failed: %s", fn, strerror(errno));
    }
    return true;
}

/* Window-system mode: /dev/input sees every keystroke and mouse movement of
   the seat, whichever client they are meant for. Deliver only what the
   window system would have delivered to us, using its focus as the truth:
   keys while one of our windows has keyboard focus, pointer input while the
   pointer is over one of our windows (or relative mode holds it), neither
   while another client holds an active grab (screen locker, WM menu or
   move). Releases
   of keys/buttons we reported down always pass, so nothing sticks. */
static bool winsys_relative_active(void)
{
    return sdlop.relative_mode_window && sdlop.keyboard_focus == sdlop.relative_mode_window;
}

static bool winsys_pointer_focus(void)
{
    return !sdlop.pointer_grabbed_elsewhere && (sdlop.mouse_focus || winsys_relative_active());
}

static bool winsys_keyboard_focus(void)
{
    return sdlop.keyboard_focus && !sdlop.keyboard_grabbed_elsewhere;
}

void SDLOP_RawInputResetPointerFrame(void)
{
    for (int i = 0; i < raw.num_devices; i++) {
        raw.devices[i].hi_res_x = raw.devices[i].hi_res_y = 0;
    }
}

bool SDLOP_RawInputKeyboardAllowed(void)
{
    return !raw.running || raw.source_lost || (raw.winsys ? (!raw.fallback_pending && winsys_keyboard_focus()) : raw_console_foreground());
}

/* absolute pointers, per device (main thread) */
typedef struct
{
    int cur[2];  /* latest raw X/Y */
    int last[2]; /* raw X/Y of the last motion sent */
    bool has_cur[2];
    bool has_last;
    bool dirty;
} SDLOP_AbsState;
static SDLOP_AbsState abs_state[SDLOP_MAX_DEVICES];

/* union of all displays: what an absolute device spans on a desktop */
static void desktop_size(float *w, float *h)
{
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = 0; ids && i < n; i++) {
        SDL_Rect r;
        if (SDL_GetDisplayBounds(ids[i], &r)) {
            if (i == 0 || r.x < x0) {
                x0 = r.x;
            }
            if (i == 0 || r.y < y0) {
                y0 = r.y;
            }
            if (i == 0 || r.x + r.w > x1) {
                x1 = r.x + r.w;
            }
            if (i == 0 || r.y + r.h > y1) {
                y1 = r.y + r.h;
            }
        }
    }
    SDL_free(ids);
    *w = x1 > x0 ? (float)(x1 - x0) : 1920.0f;
    *h = y1 > y0 ? (float)(y1 - y0) : 1080.0f;
}

static void abs_motion(const SDLOP_RawDevice *dev, Uint64 timestamp_ns, bool winsys)
{
    SDLOP_AbsState *st = &abs_state[dev->index];
    if (!st->dirty || !st->has_cur[0] || !st->has_cur[1]) {
        return;
    }
    st->dirty = false;
    const float nx = (float)(st->cur[0] - dev->abs_min[0]) / (float)dev->abs_range[0];
    const float ny = (float)(st->cur[1] - dev->abs_min[1]) / (float)dev->abs_range[1];
    SDL_Window *w = sdlop.mouse_focus ? sdlop.mouse_focus : sdlop.keyboard_focus;
    if (!winsys) {
        /* console: the window covers the display, like stock's mapping
         * onto the display mode */
        if (!w || w->w <= 0 || w->h <= 0) {
            return;
        }
        const float x = SDL_clamp(nx * (float)w->w, 0.0f, (float)(w->w - 1));
        const float y = SDL_clamp(ny * (float)w->h, 0.0f, (float)(w->h - 1));
        const float dx = sdlop.mouse_has_position ? x - sdlop.mouse_x : 0.0f;
        const float dy = sdlop.mouse_has_position ? y - sdlop.mouse_y : 0.0f;
        st->last[0] = st->cur[0];
        st->last[1] = st->cur[1];
        st->has_last = true;
        SDLOP_SendMouseMotion(x, y, dx, dy, timestamp_ns);
        return;
    }
    /* window system: the device spans the desktop and the window's place
     * on it is unknown (Wayland), so move by the device's motion scaled to
     * the desktop, anchored where the window system said the pointer
     * entered - like relative mice */
    const bool had_last = st->has_last;
    const int lx = st->last[0], ly = st->last[1];
    st->last[0] = st->cur[0];
    st->last[1] = st->cur[1];
    st->has_last = true;
    if (!had_last || (!winsys_pointer_focus() && !sdlop.mouse_buttons)) {
        return; /* first sample, or not over our window: just track */
    }
    float dw, dh;
    desktop_size(&dw, &dh);
    const float dx = (float)(st->cur[0] - lx) * dw / (float)dev->abs_range[0];
    const float dy = (float)(st->cur[1] - ly) * dh / (float)dev->abs_range[1];
    if (dx == 0.0f && dy == 0.0f) {
        return;
    }
    float x = SDLOP_NO_POS, y = SDLOP_NO_POS;
    if (w && !sdlop.relative_mode_window) {
        x = SDL_clamp(sdlop.mouse_x + dx, 0.0f, (float)(w->w - 1));
        y = SDL_clamp(sdlop.mouse_y + dy, 0.0f, (float)(w->h - 1));
    } else if (w) {
        x = sdlop.mouse_x + dx;
        y = sdlop.mouse_y + dy;
    }
    SDLOP_SendMouseMotion(x, y, dx, dy, timestamp_ns);
}

/* tests: a fake device entry (no fd) for SDLOP_RawInputTestFeed; only while
 * the worker isn't running. Returns its index or -1. */
int SDLOP_RawInputTestAddDevice(bool keyboard, bool pointer, bool absolute, int min_x, int max_x, int min_y, int max_y,
                                bool touch_is_left)
{
    if (raw.running || raw.num_devices >= SDLOP_MAX_DEVICES) {
        return -1;
    }
    SDLOP_RawDevice *dev = &raw.devices[raw.num_devices];
    memset(dev, 0, sizeof(*dev));
    dev->fd = -1;
    dev->index = (Uint32)raw.num_devices;
    dev->is_keyboard = keyboard;
    dev->is_pointer = pointer || absolute;
    dev->has_abs = absolute;
    dev->touch_is_left = touch_is_left;
    dev->abs_min[0] = min_x;
    dev->abs_max[0] = max_x;
    dev->abs_range[0] = max_x - min_x;
    dev->abs_min[1] = min_y;
    dev->abs_max[1] = max_y;
    dev->abs_range[1] = max_y - min_y;
    return (int)raw.num_devices++;
}

static void pump_event(const SDLop_RawEvent *ev, bool recovering);

static void pump_event(const SDLop_RawEvent *ev, bool recovering)
{
    SDLOP_RawDevice *dev = &raw.devices[ev->device];
    const bool winsys = raw.winsys;

    switch (ev->type) {
    case SDLop_EV_KEY: {
        if (mouse_button_from_btn(ev->code)) {
            Uint8 btn = mouse_button_from_btn(ev->code);
            abs_motion(dev, ev->timestamp_ns, winsys);
            if (winsys && !winsys_pointer_focus() &&
                !(ev->value == 0 && (sdlop.mouse_buttons & SDL_BUTTON_MASK(btn)))) {
                return;
            }
            SDLOP_SendMouseButton(ev->value != 0, btn, SDLOP_NO_POS, SDLOP_NO_POS, ev->timestamp_ns);
            return;
        }
        if (ev->code == BTN_TOUCH || ev->code == BTN_STYLUS || ev->code == BTN_STYLUS2) {
            /* pen tip / touch = left, barrel buttons = right / middle. A
             * touchscreen with no BTN_LEFT reports contact as BTN_TOUCH, and
             * that contact is its left button. */
            const Uint8 btn = ev->code == BTN_STYLUS ? SDL_BUTTON_RIGHT
                              : ev->code == BTN_STYLUS2 ? SDL_BUTTON_MIDDLE
                              : dev->touch_is_left ? SDL_BUTTON_LEFT : 0;
            if (!btn || ev->value == 2) {
                return;
            }
            abs_motion(dev, ev->timestamp_ns, winsys); /* act where the tip is */
            if (winsys && !winsys_pointer_focus() &&
                !(ev->value == 0 && (sdlop.mouse_buttons & SDL_BUTTON_MASK(btn)))) {
                return;
            }
            SDLOP_SendMouseButton(ev->value != 0, btn, SDLOP_NO_POS, SDLOP_NO_POS, ev->timestamp_ns);
            return;
        }
        if (ev->code > SDLOP_EVDEV_KEY_MAX) {
            return;
        }
        SDL_Scancode sc = (SDL_Scancode)sdlop_evdev_to_scancode[ev->code];
        if (winsys) {
            /* no VT chord here: the window system owns the console. Kernel
               autorepeat (value 2) is dropped: SDL's own repeat runs with the
               window system's delay/rate, as for native key events. */
            if (sc == SDL_SCANCODE_UNKNOWN || ev->value == 2 ||
                (!winsys_keyboard_focus() && !(ev->value == 0 && sdlop.keystate[sc]))) {
                return;
            }
            SDLOP_SendKeyboardKey(ev->value != 0, false, sc, ev->code, ev->timestamp_ns);
            return;
        }
        if (sdlop_vt_handle_chord(ev, !recovering)) {
            return;
        }
        if (raw_console_foreground() && sc != SDL_SCANCODE_UNKNOWN) {
            SDLOP_SendKeyboardKey(ev->value != 0, ev->value == 2, sc, ev->code, ev->timestamp_ns);
        }
        break;
    }
    case SDLop_EV_REL: {
        if (winsys && !winsys_pointer_focus() &&
            !((ev->code == SDLop_REL_X || ev->code == SDLop_REL_Y) && sdlop.mouse_buttons)) {
            break; /* not ours (a drag that started in our window keeps its motion) */
        }
        switch (ev->code) {
        case SDLop_REL_X:
        case SDLop_REL_Y: {
            float dx = (ev->code == SDLop_REL_X) ? (float)ev->value : 0.0f;
            float dy = (ev->code == SDLop_REL_Y) ? (float)ev->value : 0.0f;
            /* absolute position best-effort: accumulate into window space */
            float x = SDLOP_NO_POS, y = SDLOP_NO_POS;
            SDL_Window *w = sdlop.mouse_focus ? sdlop.mouse_focus : sdlop.keyboard_focus;
            if (w && !sdlop.relative_mode_window) {
                x = SDL_clamp(sdlop.mouse_x + dx, 0.0f, (float)(w->w - 1));
                y = SDL_clamp(sdlop.mouse_y + dy, 0.0f, (float)(w->h - 1));
            } else if (w) {
                x = sdlop.mouse_x + dx;
                y = sdlop.mouse_y + dy;
            }
            SDLOP_SendMouseMotion(x, y, dx, dy, ev->timestamp_ns);
            break;
        }
        case SDLop_REL_WHEEL:
            if (dev->wheel_highres_y) { break; }
            SDLOP_SendMouseWheel(0.0f, (float)ev->value, ev->timestamp_ns);
            break;
        case SDLop_REL_HWHEEL:
            if (dev->wheel_highres_x) { break; }
            SDLOP_SendMouseWheel((float)ev->value, 0.0f, ev->timestamp_ns);
            break;
        case SDLop_REL_WHEEL_HI_RES:
            if (dev->wheel_focus != (sdlop.mouse_focus ? sdlop.mouse_focus : sdlop.relative_mode_window)) {
                dev->hi_res_x = dev->hi_res_y = 0;
            }
            dev->wheel_focus = sdlop.mouse_focus ? sdlop.mouse_focus : sdlop.relative_mode_window;
            dev->hi_res_y += (float)ev->value / 120.0f;
            break;
        case SDLop_REL_HWHEEL_HI_RES:
            if (dev->wheel_focus != (sdlop.mouse_focus ? sdlop.mouse_focus : sdlop.relative_mode_window)) {
                dev->hi_res_x = dev->hi_res_y = 0;
            }
            dev->wheel_focus = sdlop.mouse_focus ? sdlop.mouse_focus : sdlop.relative_mode_window;
            dev->hi_res_x += (float)ev->value / 120.0f;
            break;
        default:
            break;
        }
        break;
    }
    case SDLop_EV_ABS:
        if (dev->has_abs && (ev->code == ABS_X || ev->code == ABS_Y)) {
            const int axis = ev->code - ABS_X;
            abs_state[dev->index].cur[axis] = ev->value;
            abs_state[dev->index].has_cur[axis] = true;
            abs_state[dev->index].dirty = true;
        }
        break;
    case SDLop_EV_SYN:
        if (ev->code == SDLop_SYN_REPORT) { abs_motion(dev, ev->timestamp_ns, winsys); }
        if (ev->code == SDLop_SYN_REPORT && (dev->hi_res_x != 0.0f || dev->hi_res_y != 0.0f)) {
            SDL_Window *focus = sdlop.mouse_focus ? sdlop.mouse_focus : sdlop.relative_mode_window;
            if (!winsys || (winsys_pointer_focus() && focus == dev->wheel_focus)) {
                SDLOP_SendMouseWheel(dev->hi_res_x, dev->hi_res_y, ev->timestamp_ns);
            }
            dev->hi_res_x = dev->hi_res_y = 0.0f;
        }
        break;
    default:
        break;
    }
}

/* SDL key/button state is the union of per-device states. Recovery of one
 * keyboard must not release a key still held on another keyboard. */
void SDLOP_RawInputTestFeed(const SDLop_RawEvent *ev, bool winsys)
{
    const bool saved = raw.winsys;
    raw.winsys = winsys;
    pump_event(ev, false);
    raw.winsys = saved;
}

static void pump_key(const SDLop_RawEvent *ev, bool recovering)
{
    if (ev->code > KEY_MAX) {
        return;
    }
    SDLOP_RawDevice *dev = &raw.devices[ev->device];
    bool was_down = test_bit(dev->keys, ev->code);
    if (ev->value == 2) {
        if (was_down) {
            pump_event(ev, recovering);
        }
        return;
    }
    bool down = ev->value != 0;
    if (was_down == down) {
        return; /* includes stale/duplicate transitions following a snapshot */
    }
    unsigned int word = ev->code / (8 * sizeof(unsigned long));
    unsigned long mask = 1ul << (ev->code % (8 * sizeof(unsigned long)));
    if (down) {
        dev->keys[word] |= mask;
    } else {
        dev->keys[word] &= ~mask;
    }
    SDL_Scancode sc = ev->code <= SDLOP_EVDEV_KEY_MAX ?
        (SDL_Scancode)sdlop_evdev_to_scancode[ev->code] : SDL_SCANCODE_UNKNOWN;
    Uint16 head = sc != SDL_SCANCODE_UNKNOWN ? raw.alias_head[sc] : 0;
    bool aliases = head && raw.alias_next[head - 1];
    for (int i = 0; i < raw.num_devices; i++) {
        bool held = (Uint32)i != ev->device && test_bit(raw.devices[i].keys, ev->code);
        if (mouse_button_from_btn(ev->code) == SDL_BUTTON_LEFT) {
            held |= (ev->code != BTN_LEFT || (Uint32)i != ev->device) && test_bit(raw.devices[i].keys, BTN_LEFT);
            held |= (ev->code != BTN_TOUCH || (Uint32)i != ev->device) && test_bit(raw.devices[i].keys, BTN_TOUCH);
        }
        if (aliases) {
            for (Uint16 link = head; link; link = raw.alias_next[link - 1]) {
                unsigned code = link - 1;
                if ((Uint32)i != ev->device || code != ev->code) {
                    held |= test_bit(raw.devices[i].keys, code);
                }
            }
        }
        if (held) {
            /* A prior physical press may have been focus-filtered. Do not
             * suppress a fresh focused press unless SDL saw a press too. */
            Uint8 button = mouse_button_from_btn(ev->code);
            bool delivered = button ? (sdlop.mouse_buttons & SDL_BUTTON_MASK(button)) != 0 :
                (ev->code <= SDLOP_EVDEV_KEY_MAX &&
                 sdlop.keystate[sdlop_evdev_to_scancode[ev->code]]);
            if (!down || delivered) {
                return;
            }
        }
    }
    if (aliases && !mouse_button_from_btn(ev->code)) {
        SDLop_RawEvent logical = *ev;
        if (down && !sdlop.keystate[sc]) { raw.delivered_code[sc] = ev->code; }
        if (!down) { logical.code = raw.delivered_code[sc]; }
        pump_event(&logical, recovering);
    } else {
        pump_event(ev, recovering);
    }
}

static void begin_sync(SDLOP_RawDevice *dev, bool wait_report)
{
    dev->syncing = true;
    dev->wait_report = wait_report;
    dev->sync_generation++;
    dev->sync_code = 0;
    dev->retry_ns = 0;
    dev->hi_res_x = dev->hi_res_y = 0.0f; /* damaged partial wheel frame */
    dev->abs_recovered = false;
    raw.have_sync = true;
    sdlop.repeat_active = false; /* don't repeat uncertain key state */
}

static void request_snapshot(SDLOP_RawDevice *dev)
{
    dev->request_generation = dev->sync_generation;
    dev->requested = true;
    dev->abs_recovered = false;
    atomic_store_explicit(&dev->snapshot_ready, false, memory_order_relaxed);
    atomic_fetch_or_explicit(&raw.resync_requests, 1u << dev->index, memory_order_release);
    uint64_t one = 1;
    ssize_t result;
    do {
        result = write(raw.control_fd, &one, sizeof(one));
    } while (result < 0 && errno == EINTR);
}

/* Main-thread recovery slow path. The snapshot is immutable until the next
 * request. Consume all ring records preceding its publication boundary before
 * diffing it; do not replay stale queued key transitions after the diff. SDL
 * state recovery always completes; event delivery may be dropped under pressure. */
static void pump_recovery(void)
{
    bool any = false;
    uint64_t tail = atomic_load_explicit(&raw.sdl_ring.tail, memory_order_relaxed);
    for (int i = 0; i < raw.num_devices; i++) {
        SDLOP_RawDevice *dev = &raw.devices[i];
        if ((raw.fallback_pending || atomic_load_explicit(&dev->disconnected, memory_order_acquire)) &&
            !dev->removal_done) {
            dev->hi_res_x = dev->hi_res_y = 0;
            /* a slot being reused must not keep the old device's absolute
             * baseline, or the first sample of the new one makes a delta */
            SDL_zero(abs_state[dev->index]);
            for (unsigned code = 0; code <= KEY_MAX; code++) {
                if (!test_bit(dev->keys, code)) { continue; }
                SDLop_RawEvent up = { .device = dev->index, .type = EV_KEY, .code = (Uint16)code };
                pump_key(&up, true);
            }
            dev->removal_done = true;
            dev->syncing = dev->requested = false;
        }
        if (!dev->syncing || dev->removal_done) {
            continue;
        }
        if (dev->requested && atomic_load_explicit(&dev->snapshot_ready, memory_order_acquire)) {
            if (dev->request_generation != dev->sync_generation ||
                dev->snapshot_dropped < raw.observed_dropped || !dev->snapshot_ok) {
                /* Never turn a failed query into an all-keys-up snapshot.
                 * Retry failures at most every 100ms while pumping. */
                bool failed = !dev->snapshot_ok;
                dev->requested = false;
                atomic_store_explicit(&dev->snapshot_ready, false, memory_order_relaxed);
                dev->retry_ns = failed ? SDLOP_MonotonicNS() + 100000000ull : 0;
                dev->sync_code = 0;
            } else if (!dev->wait_report && (int64_t)(tail - dev->snapshot_boundary) >= 0) {
                if (dev->has_abs && !dev->abs_recovered) {
                    /* Restore the position the device actually has, in the
                     * model abs_motion reads (#10's abs_state), and drop the
                     * motion baseline: the deltas lost while SYN_DROPPED are
                     * unknown, so the next sample re-anchors rather than
                     * manufacturing one jump across the gap. */
                    SDLOP_AbsState *st = &abs_state[dev->index];
                    for (int axis = 0; axis < 2; axis++) {
                        dev->abs_min[axis] = dev->snapshot_abs[axis].minimum;
                        dev->abs_max[axis] = dev->snapshot_abs[axis].maximum;
                        /* abs_motion divides by this; keep it in step with the
                         * refreshed min/max and never let it be zero/negative. */
                        dev->abs_range[axis] = dev->abs_max[axis] - dev->abs_min[axis];
                        if (dev->abs_range[axis] <= 0) {
                            dev->abs_range[axis] = 1;
                        }
                        st->cur[axis] = dev->snapshot_abs[axis].value;
                        st->has_cur[axis] = true;
                    }
                    st->has_last = false;
                    st->dirty = true;
                    abs_motion(dev, 0, raw.winsys);
                    dev->abs_recovered = true;
                }
                while (dev->sync_code <= KEY_MAX) {
                    unsigned int code = dev->sync_code;
                    bool down = test_bit(dev->snapshot_keys, code);
                    if (down != test_bit(dev->keys, code)) {
                        SDLop_RawEvent ev = { .device = dev->index, .type = EV_KEY,
                                             .code = (Uint16)code, .value = down };
                        pump_key(&ev, true);
                    }
                    dev->sync_code++;
                }
                dev->requested = dev->syncing = false;
                atomic_store_explicit(&dev->snapshot_ready, false, memory_order_relaxed);
            }
        }
        if (dev->syncing) {
            any = true;
            if (!dev->wait_report && !dev->requested &&
                (!dev->retry_ns || SDLOP_MonotonicNS() >= dev->retry_ns)) {
                request_snapshot(dev);
            }
        }
    }
    raw.have_sync = any;
    if (raw.fallback_pending) {
        bool done = true;
        for (int i = 0; i < raw.num_devices; i++) { done &= raw.devices[i].removal_done; }
        if (done) {
            sdlop.repeat_active = false;
            raw.source_lost = true; /* enable seat keyboard+mouse only after ALL raw releases */
        }
    }
}

void SDLOP_RawInputPump(void)
{
    if (!raw.running) {
        return;
    }
    if (!raw.source_lost) {
        bool keyboard = false, pointer = false, removed = false;
        for (int i = 0; i < raw.num_devices; i++) {
            SDLOP_RawDevice *dev = &raw.devices[i];
            bool dead = atomic_load_explicit(&dev->disconnected, memory_order_acquire);
            removed |= dead && !dev->removal_done;
            keyboard |= !dead && dev->is_keyboard;
            pointer |= !dead && dev->is_pointer;
        }
        raw.have_keyboard = keyboard;
        raw.have_pointer = pointer;
        if (removed) {
            raw.have_sync = true;
            raw.fallback_pending |= raw.winsys && !(keyboard && pointer);
        }
    }
    uint64_t dropped = atomic_load_explicit(&raw.sdl_dropped, memory_order_acquire);
    if (dropped != raw.observed_dropped) {
        raw.observed_dropped = dropped;
        /* The missing records have no device identity in a full ring. Recover
         * every device, without modifying any already-published ring entry. */
        for (int i = 0; i < raw.num_devices; i++) {
            begin_sync(&raw.devices[i], false);
        }
    }
    SDLop_RawEvent ev;
    int n = 0;
    /* Always consume and translate native records. Input senders update SDL
     * state first, then drop notifications if the bounded queue lacks room. */
    for (;;) {
        if (raw.have_sync) {
            /* Acquire head BEFORE checking snapshot readiness. Any record
             * published after a snapshot boundary then also makes ready
             * visible before we decide whether to discard that record. Do
             * not pop a newer record if this head snapshot was empty. */
            uint64_t head = atomic_load_explicit(&raw.sdl_ring.head, memory_order_acquire);
            pump_recovery();
            if (atomic_load_explicit(&raw.sdl_ring.tail, memory_order_relaxed) == head) {
                break;
            }
        }
        if (!SDLOP_RingPop(&raw.sdl_ring, &ev)) {
            break;
        }
        SDLOP_RawDevice *dev = &raw.devices[ev.device];
        if (raw.fallback_pending || dev->removal_done) {
            /* Native callbacks/poll ring stay global; no SDL source mixing. */
        } else if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
            begin_sync(dev, true);
        } else if (dev->syncing) {
            if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                dev->wait_report = false;
            }
        } else if (ev.type == EV_KEY) {
            pump_key(&ev, false);
        } else {
            pump_event(&ev, false);
        }
        if (++n >= SDLOP_SDL_RING_SIZE) {
            break;
        }
    }
    SDLOP_KeyboardProcessRepeats();
}

/* ------------------------------------------------------------------ */
/* SDLop raw API                                                       */
/* ------------------------------------------------------------------ */

bool SDLop_RegisterRawEventCallback(SDLop_RawEventCallback cb, void *userdata)
{
    if (!cb) {
        return SDL_SetError("NULL callback");
    }
    pthread_mutex_lock(&raw.cb_mutex);
    SDLOP_CallbackSet *old = atomic_load_explicit(&raw.callbacks, memory_order_relaxed);
    int count = old ? old->count : 0;
    if (count >= SDLOP_MAX_CALLBACKS) {
        pthread_mutex_unlock(&raw.cb_mutex);
        return SDL_SetError("Too many raw event callbacks (max %d)", SDLOP_MAX_CALLBACKS);
    }
    SDLOP_CallbackSet *set = (SDLOP_CallbackSet *)calloc(1, sizeof(*set));
    if (!set) {
        pthread_mutex_unlock(&raw.cb_mutex);
        return SDL_OutOfMemory();
    }
    if (old) {
        *set = *old;
    }
    set->entries[count].cb = cb;
    set->entries[count].userdata = userdata;
    set->count = count + 1;
    set->previous = old;
    atomic_store_explicit(&raw.callbacks, set, memory_order_release);
    pthread_mutex_unlock(&raw.cb_mutex);
    /* Old versions remain alive for in-flight worker readers until join. */
    return true;
}

void SDLop_UnregisterRawEventCallback(SDLop_RawEventCallback cb)
{
    pthread_mutex_lock(&raw.cb_mutex);
    SDLOP_CallbackSet *old = atomic_load_explicit(&raw.callbacks, memory_order_relaxed);
    bool found = false;
    for (int i = 0; old && i < old->count; i++) {
        if (old->entries[i].cb == cb) {
            found = true;
            break;
        }
    }
    if (!found) {
        pthread_mutex_unlock(&raw.cb_mutex);
        return; /* no change: no allocation, publication or retained snapshot */
    }
    SDLOP_CallbackSet *set = (SDLOP_CallbackSet *)calloc(1, sizeof(*set));
    if (!set) {
        pthread_mutex_unlock(&raw.cb_mutex);
        return;
    }
    set->count = 0;
    for (int i = 0; i < old->count; i++) {
        if (old->entries[i].cb != cb) {
            set->entries[set->count++] = old->entries[i];
        }
    }
    set->previous = old;
    atomic_store_explicit(&raw.callbacks, set, memory_order_release);
    pthread_mutex_unlock(&raw.cb_mutex);
}

int SDLop_PollRawEvents(SDLop_RawEvent *events, int max_events)
{
    if (!events || max_events <= 0) {
        return -1;
    }
    return (int)SDLOP_RingPopBatch(&raw.poll_ring, events, (uint32_t)max_events);
}

bool SDLop_RawInputAvailable(void)
{
    return raw.running;
}

Uint64 SDLop_GetRawEventCount(void)
{
    return atomic_load_explicit(&raw.event_count, memory_order_relaxed);
}

bool SDLop_GetRawInputStats(SDLop_RawInputStats *stats)
{
    if (!stats) {
        return SDL_InvalidParamError("stats");
    }
    stats->version = SDLOP_RAW_INPUT_STATS_VERSION;
    stats->size = (Uint32)sizeof(*stats);
    stats->events_seen = atomic_load_explicit(&raw.event_count, memory_order_relaxed);
    stats->sdl_ring_dropped = atomic_load_explicit(&raw.sdl_dropped, memory_order_acquire);
    stats->poll_ring_dropped = atomic_load_explicit(&raw.poll_dropped, memory_order_relaxed);
    return true;
}

void SDLOP_RawInputSetRepeatInfo(Uint32 delay_ms, Uint32 interval_ms)
{
    sdlop.repeat_delay_ms = delay_ms; /* zero is a legal immediate start */
    sdlop.repeat_disabled = interval_ms == 0;
    if (!interval_ms) {
        sdlop.repeat_active = false;
    } else {
        sdlop.repeat_interval_ms = interval_ms;
        if (sdlop.repeat_active) {
            sdlop.repeat_next_ns = SDLOP_MonotonicNS() + (Uint64)delay_ms * 1000000ull;
        }
    }
}
