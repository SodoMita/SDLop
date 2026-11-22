/* Positive regressions for the input lifecycle audit. Real-worker rootless
 * transport; no devices, new dependencies or production fault hooks.
 * zlib license, see LICENSE. */
#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include "internal/sdlop_internal.h"
#include <assert.h>
#include <stdatomic.h>
#define __wrap_ioctl pipe_ioctl
#include "../bench/raw_pipe.c"
#undef __wrap_ioctl
static bool highres;
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    int result = pipe_ioctl(fd, request, arg);
    if (result >= 0 && highres && synthetic_fd(fd) && _IOC_NR(request) == 0x20 + EV_REL) {
        set_bit(arg, _IOC_SIZE(request), REL_WHEEL_HI_RES);
    }
    return result;
}

static atomic_bool removed;
static atomic_uint dead_reads;
static unsigned sent;
extern ssize_t __real_read(int fd, void *buffer, size_t bytes);
ssize_t __wrap_read(int fd, void *buffer, size_t bytes)
{
    if (atomic_load(&removed) && synthetic_fd(fd)) {
        atomic_fetch_add(&dead_reads, 1);
        errno = ENODEV; /* an evdev removal error on a persistently ready fd */
        return -1;
    }
    return __real_read(fd, buffer, bytes);
}
static void send_event(unsigned type, unsigned code, int value)
{
    struct input_event e = { .type = type, .code = code, .value = value };
    assert(input_write_record(active->write_fd, &e, sizeof(e)));
    sent++;
}
static void drain(void)
{
    Uint64 until = SDL_GetTicks() + 3000;
    while (SDLop_GetRawEventCount() < sent && SDL_GetTicks() < until) { SDL_Delay(1); }
    assert(SDLop_GetRawEventCount() == sent);
    SDL_PumpEvents();
}
static int count_events(Uint32 type)
{
    return SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, type, type);
}
static void tap(SDL_Scancode sc, Uint16 raw)
{
    SDLOP_SendKeyboardKey(true, false, sc, raw, 0);
    SDLOP_SendKeyboardKey(false, false, sc, raw, 0);
}
static void get_text(char *text, size_t size)
{
    text[0] = 0;
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, 0, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_TEXT_INPUT) {
            strncat(text, e.text.text, size - strlen(text) - 1);
        }
    }
}
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s unplug|repeat-grab|wheel|wheel-focus|key-alias|compose\n", argv[0]);
        return 2;
    }
    bool compose = !strcmp(argv[1], "compose");
    highres = !strcmp(argv[1], "wheel") || !strcmp(argv[1], "wheel-focus");
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_RAW_INPUT", "1", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "0", 1);
    setenv("XKB_DEFAULT_LAYOUT", compose ? "de" : "us", 1);
    UInputDevice *device = uinput_synth_create("input-audit");
    assert(device && SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *window = SDL_CreateWindow("input audit", 320, 200, 0);
    assert(window);
    SDLOP_RawInputQuit();
    sdlop.video->window_system_input = true; /* model backend focus/grab policy */
    assert(SDLOP_RawInputInit());
    SDL_FlushEvents(0, SDL_EVENT_LAST);

    if (!strcmp(argv[1], "unplug")) {
        send_event(EV_KEY, KEY_A, 1);
        send_event(EV_KEY, BTN_LEFT, 1); drain();
        assert(SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A]);
        atomic_store(&removed, true);
        close(active->write_fd); /* HUP readiness remains, as with a dead device */
        active->write_fd = -1;
        SDL_Delay(50);
        SDL_PumpEvents();
        unsigned reads = atomic_load(&dead_reads);
        bool key = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_A];
        bool button = (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) != 0;
        bool source = SDLOP_RawInputIsSource();
        printf("unplug: reads_returning_ENODEV_in_50ms=%u held_A=%d held_left=%d raw_source=%d\n",
               reads, key, button, source);
        assert(reads <= 2 && !key && !button && !source);
    } else if (!strcmp(argv[1], "repeat-grab")) {
        assert(SDL_StartTextInput(window));
        send_event(EV_KEY, KEY_A, 1); drain();
        if (!sdlop.repeat_active) {
            puts("SKIP: repeat requires xkbcommon repeat metadata");
            SDL_Quit(); uinput_synth_destroy(device); return 0;
        }
        SDL_FlushEvents(0, SDL_EVENT_LAST);
        /* X11 FocusOut(Grab) sets this without clearing keyboard_focus or
         * repeat_active. No server is simulated: exercise its resulting state. */
        sdlop.keyboard_grabbed_elsewhere = true;
        sdlop.repeat_next_ns = SDLOP_MonotonicNS() - 1;
        SDL_PumpEvents();
        int keys = count_events(SDL_EVENT_KEY_DOWN), texts = count_events(SDL_EVENT_TEXT_INPUT);
        printf("repeat-grab: keyboard_grabbed_elsewhere=1 repeated_keys=%d text_events=%d\n", keys, texts);
        assert(keys == 0 && texts == 0 && !sdlop.repeat_active);
        sdlop.keyboard_grabbed_elsewhere = false;
    } else if (!strcmp(argv[1], "wheel") || !strcmp(argv[1], "wheel-focus")) {
        bool focus = !strcmp(argv[1], "wheel-focus");
        send_event(EV_REL, REL_WHEEL_HI_RES, focus ? 60 : 120);
        if (focus) {
            drain();
            assert(count_events(SDL_EVENT_MOUSE_WHEEL) == 0);
            sdlop.keyboard_focus = sdlop.mouse_focus = NULL;
        } else { send_event(EV_REL, REL_WHEEL, 1); }
        send_event(EV_SYN, SYN_REPORT, 0); drain();
        float y = 0;
        int count = 0;
        SDL_Event e;
        while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, 0, SDL_EVENT_LAST) == 1) {
            if (e.type == SDL_EVENT_MOUSE_WHEEL) { count++; y += e.wheel.y; }
        }
        printf("%s: wheel_events=%d total_y=%.2f focus_present=%d\n", argv[1], count, y, sdlop.mouse_focus != NULL);
        assert(focus ? count == 0 : (count == 1 && y == 1.0f));
        sdlop.keyboard_focus = sdlop.mouse_focus = window;
        if (!focus) {
            send_event(EV_REL, REL_WHEEL, -1); /* ignored: vertical has high-res */
            send_event(EV_REL, REL_HWHEEL, -2); /* horizontal stays legacy */
            send_event(EV_REL, REL_WHEEL_HI_RES, -30);
            send_event(EV_SYN, SYN_REPORT, 0); drain();
            float x = 0; y = 0; count = 0;
            while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, 0, SDL_EVENT_LAST) == 1) {
                if (e.type == SDL_EVENT_MOUSE_WHEEL) { count++; x += e.wheel.x; y += e.wheel.y; }
            }
            assert(count == 2 && x == -2 && y == -0.25f);
        } else {
            send_event(EV_REL, REL_WHEEL_HI_RES, 60); drain();
            SDLOP_RawInputResetPointerFrame(); /* real backend leave/grab hook */
            send_event(EV_SYN, SYN_REPORT, 0); drain();
            assert(count_events(SDL_EVENT_MOUSE_WHEEL) == 0);
        }
        sdlop.keyboard_focus = sdlop.mouse_focus = window;
    } else if (!strcmp(argv[1], "key-alias")) {
        /* Linux KEY_SYSRQ=99 and KEY_PRINT=210 both map to PRINTSCREEN. */
        send_event(EV_KEY, KEY_SYSRQ, 1); send_event(EV_KEY, KEY_PRINT, 1); drain();
        assert(SDL_GetKeyboardState(NULL)[SDL_SCANCODE_PRINTSCREEN]);
        SDL_FlushEvents(0, SDL_EVENT_LAST);
        send_event(EV_KEY, KEY_SYSRQ, 0); drain();
        bool held = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_PRINTSCREEN];
        int ups = count_events(SDL_EVENT_KEY_UP);
        printf("key-alias: KEY_PRINT_still_down=1 SDL_PRINTSCREEN_down=%d premature_keyups=%d\n", held, ups);
        assert(held && ups == 0);
        send_event(EV_KEY, KEY_PRINT, 0); drain();
        assert(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_PRINTSCREEN]);
        assert(count_events(SDL_EVENT_KEY_UP) == 1);
    } else if (compose) {
        if (!SDLOP_KeyboardSetDefaultKeymap()) {
            puts("SKIP: composition requires xkbcommon and German layout/Compose data");
            SDL_Quit(); uinput_synth_destroy(device); return 0;
        }
        /* German '=' position is dead_acute. First verify the real table. */
        char text[64];
        assert(SDL_StartTextInput(window));
        tap(SDL_SCANCODE_EQUALS, 13); tap(SDL_SCANCODE_E, 18); get_text(text, sizeof(text));
        if (strcmp(text, "é")) {
            puts("SKIP: German Compose data unavailable");
            SDL_Quit(); uinput_synth_destroy(device); return 0;
        }
        assert(SDL_StopTextInput(window));
        tap(SDL_SCANCODE_EQUALS, 13); /* outside a text session */
        assert(SDL_StartTextInput(window));
        tap(SDL_SCANCODE_E, 18); get_text(text, sizeof(text));
        bool disabled_leak = !strcmp(text, "é");
        printf("compose: dead_key_while_disabled_then_start_E='%s' (wanted 'e')\n", text);
        tap(SDL_SCANCODE_EQUALS, 13);
        assert(SDL_StopTextInput(window) && SDL_StartTextInput(window));
        tap(SDL_SCANCODE_E, 18); get_text(text, sizeof(text));
        bool restart_leak = !strcmp(text, "é");
        printf("compose: dead_key_then_stop_start_E='%s' (wanted 'e')\n", text);
        tap(SDL_SCANCODE_EQUALS, 13);
        assert(SDL_ClearComposition(window));
        tap(SDL_SCANCODE_E, 18); get_text(text, sizeof(text));
        bool clear_leak = !strcmp(text, "é");
        printf("compose: dead_key_then_ClearComposition_E='%s' (wanted 'e')\n", text);
        assert(!disabled_leak && !restart_leak && !clear_leak);
        assert(!strcmp(text, "e"));
    } else { fprintf(stderr, "unknown probe\n"); return 2; }
    SDLOP_RawInputQuit(); /* stop the worker before destroying the transport */
    SDL_Quit();
    uinput_synth_destroy(device);
    printf("%s: PASS\n", argv[1]);
    return 0;
}
