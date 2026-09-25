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
| Linux / Wayland (wl_compositor + xdg-shell, wl_seat fallback input) | ✅ done, tested against Weston 14 + sway 1.10 (pixman) |
| Linux / evdev raw input worker (asyncinput-style) | ✅ done, tested via synthetic uinput devices |
| Software rendering (`SDL_GetWindowSurface`, zero-copy wl_shm) | ✅ done, tested |
| OpenGL / OpenGL ES via EGL (llvmpipe on GPU-less systems) | ✅ done, verified by pixel read-back |
| Vulkan WSI (`SDL_Vulkan_*`, lavapipe) | ✅ done, verified by swapchain read-back |
| Pointer lock (`zwp_pointer_constraints` + `zwp_relative_pointer`) | ✅ done, tested against a mini compositor |
| `dummy` (offscreen) driver | ✅ done, used for headless CI (incl. RAM-framebuffer surfaces) |
| Web (Emscripten): canvas windows, DOM input, WebGL1/2 | ✅ done, tested in node + headless Chromium 153 |
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

Rendering — 640×480 through Weston (headless), pure software rasterizers:

| path | result |
|---|---|
| software surface: full-window fill + present | ≈11 000 fps (≈3.5 GPix/s; the pixels **are** the shm buffer, zero copy) |
| OpenGL ES 3.2 (llvmpipe): glClear + swap | ≈3 400 fps (≈1.0 GPix/s) |
| Vulkan (lavapipe): swapchain clear + present | verified by read-back, present-paced |
| software plasma (per-pixel `sin`, 640×480) | ≈35 fps (compute-bound, not presentation-bound) |

Reproduce: `cmake --build build && ./build/bench_events && ./build/bench_events_sdl3`
and `sudo ./build/bench_raw_latency` (needs `/dev/uinput`), and
`WAYLAND_DISPLAY=wayland-0 ./build/bench_render` for the rendering numbers.

## API surface (SDL3-compatible)

Init/quit · `SDL_Init` `SDL_InitSubSystem` `SDL_QuitSubSystem` `SDL_WasInit` `SDL_Quit`
Error · `SDL_SetError` `SDL_GetError` `SDL_ClearError` `SDL_OutOfMemory`
Timer · `SDL_GetTicks` `SDL_GetTicksNS` `SDL_GetPerformanceCounter` `SDL_GetPerformanceFrequency` `SDL_Delay` `SDL_DelayNS`
Video · `SDL_CreateWindow` `SDL_DestroyWindow` `SDL_ShowWindow` `SDL_HideWindow` `SDL_SetWindowTitle` `SDL_GetWindowTitle` `SDL_SetWindowSize` `SDL_GetWindowSize` `SDL_SetWindowPosition` `SDL_GetWindowPosition` `SDL_MinimizeWindow` `SDL_MaximizeWindow` `SDL_RestoreWindow` `SDL_SetWindowFullscreen` `SDL_RaiseWindow` `SDL_GetWindowID` `SDL_GetWindowFromID` `SDL_GetWindowFlags` `SDL_GetCurrentVideoDriver` `SDL_GetNumVideoDrivers` `SDL_GetVideoDriver` `SDL_GetWindowDisplayScale`
Events · `SDL_PumpEvents` `SDL_PollEvent` `SDL_WaitEvent` `SDL_WaitEventTimeout` `SDL_PushEvent` `SDL_HasEvent` `SDL_HasEvents` `SDL_FlushEvent` `SDL_FlushEvents` `SDL_RegisterEvents` `SDL_GetWindowFromEvent`
Keyboard · `SDL_GetKeyboardState` `SDL_ResetKeyboard` `SDL_GetModState` `SDL_SetModState` `SDL_GetKeyboardFocus` `SDL_GetKeyFromScancode` `SDL_GetScancodeFromKey` `SDL_GetScancodeName` `SDL_GetKeyName`
Mouse · `SDL_GetMouseState` `SDL_GetRelativeMouseState` `SDL_GetMouseFocus` `SDL_SetWindowRelativeMouseMode` `SDL_GetWindowRelativeMouseMode`
Surfaces · `SDL_CreateSurface` `SDL_CreateSurfaceFrom` `SDL_DestroySurface` `SDL_GetWindowSurface` `SDL_UpdateWindowSurface` `SDL_UpdateWindowSurfaceRects` `SDL_DestroyWindowSurface` `SDL_WindowHasSurface` `SDL_FillSurfaceRect` `SDL_FillSurfaceRects` `SDL_ReadSurfacePixel` `SDL_LockSurface` `SDL_UnlockSurface` `SDL_MapSurfaceRGB` `SDL_MapSurfaceRGBA` `SDL_MapRGB` `SDL_MapRGBA`
OpenGL · `SDL_GL_SetAttribute` `SDL_GL_GetAttribute` `SDL_GL_CreateContext` `SDL_GL_DestroyContext` `SDL_GL_MakeCurrent` `SDL_GL_GetCurrentContext` `SDL_GL_GetCurrentWindow` `SDL_GL_SwapWindow` `SDL_GL_SetSwapInterval` `SDL_GL_GetSwapInterval` `SDL_GL_GetProcAddress` `SDL_GL_ResetAttributes`
Vulkan · `SDL_Vulkan_LoadLibrary` `SDL_Vulkan_GetVkGetInstanceProcAddr` `SDL_Vulkan_GetInstanceExtensions` `SDL_Vulkan_CreateSurface` `SDL_Vulkan_DestroySurface` `SDL_Vulkan_GetPresentationSupport` `SDL_Vulkan_UnloadLibrary`
Log · `SDL_Log` `SDL_LogMessage` `SDL_LogVerbose` `SDL_LogDebug` `SDL_LogInfo` `SDL_LogWarn` `SDL_LogError`
Types · `SDL_Event` (128 B, same layout) · `SDL_Scancode` (full SDL3 list) · `SDL_Keycode`/`SDLK_*` · `SDL_Keymod` · `SDL_WindowFlags` · `SDL_MouseWheelDirection` · `SDL_Surface` (48 B, same layout) · `SDL_PixelFormat`/`SDL_PIXELFORMAT_*` · `SDL_BlendMode` · `SDL_Rect`/`SDL_FRect` · `SDL_GLAttr`/`SDL_GLProfile`

`struct`/enum/constant ABI is byte-identical to SDL3 3.2.10 (verified by an
offset/value harness compiled against both header sets).

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

# Optional: compare public layouts/constants with stock SDL3 3.2.10 headers
cmake -S . -B build-abi \
  -DSDL3_REFERENCE_INCLUDE=/path/to/SDL3/include
cmake --build build-abi -j
ctest --test-dir build-abi -R abi_check --output-on-failure
```

The manually driven Wayland behavior trace is built with the test targets but
is not registered with CTest because it waits for an external input script:

```sh
cmake --build build --target behaviour_probe_wayland
SDLOP_DISABLE_RAW_INPUT=1 WAYLAND_DISPLAY=wayland-0 \
  ./build/behaviour_probe_wayland --seconds 8 > wayland-trace.txt
```

The program prints `READY-INPUT` after the window is configured. At that point,
use the same compositor-side key/pointer script for SDLop and stock SDL3, then
compare the traces. Use `--relative` when comparing relative-pointer behavior.
`SDLOP_DISABLE_RAW_INPUT=1` is important: it forces SDLop to use Wayland seat
input rather than `/dev/input`.

Build deps: `wayland-protocols` + `wayland-scanner` (for the generated
protocol code) and the `wayland-client`/`xkbcommon`/`EGL` **headers** — all
standard on Wayland distros. Static `libsdlop.a` and shared `libSDLop.so`
are produced.

Runtime loading is SDL3-style dynamic: `libwayland-client.so.0`,
`libxkbcommon.so.0`, `libEGL.so.1`, `libwayland-egl.so.1` and `libvulkan.so.1`
are all `dlopen`-ed on demand — nothing optional is linked
(`objdump -p libSDLop.so` shows NEEDED: libc only). Apps link just
`-lsdlop` (plus `-lpthread -ldl -lm` where the toolchain doesn't fold them
into libc). Missing libs degrade gracefully: no wayland-client -> dummy
video driver; no xkbcommon -> windowing still works. Override probe names
with `SDLOP_LIB_WAYLAND` / `SDLOP_LIB_XKB` / `SDLOP_LIB_EGL`.

Tests that need privileges/compositor skip cleanly:

```sh
WAYLAND_DISPLAY=wayland-0 ctest --test-dir build   # wayland integration test
sudo ./build/test_evdev_uinput                     # raw evdev end-to-end
```

### Web (Emscripten)

```sh
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-web -j
ctest --test-dir build-web            # runs the .js outputs under node
tests/run_browser_test.sh build-web   # real DOM+WebGL run in headless chromium
```

The web build swaps in `src/video/emscripten/` (HTML5 canvas + WebGL via
`emscripten_webgl_*`, DOM event callbacks feeding the SDL queue) and
`src/SDL_keyboard_web.c` (static US keymap; no xkbcommon on the web).
Raw evdev input is unavailable there by design - `SDLop_*` raw APIs report
unsupported and standard SDL events come from the DOM. `test_web` runs in
both environments: node (dummy driver + keymap table) and a real browser
(window on `#canvas`, synthetic `KeyboardEvent`/`MouseEvent`/`WheelEvent`
through the emscripten callbacks, WebGL context, software-surface blit).

The suite is verified against two compositors (13/13 on both):

| compositor                | renderer | notes                                            |
|---------------------------|----------|--------------------------------------------------|
| sway 1.10 / wlroots 0.18  | pixman   | full suite incl. virtual input + pointer lock    |
| weston 14 headless        | GL/no-op | wlr-virtual-input tests skip (protocol absent)   |

Highlights:

- `test_gl_matrix` — ES 2.0, ES 3.0, GL 3.3 core, GL 4.5 core contexts on
  llvmpipe; FBO clear/read-back per config; `SDL_GL_SHARE_WITH_CURRENT_CONTEXT`
  verified by reading context A's texture through an FBO in shared context B.
  (Mesa gives at least the requested version — llvmpipe reports ES 3.2 / 4.5.)
- `test_vulkan` — instance/device/swapchain, render pass clear, image→buffer
  read-back, present on lavapipe.
- `test_vulkan_compute` — compute pipeline from embedded SPIR-V
  (`tests/shaders/compute_fill.comp`), storage buffer + descriptor set,
  dispatch + fence, all 256 outputs verified.
- `test_virtual_input` — end-to-end input against a real compositor via the
  wlr virtual-input protocols (XMLs vendored from wlroots 0.18 in
  `tests/protocols/`): pointer enter at an exact position, xkbcommon keymap +
  key events, buttons, wheel, pointer-lock relative motion, absolute motion
  after unlock. On sway it drives a second SDLop client connection.

## Examples

- `examples/window.c` — classic SDL3 loop; **compiles unmodified against real
  SDL3 too** (verified).
- `examples/wasd.c` — asyncinput-style: raw callback drives movement, with
  automatic SDL-queue fallback when raw input is unavailable.
- `examples/raw_latency.c` — measures kernel→callback latency percentiles
  (asyncinput `read_keys` equivalent).
- `examples/surface_plasma.c` — software rendering: plasma drawn straight
  into the zero-copy shm window surface.
- `examples/gl_triangle.c` — OpenGL ES 2 triangle via EGL (llvmpipe on
  GPU-less systems), with center-pixel read-back.
- `examples/vulkan_clear.c` — full Vulkan swapchain loop (lavapipe) via
  `SDL_Vulkan_*`, cycling clear color verified by `vkCmdCopyImageToBuffer`
  read-back.
- `tests/mini_compositor.c` — a tiny purpose-built Wayland compositor
  (wl_seat + pointer-constraints + relative-pointer) that makes the pointer
  lock integration testable without a seat-capable compositor.

## Notes & limitations

- Raw evdev input needs read access to `/dev/input` (root or `input` group);
  otherwise SDLop silently uses Wayland seat input. Like asyncinput, the raw
  path bypasses compositor keyboard focus/layout.
- Wayland cannot position windows: `SDL_SetWindowPosition` returns false.
- Relative mouse mode uses real pointer lock (`zwp_pointer_constraints` +
  `zwp_relative_pointer`) on Wayland when the compositor has a seat/pointer,
  and raw evdev deltas when the worker thread is active; the window-system
  fallback streams deltas from the relative-pointer protocol.
- `SDL_WINDOW_BORDERLESS` is inherent (no client-side decorations yet).
- Software surfaces are XRGB8888 wl_shm buffers (zero copy); the GL and
  Vulkan paths were validated against Mesa's software rasterizers
  (llvmpipe/lavapipe) but work with any EGL/Vulkan driver.
- `SDLop_SetWindowClearColor` tints a window that has no surface/GL/Vulkan
  content yet.
- Web: the static keymap is US-layout only (browser text input events are
  not used yet); `SDL_GL_SHARE_WITH_CURRENT_CONTEXT` is ignored (WebGL has no
  cross-context object sharing);
  no Vulkan (WebGPU would be the web analog).

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
