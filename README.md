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
  semantics match SDL3 3.4.20 (verified field-by-field against the real headers —
  `sizeof(SDL_Event) == 128`, identical member offsets, identical constants).
- **Fast** (measured on this repo's benchmarks, see below):
  - event queue: **~4.6× faster** push+poll than SDL3 (10.4 ns vs 48.6 ns)
  - empty `SDL_PumpEvents`: **~9× faster** (4.5 ns vs 41 ns)
  - raw input: **~5–9 µs** kernel-timestamp → your code, via a dedicated
    worker thread (asyncinput-style)
- **Lean**: windowing + input only. No audio, renderer, GPU, HID, camera,
  iostream, … (the joystick/gamepad API is exported but reports no devices,
  so unmodified SDL3 apps still link and run; ~39k lines of C including the X11,
  Wayland, KMSDRM and web drivers; the SDL surface/blit code is ported).
- **Low-latency input like asyncinput**: a background thread reads
  `/dev/input/event*` directly with `epoll`, publishes into lock-free SPSC
  rings and fires your callback *on the worker thread*. Native evdev codes,
  zero translation cost, kernel timestamps. On the console drivers it is the
  input source; on X11/Wayland it is opt-in (`SDLOP_RAW_INPUT=1`) and then
  replaces the window system's keyboard and mouse entirely — sources are never
  mixed (see input routing below).

## Status

| Platform | State |
|---|---|
| Linux / Wayland (wl_compositor + xdg-shell, wl_seat fallback input) | ✅ done, tested against Weston 14 + sway 1.10 (pixman) |
| Linux / X11 (pure XCB; MIT-SHM, XKB, XInput2 raw motion, RandR 1.5 — all `dlopen`-ed) | ✅ done, tested on Xvfb with and without a WM (openbox) |
| Linux / evdev raw input worker (asyncinput-style) | ✅ done, tested via synthetic uinput devices |
| Software rendering (`SDL_GetWindowSurface`, zero-copy wl_shm) | ✅ done, tested |
| OpenGL / OpenGL ES: GLX on X11 for desktop GL and for GLES when the server has `GLX_EXT_create_context_es_profile` (as stock SDL3), EGL otherwise and on Wayland / KMSDRM (llvmpipe on GPU-less systems) | ✅ done, verified by pixel read-back |
| Drop-in for binaries built against stock SDL3 (`libSDL3.so.0`, symbol version `SDL3_0.0.0`) | ✅ done, tested with Dear ImGui and Nuklear SDL3+OpenGL3 examples |
| Vulkan WSI (`SDL_Vulkan_*`, lavapipe) | ✅ done, verified by swapchain read-back |
| Pointer lock (`zwp_pointer_constraints` + `zwp_relative_pointer`) | ✅ done, tested against a mini compositor |
| Linux / KMSDRM (dumb buffers, GBM/EGL, VK_KHR_display, hardware cursor) | ✅ done (no DRM device in CI; build-tested) |
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
   Wayland / X11 fd ───► SDL_PumpEvents (main thread)
   (windows, native keys)   │
                               ▼
                    SDL_PollEvent / SDL_WaitEvent / state queries
```

- **No locks, no allocation on the hot path.** The producer/consumer rings use
  acquire/release atomics with cache-line-padded indices; the SDL event queue
  is a fixed 128-slot ring; error strings are thread-local buffers.
- **One translation, one place.** Raw events keep native evdev codes
  (zero cost). The single evdev→`SDL_Scancode` table lookup happens only when
  draining into SDL events.
### Input routing and focus

SDLop never mixes input sources: SDL keyboard **and** mouse events come
either all from evdev (`/dev/input`) or all from the window system.

- **X11 / Wayland default to window-system input.** The evdev worker is not
  started; keys, buttons, motion, wheel and relative motion all come from the
  server/compositor, which owns focus, layout and delivery.
- **`SDLOP_RAW_INPUT=1` makes evdev the only source on X11 / Wayland.** The
  driver then drops its own key, button, motion, wheel and relative-motion
  events; it still tracks focus, enter/leave, keymap/layout/lock state and
  repeat settings, and the pointer position it reports only re-anchors the
  position evdev's deltas are added to. Because `/dev/input` sees the whole
  seat, raw events are filtered with the window system's focus: keys need one
  of our windows to have keyboard focus, pointer input needs the pointer over
  one of our windows (or relative mode on the focused window); releases of
  keys/buttons SDL reported down always pass, and a drag that started in our
  window keeps its motion. All or nothing: if the worker cannot read both a
  keyboard and a pointer, SDLop logs why and uses window-system input for
  both. Touch stays with the window system.
- **fbdev / kmsdrm use evdev by default** (`SDLOP_RAW_INPUT=0` turns that
  off). SDL keyboard events use evdev only while SDLop owns `K_OFF` on the
  foreground VT. A working raw keyboard is required before disabling the
  kernel keyboard handler. Ctrl+Alt+Fn is forwarded through `VT_ACTIVATE`.
  Switch-away detaches keyboard delivery and releases SDL's held key state.
  fbdev takes the console keyboard by default (`K_OFF`), like kmsdrm; set
  `SDLOP_FBDEV_CONSOLE_KEYS=0` to leave terminal input working. With raw input
  disabled/unavailable the console also retains its kernel handler, because
  Ctrl+Alt+Fn forwarding needs the evdev worker.
- **The low-latency raw API needs the worker.** `SDLop_RegisterRawEventCallback`
  runs on the worker thread and `SDLop_PollRawEvents` drains a separate ring;
  on X11/Wayland they only receive events with `SDLOP_RAW_INPUT=1`. These
  expose global device events, including keys, regardless of window/VT focus.
  Applications using this API must implement their own focus/security policy;
  do not treat it as a focus-filtered SDL event queue. `SDLop_RawInputAvailable`
  reports the worker; on X11/Wayland true also means evdev is the SDL source.
- **Timestamp provenance follows the source.** Raw-derived SDL events convert
  kernel times to SDL's tick epoch; window-system events use the native
  handler's receipt time. The raw-input benchmark below measures the evdev path.

`SDLOP_DISABLE_RAW_INPUT=1` (legacy) never starts the worker, whatever
`SDLOP_RAW_INPUT` says. `0` or an empty value leaves the default.

### Options: environment or code

Every `SDLOP_*` environment variable is also an SDL hint of the same name
(`SDLOP_HINT_*` macros in `<SDL3/SDLop.h>`), so code can choose it too:

```c
SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1");      /* app default; the environment may override */
SDL_SetHintWithPriority(SDLOP_HINT_X11_NO_SHM, "1",
                        SDL_HINT_OVERRIDE);  /* wins over the environment */
SDL_Init(SDL_INIT_VIDEO);
```

Precedence: `SDL_HINT_OVERRIDE` hint > environment > lower-priority hint >
built-in default. Options are read when the subsystem or driver using them
initialises (mostly `SDL_Init`), so set them first; `SDL_ResetHint` returns to
environment/default, and `SDL_Quit` drops all hints (as in SDL3). The
environment is read through `SDL_GetEnvironment()`, a snapshot taken on first
use, so change it with `SDL_setenv_unsafe` after that. Booleans: unset/empty = default, `0`/`false`/`off`/`no` =
false, anything else true.

| Option | Meaning |
|---|---|
| `SDLOP_RAW_INPUT` | evdev input (default: on for kmsdrm/fbdev, off for X11/Wayland; see above) |
| `SDLOP_DISABLE_RAW_INPUT` | legacy: never start evdev input |
| `SDLOP_LOG_PRIORITY` | default log priority when `SDL_LOGGING` is unset |
| `SDLOP_POWER_SUPPLY_PATH` | sysfs power-supply directory (tests) |
| `TTYNO` | console drivers: VT to run on (implies switching to it) |
| `SDLOP_X11_NO_SHM`, `_NO_RENDER`, `_NO_TOUCH` | don't use MIT-SHM / XRender / XInput2 touch |
| `SDLOP_X11_RELATIVE_WARP` | relative mode by warping instead of XInput2 raw motion |
| `SDLOP_X11_EGL_PLATFORM` | `x11` skips the EGL xcb platform |
| `SDLOP_OFFLOAD`, `SDLOP_GL_OFFLOAD`, `SDLOP_VULKAN_OFFLOAD` (`SDLOP_VK_OFFLOAD`) | GPU offload policy: `auto` (default), `0`/`off`, `1`/`force`; the per-API option wins (see Notes) |
| `SDLOP_GL_OFFLOAD_PRIME` | kmsdrm: `0` copies offloaded GL frames instead of PRIME scan-out |
| `SDLOP_WAYLAND_NO_VIEWPORTER` | Wayland: don't use `wp_viewporter` (clear colour as a filled full-size buffer) |
| `SDL_VIDEO_FORCE_EGL` (`SDL_HINT_VIDEO_FORCE_EGL`) | X11: all contexts (desktop GL and GLES) through EGL instead of GLX (stock SDL3 hint) |
| `SDL_OPENGL_ES_DRIVER` (`SDL_HINT_OPENGL_ES_DRIVER`) | X11: GLES contexts through EGL instead of GLX (stock SDL3 hint) |
| `SDL_OPENGL_LIBRARY` (`SDL_HINT_OPENGL_LIBRARY`) | X11: library to load GLX from (default `libGL.so.1`, then `libGLX.so.0`) |
| `SDLOP_KMSDRM_*` | `DEVICE`, `CONNECTOR`, `ATOMIC`, `ATOMIC_CURSOR`, `DOUBLE_BUFFER`, `FLIP_TIMEOUT_MS`, `HOTPLUG`, `KEEP_MASTER`, `NOFLIP`, `REQUIRE_MASTER`, `VT_SWITCH` (see `sdlop_kmsdrm.c`) |
| `SDLOP_FBDEV_*` | `PATH`, `SIZE`, `DUMP`, `CURSOR`, `CONSOLE_KEYS`, `TAKEOVER`, `VT_SWITCH` (see `sdlop_fbdev.c`) |
| `SDLOP_LIB_*` | dlopen name for `X11`, `XCB`, `XCB_RANDR`, `XCB_RENDER`, `XCB_SHM`, `XCB_XINPUT`, `XCB_XKB`, `XKB`, `XKBCOMMON_X11`, `WAYLAND`, `EGL`, `DRM`, `GBM` (`none` disables an optional X11 extension library) |

### fbdev arbitration

If a target VT already belongs to another program (`KD_GRAPHICS` or
`VT_PROCESS`), fbdev leaves its console state **and scanout pixels** untouched.
Window surfaces and presents remain usable in RAM; frames are not made visible.
`SDLOP_FBDEV_TAKEOVER=0` explicitly requests direct framebuffer rendering without
console arbitration, including on an owned VT. Use it only when external
coordination makes those writes safe. It is not the default refusal behavior.

The fbdev software cursor is composited into a private frame before the dirty
region is copied to the card, and copies wait for `FBIO_WAITFORVSYNC` when the
driver implements it. That avoids the cursor disappearing between the erase
and redraw steps during motion. `SDLOP_FBDEV_CURSOR=0` disables the cursor;
`SDLOP_FBDEV_CONSOLE_KEYS=0` keeps the console keyboard handler alive.

KMSDRM prefers the hardware cursor plane (atomic when available, legacy ioctls
otherwise) and now sets it up before a Vulkan window hands DRM master over.
Drivers that expose no usable cursor plane (some NVIDIA KMS setups reject both
cursor ioctls) get a software cursor composited into the dumb scanout, which
keeps the pointer visible for Vulkan/GL offload presents. Native `VK_KHR_display`
WSI still depends on the driver's own cursor support.

Both console drivers use the same XCursor theme bitmap, hotspot and premultiplied
alpha handling, so the pointer looks the same on KMSDRM and fbdev. When no theme
is installed they fall back to the same built-in arrow.

Console drivers reserve their VT release/acquire signals until shutdown. They
undo only signal blocks they introduced; unrelated application signal-mask
changes made after video initialization are preserved. Do not change the two
reserved VT signal bits while the driver owns them.

## Benchmarks (this sandbox, x86-64, RelWithDebInfo)

Event queue — identical source compiled against SDLop and SDL3 3.2.10 (measured
before the 3.4 update),
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

Rendering and windows — software rasterizers (llvmpipe), the same programs
built against SDLop and stock SDL3 3.4.20, 640×480 unless noted:

| X11 (Xvfb) | SDLop | SDL3 |
|---|---|---|
| software surface: full-window fill + present | ≈3 300 fps (≈300 µs; present ≈180 µs: MIT-SHM + sync) | ≈330 fps (≈3 000 µs) |
| desktop GL (GLX): clear + swap, 320×240 | ≈220 µs | ≈225 µs |
| desktop GL forced through EGL (`SDL_VIDEO_FORCE_EGL=1`, the pre-GLX path) | ≈310 µs | |
| GLES 2 (GLX ES profile): clear + swap | ≈2 600 fps | context fails on this server (GLXBadFBConfig) |
| create + destroy a window | ≈0.4–0.5 ms | ≈2.4–2.8 ms |

| Wayland (Weston 10, headless) | SDLop |
|---|---|
| software surface: full-window fill + present | ≈400 fps, paced by the compositor (see below) |
| GLES (Mesa EGL): clear + swap | ≈385 fps, same pacing |
| create + destroy a window | ≈115 µs |

Wayland software presentation follows the `wl_buffer` contract: a buffer is
written only after the compositor's `wl_buffer.release` (see Notes), so the
software frame rate is bounded by how fast the compositor gives buffers back.
Weston releases one per repaint, about every 2.5 ms here, and Mesa's EGL
waits on the same releases. Compositors that copy `wl_shm` buffers at
commit and release them right away allow higher rates. **Earlier releases
published ≈11 000 fps here**, and an external comparison measured ≈17 500
fps. Those numbers came from re-attaching one buffer every frame without
waiting for its release, which let the application draw into a buffer the
compositor was still reading. They measured a protocol violation, not
presentation, and should not be compared with conforming clients.
Window creation used to take ≈3.4 ms here (≈0.8 ms in the external run),
mostly from mapping and filling a full-size buffer during the configure
handshake. The clear colour is now a 1×1 buffer scaled by `wp_viewporter`.

Reproduce: `cmake --build build && ./build/bench_events && ./build/bench_events_sdl3`
and `sudo ./build/bench_raw_latency` (needs `/dev/uinput`), and
`SDL_VIDEODRIVER=x11|wayland ./build/bench_render` for the rendering numbers.

## API surface (SDL3-compatible)

741 SDL3 functions; every one is declared in `include/SDL3/` (generated from
the SDL 3.4.20 headers by `tools/gen_headers.py`), exported by `libSDLop.so`
with the same signature as SDL3, and checked by the `api_exports` test.

| Area | Coverage |
|---|---|
| Init / hints / properties / log / error / timers / stdlib | `SDL_Init*`, `SDL_*Hint*`, `SDL_*Property*`, `SDL_Log*`, `SDL_GetTicks*`, `SDL_AddTimer`, `SDL_malloc`…`SDL_vsnprintf`, `SDL_iconv*`, atomics, `SDL_main` + main callbacks (`SDL_AppInit`/`AppIterate`/`AppEvent`/`AppQuit`) |
| Video | displays + modes, content scale, window create (incl. `WithProperties`), popups/tooltips (parent-relative), min/max size, aspect ratio, bordered / resizable / always-on-top / focusable / modal / parent, opacity, icon, borders size, flash, sync, safe area, fullscreen modes, `SDL_GetWindows`, screensaver |
| Grabs & mouse | relative mode, mouse/keyboard grab, mouse rect, `SDL_CaptureMouse`, warp (window + global), global mouse state, cursors (`SDL_CreateCursor`, color + animated cursors, system cursors from the XCursor theme, show/hide), device lists (`SDL_GetMice`/`GetKeyboards`/`GetTouchDevices` + names) |
| Events | queue, `SDL_PeepEvents`, filters + watches, `SDL_WaitEvent*`, user events, `SDL_GetEventDescription` |
| Keyboard / text | keymaps with xkb layouts, key names, text input start/stop, compose |
| Surfaces | create/convert/duplicate/scale/flip/rotate, palettes, colorspaces, blend modes, color keys, `SDL_BlitSurface*` incl. scaled/tiled/9-grid, float/HDR (PQ, 10-bit) conversions, premultiplied alpha, rects |
| OpenGL / Vulkan | GLX (X11 desktop GL) and EGL contexts (`SDL_GL_*`, `SDL_GL_ExtensionSupported`), `SDL_EGL_*` (current display/config/surface, attribute callbacks; NULL for GLX contexts, as stock), Vulkan WSI (`SDL_Vulkan_*`) |
| Filesystem | `SDL_GetBasePath`, `SDL_GetPrefPath`, `SDL_GetUserFolder` (XDG), `SDL_CreateDirectory`, `SDL_EnumerateDirectory`, `SDL_GetPathInfo`, `SDL_CopyFile`, `SDL_RenamePath`, `SDL_RemovePath`, `SDL_GetCurrentDirectory` (no `SDL_GlobDirectory`) |
| Joystick / gamepad | the whole `SDL_joystick.h` / `SDL_gamepad.h` API, behaving like stock SDL3 with no controller attached: empty device lists, opens fail with an error, NULL handles are harmless, string/label helpers work |
| Misc | assertions (`SDL_assert` handler/report, `SDL_ASSERT` hint), message boxes (zenity, like SDL3 on Wayland), `SDL_OpenURL` (xdg-open), `SDL_GetPowerInfo` (sysfs), GUID strings, system theme (settings portal, `SDL_EVENT_SYSTEM_THEME_CHANGED`) and window progress (Unity launcher API) over a built-in D-Bus client (no libdbus; `SDLOP_NO_DBUS=1` turns it off), file dialogs (`SDL_Show*FileDialog`, `SDL_ShowOpenFolderDialog`: xdg-desktop-portal, else zenity; `SDL_HINT_FILE_DIALOG_DRIVER`; modal to the parent window on X11 and, through xdg-foreign, on Wayland) |

Unsupported with an SDL error (like a stock SDL3 build without that feature):
FOURCC/YUV surfaces, RLE acceleration, BMP/PNG I/O (needs `SDL_IOStream`).
Out of scope by design: audio, renderer, GPU, haptics, sensors, camera,
iostream, and real game controller support.

`struct`/enum/constant ABI is byte-identical to SDL3 3.4.20 (verified by an
offset/value harness compiled against both header sets), and SDLop's exported
symbols are a strict subset of `libSDL3.so` 3.4.20 (`tools/api_subset_check.sh`).
Surface pixel output is diffed against the reference library
(`tests/run_surface_diff.sh`).

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

# Optional: compare public layouts and every SDLK_*/SDL_KMOD_*/SDL_SCANCODE_*
# constant with stock SDL3 3.4.20 headers (CI runs this)
cmake -S . -B build-abi \
  -DSDL3_REFERENCE_INCLUDE=/path/to/SDL3/include
cmake --build build-abi -j
ctest --test-dir build-abi -R abi_check --output-on-failure
```

Build switches: `SDLop_BUILD_TESTS`, `SDLop_BUILD_EXAMPLES`,
`SDLop_BUILD_BENCHMARKS` (all ON by default) and `SDLop_BUILD_TOOLS` (default:
same as `SDLop_BUILD_TESTS`). The tools are `wl_injector` and the behaviour
probes. The Wayland input injectors are skipped, with a configure message,
when wayland-client, libxkbcommon or the vendored virtual-input protocol
XMLs (`tests/protocols/`) are missing.

The manually driven Wayland behavior trace is built with the tools
(`SDLop_BUILD_TOOLS`) but is not registered with CTest because it waits for
an external input script:

```sh
cmake --build build --target behaviour_probe_wayland
SDLOP_DISABLE_RAW_INPUT=1 WAYLAND_DISPLAY=wayland-0 \
  ./build/behaviour_probe_wayland --seconds 8 > wayland-trace.txt
```

The program prints `READY-INPUT` after the window is configured. At that point,
use the same compositor-side key/pointer script for SDLop and stock SDL3, then
compare the traces. Use `--relative` when comparing relative-pointer behavior.
A reference driver ships with the repo: once `READY-INPUT` appears, run
`./build/behaviour_probe_inject` (target `behaviour_probe_inject`) to replay
the scripted enter/motion/click/key/wheel sequence on sway or any compositor
implementing the wlr virtual-pointer + virtual-keyboard protocols.
`SDLOP_DISABLE_RAW_INPUT=1` is important: it forces SDLop to use Wayland seat
input rather than `/dev/input`.

Build deps: `wayland-protocols` + `wayland-scanner` (for the generated
protocol code) and the `wayland-client`/`xkbcommon`/`EGL` **headers** — all
standard on Wayland distros. Static `libsdlop.a` and shared `libSDLop.so`
are produced.

Runtime loading is SDL3-style dynamic: `libwayland-client.so.0`,
`libxkbcommon.so.0`, `libEGL.so.1`, `libwayland-egl.so.1`, `libvulkan.so.1`
and the X11 libraries (`libxcb.so.1`, `libxcb-shm.so.0`, `libxcb-xkb.so.1`,
`libxkbcommon-x11.so.0`, `libxcb-xinput.so.0`, `libxcb-randr.so.0`, and
`libX11.so.6` + `libGL.so.1` for GLX, `libX11.so.6` also for EGL stacks
without `EGL_EXT_platform_xcb`) are all `dlopen`-ed on demand — nothing optional is linked
(`objdump -p libSDLop.so` shows NEEDED: libc and libm only). Apps link just
`-lsdlop` (plus `-lpthread -ldl -lm` where the toolchain doesn't fold them
into libc). Missing libs degrade gracefully: no wayland-client -> dummy
video driver; no xkbcommon -> windowing still works; no libxcb-shm /
-xkb / -xinput / -randr -> X11 uses `xcb_put_image`, core keymaps,
warp-based relative mode and a single root display. Override probe names
with `SDLOP_LIB_WAYLAND` / `SDLOP_LIB_XKB` / `SDLOP_LIB_EGL` /
`SDLOP_LIB_XCB` / `SDLOP_LIB_XCB_SHM` / `SDLOP_LIB_XCB_XKB` /
`SDLOP_LIB_XKBCOMMON_X11` / `SDLOP_LIB_XCB_XINPUT` / `SDLOP_LIB_XCB_RANDR` /
`SDLOP_LIB_X11` (the value `none` disables a module).

Driver order is wayland → x11 → kmsdrm → fbdev → dummy (X11 is used when
`WAYLAND_DISPLAY` is unset or the compositor is unreachable);
`SDL_VIDEODRIVER=x11` forces it, e.g. to run under XWayland. Building the X11
driver needs only the xcb **headers** (`libxcb*-dev`, `libxkbcommon-x11-dev`);
without them it is skipped (`-DSDLOP_VIDEO_X11=OFF` disables it explicitly).

Tests that need privileges/compositor skip cleanly:

```sh
WAYLAND_DISPLAY=wayland-0 ctest --test-dir build   # wayland integration test
xvfb-run -s "-screen 0 1280x1024x24" ctest --test-dir build  # X11 suite
sudo ./build/test_evdev_uinput                     # raw evdev end-to-end
```

### Drop-in use with binaries built against stock SDL3

`libSDLop.so` exports only `SDL_*`, every symbol tagged with stock SDL3's
version node `SDL3_0.0.0` (`tools/sdlop.sym`, CMake option
`SDLOP_SYMBOL_VERSIONING`, default ON). That makes two setups work:

```sh
# 1. alias: no rebuild of SDLop needed
mkdir -p dropin && ln -sf "$PWD/build/libSDLop.so" dropin/libSDL3.so.0
LD_LIBRARY_PATH=$PWD/dropin ./app-built-against-SDL3

# 2. real SDL3 file names: libSDL3.so.0.4.20, SONAME libSDL3.so.0 (opt-in,
#    because it collides with an installed SDL3)
cmake -S . -B build-sdl3 -DSDLOP_SDL3_SONAME=ON && cmake --build build-sdl3
LD_LIBRARY_PATH=$PWD/build-sdl3 ./app-built-against-SDL3
```

Both load without the "no version information available" warning. Things
stock SDL3 apps rely on that SDLop now matches: `SDL_CreateWindow` without
`SDL_Init` initializes video + events (`SDL_WasInit` = `0x4020`), init flag
dependencies (`GAMEPAD` → `JOYSTICK` → `EVENTS`), GLX contexts on X11, the
gamepad/joystick API (no devices) and the `SDL_filesystem.h` helpers
(`tests/test_dropin_compat.c`, `tests/test_glx.c`). An app that calls an
SDL3 function outside SDLop's subset still fails to load with "undefined
symbol": run it with `LD_BIND_NOW=1` to see all of them at once.

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
  automatic SDL-queue fallback when raw input is unavailable (on X11/Wayland
  run it with `SDLOP_RAW_INPUT=1` for the raw path).
- `examples/raw_latency.c` — measures kernel→callback latency percentiles
  (asyncinput `read_keys` equivalent).
- `examples/surface_plasma.c` — software rendering: plasma drawn straight
  into the zero-copy shm window surface.
- `examples/gl_triangle.c` — OpenGL ES 2 triangle via EGL (llvmpipe on
  GPU-less systems), with center-pixel read-back.
- `examples/vulkan_clear.c` — full Vulkan swapchain loop via `SDL_Vulkan_*`,
  cycling clear color verified by `vkCmdCopyImageToBuffer` read-back. With
  `--offload` it runs on the offload GPU (NVIDIA included, via transparent
  swapchain emulation) and reads back into shm/dumb framebuffers.
- `examples/vulkan_stress.c` — heavier GPU workload: compute-updated instanced
  particles, an offscreen scene pass and a post-process pass, with animated
  frame checksums. It follows SDLop's capability-based offload policy by
  default; `--offload` and `--native` force either path. Defaults are a modest
  60 frames at 320x180 with 256 particles; the full stress preset is
  `--frames 300 --size 1280x720 --particles 4096`.
- `tests/mini_compositor.c` — a tiny purpose-built Wayland compositor
  (wl_seat + pointer-constraints + relative-pointer) that makes the pointer
  lock integration testable without a seat-capable compositor.

## Notes & limitations

- Raw evdev input needs read access to `/dev/input` (root or `input` group);
  otherwise SDLop silently uses window-system input. On X11/Wayland it is off
  unless `SDLOP_RAW_INPUT=1`; then SDL events follow the window system's
  focus, while the raw callback/poll API, like asyncinput, sees the whole seat.
- Wayland cannot position toplevel windows: `SDL_SetWindowPosition` returns
  false for them. Popup and tooltip windows are `xdg_popup`s placed relative
  to their parent (menus grab input and close on outside clicks). Requests
  without a Wayland protocol (always-on-top, borders size; opacity and icon
  when the compositor lacks alpha-modifier / xdg-toplevel-icon) report "That
  operation is not supported", as in SDL3. Optional protocols used when the
  compositor offers them: xdg-decoration (server-side borders), xdg-activation
  (raise/flash), idle-inhibit (screensaver), keyboard-shortcuts-inhibit
  (keyboard grab), pointer constraints (mouse grab, mouse rect, relative
  mode), alpha-modifier (opacity), xdg-toplevel-icon, xdg-dialog (modal),
  xdg-foreign (`SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_EXPORT_HANDLE_STRING`,
  the file chooser portal's parent).
  `SDL_WINDOW_TRANSPARENT` windows get ARGB8888 shm buffers and window
  surfaces (premultiplied alpha, as in stock), start fully transparent
  (`SDLop_SetWindowClearColor` makes the clear opaque) and EGL configs with
  alpha (`SDL_GL_ALPHA_SIZE` 8 when it was 0); every other window that is
  fully opaque sets a `wl_surface` opaque region so the compositor can skip
  blending. GL rendered through the offload fallback (no native EGL window)
  is still presented opaque.
- X11 specifics: EWMH (`_NET_WM_STATE`) is used for fullscreen/maximize when a
  WM advertises it; without a WM SDLop emulates both (borderless
  display-sized window, like SDL3's legacy path). Key repeat is detected via
  XKB detectable auto-repeat (timestamp pairing as fallback) and generated
  client-side like on Wayland. Relative mouse mode = XInput2 raw motion +
  confined pointer grab + blank cursor; warp-to-center without XInput2.
  Content scale follows `SDL_VIDEO_X11_SCALING_FACTOR`, `Xft.dpi`, then
  `GDK_SCALE`. Desktop OpenGL uses GLX like stock SDL3 (loaders such as
  GLEW need `glXGetCurrentDisplay()`), on a private Xlib connection.
  OpenGL ES uses GLX as well when the server has
  `GLX_EXT_create_context_es_profile` (any ES version; the older
  `GLX_EXT_create_context_es2_profile` alone: ES 2.0) on top of
  `GLX_ARB_create_context_profile`, which avoids an EGL detour on drivers
  whose GLX path is the fast one. Unlike stock, the ES version is not capped
  by `GL_ARB_ES3_compatibility`: the driver is asked, and a version it
  refuses falls back to EGL (on every EGL path, X11/Wayland/KMSDRM, the ES
  version is passed like stock: X.0 as `EGL_CONTEXT_CLIENT_VERSION`, X.Y
  through `EGL_KHR_create_context`, so an unavailable version fails instead
  of returning a different one). `SDL_VIDEO_FORCE_EGL=1` (everything),
  `SDL_OPENGL_ES_DRIVER=1` (GLES only) and systems without usable GLX use
  EGL (`EGL_PLATFORM_XCB_EXT`, else `EGL_PLATFORM_X11_EXT`); `SDL_EGL_*`
  return NULL for GLX contexts, as in stock. GLX contexts honour
  `SDL_GL_CONTEXT_NO_ERROR` (`GLX_ARB_create_context_no_error`),
  `SDL_GL_CONTEXT_ROBUST_ACCESS_FLAG` / `SDL_GL_CONTEXT_RESET_NOTIFICATION`
  (`GLX_ARB_create_context_robustness`) and
  `SDL_GL_CONTEXT_RELEASE_BEHAVIOR` (`GLX_ARB_context_flush_control`), each
  only when the server has the extension (otherwise ignored). Framebuffer
  attributes (`SDL_GL_ALPHA_SIZE`, `SDL_GL_DEPTH_SIZE`, `SDL_GL_STENCIL_SIZE`,
  ...) pick the driver's best `glXChooseFBConfig` match and OpenGL windows
  are created with that config's X visual, as in stock SDL3 / GLFW (RGB888 +
  depth 0 gets exactly that; set them before `SDL_CreateWindow`).
  `SDL_GL_GetAttribute` reports the realised sizes of the current context
  (all backends), like stock. Swap control:
  `GLX_EXT_swap_control` (+ `_tear` for adaptive vsync, interval -1), else
  MESA, else SGI; as in stock SDL3, `SDL_GL_SetSwapInterval` fails with "not
  supported" when there is none (e.g. Xvfb).
  `SDLOP_X11_NO_SHM=1` disables MIT-SHM. Layout switches (`setxkbmap`, XKB
  group changes) are followed live and reported as
  `SDL_EVENT_KEYMAP_CHANGED`; without libxcb-xkb only full keymap changes
  are seen, not group switches.
- Keyboard layouts (Wayland, X11, evdev): keycodes follow the active xkb
  layout with SDL3's rules: `SDL_EVENT_KEY_DOWN` carries the unshifted key,
  non-Latin layouts (ru, el, ...) keep the US-QWERTY letter keycodes, and
  AZERTY keeps `SDLK_1`... on the number row while
  `SDL_GetKeyFromScancode(..., false)` reports the layout's symbol
  (`й`, `&`, `é`...). Dead keys and the Compose key are composed into text via
  xkbcommon's compose tables (locale from `LC_ALL`, `LC_CTYPE`, `LANG`, else
  `C`; needs libxkbcommon >= 0.5 and the X11 Compose data, otherwise text is
  emitted uncomposed).
- Input methods (`SDL_StartTextInput`): X11 uses XIM (as stock SDL3).
  Wayland uses `zwp_text_input_v3`; when the compositor doesn't offer it,
  Fcitx 5 or IBus is used over D-Bus (their session-bus portals), as stock
  SDL3 does. The choice follows stock's `SDL_IM_MODULE` (`fcitx`, `ibus`,
  anything else turns it off), then `XMODIFIERS` / `GTK_IM_MODULE` /
  `QT_IM_MODULE`; with none of those set, only an input method that is
  already running is used. Nothing touches the bus until text input starts,
  and keys go to the input method only while text input is active.
  Committed text arrives as `SDL_EVENT_TEXT_INPUT` and the preedit as
  `SDL_EVENT_TEXT_EDITING`. Key events are still delivered for keys the
  input method takes, just without their own text.
- Touch: `wl_touch` on Wayland and XInput 2.2 multitouch on X11 deliver
  `SDL_EVENT_FINGER_*` (x/y/dx/dy normalized to the window, pressure 1 while
  down; on X11 touchID = source device, fingerID = touch id). Like SDL3, the
  first finger also drives a left-button mouse with
  `which == SDL_TOUCH_MOUSEID` (the window systems stop emulating the pointer
  once a client takes touch events); `SDL_TOUCH_MOUSE_EVENTS=0` in the
  environment turns that off. `SDLOP_X11_NO_TOUCH=1` leaves the touch
  selection (and thus the server's own pointer emulation) to X11.
- Relative mouse mode uses real pointer lock (`zwp_pointer_constraints` +
  `zwp_relative_pointer`) on Wayland when the compositor has a seat/pointer,
  and raw evdev deltas when `SDLOP_RAW_INPUT=1` made evdev the input source;
  otherwise deltas come from the relative-pointer protocol.
- Window decorations: X11 uses `_MOTIF_WM_HINTS`; Wayland asks for
  server-side decorations via xdg-decoration (borderless windows request
  client-side mode, and SDLop draws none). Compositors without
  xdg-decoration (e.g. GNOME) show no title bar.
- Wayland software surfaces: the `SDL_GetWindowSurface` pixels are a
  client-side XRGB8888 buffer at a fixed address. Each
  `SDL_UpdateWindowSurface(Rects)` copies the damage into a `wl_shm` buffer
  the compositor has released, then attaches and commits it. Each buffer
  tracks what it has missed, so a present copies only the new damage plus
  the previous frame's. The cycle is two buffers, and a third only while the
  compositor holds both. A present waits for a release only when all three are
  held. That wait dispatches only buffer events, never input or window events.
  Until something is presented, a window shows its clear colour: a 1×1
  buffer scaled with `wp_viewporter`, or a filled full-size buffer when the
  compositor lacks it or `SDLOP_WAYLAND_NO_VIEWPORTER=1`. GL offload frames
  use the same buffer cycle. `test_wayland_buffers` checks all of this
  against a strict test compositor. The GL and Vulkan paths were validated
  against Mesa's software rasterizers (llvmpipe/lavapipe) but work with any
  EGL/Vulkan driver.
- Hardware GPU offload (OpenGL via EGL surfaceless/pbuffer on a hardware
  DRM render node; Vulkan via `VK_EXT_headless_surface` with hardware GPU
  prioritization) activates automatically when the display backend lacks
  native hardware presentation (e.g. `dummy`, `wl_shm`-only Wayland, or
  KMSDRM where the `VK_KHR_display` display/mode/plane/surface probe fails),
  presenting frames back through the window framebuffer (`wl_shm`, DRM dumb
  buffers, or `SDL_GetWindowSurface`). On KMSDRM, auto mode prefers native
  Vulkan WSI when that full capability probe succeeds; the NVIDIA proprietary
  driver is kept on the safe offload path unless selected explicitly with
  `0`/`off`.
  Configure via `SDLOP_OFFLOAD` (global default: `auto`, `0`/`off`/`false`/`no`,
  or `1`/`on`/`true`/`yes`/`force`), overridden per API by
  `SDLOP_GL_OFFLOAD` and `SDLOP_VULKAN_OFFLOAD` (or `SDLOP_VK_OFFLOAD`).
  Like every SDLop option these are also hints: `SDL_SetHint(SDLOP_HINT_GL_OFFLOAD, "0")`
  etc. (environment over a normal hint, `SDL_HINT_OVERRIDE` over both).
- `SDLop_SetWindowClearColor` tints a window that has no surface/GL/Vulkan
  content yet. If a window surface exists, it fills the surface and presents
  it (X11 and Wayland).
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
