/* Baseline-compatible SYN_DROPPED regression (no new stats API required).
 * zlib license, see LICENSE. Real worker; private pipe/mock EVIOCGKEY only. */
#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <assert.h>
#include <stdatomic.h>
#define __wrap_ioctl pipe_ioctl
#include "../bench/raw_pipe.c"
#undef __wrap_ioctl

static atomic_int callbacks, queries;

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (synthetic_fd(fd) && _IOC_TYPE(request) == 'E' && _IOC_NR(request) == 0x18) {
        memset(arg, 0, _IOC_SIZE(request)); /* button now up in the kernel */
        atomic_fetch_add(&queries, 1);
        return _IOC_SIZE(request);
    }
    return pipe_ioctl(fd, request, arg);
}

static void on_raw(const SDLop_RawEvent *event, void *userdata)
{
    (void)event; (void)userdata;
    atomic_fetch_add_explicit(&callbacks, 1, memory_order_release);
}

static void wait_callbacks(int count)
{
    Uint64 deadline = SDL_GetTicksNS() + 3000000000ull;
    while (atomic_load_explicit(&callbacks, memory_order_acquire) < count) {
        assert(SDL_GetTicksNS() < deadline);
        SDL_Delay(1);
    }
}

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_RAW_INPUT", "1", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "0", 1);
    UInputDevice *dev = uinput_synth_create("syn-dropped");
    assert(dev && SDL_Init(SDL_INIT_VIDEO));
    assert(SDL_CreateWindow("syn-dropped", 320, 200, 0));
    assert(SDLop_RegisterRawEventCallback(on_raw, NULL));
    SDL_FlushEvents(0, 0xFFFFFFFFu);
    uinput_synth_key(dev, BTN_LEFT, 1);
    wait_callbacks(1);
    SDL_Event event;
    while (SDL_PollEvent(&event)) { }
    assert(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK);
    emit(dev, EV_SYN, SYN_DROPPED, 0);
    uinput_synth_rel(dev, REL_X, 777); /* invalid damaged packet */
    uinput_synth_syn(dev); /* kernel release occurred, but its event was lost */
    wait_callbacks(4);
    int motions = 0, releases = 0;
    Uint64 deadline = SDL_GetTicksNS() + 100000000ull;
    do {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_MOUSE_MOTION) { motions++; }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) { releases++; }
        }
        if (releases && atomic_load(&queries)) { break; }
        SDL_Delay(1);
    } while (SDL_GetTicksNS() < deadline);
    bool stuck = (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) != 0;
    SDLop_RawEvent raw[8];
    int native = SDLop_PollRawEvents(raw, 8);
    bool ok = !stuck && !motions && releases == 1 && atomic_load(&queries) == 1 && native == 4;
    printf("SYN_DROPPED: stuck_button=%d damaged_motions=%d recovery_releases=%d state_queries=%d raw=%d/4 callbacks=%d/4\n",
           stuck, motions, releases, atomic_load(&queries), native, atomic_load(&callbacks));
    SDL_Quit();
    uinput_synth_destroy(dev);
    printf("test_evdev_syn_dropped: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
