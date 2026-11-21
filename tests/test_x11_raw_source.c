/*
  test_x11_raw_source - SDLOP_RAW_INPUT on X11: evdev is optional and never
  mixed with window-system input.

  /dev/input is faked inside this process: opendir("/dev/input"),
  open("/dev/input/eventN") and the EVIOCGBIT ioctls are interposed, each
  fake device is a pipe the test writes struct input_event records into.
  Native input is injected with XTest from a second connection.

  Checks:
    - default (SDLOP_RAW_INPUT unset): no evdev worker on X11, even with a
      keyboard and a pointer readable
    - SDLOP_RAW_INPUT=1 with only a keyboard: all or nothing, so no worker and
      the native keyboard still works
    - SDLOP_RAW_INPUT=1 with keyboard + pointer: evdev is the only source -
      XTest keys, buttons, wheel and motion produce no SDL input events,
      evdev ones do; evdev keys need keyboard focus, evdev pointer input
      needs the pointer over the window; another client's keyboard/pointer
      grab blocks evdev delivery until it ends.

  Skips cleanly when DISPLAY is unset.
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xtest.h>

static int failures = 0;
#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            failures++;                                          \
        }                                                        \
    } while (0)

/* ------------------------------------------------------------------ */
/* fake /dev/input                                                     */
/* ------------------------------------------------------------------ */

enum { FAKE_KBD = 0, FAKE_PTR = 1, FAKE_NUM };

static char fake_dir[64];
static bool fake_present[FAKE_NUM];
static int fake_read_fd[FAKE_NUM] = { -1, -1 };
static int fake_write_fd[FAKE_NUM] = { -1, -1 };

static int fake_index_for_fd(int fd)
{
    for (int i = 0; i < FAKE_NUM; i++) {
        if (fd >= 0 && fake_read_fd[i] == fd) {
            return i;
        }
    }
    return -1;
}

static void fake_reset(bool kbd, bool ptr)
{
    for (int i = 0; i < FAKE_NUM; i++) {
        if (fake_write_fd[i] >= 0) {
            close(fake_write_fd[i]); /* the library closed (or will close) the read end */
        }
        fake_read_fd[i] = fake_write_fd[i] = -1;
    }
    fake_present[FAKE_KBD] = kbd;
    fake_present[FAKE_PTR] = ptr;
    char path[128];
    for (int i = 0; i < FAKE_NUM; i++) {
        snprintf(path, sizeof(path), "%s/event%d", fake_dir, i);
        if (fake_present[i]) {
            int fd = creat(path, 0600);
            if (fd >= 0) {
                close(fd);
            }
        } else {
            unlink(path);
        }
    }
}

typedef DIR *(*opendir_fn)(const char *);
typedef int (*open_fn)(const char *, int, ...);
typedef int (*ioctl_fn)(int, unsigned long, ...);

DIR *opendir(const char *name)
{
    static opendir_fn real;
    if (!real) {
        real = (opendir_fn)dlsym(RTLD_NEXT, "opendir");
    }
    if (fake_dir[0] && strcmp(name, "/dev/input") == 0) {
        return real(fake_dir);
    }
    return real(name);
}

static int fake_open(const char *path, int flags, mode_t mode, const char *sym)
{
    int n = -1;
    if (fake_dir[0] && sscanf(path, "/dev/input/event%d", &n) == 1 && n >= 0 && n < FAKE_NUM) {
        if (!fake_present[n]) {
            errno = ENOENT;
            return -1;
        }
        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0) {
            return -1;
        }
        fcntl(p[0], F_SETFL, O_NONBLOCK);
        if (fake_write_fd[n] >= 0) {
            close(fake_write_fd[n]);
        }
        fake_read_fd[n] = p[0];
        fake_write_fd[n] = p[1];
        return p[0];
    }
    open_fn real = (open_fn)dlsym(RTLD_NEXT, sym);
    return real(path, flags, mode);
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return fake_open(path, flags, mode, "open");
}

int open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return fake_open(path, flags, mode, "open64");
}

static void set_bit(unsigned char *bits, size_t len, unsigned int bit)
{
    if (bit / 8 < len) {
        bits[bit / 8] |= (unsigned char)(1u << (bit % 8));
    }
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    int dev = fake_index_for_fd(fd);
    if (dev >= 0) {
        if (_IOC_TYPE(request) == 'E' && _IOC_NR(request) >= 0x20 && _IOC_NR(request) < 0x40) {
            const unsigned int ev = _IOC_NR(request) - 0x20;
            const size_t len = _IOC_SIZE(request);
            unsigned char *bits = (unsigned char *)arg;
            memset(bits, 0, len);
            if (ev == 0) {
                set_bit(bits, len, EV_SYN);
                set_bit(bits, len, dev == FAKE_KBD ? EV_KEY : EV_REL);
                if (dev == FAKE_PTR) {
                    set_bit(bits, len, EV_KEY);
                }
            } else if (ev == EV_KEY) {
                if (dev == FAKE_KBD) {
                    for (unsigned int k = KEY_ESC; k <= KEY_KPDOT; k++) {
                        set_bit(bits, len, k);
                    }
                } else {
                    set_bit(bits, len, BTN_LEFT);
                    set_bit(bits, len, BTN_RIGHT);
                    set_bit(bits, len, BTN_MIDDLE);
                }
            } else if (ev == EV_REL && dev == FAKE_PTR) {
                set_bit(bits, len, REL_X);
                set_bit(bits, len, REL_Y);
                set_bit(bits, len, REL_WHEEL);
            }
            return (int)len;
        }
        errno = ENOTTY;
        return -1;
    }
    static ioctl_fn real;
    if (!real) {
        real = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
    }
    return real(fd, request, arg);
}

static void fake_emit(int dev, unsigned short type, unsigned short code, int value)
{
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ie.input_event_sec = ts.tv_sec;
    ie.input_event_usec = ts.tv_nsec / 1000;
    ie.type = type;
    ie.code = code;
    ie.value = value;
    if (fake_write_fd[dev] >= 0) {
        ssize_t unused = write(fake_write_fd[dev], &ie, sizeof(ie));
        (void)unused;
    }
}

static void fake_key_ev(unsigned short code, int value)
{
    fake_emit(FAKE_KBD, EV_KEY, code, value);
    fake_emit(FAKE_KBD, EV_SYN, SYN_REPORT, 0);
}

static void fake_ptr_ev(unsigned short type, unsigned short code, int value)
{
    fake_emit(FAKE_PTR, type, code, value);
    fake_emit(FAKE_PTR, EV_SYN, SYN_REPORT, 0);
}

/* ------------------------------------------------------------------ */
/* X helpers                                                           */
/* ------------------------------------------------------------------ */

static xcb_connection_t *obs;
static xcb_screen_t *obs_screen;

static void xfake(uint8_t type, uint8_t detail, int16_t x, int16_t y, xcb_window_t root)
{
    xcb_test_fake_input(obs, type, detail, XCB_CURRENT_TIME, root, x, y, 0);
    xcb_flush(obs);
}

static void x_sync(void)
{
    free(xcb_get_input_focus_reply(obs, xcb_get_input_focus(obs), NULL));
}

static void pump_ms(int ms)
{
    Uint64 end = SDL_GetTicks() + (Uint64)ms;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        SDL_Delay(5);
    }
}

static void drain(void)
{
    x_sync();
    pump_ms(60);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
}

/* input event counts seen over ms milliseconds */
typedef struct
{
    int keys, buttons, motions, wheels;
    SDL_Event last_key, last_button, last_motion, last_wheel;
} Seen;

static Seen collect(int ms)
{
    Seen s;
    memset(&s, 0, sizeof(s));
    x_sync();
    Uint64 end = SDL_GetTicks() + (Uint64)ms;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                s.keys++;
                s.last_key = e;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                s.buttons++;
                s.last_button = e;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                s.motions++;
                s.last_motion = e;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                s.wheels++;
                s.last_wheel = e;
                break;
            default:
                break;
            }
        }
        SDL_Delay(5);
    }
    return s;
}

static bool wait_focus(SDL_Window *want, int ms)
{
    Uint64 end = SDL_GetTicks() + (Uint64)ms;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        if (SDL_GetKeyboardFocus() == want) {
            return true;
        }
        SDL_Delay(5);
    }
    return SDL_GetKeyboardFocus() == want;
}

static bool wait_mouse_focus(SDL_Window *want, int ms)
{
    Uint64 end = SDL_GetTicks() + (Uint64)ms;
    while (SDL_GetTicks() < end) {
        SDL_PumpEvents();
        if (SDL_GetMouseFocus() == want) {
            return true;
        }
        SDL_Delay(5);
    }
    return SDL_GetMouseFocus() == want;
}

static bool init_x11(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */

static void test_default_off(void)
{
    fake_reset(true, true);
    if (!init_x11()) {
        failures++;
        return;
    }
    CHECK(!SDLop_RawInputAvailable(), "default X11: no evdev worker although keyboard+pointer are readable");
    SDL_Quit();
}

static void test_keyboard_only_falls_back(void)
{
    fake_reset(true, false);
    if (!init_x11()) {
        failures++;
        return;
    }
    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1"); /* too late: options are read at init */
    CHECK(!SDLop_RawInputAvailable(), "hint after SDL_Init does not start evdev");
    SDL_Quit();

    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1");
    if (!init_x11()) {
        failures++;
        return;
    }
    CHECK(!SDLop_RawInputAvailable(), "SDLOP_RAW_INPUT=1 with keyboard but no pointer: not started (never mixed)");

    SDL_Window *w = SDL_CreateWindow("raw source kb-only", 300, 200, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (w && wait_focus(w, 2000)) {
        drain();
        xfake(XCB_KEY_PRESS, 38, 0, 0, XCB_NONE); /* KEY_A */
        xfake(XCB_KEY_RELEASE, 38, 0, 0, XCB_NONE);
        Seen s = collect(300);
        CHECK(s.keys == 2, "native keyboard still works in the fallback (%d key events)", s.keys);
        fake_key_ev(KEY_B, 1); /* nobody reads it */
        fake_key_ev(KEY_B, 0);
        s = collect(200);
        CHECK(s.keys == 0, "no evdev keys in the fallback (%d)", s.keys);
    } else {
        CHECK(false, "window never got keyboard focus");
    }
    SDL_Quit();
}

static void test_raw_source(void)
{
    fake_reset(true, true);
    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1");
    if (!init_x11()) {
        failures++;
        return;
    }
    CHECK(SDLop_RawInputAvailable(), "SDLOP_RAW_INPUT=1 with keyboard+pointer starts evdev");
    if (!SDLop_RawInputAvailable()) {
        SDL_Quit();
        return;
    }
    SDL_Window *w = SDL_CreateWindow("raw source", 400, 300, 0);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    if (!w || !wait_focus(w, 2000)) {
        CHECK(false, "window never got keyboard focus");
        SDL_Quit();
        return;
    }
    SDL_SetWindowPosition(w, 50, 60);
    pump_ms(100);
    int wx = 0, wy = 0;
    SDL_GetWindowPosition(w, &wx, &wy);

    /* pointer into the window: enter anchors the position, no native motion */
    drain();
    xfake(XCB_MOTION_NOTIFY, 0, (int16_t)(wx + 100), (int16_t)(wy + 120), obs_screen->root);
    CHECK(wait_mouse_focus(w, 1000), "mouse focus after native enter");
    xfake(XCB_MOTION_NOTIFY, 0, (int16_t)(wx + 110), (int16_t)(wy + 125), obs_screen->root);
    Seen s = collect(250);
    CHECK(s.motions == 0, "native motion suppressed (%d motion events)", s.motions);

    /* native keys / buttons / wheel produce nothing */
    xfake(XCB_KEY_PRESS, 38, 0, 0, XCB_NONE);
    xfake(XCB_KEY_RELEASE, 38, 0, 0, XCB_NONE);
    xfake(XCB_BUTTON_PRESS, 1, 0, 0, XCB_NONE);
    xfake(XCB_BUTTON_RELEASE, 1, 0, 0, XCB_NONE);
    xfake(XCB_BUTTON_PRESS, 4, 0, 0, XCB_NONE);
    xfake(XCB_BUTTON_RELEASE, 4, 0, 0, XCB_NONE);
    s = collect(300);
    CHECK(s.keys == 0, "native keys suppressed (%d)", s.keys);
    CHECK(s.buttons == 0, "native buttons suppressed (%d)", s.buttons);
    CHECK(s.wheels == 0, "native wheel suppressed (%d)", s.wheels);

    /* evdev keys / buttons / wheel / motion are delivered */
    fake_key_ev(KEY_B, 1);
    fake_key_ev(KEY_B, 0);
    s = collect(250);
    CHECK(s.keys == 2 && s.last_key.key.scancode == SDL_SCANCODE_B, "evdev key B delivered (%d)", s.keys);
    fake_ptr_ev(EV_KEY, BTN_LEFT, 1);
    fake_ptr_ev(EV_KEY, BTN_LEFT, 0);
    fake_ptr_ev(EV_REL, REL_WHEEL, 1);
    fake_ptr_ev(EV_REL, REL_X, 7);
    s = collect(250);
    CHECK(s.buttons == 2 && s.last_button.button.button == SDL_BUTTON_LEFT, "evdev buttons delivered (%d)", s.buttons);
    CHECK(s.wheels == 1 && s.last_wheel.wheel.y == 1.0f, "evdev wheel delivered (%d)", s.wheels);
    CHECK(s.motions == 1 && s.last_motion.motion.xrel == 7.0f, "evdev motion delivered (%d, xrel %g)",
          s.motions, s.motions ? s.last_motion.motion.xrel : 0.0f);
    if (s.motions) {
        CHECK((int)s.last_motion.motion.x == 117 && (int)s.last_motion.motion.y == 125,
              "evdev motion continues from the native anchor (%g,%g)", s.last_motion.motion.x,
              s.last_motion.motion.y);
    }

    /* another client grabs the keyboard (screen locker style) */
    xcb_grab_keyboard_reply_t *gk = xcb_grab_keyboard_reply(
        obs, xcb_grab_keyboard(obs, 1, obs_screen->root, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), NULL);
    CHECK(gk && gk->status == XCB_GRAB_STATUS_SUCCESS, "observer keyboard grab");
    free(gk);
    pump_ms(100);
    fake_key_ev(KEY_C, 1);
    fake_key_ev(KEY_C, 0);
    s = collect(250);
    CHECK(s.keys == 0, "no evdev keys while another client grabs the keyboard (%d)", s.keys);
    xcb_ungrab_keyboard(obs, XCB_CURRENT_TIME);
    xcb_flush(obs);
    pump_ms(100);
    fake_key_ev(KEY_C, 1);
    fake_key_ev(KEY_C, 0);
    s = collect(250);
    CHECK(s.keys == 2, "evdev keys again after the grab ends (%d)", s.keys);

    /* another client grabs the pointer (WM menu / move style) */
    xcb_grab_pointer_reply_t *gp = xcb_grab_pointer_reply(
        obs, xcb_grab_pointer(obs, 0, obs_screen->root, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_ASYNC,
                              XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
    CHECK(gp && gp->status == XCB_GRAB_STATUS_SUCCESS, "observer pointer grab");
    free(gp);
    pump_ms(100);
    fake_ptr_ev(EV_KEY, BTN_LEFT, 1);
    fake_ptr_ev(EV_KEY, BTN_LEFT, 0);
    fake_ptr_ev(EV_REL, REL_X, 3);
    s = collect(250);
    CHECK(s.buttons == 0 && s.motions == 0, "no evdev pointer input while another client grabs it (%d buttons, %d motions)",
          s.buttons, s.motions);
    xcb_ungrab_pointer(obs, XCB_CURRENT_TIME);
    xcb_flush(obs);
    pump_ms(100);
    fake_ptr_ev(EV_KEY, BTN_LEFT, 1);
    fake_ptr_ev(EV_KEY, BTN_LEFT, 0);
    s = collect(250);
    CHECK(s.buttons == 2, "evdev buttons again after the grab ends (%d)", s.buttons);

    /* held key survives a focus loss without sticking */
    fake_key_ev(KEY_D, 1);
    s = collect(150);
    CHECK(s.keys == 1, "evdev D down (%d)", s.keys);

    /* keyboard focus to another client's window: no evdev keys */
    xcb_window_t other = xcb_generate_id(obs);
    const uint32_t other_vals[] = { obs_screen->white_pixel };
    xcb_create_window(obs, XCB_COPY_FROM_PARENT, other, obs_screen->root, 900, 700, 200, 150, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, obs_screen->root_visual, XCB_CW_BACK_PIXEL, other_vals);
    xcb_map_window(obs, other);
    x_sync();
    for (int i = 0; i < 100; i++) { /* a WM may reparent/map it asynchronously */
        xcb_get_window_attributes_reply_t *a =
            xcb_get_window_attributes_reply(obs, xcb_get_window_attributes(obs, other), NULL);
        const bool v = a && a->map_state == XCB_MAP_STATE_VIEWABLE;
        free(a);
        if (v) {
            break;
        }
        pump_ms(10);
    }
    xcb_set_input_focus(obs, XCB_INPUT_FOCUS_PARENT, other, XCB_CURRENT_TIME);
    xcb_flush(obs);
    CHECK(wait_focus(NULL, 1000), "keyboard focus left the window");
    CHECK(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_D], "held key released on focus loss");
    drain();
    fake_key_ev(KEY_D, 0); /* release of a key SDL no longer holds: dropped */
    fake_key_ev(KEY_E, 1);
    fake_key_ev(KEY_E, 0);
    s = collect(250);
    CHECK(s.keys == 0, "no evdev keys without keyboard focus (%d)", s.keys);

    /* pointer outside the window: no evdev pointer input */
    xfake(XCB_MOTION_NOTIFY, 0, (int16_t)(wx + 700), (int16_t)(wy + 600), obs_screen->root);
    CHECK(wait_mouse_focus(NULL, 1000), "mouse focus left the window");
    drain();
    fake_ptr_ev(EV_KEY, BTN_LEFT, 1);
    fake_ptr_ev(EV_KEY, BTN_LEFT, 0);
    fake_ptr_ev(EV_REL, REL_X, 3);
    fake_ptr_ev(EV_REL, REL_WHEEL, -1);
    s = collect(250);
    CHECK(s.buttons == 0 && s.motions == 0 && s.wheels == 0,
          "no evdev pointer input outside the window (%d buttons, %d motions, %d wheels)", s.buttons, s.motions,
          s.wheels);

    xcb_destroy_window(obs, other);
    xcb_flush(obs);
    SDL_Quit();
}

int main(void)
{
    if (!getenv("DISPLAY")) {
        printf("test_x11_raw_source: SKIP (DISPLAY not set)\n");
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    SDL_unsetenv_unsafe("SDLOP_RAW_INPUT");
    SDL_unsetenv_unsafe("SDLOP_DISABLE_RAW_INPUT");

    int screen_num = 0;
    obs = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(obs)) {
        printf("test_x11_raw_source: SKIP (cannot connect to %s)\n", getenv("DISPLAY"));
        return 0;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(obs));
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    obs_screen = it.data;

    snprintf(fake_dir, sizeof(fake_dir), "/tmp/sdlop-fake-input-XXXXXX");
    if (!mkdtemp(fake_dir)) {
        perror("mkdtemp");
        return 1;
    }

    test_default_off();
    test_keyboard_only_falls_back();
    test_raw_source();

    fake_reset(false, false);
    rmdir(fake_dir);
    xcb_disconnect(obs);
    if (failures) {
        fprintf(stderr, "test_x11_raw_source: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_x11_raw_source: OK\n");
    return 0;
}
