/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Time and timers (API compatible with <SDL3/SDL_timer.h>).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_timer_h_
#define SDL_timer_h_

#include <SDL3/SDL_stdinc.h>

/**
 * Get the number of milliseconds since SDL library initialization.
 */
extern Uint64 SDL_GetTicks(void);

/**
 * Get the number of nanoseconds since SDL library initialization.
 */
extern Uint64 SDL_GetTicksNS(void);

/**
 * Get the current value of the high resolution counter (CLOCK_MONOTONIC ns).
 */
extern Uint64 SDL_GetPerformanceCounter(void);

/**
 * Get the count per second of the high resolution counter (1e9).
 */
extern Uint64 SDL_GetPerformanceFrequency(void);

/**
 * Wait a specified number of milliseconds before returning.
 */
extern void SDL_Delay(Uint32 ms);

/**
 * Wait a specified number of nanoseconds before returning.
 */
extern void SDL_DelayNS(Uint64 ns);

#endif /* SDL_timer_h_ */
