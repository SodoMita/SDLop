/*
  SDLop - Windows (Win32) video driver.

  One HWND per window. Messages arrive through the thread that created the
  window (Win32 requires that), so SDLop pumps them on the main thread and
  translates them into SDL events through the same internal injectors the
  other drivers use (SDLOP_SendWindowEvent, SDLOP_SendKeyboardKey, ...).

  Software rendering (SDL_GetWindowSurface) is a top-down 32-bit DIB section
  blitted with StretchDIBits/BitBlt; the GL and Vulkan halves live in
  sdlop_windows_gl.c and sdlop_windows_vulkan.c.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "internal/sdlop_win32_keymap.h"

#ifdef _WIN32

#include "sdlop_windows.h"

#include <windowsx.h>
#include <imm.h>

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

SDLop_Win32State sdlop_win32;

/* Taskbar button created message (Application User Model IDs) */
static UINT WM_TASKBARBUTTONCREATED;

#define SDLOP_WIN32_CLASS L"SDLop_Window"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

Uint64 SDLOP_Win32Timestamp(void)
{
    /* SDL timestamps count from SDL_Init; SDLOP_MonoToSDLTicks does that. */
    return SDLOP_MonoToSDLTicks(SDLOP_MonotonicNS());
}

static SDL_Window *window_from_hwnd(HWND hwnd)
{
    if (!hwnd) {
        return NULL;
    }
    return (SDL_Window *)GetPropW(hwnd, L"SDLopWindow");
}

bool SDLOP_Win32GetClientRectInPixels(SDL_Window *window, RECT *rc)
{
    if (!window || !window->driverdata) {
        return false;
    }
    HWND hwnd = ((SDLop_Win32WindowData *)window->driverdata)->hwnd;
    if (!hwnd || !GetClientRect(hwnd, rc)) {
        return false;
    }
    return true;
}

void SDLOP_Win32AdjustWindowRect(SDL_Window *window, RECT *rc)
{
    SDLop_Win32WindowData *data = window ? (SDLop_Win32WindowData *)window->driverdata : NULL;
    HWND hwnd = data ? data->hwnd : NULL;
    DWORD style = 0, exstyle = 0;
    if (hwnd) {
        style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
        exstyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    } else {
        style = WS_OVERLAPPEDWINDOW;
        exstyle = 0;
    }
    AdjustWindowRectEx(rc, style, FALSE, exstyle);
}

/* Convert a Win32 message time (ms since boot) to an SDL timestamp. Both are
 * monotonic, so a difference of two GetMessageTime() values is the same
 * interval as the QPC clock; anchor it at the current time. */
static Uint64 timestamp_from_msg(void)
{
    /* Messages are handled immediately; a small skew is invisible and keeps
     * this from needing a boot-time epoch. */
    return SDLOP_Win32Timestamp();
}

/* ------------------------------------------------------------------ */
/* Key translation                                                     */
/* ------------------------------------------------------------------ */

/* Win32 virtual-key + scancode -> SDL. Windows scancodes are the same set
 * SDL exposes as SDL_Scancode values for the main keyboard block, which is
 * why SDL3 can use them almost directly: the differences are the extended
 * keys and the multimedia/pause family. */
/* Resolve a WM_KEY* message to an SDL_Scancode plus the scan code to pack
 * into the rawcode. The translation itself lives in SDL_win32_keymap.c so the
 * scancode table sits with the rest of the Windows key mapping and can be
 * tested headless. */
static SDL_Scancode scancode_from_msg(const MSG *msg, Uint16 *rawcode)
{
    const Uint16 keyflags = HIWORD(msg->lParam);
    Uint16 scan_code = LOBYTE(keyflags);

    /* The On-Screen Keyboard can send the break code, with bit 7 set. */
    scan_code &= (Uint16)~0x80;

    if (scan_code != 0) {
        if ((keyflags & KF_EXTENDED) == KF_EXTENDED) {
            scan_code = MAKEWORD(scan_code, 0xe0);
        } else if (scan_code == 0x45) {
            /* Pause reports 0x45; its real code is E0 46 */
            scan_code = 0xe046;
        }
    } else {
        /* Some keys (multimedia, etc.) arrive without a scan code. */
        scan_code = LOWORD(MapVirtualKeyW((UINT)LOWORD(msg->wParam), MAPVK_VK_TO_VSC_EX));
        if (scan_code == 0xe11d) {
            /* Pause reports E1 1D; use the Win32 Pause code */
            scan_code = 0xe046;
        }
    }

    if (rawcode) {
        *rawcode = scan_code;
    }
    return SDLOP_Win32ScancodeFromScan(scan_code);
}


/* Translate the character a WM_CHAR/WM_UNICHAR produced. WM_CHAR gives UTF-16
 * code units; WM_UNICHAR gives a code point. */
static void send_text_from_msg(const MSG *msg)
{
    wchar_t utf16[2];
    char utf8[8];
    if (msg->message == WM_UNICHAR) {
        const Uint32 cp = (Uint32)msg->wParam;
        if (cp == UNICODE_NOCHAR) {
            return;
        }
        if (cp < 0xD800 || cp > 0xDFFF) {
            /* encode by hand: no SDL_iconv dependency in the driver */
            if (cp < 0x80) {
                utf8[0] = (char)cp;
                utf8[1] = 0;
                SDLOP_SendKeyboardText(utf8, SDLOP_Win32Timestamp());
            } else if (cp < 0x800) {
                utf8[0] = (char)(0xC0 | (cp >> 6));
                utf8[1] = (char)(0x80 | (cp & 0x3F));
                utf8[2] = 0;
                SDLOP_SendKeyboardText(utf8, SDLOP_Win32Timestamp());
            } else if (cp < 0x10000) {
                utf8[0] = (char)(0xE0 | (cp >> 12));
                utf8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                utf8[2] = (char)(0x80 | (cp & 0x3F));
                utf8[3] = 0;
                SDLOP_SendKeyboardText(utf8, SDLOP_Win32Timestamp());
            } else {
                utf8[0] = (char)(0xF0 | (cp >> 18));
                utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                utf8[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                utf8[3] = (char)(0x80 | (cp & 0x3F));
                utf8[4] = 0;
                SDLOP_SendKeyboardText(utf8, SDLOP_Win32Timestamp());
            }
        }
        return;
    }
    /* WM_CHAR: a UTF-16 code unit; surrogate pairs arrive as two messages */
    const Uint32 ch = (Uint32)msg->wParam;
    if (ch >= 0xD800 && ch <= 0xDBFF) {
        /* high surrogate: combine with the next WM_CHAR */
        const MSG *next = NULL;
        MSG peek;
        if (PeekMessageW(&peek, msg->hwnd, WM_CHAR, WM_CHAR, PM_NOREMOVE) && peek.wParam >= 0xDC00 && peek.wParam <= 0xDFFF) {
            next = &peek;
        }
        if (!next) {
            return; /* lone surrogate: drop */
        }
        GetMessageW(&peek, msg->hwnd, WM_CHAR, WM_CHAR); /* consume it */
        utf16[0] = (wchar_t)ch;
        utf16[1] = (wchar_t)peek.wParam;
        const int n = WideCharToMultiByte(CP_UTF8, 0, utf16, 2, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
        if (n > 0) {
            utf8[n] = 0;
            SDLOP_SendKeyboardText(utf8, SDLOP_Win32Timestamp());
        }
        return;
    }
    if (ch >= 0xDC00 && ch <= 0xDFFF) {
        return; /* lone low surrogate */
    }
    utf16[0] = (wchar_t)ch;
    const int n = WideCharToMultiByte(CP_UTF8, 0, utf16, 1, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
    if (n > 0) {
        utf8[n] = 0;
        SDLOP_SendKeyboardText(utf8, SDLOP_Win32Timestamp());
    }
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                    */
/* ------------------------------------------------------------------ */

static void update_metrics(SDL_Window *window)
{
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return;
    }
    RECT rc;
    if (GetClientRect(data->hwnd, &rc)) {
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w != window->w || h != window->h) {
            window->w = w;
            window->h = h;
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, w, h);
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, w, h);
        }
    }
    POINT pt = { 0, 0 };
    if (ClientToScreen(data->hwnd, &pt)) {
        window->x = pt.x;
        window->y = pt.y;
    }
}

/* Drop the cursor clip, but only if we are the ones who set it: another
 * window (or another application) may own it right now. */
static void release_cursor_clip(SDLop_Win32WindowData *data)
{
    RECT current;
    if (GetClipCursor(&current) && EqualRect(&current, &data->cursor_clipped_rect)) {
        ClipCursor(NULL);
        SDL_zero(data->cursor_clipped_rect);
    }
}

/* Put the clip back after focus returns, for whichever mode wants one. */
static void restore_cursor_clip(SDL_Window *window)
{
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return;
    }
    if (window->flags & SDL_WINDOW_MOUSE_RELATIVE_MODE) {
        RECT rc;
        GetClientRect(data->hwnd, &rc);
        MapWindowPoints(data->hwnd, NULL, (POINT *)&rc, 2);
        ClipCursor(&rc);
        SetCapture(data->hwnd);
    } else if (window->flags & SDL_WINDOW_MOUSE_GRABBED) {
        RECT rc;
        GetWindowRect(data->hwnd, &rc);
        ClipCursor(&rc);
        SetCapture(data->hwnd);
    } else if (window->mouse_rect.w > 0 && window->mouse_rect.h > 0) {
        RECT clip;
        clip.left = window->mouse_rect.x;
        clip.top = window->mouse_rect.y;
        clip.right = clip.left + window->mouse_rect.w;
        clip.bottom = clip.top + window->mouse_rect.h;
        MapWindowPoints(data->hwnd, NULL, (POINT *)&clip, 2);
        ClipCursor(&clip);
    }
}

static void set_focus(SDL_Window *window, bool focused)
{
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (focused) {
        if (sdlop.keyboard_focus != window) {
            sdlop.keyboard_focus = window;
            window->flags |= SDL_WINDOW_INPUT_FOCUS;
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_FOCUS_GAINED, 0, 0);
            /* SDL3 re-confines on activation: Alt-Tab back into a grabbed or
             * relative-mode game and the cursor is captured again. */
            restore_cursor_clip(window);
        }
    } else if (sdlop.keyboard_focus == window) {
        sdlop.keyboard_focus = NULL;
        window->flags &= ~SDL_WINDOW_INPUT_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
        /* Never leave the pointer trapped in an inactive window (SDL3 does
         * the same in WIN_UpdateFocus). */
        if (data) {
            release_cursor_clip(data);
        }
    }
}

static void set_mouse_focus(SDL_Window *window, bool focused)
{
    if (focused) {
        if (sdlop.mouse_focus != window) {
            sdlop.mouse_focus = window;
            window->flags |= SDL_WINDOW_MOUSE_FOCUS;
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_ENTER, 0, 0);
        }
    } else if (sdlop.mouse_focus == window) {
        sdlop.mouse_focus = NULL;
        window->flags &= ~SDL_WINDOW_MOUSE_FOCUS;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOUSE_LEAVE, 0, 0);
    }
}

/* SDL button numbering: 1 = left, 2 = middle, 3 = right (SDL3 order) */
static Uint8 mouse_button_from_msg(const MSG *msg)
{
    switch (msg->message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        return SDL_BUTTON_LEFT;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        return SDL_BUTTON_MIDDLE;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
        return SDL_BUTTON_RIGHT;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        return (HIWORD(msg->wParam) == XBUTTON1) ? SDL_BUTTON_X1 : SDL_BUTTON_X2;
    default:
        return 0;
    }
}

static bool is_button_down_msg(const MSG *msg)
{
    switch (msg->message) {
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_XBUTTONDOWN:
        return true;
    default:
        return false;
    }
}

static void send_mouse_button(const MSG *msg)
{
    const Uint8 button = mouse_button_from_msg(msg);
    if (!button) {
        return;
    }
    const float x = (float)GET_X_LPARAM(msg->lParam);
    const float y = (float)GET_Y_LPARAM(msg->lParam);
    SDLOP_SendMouseButton(is_button_down_msg(msg), button, x, y, timestamp_from_msg());
}

static void send_mouse_motion(const MSG *msg, bool relative)
{
    if (relative) {
        RAWMOUSE *raw = NULL;
        /* WM_INPUT carries the raw packet in lParam */
        UINT size = 0;
        GetRawInputData((HRAWINPUT)msg->lParam, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
        BYTE stackbuf[256];
        BYTE *buf = size <= sizeof(stackbuf) ? stackbuf : (BYTE *)SDL_malloc(size);
        if (buf && GetRawInputData((HRAWINPUT)msg->lParam, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
            raw = &((RAWINPUT *)buf)->data.mouse;
            if ((raw->usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && (raw->lLastX || raw->lLastY)) {
                const float xrel = (float)raw->lLastX;
                const float yrel = (float)raw->lLastY;
                SDLOP_SendMouseMotion(SDLOP_NO_POS, SDLOP_NO_POS, xrel, yrel, timestamp_from_msg());
            }
        }
        if (buf != stackbuf) {
            SDL_free(buf);
        }
        return;
    }
    SDLOP_SendMouseMotion((float)GET_X_LPARAM(msg->lParam), (float)GET_Y_LPARAM(msg->lParam),
                          0.0f, 0.0f, timestamp_from_msg());
}

static void handle_size(SDL_Window *window, WPARAM wparam)
{
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    SDLOP_MUTEX_LOCK(&data->surfacelock);
    update_metrics(window);
    /* the DIB belongs to the old size; SDL3 recreates it lazily */
    if (data->surface) {
        data->surface_dirty = true;
    }
    SDLOP_MUTEX_UNLOCK(&data->surfacelock);
    if (wparam == SIZE_MAXIMIZED && !(window->flags & SDL_WINDOW_MAXIMIZED)) {
        window->flags |= SDL_WINDOW_MAXIMIZED;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MAXIMIZED, 0, 0);
    } else if (wparam == SIZE_MINIMIZED) {
        if (!(window->flags & SDL_WINDOW_MINIMIZED)) {
            window->flags |= SDL_WINDOW_MINIMIZED;
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MINIMIZED, 0, 0);
        }
    } else if (wparam == SIZE_RESTORED) {
        if (window->flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) {
            window->flags &= ~(SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED);
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_RESTORED, 0, 0);
        }
    }
}

/* SDL_SetWindowAspectRatio() has no Win32 equivalent: WM_GETMINMAXINFO can
 * only express min/max tracking sizes, not a ratio, so the constraint is
 * applied while the user drags, in WM_SIZING. Only the client area counts,
 * so the frame is taken back out before the ratio is measured. */
static void win32_constrain_aspect(SDL_Window *window, RECT *rect)
{
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return;
    }
    if (window->min_aspect <= 0.0f && window->max_aspect <= 0.0f) {
        return;
    }

    /* How much frame this window adds around a client area. AdjustWindowRectEx
     * on an empty rect returns the frame as negative left/top offsets. */
    RECT zero = { 0, 0, 0, 0 };
    SDLOP_Win32AdjustWindowRect(window, &zero);
    const int border_w = -zero.left + zero.right;
    const int border_h = -zero.top + zero.bottom;
    int cw = (rect->right - rect->left) - border_w;
    int ch = (rect->bottom - rect->top) - border_h;
    if (cw <= 0 || ch <= 0) {
        return;
    }

    const float aspect = (float)cw / (float)ch;
    if (window->min_aspect > 0.0f && aspect < window->min_aspect) {
        ch = (int)SDL_lroundf((float)cw / window->min_aspect);
    } else if (window->max_aspect > 0.0f && aspect > window->max_aspect) {
        cw = (int)SDL_lroundf((float)ch * window->max_aspect);
    } else {
        return;
    }

    /* the frame that client area needs, keeping the top-left corner */
    RECT want = { 0, 0, cw, ch };
    SDLOP_Win32AdjustWindowRect(window, &want);
    rect->right = rect->left + (want.right - want.left);
    rect->bottom = rect->top + (want.bottom - want.top);
}

/* SDL_SetWindowHitTest(): called from WM_NCHITTEST for every point the window
 * manager asks about. Returns false when the application has no opinion, so
 * the caller falls through to DefWindowProc (the frame's own hit testing). */
static bool win32_run_hit_test(SDL_Window *window, LPARAM lparam, LRESULT *result)
{
    SDL_Point point;
    POINT screen_point;
    SDL_HitTestResult rc;

    if (!window || !window->hit_test) {
        return false;
    }
    screen_point.x = GET_X_LPARAM(lparam);
    screen_point.y = GET_Y_LPARAM(lparam);
    if (!ScreenToClient(((SDLop_Win32WindowData *)window->driverdata)->hwnd, &screen_point)) {
        return false;
    }
    point.x = screen_point.x;
    point.y = screen_point.y;
    rc = window->hit_test(window, &point, window->hit_test_data);
    if (rc == SDL_HITTEST_NORMAL) {
        return false; /* nothing special: let DefWindowProc answer */
    }

    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_HIT_TEST, 0, 0);

    switch (rc) {
    case SDL_HITTEST_DRAGGABLE:
        /* If a button other than the left one is held, hand the point to the
         * client area or Windows swallows the press (SDL3 does the same). */
        {
            const bool any_pressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                                     (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                                     (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0 ||
                                     (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0 ||
                                     (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
            const bool left_pressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            /* swap left/right when the user asked Windows to */
            const bool left = GetSystemMetrics(SM_SWAPBUTTON) ? !left_pressed : left_pressed;
            if (any_pressed && !left) {
                *result = HTCLIENT;
                return true;
            }
        }
        *result = HTCAPTION;
        return true;
    case SDL_HITTEST_RESIZE_TOPLEFT: *result = HTTOPLEFT; return true;
    case SDL_HITTEST_RESIZE_TOP: *result = HTTOP; return true;
    case SDL_HITTEST_RESIZE_TOPRIGHT: *result = HTTOPRIGHT; return true;
    case SDL_HITTEST_RESIZE_RIGHT: *result = HTRIGHT; return true;
    case SDL_HITTEST_RESIZE_BOTTOMRIGHT: *result = HTBOTTOMRIGHT; return true;
    case SDL_HITTEST_RESIZE_BOTTOM: *result = HTBOTTOM; return true;
    case SDL_HITTEST_RESIZE_BOTTOMLEFT: *result = HTBOTTOMLEFT; return true;
    case SDL_HITTEST_RESIZE_LEFT: *result = HTLEFT; return true;
    default: return false;
    }
}

/* ------------------------------------------------------------------ */
/* Raw keyboard input (SDL_HINT_WINDOWS_RAW_KEYBOARD)                  */
/*                                                                     */
/* Win32 delivers raw keyboard packets to the window with keyboard     */
/* focus, as RIM_TYPEKEYBOARD WM_INPUT messages carrying the hardware  */
/* make code and the E0/E1 prefix bits. That is the only way to tell   */
/* left and right Shift/Ctrl/Alt apart by hardware scan code, and it   */
/* is what SDL3 registers when the hint is on.                         */
/*                                                                     */
/* Registration is process-global: the hwndTarget is NULL, so packets */
/* follow keyboard focus rather than one window. RIDEV_REMOVE is the   */
/* documented way to deregister, and it is only issued when something  */
/* is actually registered -- removing a registration that does not     */
/* exist is not harmless.                                              */
/* ------------------------------------------------------------------ */

static void win32_handle_raw_keyboard(const RAWKEYBOARD *raw, Uint64 ts);

/* Does the raw path own keyboard events right now?
 *
 * Both raw keyboard paths (SDL_HINT_WINDOWS_RAW_KEYBOARD and the raw input
 * worker) publish keys, and WM_KEYDOWN publishes the same keys again. Exactly
 * one of the two may deliver, or every release arrives twice -- measured:
 * "UP Left Shift UP Left Shift" before this predicate existed.
 *
 * While text input is active the IME owns the keyboard: the raw path stands
 * down for presses, so WM_KEY* is the one that must deliver. Both places ask
 * this function so they cannot disagree -- an earlier version had the two
 * conditions written separately and keys disappeared entirely.
 *
 * (Defined after the driver's own window_proc needs it, so it lives here as a
 * forward-declared helper next to the raw keyboard code.) */
static bool win32_raw_keyboard_owns_keys(void)
{
    if (!(SDLOP_RawInputIsSource() || sdlop_win32.raw_keyboard_registered)) {
        return false;
    }
    if (sdlop.keyboard_focus && sdlop.keyboard_focus->text_input_active) {
        return false;
    }
    return true;
}

/* RIDEV_INPUTSINK requires a real hwndTarget -- the API rejects NULL with
 * ERROR_INVALID_PARAMETER, for the mouse as well as the keyboard. Background
 * input therefore needs a window of its own, and a message-only window is the
 * right one: it never shows, never takes focus. */
static LRESULT CALLBACK win32_raw_input_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_INPUT) {
        RAWINPUT packet;
        UINT size = sizeof(packet);
        if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &packet, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            if (packet.header.dwType == RIM_TYPEKEYBOARD) {
                win32_handle_raw_keyboard(&packet.data.keyboard, SDLOP_Win32Timestamp());
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static HWND win32_raw_input_window(void)
{
    HWND hwnd;
    WNDCLASSW wc;

    if (sdlop_win32.raw_input_hwnd) {
        return sdlop_win32.raw_input_hwnd;
    }
    SDL_zero(wc);
    wc.lpfnWndProc = win32_raw_input_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SDLopRawInput";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return NULL;
    }
    hwnd = CreateWindowExW(0, L"SDLopRawInput", NULL, 0, 0, 0, 0, 0,
                           HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (hwnd) {
        sdlop_win32.raw_input_hwnd = hwnd;
    }
    return hwnd;
}

static void win32_destroy_raw_input_window(void)
{
    if (sdlop_win32.raw_input_hwnd) {
        DestroyWindow(sdlop_win32.raw_input_hwnd);
        sdlop_win32.raw_input_hwnd = NULL;
        UnregisterClassW(L"SDLopRawInput", GetModuleHandleW(NULL));
    }
}

/* The raw input worker (src/internal/sdlop_rawinput_win32.c) registers the
 * same Generic Desktop / Keyboard usage and all of its packets already reach
 * the raw rings and callbacks. Windows keeps exactly one registration per
 * usage, so the two features cannot both own it:
 *
 *   - worker on  -> the worker owns the registration; the hint-driven
 *                   keyboard events would be a duplicate source, so they stop
 *   - worker off -> the hint-driven registration is restored as the app asked
 *
 * Measured before this rule existed: registering the worker's sink silently
 * replaced the hint's registration, and a single later RIDEV_REMOVE then
 * wiped both, leaving no keyboard raw input at all. */
static void win32_register_raw_keyboard(void)
{

    RAWINPUTDEVICE rid;
    DWORD flags = 0;
    HWND target = NULL;

    if (SDLOP_RawInputIsSource()) {
        /* The worker owns the registration; remember what the hint wanted so
         * it can be reinstated when the worker stops. */
        if (sdlop_win32.raw_keyboard_registered) {
            SDL_zero(rid);
            rid.usUsagePage = 0x01;
            rid.usUsage = 0x06;
            rid.dwFlags = RIDEV_REMOVE;
            rid.hwndTarget = NULL; /* RIDEV_REMOVE requires NULL */
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
            sdlop_win32.prev_keyboard_registered = true;
            sdlop_win32.raw_keyboard_registered = false;
            sdlop_win32.raw_keyboard_target = NULL;
        }
        return;
    }
    if (sdlop_win32.prev_keyboard_registered) {
        /* The worker has stopped: put the hint's registration back. */
        sdlop_win32.prev_keyboard_registered = false;
    }

    if (!sdlop_win32.raw_keyboard_enabled) {
        if (sdlop_win32.raw_keyboard_registered) {
            SDL_zero(rid);
            rid.usUsagePage = 0x01; /* generic desktop */
            rid.usUsage = 0x06;     /* keyboard */
            /* RIDEV_REMOVE requires hwndTarget == NULL: the device is
             * identified by usage page and usage alone. Passing the target
             * back fails with ERROR_INVALID_PARAMETER and leaves the
             * registration in place. */
            rid.dwFlags = RIDEV_REMOVE;
            rid.hwndTarget = NULL;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
            sdlop_win32.raw_keyboard_registered = false;
            sdlop_win32.raw_keyboard_target = NULL;
        }
        /* The sink window exists only to receive background input. */
        win32_destroy_raw_input_window();
        sdlop_win32.pending_e1_sequence = false;
        return;
    }

    if (sdlop_win32.raw_keyboard_nohotkeys) {
        flags |= RIDEV_NOHOTKEYS;
    }
    if (sdlop_win32.raw_keyboard_inputsink) {
        /* NULL is not allowed with INPUTSINK, so background delivery goes to
         * the message-only window instead of following keyboard focus. */
        flags |= RIDEV_INPUTSINK;
        target = win32_raw_input_window();
        if (!target) {
            return; /* cannot deliver in the background; stay unregistered */
        }
    }

    /* Nothing to do if the same registration is already in place. Without
     * this, re-setting an unrelated hint would drop the OS device list. */
    if (sdlop_win32.raw_keyboard_registered && flags == sdlop_win32.raw_keyboard_flags &&
        target == sdlop_win32.raw_keyboard_target) {
        return;
    }

    /* Deregister the old target first: the flags are read from the batch, and
     * re-adding the same usage under a different target would otherwise leave
     * both registrations alive. */
    if (sdlop_win32.raw_keyboard_registered) {
        SDL_zero(rid);
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x06;
        rid.dwFlags = RIDEV_REMOVE;
        rid.hwndTarget = NULL; /* required by RIDEV_REMOVE */
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        sdlop_win32.raw_keyboard_registered = false;
        sdlop_win32.raw_keyboard_target = NULL;
    }

    SDL_zero(rid);
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x06;
    rid.dwFlags = flags;
    rid.hwndTarget = target;
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        sdlop_win32.raw_keyboard_registered = true;
        sdlop_win32.raw_keyboard_flags = flags;
        sdlop_win32.raw_keyboard_target = target;
    } else {
        /* Registration failed: do not keep a sink window around for it. */
        sdlop_win32.raw_keyboard_registered = false;
        sdlop_win32.raw_keyboard_target = NULL;
        win32_destroy_raw_input_window();
    }
}

static void win32_update_raw_keyboard_flags(void)
{
    win32_register_raw_keyboard();
}

static void SDLCALL win32_raw_keyboard_hint_changed(void *userdata, const char *name, const char *old_value, const char *new_value)
{
    (void)userdata;
    (void)name;
    (void)old_value;
    sdlop_win32.raw_keyboard_enabled = SDLOP_GetStringBoolean(new_value, false);
    win32_update_raw_keyboard_flags();
}

static void SDLCALL win32_raw_keyboard_nohotkeys_hint_changed(void *userdata, const char *name, const char *old_value, const char *new_value)
{
    (void)userdata;
    (void)name;
    (void)old_value;
    sdlop_win32.raw_keyboard_nohotkeys = SDLOP_GetStringBoolean(new_value, false);
    win32_update_raw_keyboard_flags();
}

static void SDLCALL win32_raw_keyboard_inputsink_hint_changed(void *userdata, const char *name, const char *old_value, const char *new_value)
{
    (void)userdata;
    (void)name;
    (void)old_value;
    sdlop_win32.raw_keyboard_inputsink = SDLOP_GetStringBoolean(new_value, false);
    win32_update_raw_keyboard_flags();
}

/* One RIM_TYPEKEYBOARD packet. Mirrors WIN_HandleRawKeyboardInput(). */
static void win32_handle_raw_keyboard(const RAWKEYBOARD *raw, Uint64 ts)
{
    USHORT rawcode;
    SDL_Scancode code;
    bool down;

    if (!sdlop_win32.raw_keyboard_enabled) {
        return;
    }

    if (raw->Flags & RI_KEY_E1) {
        /* First key of a Ctrl+{key} sequence: remember it and wait. */
        sdlop_win32.pending_e1_sequence = true;
        return;
    }
    if ((raw->Flags & RI_KEY_E0) && raw->MakeCode == 0x2A) {
        /* 0xE02A make-code prefix carries no key of its own. */
        return;
    }

    {
        USHORT make = raw->MakeCode;
        if (!make && raw->VKey) {
            make = LOWORD(MapVirtualKeyW(raw->VKey, MAPVK_VK_TO_VSC_EX));
        }
        if (!make) {
            return;
        }
        rawcode = make;
    }

    switch (raw->Message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        down = true;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        down = false;
        break;
    default:
        down = (raw->Flags & RI_KEY_BREAK) == 0;
        break;
    }

    if (sdlop_win32.pending_e1_sequence) {
        sdlop_win32.pending_e1_sequence = false;
        if ((rawcode & 0xFF) == 0x45) {
            code = SDL_SCANCODE_PAUSE; /* Ctrl+NumLock */
        } else {
            code = SDL_SCANCODE_UNKNOWN; /* Ctrl+ScrollLock == Break */
        }
    } else {
        /* SDLOP_Win32ScancodeFromScan() indexes two 128-entry banks, with bit
         * 7 selecting the extended one. This is NOT "0xE000 | make": the
         * table is indexed by the low byte plus the E0 prefix bit. */
        code = SDLOP_Win32ScancodeFromScan((Uint16)((raw->Flags & RI_KEY_E0) ? (rawcode | 0xE000) : rawcode));
    }

    if (!win32_raw_keyboard_owns_keys()) {
        return; /* WM_KEY* delivers these instead */
    }
    if (down) {
        SDL_Window *focus = sdlop.keyboard_focus;
        /* With RIDEV_INPUTSINK the packets arrive unfocused on purpose; without
         * it, only deliver while one of our windows has the keyboard. */
        if (!sdlop_win32.raw_keyboard_inputsink && !focus) {
            return;
        }
    }

    if (code == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    SDLOP_SendKeyboardKey(down, false, code, (Uint16)SDLOP_WIN32_RAWCODE(raw->VKey, rawcode & 0xFF), ts);
}

/* Keep SDL_GetWindowDisplayScale() honest: the core reads window->display_scale
 * and the other backends set it from their compositor's scale. Win32 gives us
 * the window's own DPI (it follows the monitor the window is on), so the scale
 * is dpi/96 -- which is what SDL3 ends up at through pixel density. */
static void win32_update_display_scale(SDL_Window *window, Uint32 dpi)
{
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !dpi) {
        return;
    }
    data->dpi = dpi;
    const float scale = (float)dpi / (float)USER_DEFAULT_SCREEN_DPI;
    if (window->display_scale != scale) {
        window->display_scale = scale;
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED, 0, 0);
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    SDL_Window *window = window_from_hwnd(hwnd);
    MSG m;
    m.hwnd = hwnd;
    m.message = msg;
    m.wParam = wparam;
    m.lParam = lparam;
    m.time = GetMessageTime();

    switch (msg) {
    case WM_CLOSE:
        if (window) {
            SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
        }
        return 0;

    case WM_DESTROY:
        /* Only drop the focus this window actually holds: a child destroyed
         * through its parent raises WM_DESTROY too, and clearing blindly would
         * take the focus away from an unrelated window. */
        if (window) {
            if (sdlop.keyboard_focus == window) {
                sdlop.keyboard_focus = NULL;
            }
            if (sdlop.mouse_focus == window) {
                sdlop.mouse_focus = NULL;
            }
        }
        SetPropW(hwnd, L"SDLopWindow", NULL);
        return 0;

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (window) {
            Uint16 scan_code = 0;
            const SDL_Scancode sc = scancode_from_msg(&m, &scan_code);
            const bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            /* The same physical key arrives here *and* as a raw packet when
             * either raw keyboard path is on (SDL_HINT_WINDOWS_RAW_KEYBOARD,
             * or the raw input worker). Delivering both sent every release
             * twice -- measured: "UP Left Shift UP Left Shift". The raw path
             * owns the keys in that case, as it does for the mouse. */
            if (win32_raw_keyboard_owns_keys()) {
                /* System keys still need DefWindowProc (menu activation,
                 * Alt+F4), which the fall-through below handles. */
                if (msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP) {
                    return 0;
                }
                break;
            }
            if (sc != SDL_SCANCODE_UNKNOWN) {
                /* the rawcode packs the virtual-key code (high byte) with the
                 * hardware scancode (low byte); the core needs the VK to
                 * translate the key. The scan code comes from the same helper
                 * so an injected key without one still resolves. */
                const Uint16 raw = SDLOP_WIN32_RAWCODE(wparam, scan_code & 0xFF);
                const Uint64 ts = SDLOP_Win32Timestamp();
                const bool repeat = (lparam & (1 << 30)) != 0;
                SDLOP_SendKeyboardKey(down, repeat, sc, raw, ts);
            }
        }
        /* System keys still need DefWindowProc (menu activation, Alt+F4) */
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) {
            break;
        }
        return 0;

    case WM_CHAR:
    case WM_UNICHAR:
        if (window) {
            send_text_from_msg(&m);
            return 0;
        }
        return 0;

    case WM_MOUSEMOVE:
        if (window) {
            if (!(window->flags & SDL_WINDOW_MOUSE_FOCUS)) {
                set_mouse_focus(window, true);
            }
            /* With the raw input worker running it is the single source of
             * pointer events (SDLOP_RAW_INPUT=1); letting this through too
             * would deliver every movement twice. SDLop takes the same
             * position on Linux: either the raw path supplies input or the
             * window system does, never both. */
            if (!SDLOP_RawInputIsSource()) {
                send_mouse_motion(&m, false);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        if (window) {
            SetFocus(hwnd);
            if (!SDLOP_RawInputIsSource()) {
                send_mouse_button(&m);
            }
            if (msg == WM_XBUTTONUP || msg == WM_XBUTTONDOWN || msg == WM_XBUTTONDBLCLK) {
                return TRUE; /* XBUTTON results must be returned */
            }
        }
        return 0;

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        /* same single-source rule as motion and buttons */
        if (window && !SDLOP_RawInputIsSource()) {
            const short delta = GET_WHEEL_DELTA_WPARAM(wparam);
            /* SDL units are "lines" (+away from the user); one notch is
             * WHEEL_DELTA and maps to 1.0 */
            const float amount = (float)delta / (float)WHEEL_DELTA;
            if (msg == WM_MOUSEWHEEL) {
                SDLOP_SendMouseWheel(0.0f, amount, timestamp_from_msg());
            } else {
                SDLOP_SendMouseWheel(-amount, 0.0f, timestamp_from_msg());
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (window) {
            set_mouse_focus(window, false);
        }
        return 0;

    case WM_SETFOCUS:
        if (window) {
            set_focus(window, true);
        }
        return 0;
    case WM_KILLFOCUS:
        if (window) {
            set_focus(window, false);
        }
        return 0;

    case WM_SIZE:
        if (window) {
            handle_size(window, wparam);
        }
        return 0;

    case WM_MOVE:
        if (window) {
            const int x = (int)(short)LOWORD(lparam);
            const int y = (int)(short)HIWORD(lparam);
            if (x != window->x || y != window->y) {
                window->x = x;
                window->y = y;
                SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOVED, x, y);
            }
        }
        return 0;

    case WM_GETMINMAXINFO: {
        SDLop_Win32WindowData *data = window ? (SDLop_Win32WindowData *)window->driverdata : NULL;
        if (data && (data->has_minimum_size || data->has_maximum_size)) {
            MINMAXINFO *mmi = (MINMAXINFO *)lparam;
            RECT rc = { 0, 0, 0, 0 };
            if (data->has_minimum_size) {
                rc.right = data->minimum_w;
                rc.bottom = data->minimum_h;
                SDLOP_Win32AdjustWindowRect(window, &rc);
                mmi->ptMinTrackSize.x = rc.right - rc.left;
                mmi->ptMinTrackSize.y = rc.bottom - rc.top;
            }
            if (data->has_maximum_size) {
                rc.right = data->maximum_w;
                rc.bottom = data->maximum_h;
                SDLOP_Win32AdjustWindowRect(window, &rc);
                mmi->ptMaxTrackSize.x = rc.right - rc.left;
                mmi->ptMaxTrackSize.y = rc.bottom - rc.top;
            }
            return 0;
        }
        break;
    }

    case WM_NCHITTEST:
        /* tooltips must stay click-through, before any application hit test */
        if (window && (window->flags & SDL_WINDOW_TOOLTIP)) {
            return HTTRANSPARENT;
        }
        {
            LRESULT hit;
            if (win32_run_hit_test(window, lparam, &hit)) {
                return hit;
            }
        }
        break;

    case WM_SIZING:
        if (window) {
            win32_constrain_aspect(window, (RECT *)lparam);
            return TRUE;
        }
        break;

    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_KEYMENU && window) {
            /* Alt press alone: swallow, or the app loses focus to the menu */
            return 0;
        }
        if ((wparam & 0xFFF0) == SC_CLOSE && window) {
            /* handled by WM_CLOSE */
        }
        break;

    case WM_ERASEBKGND:
        /* the software path paints every pixel; erasing just flickers */
        if (window) {
            SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
            if (data && data->surface) {
                return 1;
            }
            const Uint8 r = window->clear_r, g = window->clear_g, b = window->clear_b;
            HDC hdc = (HDC)wparam;
            RECT rc;
            if (GetClientRect(hwnd, &rc)) {
                HBRUSH brush = CreateSolidBrush(RGB(r, g, b));
                if (brush) {
                    FillRect(hdc, &rc, brush);
                    DeleteObject(brush);
                }
                return 1;
            }
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            SDL_Cursor *cursor = SDLOP_GetCurrentCursor();
            if (cursor && cursor->internal) {
                SetCursor((HCURSOR)cursor->internal);
            } else {
                SetCursor(NULL); /* hidden, or a cursor with no image */
            }
            return TRUE;
        }
        break;

    case WM_DISPLAYCHANGE:
        if (window) {
            update_metrics(window);
        }
        return 0;

    case WM_DPICHANGED:
        if (window) {
            const UINT dpi = HIWORD(wparam);
            const RECT *suggest = (const RECT *)lparam;
            SetWindowPos(hwnd, NULL, suggest->left, suggest->top,
                         suggest->right - suggest->left, suggest->bottom - suggest->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            win32_update_display_scale(window, dpi);
        }
        return 0;

    case WM_INPUT:
    {
        /* One WM_INPUT carries one packet; its header says which kind. The
         * mouse path only matters in relative mode, but keyboard packets must
         * be answered whenever the hint turned them on. */
        RAWINPUT packet;
        UINT size = sizeof(packet);
        /* The worker may have stopped since the last pump (SDL_SetHint or a
         * window teardown). Restore whatever registration the app asked for
         * before handling packets, so raw keyboard input is never silently
         * lost when the two features hand the registration over. */
        if (sdlop_win32.prev_keyboard_registered && !SDLOP_RawInputIsSource()) {
            win32_register_raw_keyboard();
        }
        if (GetRawInputData((HRAWINPUT)m.lParam, RID_INPUT, &packet, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            if (packet.header.dwType == RIM_TYPEKEYBOARD) {
                win32_handle_raw_keyboard(&packet.data.keyboard, SDLOP_Win32Timestamp());
                return 0;
            }
            if (packet.header.dwType == RIM_TYPEMOUSE && window && sdlop_win32.want_relative_mouse) {
                send_mouse_motion(&m, true);
                return 0;
            }
        }
        break;
    }

    default:
        if (msg == WM_TASKBARBUTTONCREATED) {
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* ------------------------------------------------------------------ */
/* Driver init / quit                                                  */
/* ------------------------------------------------------------------ */

static bool win32_register_class(void)
{
    if (sdlop_win32.class_registered) {
        return true;
    }
    WNDCLASSEXW wc;
    SDL_zero(wc);
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; /* WM_ERASEBKGND paints the clear colour */
    wc.lpszClassName = SDLOP_WIN32_CLASS;
    if (!RegisterClassExW(&wc)) {
        return SDL_SetError("RegisterClassExW() failed: %lu", (unsigned long)GetLastError());
    }
    sdlop_win32.class_registered = true;
    return true;
}

/* SDL3 reports the desktop mode twice: the logical size (what the application
 * draws in) and the pixel size. On a scaled display they differ, and the ratio
 * is what SDL_GetWindowSizeInPixels() and SDL_GetWindowPixelDensity() use.
 * Win32 hands us both: dmPels* is the physical size, and the primary monitor's
 * logical size is the one GetSystemMetrics() reports with DPI awareness on. */
static UINT win32_system_dpi(void)
{
    const UINT dpi = GetDpiForSystem();
    return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

/* Pixel density for a monitor: physical pixels per logical unit. */
static float win32_monitor_pixel_density(void)
{
    return (float)win32_system_dpi() / (float)USER_DEFAULT_SCREEN_DPI;
}

static void win32_add_displays(SDLop_VideoDevice *device)
{
    (void)device;
    if (sdlop.num_displays > 0) {
        return;
    }
    /* One SDL display per monitor, in EnumDisplayMonitors order. */
    sdlop_win32.monitor_count = GetSystemMetrics(SM_CMONITORS);
    if (sdlop_win32.monitor_count <= 0) {
        sdlop_win32.monitor_count = 1;
    }
    sdlop_win32.monitors = (SDLop_Win32Monitor *)SDL_calloc((size_t)sdlop_win32.monitor_count, sizeof(SDLop_Win32Monitor));
    if (!sdlop_win32.monitors) {
        return;
    }
    /* primary first so displays[0] is the desktop mode */
    int idx = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < sdlop_win32.monitor_count && idx < sdlop_win32.monitor_count; i++) {
            DISPLAY_DEVICEW dd;
            SDL_zero(dd);
            dd.cb = sizeof(dd);
            if (!EnumDisplayDevicesW(NULL, (DWORD)i, &dd, 0)) {
                continue;
            }
            const bool primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
            if ((pass == 0) != primary) {
                continue;
            }
            DEVMODEW dm;
            SDL_zero(dm);
            dm.dmSize = sizeof(dm);
            if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                continue;
            }
            SDLop_Win32Monitor *mon = &sdlop_win32.monitors[idx];
            SDL_wcslcpy(mon->device, dd.DeviceName, SDL_arraysize(mon->device));
            mon->rect.left = dm.dmPosition.x;
            mon->rect.top = dm.dmPosition.y;
            mon->rect.right = dm.dmPosition.x + (LONG)dm.dmPelsWidth;
            mon->rect.bottom = dm.dmPosition.y + (LONG)dm.dmPelsHeight;
            mon->refresh_hz = dm.dmDisplayFrequency > 1 ? (int)dm.dmDisplayFrequency : 60;
            char name[128];
            SDL_snprintf(name, sizeof(name), "\\\\.\\DISPLAY%d", idx + 1);
            /* AddDisplay takes the logical (DPI-independent) size; the
             * driver's GetWindowSizeInPixels applies the density. */
            const float density = win32_monitor_pixel_density();
            const int logical_w = (int)SDL_lroundf((float)dm.dmPelsWidth / density);
            const int logical_h = (int)SDL_lroundf((float)dm.dmPelsHeight / density);
            SDL_DisplayID id = SDLOP_AddDisplay(name,
                                                logical_w, logical_h,
                                                mon->refresh_hz * 1000,
                                                dm.dmPelsWidth >= dm.dmPelsHeight ? SDL_ORIENTATION_LANDSCAPE : SDL_ORIENTATION_PORTRAIT,
                                                NULL, sdlop.init_done);
            SDLOP_SetDisplayPixelDensity(id, density);
            mon->displayID = id;
            idx++;
        }
    }
    sdlop_win32.monitor_count = idx;
}

/* Clipboard sequence number as of the last pump; see win32_check_clipboard_update. */
static DWORD win32_clipboard_sequence;

static bool win32_Init(SDLop_VideoDevice *device)
{
    SDL_zero(sdlop_win32);
    WM_TASKBARBUTTONCREATED = RegisterWindowMessageW(L"TaskbarButtonCreated");
    SetProcessDPIAware(); /* SDL3: DPI aware by default */
    if (!win32_register_class()) {
        return false;
    }
    win32_add_displays(device);
    sdlop_win32.window_count = 0;
    /* Baseline for external clipboard change detection: anything that changes
     * this number afterwards belongs to another application. */
    win32_clipboard_sequence = GetClipboardSequenceNumber();

    /* Raw keyboard input is opt-in, like SDL3: the hint defaults to off. The
     * callbacks keep it in sync if the application sets the hint later. */
    sdlop_win32.raw_keyboard_enabled = SDLOP_GetStringBoolean(SDL_GetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD), false);
    sdlop_win32.raw_keyboard_nohotkeys = SDLOP_GetStringBoolean(SDL_GetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD_EXCLUDE_HOTKEYS), false);
    sdlop_win32.raw_keyboard_inputsink = SDLOP_GetStringBoolean(SDL_GetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD_INPUTSINK), false);
    SDL_AddHintCallback(SDL_HINT_WINDOWS_RAW_KEYBOARD, win32_raw_keyboard_hint_changed, device);
    SDL_AddHintCallback(SDL_HINT_WINDOWS_RAW_KEYBOARD_EXCLUDE_HOTKEYS, win32_raw_keyboard_nohotkeys_hint_changed, device);
    SDL_AddHintCallback(SDL_HINT_WINDOWS_RAW_KEYBOARD_INPUTSINK, win32_raw_keyboard_inputsink_hint_changed, device);
    win32_register_raw_keyboard();
    return true;
}

static void win32_Quit(SDLop_VideoDevice *device)
{
    SDL_RemoveHintCallback(SDL_HINT_WINDOWS_RAW_KEYBOARD, win32_raw_keyboard_hint_changed, device);
    SDL_RemoveHintCallback(SDL_HINT_WINDOWS_RAW_KEYBOARD_EXCLUDE_HOTKEYS, win32_raw_keyboard_nohotkeys_hint_changed, device);
    SDL_RemoveHintCallback(SDL_HINT_WINDOWS_RAW_KEYBOARD_INPUTSINK, win32_raw_keyboard_inputsink_hint_changed, device);
    /* Leave no process-global registration behind for the next SDL_Init(). */
    sdlop_win32.raw_keyboard_enabled = false;
    win32_register_raw_keyboard();
    if (sdlop_win32.class_registered) {
        UnregisterClassW(SDLOP_WIN32_CLASS, GetModuleHandleW(NULL));
        sdlop_win32.class_registered = false;
    }
    SDL_free(sdlop_win32.monitors);
    sdlop_win32.monitors = NULL;
    sdlop_win32.monitor_count = 0;
    while (sdlop.num_displays > 0) {
        SDLOP_DelDisplay(sdlop.displays[sdlop.num_displays - 1].id, false);
    }
}

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

static DWORD window_style_for(SDL_Window *window)
{
    DWORD style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    if (window->flags & SDL_WINDOW_BORDERLESS) {
        style |= WS_POPUP;
    } else {
        style |= WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        if (window->flags & SDL_WINDOW_RESIZABLE) {
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        }
    }
    return style;
}

static DWORD window_exstyle_for(SDL_Window *window)
{
    DWORD exstyle = 0;
    if (window->flags & SDL_WINDOW_ALWAYS_ON_TOP) {
        exstyle |= WS_EX_TOPMOST;
    }
    if (window->flags & SDL_WINDOW_TOOLTIP) {
        exstyle |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    } else if (window->flags & SDL_WINDOW_UTILITY) {
        exstyle |= WS_EX_TOOLWINDOW;
    }
    return exstyle;
}

/* The title is UTF-8; Win32 wants UTF-16. */
static wchar_t *utf8_to_wide_alloc(const char *text)
{
    if (!text) {
        text = "";
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (len <= 0) {
        return NULL;
    }
    wchar_t *wide = (wchar_t *)SDL_malloc((size_t)len * sizeof(wchar_t));
    if (!wide) {
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, len);
    return wide;
}

static void win32_AdjustWindowSize(SDL_Window *window, RECT *rc)
{
    SDLOP_Win32AdjustWindowRect(window, rc);
}

static bool win32_CreateWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }
    SDLOP_MUTEX_INIT_ONCE(&data->surfacelock);
    data->dpi = (Uint32)GetDpiForWindow(data->hwnd);
    window->driverdata = data;

    RECT rc = { 0, 0, window->w, window->h };
    win32_AdjustWindowSize(window, &rc);

    const wchar_t *title_w = NULL;
    wchar_t *title_alloc = utf8_to_wide_alloc(window->title);
    title_w = title_alloc;

    DWORD style = window_style_for(window);
    DWORD exstyle = window_exstyle_for(window);
    if (window->flags & SDL_WINDOW_HIDDEN) {
        style &= ~WS_VISIBLE;
    }
    HWND hwnd = CreateWindowExW(exstyle, SDLOP_WIN32_CLASS, title_w ? title_w : L"",
                                style,
                                window->x, window->y,
                                rc.right - rc.left, rc.bottom - rc.top,
                                NULL, NULL, GetModuleHandleW(NULL), NULL);
    SDL_free(title_alloc);
    if (!hwnd) {
        SDLOP_MUTEX_DESTROY(&data->surfacelock);
        SDL_free(data);
        window->driverdata = NULL;
        return SDL_SetError("CreateWindowExW() failed: %lu", (unsigned long)GetLastError());
    }
    data->hwnd = hwnd;
    SetPropW(hwnd, L"SDLopWindow", (HANDLE)window);
    sdlop_win32.window_count++;

    if (!(window->flags & SDL_WINDOW_HIDDEN)) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    update_metrics(window);
    return true;
}

static void destroy_dib(SDLop_Win32WindowData *data)
{
    if (data->surface) {
        SDL_free(data->surface);
        data->surface = NULL;
    }
    if (data->bitmap) {
        DeleteObject(data->bitmap);
        data->bitmap = NULL;
    }
    if (data->dc) {
        DeleteDC(data->dc);
        data->dc = NULL;
    }
    data->surface_dirty = false;
}

static void win32_DestroyWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data) {
        return;
    }
    if (sdlop.keyboard_focus == window) {
        sdlop.keyboard_focus = NULL;
    }
    if (sdlop.mouse_focus == window) {
        sdlop.mouse_focus = NULL;
    }
    SDLOP_MUTEX_LOCK(&data->surfacelock);
    destroy_dib(data);
    SDLOP_MUTEX_UNLOCK(&data->surfacelock);
    if (data->hwnd) {
        RemovePropW(data->hwnd, L"SDLopWindow");
        DestroyWindow(data->hwnd);
        data->hwnd = NULL;
        sdlop_win32.window_count--;
    }
    SDLOP_MUTEX_DESTROY(&data->surfacelock);
    SDL_free(data);
    window->driverdata = NULL;
}

static void win32_ShowWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    window->flags &= ~SDL_WINDOW_HIDDEN;
    if (data && data->hwnd) {
        ShowWindow(data->hwnd, SW_SHOW);
        SetForegroundWindow(data->hwnd);
        SetFocus(data->hwnd);
    }
}

static void win32_HideWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    window->flags |= SDL_WINDOW_HIDDEN;
    if (data && data->hwnd) {
        ShowWindow(data->hwnd, SW_HIDE);
    }
}

static bool win32_SetWindowTitle(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return true;
    }
    wchar_t *wide = utf8_to_wide_alloc(window->title);
    if (!wide) {
        return SDL_OutOfMemory();
    }
    SetWindowTextW(data->hwnd, wide);
    SDL_free(wide);
    return true;
}

static bool win32_SetWindowSize(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (data && data->hwnd) {
        RECT rc = { 0, 0, window->w, window->h };
        win32_AdjustWindowSize(window, &rc);
        SetWindowPos(data->hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }
    /* the client rect is authoritative; report it back */
    update_metrics(window);
    return true;
}

static bool win32_SetWindowPosition(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (data && data->hwnd) {
        RECT rc = { 0, 0, window->w, window->h };
        win32_AdjustWindowSize(window, &rc);
        SetWindowPos(data->hwnd, NULL, window->x, window->y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_MOVED, window->x, window->y);
    return true;
}

static void win32_MinimizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (data && data->hwnd) {
        ShowWindow(data->hwnd, SW_MINIMIZE);
    }
}

static void win32_MaximizeWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (data && data->hwnd) {
        ShowWindow(data->hwnd, SW_MAXIMIZE);
    }
}

static void win32_RestoreWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (data && data->hwnd) {
        ShowWindow(data->hwnd, SW_RESTORE);
    }
}

static bool win32_SetWindowFullscreen(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return true;
    }
    const bool go_fullscreen = (window->flags & SDL_WINDOW_FULLSCREEN) != 0;
    if (go_fullscreen) {
        /* Remember what to restore to - but only on the windowed -> fullscreen
         * transition. SDL_SetWindowFullscreenMode() on a window that is
         * already fullscreen calls the driver again, and by then GetWindowRect
         * is the monitor rect and the style is WS_POPUP, so saving here every
         * time would record fullscreen as the windowed state and leaving
         * fullscreen would produce a borderless monitor-sized window. */
        if (!data->is_fullscreen) {
            GetWindowRect(data->hwnd, &data->windowed_rect);
            data->windowed_style = (DWORD)GetWindowLongPtrW(data->hwnd, GWL_STYLE);
            data->windowed_exstyle = (DWORD)GetWindowLongPtrW(data->hwnd, GWL_EXSTYLE);
            data->is_fullscreen = true;
        }
        MONITORINFO mi;
        SDL_zero(mi);
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(data->hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongPtrW(data->hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowLongPtrW(data->hwnd, GWL_EXSTYLE, data->windowed_exstyle & ~WS_EX_TOPMOST);
            SetWindowPos(data->hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOCOPYBITS | SWP_SHOWWINDOW);
        }
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_ENTER_FULLSCREEN, 0, 0);
    } else {
        data->is_fullscreen = false;
        SetWindowLongPtrW(data->hwnd, GWL_STYLE, (LONG_PTR)data->windowed_style);
        SetWindowLongPtrW(data->hwnd, GWL_EXSTYLE, (LONG_PTR)data->windowed_exstyle);
        SetWindowPos(data->hwnd, (data->windowed_exstyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST,
                     data->windowed_rect.left, data->windowed_rect.top,
                     data->windowed_rect.right - data->windowed_rect.left,
                     data->windowed_rect.bottom - data->windowed_rect.top,
                     SWP_NOCOPYBITS | SWP_SHOWWINDOW);
        SDLOP_SendWindowEvent(window, SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, 0, 0);
    }
    update_metrics(window);
    return true;
}

static void win32_RaiseWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (data && data->hwnd) {
        SetForegroundWindow(data->hwnd);
        BringWindowToTop(data->hwnd);
    }
}

static void win32_SetWindowClearColor(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (data && data->hwnd) {
        InvalidateRect(data->hwnd, NULL, FALSE);
    }
}

static void sdlop_relative_hide_cursor(bool hide);

static bool win32_SetWindowRelativeMouseMode(SDLop_VideoDevice *device, SDL_Window *window, bool enabled)
{
    (void)device;
    SDLop_Win32WindowData *data = window ? (SDLop_Win32WindowData *)window->driverdata : NULL;
    sdlop_win32.want_relative_mouse = enabled;
    /* Windows keeps ONE raw-input registration per usage per process, so this
     * must not fight the worker for it. win32_register_raw_keyboard() has the
     * same rule for the keyboard. When the worker is the source it owns both
     * registrations (RIDEV_INPUTSINK on its own hidden window) and already
     * delivers unaccelerated deltas, so leave them alone entirely: registering
     * here with hwndTarget=NULL would replace the worker's mouse registration
     * and RIDEV_REMOVE on the way out would delete it, after which
     * WM_MOUSEMOVE and the button messages are suppressed by
     * SDLOP_RawInputIsSource() and the pointer goes dead until restart. */
    if (!SDLOP_RawInputIsSource()) {
        RAWINPUTDEVICE rid;
        SDL_zero(rid);
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x02;
        rid.dwFlags = enabled ? 0 : RIDEV_REMOVE;
        rid.hwndTarget = NULL;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }
    if (enabled) {
        if (data && data->hwnd) {
            RECT rc;
            GetClientRect(data->hwnd, &rc);
            MapWindowPoints(data->hwnd, NULL, (POINT *)&rc, 2);
            ClipCursor(&rc);
            data->cursor_clipped_rect = rc;
            SetCapture(data->hwnd);
        }
        sdlop_relative_hide_cursor(true);
    } else {
        if (data) {
            release_cursor_clip(data);
        }
        ReleaseCapture();
        sdlop_relative_hide_cursor(false);
    }
    return true;
}

/* ShowCursor() is a counter, not a flag: it has to be moved by the same
 * amount it was moved before, and only when the state really changes. */
static void sdlop_relative_hide_cursor(bool hide)
{
    static bool hidden;
    if (hide == hidden) {
        return;
    }
    ShowCursor(hide ? FALSE : TRUE);
    hidden = hide;
}

/* ------------------------------------------------------------------ */
/* Event pump                                                          */
/* ------------------------------------------------------------------ */

/* Win32 raises no event when another application changes the clipboard, so
 * SDL3 compares the clipboard sequence number after each pump and reports a
 * non-owner update when it moves. Without this SDL_GetClipboardText silently
 * keeps returning the old string after a copy made in another program. */
static UINT win32_clipboard_format_png(void);   /* defined with the clipboard code */
static bool win32_open_clipboard(void);         /* ditto */

static void win32_check_clipboard_update(void)
{
    const DWORD count = GetClipboardSequenceNumber();
    char **mime_types = NULL;
    size_t num = 0;
    size_t capacity = 0;
    bool has_png = false;
    bool has_text = false;

    if (count == win32_clipboard_sequence) {
        return;
    }
    win32_clipboard_sequence = count;
    if (!count) {
        return; /* the clipboard was emptied and has no owner */
    }
    if (!SDL_EventEnabled(SDL_EVENT_CLIPBOARD_UPDATE)) {
        return; /* nobody is listening; do not pay for the format scan */
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT)) {
        has_text = true;
    }
    if (win32_clipboard_format_png() && IsClipboardFormatAvailable(win32_clipboard_format_png())) {
        has_png = true;
    }
    if (!has_text && !has_png) {
        return;
    }
    capacity = (has_text ? 1 : 0) + (has_png ? 1 : 0);
    mime_types = (char **)SDL_calloc(capacity + 1, sizeof(char *));
    if (!mime_types) {
        return;
    }
    if (has_text) {
        mime_types[num] = SDL_strdup("text/plain;charset=utf-8");
        if (mime_types[num]) {
            num++;
        }
    }
    if (has_png) {
        mime_types[num] = SDL_strdup("image/png");
        if (mime_types[num]) {
            num++;
        }
    }
    if (num) {
        /* owner = false: another application owns the clipboard now, so the
         * app's own offer callback is released and these types are what
         * reading will produce. */
        mime_types[num] = NULL;
        SDLOP_SendClipboardUpdate(false, mime_types, num);
    } else {
        SDL_free(mime_types);
        mime_types = NULL;
    }
    /* SDLOP_SendClipboardUpdate keeps the array on the false path; freeing it
     * here would leave the event pointing at freed memory. */
}

static void win32_PumpEvents(SDLop_VideoDevice *device, int timeout_ms)
{
    (void)device;
    MSG msg;
    win32_check_clipboard_update();
    if (timeout_ms < 0) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return;
    }
    /* Blocking wait: SDL_WaitEvent semantics. MsgWaitForMultipleObjects
     * wakes on input *and* on messages posted to this thread. */
    const DWORD start = GetTickCount();
    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (timeout_ms == 0) {
            return;
        }
        DWORD wait = timeout_ms < 0 ? 0 : (DWORD)timeout_ms;
        if (timeout_ms > 0) {
            const DWORD elapsed = GetTickCount() - start;
            if (elapsed >= (DWORD)timeout_ms) {
                return;
            }
            wait = (DWORD)timeout_ms - elapsed;
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, wait, QS_ALLINPUT);
        if (timeout_ms > 0 && (GetTickCount() - start) >= (DWORD)timeout_ms) {
            return;
        }
        if (timeout_ms < 0) {
            continue; /* SDL_WaitEvent: block forever for the next message */
        }
        return;
    }
}

static int win32_GetEventFD(SDLop_VideoDevice *device)
{
    (void)device;
    return -1; /* no pollable fd: Win32 messages are pumped, not polled */
}

/* ------------------------------------------------------------------ */
/* Software framebuffer                                                */
/* ------------------------------------------------------------------ */

static bool win32_CreateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface **surface)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return SDL_SetError("Window has no HWND");
    }
    SDLOP_MUTEX_LOCK(&data->surfacelock);
    destroy_dib(data);

    BITMAPINFO bmi;
    SDL_zero(bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = window->w;
    bmi.bmiHeader.biHeight = -window->h; /* top-down, like SDL surfaces */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(data->hwnd);
    data->dc = CreateCompatibleDC(screen);
    data->bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &data->pixels, NULL, 0);
    ReleaseDC(data->hwnd, screen);
    if (!data->dc || !data->bitmap || !data->pixels) {
        destroy_dib(data);
        SDLOP_MUTEX_UNLOCK(&data->surfacelock);
        return SDL_SetError("Couldn't create DIB section");
    }
    SelectObject(data->dc, data->bitmap);

    SDL_Surface *s = SDL_CreateSurfaceFrom(window->w, window->h, SDL_PIXELFORMAT_XRGB8888,
                                           data->pixels, window->w * 4);
    if (!s) {
        destroy_dib(data);
        SDLOP_MUTEX_UNLOCK(&data->surfacelock);
        return false;
    }
    data->surface = s;
    data->surface_dirty = true;
    SDLOP_MUTEX_UNLOCK(&data->surfacelock);
    *surface = s;
    return true;
}

static bool win32_UpdateWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd || !data->dc || !data->surface) {
        return true;
    }
    SDLOP_MUTEX_LOCK(&data->surfacelock);
    if (data->surface->w != window->w || data->surface->h != window->h) {
        /* resized under us: the DIB must be rebuilt at the new size */
        SDLOP_MUTEX_UNLOCK(&data->surfacelock);
        SDL_Surface *fresh = NULL;
        if (!win32_CreateWindowFramebuffer(device, window, &fresh)) {
            return false;
        }
        window->surface = fresh;
        data = (SDLop_Win32WindowData *)window->driverdata;
        SDLOP_MUTEX_LOCK(&data->surfacelock);
    }
    HDC hdc = GetDC(data->hwnd);
    if (hdc) {
        if (numrects > 0 && rects) {
            for (int i = 0; i < numrects; i++) {
                const RECT rc = { rects[i].x, rects[i].y, rects[i].x + rects[i].w, rects[i].y + rects[i].h };
                BitBlt(hdc, rc.left, rc.top, rects[i].w, rects[i].h, data->dc, rc.left, rc.top, SRCCOPY);
            }
        } else {
            BitBlt(hdc, 0, 0, window->w, window->h, data->dc, 0, 0, SRCCOPY);
        }
        ReleaseDC(data->hwnd, hdc);
    }
    data->surface_dirty = false;
    SDLOP_MUTEX_UNLOCK(&data->surfacelock);
    return true;
}

static void win32_DestroyWindowFramebuffer(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (data) {
        SDLOP_MUTEX_LOCK(&data->surfacelock);
        destroy_dib(data);
        SDLOP_MUTEX_UNLOCK(&data->surfacelock);
    }
}

/* ------------------------------------------------------------------ */
/* Misc window management                                              */
/* ------------------------------------------------------------------ */

static void win32_SetWindowMinimumSize(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data) {
        return;
    }
    data->has_minimum_size = window->min_w > 0 || window->min_h > 0;
    data->minimum_w = window->min_w;
    data->minimum_h = window->min_h;
    if (data->hwnd) {
        RECT rc = { 0, 0, data->minimum_w, data->minimum_h };
        SDLOP_Win32AdjustWindowRect(window, &rc);
        SetWindowPos(data->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void win32_SetWindowMaximumSize(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data) {
        return;
    }
    data->has_maximum_size = window->max_w > 0 || window->max_h > 0;
    data->maximum_w = window->max_w;
    data->maximum_h = window->max_h;
    if (data->hwnd) {
        SetWindowPos(data->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static bool win32_GetWindowBordersSize(SDLop_VideoDevice *device, SDL_Window *window, int *top, int *left, int *bottom, int *right)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return SDL_SetError("Window has no HWND");
    }
    RECT rc;
    if (!GetWindowRect(data->hwnd, &rc)) {
        return SDL_SetError("GetWindowRect() failed");
    }
    /* Client rect in screen coordinates gives all four borders at once. */
    POINT client = { 0, 0 };
    if (!ClientToScreen(data->hwnd, &client)) {
        return SDL_SetError("ClientToScreen() failed");
    }
    RECT client_rect;
    if (!GetClientRect(data->hwnd, &client_rect)) {
        return SDL_SetError("GetClientRect() failed");
    }
    if (top) {
        *top = client.y - rc.top;
    }
    if (left) {
        *left = client.x - rc.left;
    }
    if (bottom) {
        *bottom = rc.bottom - (client.y + (client_rect.bottom - client_rect.top));
    }
    if (right) {
        *right = rc.right - (client.x + (client_rect.right - client_rect.left));
    }
    return true;
}

static bool win32_SetWindowOpacity(SDLop_VideoDevice *device, SDL_Window *window, float opacity)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return SDL_SetError("Window has no HWND");
    }
    LONG_PTR exstyle = GetWindowLongPtrW(data->hwnd, GWL_EXSTYLE);
    const BYTE alpha = (BYTE)(255.0f * (opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity)));
    if (alpha == 255) {
        SetWindowLongPtrW(data->hwnd, GWL_EXSTYLE, exstyle & ~WS_EX_LAYERED);
        return true;
    }
    SetWindowLongPtrW(data->hwnd, GWL_EXSTYLE, exstyle | WS_EX_LAYERED);
    if (!SetLayeredWindowAttributes(data->hwnd, 0, alpha, LWA_ALPHA)) {
        return SDL_SetError("SetLayeredWindowAttributes() failed");
    }
    return true;
}

static void win32_SetWindowAlwaysOnTop(SDLop_VideoDevice *device, SDL_Window *window, bool on_top)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (data && data->hwnd) {
        SetWindowPos(data->hwnd, on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

static void win32_SetWindowBordered(SDLop_VideoDevice *device, SDL_Window *window, bool bordered)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return;
    }
    /* ``bordered`` is expressed through window->flags by the core, so the
     * style is recomputed from the flags -- same approach as SDL3, which
     * re-derives GetWindowStyle() rather than patching single bits. */
    if (bordered) {
        window->flags &= ~SDL_WINDOW_BORDERLESS;
    } else {
        window->flags |= SDL_WINDOW_BORDERLESS;
    }
    const DWORD style = window_style_for(window);
    SetWindowLongPtrW(data->hwnd, GWL_EXSTYLE, (LONG_PTR)window_exstyle_for(window));
    SetWindowLongPtrW(data->hwnd, GWL_STYLE, (LONG_PTR)style);
    SetWindowPos(data->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

/* Nothing to install: WM_SIZING consults the ratio on every drag. Apply it to
 * the current size too, so a new constraint takes effect immediately. */
static void win32_SetWindowAspectRatio(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = window ? (SDLop_Win32WindowData *)window->driverdata : NULL;
    RECT rc;
    if (!data || !data->hwnd || !GetWindowRect(data->hwnd, &rc)) {
        return;
    }
    win32_constrain_aspect(window, &rc);
}

static void win32_SetWindowResizable(SDLop_VideoDevice *device, SDL_Window *window, bool resizable)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return;
    }
    DWORD style = (DWORD)GetWindowLongPtrW(data->hwnd, GWL_STYLE);
    if (resizable) {
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    } else {
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    SetWindowLongPtrW(data->hwnd, GWL_STYLE, style);
    SetWindowPos(data->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

/* Undocumented but the only way to show the system menu at a point; SDL3
 * defines it the same way. */
#ifndef WM_POPUPSYSTEMMENU
#define WM_POPUPSYSTEMMENU 0x313
#endif

/* SDL_ShowWindowSystemMenu(): the window menu at a client point. Win32 answers
 * WM_POPUPSYSTEMMENU by showing the system menu there (SDL3 does the same). */
static void win32_ShowWindowSystemMenu(SDLop_VideoDevice *device, SDL_Window *window, int x, int y)
{
    (void)device;
    SDLop_Win32WindowData *data = window ? (SDLop_Win32WindowData *)window->driverdata : NULL;
    POINT pt;
    if (!data || !data->hwnd) {
        return;
    }
    pt.x = x;
    pt.y = y;
    if (ClientToScreen(data->hwnd, &pt)) {
        SendMessageW(data->hwnd, WM_POPUPSYSTEMMENU, 0, MAKELPARAM(pt.x, pt.y));
    }
}

/* SDL_GetWindowICCProfile(): the monitor's ICC profile, as the raw file
 * bytes. Windows keeps the profile as a path (GDI "ICM profile" on the
 * display's device context), so the file is read here and the caller frees
 * the buffer. Returns NULL with an error when the monitor has no profile,
 * which is common -- a plain sRGB display often has none installed. */
static void *win32_GetWindowICCProfile(SDLop_VideoDevice *device, SDL_Window *window, size_t *size)
{
    SDLop_Win32WindowData *data = window ? (SDLop_Win32WindowData *)window->driverdata : NULL;
    wchar_t path[MAX_PATH];
    DWORD path_len = (DWORD)SDL_arraysize(path);
    HMONITOR hmon;
    MONITORINFOEXW mi;
    HDC hdc;
    FILE *file;
    void *contents = NULL;

    if (size) {
        *size = 0;
    }
    (void)device;
    if (!data || !data->hwnd) {
        SDL_SetError("no window");
        return NULL;
    }
    hmon = MonitorFromWindow(data->hwnd, MONITOR_DEFAULTTONEAREST);
    SDL_zero(mi);
    mi.cbSize = sizeof(mi);
    if (!hmon || !GetMonitorInfoW(hmon, (MONITORINFO *)&mi)) {
        SDL_SetError("could not find the window's monitor");
        return NULL;
    }

    hdc = CreateDCW(mi.szDevice, NULL, NULL, NULL);
    if (!hdc) {
        SDL_SetError("could not open the display device context");
        return NULL;
    }
    if (!GetICMProfileW(hdc, &path_len, path)) {
        DeleteDC(hdc);
        SDL_SetError("the monitor has no ICC profile");
        return NULL;
    }
    DeleteDC(hdc);

    file = _wfopen(path, L"rb");
    if (!file) {
        SDL_SetError("could not open the ICC profile file");
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) == 0) {
        const long length = ftell(file);
        if (length > 0 && fseek(file, 0, SEEK_SET) == 0) {
            contents = SDL_malloc((size_t)length);
            if (contents && fread(contents, 1, (size_t)length, file) != (size_t)length) {
                SDL_free(contents);
                contents = NULL;
            }
            if (contents && size) {
                *size = (size_t)length;
            }
        }
    }
    fclose(file);
    if (!contents) {
        SDL_SetError("could not read the ICC profile file");
    }
    return contents;
}

/* SDL_SetTextInputArea(): tell the IME where the text cursor is, so the
 * composition window and the candidate list open next to it instead of in a
 * corner. Both are client coordinates. */
static bool win32_UpdateTextInputArea(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = window ? (SDLop_Win32WindowData *)window->driverdata : NULL;
    HIMC himc;
    const SDL_Rect *rect;

    if (!data || !data->hwnd) {
        return true;
    }
    rect = &window->text_input_rect;
    if (rect->w <= 0 || rect->h <= 0) {
        return true; /* nothing to position against */
    }

    himc = ImmGetContext(data->hwnd);
    if (!himc) {
        return true; /* no IME on this window: not an error */
    }

    {
        COMPOSITIONFORM cof;
        CANDIDATEFORM caf;
        int font_height = rect->h;
        LOGFONTW font;

        if (ImmGetCompositionFontW(himc, &font) && font.lfHeight) {
            font_height = font.lfHeight;
        }

        SDL_zero(cof);
        cof.dwStyle = CFS_RECT;
        cof.ptCurrentPos.x = rect->x + window->text_input_cursor;
        cof.ptCurrentPos.y = rect->y + (rect->h - font_height) / 2;
        cof.rcArea.left = rect->x;
        cof.rcArea.right = rect->x + rect->w;
        cof.rcArea.top = rect->y;
        cof.rcArea.bottom = rect->y + rect->h;
        ImmSetCompositionWindow(himc, &cof);

        SDL_zero(caf);
        caf.dwIndex = 0;
        caf.dwStyle = CFS_EXCLUDE;
        caf.ptCurrentPos.x = rect->x + window->text_input_cursor;
        caf.ptCurrentPos.y = rect->y;
        caf.rcArea.left = rect->x;
        caf.rcArea.right = rect->x + rect->w;
        caf.rcArea.top = rect->y;
        caf.rcArea.bottom = rect->y + rect->h;
        ImmSetCandidateWindow(himc, &caf);
    }

    ImmReleaseContext(data->hwnd, himc);
    return true;
}

/* SDL_SetWindowShape(): SDL_WINDOW_TRANSPARENT windows. Win32 expresses a
 * non-rectangular window as a window region, so the alpha channel is turned
 * into one span per run of opaque pixels (the approach SDL3 takes in
 * SDL_windowsshape.c). A NULL shape puts the rectangular region back. */
static void win32_add_shape_span(HRGN *region, int x1, int y1, int x2, int y2)
{
    HRGN span = CreateRectRgn(x1, y1, x2, y2);
    if (!span) {
        return;
    }
    if (*region) {
        CombineRgn(*region, *region, span, RGN_OR);
        DeleteObject(span);
    } else {
        *region = span;
    }
}

static HRGN win32_region_from_shape(SDL_Surface *shape, int offset_x, int offset_y)
{
    HRGN region = NULL;
    for (int y = 0; y < shape->h; y++) {
        const Uint8 *row = (const Uint8 *)shape->pixels + (size_t)y * shape->pitch;
        int span_start = -1;
        for (int x = 0; x < shape->w; x++) {
            const Uint8 alpha = row[x * 4 + 3];
            if (alpha == SDL_ALPHA_TRANSPARENT) {
                if (span_start >= 0) {
                    win32_add_shape_span(&region, offset_x + span_start, offset_y + y,
                                         offset_x + x, offset_y + y + 1);
                    span_start = -1;
                }
            } else if (span_start < 0) {
                span_start = x;
            }
        }
        if (span_start >= 0) {
            win32_add_shape_span(&region, offset_x + span_start, offset_y + y,
                                 offset_x + shape->w, offset_y + y + 1);
        }
    }
    return region;
}

static bool win32_UpdateWindowShape(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface *shape)
{
    (void)device;
    SDLop_Win32WindowData *data = window ? (SDLop_Win32WindowData *)window->driverdata : NULL;
    HRGN region = NULL;

    if (!data || !data->hwnd) {
        return true;
    }

    if (shape) {
        /* The frame is drawn outside the client area, so shift the shape by
         * the frame size and add the frame itself back as four spans, or the
         * title bar and borders would be cut away with the shape. */
        RECT frame = { 0, 0, 0, 0 };
        const bool bordered = (window->flags & SDL_WINDOW_BORDERLESS) == 0;
        if (bordered) {
            SDLOP_Win32AdjustWindowRect(window, &frame);
        }
        const int ox = -frame.left;
        const int oy = -frame.top;

        region = win32_region_from_shape(shape, ox, oy);
        if (bordered) {
            const int full_w = ox + shape->w + frame.right;
            const int full_h = oy + shape->h + frame.bottom;
            win32_add_shape_span(&region, 0, 0, full_w + 1, oy + 1);                      /* top */
            win32_add_shape_span(&region, 0, oy, ox + 1, oy + shape->h + 1);              /* left */
            win32_add_shape_span(&region, ox + shape->w, oy, full_w + 1, oy + shape->h + 1); /* right */
            win32_add_shape_span(&region, 0, oy + shape->h, full_w + 1, full_h + 1);      /* bottom */
        }
    }

    /* SetWindowRgn takes ownership of the region on success */
    if (!SetWindowRgn(data->hwnd, region, TRUE)) {
        if (region) {
            DeleteObject(region);
        }
        return SDL_SetError("SetWindowRgn() failed");
    }
    return true;
}

/* SDL_SetWindowHitTest(): nothing to install up front, the callback is
 * consulted from WM_NCHITTEST whenever the window manager asks. */
static bool win32_SetWindowHitTest(SDLop_VideoDevice *device, SDL_Window *window, bool enabled)
{
    (void)device;
    (void)window;
    (void)enabled;
    return true;
}

/* SDL_CaptureMouse(): SDL_WINDOW_MOUSE_CAPTURE. SetCapture makes Win32 keep
 * sending mouse messages to this window after the pointer leaves it, which is
 * what "drag outside the window and keep tracking" needs. */
static bool win32_CaptureMouse(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    if (window) {
        SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
        if (!data || !data->hwnd) {
            return false;
        }
        SetCapture(data->hwnd);
    } else if (GetCapture() != NULL) {
        ReleaseCapture();
    }
    return true;
}

/* Keep the display on while a window has focus, the way SDL3 does: Windows
 * has no screensaver API, SetThreadExecutionState is the documented way to
 * say "the user is here". Called again periodically, so re-assert it. */
static bool win32_SuspendScreenSaver(SDLop_VideoDevice *device)
{
    (void)device;
    if (!sdlop.suspend_screensaver) {
        return true;
    }
    if (!SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)) {
        return SDL_SetError("SetThreadExecutionState() failed");
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Cursors                                                             */
/* ------------------------------------------------------------------ */

/* Map an SDL system cursor onto the Win32 stock cursor id. SDL3 does the same
 * through WINCURSORTYPE; the diagonal resizes have no Win32 stock cursor, so
 * they fall back to the double arrow pair, as SDL3 does. */
static LPCTSTR win32_cursor_name(SDL_SystemCursor id)
{
    switch (id) {
    case SDL_SYSTEM_CURSOR_TEXT: return IDC_IBEAM;
    case SDL_SYSTEM_CURSOR_WAIT: return IDC_WAIT;
    case SDL_SYSTEM_CURSOR_CROSSHAIR: return IDC_CROSS;
    case SDL_SYSTEM_CURSOR_PROGRESS: return IDC_APPSTARTING;
    case SDL_SYSTEM_CURSOR_NWSE_RESIZE: return IDC_SIZENWSE;
    case SDL_SYSTEM_CURSOR_NESW_RESIZE: return IDC_SIZENESW;
    case SDL_SYSTEM_CURSOR_EW_RESIZE: return IDC_SIZEWE;
    case SDL_SYSTEM_CURSOR_NS_RESIZE: return IDC_SIZENS;
    case SDL_SYSTEM_CURSOR_MOVE: return IDC_SIZEALL;
    case SDL_SYSTEM_CURSOR_NOT_ALLOWED: return IDC_NO;
    case SDL_SYSTEM_CURSOR_POINTER: return IDC_HAND;
    case SDL_SYSTEM_CURSOR_NW_RESIZE:
    case SDL_SYSTEM_CURSOR_SE_RESIZE: return IDC_SIZENWSE;
    case SDL_SYSTEM_CURSOR_NE_RESIZE:
    case SDL_SYSTEM_CURSOR_SW_RESIZE: return IDC_SIZENESW;
    case SDL_SYSTEM_CURSOR_E_RESIZE:
    case SDL_SYSTEM_CURSOR_W_RESIZE: return IDC_SIZEWE;
    case SDL_SYSTEM_CURSOR_N_RESIZE:
    case SDL_SYSTEM_CURSOR_S_RESIZE: return IDC_SIZENS;
    case SDL_SYSTEM_CURSOR_DEFAULT:
    default: return IDC_ARROW;
    }
}

/* Build a Win32 cursor from the ARGB8888 image the core prepared. Win32 wants
 * a 1-bit AND mask plus a 1-bit XOR mask, so alpha is thresholded: pixels that
 * are more than half opaque become part of the image, the rest stay
 * transparent (AND mask 1, XOR mask 0). */
static HCURSOR win32_create_cursor_from_surface(SDL_Surface *surface, int hot_x, int hot_y)
{
    const int w = surface->w;
    const int h = surface->h;
    ICONINFO info;
    HBITMAP color = NULL, mask = NULL;
    HCURSOR cursor = NULL;
    const size_t mask_stride = (((size_t)w + 15) / 16) * 2;
    const size_t color_stride = (((size_t)w * 32 + 31) / 32) * 4;
    Uint8 *mask_bits = (Uint8 *)SDL_calloc(mask_stride, (size_t)h);
    Uint8 *color_bits = (Uint8 *)SDL_calloc(color_stride, (size_t)h);

    if (!mask_bits || !color_bits) {
        SDL_free(mask_bits);
        SDL_free(color_bits);
        return NULL;
    }

    for (int y = 0; y < h; y++) {
        const Uint32 *row = (const Uint32 *)((const Uint8 *)surface->pixels + y * surface->pitch);
        for (int x = 0; x < w; x++) {
            const Uint32 argb = row[x];
            const Uint8 alpha = (Uint8)(argb >> 24);
            const Uint8 r = (Uint8)(argb >> 16);
            const Uint8 g = (Uint8)(argb >> 8);
            const Uint8 b = (Uint8)(argb);
            /* 1bpp, MSB first, rows are DWORD-aligned */
            const size_t bit = mask_stride * (size_t)y + (size_t)x / 8;
            const Uint8 bit_mask = (Uint8)(0x80 >> (x % 8));
            const size_t cbit = color_stride * (size_t)y + ((size_t)x * 4);
            const Uint32 rgb = ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
            if (alpha >= 0x80) {
                mask_bits[bit] &= (Uint8)~bit_mask; /* opaque: AND 0 */
                SDL_memcpy(&color_bits[cbit], &rgb, 4);
            } else {
                mask_bits[bit] |= bit_mask;        /* transparent: AND 1, XOR 0 */
            }
        }
    }

    color = CreateBitmap(w, h, 1, 32, color_bits);
    mask = CreateBitmap(w, h, 1, 1, mask_bits);
    if (color && mask) {
        SDL_zero(info);
        info.fIcon = FALSE;
        info.xHotspot = (DWORD)hot_x;
        info.yHotspot = (DWORD)hot_y;
        info.hbmMask = mask;
        info.hbmColor = color;
        cursor = CreateIconIndirect(&info);
    }
    if (color) {
        DeleteObject(color);
    }
    if (mask) {
        DeleteObject(mask);
    }
    SDL_free(mask_bits);
    SDL_free(color_bits);
    return cursor;
}

/* Show/hide by installing the cursor itself, the way SDL3 does, instead of
 * moving Win32's ShowCursor() counter: the counter is process-global state
 * that no longer matches when application threads call ShowCursor(), and a
 * NULL cursor leaves nothing on screen anyway. */
static bool win32_ShowCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    if (sdlop.keyboard_focus == NULL && sdlop.mouse_focus == NULL) {
        return true; /* no window of ours to install it on */
    }
    SetCursor(cursor ? (HCURSOR)cursor->internal : NULL);
    return true;
}

/* Warp to a point in window coordinates. The window messages report the
 * motion, so the core does not need a synthetic event. */
static bool win32_WarpMouse(SDLop_VideoDevice *device, SDL_Window *window, float x, float y)
{
    (void)device;
    SDLop_Win32WindowData *data = window ? (SDLop_Win32WindowData *)window->driverdata : NULL;
    POINT pt;
    if (!data || !data->hwnd) {
        return false;
    }
    pt.x = (LONG)SDL_lroundf(x);
    pt.y = (LONG)SDL_lroundf(y);
    if (!ClientToScreen(data->hwnd, &pt)) {
        return false;
    }
    /* Windows occasionally ignores a SetCursorPos() to the position the cursor
     * is already at (and over RDP it coalesces them), so nudge by one pixel
     * and come back -- same trick as SDL3's WIN_SetCursorPos(). */
    SetCursorPos(pt.x, pt.y);
    SetCursorPos(pt.x + 1, pt.y);
    return SetCursorPos(pt.x, pt.y) ? true : false;
}

static bool win32_WarpMouseGlobal(SDLop_VideoDevice *device, float x, float y)
{
    (void)device;
    /* the desktop is in physical pixels and so is SetCursorPos, so these are
     * already the right units once the process is DPI aware */
    return SetCursorPos((int)SDL_lroundf(x), (int)SDL_lroundf(y)) ? true : false;
}

/* The real buttons, even when the window is not focused: this is what
 * SDL_GetGlobalMouseState() is for. */
static SDL_MouseButtonFlags win32_GetGlobalMouseState(SDLop_VideoDevice *device, float *x, float *y)
{
    (void)device;
    POINT pt;
    SDL_MouseButtonFlags flags = 0;

    if (GetCursorPos(&pt)) {
        if (x) {
            *x = (float)pt.x;
        }
        if (y) {
            *y = (float)pt.y;
        }
    }
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        flags |= SDL_BUTTON_LMASK;
    }
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) {
        flags |= SDL_BUTTON_MMASK;
    }
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) {
        flags |= SDL_BUTTON_RMASK;
    }
    if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) {
        flags |= SDL_BUTTON_X1MASK;
    }
    if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) {
        flags |= SDL_BUTTON_X2MASK;
    }
    if (GetSystemMetrics(SM_SWAPBUTTON)) {
        /* swap left and right */
        if (flags & SDL_BUTTON_LMASK) {
            flags &= ~SDL_BUTTON_LMASK;
            flags |= SDL_BUTTON_RMASK;
        } else if (flags & SDL_BUTTON_RMASK) {
            flags &= ~SDL_BUTTON_RMASK;
            flags |= SDL_BUTTON_LMASK;
        }
    }
    return flags;
}

static bool win32_CreateCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    HCURSOR hcursor;
    if (cursor->surface) {
        hcursor = win32_create_cursor_from_surface(cursor->surface, cursor->hot_x, cursor->hot_y);
        if (!hcursor) {
            return SDL_SetError("Couldn't create a Windows cursor from the image");
        }
    } else {
        /* shared stock cursors are owned by the system: copy them so FreeCursor
         * can treat every cursor the same way, as SDL3 does */
        /* IDC_* are MAKEINTRESOURCE(), narrow unless UNICODE is defined, so
         * go through the resource id rather than the macro's type. */
        hcursor = CopyCursor(LoadCursorW(NULL,
                                         (LPCWSTR)win32_cursor_name(cursor->system_id)));
        if (!hcursor) {
            return SDL_SetError("Couldn't load the Windows system cursor");
        }
    }
    cursor->internal = (void *)hcursor;
    return true;
}

static void win32_FreeCursor(SDLop_VideoDevice *device, SDL_Cursor *cursor)
{
    (void)device;
    HCURSOR hcursor = (HCURSOR)cursor->internal;
    if (hcursor && SDLOP_GetCurrentCursor() == cursor) {
        SetCursor(NULL);
    }
    if (hcursor) {
        DestroyCursor(hcursor);
    }
    cursor->internal = NULL;
}

static bool win32_SetWindowMouseRect(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return true;
    }
    /* SDL_SetWindowMouseRect: clip the cursor to the rect, or the window */
    RECT clip;
    if (window->mouse_rect.w > 0 && window->mouse_rect.h > 0) {
        clip.left = window->mouse_rect.x;
        clip.top = window->mouse_rect.y;
        clip.right = clip.left + window->mouse_rect.w;
        clip.bottom = clip.top + window->mouse_rect.h;
        MapWindowPoints(data->hwnd, NULL, (POINT *)&clip, 2);
    } else {
        GetWindowRect(data->hwnd, &clip);
    }
    ClipCursor(&clip);
    data->cursor_clipped_rect = clip;
    return true;
}

static bool win32_SetWindowMouseGrab(SDLop_VideoDevice *device, SDL_Window *window, bool grabbed)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return true;
    }
    if (grabbed) {
        RECT rc;
        GetWindowRect(data->hwnd, &rc);
        ClipCursor(&rc);
        data->cursor_clipped_rect = rc;
        SetCapture(data->hwnd);
    } else {
        release_cursor_clip(data);
        ReleaseCapture();
    }
    return true;
}

static bool win32_SetWindowKeyboardGrab(SDLop_VideoDevice *device, SDL_Window *window, bool grabbed)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return true;
    }
    if (grabbed) {
        SetFocus(data->hwnd);
    }
    return true;
}

static bool win32_FlashWindow(SDLop_VideoDevice *device, SDL_Window *window, SDL_FlashOperation operation)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return SDL_SetError("Window has no HWND");
    }
    FLASHWINFO fi;
    SDL_zero(fi);
    fi.cbSize = sizeof(fi);
    fi.hwnd = data->hwnd;
    switch (operation) {
    case SDL_FLASH_CANCEL:
        fi.dwFlags = FLASHW_STOP;
        break;
    case SDL_FLASH_BRIEFLY:
        fi.dwFlags = FLASHW_TRAY;
        fi.uCount = 2;
        break;
    case SDL_FLASH_UNTIL_FOCUSED:
        fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        break;
    default:
        return SDL_Unsupported();
    }
    if (!FlashWindowEx(&fi)) {
        return SDL_SetError("FlashWindowEx() failed");
    }
    return true;
}

static bool win32_SyncWindow(SDLop_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return true;
    }
    /* True once the window has no pending messages; SDL3 returns true when
     * there is nothing left to do. */
    MSG msg;
    if (PeekMessageW(&msg, data->hwnd, 0, 0, PM_NOREMOVE)) {
        return false;
    }
    return true;
}

static bool win32_SetWindowFocusable(SDLop_VideoDevice *device, SDL_Window *window, bool focusable)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return true;
    }
    LONG_PTR exstyle = GetWindowLongPtrW(data->hwnd, GWL_EXSTYLE);
    if (focusable) {
        exstyle &= ~WS_EX_NOACTIVATE;
    } else {
        exstyle |= WS_EX_NOACTIVATE;
    }
    SetWindowLongPtrW(data->hwnd, GWL_EXSTYLE, exstyle);
    return true;
}

static bool win32_SetWindowParent(SDLop_VideoDevice *device, SDL_Window *window, SDL_Window *parent)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return true;
    }
    HWND parent_hwnd = NULL;
    if (parent && parent->driverdata) {
        parent_hwnd = ((SDLop_Win32WindowData *)parent->driverdata)->hwnd;
    }
    SetWindowLongPtrW(data->hwnd, GWLP_HWNDPARENT, (LONG_PTR)parent_hwnd);
    return true;
}

static bool win32_SetWindowModal(SDLop_VideoDevice *device, SDL_Window *window, bool modal)
{
    (void)device;
    (void)window;
    (void)modal;
    return true; /* parenting covers SDL3's use; modality is the app's job */
}

static bool win32_SetWindowIcon(SDLop_VideoDevice *device, SDL_Window *window, SDL_Surface *icon)
{
    (void)device;
    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data || !data->hwnd) {
        return true;
    }
    if (!icon) {
        return SDL_InvalidParamError("icon");
    }
    /* Build a 32-bpp DIB and hand it to SetIcon; Windows keeps the bitmap. */
    SDL_Surface *argb = SDL_ConvertSurface(icon, SDL_PIXELFORMAT_ARGB8888);
    if (!argb) {
        return false;
    }
    BITMAPV5HEADER bi;
    SDL_zero(bi);
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = argb->w;
    bi.bV5Height = -argb->h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC hdc = GetDC(data->hwnd);
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(data->hwnd, hdc);
    HBITMAP mask = CreateBitmap(argb->w, argb->h, 1, 1, NULL);
    if (!color || !mask || !bits) {
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        SDL_DestroySurface(argb);
        return SDL_SetError("Couldn't create icon bitmap");
    }
    for (int y = 0; y < argb->h; y++) {
        const Uint8 *src = (const Uint8 *)argb->pixels + (size_t)y * argb->pitch;
        Uint8 *dst = (Uint8 *)bits + (size_t)y * argb->w * 4;
        for (int x = 0; x < argb->w; x++) {
            const Uint32 px = ((const Uint32 *)src)[x];
            const Uint8 a = (Uint8)(px >> 24);
            if (a == 0) {
                dst[x * 4 + 0] = 0;
                dst[x * 4 + 1] = 0;
                dst[x * 4 + 2] = 0;
                dst[x * 4 + 3] = 0;
            } else {
                /* premultiply; Windows uses straight alpha but tolerates both */
                dst[x * 4 + 0] = (Uint8)((((px >> 0) & 0xFF) * a) / 255);
                dst[x * 4 + 1] = (Uint8)((((px >> 8) & 0xFF) * a) / 255);
                dst[x * 4 + 2] = (Uint8)((((px >> 16) & 0xFF) * a) / 255);
                dst[x * 4 + 3] = a;
            }
        }
    }
    SDL_DestroySurface(argb);
    ICONINFO ii;
    SDL_zero(ii);
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON handle = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    if (!handle) {
        return SDL_SetError("CreateIconIndirect() failed");
    }
    HICON old_small = (HICON)SendMessageW(data->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)handle);
    HICON old_big = (HICON)SendMessageW(data->hwnd, WM_SETICON, ICON_BIG, (LPARAM)handle);
    if (old_small) DestroyIcon(old_small);
    if (old_big) DestroyIcon(old_big);
    data->icon = handle;
    return true;
}

static SDL_SystemTheme win32_GetSystemTheme(SDLop_VideoDevice *device)
{
    (void)device;
    HKEY key;
    DWORD value = 1, size = sizeof(value);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        const LONG rc = RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&value, &size);
        RegCloseKey(key);
        if (rc == ERROR_SUCCESS) {
            return value ? SDL_SYSTEM_THEME_LIGHT : SDL_SYSTEM_THEME_DARK;
        }
    }
    return SDL_SYSTEM_THEME_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Device table                                                        */
/* ------------------------------------------------------------------ */


/* ---- Clipboard ----------------------------------------------------------
 *
 * Win32 keeps the clipboard itself: an app hands over a block of memory per
 * format and the system shares it, so there is nothing to render later. Real
 * SDL3 does the same and notes that delayed rendering buys nothing for text
 * and images (both are requested immediately). Accessibility formats such as
 * text/html are converted to CF_HTML, the way CF_HTML-aware apps expect.
 *
 * Formats SDLop does not implement here fall back to the in-process clipboard,
 * which is the generic behaviour when these hooks are NULL.
 */

/* Formats a caller should see as text when reading the clipboard. */
static bool win32_is_text_mime_type(const char *mime_type)
{
    /* SDL3 uses the same prefix test (SDL_IsTextMimeType). */
    return SDL_strncmp(mime_type, "text", 4) == 0;
}

/* Any window of ours will do as the clipboard owner; NULL is also valid (the
 * clipboard is then owned by the task), so this never fails the operation. */
static HWND win32_clipboard_window(void)
{
    SDL_Window *window = sdlop.keyboard_focus;
    if (!window) {
        window = sdlop.windows;
    }
    if (window && window->driverdata) {
        return ((SDLop_Win32WindowData *)window->driverdata)->hwnd;
    }
    return NULL;
}

/* The clipboard is a shared resource: another process may hold it open, so
 * retry briefly instead of failing outright (as SDL3 does). */
static bool win32_open_clipboard(void)
{
    HWND owner = win32_clipboard_window();
    int attempt;

    for (attempt = 0; attempt < 3; ++attempt) {
        if (OpenClipboard(owner)) {
            return true;
        }
        SDL_Delay(10);
    }
    return SDL_SetError("Couldn't open clipboard");
}

static UINT win32_clipboard_format_png(void)
{
    static UINT format;
    if (!format) {
        format = RegisterClipboardFormatW(L"PNG");
    }
    return format;
}

static bool win32_set_clipboard_text(const char *mime_type)
{
    size_t size = 0;
    wchar_t *wide;
    HGLOBAL memory;
    int length;
    bool result = false;

    /* One fetch: SDLOP_GetInternalClipboardData runs the application's data
     * callback, and asking twice for the size and then the bytes called it
     * twice for a single copy. */
    void *utf8 = SDLOP_GetInternalClipboardData(mime_type, &size);
    if (!utf8) {
        return true; /* the app offered no text under this type */
    }
    if (!size) {
        SDL_free(utf8);
        return true;
    }

    length = MultiByteToWideChar(CP_UTF8, 0, (const char *)utf8, (int)size, NULL, 0);
    if (length > 0) {
        memory = GlobalAlloc(GMEM_MOVEABLE, ((SIZE_T)length + 1) * sizeof(wchar_t));
        if (memory) {
            wide = (wchar_t *)GlobalLock(memory);
            if (wide) {
                MultiByteToWideChar(CP_UTF8, 0, (const char *)utf8, (int)size, wide, length);
                wide[length] = L'\0';
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory)) {
                    result = true; /* the system owns the block now */
                } else {
                    GlobalFree(memory);
                    SDL_SetError("Couldn't set clipboard data");
                }
            } else {
                GlobalFree(memory);
                SDL_SetError("Couldn't lock clipboard memory");
            }
        } else {
            SDL_SetError("Couldn't allocate clipboard memory");
        }
    }
    SDL_free(utf8);
    return result;
}

/* text/html is exposed to Windows apps as CF_HTML: a header of byte offsets,
 * then the markup. The offsets are ten-digit fixed width fields, so they can be
 * patched after the header is laid out. They are located by name rather than by
 * hardcoded positions -- an earlier version used positions I had counted by
 * hand, and every one was wrong, which overwrote "StartHTML" with digits. */
static void win32_patch_offset(char *buffer, const char *field, size_t value)
{
    char *at = SDL_strstr(buffer, field);
    if (at) {
        char digits[16];
        SDL_snprintf(digits, sizeof(digits), "%010lu", (unsigned long)value);
        SDL_memcpy(at + SDL_strlen(field), digits, 10);
    }
}

static bool win32_set_clipboard_html(const char *mime_type)
{
    static const char header[] =
        "Version:0.9\r\n"
        "StartHTML:" "0000000000" "\r\n"
        "EndHTML:" "0000000000" "\r\n"
        "StartFragment:" "0000000000" "\r\n"
        "EndFragment:" "0000000000" "\r\n";
    static const char open_marker[] = "<!--StartFragment-->";
    static const char close_marker[] = "<!--EndFragment-->";
    size_t size = 0;
    void *html;
    char *buffer;
    HGLOBAL memory = NULL;
    const size_t header_len = sizeof(header) - 1;
    const size_t open_len = sizeof(open_marker) - 1;
    const size_t close_len = sizeof(close_marker) - 1;
    const size_t fragment_start = header_len + open_len;
    size_t total;
    UINT format;
    bool result = false;

    html = SDLOP_GetInternalClipboardData(mime_type, &size);
    if (!html) {
        return true; /* the app offered no HTML under this type */
    }
    if (!size) {
        SDL_free(html);
        return true;
    }
    total = header_len + open_len + size + close_len;

    buffer = (char *)SDL_malloc(total);
    if (!buffer) {
        SDL_free(html);
        return SDL_SetError("Out of memory");
    }
    SDL_memcpy(buffer, header, header_len);
    SDL_memcpy(buffer + header_len, open_marker, open_len);
    SDL_memcpy(buffer + fragment_start, html, size);
    SDL_memcpy(buffer + fragment_start + size, close_marker, close_len);
    SDL_free(html);

    /* StartHTML is the offset of the HTML *context*, not of the "Version:"
     * header: the spec starts it at "<html>" (or at the fragment when that is
     * omitted, as here), and readers that take 0 literally try to parse the
     * Version/StartHTML lines as markup. */
    win32_patch_offset(buffer, "StartHTML:", fragment_start);
    win32_patch_offset(buffer, "EndHTML:", total);
    win32_patch_offset(buffer, "StartFragment:", fragment_start);
    win32_patch_offset(buffer, "EndFragment:", fragment_start + size);

    memory = GlobalAlloc(GMEM_MOVEABLE, total);
    if (memory) {
        void *locked = GlobalLock(memory);
        if (locked) {
            SDL_memcpy(locked, buffer, total);
            GlobalUnlock(memory);
            format = RegisterClipboardFormatW(L"HTML Format");
            if (format && SetClipboardData(format, memory)) {
                result = true;
                SDL_free(buffer);
                return result;
            }
            GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    } else {
        SDL_SetError("Out of memory");
    }
    SDL_free(buffer);
    if (!result) {
        /* A FALSE return with no error set leaves callers with a stale
         * SDL_GetError() from something unrelated. */
        SDL_SetError("Couldn't set clipboard data");
    }
    return result;
}

static bool win32_set_clipboard_image(const char *mime_type)
{
    size_t size = 0;
    void *data;
    HGLOBAL memory;
    bool result = false;

    data = SDLOP_GetInternalClipboardData(mime_type, &size);
    if (!data) {
        return true;
    }
    if (!size) {
        SDL_free(data);
        return true;
    }

    memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (memory) {
        void *locked = GlobalLock(memory);
        if (locked) {
            SDL_memcpy(locked, data, size);
            GlobalUnlock(memory);
            if (SetClipboardData(win32_clipboard_format_png(), memory)) {
                result = true;
            } else {
                GlobalFree(memory);
            }
        } else {
            GlobalFree(memory);
        }
    }
    SDL_free(data);
    return result;
}

static bool win32_SetClipboardData(SDLop_VideoDevice *device)
{
    const char *const *mime_types = NULL;
    size_t count = 0;
    size_t i;
    bool result = true;

    (void)device;
    mime_types = SDLOP_GetClipboardOfferMimeTypes(&count);
    if (!win32_open_clipboard()) {
        return false;
    }
    EmptyClipboard();
    for (i = 0; i < count; ++i) {
        const char *mime_type = mime_types[i];
        if (win32_is_text_mime_type(mime_type)) {
            /* Windows has one text flavour; offer the first text type only,
             * as SDL3 does. */
            if (!win32_set_clipboard_text(mime_type)) {
                result = false;
            }
            break;
        }
    }
    for (i = 0; i < count; ++i) {
        const char *mime_type = mime_types[i];
        if (SDL_strcmp(mime_type, "text/html") == 0) {
            if (!win32_set_clipboard_html(mime_type)) {
                result = false;
            }
        } else if (SDL_strcmp(mime_type, "image/png") == 0) {
            if (!win32_set_clipboard_image(mime_type)) {
                result = false;
            }
        }
    }
    CloseClipboard();
    return result;
}

/* CF_TEXT is the ANSI flavour. Wine and Windows both still expose it, and an
 * app that only understands CF_TEXT has nothing otherwise. */
static char *win32_clipboard_text_from_ansi(HANDLE handle)
{
    const char *ansi = (const char *)GlobalLock(handle);
    char *utf8 = NULL;

    if (ansi) {
        const int needed = MultiByteToWideChar(CP_ACP, 0, ansi, -1, NULL, 0);
        if (needed > 0) {
            wchar_t *wide = (wchar_t *)SDL_malloc(((size_t)needed) * sizeof(wchar_t));
            if (wide &&
                MultiByteToWideChar(CP_ACP, 0, ansi, -1, wide, needed) > 0) {
                const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
                if (bytes > 0) {
                    utf8 = (char *)SDL_malloc((size_t)bytes);
                    if (utf8) {
                        WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, bytes, NULL, NULL);
                    }
                }
            }
            SDL_free(wide);
        }
        GlobalUnlock(handle);
    }
    return utf8;
}

static void *win32_GetClipboardData(SDLop_VideoDevice *device, const char *mime_type, size_t *size)
{
    void *data = NULL;

    (void)device;
    *size = 0;
    if (win32_is_text_mime_type(mime_type)) {
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            if (win32_open_clipboard()) {
                HANDLE handle = GetClipboardData(CF_UNICODETEXT);
                if (handle) {
                    const wchar_t *wide = (const wchar_t *)GlobalLock(handle);
                    if (wide) {
                        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
                        if (bytes > 0) {
                            char *utf8 = (char *)SDL_malloc((size_t)bytes);
                            if (utf8) {
                                WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, bytes, NULL, NULL);
                                *size = SDL_strlen(utf8);
                                data = utf8;
                            }
                        }
                        GlobalUnlock(handle);
                    }
                }
                CloseClipboard();
            }
        } else if (IsClipboardFormatAvailable(CF_TEXT)) {
            if (win32_open_clipboard()) {
                HANDLE handle = GetClipboardData(CF_TEXT);
                if (handle) {
                    char *utf8 = win32_clipboard_text_from_ansi(handle);
                    if (utf8) {
                        *size = SDL_strlen(utf8);
                        data = utf8;
                    }
                }
                CloseClipboard();
            }
        }
        return data;
    }
    if (SDL_strcmp(mime_type, "image/png") == 0) {
        const UINT format = win32_clipboard_format_png();
        if (format && IsClipboardFormatAvailable(format) && win32_open_clipboard()) {
            HANDLE handle = GetClipboardData(format);
            if (handle) {
                const size_t bytes = (size_t)GlobalSize(handle);
                const void *locked = GlobalLock(handle);
                if (locked) {
                    data = SDL_malloc(bytes);
                    if (data) {
                        SDL_memcpy(data, locked, bytes);
                        *size = bytes;
                    }
                    GlobalUnlock(handle);
                }
            }
            CloseClipboard();
        }
        return data;
    }
    return SDLOP_GetInternalClipboardData(mime_type, size);
}

static bool win32_HasClipboardData(SDLop_VideoDevice *device, const char *mime_type)
{
    (void)device;
    if (win32_is_text_mime_type(mime_type)) {
        return (IsClipboardFormatAvailable(CF_UNICODETEXT) ||
                IsClipboardFormatAvailable(CF_TEXT)) ? true : false;
    }
    if (SDL_strcmp(mime_type, "image/png") == 0) {
        const UINT format = win32_clipboard_format_png();
        return (format && IsClipboardFormatAvailable(format)) ? true : false;
    }
    return SDLOP_HasInternalClipboardData(mime_type);
}

SDLop_VideoDevice SDLop_windows_device = {
    .name = "windows",
    .Init = win32_Init,
    .Quit = win32_Quit,
    .CreateWindow = win32_CreateWindow,
    .DestroyWindow = win32_DestroyWindow,
    .ShowWindow = win32_ShowWindow,
    .HideWindow = win32_HideWindow,
    .SetWindowTitle = win32_SetWindowTitle,
    .SetWindowSize = win32_SetWindowSize,
    .SetWindowPosition = win32_SetWindowPosition,
    .MinimizeWindow = win32_MinimizeWindow,
    .MaximizeWindow = win32_MaximizeWindow,
    .RestoreWindow = win32_RestoreWindow,
    .SetWindowFullscreen = win32_SetWindowFullscreen,
    .RaiseWindow = win32_RaiseWindow,
    .SetWindowClearColor = win32_SetWindowClearColor,
    .SetWindowRelativeMouseMode = win32_SetWindowRelativeMouseMode,
    .PumpEvents = win32_PumpEvents,
    .GetEventFD = win32_GetEventFD,
    .CreateWindowFramebuffer = win32_CreateWindowFramebuffer,
    .UpdateWindowFramebuffer = win32_UpdateWindowFramebuffer,
    .DestroyWindowFramebuffer = win32_DestroyWindowFramebuffer,

    /* OpenGL and Vulkan live in their own files */
    .GL_CreateContext = SDLOP_Win32GLCreateContext,
    .GL_MakeCurrent = SDLOP_Win32GLMakeCurrent,
    .GL_SwapBuffers = SDLOP_Win32GLSwapBuffers,
    .GL_DeleteContext = SDLOP_Win32GLDeleteContext,
    .GL_GetProcAddress = SDLOP_Win32GLGetProcAddress,
    .GL_SetSwapInterval = SDLOP_Win32GLSetSwapInterval,
    .GL_GetSwapInterval = SDLOP_Win32GLGetSwapInterval,
    .Vulkan_CreateSurface = SDLOP_Win32VulkanCreateSurface,
    .Vulkan_GetInstanceExtensions = SDLOP_Win32VulkanGetInstanceExtensions,
    .Vulkan_GetPresentationSupport = SDLOP_Win32VulkanGetPresentationSupport,
    .SetClipboardData = win32_SetClipboardData,
    .GetClipboardData = win32_GetClipboardData,
    .HasClipboardData = win32_HasClipboardData,

    .SetWindowMinimumSize = win32_SetWindowMinimumSize,
    .SetWindowMaximumSize = win32_SetWindowMaximumSize,
    .GetWindowBordersSize = win32_GetWindowBordersSize,
    .SetWindowOpacity = win32_SetWindowOpacity,
    .SetWindowAlwaysOnTop = win32_SetWindowAlwaysOnTop,
    .SetWindowBordered = win32_SetWindowBordered,
    .SetWindowResizable = win32_SetWindowResizable,
    .SetWindowIcon = win32_SetWindowIcon,
    .SetWindowMouseGrab = win32_SetWindowMouseGrab,
    .SetWindowKeyboardGrab = win32_SetWindowKeyboardGrab,
    .SetWindowMouseRect = win32_SetWindowMouseRect,
    .CreateCursor = win32_CreateCursor,
    .FreeCursor = win32_FreeCursor,
    .ShowCursor = win32_ShowCursor,
    .SuspendScreenSaver = win32_SuspendScreenSaver,
    .CaptureMouse = win32_CaptureMouse,
    .SetWindowHitTest = win32_SetWindowHitTest,
    .UpdateWindowShape = win32_UpdateWindowShape,
    .UpdateTextInputArea = win32_UpdateTextInputArea,
    .GetWindowICCProfile = win32_GetWindowICCProfile,
    .ShowWindowSystemMenu = win32_ShowWindowSystemMenu,
    .SetWindowAspectRatio = win32_SetWindowAspectRatio,
    .WarpMouse = win32_WarpMouse,
    .WarpMouseGlobal = win32_WarpMouseGlobal,
    .GetGlobalMouseState = win32_GetGlobalMouseState,
    .FlashWindow = win32_FlashWindow,
    .SyncWindow = win32_SyncWindow,
    .SetWindowFocusable = win32_SetWindowFocusable,
    .SetWindowParent = win32_SetWindowParent,
    .SetWindowModal = win32_SetWindowModal,
    .GetSystemTheme = win32_GetSystemTheme,
};

#endif /* _WIN32 */
