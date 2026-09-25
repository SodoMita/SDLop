# SDLop

A lean reimplementation of the **SDL3 API** — windowing, input and timing only —
aimed at being *faster than SDL3* and free of the modules a game that already
brings its own renderer, audio mixer and physics engine never calls.

It is not a fork: `include/SDL3/*.h` declares the same names, types, enums and
signatures as SDL3, so an application that uses windowing, input, timing, GL and
Vulkan survives a recompile against SDLop unchanged. Everything outside that
core is deliberately absent, and the linker says so.

```
    SDL3 3.2.10 (system)          SDLop 0.1.0
    --------------------          -------------
    ~1208 exported SDL_* symbols  ~398 exported SDL_* symbols
    2.87 MB shared library        0.23 MB stripped shared library
```

* **API target:** SDL 3.2.10 — the exact version whose headers are used as the
  reference (`tools/check_api.py` diffs every header against them).
* **Platforms:** Wayland (Linux) today, `offscreen` for headless testing; see
  [docs/ROADMAP.md](docs/ROADMAP.md) for the full SDL3 platform matrix and the
  plan for each one.
* **Input model:** a background thread reads `/dev/input/event*` with `epoll` and
  hands *kernel-timestamped* records to the main thread through a lock-free ring
  — the model [asyncinput](https://github.com/CoCkMelon/asyncinput) uses — so a
  blocked `SDL_WaitEvent()` wakes up on the input itself, not on a poll timeout.

## Build

No cmake, no configure: one Makefile that detects what is installed.

```sh
make -j4            # build/libSDLop.a and build/libSDLop.so
make tests          # build the test programs
make check          # build and run them
make bench          # build the SDL3-vs-SDLop benchmark (needs real SDL3)
make abi-check      # layouts/constants vs stock SDL3 headers (needs libsdl3-dev)
make x11-check      # drive the X11 backend with xdotool (needs an X server)
make wayland-check  # drive the Wayland backend through wl_inject (needs sway)
make install        # headers + libraries + sdl3-sdlop.pc
```

Optional dependencies are picked up with `pkg-config` and enabled per `-D` flag,
so a machine without Wayland, EGL or xkbcommon still builds everything else:

```sh
make WAYLAND=0 EGL=0       # offscreen-only build
make X11=0                 # without libX11
make DEBUG=1               # -O0 -DSDLOP_DEBUG
make CC=clang OPT=-O3
```

Both Linux backends are on by default when their libraries are installed:
Wayland uses `wayland-client`, `wayland-egl` and `xkbcommon`; X11 adds `x11`,
`xext`, `xi`, `xrandr`, `xkbcommon-x11` and `x11-xcb`. X11's optional pieces are
probed separately — `XSHM=1|0`, and XInput2/RandR/XShape as well as the
`xkbcommon-x11` keymap source compile in only when their headers are found
(`make X11XCB=0` drops the server keymap and falls back to the local XKB
configuration).

Runnable example:

```sh
SDL_VIDEODRIVER=offscreen ./build/examples/hello --frames 2
XDG_RUNTIME_DIR=/tmp/xdg weston --backend=headless-backend.so --socket=sdlop-wl &
WAYLAND_DISPLAY=sdlop-wl SDL_VIDEODRIVER=wayland ./build/examples/hello --frames 40
```

Environment variables that exist for testing and for stubborn machines:

| Variable | Effect |
|---|---|
| `SDLOP_TEST_INPUT=<fifo>` | Feed input records (`EV_KEY 30 1`) through the async ring without `/dev/input`; what the input tests use. |
| `SDLOP_XKB_KEYMAP=<file>` | Use this XKB keymap file instead of the compositor's / the X server's. Also the way to get a non-US layout from a compositor that sends the wrong one. |
| `SDLOP_XKB_DUMP=<path>` | Write out the keymap that was loaded (whichever source won), so layout bugs are debuggable. |
| `SDL_HINT_NO_SIGNAL_HANDLERS=1` | The stock SDL3 hint, honoured: do not turn SIGINT/SIGTERM into `SDL_EVENT_QUIT`, leave the default handlers alone. |
| `SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY=1` | The stock SDL3 hint, honoured: displays, window surfaces and content scale switch to physical pixels instead of letting the compositor upscale a 1x surface. |

## What is implemented

| Area | Contents |
|------|----------|
| Core | `SDL_Init`/`SDL_Quit`, errors, logging, asserts, properties (with cleanup callbacks), hints, version, `SDL_stdinc` subset, rects |
| Timing | monotonic ticks and performance counter, `SDL_Delay`, `SDL_AddTimer`/`SDL_RemoveTimer` |
| Events | 512-event queue, filters and watches, `SDL_PollEvent`/`SDL_WaitEvent`/`SDL_PeepEvents`, user events, main-thread callbacks |
| Input | keyboard (scancodes, keycodes, mods, text input), mouse (buttons, motion, wheel, relative mode, grab/mouse-rect, capture), focus handling, async evdev worker, and the X11 fallback reader (XInput2 raw motion + core events, server-side auto-repeat, the server's XKB keymap) for a session where `/dev/input` is not readable |
| Video | window create/destroy/state, displays and modes from `zxdg_output_v1` (logical geometry, HiDPI scale, hotplug), window surfaces that follow the window size (`SDL_GetWindowSurface`, `SDL_UpdateWindowSurface`), cursors |
| GL | `SDL_GL_*` through dlopen'ed EGL (Wayland and X11 platform displays) |
| Vulkan | `SDL_Vulkan_*` through the dlopen'ed Vulkan loader, `vkCreateWaylandSurfaceKHR` and `vkCreateXlibSurfaceKHR` |
| Backends | `wayland`, `x11`, `offscreen` |

Deliberately **not** implemented (link errors are the intended outcome):
audio, renderer, camera, joystick/gamepad, HIDAPI, sensors, dialogs, message
boxes, clipboard, filesystem, locale, threads/mutexes, `SDL_LoadObject`,
`.ini`/`.bmp`/`.wav` loaders, and the rest of upstream. `tools/dropped.txt` lists
every dropped declaration (112 of them were not kept on purpose).

## Status

Verified on this machine (gcc 14.2.0, Linux; Xvfb, weston 14.0.2 X11 backend and
sway 1.10.1 headless with two outputs, one of them at scale 2;
lavapipe/llvmpipe):

| Check | Result |
|-------|--------|
| `python3 tools/check_api.py --lib build/libSDLop.so` | 21/21 headers identical to SDL3 3.2.10, every declared function exported |
| `test_core` (offscreen, wayland **and** x11) | 118 checks, 0 failures |
| `test_input` (offscreen **and** wayland **and** x11; FIFO record feed) | 93 checks, 0 failures — passes on both a US and a French keyboard layout |
| `test_video` (offscreen **and** wayland **and** x11) | 91 checks, 0 failures (102 under sway's two outputs) |
| `test_gl` (weston, x11) | 28 checks, 0 failures — EGL context *and* Vulkan surface created and destroyed |
| `examples/hello` (weston, sway, Xvfb) | frames rendered and presented, exit 0 |
| `tests/x11_input.sh` (`make x11-check`, Xvfb + `xdotool`) | 25 checks, 0 failures — enter/motion/button/wheel/leave, scancodes, keycodes, modifiers, `SDL_EVENT_TEXT_INPUT`, shift-a, Ctrl suppression, held-key repeat (and the same with the server's auto-repeat switched off), resize/move from the X server, the platform properties, display bounds against `xrandr` |
| Key and pointer stream vs stock SDL3 on X11 (`xdotool`-driven, same script) | identical scancodes, keycodes, modifiers, text, buttons, wheel and focus events; the only differences are the order of the first four lifecycle events and one extra motion event stock sends on a button press |
| Real key events (`xdotool` into weston's X11 backend) | correct scancodes, keycodes (shift uppercases), `SDL_EVENT_TEXT_INPUT`, Ctrl-suppression, and client-side key repeat |
| Display geometry vs stock SDL3 (sway, outputs `800x600@1` at 0,0 and `1024x768@2` at 800,0) | identical: bounds, current mode, `pixel_density`, content scale — `512x384` logical for the scale-2 output, content scale 1.0 unless `SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY` |
| `tests/wayland_input.sh` (`make wayland-check`, sway) | 9 checks, 0 failures — enter/motion/button/wheel/leave plus relative-mode deltas, injected with `tools/wl_inject` |
| `tests/wayland_display.sh` (`make wayland-check`, sway) | 12 checks, 0 failures — two outputs, logical vs physical geometry, unplug/replug, the window's display tracking the output it is really on |
| Display hotplug (`swaymsg output <name> unplug` + replug) | `SDL_EVENT_DISPLAY_REMOVED`/`ADDED`, windows re-homed, no reused display IDs, new output's geometry applied |
| Window ↔ display tracking | `wl_surface.enter`/`leave` decide the window's display (like SDL3), so `SDL_EVENT_WINDOW_DISPLAY_CHANGED` arrives when the compositor moves the window, including after an unplug |
| Relative mouse mode (`xdotool` into weston's X11 backend) | pointer locked (`zwp_locked_pointer_v1`), unaccelerated deltas arrive as `xrel/yrel`, unlock on demand; grab/mouse-rect confine and lock/confine transitions produce no protocol errors |

`make regen` reproduces `include/` and `src/generated/` byte for byte, so the
generated headers in the repository are exactly what `tools/sdlop.py` produces.

Testing the compositor input path needs a compositor that has a seat, which means
weston's X11 backend plus a virtual X server:

```sh
mkdir -p /tmp/xdg-run && chmod 700 /tmp/xdg-run
nohup Xvfb :99 -screen 0 1024x768x24 &
DISPLAY=:99 XDG_RUNTIME_DIR=/tmp/xdg-run weston --backend=x11-backend.so --socket=sdlop-wl &
export XDG_RUNTIME_DIR=/tmp/xdg-run WAYLAND_DISPLAY=sdlop-wl SDL_VIDEODRIVER=wayland
DISPLAY=:99 xdotool mousemove 400 300 click 1 key a   # the click gives the surface focus
```

`weston --backend=headless-backend.so` is enough for the window/video/GL tests
but exposes no `wl_seat`, so it never sends a keymap or key events at all.

Display work (multi-output, HiDPI, hotplug) needs a compositor that can describe
more than one output, which weston cannot. sway can:

```sh
pkill weston   # one compositor at a time
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER_ALLOW_SOFTWARE=1 \
WLR_HEADLESS_OUTPUTS=2 sway -c /dev/null -d &
swaymsg output HEADLESS-1 mode 800x600  scale 1 position 0   0
swaymsg output HEADLESS-2 mode 1024x768 scale 2 position 800 0   # 512x384 logical
swaymsg output HEADLESS-2 unplug                                 # hotplug check
```

Headless sway has no input devices at all (`wl_seat.capabilities(0)`), which used
to mean pointer behaviour could only be tested against weston's X11 backend with
`xdotool`. wlroots compositors do have `zwlr_virtual_pointer_manager_v1`, so
`tools/wl_inject` (built by `make inject`, dev-only: nothing in `src/` uses it)
can drive a headless session itself:

```sh
make inject                       # build/tools/wl_inject
WAYLAND_DISPLAY=wayland-1 ./build/tools/wl_inject --check   # can this compositor be driven?
printf 'sleep 200\ncursor 300 260\nmove 12 7\nbutton left down\n' > /tmp/in.s
WAYLAND_DISPLAY=wayland-1 ./build/tools/wl_inject /tmp/in.s  # sleep/cursor/move/button/click/wheel/flush
```

`make x11-check` drives the X11 backend from the outside: `tests/x11_input.c`
reports where its window is (and what the backend told it about the display),
`tests/x11_input.sh` moves the pointer and types at it with `xdotool`, and the
client's event log is asserted line by line (25 checks). Nothing is assumed about
the X server — not even that a window manager is running:

```sh
Xvfb :99 -screen 0 1280x800x24 &
DISPLAY=:99 make x11-check
```

`make wayland-check` runs both scripted rigs: `tests/wayland_input.sh` (absolute
and relative pointer, 9 checks) and `tests/wayland_display.sh` (two outputs,
HiDPI, hotplug, 12 checks). Both are self-contained — they rebuild the sway
session's outputs through `swaymsg`, find the client window's real position in
the compositor's tree instead of assuming one, inject events in compositor
coordinates, and read the client's event log. Against a compositor without the
virtual-pointer protocol (weston) or without swaymsg they skip instead of
failing. Both are excluded from `make check`/`TESTS` because they need a sway
session and a few seconds of wall clock.

That rig found a real difference rather than just testing SDLop: under headless
sway **stock SDL3 3.2.10 receives no pointer events at all** (its shared-memory
path uses `wp_alpha_modifier` and sway reports the surface as unmapped), while
SDLop maps and receives them. SDLop is not "more correct" there — it simply
uses fewer protocols on that path.

## Performance

`bench/bench_compare` links stock SDL3 and `dlopen`s SDLop, then runs the same
operations through both in one process (median of 5 runs each, `offscreen`
driver). SDL 3.2.10 vs SDLop, ns per operation:

| operation | SDL3 | SDLop | speedup |
|---|---:|---:|---:|
| `SDL_Init` + `SDL_Quit` | 24033 | 5986 | **4.0x** |
| `SDL_CreateWindow` + `SDL_DestroyWindow` | 29953 | 906 | **33.1x** |
| `SDL_PumpEvents` (nothing pending) | 40.2 | 11.1 | **3.6x** |
| `SDL_PushEvent` + `SDL_PollEvent` | 43.3 | 10.2 | **4.3x** |
| `SDL_GetTicks` | 32.8 | 27.4 | 1.2x |
| `SDL_GetPerformanceCounter` | 24.7 | 24.2 | 1.0x |
| `SDL_FillSurfaceRect` + `SDL_UpdateWindowSurface` | 2518572 | 59450 | **42.4x** |
| `SDL_GetKeyboardState` | 2.7 | 2.3 | 1.2x |
| `SDL_GetModState` | 2.4 | 1.8 | 1.3x |

Run on X11 (Xvfb, one real `X11` window and a real `XPutImage`/MIT-SHM present)
the same benchmark gives 1.4x for init, 8.4x for window create/destroy, 3.5x for
`SDL_PumpEvents`, 3.2x for push+poll and 11.5x for fill+present — the smaller
numbers are the X server round trips that both libraries share. The hot paths
that used to be *slower* than SDL3 (a `write()` to the wakeup eventfd on every
pushed event, a mutex taken by every `SDL_PumpEvents`, and a pump per polled
event) were found and removed this way — see
[docs/PERFORMANCE.md](docs/PERFORMANCE.md).

## Layout

```
include/SDL3/       24 public headers (19 generated from upstream SDL3 + 5 hand written)
src/core/           stdinc, errors, logging, asserts, properties, hints, init, version, rect
src/timer/          ticks, performance counter, timers
src/events/         event queue, filters/watches, waiting (poll + wakeup fd)
src/input/          async evdev worker, record -> SDL_Event translation, keyboard, mouse
src/video/          video core, window surfaces, Wayland and X11 backends, offscreen
                    backend, Vulkan surface creation
src/gl/             EGL driver behind SDL_GL_*
src/main/           SDL_RunApp / SDL_EnterAppMainCallbacks
src/generated/      generated tables (key names, pixel formats, evdev map, Wayland protocols)
tools/              header generator, API checker, table generators, wl_inject
                    (dev-only virtual-pointer client), upstream reference data
tests/              test_core, test_video, test_input, test_gl, and the
                    display-driven rigs: wayland_input / wayland_display (shared
                    session helpers in wayland_setup.sh) and x11_input
examples/           hello
bench/              SDL3-vs-SDLop benchmark
docs/               ARCHITECTURE.md, PERFORMANCE.md, ROADMAP.md
```

The internal contract (driver vtables, helpers shared between modules) lives in
`src/sdlop_internal.h`; see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for
how the pieces fit together and where data tables come from.

## Licence

The reimplementation is original code. Where it consumes upstream data — the
public headers, `SDL_scancode_names[]`, `scancodes_linux.h`, the Wayland protocol
XMLs — the upstream zlib licence applies to that material; see
`tools/reference/README.md`.
