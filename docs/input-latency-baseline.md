# Input latency baseline (Phase 0)

This records the historical measurement-only Phase 0 from the supplied SDLop
improvement plan, before production input changes. Subsequent production fixes
and measured tradeoffs are documented in [input loss reduction](input-loss-reduction.md).
The numbers below remain the pre-fix baseline.

**API overhead does not require root.** `bench_raw_latency_pipe` measures the
real worker/public APIs without input devices, a display server, or elevated
permissions. Keep its userspace measurements distinct from the hardware/uinput
measurements made by `bench_raw_latency`.

**For sustained load, see the [million-report measurements](input-load-sustained.md).**
The original 2,000-report trials below were smoke tests. The larger 16/64/256 kHz
and saturation runs exposed losses that those short runs did not.

## Verified before changing

Checked against base commit `5b2b746b12e878465f3d97a6a263aaf043b493ff`:

| Location (`file:function`) | Observed behavior |
| --- | --- |
| `bench/bench_raw_latency.c:report` | p50/p95/max existed; p99 was missing. |
| `bench/bench_raw_latency.c:on_raw` | Measured kernel event timestamp to callback, despite the report calling it “uinput write -> callback”. |
| `bench/bench_raw_latency.c:main` | Injected and consumed on the same thread; no independent producer/main-thread concurrent-drain stress mode. |
| `bench/bench_raw_latency.c:main` | The second latency used injection time, not event time. It appended a sample even when the raw-ring observation timed out. |
| `bench/bench_raw_latency.c:main` | Never measured `SDL_PollEvent` or `SDL_WaitEvent`, and did not create a window. |
| `examples/raw_latency.c:main` | p99 was missing. The main thread slept without draining either input ring. |
| `examples/raw_latency.c:main` / `on_raw_event` | Sorted callback-owned samples before joining the worker. ESC does not prevent subsequent callbacks in a batch; Ctrl+C had no stats-producing handler. |
| `src/internal/sdlop_evdev.c:SDLOP_RawInputQuit` | Joins the worker, providing a safe point to read/sort callback-owned memory. |
| `src/SDL_events.c:SDL_WaitEvent` / `SDL_WaitEventTimeout` | The unbounded function delegates to the timeout implementation with -1. |
| `tests/test_x11_raw_source.c:opendir` / `fake_open` / `ioctl` | Already tests the real worker with process-local pipe-backed device discovery; no privileged input device is necessary for a userspace test. |

The rootless integration run also verified a timestamp-origin discrepancy:
`src/internal/sdlop_internal.h:SDLOP_MonoToSDLTicks` subtracts
`sdlop.init_monotonic_ns`, while `src/SDL_timer.c:SDL_GetTicksNS` subtracts the
separately initialized `timer_origin_ns`. `SDL_Init` resets the former, but not
the latter. Directly subtracting SDL event time from `SDL_GetTicksNS` is therefore
not a reliable latency measurement. The benchmark instead matches raw source
timestamps to SDL receipt times by sequence, both using CLOCK_MONOTONIC. It does
**not** change either library clock as part of this measurement item.

A preliminary check of later suggestions found an outdated assumption:
`src/internal/sdlop_evdev.c:pump_event` already gates window-system keys and
pointer input using `winsys_keyboard_focus` / `winsys_pointer_focus`. This is
not a full audit of focus transitions/resynchronization; no focus code changes.

## Run without root

With the normal project build prerequisites installed:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

# Direct API call costs: no producer active during measurement.
./build/bench_raw_latency_pipe --api-overhead

# Userspace timestamp -> real worker -> public consumers.
./build/bench_raw_latency_pipe
./build/bench_raw_latency_pipe --stress

ctest --test-dir build -R 'raw_latency_pipe_api|test_raw_latency_bench|test_ring' --output-on-failure
```

The pipe executable uses the same benchmark source and real static SDLop
library. Its `bench/raw_pipe.c` transport substitutes only device discovery,
open, capability ioctls and injection, using executable-local GNU linker wraps.
The worker still runs its real epoll/read, SPSC rings, callbacks, SDL translation
and eventfd wakeups. Only a private temporary directory and an ordinary pipe
are created; real `/dev/input` devices are never opened. This follows the
existing in-repository X11 raw-source test approach, without adding dependencies
or test hooks to production code. The executable explicitly enables its worker
and selects the dummy video driver so inherited raw-disable hints cannot cause
it to benchmark a disabled path accidentally.

`--api-overhead` reports empty `SDLop_PollRawEvents`, empty `SDL_PollEvent`,
empty `SDL_WaitEventTimeout(..., 0)`, and a push+poll pair. Results are in
**ns/call (or per pair)**. Each displayed distribution consists of 1,000 block
averages of 1,000 calls, after 10 warm-up blocks. Loop/check costs are included;
clock overhead is amortized rather than subtracted. The push+poll pair reuses
a pre-stamped user event after warm-up, so it excludes constructing/timestamping
a fresh event on every call. This is not the distribution of individually timed
calls, and a zero-timeout wait does not measure wakeup latency. Callback dispatch
is covered by the separate pipe transport trials,
not represented as an isolated API call cost.

## Synthetic workload and accounting

Both executables run an independent injection thread and main-thread consumer.
The current default sends **160,000 two-axis reports at 16,000 reports/s**:
REL_X + REL_Y + SYN_REPORT, or 480,000 native records over approximately 10 seconds
per consumer mode. `--reports` (alias `--events`), `--rate`, `--burst` and `--axes` select larger and
higher-rate loads. `--stress` now selects saturation (rate 0, batch 64), not the
old short run with relative sleeps. Absolute-deadline scheduling and actual-rate
and native-record loss counters are described in the
[sustained-load report](input-load-sustained.md).

Each report carries its sequence number in REL_X. A discovery marker identifies
the synthetic device for raw polling/callbacks, and discovery waits for the
complete SYN_REPORT before clearing queues. SDL motion preserves the sequence
in `xrel`; keep other pointers idle in uinput mode, since the SDL mouse adapter
uses `which=0` rather than the raw device ID. This measures motion, not key/button
translation or rendering.

Two trials, each with a fresh SDL initialization, cover:

- `--mode poll`: source timestamp -> callback, raw poll, and SDL poll;
- `--mode wait`: source timestamp -> callback, raw poll, and SDL wait;
- `--mode all` (default): both trials.

Wait uses a **50 ms timeout** rather than unbounded `SDL_WaitEvent`, so a broken
route cannot hang the benchmark. The label names the actual API used. Raw
polling in the wait trial includes time asleep; use the poll trial for
busy-draining raw-poll latency. Pipe timestamps are set in userspace immediately
before write, so pipe results include write/backpressure, scheduling, epoll/read
and consumer work. They do **not** include evdev/uinput or hardware input latency.

Latency reports include n/expected, missing/invalid counts, min/avg and
p50/p95/p99/max in microseconds. Sorted percentile indices are `n/2`, `n*95/100`
and `n*99/100`. Only observed events with valid matched timestamps produce
samples; timeouts are not samples, and duplicates cannot hide lost events.
Partial trials print their distribution but fail. Callback-owned timestamps
are read and paired with main-thread SDL receipt times only after worker join.

The uinput executable exits 77 when input access is unavailable. Complete
trials exit 0; setup/accounting/transport failures exit nonzero. The rootless
transport never treats missing private-directory/pipe resources as a valid skip.

## Historical short-run baseline — 2026-11-21

These original 2,000-report transport runs establish smoke coverage only.
They used one axis and 200-microsecond relative sleeps (every report for paced,
every 64 reports for burst), not today's default workload. Their API-cost trial
already used one million timed calls per operation. For current sustained-load
findings, including failures, use the [million-report results](input-load-sustained.md).
Do not compare different workload configurations as an optimization result.

Ran as **uid 1001**, without sudo, `/dev/input` or `/dev/uinput`, on Linux
6.1.158+ x86-64, GCC 12.2.0, `-O2 -g`, two visible CPUs. The actual library
implementation is unchanged from the base commit. Five repetitions each of API
overhead, paced poll/wait and burst poll/wait all succeeded. All transport
consumers received **2,000/2,000 samples with zero missing/invalid events**.

API overhead, ns/call (push+poll is per pair): each cell is the **median of that
statistic across five runs**, not a pooled distribution. In particular the last
column is the median run maximum, not the worst observation across all runs.

| Operation | p50 | p95 | p99 | Run max |
| --- | ---: | ---: | ---: | ---: |
| Empty raw poll | 1.5 | 2.5 | 3.6 | 34.8 |
| Empty SDL poll | 13.3 | 16.5 | 31.9 | 65.9 |
| Empty wait, timeout 0 | 56.4 | 86.3 | 105.2 | 124.5 |
| SDL push + poll pair | 11.5 | 12.1 | 26.7 | 46.2 |

Userspace pipe transport latency, microseconds, with the same median-of-five
convention:

| Trial | Consumer | p50 | p95 | p99 | Run max |
| --- | --- | ---: | ---: | ---: | ---: |
| Paced/poll | Callback | 4.5 | 7.0 | 18.9 | 47.6 |
| Paced/poll | Raw poll | 4.8 | 8.0 | 20.5 | 70.5 |
| Paced/poll | SDL poll | 4.8 | 8.2 | 20.3 | 70.1 |
| Paced/wait | Callback | 43.6 | 60.6 | 86.3 | 134.3 |
| Paced/wait | Raw poll | 61.6 | 84.1 | 113.3 | 178.2 |
| Paced/wait | SDL wait | 61.5 | 83.9 | 113.1 | 178.0 |
| Burst/poll | Callback | 3.0 | 49.1 | 53.8 | 65.3 |
| Burst/poll | Raw poll | 3.5 | 52.4 | 63.8 | 68.8 |
| Burst/poll | SDL poll | 3.3 | 60.1 | 68.0 | 72.6 |
| Burst/wait | Callback | 15.1 | 41.6 | 55.2 | 86.2 |
| Burst/wait | Raw poll | 40.8 | 82.4 | 102.8 | 125.8 |
| Burst/wait | SDL wait | 44.7 | 84.4 | 107.5 | 125.0 |

[Complete commands, exit codes and output from all five repetitions](benchmarks/2026-11-21-rootless.txt).
Scheduling and power noise in this shared sandbox are uncontrolled. These are
baseline diagnostics, not latency guarantees or evidence of an improvement.
There is no before/after worker change in this item.

## Validation

The normal CMake configure still cannot run here because CMake is absent, and
the Wayland/KMS build prerequisites are unavailable. To actually exercise the
library rather than stop at syntax checks, a direct GCC build compiled **37 real
translation units**: the `SDLop_SOURCES` list minus Wayland and KMS sources,
with `SDLop_VIDEO_FBDEV=1`. Header-only build prerequisites came from the
libxkbcommon 1.5.0 and Khronos EGL Registry source archives, kept in the ignored
validation directory. No runtime library was installed or newly linked; the
library still links only pthread, dl and m. This validates dummy/fbdev, not a
full graphics-backend build.

Checks completed:

- `-Wall -Wextra -Werror` syntax checks of benchmark, transport, example and
  sample-accounting test.
- 17 executable tests passed: ring (4,000,000 events), scancode, events, surface,
  web stubs, built-in-US keycodes, core, main callbacks, misc, threads, options,
  drop-in compatibility, dummy window API, VT mask, fbdev RAM/PPM, fbdev device
  ioctl shims, and benchmark accounting. Layout tests explicitly skipped due
  to unavailable XKB. X11-specific subchecks and real VT/device checks were not
  exercised. The no-XKB keycode variant was selected with `SDLOP_LIB_XKB=none`;
  the regular text/XKB keycode variant cannot pass without its runtime/data.
- The **entire 37-unit library**, rootless transport and benchmark were rebuilt
  with ASan+UBSan; API overhead, paced poll/wait, burst poll/wait and accounting
  tests all passed. Sanitized numbers were not used in the tables above.
- Help/invalid-mode/missing-mode/unknown-flag handling, and API runs with ambient
  raw-disable variables, returned the expected status.
- The real uinput benchmark was linked and run: correctly skipped with exit 77.
- The interactive example was linked; its help path ran. Actual input timing
  remains unmeasured for that example.
- `git diff --check` passed.

The accounting test covers empty/partial/complete datasets, duplicates,
zero/future timestamps, percentile indices, device/axis/marker filtering and
post-join SDL/source correlation independent of SDL clock origins. Its printed
artificial distributions are test fixtures, **not measured latency**.

## Hardware measurements still pending

Kernel timestamp -> callback/raw poll/SDL poll/SDL wait p50/p95/p99/max remain
unmeasured here because there is no input device node. Unlike API overhead,
these need an accessible evdev device; use sudo only if the user's actual device
permissions require it. On an input-capable host:

```sh
./build/bench_raw_latency
./build/bench_raw_latency --stress

# Interactive callback example: move a physical pointer/type during each run.
SDL_VIDEODRIVER=dummy ./build/raw_latency --seconds 10
SDL_VIDEODRIVER=dummy ./build/raw_latency --seconds 10 --drain
```

The example adds p99, bounded runs (1–3600 seconds), continuous draining via
`--drain`, and a Ctrl+C stats handler. It joins the worker before sorting, and
reports when its first-8,192-event buffer fills. It measures callbacks only.

Repeat each workload at least five times before/after later library changes,
using the **same harness, mode/load, kernel and power settings**. Record all
consumer distributions and loss counts. Investigate callback p50/p95 increases
over about 5% and repeat to distinguish noise; incomplete trials cannot pass.
Rootless tests are useful development coverage, but cannot substitute for the
plan's evdev/hardware performance gate. Do not compare the old
injection-to-pump metric with the new source-to-consumer metric.
