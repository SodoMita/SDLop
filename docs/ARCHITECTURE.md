# Architecture

SDLop is one library of small modules with a single internal contract
(`src/sdlop_internal.h`). Nothing in `src/` includes another module's private
header: a module either implements public SDL3 API, or exposes `SDLOP_*` helpers
through the internal header.

```
                    public API (include/SDL3/*.h, SDL3 3.2.10 names & signatures)
                                        |
  +----------------+----------------+---+------------+----------------+-----------+
  | src/core       | src/timer      | src/events      | src/input     | src/video |
  | stdinc, error  | ticks, perf    | queue, filters  | async worker  | windows,  |
  | log, assert    | counter,       | watchers, wait  | record->event | displays, |
  | properties,    | SDL_AddTimer   | (poll+wakeup fd)| translation   | surfaces  |
  | hints, init    |                |                 |               |           |
  +----------------+----------------+-----------------+---------------+-----------+
                                        |
                              SDLOP_* internal contract
                                        |
             +--------------------------+---------------------------+
             |                          |                           |
      src/video/SDL_wayland.c    src/video/SDL_offscreen.c   src/gl/SDL_egl.c
      src/video/SDL_vulkan.c                                 (SDL_GL_*)
```

## 1. The video driver vtable

Everything platform-specific about windowing is one struct with function
pointers (`SDLOP_VideoDriver`): init/quit, create/destroy/show/hide a window,
resize/maximize/minimize/restore, present, cursor, displays, and two hooks the
event loop needs —

* `get_event_fd()` / `prepare_read()` — a descriptor `poll()` can block on (the
  Wayland socket) plus the call a backend must make immediately before blocking
  (Wayland has to flush its outgoing queue and claim the connection's read flag,
  or it can deadlock while events sit unflushed);
* `pump_events()` — drain whatever the platform has queued and turn it into
  `SDL_Event`s (`SDLOP_OnWindow*` helpers keep window state and events in sync).
  A backend that owns a socket has to *read* it here, not only in the blocking
  wait: an application that only ever calls `SDL_PollEvent()` would otherwise
  never read, the compositor's small outgoing queue would fill up, and it would
  drop the client;
* `get_event_timeout_ns()` — "I have work to do at a time of my own choosing",
  currently used by Wayland's key repeat so a blocked `SDL_WaitEvent()` wakes up
  in time for the next repeat instead of sleeping through it.

`SDL_PumpEvents()` is a fixed sequence:

```c
SDLOP_VideoPumpEvents();        /* platform events: window state first */
SDLOP_PumpRawInput();           /* then the async input records */
SDLOP_RunMainThreadCallbacks(); /* then anything queued for this thread */
SDLOP_RunTimerCallbacks();
```

The order matters: an input record may reference a window, so the window state a
compositor sent (resize, focus, scale) is applied before the record is translated.

Backends currently: **wayland**, **x11** and **offscreen** (a window that is a
pixel buffer — used by the tests and by headless CI). The list is compiled per
availability, and the core picks at runtime: `WAYLAND_DISPLAY` first, then
`DISPLAY`, then offscreen, and an auto-detected driver that fails to connect
hands over to the next one (a stale `WAYLAND_DISPLAY` in an SSH session should
not stop an application from coming up on X11). An explicit `SDL_VIDEODRIVER` is
never retried — the application asked for *that* driver.

#### Wayland

xdg-shell + `wl_shm` XRGB8888 buffers with viewporter scaling,
`wl_surface_frame` pacing, cursor-shape-v1, pointer constraints and the relative
pointer; a `wl_egl_window` for GL; `vkCreateWaylandSurfaceKHR` for Vulkan. See
the display and pointer notes below.

#### X11

`src/video/SDL_x11.c` is a plain Xlib client (no XCB except the one bridge
`xkbcommon-x11` needs) and the fallback for every session where `/dev/input` is
not readable — XWayland, Flatpak, a sandbox, a remote session. What it owns:

* **Window state through EWMH**, with `_MOTIF_WM_HINTS` for the borderless case
  (EWMH's decoration hints are not universally implemented) and
  `_NET_WM_STATE_*` for fullscreen/maximized/above/hidden/demands-attention;
  display names come from RandR, and a screen without RandR still enumerates one
  display (refresh rate 0 when the server does not report a dot clock).
* **Presenting through MIT-SHM** (`XShmCreateImage` + `XShmPutImage`) with a
  plain `XPutImage` fallback, and the shared memory segment reclaimed on resize.
  The window's format is XRGB8888 unless the visual says otherwise.
* **Input translation** (see the input chapter): core events plus XInput2 raw
  motion when the extension is there.
* **GL through EGL**, not GLX: the window has to be created with the visual the
  EGL config can present to, so `sdlop_egl_x11_visual()` is asked *before*
  `XCreateWindow` and the answer goes into the window attributes (with a
  `Colormap` of its own when the visual is not the screen default). GLX needs a
  different config-selection path for the same result.
* **Vulkan through `vkCreateXlibSurfaceKHR`** — the WSI is declared in the
  backend, like the Wayland one, so `src/video/SDL_vulkan.c` has no platform code
  at all: it owns the loader and dispatches to the driver's
  `create_vulkan_surface` / `get_vulkan_instance_extensions` /
  `vulkan_presentation_support`.

The per-backend state a window carries is a named struct in the internal header
(`SDLOP_X11WindowState`, `SDLOP_WaylandWindowState`) and the `driver` union is
made of those two types, because a backend reaches its state through a local
macro (`SDLOP_X11_STATE(window)`) and a second copy of the layout is a trap: the
X11 copy once kept three fields the union had dropped, so every field from the
GC on was addressed 24 bytes off. The present path worked — it was self-consistent
inside the file — while the teardown read the wrong offset and leaked one GC per
window. The address sanitizer found it (`make SANITIZE=1`, a 50-cycle
init/window/present/destroy loop); it is documented there so the copy does not
come back.

### Displays: a burst of events, closed by `wl_output.done`

A Wayland output is not described by one object: `wl_output` carries the mode,
the physical geometry and the scale, while `zxdg_output_v1` carries the position
and size in the *global compositing space* — the only place display coordinates
exist at all. Each batch ends with `wl_output.done`, and when an `xdg_output` is
attached the compositor sends **two** of them (one for each object's events), so
the completion condition is

```c
event_await_count = 1 + (xdg_output != NULL);
```

`zxdg_output_v1.done` itself is deprecated from manager version 3 on and newer
compositors (sway) never send it, which is why the counting is on
`wl_output.done`. Once a burst is complete the display is recomputed from the raw
fields (they are never mutated in place, so a repeated burst is idempotent):

* the *logical* size is what SDL reports — a `1024x768` output at scale 2 is a
  `512x384` display;
* the display's native scale factor is `native_width / logical_width` when
  viewporter is available (that ratio is also the only way to see a fractional
  scale), else the integer `wl_output.scale`;
* the desktop/current mode is the logical size with `pixel_density` set to that
  factor, plus the native mode as a fullscreen mode;
* with `SDL_HINT_VIDEO_WAYLAND_SCALE_TO_DISPLAY` the desktop, the bounds and the
  content scale switch to physical pixels instead. This is SDL3's model on the
  nose: **without the hint `SDL_GetDisplayContentScale()` stays 1.0**, and the
  display stays in logical coordinates.

#### Which output is a window on?

`wl_surface.enter`/`leave` are the only authority on that: the compositor decides
where a toplevel is shown, so a window tracks the outputs it entered (a small
array on the window, oldest first) and reports the *last* one as its display —
exactly SDL3's rule, including the fullscreen exception (a fullscreen window
belongs to the output it went fullscreen on, which is the first `enter`). The
position it publishes is that display's top-left, because Wayland has no window
position and applications (and their mouse math) expect the window's position to
be inside the display it claims to be on.

Everything else follows from that: an unplugged output is dropped from every
window's list *before* the display is removed, so the display removal then
re-homes the window onto a surviving display; and a window that is on no output
at all (minimized) keeps the display it had, again as in SDL3. The core's
`SDL_GetDesktopDisplayMode()` asks the backend (`get_display_mode`) when a
display has no mode yet, which is the normal state between
`SDL_EVENT_DISPLAY_ADDED` and the first complete event burst.

Display *names* are resolved once, on that first complete burst, in SDL3's order:
`wl_output.description` ("Dell Inc. DELL U2720Q"), else the `zxdg_output_v1`
description (pre-v4 compositors only — it is deprecated from v4 on), else the
`wl_output.geometry()` model string, else the connector name. A later event must
not be able to rename a display the application already has an ID for, and a
display that has not been described yet sits at 0,0 with no mode rather than at
an invented offset.

Window scale has three layers, mirroring SDL3:

| value | where | meaning |
|---|---|---|
| `display->scale` | `SDLOP_Display` | native factor of the output |
| `display->content_scale` | `SDLOP_Display` | what `SDL_GetDisplayContentScale()` returns (1.0 unless scale-to-display) |
| `window->scale_factor` | `SDL_Window` | native factor of the display the window is on |
| `window->display_scale` | `SDL_Window` | the scale the window *renders* at: `scale_factor` only with `SDL_WINDOW_HIGH_PIXEL_DENSITY` (or scale-to-display), else 1.0 |

`window->pixel_w/h` is `w/h * display_scale` and the window surface follows it, so
`SDL_GetWindowSizeInPixels()` and `SDL_GetWindowSize()` differ exactly when the
application asked them to. Unplugging an output removes its display
(`registry_global_remove`), re-homes its windows and never reuses the ID; output
slots in the backend array are never compacted because the compositor holds a
pointer to the slot as its listener data.

### Pointer constraints: locking and confining the cursor

Wayland has no `XWarpPointer` and no global pointer grab; what it has is
`zwp_pointer_constraints_v1` (lock/confine the pointer to a surface) and
`zwp_relative_pointer_v1` (unaccelerated deltas of a locked pointer). Both are
bound when the compositor advertises them:

* **relative mouse mode** locks the pointer on every window and reports the
  relative pointer's *unaccelerated* deltas as `SDL_EVENT_MOUSE_MOTION` with
  `xrel/yrel` set — nothing is clamped, scaled or accelerated on the way;
* **`SDL_SetWindowMouseGrab`** and **`SDL_SetWindowMouseRect`** confine the
  pointer (the rect becomes a `wl_region`, and the confine is committed because
  the region is double buffered);
* a surface may be locked *or* confined, never both, so every transition
  destroys the other object first — that is a protocol error otherwise, and it is
  the reason `relative off` re-confines a grabbed window;
* without the protocols relative mode still works in the degraded form the
  backend had before: deltas are derived from the absolute pointer position.

## 2. Input: an async producer instead of a synchronous pump

The input path follows [asyncinput](https://github.com/CoCkMelon/asyncinput)
rather than SDL3's "read devices when the application pumps" model.

```
 /dev/input/event*            worker thread                    main thread
 -----------------            -------------                    -----------
 epoll_wait  -------------->  struct input_event
                              (kernel CLOCK_MONOTONIC timestamp)
                                    |
                              convert to ticks domain (offset captured at start)
                                    |
                              SDLOP_RawInputRecord {kind, code, value, ts, device}
                                    |
                              lock-free SPSC ring (64-byte aligned head/tail)
                                    v
                             SDLOP_PumpRawInput()  <----- SDL_PumpEvents()
                                    |
                              SDLOP_ScancodeFromEvdevKeycode[code]   (generated table)
                                    |
                              SDL_Event -> event queue -> SDL_PollEvent()
```

Design points:

* **One worker, epoll, no libudev.** Devices are found by scanning
  `/dev/input/event*` and are re-scanned once a second, so hot-plug works without
  a udev dependency. `epoll_event.data.ptr` points straight at the device, so the
  wakeup needs no lookup.
* **Kernel timestamps.** The worker asks for `CLOCK_MONOTONIC` (`EVIOCSCLOCKID`)
  and converts to the library's tick domain with an offset captured at start, so
  an event's timestamp is when the hardware generated it — not when the app got
  around to pumping.
* **No allocation on the hot path.** Records are fixed-size values in a ring;
  the ring's head/tail are `_Atomic` and live on separate cache lines.
* **Zero syscalls when nobody is waiting.** The worker wakes a blocked
  `SDL_WaitEvent()` through an eventfd; producers only pay for that write when a
  thread has actually parked, which is what makes the polling case a pure
  user-space round trip (see [PERFORMANCE.md](PERFORMANCE.md)).
* **Testability without hardware.** `SDLOP_TEST_INPUT=<fifo>` starts a reader
  thread that accepts `EV_KEY|EV_REL|EV_ABS <code> <value> [device] [ts]` lines
  and pushes them through the *same* ring, so the whole translation path is
  covered by `tests/test_input.c` on a machine with no `/dev/input` at all.
* **The X11 reader is the fallback producer.** When `/dev/input` cannot be
  opened — XWayland, Flatpak, a sandbox, a remote session — the X11 backend feeds
  the same translation layer from X events instead: key press/release with the
  scancode derived from the server's keycode (XKB `evdev+8`, so the two sources
  agree), the layout's own state fed from every key
  (`layout->update_key()`, exactly like the evdev worker does), buttons and the
  wheel (button 4–7, press only — X sends a release for them too), and motion
  from core events plus XInput2 raw motion so a grabbed pointer reports the
  unaccelerated deltas. Key repeat stays where the X server puts it (the server
  sends press events, `XkbSetDetectableAutoRepeat` tells them apart from real
  presses), text comes from the layout, and an active FIFO producer takes
  precedence over both sources so a test never fights a real device.
  `tests/x11_input.sh` drives it with `xdotool` (`make x11-check`).

Translation (`src/input/SDL_input.c`) is table-driven: `sdlop_scancode_from_evdev`
maps Linux keycodes to SDL scancodes (generated from upstream's
`scancodes_linux.h`, so keypad and media keys match SDL3 exactly), and the
keycode/modifier layer adds shift state, `SDL_GetModState()` bookkeeping, repeat
gating on text input being active, and UTF-8 text synthesis.

### Keycodes and text: the layout

SDL scancodes are physical keys, but the *keycode* (`event.key.key`) and the text
a key types depend on the keyboard layout. A backend can therefore register a
layout:

```c
typedef struct SDLOP_KeyLayout
{
    void (*update_key)(Uint32 evdev_code, bool down);          /* sees every key event */
    SDL_Keycode (*keycode_from_evdev)(Uint32 evdev_code);      /* e.g. 'A' with shift held */
    int (*text_from_evdev)(Uint32 evdev_code, char *buf, size_t n);
} SDLOP_KeyLayout;
```

`update_key` exists because of the async design: keys normally reach SDL from the
evdev worker, not from the compositor, so a layout cannot assume anybody else is
feeding it the modifier state — it has to follow the keys the application
actually sees. The Wayland backend registers an xkbcommon layout, compiled from,
in order of preference:

1. `SDLOP_XKB_KEYMAP=<file>` — an explicit XKB keymap file (debugging, or a
   compositor that sends the wrong one);
2. the compositor's `wl_keyboard.keymap`;
3. the local XKB configuration (`XKB_DEFAULT_LAYOUT` and friends) — what a
   headless or KMS compositor that never sends a keymap gets
   (`xkb_keymap_new_from_names`). This one is compiled **lazily**, on the first
   key that needs it: it costs ~2 ms, more than everything else
   `SDL_Init(SDL_INIT_VIDEO)` does, and a session with a keyboard is told its
   layout by the compositor or the X server anyway. `SDLOP_XKBEnsure()` is called
   by the input translation (and by the layout's own queries), and a keymap that
   arrives from the platform cancels the pending fallback;
4. nothing: SDL's built-in tables derive text and keycodes from scancodes.

On X11 the same module gets the keymap from the *server* through
`xkbcommon-x11` (the core keyboard's device id and the Xlib connection handed to
XCB), so layouts, dead keys and group switching follow the X configuration
instead of the local one — XKB is how X11 has always described a keyboard.

SDL keycodes are **unshifted**: shift+a reports the same keycode as a, with
`SDL_KMOD_LSHIFT` in `event.key.mod` and the capital as the text (verified
against stock SDL3 3.2.10 on both backends). The layout module therefore reads
level 0 of the keymap (`xkb_keymap_key_get_syms_by_level`) and applies no shift
state of its own, plus SDL's default `SDL_HINT_KEYCODE_OPTIONS` rules
(`french_numbers`, `latin_letters`).

A layout that is registered also has the final say on text: if it says a key
types nothing (a modifier, a dead key), no text is invented from the keycode.
Text is only sent on press, never while Ctrl or Alt is held, and only for
applications that turned text input on — SDL3's rules.

Key repeat is a client-side duty on Wayland (`wl_keyboard` never sends repeats):
`repeat_info` gives the rate and delay, `xkb_keymap_key_repeats()` says whether a
key repeats at all, and the pump emits the repeats, with the backend timer above
making sure a blocked wait still wakes up for them.

## 3. Event queue and waiting

A fixed 512-event ring guarded by one mutex, plus a condition variable and a
wakeup eventfd:

* `SDL_PushEvent` takes the lock, appends, and — **only if a thread is parked** —
  broadcasts and writes the eventfd. When nobody is waiting, it touches neither.
* `SDL_PollEvent` pops an event that is already queued *without pumping first*,
  and only pumps when the queue is empty. Pumping costs a `poll(2)` on the
  backends' descriptors (and the input ring, and the due timers), so this is the
  difference between a tight poll loop that pays for a pump per event and one
  that pays for none; SDL3 has the same fast path and the benchmark measures it
  (see [PERFORMANCE.md](PERFORMANCE.md)).
* `SIGINT`/`SIGTERM` are installed once (SDL_HINT_NO_SIGNAL_HANDLERS turns that
  off, and an application's own handler is never replaced) and set a flag in the
  handler; the handler also writes the wakeup fd, which is async-signal-safe. The
  next pump turns the flag into `SDL_EVENT_QUIT`, so Ctrl+C in a windowed program
  is a request to shut down rather than a kill, and a thread blocked in
  `SDL_WaitEvent()` finds it immediately.
* `SDL_WaitEvent[_Timeout]` pumps, pops, and otherwise parks in `poll()` over
  {platform event fd, wakeup fd} (or on the condition variable when a backend has
  no descriptor), with the next timer deadline as the timeout. Because input
  arrives through the wakeup fd, a waiting application wakes on the event itself.
* A fixed queue is a deliberate choice: it is a bounded, predictable amount of
  memory, and overflow is reported ("Event queue is full") rather than turning
  into unbounded growth in a frame where the app is too busy to pump.

`SDLOP_RunMainThreadCallbacks()` and `SDLOP_RunTimerCallbacks()` are called from
every pump, so each has an atomic fast path that returns without touching a lock
or the clock when there is nothing queued and no timer exists.

## 4. Generated code and upstream data

Nothing that is *data* is hand-written; `make regen` rebuilds it:

| generated file | generator | source |
|---|---|---|
| `include/SDL3/*.h` (19 of 24) | `tools/sdlop.py` | upstream headers + keep/drop lists |
| `src/generated/sdlop_keynames.h` | `tools/gen_keynames.py` | `SDL_keycode.h`, `SDL_scancode.h`, `SDL_scancode_names[]` |
| `src/generated/sdlop_evdev.h` | `tools/gen_evdev.py` | upstream `src/events/scancodes_linux.h` |
| `src/generated/sdlop_pixelformats.h` | `tools/gen_pixelformats.py` | `SDL_pixelformat.h` |
| `src/generated/*-protocol.{c,h}` | `wayland-scanner` | wayland-protocols XMLs |

`tools/reference/` keeps the upstream inputs so a regeneration needs no network.
`tools/check_api.py` re-derives the declaration set from the SDLop headers with
clang and diffs it against `/usr/include/SDL3/` — that is the guard that the
"same API" promise still holds, and (with `--lib`) that every function the
headers declare is actually exported by the built library.

## 5. Rendering surfaces

The library creates surfaces but never draws into them:

* `SDL_GetWindowSurface` gives a software surface the app can fill and blit
  (`SDL_UpdateWindowSurface` marks it dirty and the backend presents it — a
  Wayland `wl_shm` buffer attach/commit, a no-op on `offscreen`).
* `SDL_GL_*` goes through `src/gl/SDL_egl.c`: all fifteen EGL entry points are
  resolved with `dlsym` from a `dlopen`ed `libEGL.so.1` (the library itself links
  no EGL at all, so the `DT_NEEDED` set stays the platform libraries and the C
  library), and the platform display is the driver's — `EGL_PLATFORM_WAYLAND_EXT`
  or `EGL_PLATFORM_X11_EXT` — with the native window being a `wl_egl_window` on
  Wayland and the X window number on X11. On X11 the EGL config also decides the
  X visual (`sdlop_egl_x11_visual()`), which is why the backend asks for it
  before it creates the window.
* `SDL_Vulkan_*` goes through `src/video/SDL_vulkan.c`, which `dlopen`s the
  Vulkan loader (`libvulkan.so.1` or `SDL_HINT_VULKAN_LIBRARY`) and then asks the
  driver for everything platform-specific: `{VK_KHR_surface,
  VK_KHR_wayland_surface}` + `vkCreateWaylandSurfaceKHR` +
  `vkGetPhysicalDeviceWaylandPresentationSupportKHR` on Wayland,
  `{VK_KHR_surface, VK_KHR_xlib_surface}` + `vkCreateXlibSurfaceKHR` +
  `vkGetPhysicalDeviceXlibPresentationSupportKHR` on X11. No Vulkan headers are
  needed to build: the types come from `SDL_vulkan.h` and the entry points are
  looked up by name.

## 6. Conventions

* C11, no C++, no exceptions/exits: every entry point returns a `bool` and calls
  `SDL_SetError` (or `SDL_InvalidParamError`) on failure, exactly like SDL3.
* `SDL_InvalidParamError("name")` and `SDL_Unsupported()` are macros taking
  *quoted literals*.
* Allocation goes through `SDLOP_Alloc/Calloc/Realloc/Free` so a host can later
  replace the allocator in one place; nothing is allocated on an input or event
  hot path.
* Threads are `pthread`s (the SDLop build assumes a POSIX host for now); the
  Windows/macOS ports will provide their own worker implementations behind the
  same ring interface.

## 7. Comparing against stock SDL3

"The same API" is not enough on its own: a reimplementation that names a key
differently, or that sends a resize event at a different moment, breaks
applications in ways the header check cannot see. So there is a second rig, next
to `make abi-check` (which compares declarations and constants):

* `tools/behaviour_probe.c` is one program, compiled twice - against SDLop and
  against the system's stock SDL3 - that prints everything an application can
  observe about a window and its input: the video driver, the displays, window
  state, every event with its name and payload, the key/scancode name tables, and
  `SDL_GetKeyName()`/`SDL_GetKeyFromName()` answers for a fixed list.
* `tests/behaviour_check.sh` (`make behaviour-check`) runs both binaries with the
  same scripted input on Xvfb - `xdotool` for keys, buttons, the wheel and the
  pointer, `setxkbmap` for the layout - and diffs the two traces.

The comparison is deliberately not "diff and hope". The input events and the
window operations the probe performs are compared **in order**, because that is
what an application reacts to. The window-lifecycle phase is compared as a
**set** rather than in order: when the server says "mapped, exposed, focused"
depends on the order the X requests were issued in, and no application can depend
on it - the rig therefore reports whether the order matches as well (on the
reference rig it now does, event for event) but does not fail when it does not.
Getting that far took making SDLop's X11 backend answer the same way stock's does:
`show_window()` and `create_window()` map, take the focus and then dispatch what
the server has to say about the map before returning (`XSync()` round trip, then
one pump - the events a request produces are queued before the reply to a later
one, so this does not block on anything), and `hide_window()` does the same, which
is why the focus change is reported before `SDL_EVENT_WINDOW_HIDDEN`. Each
rule that filters something out is written down with its reason in the script -
`STATE flags=`, zero-delta motions, and the symbol-name line - so a difference is
either fixed, documented, or reported; it is never silently swallowed. The script
calls a difference it only reports a *note*, and that is how the one remaining
divergence is handled: `SDL_GetKeyFromName("?")` answers with the key that types
"?" on the active layout in stock SDL3 (0xdf on a German layout) and with the US
layout's key here.

Window state is compared too, through the flags line the probe prints: the focus
a window has right after creation is part of this (stock's X11 driver takes the
input focus when there is no window manager, and SDLop does the same, which is
what makes typing into a window on a bare X server work), and so is the position
a geometry request has not answered yet (SDL3 hands the request to the server and
`SDL_SyncWindow()` is what waits for the answer - on X11 both libraries still
report the previous position until then).

Getting the two traces to agree is what found, and fixed, the divergences worth
fixing: Caps Lock reporting the lock on the wrong event, a missing
`SDL_EVENT_WINDOW_SAFE_AREA_CHANGED`, `SDL_EVENT_KEYMAP_CHANGED` sent for the
first keymap (a change, not a first setup), the pixel size and safe area never
announced at creation, an `EXPOSED` (and an `OCCLUDED`) announcement for every
visibility change on top of the expose events (stock sends `EXPOSED` once per
Expose event and reads occlusion from `_NET_WM_STATE_HIDDEN` instead), a move
reported from the request *and* from the server's answer (the answer is the truth,
and `SDL_SyncWindow()` is what waits for it - which also fixed what the rigs read
back after a resize), and no motion event on the pointer crossings - X's crossing
events carry the pointer's position, which is the only way an application that
draws on motion learns where the pointer is on entry.

The last of those divergences took the longest to pin down, because the reason was
on the server's side: for a layout switched while the application runs, this X
server sends the core `MappingNotify` only to clients that never spoke XKB to it
at all (measured: a client that has sent `XkbUseExtension` receives nothing when
`setxkbmap` reloads a layout, while one that has not receives the pair), and
SDLop's connection has to speak XKB - its keymap comes from xkbcommon-x11. What
such a server does write on every reload is the root property `_XKB_RULES_NAMES`,
and the new keymap is already readable when that event arrives, so that is what
the X11 backend answers now (`XKLAVIER_STATE` is watched as well, because it is
the property stock SDL3 reads for exactly this reason). Both libraries then
announce the switch; they do not agree on *how many* times (this server posts
three `MappingNotify` events for one switch, stock sends one event each, SDLop has
one property change), which is a count no application can depend on - the rig
collapses the run of `KEYMAP_CHANGED` events so the switch is compared rather than
the server's chattiness.

Two habits from this work are worth keeping:

* instrument the backend, do not guess what the server sent
  (`SDLOP_X11_DEBUG_EVENTS=1` logs every dispatched X event, in order);
* make the rig deterministic before believing a difference - the pointer's
  position left over from the previous run changed which events a window saw
  while it was being created, and a filter that was supposed to hide one rule
  turned out to read the trace from its own stdin instead of from the previous
  stage of the pipeline (a function body is not a pipeline).
