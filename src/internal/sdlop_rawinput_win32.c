/*
  SDLop - asyncinput-style raw input worker for Windows.

  The Linux worker (src/internal/sdlop_evdev.c) reads /dev/input/event* on a
  background thread and publishes each kernel event to three consumers:

    1. user callbacks (invoked right there - lowest latency)
    2. an SPSC ring drained by SDL_PumpEvents()
    3. an SPSC ring drained by SDLop_PollRawEvents()

  Windows has no evdev; the equivalent source is Raw Input. This file is the
  same worker with a Win32 producer: a hidden window on a dedicated thread,
  registered RIDEV_INPUTSINK so input keeps arriving while the application is
  unfocused, each packet read with GetRawInputData().

  GetRawInputBuffer() would batch a burst into one syscall, which is what
  high-polling-rate mice want (SDL issue #8756), but Wine returns 0 from it in
  every configuration measured here, so it is unusable for testing. Per-message
  GetRawInputData() is what every Windows version handles identically.

  Raw Input yields Windows-native data, so the translation to SDLop's
  evdev-shaped event codes happens here:

    - mouse motion   -> SDLop_EV_REL  SDLop_REL_X / SDLop_REL_Y
    - wheel          -> SDLop_EV_REL  SDLop_REL_WHEEL / SDLop_REL_HWHEEL,
                        emitted in 120-unit notches, the same units evdev uses
                        for a detent, so a consumer treating 120 as one click
                        works unchanged
    - mouse buttons  -> SDLop_EV_KEY  SDLop_BTN_LEFT..SDLOP_BTN_EXTRA
    - keyboard       -> SDLop_EV_KEY  SDLop_KEY_* (evdev codes)

  Keyboard packets carry a hardware scan code. It goes through the project's
  existing scan -> SDL_Scancode table (which already understands the E0
  prefix), then an SDL_Scancode -> evdev index inverted from the generated
  evdev table at startup. No second hand-written key table.

  Timestamps come from QueryPerformanceCounter, offset onto the origin of
  SDLOP_MonotonicNS(), so an event's timestamp_ns can be compared against
  SDL_GetTicksNS() directly, as the evdev worker's can.

  This worker is opt-in (SDLOP_RAW_INPUT=1, or SDL_SetHint): on Windows the
  window message path is the default source and already delivers input, so
  enabling raw input is a deliberate trade of one thread for lower latency
  and for left/right modifier distinction.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#ifdef _WIN32

#include <SDL3/SDLop.h>
#include "internal/sdlop_ring.h"
#include "internal/scancode_evdev.h"
#include "internal/sdlop_win32_keymap.h"

#include <windows.h>

#define SDLOP_WIN_SDL_RING_SIZE  2048 /* power of two */
#define SDLOP_WIN_POLL_RING_SIZE 1024 /* power of two */
#define SDLOP_WIN_MAX_CALLBACKS  8

#define SDLOP_USAGE_PAGE_DESKTOP 0x01
#define SDLOP_USAGE_MOUSE        0x02
#define SDLOP_USAGE_KEYBOARD     0x06

/* evdev button codes, mirrored from SDLop.h */
#define SDLOP_BTN_MIDDLE_BIT (1u << 0)
#define SDLOP_BTN_RIGHT_BIT  (1u << 1)
#define SDLOP_BTN_LEFT_BIT   (1u << 2)

#ifndef WM_INPUT
#define WM_INPUT 0x00FF
#endif

/* NEXTRAWINPUTBLOCK() aligns to a QWORD, and MinGW's headers do not define
 * the typedef outside of a few SDK configurations (SDL3 defines it too). */
typedef unsigned long long QWORD;

typedef struct SDLOP_WinCallbackEntry
{
    SDLop_RawEventCallback cb;
    void *userdata;
} SDLOP_WinCallbackEntry;

typedef struct SDLOP_WinCallbackSet
{
    int count;
    SDLOP_WinCallbackEntry entries[SDLOP_WIN_MAX_CALLBACKS];
} SDLOP_WinCallbackSet;

typedef struct SDLOP_WinRawState
{
    volatile LONG running;
    volatile LONG stop_requested;
    HANDLE thread;
    DWORD thread_id;
    HWND hwnd; /* message-only window, created on the worker thread */

    /* rings: single producer (worker), single consumer (main thread) */
    _Alignas(SDLOP_CACHELINE) SDLop_RawEvent sdl_ring_storage[SDLOP_WIN_SDL_RING_SIZE];
    SDLOP_Ring sdl_ring;
    _Alignas(SDLOP_CACHELINE) SDLop_RawEvent poll_ring_storage[SDLOP_WIN_POLL_RING_SIZE];
    SDLOP_Ring poll_ring;

    atomic_uint_least64_t event_count;

    /* callbacks: published by the app thread, read by the worker.
     *
     * A read/write lock rather than an atomic pointer swap: the worker holds
     * it shared while walking the list, so the publisher can free the old set
     * once nobody is inside. The Linux worker publishes lock-free and leaks
     * each superseded set instead (bounded, but a leak), and the cost of
     * correctness here is one uncontended lock acquisition per event. */
    SDLOP_WinCallbackSet *callbacks;
    SRWLOCK cb_lock;

    Uint64 qpc_freq;
    Uint64 qpc_base;
    Uint64 mono_base;

    int wheel_acc;  /* per-axis wheel remainder, in 120-unit notches */
    int hwheel_acc;

    /* Indexed by evdev key code, which reaches SDLOP_EVDEV_KEY_MAX (767),
     * not by scan code (0..255). Sized from the table so it cannot drift. */
    Uint8 key_down[SDLOP_EVDEV_KEY_MAX + 1]; /* 1 = held; suppresses repeats */
} SDLOP_WinRawState;

static SDLOP_WinRawState winraw;

/* ------------------------------------------------------------------ */
/* Lookup table: SDL_Scancode -> evdev key code                        */
/* ------------------------------------------------------------------ */

static Uint16 scancode_to_evdev[SDL_SCANCODE_COUNT];

static void build_scancode_table(void)
{
    static bool built;
    Uint16 evdev;

    if (built) {
        return;
    }
    SDL_zero(scancode_to_evdev);
    for (evdev = 0; evdev <= SDLOP_EVDEV_KEY_MAX; evdev++) {
        const SDL_Scancode sc = (SDL_Scancode)sdlop_evdev_to_scancode[evdev];
        if (sc == SDL_SCANCODE_UNKNOWN || sc >= SDL_SCANCODE_COUNT) {
            continue;
        }
        /* Lowest evdev code wins, which is the plain (non-E0) form of a key
         * that the table lists twice. */
        if (!scancode_to_evdev[sc]) {
            scancode_to_evdev[sc] = evdev;
        }
    }
    built = true;
}

/* ------------------------------------------------------------------ */
/* Wheels: accumulate Windows' signed deltas into evdev's notch units  */
/* ------------------------------------------------------------------ */

/* Windows reports WHEEL_DELTA (120) per detent in wheel units already for
 * legacy messages, but RAWMOUSE gives 120 per notch only for regular mice;
 * high-resolution wheels report smaller values. Re-emitting 120 per notch
 * keeps the consumer-visible contract identical to evdev. */
static int wheel_notches(int *acc, int delta, int unit)
{
    int n;

    *acc += delta;
    if (*acc >= unit || *acc <= -unit) {
        n = *acc / unit; /* truncates toward zero, good for both signs */
        *acc -= n * unit;
        return n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Publishing                                                          */
/* ------------------------------------------------------------------ */

static void dispatch_callbacks(const SDLop_RawEvent *event)
{
    SDLOP_WinCallbackSet *set = winraw.callbacks;
    int i;

    if (!set) {
        return;
    }
    /* Shared: many worker-thread dispatches may overlap; the publisher takes
     * it exclusive and only then frees the superseded set. */
    AcquireSRWLockShared(&winraw.cb_lock);
    set = winraw.callbacks;
    if (!set) {
        ReleaseSRWLockShared(&winraw.cb_lock);
        return;
    }
    for (i = 0; i < set->count; i++) {
        set->entries[i].cb(event, set->entries[i].userdata);
    }
    ReleaseSRWLockShared(&winraw.cb_lock);
}

static void publish(Uint32 device, Uint16 type, Uint16 code, Sint32 value, Uint64 ts)
{
    SDLop_RawEvent ev;

    SDL_zero(ev);
    ev.device = device;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ev.timestamp_ns = ts;

    SDLOP_RingPush(&winraw.sdl_ring, &ev);
    SDLOP_RingPush(&winraw.poll_ring, &ev);
    dispatch_callbacks(&ev);
    atomic_fetch_add_explicit(&winraw.event_count, 1, memory_order_relaxed);
}

/* QueryPerformanceCounter -> the SDLOP_MonotonicNS() origin */
static Uint64 now_ns(void)
{
    LARGE_INTEGER qpc;
    Uint64 elapsed_ns;

    QueryPerformanceCounter(&qpc);
    if (!winraw.qpc_freq) {
        return SDLOP_MonotonicNS();
    }
    elapsed_ns = ((Uint64)qpc.QuadPart - winraw.qpc_base) * 1000000000ull / winraw.qpc_freq;
    return winraw.mono_base + elapsed_ns;
}

/* ------------------------------------------------------------------ */
/* Mouse                                                               */
/* ------------------------------------------------------------------ */

static void handle_mouse(const RAWMOUSE *raw, Uint32 device, Uint64 ts)
{
    const int dx = (int)raw->lLastX;
    const int dy = (int)raw->lLastY;

    /* A tablet or an RDP session reports absolute positions in this stream.
     * Passing them on as deltas would be garbage; the window message path
     * already covers absolute pointing. */
    if ((raw->usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
        if (dx) {
            publish(device, SDLop_EV_REL, SDLop_REL_X, dx, ts);
        }
        if (dy) {
            publish(device, SDLop_EV_REL, SDLop_REL_Y, dy, ts);
        }
    }

    if (raw->usButtonFlags) {
        static const struct {
            USHORT flag;
            Uint16 code;
            int down;
        } buttons[] = {
            { RI_MOUSE_LEFT_BUTTON_DOWN, SDLop_BTN_LEFT, 1 },
            { RI_MOUSE_LEFT_BUTTON_UP, SDLop_BTN_LEFT, 0 },
            { RI_MOUSE_RIGHT_BUTTON_DOWN, SDLop_BTN_RIGHT, 1 },
            { RI_MOUSE_RIGHT_BUTTON_UP, SDLop_BTN_RIGHT, 0 },
            { RI_MOUSE_MIDDLE_BUTTON_DOWN, SDLop_BTN_MIDDLE, 1 },
            { RI_MOUSE_MIDDLE_BUTTON_UP, SDLop_BTN_MIDDLE, 0 },
            { RI_MOUSE_BUTTON_4_DOWN, SDLop_BTN_SIDE, 1 },
            { RI_MOUSE_BUTTON_4_UP, SDLop_BTN_SIDE, 0 },
            { RI_MOUSE_BUTTON_5_DOWN, SDLop_BTN_EXTRA, 1 },
            { RI_MOUSE_BUTTON_5_UP, SDLop_BTN_EXTRA, 0 },
        };
        size_t i;
        for (i = 0; i < SDL_arraysize(buttons); i++) {
            if (raw->usButtonFlags & buttons[i].flag) {
                publish(device, SDLop_EV_KEY, buttons[i].code, buttons[i].down, ts);
            }
        }
    }

    /* Published as a notch count, which is what evdev's REL_WHEEL carries and
     * what SDLOP_SendMouseWheel() takes -- not the 120-unit delta. */
    if (raw->usButtonFlags & RI_MOUSE_WHEEL) {
        const int n = wheel_notches(&winraw.wheel_acc, (short)raw->usButtonData, WHEEL_DELTA);
        if (n) {
            publish(device, SDLop_EV_REL, SDLop_REL_WHEEL, n, ts);
        }
    }
    if (raw->usButtonFlags & RI_MOUSE_HWHEEL) {
        const int n = wheel_notches(&winraw.hwheel_acc, (short)raw->usButtonData, WHEEL_DELTA);
        if (n) {
            publish(device, SDLop_EV_REL, SDLop_REL_HWHEEL, n, ts);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

static void handle_keyboard(const RAWKEYBOARD *raw, Uint32 device, Uint64 ts)
{
    USHORT make = raw->MakeCode;
    USHORT scan;
    SDL_Scancode sc;
    Uint16 evdev;
    int down;

    if (!make && raw->VKey) {
        /* Some devices report only a virtual key. */
        make = LOWORD(MapVirtualKeyW(raw->VKey, MAPVK_VK_TO_VSC_EX));
    }
    if (!make) {
        return;
    }
    if ((raw->Flags & RI_KEY_E0) && make == 0x2A) {
        return; /* the 0xE02A prefix carries no key of its own */
    }

    scan = (USHORT)((raw->Flags & RI_KEY_E0) ? (make | 0xE000) : make);
    sc = SDLOP_Win32ScancodeFromScan(scan);
    if (sc == SDL_SCANCODE_UNKNOWN || sc >= SDL_SCANCODE_COUNT) {
        return;
    }
    evdev = scancode_to_evdev[sc];
    if (!evdev) {
        return;
    }

    switch (raw->Message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        down = 1;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        down = 0;
        break;
    default:
        down = (raw->Flags & RI_KEY_BREAK) ? 0 : 1;
        break;
    }

    /* Raw Input repeats the key for a held key. Report the transition only:
     * a consumer that counts press events must not see N of them, and the
     * window message path handles autorepeat for the SDL queue. */
    if (down && winraw.key_down[evdev]) {
        return;
    }
    winraw.key_down[evdev] = (Uint8)down;

    publish(device, SDLop_EV_KEY, evdev, down, ts);
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

static UINT raw_win_count; /* packets seen, for diagnostics */

/* One WM_INPUT carries exactly one packet for GetRawInputData(); a single
 * stack buffer is enough and RID_INPUT never needs a large allocation. */
static void handle_one_input(HRAWINPUT handle)
{
    BYTE stack[512];
    RAWINPUT *input = (RAWINPUT *)stack;
    UINT size = sizeof(stack);
    UINT got;

    got = GetRawInputData(handle, RID_INPUT, input, &size, sizeof(RAWINPUTHEADER));
    /* On success GetRawInputData() returns the bytes it wrote and leaves
     * cbSize alone; comparing the return value against cbSize rejects every
     * packet. Only the failure return is an error. */
    if (got == (UINT)-1 || got == 0) {
        return;
    }
    if (input->header.dwType == RIM_TYPEMOUSE) {
        handle_mouse(&input->data.mouse, (Uint32)(UINT_PTR)input->header.hDevice, now_ns());
    } else if (input->header.dwType == RIM_TYPEKEYBOARD) {
        handle_keyboard(&input->data.keyboard, (Uint32)(UINT_PTR)input->header.hDevice, now_ns());
    }
    raw_win_count++;
}

static LRESULT CALLBACK raw_win_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_INPUT) {
        /* GetRawInputData(), not GetRawInputBuffer(): the buffer API batches
         * one syscall per burst, which matters for 8 kHz mice, but Wine does
         * not implement it (returns 0) and one message per packet is what
         * every Windows version handles identically. */
        handle_one_input((HRAWINPUT)lparam);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static DWORD WINAPI raw_win_thread(LPVOID param)
{
    WNDCLASSW wc;
    RAWINPUTDEVICE devices[2];
    UINT count = 0;
    MSG msg;
    LARGE_INTEGER freq;

    (void)param;

    QueryPerformanceFrequency(&freq);
    winraw.qpc_freq = (Uint64)freq.QuadPart;
    {
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        winraw.qpc_base = (Uint64)qpc.QuadPart;
    }
    winraw.mono_base = SDLOP_MonotonicNS();

    SDL_zero(wc);
    wc.lpfnWndProc = raw_win_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SDLopRawInput";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 1;
    }

    /* A hidden window parked off-screen, not a message-only one.
     *
     * HWND_MESSAGE would be the tidier choice, but it is not reliably
     * equivalent: Wine's message-only windows receive no raw input at all
     * (measured: 0 WM_INPUT for message-only vs 10 for a hidden window), and
     * there is no reason to assume a production driver never has a similar
     * gap. WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW keeps it out of the taskbar and
     * out of the alt-tab list, which is the visible part of "message-only",
     * and RIDEV_INPUTSINK makes it receive input whether or not it has
     * focus, so being hidden costs nothing. */
    winraw.hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                  L"SDLopRawInput", L"SDLopRawInput", WS_POPUP,
                                  -32000, -32000, 64, 64,
                                  NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!winraw.hwnd) {
        return 1;
    }

    SDL_zeroa(devices);
    devices[count].usUsagePage = SDLOP_USAGE_PAGE_DESKTOP;
    devices[count].usUsage = SDLOP_USAGE_MOUSE;
    devices[count].dwFlags = RIDEV_INPUTSINK;
    devices[count].hwndTarget = winraw.hwnd;
    count++;

    devices[count].usUsagePage = SDLOP_USAGE_PAGE_DESKTOP;
    devices[count].usUsage = SDLOP_USAGE_KEYBOARD;
    devices[count].dwFlags = RIDEV_INPUTSINK;
    devices[count].hwndTarget = winraw.hwnd;
    count++;

    if (!RegisterRawInputDevices(devices, count, sizeof(devices[0]))) {
        DestroyWindow(winraw.hwnd);
        winraw.hwnd = NULL;
        return 1;
    }

    /* Lower latency than the default, but not time-critical: this thread must
     * never starve the rest of the process. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    InterlockedExchange(&winraw.running, 1);

    while (!InterlockedCompareExchange(&winraw.stop_requested, 0, 0)) {
        const DWORD got = MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_RAWINPUT);
        if (got == WAIT_OBJECT_0) {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_INPUT) {
                    handle_one_input((HRAWINPUT)msg.lParam);
                } else {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        }
    }

    InterlockedExchange(&winraw.running, 0);

    /* Deregister before the target window disappears. */
    {
        RAWINPUTDEVICE remove[2];
        SDL_zeroa(remove);
        remove[0].usUsagePage = SDLOP_USAGE_PAGE_DESKTOP;
        remove[0].usUsage = SDLOP_USAGE_MOUSE;
        remove[0].dwFlags = RIDEV_REMOVE;
        remove[0].hwndTarget = NULL; /* RIDEV_REMOVE requires NULL */
        remove[1].usUsagePage = SDLOP_USAGE_PAGE_DESKTOP;
        remove[1].usUsage = SDLOP_USAGE_KEYBOARD;
        remove[1].dwFlags = RIDEV_REMOVE;
        remove[1].hwndTarget = NULL;
        RegisterRawInputDevices(remove, 2, sizeof(remove[0]));
    }
    DestroyWindow(winraw.hwnd);
    winraw.hwnd = NULL;
    UnregisterClassW(L"SDLopRawInput", GetModuleHandleW(NULL));

    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

bool SDLOP_RawInputInit(void)
{
    if (winraw.running) {
        return true;
    }
    if (!SDLOP_GetStringBoolean(SDL_GetHint(SDLOP_HINT_RAW_INPUT), false)) {
        return SDL_SetError("raw input is disabled (set %s=1 to enable)", SDLOP_HINT_RAW_INPUT);
    }
    if (SDLOP_GetStringBoolean(SDL_GetHint(SDLOP_HINT_DISABLE_RAW_INPUT), false)) {
        return SDL_SetError("raw input is disabled");
    }

    build_scancode_table();

    SDL_zero(winraw);
    SDLOP_RingInit(&winraw.sdl_ring, winraw.sdl_ring_storage, SDLOP_WIN_SDL_RING_SIZE, sizeof(SDLop_RawEvent));
    SDLOP_RingInit(&winraw.poll_ring, winraw.poll_ring_storage, SDLOP_WIN_POLL_RING_SIZE, sizeof(SDLop_RawEvent));
    atomic_store(&winraw.event_count, 0);
    InitializeSRWLock(&winraw.cb_lock);
    winraw.callbacks = NULL;

    InterlockedExchange(&winraw.stop_requested, 0);
    winraw.thread = CreateThread(NULL, 0, raw_win_thread, NULL, 0, &winraw.thread_id);
    if (!winraw.thread) {
        return SDL_SetError("could not start the raw input thread");
    }

    /* Wait for the worker, so RawInputAvailable() is truthful on return. The
     * thread sets running after registration, or exits without doing so. */
    {
        int spins = 0;
        while (!winraw.running && spins < 2000) {
            if (WaitForSingleObject(winraw.thread, 1) == WAIT_OBJECT_0) {
                CloseHandle(winraw.thread);
                winraw.thread = NULL;
                return SDL_SetError("could not register for raw input");
            }
            spins++;
        }
    }
    if (!winraw.running) {
        return false;
    }
    return true;
}

void SDLOP_RawInputQuit(void)
{
    if (!winraw.thread) {
        return;
    }
    InterlockedExchange(&winraw.stop_requested, 1);
    WaitForSingleObject(winraw.thread, INFINITE);
    CloseHandle(winraw.thread);
    winraw.thread = NULL;
    winraw.running = 0;
    winraw.hwnd = NULL;

    /* The worker is joined by now, so nobody can be inside the list. */
    SDL_free((void *)winraw.callbacks);
    winraw.callbacks = NULL;
}

/* Feed the ring into the SDL event queue. The worker sees every device that
 * reports to the sink window, so the filtering the window-message path gets
 * for free -- only input aimed at us -- has to happen here: keys while one of
 * our windows has keyboard focus, the pointer while it is over one of our
 * windows or relative mode holds it. Releases always pass, so nothing sticks
 * if focus is lost mid-press. */
static Uint8 mouse_button_from_evdev(Uint16 code)
{
    switch (code) {
    case SDLop_BTN_LEFT:   return SDL_BUTTON_LEFT;
    case SDLop_BTN_RIGHT:  return SDL_BUTTON_RIGHT;
    case SDLop_BTN_MIDDLE: return SDL_BUTTON_MIDDLE;
    case SDLop_BTN_SIDE:   return SDL_BUTTON_X1;
    case SDLop_BTN_EXTRA:  return SDL_BUTTON_X2;
    default:               return 0;
    }
}

static bool win_relative_active(void)
{
    return sdlop.relative_mode_window && sdlop.keyboard_focus == sdlop.relative_mode_window;
}

static bool win_pointer_wanted(void)
{
    return !sdlop.pointer_grabbed_elsewhere && (sdlop.mouse_focus || win_relative_active());
}

static bool win_keyboard_wanted(void)
{
    return sdlop.keyboard_focus && !sdlop.keyboard_grabbed_elsewhere;
}

static void win_pump_event(const SDLop_RawEvent *ev)
{
    switch (ev->type) {
    case SDLop_EV_KEY: {
        if (ev->code >= SDLop_BTN_LEFT && ev->code <= SDLop_BTN_EXTRA) {
            const Uint8 btn = mouse_button_from_evdev(ev->code);
            if (!btn) {
                return;
            }
            if (!win_pointer_wanted() &&
                !(ev->value == 0 && (sdlop.mouse_buttons & SDL_BUTTON_MASK(btn)))) {
                return;
            }
            SDLOP_SendMouseButton(ev->value != 0, btn, SDLOP_NO_POS, SDLOP_NO_POS, ev->timestamp_ns);
            return;
        }
        {
            SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
            if (ev->code <= SDLOP_EVDEV_KEY_MAX) {
                sc = (SDL_Scancode)sdlop_evdev_to_scancode[ev->code];
            }
            if (sc == SDL_SCANCODE_UNKNOWN) {
                return;
            }
            if (!win_keyboard_wanted() && !(ev->value == 0 && sdlop.keystate[sc])) {
                return;
            }
            /* The worker reports transitions only, so never a repeat. */
            SDLOP_SendKeyboardKey(ev->value != 0, false, sc, ev->code, ev->timestamp_ns);
        }
        return;
    }

    case SDLop_EV_REL:
        if (!win_pointer_wanted()) {
            return;
        }
        switch (ev->code) {
        case SDLop_REL_X:
        case SDLop_REL_Y: {
            /* Pair the two axes into one motion event when the ring has them
             * adjacent, but never hold an axis back: a mouse that reports only
             * X (or only Y) for a while must still move. */
            SDLOP_SendMouseMotion(SDLOP_NO_POS, SDLOP_NO_POS,
                                  ev->code == SDLop_REL_X ? (float)ev->value : 0.0f,
                                  ev->code == SDLop_REL_Y ? (float)ev->value : 0.0f,
                                  ev->timestamp_ns);
            return;
        }
        case SDLop_REL_WHEEL:
            SDLOP_SendMouseWheel(0.0f, (float)ev->value, ev->timestamp_ns);
            return;
        case SDLop_REL_HWHEEL:
            SDLOP_SendMouseWheel((float)ev->value, 0.0f, ev->timestamp_ns);
            return;
        default:
            return;
        }

    default:
        return;
    }
}

void SDLOP_RawInputPump(void)
{
    SDLop_RawEvent ev;
    int n = 0;

    if (!winraw.running) {
        return;
    }
    while (SDLOP_RingPop(&winraw.sdl_ring, &ev)) {
        win_pump_event(&ev);
        if (++n >= SDLOP_WIN_SDL_RING_SIZE) {
            break;
        }
    }
    if (n) {
        SDLOP_KeyboardProcessRepeats();
    }
}

bool SDLOP_RawInputIsSource(void)
{
    return winraw.running != 0;
}

bool SDLOP_RawInputHasKeyboard(void)
{
    return winraw.running != 0;
}

void SDLOP_RawInputAttachConsole(int console_fd)
{
    (void)console_fd; /* console VTs are a Linux concept */
}

void SDLOP_RawInputDetachConsole(void)
{
}

void SDLOP_RawInputSetRepeatInfo(Uint32 delay_ms, Uint32 interval_ms)
{
    (void)delay_ms;
    (void)interval_ms;
}

/* ------------------------------------------------------------------ */
/* Public raw-event API                                                */
/* ------------------------------------------------------------------ */

int SDLop_PollRawEvents(SDLop_RawEvent *events, int max_events)
{
    int n = 0;

    if (!events || max_events <= 0) {
        return -1;
    }
    while (n < max_events && SDLOP_RingPop(&winraw.poll_ring, &events[n])) {
        n++;
    }
    return n;
}

bool SDLop_RegisterRawEventCallback(SDLop_RawEventCallback cb, void *userdata)
{
    SDLOP_WinCallbackSet *old;
    SDLOP_WinCallbackSet *set;
    int count;

    if (!cb) {
        return SDL_SetError("NULL callback");
    }
    AcquireSRWLockExclusive(&winraw.cb_lock);
    old = winraw.callbacks;
    count = old ? old->count : 0;
    if (count >= SDLOP_WIN_MAX_CALLBACKS) {
        ReleaseSRWLockExclusive(&winraw.cb_lock);
        return SDL_SetError("Too many raw event callbacks (max %d)", SDLOP_WIN_MAX_CALLBACKS);
    }
    set = (SDLOP_WinCallbackSet *)SDL_calloc(1, sizeof(*set));
    if (!set) {
        ReleaseSRWLockExclusive(&winraw.cb_lock);
        return SDL_OutOfMemory();
    }
    if (old) {
        *set = *old;
    }
    set->entries[count].cb = cb;
    set->entries[count].userdata = userdata;
    set->count = count + 1;
    winraw.callbacks = set;
    ReleaseSRWLockExclusive(&winraw.cb_lock);
    SDL_free(old);
    return true;
}

void SDLop_UnregisterRawEventCallback(SDLop_RawEventCallback cb)
{
    SDLOP_WinCallbackSet *old;
    SDLOP_WinCallbackSet *set;
    int i;
    int n = 0;

    if (!cb) {
        return;
    }
    AcquireSRWLockExclusive(&winraw.cb_lock);
    old = winraw.callbacks;
    if (!old) {
        ReleaseSRWLockExclusive(&winraw.cb_lock);
        return;
    }
    set = (SDLOP_WinCallbackSet *)SDL_calloc(1, sizeof(*set));
    if (!set) {
        ReleaseSRWLockExclusive(&winraw.cb_lock);
        return;
    }
    for (i = 0; i < old->count; i++) {
        if (old->entries[i].cb != cb) {
            set->entries[n++] = old->entries[i];
        }
    }
    set->count = n;
    winraw.callbacks = set;
    ReleaseSRWLockExclusive(&winraw.cb_lock);
    SDL_free(old);
}

/* The core calls this on focus/grab transitions to drop a half-finished
 * pointer frame. Win32 Raw Input has no such frame: each packet carries a
 * complete relative delta and every axis it moved is published immediately, so
 * a dropped or partial frame cannot occur the way it can with evdev's
 * axis-until-SYN_REPORT batching. Nothing to reset. */
/* The core asks whether the raw path may deliver keys before posting its own
 * keyboard events. On Win32 the raw worker only runs when the app opted in
 * (SDLOP_RAW_INPUT=1), and when it is not running the window-message path owns
 * the keyboard, so this is always true. */
/* Real counters, not placeholders: the SPSC rings already track producer-side
 * drops, and event_count is the same atomic the evdev worker publishes. The
 * contract matches the Linux implementation -- independently sampled fields,
 * not a transactional snapshot. */
bool SDLop_GetRawInputStats(SDLop_RawInputStats *stats)
{
    if (!stats) {
        return SDL_SetError("NULL stats pointer");
    }
    stats->events_seen = atomic_load_explicit(&winraw.event_count, memory_order_relaxed);
    stats->sdl_ring_dropped = SDLOP_RingDropped(&winraw.sdl_ring);
    stats->poll_ring_dropped = SDLOP_RingDropped(&winraw.poll_ring);
    return true;
}

bool SDLOP_RawInputKeyboardAllowed(void)
{
    return true;
}

void SDLOP_RawInputResetPointerFrame(void)
{
    /* intentionally empty */
}

bool SDLop_RawInputAvailable(void)
{
    return winraw.running != 0;
}

Uint64 SDLop_GetRawEventCount(void)
{
    return atomic_load_explicit(&winraw.event_count, memory_order_relaxed);
}

#endif /* _WIN32 */
