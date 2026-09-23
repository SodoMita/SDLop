/*
  SDLop example: asyncinput-style low-latency WASD movement.

  A raw-event callback runs on the input worker thread (like
  ni_register_callback in asyncinput) and flips movement bits; the main
  loop integrates them. Zero allocation, zero locks on the hot path.
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <stdatomic.h>
#include <stdio.h>

static atomic_uint movement; /* bit0=W bit1=A bit2=S bit3=D */
static atomic_bool quit_requested;

static void on_raw_event(const SDLop_RawEvent *ev, void *userdata)
{
    (void)userdata;
    if (ev->type != SDLop_EV_KEY) {
        return;
    }
    unsigned bit = 0;
    switch (ev->code) {
    case SDLop_KEY_W: bit = 1u << 0; break;
    case SDLop_KEY_A: bit = 1u << 1; break;
    case SDLop_KEY_S: bit = 1u << 2; break;
    case SDLop_KEY_D: bit = 1u << 3; break;
    case SDLop_KEY_ESC:
        if (ev->value) {
            atomic_store(&quit_requested, true);
        }
        return;
    default:
        return;
    }
    if (ev->value) {
        atomic_fetch_or(&movement, bit);
    } else {
        atomic_fetch_and(&movement, ~bit);
    }
}

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("SDLop - WASD (raw callback)", 640, 480, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (SDLop_RawInputAvailable()) {
        SDLop_RegisterRawEventCallback(on_raw_event, NULL);
        printf("raw input active: callback drives movement (WASD, ESC quits)\n");
    } else {
        printf("raw input unavailable (%s); using SDL event queue\n", SDL_GetError());
        SDL_ClearError();
    }

    float x = 0.0f, y = 0.0f;
    Uint64 last = SDL_GetTicksNS();
    bool done = false;

    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                done = true;
            }
            if (!SDLop_RawInputAvailable()) {
                /* fallback path: drive movement from the SDL event queue */
                if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                    unsigned bit = 0;
                    switch (event.key.scancode) {
                    case SDL_SCANCODE_W: bit = 1u << 0; break;
                    case SDL_SCANCODE_A: bit = 1u << 1; break;
                    case SDL_SCANCODE_S: bit = 1u << 2; break;
                    case SDL_SCANCODE_D: bit = 1u << 3; break;
                    case SDL_SCANCODE_ESCAPE: done = true; break;
                    default: break;
                    }
                    if (bit) {
                        unsigned m = atomic_load(&movement);
                        m = event.key.down ? (m | bit) : (m & ~bit);
                        atomic_store(&movement, m);
                    }
                }
            }
        }

        Uint64 now = SDL_GetTicksNS();
        float dt = (float)(now - last) / 1e9f;
        last = now;

        unsigned m = atomic_load(&movement);
        float vx = ((m & 2) ? -1.0f : 0.0f) + ((m & 8) ? 1.0f : 0.0f);
        float vy = ((m & 1) ? -1.0f : 0.0f) + ((m & 4) ? 1.0f : 0.0f);
        if (vx || vy) {
            x += vx * 200.0f * dt;
            y += vy * 200.0f * dt;
            printf("\rposition: %7.1f, %7.1f", x, y);
            fflush(stdout);
        }
        if (atomic_load(&quit_requested)) {
            done = true;
        }
        SDL_Delay(8);
    }
    printf("\nbye\n");

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
