/*
  SDLop - timers.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL_timer.h>
#include <time.h>

/* CLOCK_MONOTONIC origin captured at first use (~library load). */
static Uint64 timer_origin_ns(void)
{
    static Uint64 origin;
    static int inited;
    if (!inited) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        origin = (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
        inited = 1;
    }
    return origin;
}

Uint64 SDL_GetPerformanceCounter(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return 1000000000ull;
}

Uint64 SDL_GetTicksNS(void)
{
    return SDL_GetPerformanceCounter() - timer_origin_ns();
}

Uint64 SDL_GetTicks(void)
{
    return SDL_GetTicksNS() / 1000000ull;
}

void SDL_Delay(Uint32 ms)
{
    SDL_DelayNS((Uint64)ms * 1000000ull);
}

void SDL_DelayNS(Uint64 ns)
{
    struct timespec req, rem;
    req.tv_sec = (time_t)(ns / 1000000000ull);
    req.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&req, &rem) != 0) {
        req = rem;
    }
}
