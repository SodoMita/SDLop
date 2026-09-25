/*
  ABI probe: the layouts and constants an application compiles against.

  Build it twice - once against SDLop's headers, once against the system SDL3's -
  and diff the output:

      make abi-check

  It calls nothing and links nothing: sizes, member offsets, event/scancode/
  keycode values, flags and hint strings are all compile-time facts, so the diff
  answers "would an application compiled against stock SDL3 3.2.10 see exactly
  the same things?" without either library being involved.
*/
#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdio.h>

#define V(x)   printf("%-46s %lld\n", #x, (long long)(x))
#define S(t)   printf("sizeof(%-27s) %lld\n", #t, (long long)sizeof(t))
#define O(t,f) printf("offsetof(%-19s,%-14s) %lld\n", #t, #f, (long long)offsetof(t, f))

int main(void)
{
    S(SDL_Event); S(SDL_Rect); S(SDL_FRect); S(SDL_Point); S(SDL_FPoint);
    S(SDL_KeyboardEvent); S(SDL_MouseMotionEvent); S(SDL_MouseButtonEvent);
    S(SDL_MouseWheelEvent); S(SDL_TextInputEvent); S(SDL_WindowEvent);
    S(SDL_UserEvent); S(SDL_DisplayMode); S(SDL_TouchFingerEvent);
    /* The device and text-editing events: an application that tracks keyboards
       and mice being plugged in reaches through these, and their layouts are as
       much part of the ABI as the more common ones. */
    S(SDL_KeyboardDeviceEvent); S(SDL_MouseDeviceEvent);
    S(SDL_TextEditingEvent); S(SDL_TextEditingCandidatesEvent);

    O(SDL_Event, type); O(SDL_Event, common.timestamp);
    /* The union members an application reaches through event.<member>. Their
       offsets are as much a part of the ABI as the struct sizes, and a missing
       member (SDL_TouchFingerEvent was one here) only shows up in an
       application's compile, not in a size check. */
    O(SDL_Event, common); O(SDL_Event, display); O(SDL_Event, window);
    O(SDL_Event, kdevice); O(SDL_Event, key); O(SDL_Event, edit);
    O(SDL_Event, edit_candidates); O(SDL_Event, text); O(SDL_Event, mdevice);
    O(SDL_Event, motion); O(SDL_Event, button); O(SDL_Event, wheel);
    O(SDL_Event, quit); O(SDL_Event, user); O(SDL_Event, tfinger);
    O(SDL_TouchFingerEvent, type); O(SDL_TouchFingerEvent, timestamp);
    O(SDL_TouchFingerEvent, touchID); O(SDL_TouchFingerEvent, fingerID);
    O(SDL_TouchFingerEvent, x); O(SDL_TouchFingerEvent, y);
    O(SDL_TouchFingerEvent, dx); O(SDL_TouchFingerEvent, dy);
    O(SDL_TouchFingerEvent, pressure); O(SDL_TouchFingerEvent, windowID);
    O(SDL_KeyboardEvent, timestamp); O(SDL_KeyboardEvent, windowID);
    O(SDL_KeyboardEvent, scancode); O(SDL_KeyboardEvent, key);
    O(SDL_KeyboardEvent, mod); O(SDL_KeyboardEvent, raw); O(SDL_KeyboardEvent, down);
    O(SDL_KeyboardEvent, repeat);
    O(SDL_MouseMotionEvent, x); O(SDL_MouseMotionEvent, y);
    O(SDL_MouseMotionEvent, xrel); O(SDL_MouseMotionEvent, yrel);
    O(SDL_MouseButtonEvent, button); O(SDL_MouseButtonEvent, down);
    O(SDL_MouseButtonEvent, clicks);
    O(SDL_MouseWheelEvent, x); O(SDL_MouseWheelEvent, y);
    O(SDL_MouseWheelEvent, direction);
    O(SDL_WindowEvent, data1); O(SDL_WindowEvent, data2);
    O(SDL_KeyboardDeviceEvent, timestamp); O(SDL_KeyboardDeviceEvent, which);
    O(SDL_MouseDeviceEvent, timestamp); O(SDL_MouseDeviceEvent, which);
    O(SDL_TextEditingEvent, windowID); O(SDL_TextEditingEvent, text);
    O(SDL_TextEditingEvent, start); O(SDL_TextEditingEvent, length);
    O(SDL_TextEditingCandidatesEvent, windowID); O(SDL_TextEditingCandidatesEvent, num_candidates);
    O(SDL_DisplayMode, displayID); O(SDL_DisplayMode, format);
    O(SDL_DisplayMode, w); O(SDL_DisplayMode, h); O(SDL_DisplayMode, refresh_rate);

    V(SDL_EVENT_QUIT); V(SDL_EVENT_KEY_DOWN); V(SDL_EVENT_KEY_UP);
    V(SDL_EVENT_TEXT_INPUT); V(SDL_EVENT_MOUSE_MOTION); V(SDL_EVENT_MOUSE_BUTTON_DOWN);
    V(SDL_EVENT_MOUSE_BUTTON_UP); V(SDL_EVENT_MOUSE_WHEEL); V(SDL_EVENT_WINDOW_SHOWN);
    V(SDL_EVENT_WINDOW_EXPOSED); V(SDL_EVENT_WINDOW_RESIZED); V(SDL_EVENT_WINDOW_MOVED);
    V(SDL_EVENT_WINDOW_FOCUS_GAINED); V(SDL_EVENT_WINDOW_FOCUS_LOST);
    V(SDL_EVENT_WINDOW_CLOSE_REQUESTED); V(SDL_EVENT_WINDOW_MOUSE_ENTER);
    V(SDL_EVENT_WINDOW_MOUSE_LEAVE); V(SDL_EVENT_WINDOW_DISPLAY_CHANGED);
    V(SDL_EVENT_CLIPBOARD_UPDATE); V(SDL_EVENT_USER); V(SDL_EVENT_LAST);
    /* The keyboard and mouse device events, in the order SDL3 declares them:
       a wrong position here shifts every event after it, which is exactly the
       kind of break a probe that only checks a handful of names misses. */
    V(SDL_EVENT_KEYMAP_CHANGED); V(SDL_EVENT_KEYBOARD_ADDED);
    V(SDL_EVENT_KEYBOARD_REMOVED); V(SDL_EVENT_TEXT_EDITING_CANDIDATES);
    V(SDL_EVENT_TEXT_EDITING); V(SDL_EVENT_TEXT_EDITING_CANDIDATES);
    V(SDL_EVENT_MOUSE_ADDED); V(SDL_EVENT_MOUSE_REMOVED);
    /* the finger events and the display/scale+orientation family */
    V(SDL_EVENT_FINGER_DOWN); V(SDL_EVENT_FINGER_UP); V(SDL_EVENT_FINGER_MOTION);
    V(SDL_EVENT_FINGER_CANCELED);
    V(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED); V(SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED);
    V(SDL_EVENT_WINDOW_SAFE_AREA_CHANGED); V(SDL_EVENT_WINDOW_OCCLUDED);
    V(SDL_EVENT_WINDOW_ENTER_FULLSCREEN); V(SDL_EVENT_WINDOW_LEAVE_FULLSCREEN);
    V(SDL_EVENT_WINDOW_DESTROYED); V(SDL_EVENT_DISPLAY_ADDED);
    V(SDL_EVENT_DISPLAY_REMOVED); V(SDL_EVENT_DISPLAY_ORIENTATION);
    V(SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED); V(SDL_EVENT_WINDOW_DISPLAY_CHANGED);
    V(SDL_EVENT_LOCALE_CHANGED); V(SDL_EVENT_SYSTEM_THEME_CHANGED);

    V(SDL_SCANCODE_A); V(SDL_SCANCODE_Z); V(SDL_SCANCODE_1); V(SDL_SCANCODE_RETURN);
    V(SDL_SCANCODE_ESCAPE); V(SDL_SCANCODE_UP); V(SDL_SCANCODE_F12);
    V(SDL_SCANCODE_LSHIFT); V(SDL_SCANCODE_RCTRL);
    V(SDL_SCANCODE_COUNT); V(SDL_SCANCODE_UNKNOWN);

    V(SDLK_ESCAPE); V(SDLK_RETURN); V(SDLK_UP); V(SDLK_F1); V(SDLK_SPACE);
    V(SDLK_LSHIFT); V(SDLK_RCTRL); V(SDLK_TAB);
    V(SDL_KMOD_NONE); V(SDL_KMOD_LSHIFT); V(SDL_KMOD_RSHIFT); V(SDL_KMOD_LCTRL);
    V(SDL_KMOD_RCTRL); V(SDL_KMOD_LALT); V(SDL_KMOD_RALT); V(SDL_KMOD_LGUI);
    V(SDL_KMOD_RGUI); V(SDL_KMOD_NUM); V(SDL_KMOD_CAPS); V(SDL_KMOD_MODE);
    V(SDL_KMOD_CTRL); V(SDL_KMOD_SHIFT); V(SDL_KMOD_ALT); V(SDL_KMOD_GUI);

    V(SDL_WINDOW_FULLSCREEN); V(SDL_WINDOW_OPENGL); V(SDL_WINDOW_HIDDEN);
    V(SDL_WINDOW_BORDERLESS); V(SDL_WINDOW_RESIZABLE); V(SDL_WINDOW_MINIMIZED);
    V(SDL_WINDOW_MAXIMIZED); V(SDL_WINDOW_MOUSE_GRABBED); V(SDL_WINDOW_INPUT_FOCUS);
    V(SDL_WINDOW_MOUSE_FOCUS); V(SDL_WINDOW_MOUSE_CAPTURE); V(SDL_WINDOW_ALWAYS_ON_TOP);
    V(SDL_WINDOW_HIGH_PIXEL_DENSITY); V(SDL_WINDOW_VULKAN); V(SDL_WINDOW_NOT_FOCUSABLE);

    V(SDL_BUTTON_LEFT); V(SDL_BUTTON_MIDDLE); V(SDL_BUTTON_RIGHT); V(SDL_BUTTON_X1);
    V(SDL_BUTTON_X2); V(SDL_BUTTON_MASK(SDL_BUTTON_LEFT));
    V(SDL_MOUSEWHEEL_NORMAL); V(SDL_MOUSEWHEEL_FLIPPED);
    V(SDL_INIT_VIDEO); V(SDL_INIT_EVENTS); 
    V(SDL_PIXELFORMAT_XRGB8888); V(SDL_PIXELFORMAT_ARGB8888);
    V(SDL_WINDOWPOS_CENTERED); V(SDL_WINDOWPOS_UNDEFINED);
    V(SDL_ORIENTATION_UNKNOWN); V(SDL_ORIENTATION_LANDSCAPE);
    V(SDL_ORIENTATION_PORTRAIT); V(SDL_ORIENTATION_LANDSCAPE_FLIPPED);
    V(SDL_ORIENTATION_PORTRAIT_FLIPPED);
    V(SDL_SYSTEM_THEME_UNKNOWN); V(SDL_GL_CONTEXT_MAJOR_VERSION);
    V(SDL_GL_DOUBLEBUFFER); V(SDL_GL_CONTEXT_PROFILE_CORE);
    /* Hints are strings: the pointer differs per build, the text must not. */
    printf("%-46s %s\n", "SDL_HINT_NO_SIGNAL_HANDLERS", SDL_HINT_NO_SIGNAL_HANDLERS);
    printf("%-46s %s\n", "SDL_HINT_VIDEO_DRIVER", SDL_HINT_VIDEO_DRIVER);
    printf("%-46s %s\n", "SDL_HINT_RETURN_KEY_HIDES_IME", SDL_HINT_RETURN_KEY_HIDES_IME);
    printf("%-46s %s\n", "SDL_HINT_MOUSE_RELATIVE_MODE_CENTER", SDL_HINT_MOUSE_RELATIVE_MODE_CENTER);
    printf("%-46s %s\n", "SDL_HINT_KEYCODE_OPTIONS", SDL_HINT_KEYCODE_OPTIONS);
    V(SDL_HITTEST_NORMAL); V(SDL_HITTEST_DRAGGABLE); V(SDL_HITTEST_RESIZE_BOTTOMLEFT);
    return 0;
}
