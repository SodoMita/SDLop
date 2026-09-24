/*
  SDLop test: event queue, window lifecycle, keyboard/mouse state -
  against the dummy video driver (headless, no compositor needed).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    assert(SDL_Init(SDL_INIT_VIDEO));
    assert(SDL_WasInit(SDL_INIT_VIDEO));

    const char *driver = SDL_GetCurrentVideoDriver();
    assert(driver && strcmp(driver, "dummy") == 0);

    /* --- window lifecycle --- */
    SDL_Window *w = SDL_CreateWindow("test", 320, 200, SDL_WINDOW_RESIZABLE);
    assert(w);
    assert(!(SDL_GetWindowFlags(w) & SDL_WINDOW_HIDDEN));
    assert(SDL_GetWindowFromID(SDL_GetWindowID(w)) == w);
    assert(strcmp(SDL_GetWindowTitle(w), "test") == 0);

    int ww = 0, hh = 0;
    assert(SDL_GetWindowSize(w, &ww, &hh));
    assert(ww == 320 && hh == 200);

    assert(SDL_SetWindowTitle(w, "renamed"));
    assert(strcmp(SDL_GetWindowTitle(w), "renamed") == 0);

    assert(SDL_SetWindowSize(w, 640, 480));
    {
        SDL_Event ev;
        bool saw_resized = false;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_WINDOW_RESIZED && ev.window.data1 == 640 && ev.window.data2 == 480) {
                saw_resized = true;
            }
        }
        assert(saw_resized);
    }

    /* focus: dummy driver focuses on show */
    assert(SDL_GetKeyboardFocus() == w);
    assert(SDL_GetMouseFocus() == w);

    /* --- push/poll user events --- */
    Uint32 usertype = SDL_RegisterEvents(2);
    assert(usertype == SDL_EVENT_USER);

    SDL_Event push;
    SDL_zero(push);
    push.type = usertype;
    push.user.code = 42;
    push.user.data1 = (void *)(uintptr_t)0xBEEF;
    assert(SDL_PushEvent(&push));

    assert(SDL_HasEvent(usertype));
    SDL_Event got;
    assert(SDL_PollEvent(&got));
    assert(got.type == usertype && got.user.code == 42 && got.user.data1 == (void *)(uintptr_t)0xBEEF);
    assert(!SDL_PollEvent(&got));
    assert(!SDL_HasEvent(usertype));

    /* --- text events keep their string through the queue --- */
    SDL_zero(push);
    push.type = SDL_EVENT_TEXT_INPUT;
    push.text.windowID = SDL_GetWindowID(w);
    push.text.text = "héllo";
    assert(SDL_PushEvent(&push));
    assert(SDL_PollEvent(&got));
    assert(got.type == SDL_EVENT_TEXT_INPUT);
    assert(strcmp(got.text.text, "héllo") == 0);

    /* --- flush --- */
    for (int i = 0; i < 5; i++) {
        SDL_zero(push);
        push.type = SDL_EVENT_MOUSE_MOTION;
        assert(SDL_PushEvent(&push));
    }
    SDL_FlushEvent(SDL_EVENT_MOUSE_MOTION);
    assert(!SDL_HasEvent(SDL_EVENT_MOUSE_MOTION));

    /* --- keyboard state via injected key --- */
    extern void SDLOP_SendKeyboardKey(bool down, bool repeat, SDL_Scancode scancode, Uint16 rawcode, Uint64 timestamp_ns);
    extern void SDLOP_SendWindowEvent(SDL_Window *window, SDL_EventType type, Sint32 data1, Sint32 data2);
    SDLOP_SendKeyboardKey(true, false, SDL_SCANCODE_W, 17, 0);
    {
        SDL_Event ev;
        bool saw_down = false;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                saw_down = true;
                assert(ev.key.scancode == SDL_SCANCODE_W);
                assert(ev.key.down);
                assert(ev.key.raw == 17);
                assert(ev.key.windowID == SDL_GetWindowID(w));
            }
        }
        assert(saw_down);
    }
    assert(SDL_GetKeyState(SDL_SCANCODE_W));
    {
        int n = 0;
        const bool *ks = SDL_GetKeyboardState(&n);
        assert(n == SDL_SCANCODE_COUNT);
        assert(ks[SDL_SCANCODE_W]);
    }
    SDLOP_SendKeyboardKey(false, false, SDL_SCANCODE_W, 17, 0);
    SDL_FlushEvent(SDL_EVENT_KEY_UP);
    assert(!SDL_GetKeyState(SDL_SCANCODE_W));

    /* modifier state tracking */
    SDLOP_SendKeyboardKey(true, false, SDL_SCANCODE_LSHIFT, 42, 0);
    assert(SDL_GetModState() & SDL_KMOD_LSHIFT);
    SDLOP_SendKeyboardKey(false, false, SDL_SCANCODE_LSHIFT, 42, 0);
    assert(!(SDL_GetModState() & SDL_KMOD_LSHIFT));
    SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP);

    /* --- mouse state --- */
    extern void SDLOP_SendMouseMotion(float x, float y, float xrel, float yrel, Uint64 ts);
    extern void SDLOP_SendMouseButton(bool down, Uint8 button, float x, float y, Uint64 ts);
    SDLOP_SendMouseMotion(100.0f, 50.0f, 10.0f, 5.0f, 0);
    {
        SDL_Event ev;
        bool saw_motion = false;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                saw_motion = true;
                assert(ev.motion.x == 100.0f && ev.motion.y == 50.0f);
                assert(ev.motion.xrel == 10.0f && ev.motion.yrel == 5.0f);
            }
        }
        assert(saw_motion);
    }
    float mx = -1, my = -1;
    SDL_MouseButtonFlags b = SDL_GetMouseState(&mx, &my);
    assert(mx == 100.0f && my == 50.0f);
    assert(b == 0); /* no buttons pressed yet */
    float rx = 0, ry = 0;
    b = SDL_GetRelativeMouseState(&rx, &ry);
    assert(b == 0);
    assert(rx == 10.0f && ry == 5.0f);
    /* accumulators are cleared by the query */
    SDL_GetRelativeMouseState(&rx, &ry);
    assert(rx == 0.0f && ry == 0.0f);

    SDLOP_SendMouseButton(true, SDL_BUTTON_LEFT, 0, 0, 0);
    {
        SDL_Event ev;
        bool saw_button = false;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                saw_button = true;
                assert(ev.button.button == SDL_BUTTON_LEFT && ev.button.down);
            }
        }
        assert(saw_button);
    }
    assert(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK);
    SDLOP_SendMouseButton(false, SDL_BUTTON_LEFT, 0, 0, 0);
    assert(!(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK)); /* released */
    SDL_FlushEvent(SDL_EVENT_MOUSE_BUTTON_UP);

    /* --- window close request + quit event --- */
    SDLOP_SendWindowEvent(w, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
    assert(SDL_HasEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED));
    SDL_FlushEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED);

    SDL_Event quit;
    SDL_zero(quit);
    quit.type = SDL_EVENT_QUIT;
    assert(SDL_PushEvent(&quit));
    assert(SDL_QuitRequested());
    SDL_FlushEvent(SDL_EVENT_QUIT);

    /* --- WaitEventTimeout with nothing pending times out --- */
    Uint64 t0 = SDL_GetTicks();
    assert(!SDL_WaitEventTimeout(&got, 50));
    Uint64 dt = SDL_GetTicks() - t0;
    assert(dt >= 40 && dt < 500);

    /* --- GetWindowFromEvent --- */
    SDL_zero(push);
    push.type = SDL_EVENT_KEY_DOWN;
    push.key.windowID = SDL_GetWindowID(w);
    assert(SDL_PushEvent(&push));
    assert(SDL_PollEvent(&got));
    assert(SDL_GetWindowFromEvent(&got) == w);

    /* hide/show flags */
    assert(SDL_HideWindow(w));
    assert(SDL_GetWindowFlags(w) & SDL_WINDOW_HIDDEN);
    assert(SDL_ShowWindow(w));
    assert(!(SDL_GetWindowFlags(w) & SDL_WINDOW_HIDDEN));

    /* --- destroy --- */
    SDL_WindowID saved_id = SDL_GetWindowID(w);
    SDL_DestroyWindow(w);
    assert(SDL_GetWindowFromID(saved_id) == NULL);

    SDL_Quit();
    printf("test_events: PASS\n");
    return 0;
}
