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

| operation | SDL3 | SDLop | ratio |
|---|---:|---:|---:|
| `SDL_Init(SDL_INIT_VIDEO)` + `SDL_Quit` | 20159 ns | 4034 ns | **5.0x faster** |
| `SDL_CreateWindow` + `SDL_DestroyWindow` (640x480) | 16727 ns | 776 ns | **21.5x faster** |
| `SDL_PumpEvents` (nothing pending) | 34.0 ns | 9.6 ns | **3.5x faster** |
| `SDL_PushEvent` + `SDL_PollEvent` (round trip) | 37.5 ns | 14.0 ns | **2.7x faster** |
| `SDL_GetTicks` | 34.3 ns | 28.7 ns | 1.2x faster |
| `SDL_GetPerformanceCounter` | 27.3 ns | 27.1 ns | tie |
| `SDL_FillSurfaceRect` + `SDL_UpdateWindowSurface` (640x480) | 2132637 ns | 55662 ns | 38.3x faster |
| `SDL_GetKeyboardState` | 1.9 ns | 1.5 ns | 1.3x faster |
| `SDL_GetModState` | 1.8 ns | 1.2 ns | 1.5x faster |

Readings of the individual benchmarks:

* **Init/teardown (5.0x).** SDLop has no HIDAPI scan, no udev enumeration of
  every subsystem, no joystick/audio/haptic probing when only `SDL_INIT_VIDEO`
  was asked for, and it starts exactly one input thread.
* **Window create/destroy (21.5x).** An SDLop window is a struct, a surface and (on
  Wayland) a `wl_surface`; there is no renderer, no per-window properties bag, no
  display-mode list to rebuild, and no `SDL_PumpEvents` in between.
* **`SDL_PumpEvents` (3.5x).** The offscreen/wayland backends pump with a
  descriptor and a ring; callbacks and timers return via an atomic fast path
  before taking any lock or reading the clock. Measured **on the Wayland path**
  (offscreen is not comparable, it has no socket): the SDLop pump is **299 ns/call**
  against stock SDL3's **1329 ns/call** — 4.4x — *including* the non-blocking
  `poll()` that keeps a polling application reading its compositor socket.
* **`SDL_PushEvent` + `SDL_PollEvent` (2.7x).** A single mutex and a ring copy in
  each direction, and — the important part — *no syscall when no thread is
  waiting* (see below).
* **Ticks/performance counter (1.0–1.2x).** Both implementations end up in
  `clock_gettime(CLOCK_MONOTONIC)`; SDLop keeps one cached start offset
  instead of SDL3's more general tick bookkeeping.
* **`SDL_UpdateWindowSurface` (38x).** Careful with this one: it is *not* a
  like-for-like comparison of presentation cost. The `offscreen` driver of stock
  SDL3 pushes the frame through its renderer-side path, while SDLop's offscreen
  present is "the surface is already the window's buffer". The number is real but
  it says more about module count than about Wayland present speed.

## What the benchmark found (and what was fixed)

The first run had two hot paths **slower** than SDL3, which is why the benchmark
is worth keeping:

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

Both fixes are pure fast paths: the slow paths (an actual waiter, an actual
queued callback, an actual timer) are unchanged, and the whole test suite plus
the Wayland end-to-end checks were re-run after them.

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
| shared library (unstripped, with `-g`) | 0.78 MB | 2.87 MB |
| shared library (stripped) | 0.23 MB | 2.87 MB |
| exported `SDL_*` symbols | 398 | 1208 |

The stripped comparison is the meaningful one: about **12x less code** for the
same windowing, input, timing, GL and Vulkan surface APIs.
