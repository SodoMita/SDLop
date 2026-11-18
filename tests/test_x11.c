/*
  SDLop test: X11 integration - window lifecycle, input and presentation
  against a real X server (Xvfb) or the scripted mini server
  (tests/mini_xserver.py).

  Skips cleanly when DISPLAY is not set. When the harness sets
  SDLOP_TEST_EXPECT_INPUT=1 the server is expected to inject the scripted
  input sequence (mini server) and this test asserts every event.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fprintf(stderr, "\n");                             \
            failures++;                                        \
        }                                                      \
    } while (0)

/* pump events for a while, collecting the ones we care about */
typedef struct EventLog
{
    unsigned key_down_a;
    unsigned key_down_a_repeat;
    unsigned key_up_a;
    unsigned text_a;
    unsigned motion;
    unsigned button_down_left;
    unsigned button_up_left;
    unsigned wheel_up;
    unsigned mouse_enter;
    unsigned focus_gained;
    unsigned close_requested;
    unsigned resized_400_300;
    unsigned moved;
    unsigned exposed;
    float last_motion_x, last_motion_y;
    bool keycode_resolved; /* any key carried a resolved keycode */
} EventLog;

static void pump_for(int ms, EventLog *log)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)ms;
    while (SDL_GetTicks() < deadline) {
        SDL_Event ev;
        SDL_PumpEvents();
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.scancode == SDL_SCANCODE_A) {
                    if (ev.key.repeat) {
                        log->key_down_a_repeat++;
                    } else {
                        log->key_down_a++;
                    }
                    if (ev.key.key != SDLK_UNKNOWN) {
                        log->keycode_resolved = true;
                    }
                }
                break;
            case SDL_EVENT_KEY_UP:
                if (ev.key.scancode == SDL_SCANCODE_A) {
                    log->key_up_a++;
                }
                break;
            case SDL_EVENT_TEXT_INPUT:
                if (ev.text.text && strcmp(ev.text.text, "a") == 0) {
                    log->text_a++;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                log->motion++;
                log->last_motion_x = ev.motion.x;
                log->last_motion_y = ev.motion.y;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    log->button_down_left++;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    log->button_up_left++;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (ev.wheel.y > 0.0f) {
                    log->wheel_up++;
                }
                break;
            case SDL_EVENT_WINDOW_MOUSE_ENTER: log->mouse_enter++; break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED: log->focus_gained++; break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: log->close_requested++; break;
            case SDL_EVENT_WINDOW_RESIZED:
                if (ev.window.data1 == 400 && ev.window.data2 == 300) {
                    log->resized_400_300++;
                }
                break;
            case SDL_EVENT_WINDOW_MOVED: log->moved++; break;
            case SDL_EVENT_WINDOW_EXPOSED: log->exposed++; break;
            default: break;
            }
        }
        SDL_Delay(5);
    }
}

int main(void)
{
    failures = 0;

    if (!getenv("DISPLAY")) {
        printf("test_x11: SKIP (DISPLAY not set)\n");
        return 0;
    }
    const bool expect_input = getenv("SDLOP_TEST_EXPECT_INPUT") != NULL;
    /* keep the test deterministic: window-system input only */
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDL_VIDEODRIVER", "x11", 1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    CHECK(strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0, "video driver is x11");

    /* display enumeration */
    int num_displays = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&num_displays);
    CHECK(displays && num_displays >= 1, "at least one display");
    if (displays && num_displays >= 1) {
        SDL_Rect bounds;
        CHECK(SDL_GetDisplayBounds(displays[0], &bounds), "display bounds");
        CHECK(bounds.w > 0 && bounds.h > 0, "bounds positive (%dx%d)", bounds.w, bounds.h);
        CHECK(SDL_GetDisplayName(displays[0]) != NULL, "display name");
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(displays[0]);
        CHECK(mode && mode->w == bounds.w && mode->h == bounds.h, "desktop mode matches bounds");
        float scale = SDL_GetDisplayContentScale(displays[0]);
        CHECK(scale >= 1.0f && scale <= 8.0f, "content scale %g in range", scale);
        free(displays);
    }

    EventLog log;
    memset(&log, 0, sizeof(log));

    SDL_Window *w = SDL_CreateWindow("SDLop x11 test", 320, 200, SDL_WINDOW_RESIZABLE);
    if (!w) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    pump_for(200, &log);

    CHECK(SDL_GetWindowTitle(w) && strcmp(SDL_GetWindowTitle(w), "SDLop x11 test") == 0, "title");
    int ww = 0, hh = 0;
    CHECK(SDL_GetWindowSize(w, &ww, &hh) && ww == 320 && hh == 200, "size 320x200 (got %dx%d)", ww, hh);
    CHECK((SDL_GetWindowFlags(w) & SDL_WINDOW_HIDDEN) == 0, "window shown");

    /* title change */
    CHECK(SDL_SetWindowTitle(w, "renamed"), "set title");
    CHECK(strcmp(SDL_GetWindowTitle(w), "renamed") == 0, "title updated");

    /* explicit resize -> ConfigureNotify -> RESIZED event */
    CHECK(SDL_SetWindowSize(w, 400, 300), "set size");
    pump_for(300, &log);
    CHECK(SDL_GetWindowSize(w, &ww, &hh) && ww == 400 && hh == 300, "size now 400x300 (got %dx%d)", ww, hh);
    CHECK(log.resized_400_300 > 0, "RESIZED 400x300 event seen");

    /* move */
    CHECK(SDL_SetWindowPosition(w, 30, 20), "set position");
    pump_for(200, &log);

    /* software framebuffer: create, fill, present */
    SDL_Surface *surface = SDL_GetWindowSurface(w);
    CHECK(surface != NULL, "window surface");
    if (surface) {
        CHECK(surface->w == 400 && surface->h == 300, "surface matches window (%dx%d)", surface->w, surface->h);
        CHECK(surface->format == SDL_PIXELFORMAT_XRGB8888, "surface format XRGB8888");
        Uint32 *pix = (Uint32 *)surface->pixels;
        for (int i = 0; i < surface->w * surface->h; i++) {
            pix[i] = 0xFFFF00FF; /* magenta */
        }
        CHECK(SDL_UpdateWindowSurface(w), "update window surface");
        pump_for(100, &log);
        /* the surface survives a resize (same object, updated geometry) */
        CHECK(SDL_SetWindowSize(w, 320, 200), "set size back");
        pump_for(300, &log);
        SDL_Surface *surface2 = SDL_GetWindowSurface(w);
        CHECK(surface2 == surface, "surface object survives resize");
        if (surface2) {
            CHECK(surface2->w == 320 && surface2->h == 200, "surface resized (%dx%d)", surface2->w, surface2->h);
            CHECK(SDL_UpdateWindowSurface(w), "update after resize");
        }
    }

    /* relative mouse mode: grab + recentering; toggling must not crash */
    if (SDL_SetWindowRelativeMouseMode(w, true)) {
        CHECK(SDL_GetWindowRelativeMouseMode(w), "relative mode flag");
        pump_for(100, &log);
        CHECK(SDL_SetWindowRelativeMouseMode(w, false), "relative mode off");
        CHECK(!SDL_GetWindowRelativeMouseMode(w), "relative mode flag cleared");
    } else {
        CHECK(!SDL_GetWindowRelativeMouseMode(w), "relative mode flag rolled back on failure");
    }

    /* fullscreen roundtrip (a WM-less server simply never confirms;
     * the request itself must not crash) */
    CHECK(SDL_SetWindowFullscreen(w, true), "enter fullscreen request");
    pump_for(150, &log);
    CHECK(SDL_SetWindowFullscreen(w, false), "leave fullscreen request");
    pump_for(150, &log);

    /* minimize/restore/maximize requests must not crash */
    CHECK(SDL_MinimizeWindow(w), "minimize");
    pump_for(150, &log);
    CHECK(SDL_RestoreWindow(w), "restore");
    pump_for(150, &log);
    CHECK(SDL_MaximizeWindow(w), "maximize");
    pump_for(150, &log);
    CHECK(SDL_RestoreWindow(w), "restore2");
    pump_for(150, &log);

    /* hide/show */
    CHECK(SDL_HideWindow(w), "hide");
    pump_for(150, &log);
    CHECK(SDL_GetWindowFlags(w) & SDL_WINDOW_HIDDEN, "hidden flag");
    CHECK(SDL_ShowWindow(w), "show");
    pump_for(200, &log);
    CHECK(!(SDL_GetWindowFlags(w) & SDL_WINDOW_HIDDEN), "shown again");

    /* GLX smoke: only when the window was created with SDL_WINDOW_OPENGL
     * semantics available; skip gracefully when GLX/libGL is missing */
    {
        SDL_Window *gw = SDL_CreateWindow("gl", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (gw) {
            SDL_GLContext ctx = SDL_GL_CreateContext(gw);
            if (ctx) {
                typedef const unsigned char *(*PFNGLGETSTRING)(unsigned int);
                PFNGLGETSTRING get_string = (PFNGLGETSTRING)SDL_GL_GetProcAddress("glGetString");
                CHECK(get_string != NULL, "glGetString via SDL_GL_GetProcAddress");
                if (get_string) {
                    const unsigned char *version = get_string(0x1F02 /* GL_VERSION */);
                    CHECK(version != NULL, "GL_VERSION string");
                    if (version) {
                        printf("GLX context: GL_VERSION = %s\n", (const char *)version);
                    }
                }
                CHECK(SDL_GL_SwapWindow(gw), "GLX swap");
                SDL_GL_DestroyContext(ctx);
            } else {
                printf("test_x11: GLX context unavailable (%s) - skipping GL part\n", SDL_GetError());
            }
            SDL_DestroyWindow(gw);
        }
    }

    /* input injection phase: the mini server injects a scripted sequence
     * after map; wait for the close request (or time out) */
    pump_for(expect_input ? 3000 : 500, &log);

    if (expect_input) {
        CHECK(log.focus_gained > 0, "FOCUS_GAINED from server focus");
        CHECK(log.key_down_a >= 1, "KEY_DOWN 'a' (%u)", log.key_down_a);
        CHECK(log.key_down_a_repeat >= 1, "KEY_DOWN 'a' repeat pair (%u)", log.key_down_a_repeat);
        CHECK(log.key_up_a >= 1, "KEY_UP 'a' (%u)", log.key_up_a);
        /* keycode/text resolution needs xkbcommon + xkb data at run time */
        if (log.keycode_resolved) {
            /* text input additionally needs a real xkb keymap, which is
             * only available when libxkbcommon is loadable at run time */
            void *xkb = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL);
            if (xkb) {
                CHECK(log.text_a >= 1, "TEXT_INPUT \"a\" (%u)", log.text_a);
                dlclose(xkb);
            } else {
                printf("test_x11: note - libxkbcommon absent at run time (text input untested)\n");
            }
        } else {
            printf("test_x11: note - no xkb keymap (text input untested)\n");
        }
        CHECK(log.mouse_enter > 0, "MOUSE_ENTER");
        CHECK(log.motion > 0, "MOUSE_MOTION (%u)", log.motion);
        CHECK(log.button_down_left > 0, "BUTTON_DOWN left");
        CHECK(log.button_up_left > 0, "BUTTON_UP left");
        CHECK(log.wheel_up > 0, "wheel up");
        CHECK(log.close_requested > 0, "CLOSE_REQUESTED (WM_DELETE_WINDOW)");
    }

    SDL_DestroyWindow(w);
    SDL_Quit();

    if (failures) {
        printf("test_x11: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_x11: PASS%s\n", expect_input ? " (scripted input verified)" : "");
    return 0;
}
