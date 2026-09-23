/*
  SDLop - Emscripten (HTML5 canvas / WebGL) video driver.

  Windows map to canvas elements (first window: "#canvas", extra windows
  get their own canvas in document.body). Input comes from emscripten
  html5 callbacks and is pushed straight into the SDL event queue;
  OpenGL is WebGL1/WebGL2 via emscripten_webgl_*.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#if SDLop_VIDEO_EMSCRIPTEN

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>

typedef struct EmscriptenWindowData
{
    char selector[32];      /* "#canvas" or "#sdlop-canvas-N" */
    bool owns_canvas;       /* created by us (extra windows) */
    int webgl_context;      /* app's context, 0 if none */
    int sw_context;         /* internal context for software blits */
    bool sw_gl_ready;       /* shader/attribs uploaded */
    GLuint sw_tex;
    GLuint sw_prog;
    GLint sw_pos_loc, sw_uv_loc;
    bool pointer_locked;
} EmscriptenWindowData;

static bool input_callbacks_registered = false;
static int swap_interval = 0;
static int next_canvas_id = 0;

/* ------------------------------------------------------------------ */
/* key mapping (DOM code string -> scancode)                           */
/* ------------------------------------------------------------------ */

struct code_map
{
    const char *code;
    SDL_Scancode scancode;
};

/* clang-format off */
static const struct code_map code_table[] = {
    { "KeyA", SDL_SCANCODE_A }, { "KeyB", SDL_SCANCODE_B }, { "KeyC", SDL_SCANCODE_C },
    { "KeyD", SDL_SCANCODE_D }, { "KeyE", SDL_SCANCODE_E }, { "KeyF", SDL_SCANCODE_F },
    { "KeyG", SDL_SCANCODE_G }, { "KeyH", SDL_SCANCODE_H }, { "KeyI", SDL_SCANCODE_I },
    { "KeyJ", SDL_SCANCODE_J }, { "KeyK", SDL_SCANCODE_K }, { "KeyL", SDL_SCANCODE_L },
    { "KeyM", SDL_SCANCODE_M }, { "KeyN", SDL_SCANCODE_N }, { "KeyO", SDL_SCANCODE_O },
    { "KeyP", SDL_SCANCODE_P }, { "KeyQ", SDL_SCANCODE_Q }, { "KeyR", SDL_SCANCODE_R },
    { "KeyS", SDL_SCANCODE_S }, { "KeyT", SDL_SCANCODE_T }, { "KeyU", SDL_SCANCODE_U },
    { "KeyV", SDL_SCANCODE_V }, { "KeyW", SDL_SCANCODE_W }, { "KeyX", SDL_SCANCODE_X },
    { "KeyY", SDL_SCANCODE_Y }, { "KeyZ", SDL_SCANCODE_Z },
    { "Digit0", SDL_SCANCODE_0 }, { "Digit1", SDL_SCANCODE_1 }, { "Digit2", SDL_SCANCODE_2 },
    { "Digit3", SDL_SCANCODE_3 }, { "Digit4", SDL_SCANCODE_4 }, { "Digit5", SDL_SCANCODE_5 },
    { "Digit6", SDL_SCANCODE_6 }, { "Digit7", SDL_SCANCODE_7 }, { "Digit8", SDL_SCANCODE_8 },
    { "Digit9", SDL_SCANCODE_9 },
    { "F1", SDL_SCANCODE_F1 }, { "F2", SDL_SCANCODE_F2 }, { "F3", SDL_SCANCODE_F3 },
    { "F4", SDL_SCANCODE_F4 }, { "F5", SDL_SCANCODE_F5 }, { "F6", SDL_SCANCODE_F6 },
    { "F7", SDL_SCANCODE_F7 }, { "F8", SDL_SCANCODE_F8 }, { "F9", SDL_SCANCODE_F9 },
    { "F10", SDL_SCANCODE_F10 }, { "F11", SDL_SCANCODE_F11 }, { "F12", SDL_SCANCODE_F12 },
    { "Numpad0", SDL_SCANCODE_KP_0 }, { "Numpad1", SDL_SCANCODE_KP_1 },
    { "Numpad2", SDL_SCANCODE_KP_2 }, { "Numpad3", SDL_SCANCODE_KP_3 },
    { "Numpad4", SDL_SCANCODE_KP_4 }, { "Numpad5", SDL_SCANCODE_KP_5 },
    { "Numpad6", SDL_SCANCODE_KP_6 }, { "Numpad7", SDL_SCANCODE_KP_7 },
    { "Numpad8", SDL_SCANCODE_KP_8 }, { "Numpad9", SDL_SCANCODE_KP_9 },
    { "NumpadAdd", SDL_SCANCODE_KP_PLUS }, { "NumpadSubtract", SDL_SCANCODE_KP_MINUS },
    { "NumpadMultiply", SDL_SCANCODE_KP_MULTIPLY }, { "NumpadDivide", SDL_SCANCODE_KP_DIVIDE },
    { "NumpadDecimal", SDL_SCANCODE_KP_PERIOD }, { "NumpadEnter", SDL_SCANCODE_KP_ENTER },
    { "NumpadEqual", SDL_SCANCODE_KP_EQUALS },
    { "Enter", SDL_SCANCODE_RETURN }, { "Space", SDL_SCANCODE_SPACE },
    { "Tab", SDL_SCANCODE_TAB }, { "Backspace", SDL_SCANCODE_BACKSPACE },
    { "Escape", SDL_SCANCODE_ESCAPE }, { "CapsLock", SDL_SCANCODE_CAPSLOCK },
    { "NumLock", SDL_SCANCODE_NUMLOCKCLEAR }, { "ScrollLock", SDL_SCANCODE_SCROLLLOCK },
    { "ShiftLeft", SDL_SCANCODE_LSHIFT }, { "ShiftRight", SDL_SCANCODE_RSHIFT },
    { "ControlLeft", SDL_SCANCODE_LCTRL }, { "ControlRight", SDL_SCANCODE_RCTRL },
    { "AltLeft", SDL_SCANCODE_LALT }, { "AltRight", SDL_SCANCODE_RALT },
    { "MetaLeft", SDL_SCANCODE_LGUI }, { "MetaRight", SDL_SCANCODE_RGUI },
    { "ArrowUp", SDL_SCANCODE_UP }, { "ArrowDown", SDL_SCANCODE_DOWN },
    { "ArrowLeft", SDL_SCANCODE_LEFT }, { "ArrowRight", SDL_SCANCODE_RIGHT },
    { "Home", SDL_SCANCODE_HOME }, { "End", SDL_SCANCODE_END },
    { "PageUp", SDL_SCANCODE_PAGEUP }, { "PageDown", SDL_SCANCODE_PAGEDOWN },
    { "Insert", SDL_SCANCODE_INSERT }, { "Delete", SDL_SCANCODE_DELETE },
    { "Minus", SDL_SCANCODE_MINUS }, { "Equal", SDL_SCANCODE_EQUALS },
    { "BracketLeft", SDL_SCANCODE_LEFTBRACKET }, { "BracketRight", SDL_SCANCODE_RIGHTBRACKET },
    { "Backslash", SDL_SCANCODE_BACKSLASH }, { "Semicolon", SDL_SCANCODE_SEMICOLON },
    { "Quote", SDL_SCANCODE_APOSTROPHE }, { "Backquote", SDL_SCANCODE_GRAVE },
    { "Comma", SDL_SCANCODE_COMMA }, { "Period", SDL_SCANCODE_PERIOD },
    { "Slash", SDL_SCANCODE_SLASH }, { "PrintScreen", SDL_SCANCODE_PRINTSCREEN },
    { "Pause", SDL_SCANCODE_PAUSE }, { "ContextMenu", SDL_SCANCODE_APPLICATION },
    { "IntlBackslash", SDL_SCANCODE_NONUSBACKSLASH },
};
/* clang-format on */

static SDL_Scancode scancode_from_code(const char *code)
{
    for (size_t i = 0; i < SDL_arraysize(code_table); i++) {
        if (strcmp(code_table[i].code, code) == 0) {
            return code_table[i].scancode;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

/* DOM button index -> SDL button (DOM: 0 left, 1 middle, 2 right,
 * 3 back, 4 forward; SDL: LEFT 1, MIDDLE 2, RIGHT 3, X1 4, X2 5) */
static Uint8 sdl_button_from_dom(int dom_button)
{
    switch (dom_button) {
    case 0:
        return SDL_BUTTON_LEFT;
    case 1:
        return SDL_BUTTON_MIDDLE;
    case 2:
        return SDL_BUTTON_RIGHT;
    case 3:
        return SDL_BUTTON_X1;
    case 4:
        return SDL_BUTTON_X2;
    default:
        return (Uint8)(dom_button + 1);
    }
}

static SDL_Window *window_from_selector(const char *selector)
{
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        EmscriptenWindowData *d = (EmscriptenWindowData *)w->driverdata;
        if (d && strcmp(d->selector, selector) == 0) {
            return w;
        }
    }
    return sdlop.windows; /* fallback: first window */
}

/* ------------------------------------------------------------------ */
/* input callbacks                                                     */
/* ------------------------------------------------------------------ */

static EM_BOOL key_cb(int event_type, const EmscriptenKeyboardEvent *e, void *user_data)
{
    (void)user_data;
    SDL_Scancode sc = scancode_from_code(e->code);
    if (sc == SDL_SCANCODE_UNKNOWN && e->keyCode == 229) {
        return EM_FALSE; /* IME composition key, let the browser handle it */
    }
    /* modifiers: DOM tells us which are down; left/right comes from the
     * code for the modifier keys themselves */
    SDL_Keymod mod = 0;
    if (e->shiftKey) {
        mod |= SDL_KMOD_LSHIFT;
    }
    if (e->ctrlKey) {
        mod |= SDL_KMOD_LCTRL;
    }
    if (e->altKey) {
        mod |= SDL_KMOD_LALT;
    }
    if (e->metaKey) {
        mod |= SDL_KMOD_LGUI;
    }
    if (e->location == DOM_KEY_LOCATION_RIGHT) {
        mod &= ~(SDL_KMOD_LSHIFT | SDL_KMOD_LCTRL | SDL_KMOD_LALT | SDL_KMOD_LGUI);
        if (e->shiftKey) {
            mod |= SDL_KMOD_RSHIFT;
        }
        if (e->ctrlKey) {
            mod |= SDL_KMOD_RCTRL;
        }
        if (e->altKey) {
            mod |= SDL_KMOD_RALT;
        }
        if (e->metaKey) {
            mod |= SDL_KMOD_RGUI;
        }
    }
    sdlop.modstate = mod;

    bool down = (event_type == EMSCRIPTEN_EVENT_KEYDOWN);
    SDLOP_SendKeyboardKey(down, (bool)e->repeat, sc, 0, SDLOP_MonotonicNS());
    return EM_TRUE;
}

static EM_BOOL mouse_move_cb(int event_type, const EmscriptenMouseEvent *e, void *user_data)
{
    (void)event_type;
    SDL_Window *w = window_from_selector((const char *)user_data);
    if (!w) {
        return EM_FALSE;
    }
    EmscriptenWindowData *d = (EmscriptenWindowData *)w->driverdata;
    Uint64 now = SDLOP_MonotonicNS();
    if (d && d->pointer_locked) {
        SDLOP_SendMouseMotion(0, 0, (float)e->movementX, (float)e->movementY, now);
    } else {
        SDLOP_SendMouseMotion((float)e->targetX, (float)e->targetY, 0, 0, now);
    }
    return EM_TRUE;
}

static EM_BOOL mouse_button_cb(int event_type, const EmscriptenMouseEvent *e, void *user_data)
{
    SDL_Window *w = window_from_selector((const char *)user_data);
    if (w) {
        sdlop.mouse_x = (float)e->targetX;
        sdlop.mouse_y = (float)e->targetY;
    }
    Uint8 button = sdl_button_from_dom(e->button);
    bool down = (event_type == EMSCRIPTEN_EVENT_MOUSEDOWN);
    SDLOP_SendMouseButton(down, button, (float)e->targetX, (float)e->targetY, SDLOP_MonotonicNS());
    return EM_TRUE;
}

static EM_BOOL wheel_cb(int event_type, const EmscriptenWheelEvent *e, void *user_data)
{
    (void)event_type;
    (void)user_data;
    double dx = e->deltaX, dy = e->deltaY;
    if (e->deltaMode == DOM_DELTA_LINE) {
        dx *= 20.0;
        dy *= 20.0;
    } else if (e->deltaMode == DOM_DELTA_PAGE) {
        dx *= 800.0;
        dy *= 800.0;
    }
    /* DOM: deltaY > 0 scrolls down; SDLop (like SDL3): y negative = down */
    SDLOP_SendMouseWheel((float)-dx, (float)-dy, SDLOP_MonotonicNS());
    return EM_TRUE;
}

static EM_BOOL pointerlock_cb(int event_type, const EmscriptenPointerlockChangeEvent *e, void *user_data)
{
    (void)event_type;
    (void)user_data;
    const char *sel = e->isActive ? (e->id[0] ? e->id : "#canvas") : NULL;
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        EmscriptenWindowData *d = (EmscriptenWindowData *)w->driverdata;
        if (!d) {
            continue;
        }
        bool active = sel && (e->id[0] == '\0' || strcmp(e->id, d->selector + 1) == 0);
        if (active != d->pointer_locked) {
            d->pointer_locked = active;
            SDLOP_SendWindowEvent(w, active ? SDL_EVENT_WINDOW_MOUSE_ENTER : SDL_EVENT_WINDOW_MOUSE_LEAVE, 0, 0);
        }
    }
    return EM_TRUE;
}

static EM_BOOL focus_cb(int event_type, const EmscriptenFocusEvent *e, void *user_data)
{
    (void)e;
    (void)user_data;
    SDL_Window *w = sdlop.keyboard_focus ? sdlop.keyboard_focus : sdlop.windows;
    if (w) {
        SDLOP_SendWindowEvent(w, event_type == EMSCRIPTEN_EVENT_FOCUS ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
    }
    return EM_TRUE;
}

static void register_input_callbacks(const char *selector)
{
    if (!input_callbacks_registered) {
        input_callbacks_registered = true;
        emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, key_cb);
        emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, key_cb);
        emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_TRUE, pointerlock_cb);
        emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, focus_cb);
        emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, focus_cb);
    }
    /* mouse/wheel are per-canvas; the selector string outlives the window
     * (callbacks are torn down with the document) */
    static char selectors[SDLOP_MAX_WINDOWS][32];
    static int nsel = 0;
    char *sel = selectors[nsel < SDLOP_MAX_WINDOWS ? nsel : 0];
    if (nsel < SDLOP_MAX_WINDOWS) {
        nsel++;
        strncpy(sel, selector, sizeof(selectors[0]) - 1);
        sel[sizeof(selectors[0]) - 1] = '\0';
    }
    emscripten_set_mousedown_callback(sel, sel, EM_TRUE, mouse_button_cb);
    emscripten_set_mouseup_callback(sel, sel, EM_TRUE, mouse_button_cb);
    emscripten_set_mousemove_callback(sel, sel, EM_TRUE, mouse_move_cb);
    emscripten_set_wheel_callback(sel, sel, EM_TRUE, wheel_cb);
}

/* ------------------------------------------------------------------ */
/* driver implementation                                               */
/* ------------------------------------------------------------------ */

static bool emscripten_Init(SDLop_VideoDevice *device)
{
    (void)device;
    /* #canvas must exist (emscripten provides it in browser shells);
     * in node tests SDL_VIDEODRIVER=dummy is used instead */
    int w = 0, h = 0;
    if (emscripten_get_canvas_element_size("#canvas", &w, &h) != EMSCRIPTEN_RESULT_SUCCESS) {
        return SDL_SetError("Emscripten: no #canvas element");
    }
    return true;
}

static void emscripten_Quit(SDLop_VideoDevice *device)
{
    (void)device;
}

static bool emscripten_CreateWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    EmscriptenWindowData *d = (EmscriptenWindowData *)calloc(1, sizeof(*d));
    if (!d) {
        return SDL_OutOfMemory();
    }

    bool first = true;
    for (SDL_Window *w = sdlop.windows; w && first; w = w->next) {
        if (w != window) {
            first = false;
        }
    }
    if (first) {
        strncpy(d->selector, "#canvas", sizeof(d->selector) - 1);
    } else {
        int id = next_canvas_id++;
        snprintf(d->selector, sizeof(d->selector), "#sdlop-canvas-%d", id);
        MAIN_THREAD_EM_ASM(
            {
                const c = document.createElement("canvas");
                c.id = "sdlop-canvas-" + $0;
                document.body.appendChild(c);
            },
            id);
        d->owns_canvas = true;
    }
    emscripten_set_canvas_element_size(d->selector, window->w, window->h);
    window->driverdata = d;
    register_input_callbacks(d->selector);
    return true;
}

static void emscripten_DestroyWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    EmscriptenWindowData *d = (EmscriptenWindowData *)window->driverdata;
    if (!d) {
        return;
    }
    if (d->owns_canvas) {
        MAIN_THREAD_EM_ASM({ const c = document.getElementById(UTF8ToString($0)); if (c) c.remove(); }, d->selector + 1);
    }
    if (d->webgl_context) {
        emscripten_webgl_destroy_context(d->webgl_context);
    }
    if (d->sw_context) {
        emscripten_webgl_destroy_context(d->sw_context);
    }
    free(d);
    window->driverdata = NULL;
}

static void emscripten_ShowWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    sdlop.keyboard_focus = window;
    sdlop.mouse_focus = window;
    window->flags |= SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
}

static void emscripten_HideWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    window->flags &= ~(SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
    if (sdlop.keyboard_focus == window) {
        sdlop.keyboard_focus = NULL;
    }
    if (sdlop.mouse_focus == window) {
        sdlop.mouse_focus = NULL;
    }
}

static bool emscripten_SetWindowTitle(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    MAIN_THREAD_EM_ASM({ document.title = UTF8ToString($0); }, window->title ? window->title : "");
    return true;
}

static bool emscripten_SetWindowSize(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    EmscriptenWindowData *d = (EmscriptenWindowData *)window->driverdata;
    if (d) {
        emscripten_set_canvas_element_size(d->selector, window->w, window->h);
    }
    if (window->surface) {
        void *pixels = realloc(window->surface->pixels, (size_t)window->w * 4u * (size_t)window->h);
        if (!pixels) {
            return SDL_OutOfMemory();
        }
        window->surface->pixels = pixels;
        window->surface->w = window->w;
        window->surface->h = window->h;
        window->surface->pitch = window->w * 4;
    }
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window->w, window->h);
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, window->w, window->h);
    return true;
}

static bool emscripten_SetWindowPosition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    return true; /* canvas position is CSS's business */
}

static void emscripten_MinimizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
}

static void emscripten_MaximizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
}

static void emscripten_RestoreWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
}

static bool emscripten_SetWindowFullscreen(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    EmscriptenWindowData *d = (EmscriptenWindowData *)window->driverdata;
    if (!d) {
        return SDL_SetError("No window data");
    }
    bool want_full = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
    if (want_full) {
        EmscriptenFullscreenStrategy strat = { .scaleMode = EMSCRIPTEN_FULLSCREEN_SCALE_ASPECT, .canvasResolutionScaleMode = EMSCRIPTEN_FULLSCREEN_CANVAS_SCALE_HIDEF, .filteringMode = EMSCRIPTEN_FULLSCREEN_FILTERING_DEFAULT };
        EMSCRIPTEN_RESULT r = emscripten_request_fullscreen_strategy(d->selector, EM_FALSE, &strat);
        if (r != EMSCRIPTEN_RESULT_SUCCESS && r != EMSCRIPTEN_RESULT_DEFERRED) {
            return SDL_SetError("Fullscreen request failed (%d) - needs a user gesture", (int)r);
        }
    } else {
        emscripten_exit_fullscreen();
    }
    return true;
}

static void emscripten_RaiseWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
}

static void emscripten_SetWindowClearColor(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
}

static bool emscripten_SetWindowRelativeMouseMode(SDLop_VideoDevice *device, SDL_Window *window, bool enabled)
{
    (void)device;
    EmscriptenWindowData *d = (EmscriptenWindowData *)window->driverdata;
    if (!d) {
        return SDL_SetError("No window data");
    }
    EMSCRIPTEN_RESULT r;
    if (enabled) {
        r = emscripten_request_pointerlock(d->selector, EM_FALSE);
        if (r != EMSCRIPTEN_RESULT_SUCCESS && r != EMSCRIPTEN_RESULT_DEFERRED) {
            return SDL_SetError("Pointer lock refused (%d) - needs a user gesture", (int)r);
        }
    } else {
        r = emscripten_exit_pointerlock();
        if (r != EMSCRIPTEN_RESULT_SUCCESS) {
            return SDL_SetError("Pointer unlock failed (%d)", (int)r);
        }
    }
    return true;
}

static void emscripten_PumpEvents(SDLop_VideoDevice *device, int timeout_ms)
{
    (void)device;
    (void)timeout_ms;
    /* callbacks push events directly from the browser event loop */
}

static int emscripten_GetEventFD(SDLop_VideoDevice *device)
{
    (void)device;
    return -1;
}

/* ------------------------------------------------------------------ */
/* software framebuffer (WebGL texture blit)                           */
/* ------------------------------------------------------------------ */

static bool emscripten_CreateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface **surface)
{
    (void)device;
    SDL_Surface *s = SDL_CreateSurface(window->w, window->h, SDL_PIXELFORMAT_ARGB8888);
    if (!s) {
        return false;
    }
    window->surface = s;
    *surface = s;
    return true;
}

static bool sw_ensure_gl(SDL_Window *window)
{
    EmscriptenWindowData *d = (EmscriptenWindowData *)window->driverdata;
    if (!d) {
        return SDL_SetError("No window data");
    }
    if (!d->sw_context) {
        EmscriptenWebGLContextAttributes attrs;
        emscripten_webgl_init_context_attributes(&attrs);
        attrs.alpha = EM_FALSE;
        attrs.depth = EM_FALSE;
        d->sw_context = emscripten_webgl_create_context(d->selector, &attrs);
        if (d->sw_context <= 0) {
            return SDL_SetError("WebGL context creation failed (%d)", d->sw_context);
        }
    }
    if (emscripten_webgl_make_context_current(d->sw_context) != EMSCRIPTEN_RESULT_SUCCESS) {
        return SDL_SetError("WebGL make-current failed");
    }
    if (!d->sw_gl_ready) {
        static const char *vs = "attribute vec2 p; attribute vec2 uv; varying vec2 vuv; void main() { vuv = uv; gl_Position = vec4(p, 0.0, 1.0); }";
        static const char *fs = "precision mediump float; varying vec2 vuv; uniform sampler2D t; void main() { gl_FragColor = texture2D(t, vuv); }";
        GLuint v = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(v, 1, &vs, NULL);
        glCompileShader(v);
        GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(f, 1, &fs, NULL);
        glCompileShader(f);
        d->sw_prog = glCreateProgram();
        glAttachShader(d->sw_prog, v);
        glAttachShader(d->sw_prog, f);
        glLinkProgram(d->sw_prog);
        glDeleteShader(v);
        glDeleteShader(f);
        d->sw_pos_loc = glGetAttribLocation(d->sw_prog, "p");
        d->sw_uv_loc = glGetAttribLocation(d->sw_prog, "uv");
        glGenTextures(1, &d->sw_tex);
        glBindTexture(GL_TEXTURE_2D, d->sw_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        d->sw_gl_ready = true;
    }
    return true;
}

static bool emscripten_UpdateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    (void)device;
    (void)rects;
    (void)numrects;
    SDL_Surface *s = window->surface;
    if (!s || !s->pixels) {
        return true;
    }
    if (!sw_ensure_gl(window)) {
        return false;
    }
    EmscriptenWindowData *d = (EmscriptenWindowData *)window->driverdata;
    /* ARGB8888 in memory = little-endian BGRA bytes = GL_BGRA_EXT when
     * available; otherwise upload as RGBA with a shader-free swizzle is
     * overkill - use BGRA_EXT, universally present in WebGL */
    glBindTexture(GL_TEXTURE_2D, d->sw_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
    glViewport(0, 0, window->w, window->h);
    glUseProgram(d->sw_prog);
    /* fullscreen quad, v flipped (surface row 0 = top) */
    static const float verts[16] = { -1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0, 1, 1, 1, 0 };
    glVertexAttribPointer((GLuint)d->sw_pos_loc, 2, GL_FLOAT, GL_FALSE, 16, verts);
    glEnableVertexAttribArray((GLuint)d->sw_pos_loc);
    glVertexAttribPointer((GLuint)d->sw_uv_loc, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
    glEnableVertexAttribArray((GLuint)d->sw_uv_loc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray((GLuint)d->sw_pos_loc);
    glDisableVertexAttribArray((GLuint)d->sw_uv_loc);
    return true;
}

static void emscripten_DestroyWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    EmscriptenWindowData *d = (EmscriptenWindowData *)window->driverdata;
    if (d && d->sw_gl_ready && d->sw_context) {
        emscripten_webgl_make_context_current(d->sw_context);
        glDeleteTextures(1, &d->sw_tex);
        glDeleteProgram(d->sw_prog);
        d->sw_gl_ready = false;
    }
}

/* ------------------------------------------------------------------ */
/* WebGL                                                               */
/* ------------------------------------------------------------------ */

static void *emscripten_GL_CreateContext(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    EmscriptenWindowData *d = (EmscriptenWindowData *)window->driverdata;
    if (!d) {
        SDL_SetError("No window data");
        return NULL;
    }
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    int major = sdlop_glattrs.major_version;
    bool want_es3 = (sdlop_glattrs.profile_mask == SDL_GL_CONTEXT_PROFILE_ES && major >= 3) || (sdlop_glattrs.profile_mask == 0 && major >= 3);
    attrs.majorVersion = want_es3 ? 2 : 1; /* WebGL2 ~ GLES3, WebGL1 ~ GLES2 */
    attrs.alpha = sdlop_glattrs.alpha_size > 0 ? EM_TRUE : EM_FALSE;
    attrs.depth = sdlop_glattrs.depth_size > 0 ? EM_TRUE : EM_FALSE;
    attrs.stencil = sdlop_glattrs.stencil_size > 0 ? EM_TRUE : EM_FALSE;
    attrs.antialias = sdlop_glattrs.multisamplebuffers > 0 ? EM_TRUE : EM_FALSE;
    attrs.preserveDrawingBuffer = EM_FALSE;
    attrs.failIfMajorPerformanceCaveat = EM_FALSE;

    int handle = emscripten_webgl_create_context(d->selector, &attrs);
    if (handle <= 0) {
        SDL_SetError("WebGL context creation failed (%d)", handle);
        return NULL;
    }
    d->webgl_context = handle;
    if (emscripten_webgl_make_context_current(handle) != EMSCRIPTEN_RESULT_SUCCESS) {
        emscripten_webgl_destroy_context(handle);
        d->webgl_context = 0;
        SDL_SetError("WebGL make-current failed");
        return NULL;
    }
    return (void *)(intptr_t)handle;
}

static bool emscripten_GL_MakeCurrent(SDLop_VideoDevice *device, SDL_Window *window, void *context)
{
    (void)device;
    (void)window;
    if (emscripten_webgl_make_context_current((int)(intptr_t)context) != EMSCRIPTEN_RESULT_SUCCESS) {
        return SDL_SetError("WebGL make-current failed");
    }
    return true;
}

static bool emscripten_GL_SwapBuffers(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    (void)window;
    return true; /* the browser composites at frame boundaries */
}

static void emscripten_GL_DeleteContext(SDLop_VideoDevice *device, void *context)
{
    (void)device;
    emscripten_webgl_destroy_context((int)(intptr_t)context);
    for (SDL_Window *w = sdlop.windows; w; w = w->next) {
        EmscriptenWindowData *d = (EmscriptenWindowData *)w->driverdata;
        if (d && d->webgl_context == (int)(intptr_t)context) {
            d->webgl_context = 0;
        }
    }
}

static SDL_FunctionPointer emscripten_GL_GetProcAddress(SDLop_VideoDevice *device, const char *proc)
{
    (void)device;
    return (SDL_FunctionPointer)emscripten_webgl_get_proc_address(proc);
}

static bool emscripten_GL_SetSwapInterval(SDLop_VideoDevice *device, int interval)
{
    (void)device;
    swap_interval = interval;
    if (interval <= 0) {
        emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 0);
    } else {
        emscripten_set_main_loop_timing(EM_TIMING_RAF, interval);
    }
    return true;
}

static bool emscripten_GL_GetSwapInterval(SDLop_VideoDevice *device, int *interval)
{
    (void)device;
    *interval = swap_interval;
    return true;
}

SDLop_VideoDevice SDLop_emscripten_device = {
    .name = "emscripten",
    .Init = emscripten_Init,
    .Quit = emscripten_Quit,
    .CreateWindow = emscripten_CreateWindow,
    .DestroyWindow = emscripten_DestroyWindow,
    .ShowWindow = emscripten_ShowWindow,
    .HideWindow = emscripten_HideWindow,
    .SetWindowTitle = emscripten_SetWindowTitle,
    .SetWindowSize = emscripten_SetWindowSize,
    .SetWindowPosition = emscripten_SetWindowPosition,
    .MinimizeWindow = emscripten_MinimizeWindow,
    .MaximizeWindow = emscripten_MaximizeWindow,
    .RestoreWindow = emscripten_RestoreWindow,
    .SetWindowFullscreen = emscripten_SetWindowFullscreen,
    .RaiseWindow = emscripten_RaiseWindow,
    .SetWindowClearColor = emscripten_SetWindowClearColor,
    .SetWindowRelativeMouseMode = emscripten_SetWindowRelativeMouseMode,
    .PumpEvents = emscripten_PumpEvents,
    .GetEventFD = emscripten_GetEventFD,
    .CreateWindowFramebuffer = emscripten_CreateWindowFramebuffer,
    .UpdateWindowFramebuffer = emscripten_UpdateWindowFramebuffer,
    .DestroyWindowFramebuffer = emscripten_DestroyWindowFramebuffer,
    .GL_CreateContext = emscripten_GL_CreateContext,
    .GL_MakeCurrent = emscripten_GL_MakeCurrent,
    .GL_SwapBuffers = emscripten_GL_SwapBuffers,
    .GL_DeleteContext = emscripten_GL_DeleteContext,
    .GL_GetProcAddress = emscripten_GL_GetProcAddress,
    .GL_SetSwapInterval = emscripten_GL_SetSwapInterval,
    .GL_GetSwapInterval = emscripten_GL_GetSwapInterval,
    .Vulkan_CreateSurface = NULL, /* no Vulkan on the web */
};

#endif /* SDLop_VIDEO_EMSCRIPTEN */
