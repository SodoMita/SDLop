# Performance

The claim to defend is "same API, less work, faster". This document is how the
numbers are produced and what they are (and are not).

## How to run it

```sh
make bench                       # builds build/bench/bench_compare
./build/bench/bench_compare build/libSDLop.so
./build/bench/bench_compare build/libSDLop.so --json     # machine readable
make bench-run                   # the two commands above
```

`bench/bench.c` links the **system SDL3** and `dlopen`s **SDLop**'s shared
library (`RTLD_LOCAL | RTLD_DEEPBIND`, so each library resolves its own symbols
and neither sees the other's state). Every benchmark is a plain public-API loop
driven through a table of function pointers, so the identical code runs against
both implementations in one process, on one machine, with one set of caches.

Method: 5 runs per benchmark, the **median** is reported; each benchmark is
warmed up first; timings come from `clock_gettime(CLOCK_MONOTONIC)` in the
benchmark itself, never from the library being measured. The video driver is
`offscreen` unless `SDL_VIDEODRIVER` says otherwise, so nothing depends on a
compositor being present.

## Results (SDL 3.2.10 vs SDLop 0.1.0, x86-64, gcc 14.2, -O2)

`offscreen` driver, so both libraries do the same bookkeeping without a
compositor in the way:

| operation | SDL3 | SDLop | ratio |
|---|---:|---:|---:|
| `SDL_Init(SDL_INIT_VIDEO)` + `SDL_Quit` | 24766 ns | 6210 ns | **4.0x faster** |
| `SDL_CreateWindow` + `SDL_DestroyWindow` (640x480) | 30890 ns | 1023 ns | **30.2x faster** |
| `SDL_PumpEvents` (nothing pending) | 40.7 ns | 10.8 ns | **3.8x faster** |
| `SDL_PushEvent` + `SDL_PollEvent` (round trip) | 44.2 ns | 10.5 ns | **4.2x faster** |
| `SDL_GetTicks` | 32.9 ns | 27.3 ns | 1.2x faster |
| `SDL_GetPerformanceCounter` | 25.5 ns | 24.1 ns | tie |
| `SDL_FillSurfaceRect` + `SDL_UpdateWindowSurface` (640x480) | 2335787 ns | 59712 ns | 39.1x faster |
| `SDL_GetKeyboardState` | 2.6 ns | 2.3 ns | 1.1x faster |
| `SDL_GetModState` | 2.4 ns | 1.8 ns | 1.3x faster |

And on X11 (Xvfb, a real X window, `XShmPutImage` presentation), where the same
X server round trips are paid by both libraries:

| operation | SDL3 | SDLop | ratio |
|---|---:|---:|---:|
| `SDL_Init(SDL_INIT_VIDEO)` + `SDL_Quit` | 6836328 ns | 3296747 ns | **2.1x faster** |
| `SDL_CreateWindow` + `SDL_DestroyWindow` (640x480) | 1680684 ns | 229278 ns | **7.3x faster** |
| `SDL_PumpEvents` (nothing pending) | 1474.8 ns | 410.2 ns | **3.6x faster** |
| `SDL_PushEvent` + `SDL_PollEvent` (round trip) | 52.4 ns | 16.6 ns | **3.2x faster** |
| `SDL_FillSurfaceRect` + `SDL_UpdateWindowSurface` (640x480) | 2780572 ns | 202042 ns | **13.8x faster** |
| `SDL_GetTicks` / `SDL_GetPerformanceCounter` | 32.3 / 25.8 ns | 27.1 / 25.4 ns | 1.2x / tie |

And on Wayland (headless sway, two outputs), where every call has to talk to a
compositor:

| operation | SDL3 | SDLop | ratio |
|---|---:|---:|---:|
| `SDL_Init(SDL_INIT_VIDEO)` + `SDL_Quit` | 273990 ns | 184449 ns | **1.5x faster** |
| `SDL_CreateWindow` + `SDL_DestroyWindow` (640x480) | 356906 ns | 118005 ns | **3.0x faster** |
| `SDL_PumpEvents` (nothing pending) | 303.3 ns | 258.3 ns | 1.2x faster |
| `SDL_PushEvent` + `SDL_PollEvent` (round trip) | 50.3 ns | 12.2 ns | **4.1x faster** |
| `SDL_FillSurfaceRect` + `SDL_UpdateWindowSurface` (640x480) | 2880690 ns | 1483737 ns | 1.9x faster |
| `SDL_GetTicks` / `SDL_GetPerformanceCounter` | 31.9 / 25.5 ns | 27.5 / 24.8 ns | 1.2x / tie |

Two caveats on the Wayland row. Weston, unlike sway, has no
`zxdg_decoration_manager_v1`, so stock SDL3 falls back to client-side
decorations and loads libdecor + GTK 3 per window — on that compositor the
init/window rows compare SDL3 *with* decorations against SDLop *without* (SDLop
does not implement client-side decorations; it is a documented omission). The
table above is sway, where both libraries let the compositor draw the titlebar
and the comparison is like for like.

`SDL_UpdateWindowSurface` on Wayland is the one number that is close to a tie,
and it should be: both libraries attach one `wl_shm` buffer and commit, so the
cost is the compositor's, not the library's. On X11 it is `XShmPutImage` against
stock's `XPutImage` path, which is where the 13.8x comes from.

Readings of the individual benchmarks:

* **Init/teardown (4.3x).** SDLop has no HIDAPI scan, no udev enumeration of
  every subsystem, no joystick/audio/haptic probing when only `SDL_INIT_VIDEO`
  was asked for, and it starts exactly one input thread.
* **Window create/destroy (26.5x offscreen, 6.2x on X11).** An SDLop window is a
  struct, a surface and (on Wayland) a `wl_surface`, or on X11 an `XCreateWindow`
  and a GC; there is no renderer, no per-window properties bag, no display-mode
  list to rebuild, and no `SDL_PumpEvents` in between. On X11 the 6.2x is what is
  left after the X server round trips that both libraries pay: SDLop now waits
  for the map (an `XSync()` round trip and one pump, 400 us per create+destroy)
  because that is what makes the events an application sees after
  `SDL_CreateWindow()` - the expose, the focus, and `SDL_WINDOW_INPUT_FOCUS` - the
  same as stock's, which pays even more for the same thing (2.48 ms). The ratio
  is a little lower than it was without the wait; the events are worth it, and
  the absolute cost is still 6x lower.
* **`SDL_PumpEvents` (1.2x on Wayland, 1.7x on X11).** The backends pump with a
  descriptor and a ring; callbacks and timers return via an atomic fast path
  before taking any lock or reading the clock. Measured **on the Wayland path**
  (offscreen is not comparable, it has no socket): the SDLop pump is
  **270 ns/call** against stock SDL3's **330 ns/call**, *including* the
  non-blocking `poll()` that keeps a polling application reading its compositor
  socket. On X11 the pump is **1036 ns** against stock's **1716 ns** - both are
  now paying for a server round trip on an empty queue, which is the honest
  measurement: SDLop's create/show paths dispatch the events the server sends
  about a map, so by the time the benchmark pumps there is nothing left in the
  queue, where before those events were still sitting there and `XPending()`
  answered from the local buffer (421 ns). An application that pumps while events
  are already queued still sees the fast path; the empty-queue case is what a
  frame loop actually pays, and there stock is the slower one.
* **`SDL_PushEvent` + `SDL_PollEvent` (4.3x offscreen, 3.2x on X11).** A single
  mutex and a ring copy in each direction, *no syscall when no thread is
  waiting*, and no pump when an event is already queued (see below).
* **Ticks/performance counter (1.0–1.2x).** Both implementations end up in
  `clock_gettime(CLOCK_MONOTONIC)`; SDLop keeps one cached start offset
  instead of SDL3's more general tick bookkeeping.
* **`SDL_UpdateWindowSurface` (42x).** Careful with this one: it is *not* a
  like-for-like comparison of presentation cost. The `offscreen` driver of stock
  SDL3 pushes the frame through its renderer-side path, while SDLop's offscreen
  present is "the surface is already the window's buffer". The number is real but
  it says more about module count than about Wayland present speed.

## What the benchmark found (and what was fixed)

Five problems were found this way (four of them *slower* hot paths, one a crash);
none was found by reading the code, and all are fixed:

1. **A `write()` to the wakeup eventfd on every pushed event** (0.34x on the
   push/poll round trip). The eventfd exists so that a thread parked in
   `SDL_WaitEvent()` wakes the instant input arrives; but producers were writing
   to it even when nobody was parked, which put a syscall in the middle of every
   frame's event loop. Producers now read the waiter count under the queue lock
   and only pay for the write when a thread is actually parked; the wait path
   registers itself as a waiter *before* it can sleep, so no wakeup is lost.
   Push/poll went 127 ns → 15 ns per event.
2. **A mutex taken by every `SDL_PumpEvents`** (1.1x). `SDLOP_RunMainThreadCallbacks`
   and `SDLOP_RunTimerCallbacks` now check an atomic mirror of their queue
   depth first and return immediately when there is nothing to do — no lock, and
   for timers not even a clock read. Pump went 36 ns → 9.6 ns.
3. **An eagerly compiled XKB keymap on Wayland init** (0.13x — found by running
   the benchmark against this tree's Wayland backend, where `SDL_Init` was
   2681 µs against stock's 342 µs). Compiling the local XKB configuration
   (`xkb_keymap_new_from_names`) costs ~2.1–3.2 ms, and init was doing it "just
   in case" — while a session that has a keyboard is told its layout by the
   compositor (or by the X server) anyway. The fallback keymap is now compiled on
   the first key that needs one (`SDLOP_XKBEnsure()`), and a keymap that arrives
   from the platform cancels the deferred one. `SDL_Init` went 2681 µs → 184 µs
   (stock: 274 µs) with the same layout behaviour: composing the local rules on
   the first key still works where nothing sends a keymap, and a compositor's
   keymap still wins.
4. **A pump per polled event** (0.13x on X11 — the first X11 bench run is how
   this was found: `SDL_PollEvent` pumped on *every* call, so a frame that
   drained 64 events paid for 64 `poll(2)`s on the X connection). `SDL_PollEvent`
   now hands out an event that is already queued and only pumps when the queue is
   empty — the same fast path stock SDL3 has, confirmed by measuring stock on the
   same X server (26 ns/event there while its own empty-queue poll is 1582 ns).
   Push/poll went 383 ns → 16 ns per event on X11.
5. **A stale Wayland connection on re-init.** `make bench` on weston exited 139
   (segfault) while the same binary was clean on sway, and no amount of reading
   the teardown order explained it. Running the cycle loop under
   `-fsanitize=address` showed weston rejecting the *second* `SDL_Init` with
   "invalid arguments for zwp_relative_pointer_manager_v1.get_relative_pointer",
   after which the quit path dereferenced proxies from the dead connection:
   `sdlop_wayland_quit()` destroyed the globals but never dropped its pointers,
   so a re-init handed libwayland objects from a connection that no longer
   belonged to the compositor. A live connection makes that invisible — the
   benchmark's init/quit cycle loop is what exposed it, and it is the reason
   `make SANITIZE=1` now exists.

All four performance fixes are pure fast paths: the slow paths (an actual waiter, an actual
queued callback, an actual timer, an empty queue, a session that does need the
fallback keymap) are unchanged, and the whole test suite plus the Wayland and X11
end-to-end rigs were re-run after them.

A later round of Wayland work added things a benchmark would only catch
indirectly, so they are noted here instead: every pump now reads the compositor
socket (a polling application otherwise never reads, and the compositor's
outgoing queue — a few kilobytes — overflows and the client is dropped);
presentation is paced with `wl_surface_frame` so a buffer is never attached
while it is still being displayed; key repeats are synthesized client-side from
the compositor's `repeat_info` rate and delay; and **relative mouse mode goes
through `zwp_relative_pointer_v1`** — the compositor's unaccelerated deltas of a
locked pointer, one event per real device motion, with no pointer acceleration
state machine, no cursor warping and no clamping of the position on the way to
the application (the `x`/`y` in the event only advance; a game that integrates
`xrel`/`yrel` never sees a compositor-imposed edge).

Display and window scale became a no-cost path as well: `SDL_CreateWindow` now
resolves the window's scale factor (native factor of the display, applied only
for `SDL_WINDOW_HIGH_PIXEL_DENSITY` or scale-to-display) in the same pass that
used to read the content scale, and the pixel size is only recomputed when that
factor actually changes.

## Input latency

Async input is measured by `tests/test_input.c` rather than by the micro
benchmark, because latency — not throughput — is the point: a feeder thread
writes one key record into the `SDLOP_TEST_INPUT` FIFO 120 ms after the test
starts, while the main thread sits in `SDL_WaitEvent()` **with no timeout**. The
test asserts that the call returns with the right event, and reports how long
after the record was sent that happened:

```
input woke a blocked SDL_WaitEvent() 121 us after it was sent
```

So a blocked application notices input in well under a millisecond through the
worker → ring → translation → wakeup-fd path, and it is the input that wakes it,
not a polling timeout.

**Still to measure** (see [ROADMAP.md](ROADMAP.md)): the same number through the
*real* evdev path on a machine with `/dev/input`, including the kernel timestamp
of the event versus the moment the application sees it. `uinput`-driven CI is the
plan; this sandbox has no `/dev/input`, which is exactly why the FIFO hook exists.

## Library size

| | SDLop | SDL3 3.2.10 |
|---|---:|---:|
| shared library (unstripped, with `-g`) | 1.00 MB | 2.87 MB |
| shared library (stripped) | 0.29 MB | 2.87 MB |
| exported `SDL_*` symbols | 400 | 1208 |

The stripped comparison is the meaningful one: about **10x less code** for the
same windowing, input, timing, GL and Vulkan surface APIs — and this now includes
*both* Linux backends (Wayland and X11), where the earlier 0.23 MB number was
Wayland and offscreen only. SDL3's own 2.87 MB is one build of the full library
for the same machine.
