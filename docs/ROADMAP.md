# SDLop roadmap

Two things are being tracked here:

1. **Platforms** — SDL3 supports a lot of systems; this is the plan for each one.
2. **Work items** — what is missing in the Linux/Wayland build that is being
   written now, and what should happen after that.

Status legend: **done** (implemented and verified on this machine), **WIP**
(branch of work started), **planned** (designed, not started), **n/a**
(intentionally never — outside the "window + input + timing" scope).

---

## 1. Platforms

The list below is exactly SDL3 3.2.10's
[docs/README-platforms.md](https://github.com/libsdl-org/SDL/blob/release-3.2.10/docs/README-platforms.md),
so nothing SDL3 supports is missing from the plan.

### Desktop Linux (the current target)

| Backend | Status | Notes |
|---|---|---|
| **Wayland** | **done** | xdg-shell, `wl_shm` XRGB8888 + viewporter for HiDPI, cursor-shape-v1, wl_egl_window for GL, `vkCreateWaylandSurfaceKHR` for Vulkan, xkbcommon keymaps (compositor / local XKB / `SDLOP_XKB_KEYMAP` override, shared with the X11 backend in `src/input/SDL_xkb.c`), client-side key repeat, `wl_surface_frame` pacing, wl_output v4, `zxdg_output_v1` display geometry + hotplug, `zwp_pointer_constraints_v1` + `zwp_relative_pointer_v1` for relative mouse mode / mouse grab / mouse rect. Verified under weston (X11 backend) and sway (headless, two outputs, one of them scale 2) — display geometry, content scale and window scale match stock SDL3 exactly on both, and against `xdotool`-driven real key and pointer events. |
| **X11** | **done** | Window creation/present plus the **input fallback** for when `/dev/input` is unreadable (XWayland, Flatpak, sandbox, remote session): `src/video/SDL_x11.c` owns the X connection, EWMH/`_MOTIF_WM_HINTS` window state, MIT-SHM present (with `XPutImage` when the extension is missing), RandR displays (with the screen as fallback), XShape hit tests, 1-bit cursors, grabs/relative mode/capture, and the input translation (core + XInput2 events, the server's XKB keymap through `xkbcommon-x11`, detectable auto-repeat). The X11 branches of `src/gl/SDL_egl.c` and the Xlib Vulkan WSI came back with it. Verified with `make x11-check` (25 checks, Xvfb + `xdotool`) and against stock SDL3 on the same X server, event line for event line. |
| **KMSDRM** (console, no compositor) | planned | Owns the master plane and the input devices; a good fit for the SDLop design because it removes both a compositor and a window manager from the loop — one place where SDL3's session-management code is a lot of machinery. |
| **SteamOS** | planned | Gamescope presents Wayland; a SteamOS port is "Wayland with a session script", so it is really a packaging + testing task once the Wayland backend handles the gamescope quirks (`wp_presentation`, tearing control, fractional scaling). |
| **Raspberry Pi / VideoCore** | planned | X11/Wayland on the Pi needs `rpi`-specific window sizing and the legacy dispmanx path only for very old images; the plan is KMSDRM first. |
| Emscripten (web), Android | planned | Both need a different input source anyway (DOM events / `ALooper`), so they slot into the same "raw record producer" interface the evdev worker implements. |

### Other desktop and mobile systems

| Platform | Status | Notes |
|---|---|---|
| Windows (Win32) | planned | `CreateWindowEx` + `RAWINPUT`/`WM_INPUT` — the same async model: a worker thread reading raw input, the window thread only presents. |
| Windows GDK / Xbox | planned | GDK input APIs behind the same producer interface. |
| macOS | planned | Cocoa + `CGEventTap`/IOKit; the async worker owns an IOHIDManager, the main thread runs the run loop. |
| iOS, tvOS | planned | UIKit touch/`UIKey` input; no window manager, so windowing reduces to a `UIView` layer. |
| FreeBSD, NetBSD, OpenBSD | planned | Wayland/X11 backends should compile unchanged where the headers allow (evdev is available on all three, with `kqueue`/`/dev/input` differences). |
| Haiku OS | planned | Its own app server API; a small backend on top of the shared video core. |
| RISC OS | planned | Very different event model; lowest priority. |
| Nintendo Switch, Nintendo 3DS | planned | Console ports go through the platform SDKs; touch/sensors are out of scope, so only window + input + timing are needed. |
| PlayStation 2 / 4 / 5, PSP, Vita | planned | PS2/PSP have no windowing in the desktop sense: the backend would be a "display + pad" driver over the same core. |

### Systems SDL3 does *not* support

Google Stadia, NaCL, Nokia N-Gage, OS/2, QNX, WinPhone, WinRT/UWP: SDL3 dropped
them, SDL2-only, so SDLop does not target them either. (They stay reachable
through SDL2.)

### Adding a platform: what a port has to provide

The whole surface a new backend has to fill is one vtable
(`SDLOP_VideoDriver`, see `src/sdlop_internal.h`): create/destroy/show/hide a
window, resize, present a pixel buffer, report displays, pump platform events,
and expose an event descriptor plus a wakeup callback for `SDL_WaitEvent`.
Input comes from an independent *record producer* — the evdev worker, an XInput2
reader, a Cocoa event tap, DOM events — which pushes timestamped records into the
lock-free ring. Porting therefore does not touch the core, the event queue, the
keyboard/mouse layers or any of the `src/generated/` tables.

---

## 2. Work items

### Now (Linux/Wayland build)

- [x] Public headers: 21 compared headers identical to SDL3 3.2.10, 112 drops documented.
- [x] Event queue with filters, watches, `SDL_WaitEvent` on a poll set + wakeup fd.
- [x] Async evdev input worker (epoll, kernel timestamps, lock-free SPSC ring,
      no libudev, 1 s `/dev/input` rescan) with the FIFO test hook used by
      `tests/test_input.c`.
- [x] Wayland windowing verified end-to-end (xdg-shell, shm buffers, viewporter,
      cursor shapes, GL via `wl_egl_window`).
- [x] `SDL_GL_*` (EGL) and `SDL_Vulkan_*` (loader + Wayland surface) — verified
      under weston with lavapipe/llvmpipe.
- [x] `bench/` versus real SDL3, and the two hot-path fixes it found.
- [x] **X11 backend** (`src/video/SDL_x11.c`): window, present, and the input
      fallback for when `/dev/input` is not readable; the X11 branches of
      `src/gl/SDL_egl.c` and the Xlib Vulkan WSI restored in the same change.
      Sourcing is per session, not per build: the evdev worker is the input
      producer when `/dev/input` is readable, otherwise the X11 reader takes over
      (and `SDLOP_TEST_INPUT` overrides both for tests).
- [x] SIGINT/SIGTERM become `SDL_EVENT_QUIT` in the pump instead of killing the
      process, and the signal wakes a blocked `SDL_WaitEvent()` through the
      wakeup fd; `SDL_HINT_NO_SIGNAL_HANDLERS` is honoured, and a handler the
      application installed itself is left alone.
- [x] `SDL_PollEvent()` pumps only when the queue is empty (SDL3's own fast
      path): a poll loop no longer pays for a poll(2) per event.
- [x] X11 rig (`tests/x11_input.c` + `tests/x11_input.sh`, `make x11-check`):
      `xdotool`-driven, no window manager needed, 25 checks over enter/motion/
      button/wheel/leave, scancodes/keycodes/mods/text, held-key repeat (with the
      server's auto-repeat both on and off), server-side resize/move and the
      platform properties.
- [ ] **Input latency harness**: measure record-arrival → `SDL_PollEvent` return
      through the real evdev path (needs a machine with `/dev/input`, or a
      `uinput` device created by the test itself).
- [ ] `uinput`-driven test that creates a virtual keyboard/mouse, so CI exercises
      the evdev → SDL event path without hardware.
- [x] XKB keymap loading with xkbcommon: the compositor's `wl_keyboard.keymap`
      is compiled and registered as the keyboard layout (`SDLOP_KeyLayout`), with
      three fallbacks — an explicit `SDLOP_XKB_KEYMAP=<file>` override, the
      local XKB configuration (`XKB_DEFAULT_LAYOUT`, for headless/KMS
      compositors that never send a keymap), and finally SDL's built-in tables.
      Because keys arrive from the evdev worker rather than from the compositor,
      the layout is told about every key event so its own modifier state follows.
      `SDLOP_XKB_DUMP=<path>` writes out what was loaded, whichever source won
      (compositor keymap, X server keymap, override file or the local rules).
      The local fallback is compiled on the first key that needs it rather than
      at init — it costs ~2 ms and is usually superseded, which the benchmark
      caught as a 0.13x `SDL_Init` (see PERFORMANCE.md).
- [x] Client-side key repeat (`wl_keyboard.repeat_info` + `xkb_keymap_key_repeats`),
      including waking a blocked `SDL_WaitEvent()` in time for the next repeat.
- [x] `wl_surface_frame` present pacing, so a buffer is never attached while the
      compositor is still displaying it.
- [x] Display enumeration from `zxdg_output_v1`: logical position/size in the
      global compositing space, native scale factor from the mode/logical ratio,
      `wl_output.done` counting (the xdg-output `done` is deprecated from manager
      v3, so completion is `1 + (xdg_output != NULL)` output `done` events).
- [x] Display hotplug: `registry_global_remove` releases the output and removes
      its display, windows are re-homed, and display IDs are handed out once and
      never reused (a window that outlives an unplugged monitor must not land on
      a different display). Output slots are never compacted either — the
      compositor holds a pointer to the slot as its listener data.
- [x] SDL3's scale model, matched field by field against stock SDL3 3.2.10 under
      sway with a scale-2 output: displays are logical (`512x384` for a
      `1024x768@2` output), `SDL_GetDisplayContentScale()` stays 1.0 unless
      `SDL_HINT_VIDEO_WAYLAND_SCALE_TO_DISPLAY` is set, and a window renders at
      the display scale only with `SDL_WINDOW_HIGH_PIXEL_DENSITY`.
- [x] `zwp_pointer_constraints_v1` + `zwp_relative_pointer_v1`: relative mouse
      mode locks the pointer on every window and reports unaccelerated deltas,
      `SDL_SetWindowMouseGrab` / `SDL_SetWindowMouseRect` confine the pointer
      (with a `wl_region` for the rect), and every transition destroys the
      constraint object that would otherwise be a protocol error.
- [x] Window surfaces follow the window: `SDL_GetWindowSurface()` hands back the
      same surface, resized, after `SDL_SetWindowSize()` or a pixel-size change,
      like SDL3 does.
- [x] Window ↔ display tracking from `wl_surface.enter`/`leave`: the compositor
      is the authority on which output a window is shown on, so the window's
      display (and `SDL_EVENT_WINDOW_DISPLAY_CHANGED`) follows the outputs it
      reports, and a window whose output is unplugged is re-homed exactly like
      SDL3 does it. Before this it was a guess from the window's synthetic
      position, which was wrong whenever the compositor placed the window
      somewhere else (a tiling compositor always does).
- [x] Display naming matched to SDL3: the name is resolved once per display from
      `wl_output.description`, falling back to the `zxdg_output_v1` description
      (below wl_output v4 only) and then the geometry model string, and it is
      never rewritten afterwards, so a display cannot be renamed under an ID the
      application already holds. A display that has not been described yet sits
      at 0,0 and reports no mode instead of an invented offset/size, and
      `SDL_GetDesktopDisplayMode()` re-queries the backend — both as in SDL3.
- [ ] Touch and tablet input (evdev `ABS_MT_*`, `EV_ABS` slots) — the record ring
      already carries them; only translation is missing.

### Next

- [ ] Gamepad/joystick — explicitly **out of scope** for now; if it ever comes
      back it belongs in the same producer as a `/dev/input/js*` reader.
- [ ] `wp_presentation` + tearing-control so frame pacing can be measured and
      VRR used on Wayland.
- [ ] Fractional scale: `wp_fractional_scale_v1` is advertised by sway and
      `wl_surface.preferred_buffer_scale` by v6 compositors; viewporter's
      `set_source` is the other half (a 1.5x display needs a scaled source rect,
      not just an integer multiplier). `wl_surface.enter`/`leave` now track which
      outputs a window is on (see *Now*), so the per-window scale factor can be
      derived from those outputs the way SDL3's `Wayland_MaybeUpdateScaleFactor`
      does.
- [ ] `zwp_input_timestamps_v1`: ask the compositor for the real event timestamps
      (the backend currently stamps events with the local clock when they are
      read, like the rest of the pump).
- GLX is deliberately not planned (not a TODO): EGL through
      `eglGetPlatformDisplay(EGL_PLATFORM_X11_EXT)` covers the same machines with
      less code, and that is what `src/gl/SDL_egl.c` does on X11 as well.
- [ ] Headless input rig for non-wlroots compositors: `zwlr_virtual_pointer_v1`
      covers sway/wlroots (`tests/wayland_input.sh`); weston's `weston-test`
      protocol or a `uinput` device would cover the rest, and would also let the
      pointer path be tested with more than one pointer or an absolute device.
- [ ] CI: build matrix (`WAYLAND=0 EGL=0`, `DEBUG=1`, clang), `make check` under
      weston headless, `check_api.py`, and a bench run that fails on regression.
- [x] Header-level ABI check: `tools/abi_probe.c` is compiled twice — against
      SDLop's headers and against stock SDL3's — and prints sizes, member offsets,
      event/scancode/keycode values, flags and hint strings; `make abi-check`
      diffs the two. 208 values, currently identical: struct sizes and member
      offsets (including every `SDL_Event` union member, and the keyboard/mouse
      device and text-editing event structs), event/scancode/keycode values, flags,
      and hint strings. The union members are checked because a size check passes
      while a member is missing — which is exactly how `SDL_TouchFingerEvent` was
      found to be absent here — and the device events are checked as an ordered
      run, because a wrong position in the enum shifts everything after it.
- [x] Link-level ABI check: `tools/link_probe.c` is an ordinary SDL3 program (no
      SDLop-specific calls, no `#ifdef`s) compiled against the **system's** SDL3
      headers and linked against `libSDLop.so`, then compiled again against
      SDLop's own headers and linked against `libSDLop.a`; `make link-check` runs
      both on every driver the session has and diffs their output. It is the one
      check that catches a function that is declared, laid out correctly, and not
      *exported* — the reason the library has to keep every symbol the subset
      promises. Where the numbers are the session's rather than the program's (a
      tiling compositor choosing the window size) the probe prints a verdict
      instead of the numbers, so the Wayland leg compares what it can.
- [x] Behaviour diff against stock SDL3: `tools/behaviour_probe.c` compiled
      against both libraries, driven with the same scripted input, traces diffed
      by `make behaviour-check` (see *Comparing against stock SDL3* in
      ARCHITECTURE.md). Input events and window operations match in order on a US
      and a German layout, and on the reference rig the window-lifecycle events do
      too - they are compared as a set, because their order is up to the server.
      What it
      caught has been fixed, and the one remaining difference is reported as a
      note: `SDL_GetKeyFromName()` for punctuation the layout shifts (stock
      answers with the key that types it - German `?` is 0xdf - SDLop answers
      with its US table). SDLop's name tables are layout-independent by
      construction; stock's answer needs a keycode -> scancode lookup in the
      active keymap (`SDL_GetKeymapScancode`), which the key layout interface
      here does not have yet.
- [ ] `SDL_GetKeyFromName()` for layout-shifted punctuation: the lookup stock
      does in its keymap (`keycode -> scancode + modstate`, then the key's
      unshifted keycode). It needs a reverse lookup in `SDLOP_KeyLayout`
      (`SDL_xkb.c` has the level tables to answer it) - worth doing when an
      application needs it, because the current answer is wrong on any layout
      that is not US.

### Later

- [ ] Windows, macOS, Android, Emscripten backends (see the tables above).
- [ ] KMSDRM backend for console use.
- [ ] Optional `SDL_LoadObject`/threads subset *only if* a real application needs
      it — the SDLop build deliberately ships neither.
