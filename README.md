# SDLop

**A lean, fast, SDL3-compatible windowing + input library.**
Think `WIN32_LEAN_AND_MEAN`, but for SDL3: the same API, the same ABI, none of
the modules you don't use — and an [asyncinput](https://github.com/CoCkMelon/asyncinput)-style
low-latency input path underneath.

```c
#include <SDL3/SDL.h>   /* yes, really — unmodified SDL3 code compiles */

int main(void) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *w = SDL_CreateWindow("hello", 800, 600, 0);
    SDL_Event e;
    while (SDL_PollEvent(&e)) { /* ... */ }
}
```

- **Drop-in API**: names, signatures, struct layouts, enum values and event
  semantics match SDL3 3.2 (verified field-by-field against the real headers —
  `sizeof(SDL_Event) == 128`, identical member offsets, identical constants).
- **Fast** (measured on this repo's benchmarks, see below):
  - event queue: **~4.6× faster** push+poll than SDL3 (10.4 ns vs 48.6 ns)
  - empty `SDL_PumpEvents`: **~9× faster** (4.5 ns vs 41 ns)
  - raw input: **~5–9 µs** kernel-timestamp → your code, via a dedicated
    worker thread (asyncinput-style)
- **Lean**: windowing + input only. No audio, renderer, GPU, joystick, HID,
  camera, filesystem, iostream, … ~5k lines of C total.
- **Low-latency input like asyncinput**: a background thread reads
  `/dev/input/event*` directly with `epoll`, publishes into lock-free SPSC
  rings and fires your callback *on the worker thread*. Native evdev codes,
  zero translation cost, kernel timestamps. Falls back to Wayland seat input
  when `/dev/input` is not readable.

## Status

| Platform | State |
|---|---|
| Linux / Wayland (wl_compositor + xdg-shell, wl_seat fallback input) | ✅ done, tested against Weston 14 |
| Linux / evdev raw input worker (asyncinput-style) | ✅ done, tested via synthetic uinput devices |
| `dummy` (offscreen) driver | ✅ done, used for headless CI |
| Web (Emscripten) | 🔜 next (driver interface ready) |
| Windows / Win32, macOS / Cocoa | 🔜 after web |

## Architecture

```
            ┌──────────────────────────────────────────────────────┐
            │                 input worker thread                  │
 /dev/input │  epoll ─► batch read(struct input_event)             │
 event*  ───┤   ├─► user callbacks (your code, lowest latency)     │
            │   ├─► SPSC ring ──► SDL_PumpEvents ──► SDL_Event     │
            │   └─► SPSC ring ──► SDLop_PollRawEvents              │
            └───────────────┬──────────────────────────────────────┘
                            │ eventfd wake
   Wayland display ────► SDL_PumpEvents (main thread)
   (windows, fallback input)   │
                               ▼
                    SDL_PollEvent / SDL_WaitEvent / state queries
```

- **No locks, no allocation on the hot path.** The producer/consumer rings use
  acquire/release atomics with cache-line-padded indices; the SDL event queue
  is a fixed 128-slot ring; error strings are thread-local buffers.
- **One translation, one place.** Raw events keep native evdev codes
  (zero cost). The single evdev→`SDL_Scancode` table lookup happens only when
  draining into SDL events.
- **Evdev-preferred, Wayland-fallback.** When the worker sees a keyboard or
  pointer device, the corresponding `wl_seat` stream is ignored (keymap and
  modifier state are still consumed for correct layout/keysyms via
  xkbcommon). No double-delivered events.
- **Kernel timestamps** flow into `SDL_Event.timestamp` (ns) and every
  `SDLop_RawEvent.timestamp_ns`, so you can measure end-to-end latency.

## Benchmarks (this sandbox, x86-64, RelWithDebInfo)

Event queue — identical source compiled against SDLop and SDL3 3.2.10,
dummy video driver, 1M events, push+drain pattern:

| | SDLop | SDL3 | speedup |
|---|---|---|---|
| push + poll | 10.4 ns/event (≈96 M/s) | 48.6 ns/event (≈21 M/s) | **4.6×** |
| empty `SDL_PumpEvents` | 4.5 ns | 41.2 ns | **9.2×** |

Raw input path (synthetic uinput device, 2000 events):

| path | p50 | avg | p95 |
|---|---|---|---|
| kernel → worker-thread callback | 5.2 µs | 11.2 µs | 11.4 µs |
| kernel → main-thread `SDL_PumpEvents` | 9.1 µs | 16.0 µs | 21.8 µs |

(includes the uinput→evdev kernel round-trip; real hardware is similar)

Reproduce: `cmake --build build && ./build/bench_events && ./build/bench_events_sdl3`
and `sudo ./build/bench_raw_latency` (needs `/dev/uinput`).

## API surface (SDL3-compatible)

Init/quit · `SDL_Init` `SDL_InitSubSystem` `SDL_QuitSubSystem` `SDL_WasInit` `SDL_Quit`
Error · `SDL_SetError` `SDL_GetError` `SDL_ClearError` `SDL_OutOfMemory`
Timer · `SDL_GetTicks` `SDL_GetTicksNS` `SDL_GetPerformanceCounter` `SDL_GetPerformanceFrequency` `SDL_Delay` `SDL_DelayNS`
Video · `SDL_CreateWindow` `SDL_DestroyWindow` `SDL_ShowWindow` `SDL_HideWindow` `SDL_SetWindowTitle` `SDL_GetWindowTitle` `SDL_SetWindowSize` `SDL_GetWindowSize` `SDL_SetWindowPosition` `SDL_GetWindowPosition` `SDL_MinimizeWindow` `SDL_MaximizeWindow` `SDL_RestoreWindow` `SDL_SetWindowFullscreen` `SDL_RaiseWindow` `SDL_GetWindowID` `SDL_GetWindowFromID` `SDL_GetWindowFlags` `SDL_GetCurrentVideoDriver` `SDL_GetNumVideoDrivers` `SDL_GetVideoDriver`
Events · `SDL_PumpEvents` `SDL_PollEvent` `SDL_WaitEvent` `SDL_WaitEventTimeout` `SDL_PushEvent` `SDL_HasEvent` `SDL_HasEvents` `SDL_FlushEvent` `SDL_FlushEvents` `SDL_RegisterEvents` `SDL_QuitRequested` `SDL_GetWindowFromEvent`
Keyboard · `SDL_GetKeyboardState` `SDL_GetKeyState` `SDL_ResetKeyboard` `SDL_GetModState` `SDL_SetModState` `SDL_GetKeyboardFocus` `SDL_GetKeyFromScancode` `SDL_GetScancodeFromKey` `SDL_GetScancodeName` `SDL_GetKeyName`
Mouse · `SDL_GetMouseState` `SDL_GetRelativeMouseState` `SDL_GetMouseFocus` `SDL_SetWindowRelativeMouseMode` `SDL_GetWindowRelativeMouseMode`
Log · `SDL_Log` `SDL_LogMessage` `SDL_LogVerbose` `SDL_LogDebug` `SDL_LogInfo` `SDL_LogWarn` `SDL_LogError`
Types · `SDL_Event` (128 B, same layout) · `SDL_Scancode` (full SDL3 list) · `SDL_Keycode`/`SDLK_*` · `SDL_Keymod` · `SDL_WindowFlags` · `SDL_MouseWheelDirection`

### SDLop extensions (`<SDL3/SDLop.h>`) — the asyncinput-style API

```c
#include <SDL3/SDLop.h>

/* zero-cost native constants: SDLop_KEY_W, SDLop_BTN_LEFT, SDLop_REL_X, ...
   (identical to Linux evdev codes, like asyncinput's NI_* constants) */

void on_raw(const SDLop_RawEvent *ev, void *ud) {   /* worker thread! */
    if (ev->type == SDLop_EV_KEY && ev->code == SDLop_KEY_W && ev->value) {
        /* ~µs after the kernel saw it */
    }
}
SDLop_RegisterRawEventCallback(on_raw, NULL);  /* like ni_register_callback */

SDLop_RawEvent raws[64];
int n = SDLop_PollRawEvents(raws, 64);         /* like ni_poll */

SDLop_RawInputAvailable();     /* is the evdev worker running? */
SDLop_GetRawEventCount();
SDLop_SetWindowClearColor(w, 0x30, 0xA0, 0x40);  /* no renderer needed */
```

`SDLop_RawEvent` = `{ device, type, code, value, timestamp_ns }` — exactly
`struct input_event` semantics. The standard SDL event queue keeps working in
parallel; mix both models freely.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build            # headless tests
```

Deps: `wayland-client`, `xkbcommon`, `wayland-protocols` + `wayland-scanner`
(all standard on Wayland distros). Static `libsdlop.a` and shared
`libSDLop.so` are produced. Link: `-lsdlop -lwayland-client -lxkbcommon -lpthread`.

Tests that need privileges/compositor skip cleanly:

```sh
WAYLAND_DISPLAY=wayland-0 ctest --test-dir build   # wayland integration test
sudo ./build/test_evdev_uinput                     # raw evdev end-to-end
```

## Examples

- `examples/window.c` — classic SDL3 loop; **compiles unmodified against real
  SDL3 too** (verified).
- `examples/wasd.c` — asyncinput-style: raw callback drives movement, with
  automatic SDL-queue fallback when raw input is unavailable.
- `examples/raw_latency.c` — measures kernel→callback latency percentiles
  (asyncinput `read_keys` equivalent).

## Notes & limitations

- Raw evdev input needs read access to `/dev/input` (root or `input` group);
  otherwise SDLop silently uses Wayland seat input. Like asyncinput, the raw
  path bypasses compositor keyboard focus/layout.
- Wayland cannot position windows: `SDL_SetWindowPosition` returns false.
- Relative mouse mode hides the cursor and streams deltas from evdev;
  `zwp_pointer_constraints` (true pointer lock) is on the list.
- `SDL_WINDOW_BORDERLESS` is inherent (no client-side decorations yet).
- No rendering API by design; `SDLop_SetWindowClearColor` tints the window.

## Why "faster"?

1. Zero-allocation fixed-size event structures vs SDL3's dynamically sized
   queue with mutex + per-event bookkeeping.
2. Single-purpose code paths — no subsystem dispatch, no event watches/filters
   layer, no device-object indirection.
3. Lock-free SPSC rings instead of lock-protected queues between threads.
4. No translation on the raw path; one table lookup only when you consume
   SDL events.

## License

zlib license (same as SDL). Scancode/keycode tables and names are derived
from SDL3 headers (© Sam Lantinga, zlib) — attribution in the file headers.
