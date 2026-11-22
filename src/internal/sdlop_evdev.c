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
    /* absolute pointer (VM/USB tablet, pen tablet, single-touch screen):
     * EV_ABS X/Y ranges; BTN_TOUCH is the left button when there's no
     * BTN_LEFT */
    bool is_abs;
    bool touch_is_left;
    int abs_min[2], abs_range[2];
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
    _Atomic bool running; /* worker loop flag: written by main, read by worker */
    pthread_t thread;
    int epoll_fd;
    int control_fd; /* eventfd used to wake the worker for shutdown */
    SDLOP_RawDevice devices[SDLOP_MAX_DEVICES];
    int num_devices;
    bool have_keyboard;
    bool have_pointer;
    bool winsys; /* started for X11/Wayland: the only SDL input source there */

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

        dev->is_abs = dev->touch_is_left = false;

        unsigned long keybits[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = { 0 };
        if (test_bit(evbits, EV_KEY)) {
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
                if (test_bit(keybits, KEY_W) || test_bit(keybits, KEY_A)) {
                    dev->is_keyboard = true;
                }
            }
        }
        if (test_bit(evbits, EV_REL)) {
            unsigned long relbits[REL_MAX / (8 * sizeof(unsigned long)) + 1] = { 0 };
            if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) >= 0) {
                if (test_bit(relbits, REL_X) && test_bit(relbits, REL_Y)) {
                    dev->is_pointer = true;
                }
            }
        }
        if (!dev->is_pointer && test_bit(evbits, EV_ABS)) {
            /* udev's input_id rules: X/Y plus a pointer button, not a
             * touchpad (BTN_TOOL_FINGER: the window system turns those into
             * relative motion and gestures) and not a joystick */
            unsigned long absbits[ABS_MAX / (8 * sizeof(unsigned long)) + 1] = { 0 };
            struct input_absinfo ax, ay;
            const bool pen = test_bit(keybits, BTN_TOOL_PEN) || test_bit(keybits, BTN_STYLUS);
            const bool touchpad = test_bit(keybits, BTN_TOOL_FINGER) && !pen;
            const bool joystick = test_bit(keybits, BTN_TRIGGER) || test_bit(keybits, BTN_A) ||
                                  test_bit(keybits, BTN_1) || test_bit(keybits, BTN_JOYSTICK);
            const bool button = test_bit(keybits, BTN_LEFT) || test_bit(keybits, BTN_TOUCH);
            if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0 && test_bit(absbits, ABS_X) &&
                test_bit(absbits, ABS_Y) && button && !touchpad && !joystick &&
                ioctl(fd, EVIOCGABS(ABS_X), &ax) >= 0 && ioctl(fd, EVIOCGABS(ABS_Y), &ay) >= 0 &&
                ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
                dev->is_pointer = dev->is_abs = true;
                dev->touch_is_left = !test_bit(keybits, BTN_LEFT);
                dev->abs_min[0] = ax.minimum;
                dev->abs_range[0] = ax.maximum - ax.minimum;
                dev->abs_min[1] = ay.minimum;
                dev->abs_range[1] = ay.maximum - ay.minimum;
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
    raw.winsys = winsys;
    pthread_mutex_init(&raw.cb_mutex, NULL);
    atomic_store(&raw.event_count, 0);

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
    raw.winsys = false;
    free((void *)raw.callbacks);
    raw.callbacks = NULL;
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
    return raw.running && raw.winsys;
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
static bool sdlop_vt_handle_chord(const SDLop_RawEvent *ev)
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
    if (!fn || !(chord.modifiers & 3) || !(chord.modifiers & 12) || ev->value != 1) {
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

static void pump_event(const SDLop_RawEvent *ev)
{
    static float hi_res_x_acc, hi_res_y_acc;
    const bool winsys = raw.winsys;
    const SDLOP_RawDevice *dev = ev->device < (Uint32)raw.num_devices ? &raw.devices[ev->device] : NULL;

    if (dev && dev->is_abs) {
        if (ev->type == SDLop_EV_ABS && (ev->code == ABS_X || ev->code == ABS_Y)) {
            const int axis = ev->code == ABS_Y;
            abs_state[dev->index].cur[axis] = ev->value;
            abs_state[dev->index].has_cur[axis] = true;
            abs_state[dev->index].dirty = true;
            return;
        }
        if (ev->type == SDLop_EV_SYN && ev->code == SDLop_SYN_REPORT) {
            abs_motion(dev, ev->timestamp_ns, winsys); /* motion first, then this frame's buttons */
        }
        if (ev->type == SDLop_EV_KEY && ev->code >= BTN_LEFT && ev->code <= BTN_EXTRA) {
            abs_motion(dev, ev->timestamp_ns, winsys); /* click where the pointer is */
        }
        if (ev->type == SDLop_EV_KEY && (ev->code == BTN_TOUCH || ev->code == BTN_STYLUS || ev->code == BTN_STYLUS2)) {
            /* pen tip / touch = left, barrel buttons = right / middle */
            const Uint8 btn = ev->code == BTN_STYLUS ? SDL_BUTTON_RIGHT
                              : ev->code == BTN_STYLUS2 ? SDL_BUTTON_MIDDLE
                              : dev->touch_is_left ? SDL_BUTTON_LEFT : 0;
            if (!btn || ev->value == 2) {
                return;
            }
            abs_motion(dev, ev->timestamp_ns, winsys); /* press where the tip is */
            if (winsys && !winsys_pointer_focus() &&
                !(ev->value == 0 && (sdlop.mouse_buttons & SDL_BUTTON_MASK(btn)))) {
                return;
            }
            SDLOP_SendMouseButton(ev->value != 0, btn, SDLOP_NO_POS, SDLOP_NO_POS, ev->timestamp_ns);
            return;
        }
    }

    switch (ev->type) {
    case SDLop_EV_KEY: {
        if (ev->code >= BTN_LEFT && ev->code <= BTN_EXTRA) {
            Uint8 btn = mouse_button_from_btn(ev->code);
            if (!btn) {
                return;
            }
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
        if (sdlop_vt_handle_chord(ev)) {
            return;
        }
        /* kernel autorepeat (value 2) is dropped here too: SDL's own repeat
           (armed by the press) is the one source of repeats - forwarding
           both doubled them */
        if (raw_console_foreground() && sc != SDL_SCANCODE_UNKNOWN && ev->value != 2) {
            SDLOP_SendKeyboardKey(ev->value != 0, false, sc, ev->code, ev->timestamp_ns);
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

/* tests: one event through the main-thread translation as if the worker
 * had read it, under the window-system or console policy */
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
    dev->is_abs = absolute;
    dev->touch_is_left = touch_is_left;
    dev->abs_min[0] = min_x;
    dev->abs_range[0] = max_x - min_x;
    dev->abs_min[1] = min_y;
    dev->abs_range[1] = max_y - min_y;
    memset(&abs_state[dev->index], 0, sizeof(abs_state[0]));
    return raw.num_devices++;
}

void SDLOP_RawInputTestFeed(const SDLop_RawEvent *ev, bool winsys)
{
    const bool saved = raw.winsys;
    raw.winsys = winsys;
    pump_event(ev);
    raw.winsys = saved;
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
