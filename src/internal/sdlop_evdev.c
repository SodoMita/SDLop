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
#include <stdatomic.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#define SDLOP_MAX_DEVICES     32
#define SDLOP_MAX_EPOLL_EVENTS 16
#define SDLOP_SDL_RING_SIZE   2048 /* power of two */
#define SDLOP_POLL_RING_SIZE  1024 /* power of two */
#define SDLOP_MAX_CALLBACKS   8
#define SDLOP_MAX_EVENTS_PER_READ 64

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct SDLOP_RawDevice
{
    int fd;
    Uint32 index;
    bool is_keyboard;
    bool is_pointer;
} SDLOP_RawDevice;

typedef struct SDLOP_CallbackSet
{
    int count;
    struct {
        SDLop_RawEventCallback cb;
        void *userdata;
    } entries[SDLOP_MAX_CALLBACKS];
} SDLOP_CallbackSet;

static struct
{
    bool running;
    pthread_t thread;
    int epoll_fd;
    int control_fd; /* eventfd used to wake the worker for shutdown */
    SDLOP_RawDevice devices[SDLOP_MAX_DEVICES];
    int num_devices;
    bool have_keyboard;
    bool have_pointer;

    /* rings: single producer (worker), single consumer (main thread) */
    _Alignas(SDLOP_CACHELINE) SDLop_RawEvent sdl_ring_storage[SDLOP_SDL_RING_SIZE];
    SDLOP_Ring sdl_ring;
    _Alignas(SDLOP_CACHELINE) SDLop_RawEvent poll_ring_storage[SDLOP_POLL_RING_SIZE];
    SDLOP_Ring poll_ring;

    atomic_uint_least64_t event_count;

    /* callbacks (RCU-style snapshot pointer swap) */
    SDLOP_CallbackSet *callbacks;
    pthread_mutex_t cb_mutex;
} raw;

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

        /* what can this device do? */
        unsigned long evbits[EV_MAX / (8 * sizeof(unsigned long)) + 1] = { 0 };
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
            close(fd);
            continue;
        }

        SDLOP_RawDevice *dev = &raw.devices[raw.num_devices];
        dev->fd = fd;
        dev->index = (Uint32)raw.num_devices;
        dev->is_keyboard = dev->is_pointer = false;

        if (test_bit(evbits, EV_KEY)) {
            unsigned long keybits[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = { 0 };
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
                if (test_bit(keybits, KEY_W) || test_bit(keybits, KEY_A)) {
                    dev->is_keyboard = true;
                    raw.have_keyboard = true;
                }
            }
        }
        if (test_bit(evbits, EV_REL)) {
            unsigned long relbits[REL_MAX / (8 * sizeof(unsigned long)) + 1] = { 0 };
            if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) >= 0) {
                if (test_bit(relbits, REL_X) && test_bit(relbits, REL_Y)) {
                    dev->is_pointer = true;
                    raw.have_pointer = true;
                }
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
    SDLOP_CallbackSet *set = atomic_load_explicit((_Atomic(SDLOP_CallbackSet *) *)&raw.callbacks, memory_order_acquire);
    if (!set || set->count == 0) {
        return;
    }
    for (int i = 0; i < set->count; i++) {
        set->entries[i].cb(event, set->entries[i].userdata);
    }
}

static void *worker_main(void *arg)
{
    (void)arg;
    struct epoll_event evs[SDLOP_MAX_EPOLL_EVENTS];
    struct input_event batch[SDLOP_MAX_EVENTS_PER_READ];

    /* Some event sources (notably uinput-injected events) carry
     * CLOCK_REALTIME stamps instead of CLOCK_MONOTONIC. Measure the
     * offset once so we can normalize: timestamps more than 2s "in the
     * future" relative to monotonic are realtime and get shifted. */
    Uint64 rt_offset_ns = 0;
    {
        struct timespec mono, real;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        clock_gettime(CLOCK_REALTIME, &real);
        Uint64 m = (Uint64)mono.tv_sec * 1000000000ull + (Uint64)mono.tv_nsec;
        Uint64 r = (Uint64)real.tv_sec * 1000000000ull + (Uint64)real.tv_nsec;
        rt_offset_ns = r - m;
    }

    while (raw.running) {
        int n = epoll_wait(raw.epoll_fd, evs, SDLOP_MAX_EPOLL_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        bool ring_was_empty = SDLOP_RingEmpty(&raw.sdl_ring);
        bool quit_requested = false;
        Uint64 mono_now = SDLOP_MonotonicNS();
        for (int i = 0; i < n; i++) {
            if (evs[i].data.u32 == 0xFFFFFFFFu) {
                quit_requested = true;
                break;
            }
            SDLOP_RawDevice *dev = &raw.devices[evs[i].data.u32];
            for (;;) {
                ssize_t got = read(dev->fd, batch, sizeof(batch));
                if (got <= 0) {
                    break;
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
                    if (ev2.timestamp_ns > mono_now + 2000000000ull && ev2.timestamp_ns > rt_offset_ns) {
                        /* realtime-stamped source: normalize to monotonic */
                        ev2.timestamp_ns -= rt_offset_ns;
                    }

                    SDLOP_RingPush(&raw.sdl_ring, &ev2);
                    SDLOP_RingPush(&raw.poll_ring, &ev2);
                    dispatch_callbacks(&ev2);
                    atomic_fetch_add_explicit(&raw.event_count, 1, memory_order_relaxed);
                }
            }
        }
        if (ring_was_empty && !SDLOP_RingEmpty(&raw.sdl_ring) && sdlop.wake_fd >= 0) {
            uint64_t one = 1;
            ssize_t unused = write(sdlop.wake_fd, &one, sizeof(one));
            (void)unused;
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

bool SDLOP_RawInputInit(void)
{
    if (getenv("SDLOP_DISABLE_RAW_INPUT")) {
        return false;
    }
    memset(&raw, 0, sizeof(raw));
    pthread_mutex_init(&raw.cb_mutex, NULL);
    atomic_store(&raw.event_count, 0);

    SDLOP_RingInit(&raw.sdl_ring, raw.sdl_ring_storage, SDLOP_SDL_RING_SIZE, sizeof(SDLop_RawEvent));
    SDLOP_RingInit(&raw.poll_ring, raw.poll_ring_storage, SDLOP_POLL_RING_SIZE, sizeof(SDLop_RawEvent));

    if (sdlop.wake_fd < 0) {
        sdlop.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    }

    raw.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (raw.epoll_fd < 0) {
        return SDL_SetError("epoll_create1 failed: %s", strerror(errno));
    }

    raw.control_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (raw.control_fd < 0) {
        close(raw.epoll_fd);
        raw.epoll_fd = -1;
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
            return SDL_SetError("epoll_ctl failed: %s", strerror(errno));
        }
    }

    if (open_input_devices() == 0) {
        close(raw.control_fd);
        close(raw.epoll_fd);
        raw.control_fd = raw.epoll_fd = -1;
        return SDL_SetError("No readable /dev/input devices (need root or the 'input' group); falling back to window-system input");
    }

    raw.running = true;
    if (pthread_create(&raw.thread, NULL, worker_main, NULL) != 0) {
        raw.running = false;
        for (int i = 0; i < raw.num_devices; i++) {
            close(raw.devices[i].fd);
        }
        close(raw.epoll_fd);
        raw.epoll_fd = -1;
        raw.num_devices = 0;
        return SDL_SetError("Could not start input worker thread");
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "SDLop raw input: %d device(s)%s%s",
                raw.num_devices,
                raw.have_keyboard ? " [keyboard]" : "",
                raw.have_pointer ? " [pointer]" : "");
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
        ssize_t unused = write(raw.control_fd, &one, sizeof(one));
        (void)unused;
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
    free((void *)raw.callbacks);
    raw.callbacks = NULL;
    pthread_mutex_destroy(&raw.cb_mutex);
}

bool SDLOP_RawInputKeyboardActive(void)
{
    return raw.running && raw.have_keyboard;
}

bool SDLOP_RawInputMouseActive(void)
{
    return raw.running && raw.have_pointer;
}

/* ------------------------------------------------------------------ */
/* Main-thread consumption                                             */
/* ------------------------------------------------------------------ */

static Uint8 mouse_button_from_btn(Uint16 code)
{
    switch (code) {
    case BTN_LEFT:   return SDL_BUTTON_LEFT;
    case BTN_RIGHT:  return SDL_BUTTON_RIGHT;
    case BTN_MIDDLE: return SDL_BUTTON_MIDDLE;
    case BTN_SIDE:   return SDL_BUTTON_X1;
    case BTN_EXTRA:  return SDL_BUTTON_X2;
    default:         return 0;
    }
}

static void pump_event(const SDLop_RawEvent *ev)
{
    static float hi_res_x_acc, hi_res_y_acc;

    switch (ev->type) {
    case SDLop_EV_KEY: {
        if (ev->code >= BTN_LEFT && ev->code <= BTN_EXTRA) {
            Uint8 btn = mouse_button_from_btn(ev->code);
            if (btn) {
                SDLOP_SendMouseButton(ev->value != 0, btn, SDLOP_NO_POS, SDLOP_NO_POS, ev->timestamp_ns);
            }
            return;
        }
        if (ev->code <= SDLOP_EVDEV_KEY_MAX) {
            SDL_Scancode sc = (SDL_Scancode)sdlop_evdev_to_scancode[ev->code];
            if (sc != SDL_SCANCODE_UNKNOWN) {
                SDLOP_SendKeyboardKey(ev->value != 0, ev->value == 2, sc, ev->code, ev->timestamp_ns);
            }
        }
        break;
    }
    case SDLop_EV_REL: {
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
            SDLOP_SendMouseWheel(0.0f, (float)ev->value, ev->timestamp_ns);
            break;
        case SDLop_REL_HWHEEL:
            SDLOP_SendMouseWheel((float)ev->value, 0.0f, ev->timestamp_ns);
            break;
        case SDLop_REL_WHEEL_HI_RES:
            hi_res_y_acc += (float)ev->value / 120.0f;
            break;
        case SDLop_REL_HWHEEL_HI_RES:
            hi_res_x_acc += (float)ev->value / 120.0f;
            break;
        default:
            break;
        }
        break;
    }
    case SDLop_EV_SYN:
        if (ev->code == SDLop_SYN_REPORT && (hi_res_x_acc != 0.0f || hi_res_y_acc != 0.0f)) {
            SDLOP_SendMouseWheel(hi_res_x_acc, hi_res_y_acc, ev->timestamp_ns);
            hi_res_x_acc = hi_res_y_acc = 0.0f;
        }
        break;
    default:
        break;
    }
}

void SDLOP_RawInputPump(void)
{
    if (!raw.running) {
        return;
    }
    SDLop_RawEvent ev;
    int n = 0;
    while (SDLOP_RingPop(&raw.sdl_ring, &ev)) {
        pump_event(&ev);
        if (++n >= SDLOP_SDL_RING_SIZE) {
            break;
        }
    }
    if (n) {
        SDLOP_KeyboardProcessRepeats();
    }
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
    SDLOP_CallbackSet *old = raw.callbacks;
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
    atomic_store_explicit((_Atomic(SDLOP_CallbackSet *) *)&raw.callbacks, set, memory_order_release);
    pthread_mutex_unlock(&raw.cb_mutex);
    /* previous set intentionally not freed: worker thread may still read it.
     * Bounded leak (one small struct per registration), like asyncinput's
     * fixed callback table. */
    (void)old;
    return true;
}

void SDLop_UnregisterRawEventCallback(SDLop_RawEventCallback cb)
{
    pthread_mutex_lock(&raw.cb_mutex);
    SDLOP_CallbackSet *old = raw.callbacks;
    if (!old) {
        pthread_mutex_unlock(&raw.cb_mutex);
        return;
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
    atomic_store_explicit((_Atomic(SDLOP_CallbackSet *) *)&raw.callbacks, set, memory_order_release);
    pthread_mutex_unlock(&raw.cb_mutex);
}

int SDLop_PollRawEvents(SDLop_RawEvent *events, int max_events)
{
    if (!events || max_events <= 0) {
        return -1;
    }
    int n = 0;
    while (n < max_events && SDLOP_RingPop(&raw.poll_ring, &events[n])) {
        n++;
    }
    return n;
}

bool SDLop_RawInputAvailable(void)
{
    return raw.running;
}

Uint64 SDLop_GetRawEventCount(void)
{
    return atomic_load_explicit(&raw.event_count, memory_order_relaxed);
}

void SDLOP_RawInputSetRepeatInfo(Uint32 delay_ms, Uint32 interval_ms)
{
    if (delay_ms) {
        sdlop.repeat_delay_ms = delay_ms;
    }
    if (interval_ms) {
        sdlop.repeat_interval_ms = interval_ms;
    }
}
