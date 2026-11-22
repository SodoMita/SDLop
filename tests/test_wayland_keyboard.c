/* Actual protocol regression against mini_compositor: held modifiers,
 * ignored held action keys, repeat disable and immediate repeat settings.
 * zlib license, see LICENSE. */
#include <SDL3/SDL.h>
#include "internal/sdlop_internal.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    assert(argc >= 2);
    bool immediate = argc == 3;
    char dir[] = "/tmp/sdlop-keyboard-XXXXXX";
    assert(mkdtemp(dir));
    char socket[256];
    snprintf(socket, sizeof(socket), "%s/wayland-mini", dir);
    pid_t server = fork();
    assert(server >= 0);
    if (!server) {
        setenv("XDG_RUNTIME_DIR", dir, 1);
        setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
        setenv("SDLOP_TEST_KEYBOARD", "1", 1);
        if (immediate) { setenv("SDLOP_TEST_KEYBOARD_IMMEDIATE", "1", 1); }
        else { unsetenv("SDLOP_TEST_KEYBOARD_IMMEDIATE"); }
        execl(argv[1], "mini_compositor", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 300 && access(socket, F_OK); i++) { SDL_Delay(10); }
    assert(access(socket, F_OK) == 0);
    setenv("XDG_RUNTIME_DIR", dir, 1);
    setenv("WAYLAND_DISPLAY", "wayland-mini", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv(SDL_HINT_TOUCH_MOUSE_EVENTS, "0", 1);
    assert(SDL_Init(SDL_INIT_VIDEO));
    if (!SDLOP_KeyboardSetDefaultKeymap()) {
        puts("SKIP: Wayland keyboard test needs xkbcommon and US layout data");
        SDL_Quit(); kill(server, SIGTERM); waitpid(server, NULL, 0);
        unlink(socket); rmdir(dir); return 0;
    }
    SDL_Window *window = SDL_CreateWindow("keyboard", 320, 200, 0);
    assert(window && SDL_StartTextInput(window));
    assert(SDL_GetWindowSurface(window) && SDL_UpdateWindowSurface(window));
    Uint64 until = SDL_GetTicks() + 3000;
    while (!(window->flags & SDL_WINDOW_INPUT_FOCUS) && SDL_GetTicks() < until) {
        SDL_PumpEvents(); SDL_Delay(1);
    }
    assert(window->flags & SDL_WINDOW_INPUT_FOCUS);
    int repeats = 0, presses = 0, texts = 0;
    until = SDL_GetTicks() + (immediate ? 200 : 700);
    do {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_A) {
                if (event.key.repeat) { repeats++; } else { presses++; }
            }
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                assert(!strcmp(event.text.text, "A")); texts++;
            }
        }
        SDL_Delay(1);
    } while (SDL_GetTicks() < until);
    assert(SDL_GetKeyboardState(NULL)[SDL_SCANCODE_LSHIFT]);
    assert(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_B]); /* not a fresh action */
    assert(presses == 1 && texts == presses + repeats);
    assert(immediate ? (repeats > 0 && sdlop.repeat_delay_ms == 0 && sdlop.repeat_interval_ms == 50)
                     : (repeats == 0 && sdlop.repeat_disabled));
    SDL_Quit();
    kill(server, SIGTERM); waitpid(server, NULL, 0);
    unlink(socket); rmdir(dir);
    printf("PASS: Wayland held modifiers only, %s repeat, text counts (%d repeats)\n",
           immediate ? "zero-delay" : "disabled", repeats);
    return 0;
}
