/*
  SDLop test: Emscripten build validation. Runs in two environments:

  - node (no DOM): core init via the dummy driver + the static web
    keymap table (SDL_GetKeyFromScancode, names, key/text generation).
  - browser (headless chromium): the emscripten driver itself - window
    creation on #canvas, and synthetic DOM events (KeyboardEvent /
    MouseEvent / WheelEvent) flowing through the emscripten callbacks
    into the SDL event queue.

  Results go to stdout (node) and into <pre id="sdlop-test-out">
  (browser, for chromium --dump-dom).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#ifndef __EMSCRIPTEN__
#include "internal/sdlop_internal.h"
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

static bool has_dom(void)
{
    return EM_ASM_INT({ return typeof document !== "undefined" ? 1 : 0; }) != 0;
}

static void emit_to_dom(const char *text)
{
    MAIN_THREAD_EM_ASM(
        {
            let el = document.getElementById("sdlop-test-out");
            if (!el) {
                el = document.createElement("pre");
                el.id = "sdlop-test-out";
                document.body.appendChild(el);
            }
            el.textContent += UTF8ToString($0) + "\n";
        },
        text);
}

#define LOGF(...)                     \
    do {                              \
        char _b[512];                 \
        snprintf(_b, sizeof(_b), __VA_ARGS__); \
        printf("%s\n", _b);           \
        if (in_browser) {             \
            emit_to_dom(_b);          \
        }                             \
    } while (0)
#else
static bool has_dom(void) { return false; }
#define LOGF printf
#endif

static int failures = 0;
static bool in_browser = false;

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            LOGF("FAIL %s:%d: ", __FILE__, __LINE__); \
            LOGF(__VA_ARGS__);                    \
            failures++;                           \
        }                                         \
    } while (0)

/* ---------- node-safe parts (also run in the browser) ---------- */

static void test_keymap_table(void)
{
    /* base + shifted key resolution (SDL3 semantics) */
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_W, SDL_KMOD_NONE, true) == SDLK_w,
          "W base -> SDLK_w");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_W, SDL_KMOD_LSHIFT, true) == SDLK_W,
          "shift+W -> SDLK_W");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_W, SDL_KMOD_LSHIFT, false) == SDLK_w,
          "polled shift+W -> base");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_2, SDL_KMOD_LSHIFT, true) == SDLK_AT,
          "shift+2 -> @");
    CHECK(SDL_GetKeyFromScancode(SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, true) == SDLK_ESCAPE,
          "escape -> SDLK_ESCAPE");
    CHECK(SDL_GetScancodeFromKey(SDLK_W, NULL) == SDL_SCANCODE_W, "key -> scancode");
    SDL_Keymod m = SDL_KMOD_NONE;
    CHECK(SDL_GetScancodeFromKey(SDLK_AT, &m) == SDL_SCANCODE_2 && m == SDL_KMOD_LSHIFT,
          "@ -> 2 + shift");

    /* names */
    CHECK(strcmp(SDL_GetScancodeName(SDL_SCANCODE_RETURN), "Return") == 0,
          "name of RETURN is '%s'", SDL_GetScancodeName(SDL_SCANCODE_RETURN));
    /* stock SDL3 names a letter key by its printed (capital) letter */
  CHECK(strcmp(SDL_GetKeyName(SDLK_w), "W") == 0, "name of SDLK_w");
    CHECK(strcmp(SDL_GetKeyName(SDLK_ESCAPE), "Escape") == 0, "name of ESCAPE");
}

__attribute__((unused)) /* kept for interactive debugging of the web shell */
static bool poll_key(SDL_Scancode sc, bool want_down, const char **text_out)
{
    bool found = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) { /* drain fully: TEXT_INPUT follows the key */
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
            if (e.key.scancode == sc && e.key.down == want_down) {
                found = true;
            }
        } else if (e.type == SDL_EVENT_TEXT_INPUT && text_out) {
            *text_out = e.text.text;
        }
    }
    return found;
}

/* ---------- browser-only parts ---------- */

#ifdef __EMSCRIPTEN__
static void dispatch_js(const char *js)
{
    MAIN_THREAD_EM_ASM({ eval(UTF8ToString($0)); }, js);
}

static void test_browser_driver(void)
{
    setenv("SDL_VIDEODRIVER", "emscripten", 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        CHECK(false, "browser SDL_Init: %s", SDL_GetError());
        return;
    }
    CHECK(strcmp(SDL_GetCurrentVideoDriver(), "emscripten") == 0,
          "video driver is '%s'", SDL_GetCurrentVideoDriver());

    SDL_Window *w = SDL_CreateWindow("web", 320, 240, 0);
    CHECK(w != NULL, "CreateWindow: %s", SDL_GetError());
    if (!w) {
        SDL_Quit();
        return;
    }
    CHECK(SDL_GetWindowSize(w, NULL, NULL) && true, "GetWindowSize");
    int cw = 0, ch = 0;
    MAIN_THREAD_EM_ASM(
        {
            const c = document.getElementById("canvas");
            setValue($0, c.width, "i32");
            setValue($1, c.height, "i32");
        },
        &cw, &ch);
    CHECK(cw == 320 && ch == 240, "canvas sized to window (%dx%d)", cw, ch);

    /* stock SDL3: TEXT_INPUT only flows after an explicit opt-in */
    CHECK(!SDL_TextInputActive(w), "text input inactive by default");
    CHECK(SDL_StartTextInput(w), "StartTextInput: %s", SDL_GetError());
    CHECK(SDL_TextInputActive(w), "text input active after StartTextInput");

    /* keyboard: synthetic KeyboardEvent on window */
    dispatch_js("window.dispatchEvent(new KeyboardEvent('keydown', {code:'KeyW', key:'w', bubbles:true, cancelable:true}))");
    SDL_PumpEvents();
    const char *text = NULL;
    CHECK(poll_key(SDL_SCANCODE_W, true, &text), "KEY_DOWN W from DOM");
    /* text event: pump again, it was pushed right after the key */
    CHECK(text && strcmp(text, "w") == 0, "text input 'w' (got %s)", text ? text : "(none)");

    dispatch_js("window.dispatchEvent(new KeyboardEvent('keydown', {code:'ShiftLeft', key:'Shift', location:2, shiftKey:true, bubbles:true}))");
    dispatch_js("window.dispatchEvent(new KeyboardEvent('keydown', {code:'KeyW', key:'W', shiftKey:true, bubbles:true}))");
    SDL_PumpEvents();
    SDL_Event e;
    bool saw_shifted = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_W) {
            saw_shifted = (e.key.key == SDLK_W) && (e.key.mod & SDL_KMOD_LSHIFT) != 0;
        }
    }
    CHECK(saw_shifted, "shift+W gives SDLK_W + LSHIFT");
    dispatch_js("window.dispatchEvent(new KeyboardEvent('keyup', {code:'KeyW', key:'w', shiftKey:true, bubbles:true}))");
    dispatch_js("window.dispatchEvent(new KeyboardEvent('keyup', {code:'ShiftLeft', key:'Shift', location:2, bubbles:true}))");
    SDL_PumpEvents();
    CHECK(poll_key(SDL_SCANCODE_W, false, NULL), "KEY_UP W");
    CHECK(!SDL_GetKeyboardState(NULL)[SDL_SCANCODE_LSHIFT], "shift released in keystate");

    /* mouse: synthetic events on #canvas */
    dispatch_js("const c = document.getElementById('canvas');"
                "c.dispatchEvent(new MouseEvent('mousemove', {clientX:100, clientY:50, bubbles:true}));"
                "c.dispatchEvent(new MouseEvent('mousedown', {button:0, clientX:100, clientY:50, bubbles:true}));");
    SDL_PumpEvents();
    bool saw_down = false, saw_motion = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.x == 100.0f && e.motion.y == 50.0f) {
            saw_motion = true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            saw_down = true;
        }
    }
    CHECK(saw_motion, "MOUSE_MOTION (100,50) from DOM");
    CHECK(saw_down, "MOUSE_BUTTON_DOWN left from DOM");
    CHECK((SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) != 0, "button state latched");

    dispatch_js("const c2 = document.getElementById('canvas');"
                "c2.dispatchEvent(new MouseEvent('mouseup', {button:0, clientX:100, clientY:50, bubbles:true}));");
    SDL_PumpEvents();
    bool saw_up = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            saw_up = true;
        }
    }
    CHECK(saw_up, "MOUSE_BUTTON_UP left");

    /* wheel: DOM deltaY>0 (scroll down) -> SDLop y<0 */
    dispatch_js("const c3 = document.getElementById('canvas');"
                "c3.dispatchEvent(new WheelEvent('wheel', {deltaX:0, deltaY:120, deltaMode:0, bubbles:true, cancelable:true}));");
    SDL_PumpEvents();
    bool saw_wheel = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_MOUSE_WHEEL) {
            saw_wheel = (e.wheel.y == -120.0f);
        }
    }
    CHECK(saw_wheel, "WHEEL y=-120 from DOM deltaY=120");

    /* GL context creation (WebGL) */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_Window *gw = SDL_CreateWindow("webgl", 64, 64, SDL_WINDOW_OPENGL);
    SDL_GLContext ctx = gw ? SDL_GL_CreateContext(gw) : NULL;
    CHECK(ctx != NULL, "WebGL context: %s", SDL_GetError());
    if (ctx) {
        SDL_GL_MakeCurrent(gw, ctx);
        /* WebGL without linking GL at build time: query through SDL */
        typedef const unsigned char *(*PFN_glGetString)(unsigned int);
        PFN_glGetString gls = (PFN_glGetString)SDL_GL_GetProcAddress("glGetString");
        CHECK(gls != NULL, "glGetString proc");
        if (gls) {
            const char *ver = (const char *)gls(0x1F02 /*GL_VERSION*/);
            LOGF("WebGL version: %s", ver ? ver : "(null)");
            CHECK(ver && strstr(ver, "WebGL") != NULL, "version mentions WebGL");
        }
        SDL_GL_DestroyContext(ctx);
    }
    if (gw) {
        SDL_DestroyWindow(gw);
    }

    /* software surface path: blit through the internal WebGL context */
    SDL_Surface *s = SDL_GetWindowSurface(w);
    CHECK(s != NULL, "window surface: %s", SDL_GetError());
    if (s) {
        SDL_FillSurfaceRect(s, NULL, SDL_MapSurfaceRGB(s, 255, 0, 255));
        CHECK(SDL_UpdateWindowSurface(w), "UpdateWindowSurface: %s", SDL_GetError());
    }

    SDL_DestroyWindow(w);
    SDL_Quit();
}
#endif /* __EMSCRIPTEN__ */

int main(void)
{
#ifndef __EMSCRIPTEN__
    /* headless compositors never deliver a keymap; the table checks need
     * one, exactly like a session with a real keyboard layout */
    SDLOP_KeyboardSetDefaultKeymap();
#endif
    in_browser = has_dom();
    LOGF("test_web: environment = %s", in_browser ? "browser" : "node");

    test_keymap_table();

    if (!in_browser) {
        /* node: core through the dummy driver */
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            CHECK(false, "node SDL_Init: %s", SDL_GetError());
        } else {
#ifdef __EMSCRIPTEN__
            CHECK(strcmp(SDL_GetCurrentVideoDriver(), "dummy") == 0,
                  "node falls back to dummy ('%s')", SDL_GetCurrentVideoDriver());
#endif
            SDL_Window *w = SDL_CreateWindow("node", 64, 64, 0);
            CHECK(w != NULL, "node window");
            SDL_DestroyWindow(w);
            SDL_Quit();
        }
    }

#ifdef __EMSCRIPTEN__
    if (in_browser) {
        test_browser_driver();
    }
#endif

    if (failures) {
        LOGF("test_web: FAIL (%d checks)", failures);
        return 1;
    }
    LOGF("test_web: PASS");
    return 0;
}
