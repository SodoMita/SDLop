/*
  SDLop smoke test: relative mouse mode against a real compositor.
  Arms and disarms pointer lock; both outcomes are valid depending on
  whether the compositor exposes a seat/pointer (sway headless: yes;
  weston headless: no). Fails only on crashes, protocol errors or
  inconsistent state.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    if (!getenv("WAYLAND_DISPLAY")) {
        printf("smoke_relative_mode: SKIP (WAYLAND_DISPLAY not set)\n");
        return 0;
    }
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("relative mode smoke", 320, 240, 0);
    if (!w) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    /* map the window so the compositor knows the surface */
    SDL_Surface *s = SDL_GetWindowSurface(w);
    if (s) {
        SDL_UpdateWindowSurface(w);
    }
    for (int i = 0; i < 20; i++) {
        SDL_Delay(10);
        SDL_PumpEvents();
    }

    int rc = 0;
    if (SDL_SetWindowRelativeMouseMode(w, true)) {
        printf("relative mode armed OK (%s)\n", SDL_GetCurrentVideoDriver());
        if (!SDL_GetWindowRelativeMouseMode(w)) {
            fprintf(stderr, "FAIL: flag not set after successful arm\n");
            rc = 1;
        }
        for (int i = 0; i < 10; i++) {
            SDL_Delay(10);
            SDL_PumpEvents(); /* let locked/relative events flow */
        }
        float rx = 0, ry = 0;
        SDL_GetRelativeMouseState(&rx, &ry);
        if (!SDL_SetWindowRelativeMouseMode(w, false)) {
            fprintf(stderr, "FAIL: disarm failed: %s\n", SDL_GetError());
            rc = 1;
        }
        if (SDL_GetWindowRelativeMouseMode(w)) {
            fprintf(stderr, "FAIL: flag still set after disarm\n");
            rc = 1;
        }
    } else {
        printf("relative mode refused (expected on seat-less compositors): %s\n",
               SDL_GetError());
        if (SDL_GetWindowRelativeMouseMode(w)) {
            fprintf(stderr, "FAIL: flag set despite refused arm\n");
            rc = 1;
        }
    }

    SDL_DestroyWindow(w);
    SDL_Quit();
    printf("smoke_relative_mode: %s\n", rc ? "FAIL" : "PASS");
    return rc;
}
