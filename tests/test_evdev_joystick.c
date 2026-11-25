/*
  SDLop test: end-to-end Linux evdev joystick/gamepad path.

  Creates synthetic joystick and gamepad devices with /dev/uinput, injects
  evdev events, and verifies they arrive through SDL's joystick and gamepad
  APIs: enumeration, state queries, events, and hotplug removal.

  Skips cleanly when /dev/uinput or /dev/input/event* are not accessible.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

#include "uinput_synth.h"

#define JOY_NAME "SDLop Test Joystick"
#define PAD_NAME "SDLop Test Gamepad"

static int failures;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, " [SDL: %s]\n", SDL_GetError());    \
            failures++;                                          \
        }                                                        \
    } while (0)

static void drain_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
    }
}

static bool wait_for_event(Uint32 type, SDL_JoystickID which, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.common.type == type && ev.jdevice.which == which) {
                return true;
            }
        }
        SDL_Delay(5);
    }
    return false;
}

static SDL_JoystickID find_id_by_name(const char *name, bool gamepad)
{
    int count = 0;
    SDL_JoystickID *ids = gamepad ? SDL_GetGamepads(&count) : SDL_GetJoysticks(&count);
    SDL_JoystickID result = 0;
    for (int i = 0; ids && i < count; ++i) {
        const char *n = gamepad ? SDL_GetGamepadNameForID(ids[i]) : SDL_GetJoystickNameForID(ids[i]);
        if (n && strcmp(n, name) == 0) {
            result = ids[i];
            break;
        }
    }
    SDL_free(ids);
    return result;
}

static bool wait_for_device(const char *name, bool gamepad, SDL_JoystickID *id, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        *id = find_id_by_name(name, gamepad);
        if (*id) {
            return true;
        }
        SDL_PumpEvents();
        SDL_Delay(5);
    }
    return false;
}

static bool device_gone(const char *name, bool gamepad)
{
    return find_id_by_name(name, gamepad) == 0;
}

static bool wait_for_removals(SDL_JoystickID joy_id, SDL_JoystickID pad_id, int timeout_ms)
{
    bool joy_event = false, pad_event = false;
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        SDL_PumpEvents();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.common.type == SDL_EVENT_JOYSTICK_REMOVED ||
                ev.common.type == SDL_EVENT_GAMEPAD_REMOVED) {
                if (ev.jdevice.which == joy_id) joy_event = true;
                if (ev.jdevice.which == pad_id) pad_event = true;
            }
        }
        if (device_gone(JOY_NAME, false) && device_gone(PAD_NAME, true)) {
            return joy_event && pad_event;
        }
        SDL_Delay(5);
    }
    return false;
}

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    static const uint16_t joy_axes[] = { ABS_X, ABS_Y };
    static const uint16_t joy_buttons[] = { BTN_TRIGGER, BTN_THUMB };
    static const uint16_t pad_axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z };
    static const uint16_t pad_buttons[] = {
        BTN_GAMEPAD, BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,
        BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL
    };

    UInputDevice *joy_dev = uinput_synth_create_joystick(JOY_NAME, joy_axes, 2, joy_buttons, 2);
    if (!joy_dev) {
        printf("SKIP: /dev/uinput unavailable\n");
        return 0;
    }
    UInputDevice *pad_dev = uinput_synth_create_joystick(PAD_NAME, pad_axes, 5, pad_buttons, 11);
    if (!pad_dev) {
        uinput_synth_destroy(joy_dev);
        printf("SKIP: /dev/uinput gamepad creation failed\n");
        return 0;
    }

    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        CHECK(false, "SDL_Init failed");
        goto cleanup;
    }
    drain_events();

    SDL_JoystickID joy_id = 0, pad_id = 0;
    if (!wait_for_device(JOY_NAME, false, &joy_id, 2000) ||
        !wait_for_device(PAD_NAME, true, &pad_id, 2000)) {
        printf("SKIP: /dev/input/event* not accessible to this process\n");
        goto cleanup;
    }

    CHECK(SDL_IsGamepad(joy_id) == false, "plain joystick is not a gamepad");
    CHECK(SDL_IsGamepad(pad_id) == true, "synthetic gamepad is a gamepad");

    SDL_Joystick *joy = SDL_OpenJoystick(joy_id);
    CHECK(joy != NULL, "SDL_OpenJoystick");
    if (joy) {
        CHECK(strcmp(SDL_GetJoystickName(joy), JOY_NAME) == 0, "joystick name '%s'", SDL_GetJoystickName(joy));
        CHECK(SDL_GetJoystickPath(joy) != NULL, "joystick path");
        CHECK(SDL_GetNumJoystickAxes(joy) == 2, "joystick axes %d", SDL_GetNumJoystickAxes(joy));
        CHECK(SDL_GetNumJoystickButtons(joy) >= 2, "joystick buttons %d", SDL_GetNumJoystickButtons(joy));
        CHECK(SDL_GetJoystickVendor(joy) == 0x1234 && SDL_GetJoystickProduct(joy) == 0x5678,
              "joystick IDs %04x/%04x", SDL_GetJoystickVendor(joy), SDL_GetJoystickProduct(joy));
        CHECK(SDL_JoystickConnected(joy) && SDL_GetJoystickConnectionState(joy) == SDL_JOYSTICK_CONNECTION_WIRED,
              "joystick connected");

        drain_events();
        CHECK(uinput_synth_abs(joy_dev, ABS_X, 12345), "inject ABS_X");
        CHECK(uinput_synth_key(joy_dev, BTN_THUMB, 1), "inject BTN_THUMB");
        CHECK(uinput_synth_syn(joy_dev), "inject SYN");
        bool got_axis = wait_for_event(SDL_EVENT_JOYSTICK_AXIS_MOTION, joy_id, 1000);
        bool got_button = wait_for_event(SDL_EVENT_JOYSTICK_BUTTON_DOWN, joy_id, 1000);
        CHECK(got_axis, "joystick axis event");
        CHECK(got_button, "joystick button event");
        CHECK(SDL_GetJoystickAxis(joy, 0) == 12345, "joystick axis state %d", SDL_GetJoystickAxis(joy, 0));
        CHECK(SDL_GetJoystickButton(joy, 1) == true, "joystick button state");
    }

    SDL_Gamepad *pad = SDL_OpenGamepad(pad_id);
    CHECK(pad != NULL, "SDL_OpenGamepad");
    if (pad) {
        CHECK(strcmp(SDL_GetGamepadName(pad), PAD_NAME) == 0, "gamepad name '%s'", SDL_GetGamepadName(pad));
        CHECK(SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_STANDARD, "gamepad type %d", SDL_GetGamepadType(pad));
        CHECK(SDL_GamepadHasAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), "gamepad has leftx");
        CHECK(SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_SOUTH), "gamepad has south");
        CHECK(SDL_GetGamepadJoystick(pad) != NULL, "gamepad joystick handle");

        drain_events();
        CHECK(uinput_synth_abs(pad_dev, ABS_X, -10000), "inject gamepad ABS_X");
        CHECK(uinput_synth_key(pad_dev, BTN_SOUTH, 1), "inject gamepad BTN_SOUTH");
        CHECK(uinput_synth_syn(pad_dev), "inject gamepad SYN");
        bool got_axis = wait_for_event(SDL_EVENT_GAMEPAD_AXIS_MOTION, pad_id, 1000);
        bool got_button = wait_for_event(SDL_EVENT_GAMEPAD_BUTTON_DOWN, pad_id, 1000);
        CHECK(got_axis, "gamepad axis event");
        CHECK(got_button, "gamepad button event");
        CHECK(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) == -10000,
              "gamepad leftx %d", SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
        CHECK(SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH), "gamepad south state");

        char *mapping = SDL_GetGamepadMapping(pad);
        CHECK(mapping && strstr(mapping, "leftx:a0") != NULL, "generic gamepad mapping");
        SDL_free(mapping);
    }

    uinput_synth_destroy(joy_dev);
    uinput_synth_destroy(pad_dev);
    joy_dev = pad_dev = NULL;
    CHECK(wait_for_removals(joy_id, pad_id, 2000), "hotplug removal events");
    if (joy) {
        CHECK(!SDL_JoystickConnected(joy), "opened joystick reports disconnected");
        CHECK(SDL_GetJoystickConnectionState(joy) == SDL_JOYSTICK_CONNECTION_UNKNOWN,
              "opened joystick connection state");
    }
    if (pad) {
        CHECK(!SDL_GamepadConnected(pad), "opened gamepad reports disconnected");
    }

    SDL_CloseGamepad(pad);
    SDL_CloseJoystick(joy);
    drain_events();

cleanup:
    if (joy_dev) uinput_synth_destroy(joy_dev);
    if (pad_dev) uinput_synth_destroy(pad_dev);
    SDL_Quit();
    if (failures) {
        fprintf(stderr, "test_evdev_joystick: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_evdev_joystick: OK\n");
    return 0;
}
