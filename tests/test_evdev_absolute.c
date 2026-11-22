/* Real worker, rootless native-event transport: absolute pointer discovery,
 * SDL coordinates/focus/recovery and repeat TEXT_INPUT. No production hooks.
 * zlib license, see LICENSE. */
#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include "internal/sdlop_internal.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#define __wrap_ioctl pipe_ioctl
#include "../bench/raw_pipe.c"
#undef __wrap_ioctl

enum Device { MOUSE, PEN, JOYSTICK, DIRECT, TOUCHPAD, MULTITOUCH, BAD_RANGE, RELATIVE };
static enum Device kind;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int physical[2] = { 100, -100 };
static atomic_int abs_callbacks;
static unsigned sent;

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (!synthetic_fd(fd)) { return __real_ioctl(fd, request, arg); }
    if (kind == RELATIVE) { return pipe_ioctl(fd, request, arg); }
    unsigned nr = _IOC_NR(request);
    size_t len = _IOC_SIZE(request);
    if (_IOC_TYPE(request) == 'E' && nr >= 0x20 && nr < 0x40) {
        unsigned char *bits = arg;
        memset(bits, 0, len);
        switch (nr - 0x20) {
        case 0:
            set_bit(bits, len, EV_SYN); set_bit(bits, len, EV_KEY); set_bit(bits, len, EV_ABS); break;
        case EV_KEY:
            set_bit(bits, len, KEY_A); set_bit(bits, len, KEY_W);
            if (kind == PEN) { set_bit(bits, len, BTN_TOOL_PEN); set_bit(bits, len, BTN_TOUCH); }
            else { set_bit(bits, len, kind == JOYSTICK ? BTN_JOYSTICK : BTN_LEFT); }
            if (kind == TOUCHPAD) { set_bit(bits, len, BTN_TOOL_FINGER); }
            break;
        case EV_ABS:
            set_bit(bits, len, ABS_X); set_bit(bits, len, ABS_Y);
            if (kind == MULTITOUCH) { set_bit(bits, len, ABS_MT_POSITION_X); }
            break;
        }
        return (int)len;
    }
    if (request == EVIOCGPROP(len)) {
        memset(arg, 0, len);
        if (kind == DIRECT) { set_bit(arg, len, INPUT_PROP_DIRECT); }
        return (int)len;
    }
    if (request == EVIOCGABS(ABS_X) || request == EVIOCGABS(ABS_Y)) {
        int axis = request == EVIOCGABS(ABS_Y);
        pthread_mutex_lock(&state_lock);
        *(struct input_absinfo *)arg = (struct input_absinfo){
            .value = physical[axis], .minimum = axis ? -100 : 100,
            .maximum = kind == BAD_RANGE ? 100 : (axis ? 900 : 1100)
        };
        pthread_mutex_unlock(&state_lock);
        return 0;
    }
    if (nr == 0x18) { memset(arg, 0, len); return (int)len; }
    return pipe_ioctl(fd, request, arg);
}
static void callback(const SDLop_RawEvent *event, void *userdata)
{
    (void)userdata;
    if (event->type == EV_ABS) { atomic_fetch_add(&abs_callbacks, 1); }
}
static void send_event(unsigned type, unsigned code, int value)
{
    struct input_event e = { .type = type, .code = code, .value = value };
    assert(input_write_record(active->write_fd, &e, sizeof(e)));
    sent++;
}
static void drain(void)
{
    Uint64 deadline = SDL_GetTicks() + 3000;
    while (SDLop_GetRawEventCount() < sent && SDL_GetTicks() < deadline) { SDL_Delay(1); }
    assert(SDLop_GetRawEventCount() == sent);
    SDL_PumpEvents();
}
static void position(float x, float y)
{
    float a, b;
    SDL_GetMouseState(&a, &b);
    assert(fabsf(a - x) < 0.01f && fabsf(b - y) < 0.01f);
}
static void move(int x, int y)
{
    pthread_mutex_lock(&state_lock);
    physical[0] = x; physical[1] = y;
    pthread_mutex_unlock(&state_lock);
    send_event(EV_ABS, ABS_X, x); send_event(EV_ABS, ABS_Y, y);
    send_event(EV_SYN, SYN_REPORT, 0); drain();
}
static void count_text(int expected_down, int expected_text)
{
    int down = 0, text = 0;
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, 0, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_KEY_DOWN) { down++; }
        if (e.type == SDL_EVENT_TEXT_INPUT) { assert(!strcmp(e.text.text, "a")); text++; }
    }
    assert(down == expected_down && text == expected_text);
}
int main(int argc, char **argv)
{
    bool repeat_only = argc == 2 && !strcmp(argv[1], "--repeat-only");
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("XKB_DEFAULT_LAYOUT", "us", 1);
    setenv("SDLOP_RAW_INPUT", "1", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "0", 1);
    UInputDevice *dev = uinput_synth_create("absolute");
    assert(dev && SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *w = SDL_CreateWindow("absolute", 321, 201, 0);
    assert(w);
    SDLOP_RawInputQuit();
    sdlop.video->window_system_input = true;
    for (kind = JOYSTICK; !repeat_only && kind <= BAD_RANGE; kind++) {
        assert(!SDLOP_RawInputInit()); /* keyboard alone cannot enable raw source */
        assert(!SDLOP_RawInputIsSource());
    }
    for (kind = repeat_only ? RELATIVE : MOUSE; kind <= (repeat_only ? RELATIVE : PEN); kind++) {
        assert(SDLOP_RawInputInit() && SDLOP_RawInputIsSource());
        sent = 0;
        assert(SDLop_RegisterRawEventCallback(callback, NULL));
        if (!repeat_only) {
            move(600, 400); position(160, 100);
            move(9999, -9999); position(320, 0);
            SDL_FlushEvents(0, SDL_EVENT_LAST);
            send_event(EV_ABS, ABS_X, 100); drain();
            assert(!SDL_HasEvent(SDL_EVENT_MOUSE_MOTION)); /* wait for the report */
            send_event(EV_ABS, ABS_Y, 900);
            send_event(EV_KEY, kind == PEN ? BTN_TOUCH : BTN_LEFT, 1);
            send_event(EV_SYN, SYN_REPORT, 0); drain(); position(0, 200);
            SDL_Event e;
            assert(SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_DOWN) == 1);
            assert(e.button.button == SDL_BUTTON_LEFT && e.button.x == 0 && e.button.y == 200);
            send_event(EV_KEY, kind == PEN ? BTN_TOUCH : BTN_LEFT, 0); drain();
            SDL_FlushEvents(0, SDL_EVENT_LAST);
            sdlop.mouse_focus = NULL; sdlop.keyboard_focus = NULL;
            move(600, 400); position(0, 200);
            assert(!SDL_HasEvent(SDL_EVENT_MOUSE_MOTION));
            sdlop.mouse_focus = sdlop.keyboard_focus = w;
            move(600, 400); position(160, 100);
            assert(SDL_SetWindowRelativeMouseMode(w, true));
            move(700, 500); /* normalized delta, no absolute warp */
            position(192, 120);
            assert(SDL_SetWindowRelativeMouseMode(w, false));
            move(600, 400); position(160, 100);
            pthread_mutex_lock(&state_lock);
            physical[0] = 1100; physical[1] = 900;
            pthread_mutex_unlock(&state_lock);
            send_event(EV_SYN, SYN_DROPPED, 0);
            send_event(EV_ABS, ABS_X, 123); /* damaged frame must be discarded */
            send_event(EV_SYN, SYN_REPORT, 0); drain();
            Uint64 until = SDL_GetTicks() + 3000;
            float x, y;
            do { SDL_PumpEvents(); SDL_GetMouseState(&x, &y); SDL_Delay(1); }
            while ((x != 320 || y != 200) && SDL_GetTicks() < until);
            position(320, 200);
            assert(atomic_load(&abs_callbacks) >= 13); /* unfiltered native ABS records */
        }
        /* Existing live xkb text path must generate text on every software
         * repeat. Kernel value=2 must not create a second repeat stream. */
        if (SDLOP_KeyboardSetDefaultKeymap()) {
            assert(SDL_StartTextInput(w));
            SDL_FlushEvents(0, SDL_EVENT_LAST);
            send_event(EV_KEY, KEY_A, 1); drain(); count_text(1, 1);
            for (int i = 0; i < 20; i++) { send_event(EV_KEY, KEY_A, 2); }
            drain(); count_text(0, 0);
            for (int i = 0; i < 20; i++) {
                sdlop.repeat_next_ns = SDLOP_MonotonicNS() - 1;
                SDLOP_KeyboardProcessRepeats(); count_text(1, 1);
            }
            send_event(EV_KEY, KEY_A, 0); drain();
            assert(!sdlop.repeat_active);
            puts("PASS: real-worker raw repeat emits one TEXT_INPUT per repeat, no kernel duplicates");
        } else { puts("SKIP: repeat text requires libxkbcommon and a default US keymap"); }
        SDLOP_RawInputQuit();
    }
    SDL_Quit(); uinput_synth_destroy(dev);
    if (!repeat_only) {
        puts("PASS: absolute discovery, frames, coordinates, pen contact, focus, relative deltas, SYN_DROPPED");
    }
    return 0;
}
