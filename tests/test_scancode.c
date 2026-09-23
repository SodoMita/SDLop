/*
  SDLop test: evdev -> SDL_Scancode translation table correctness.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/scancode_evdev.h"
#include <SDL3/SDL_scancode.h>
#include <linux/input-event-codes.h>
#include <assert.h>
#include <stdio.h>

static void check(uint16_t evdev, SDL_Scancode sc, const char *what)
{
    assert(evdev <= SDLOP_EVDEV_KEY_MAX);
    if (sdlop_evdev_to_scancode[evdev] != (uint16_t)sc) {
        fprintf(stderr, "FAIL: %s: evdev %u -> %u, expected %u\n",
                what, (unsigned)evdev, (unsigned)sdlop_evdev_to_scancode[evdev], (unsigned)sc);
        assert(0);
    }
}

int main(void)
{
    check(KEY_ESC, SDL_SCANCODE_ESCAPE, "ESC");
    check(KEY_1, SDL_SCANCODE_1, "1");
    check(KEY_Q, SDL_SCANCODE_Q, "Q");
    check(KEY_W, SDL_SCANCODE_W, "W");
    check(KEY_A, SDL_SCANCODE_A, "A");
    check(KEY_S, SDL_SCANCODE_S, "S");
    check(KEY_D, SDL_SCANCODE_D, "D");
    check(KEY_Z, SDL_SCANCODE_Z, "Z");
    check(KEY_ENTER, SDL_SCANCODE_RETURN, "ENTER");
    check(KEY_LEFTCTRL, SDL_SCANCODE_LCTRL, "LCTRL");
    check(KEY_RIGHTCTRL, SDL_SCANCODE_RCTRL, "RCTRL");
    check(KEY_LEFTSHIFT, SDL_SCANCODE_LSHIFT, "LSHIFT");
    check(KEY_RIGHTSHIFT, SDL_SCANCODE_RSHIFT, "RSHIFT");
    check(KEY_LEFTALT, SDL_SCANCODE_LALT, "LALT");
    check(KEY_RIGHTALT, SDL_SCANCODE_RALT, "RALT");
    check(KEY_LEFTMETA, SDL_SCANCODE_LGUI, "LGUI");
    check(KEY_SPACE, SDL_SCANCODE_SPACE, "SPACE");
    check(KEY_TAB, SDL_SCANCODE_TAB, "TAB");
    check(KEY_CAPSLOCK, SDL_SCANCODE_CAPSLOCK, "CAPSLOCK");
    check(KEY_F1, SDL_SCANCODE_F1, "F1");
    check(KEY_F12, SDL_SCANCODE_F12, "F12");
    check(KEY_UP, SDL_SCANCODE_UP, "UP");
    check(KEY_DOWN, SDL_SCANCODE_DOWN, "DOWN");
    check(KEY_LEFT, SDL_SCANCODE_LEFT, "LEFT");
    check(KEY_RIGHT, SDL_SCANCODE_RIGHT, "RIGHT");
    check(KEY_KPENTER, SDL_SCANCODE_KP_ENTER, "KPENTER");
    check(KEY_KPSLASH, SDL_SCANCODE_KP_DIVIDE, "KPSLASH");
    check(KEY_KP7, SDL_SCANCODE_KP_7, "KP7");
    check(KEY_102ND, SDL_SCANCODE_NONUSHASH, "102ND");
    check(KEY_COMPOSE, SDL_SCANCODE_APPLICATION, "COMPOSE");
    check(KEY_VOLUMEDOWN, SDL_SCANCODE_VOLUMEDOWN, "VOLUMEDOWN");
    check(KEY_MUTE, SDL_SCANCODE_MUTE, "MUTE");

    /* letters must follow USB HID usage (SDL semantics), not evdev order */
    assert(sdlop_evdev_to_scancode[KEY_A] == 4);
    assert(sdlop_evdev_to_scancode[KEY_B] == 5);
    assert(sdlop_evdev_to_scancode[KEY_Z] == 29);

    printf("test_scancode: PASS (%d evdev codes mapped)\n",
           SDLOP_EVDEV_KEY_MAX + 1);
    return 0;
}
