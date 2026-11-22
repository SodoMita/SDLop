# Reducing high-load input losses

2026-11-21. Production changes following the [sustained baseline](input-load-sustained.md).
No new dependencies, no motion coalescing, no changed raw callback filtering.
This report describes the loss-reduction stage before the subsequent
[worker wakeup/clock changes](input-worker-wakeup-clock.md); measurements below
are retained for that stage, not relabeled as current-build results.

## Verified loss sites and changes

- `src/internal/sdlop_evdev.c:SDLOP_RawInputPump` previously popped up to
  2,048 native records per pump, irrespective of room in the 512-slot SDL queue
  in `src/SDL_events.c:queue_add_with_memory`. It could update mouse/key state
  while the corresponding SDL events were rejected. Now it checks main-thread
  queue headroom **before popping**, reserving three slots for the largest
  current translation (layout change + key + text). Pending records remain in
  the raw SDL ring until the application drains the SDL queue.
- `src/SDL_keyboard.c:SDLOP_KeyboardProcessRepeats` previously advanced overdue
  repeats even when their generated events could not fit. It now keeps them
  pending until the same headroom is available. Other backend event producers
  still use their existing overflow behavior.
- `src/internal/sdlop_evdev.c:worker_main` publishes independently to two bounded
  rings. Their old capacities, 2,048 SDL-input and 1,024 raw-poll records, held
  only 10.7ms / 5.3ms at 192,000 native records/s. Both are now 32,768 records:
  about 171ms at that rate or 20ms at 1.6M native records/s. Existing init-time
  `memset(&raw, ...)` clears the backing storage before the worker starts.
- `src/SDL_events.c:event_queue` grows from 512 to 8,192 slots to cover more
  frame/scheduling bursts. This is bounded headroom, **not** a throughput or
  unlimited-losslessness guarantee. The backpressure fix remains necessary
  when the larger queue fills.
- `src/internal/sdlop_evdev.c:SDLop_PollRawEvents` uses a consumer-side bulk pop,
  copying at most two contiguous regions and publishing tail once per call.
  The head snapshot bounds the batch; concurrently arriving records can be
  returned by the next call. FIFO order and single-consumer ownership remain.

`worker_main`, `dispatch_callbacks`, and the producer ring-push algorithm are
unchanged. Callbacks still receive each native record on the worker, including
records rejected by a full ring. No added per-event worker locks, allocations,
syscalls, state queries, or callback batching. Larger storage/cache effects can
still affect measured latency; unchanged dispatch is not proof of a timing gate.

### Memory and semantics

On this x86-64 build, `nm -S` reports raw state growing from 74,624 to 1,573,760
bytes and the SDL event queue from 102,400 to 1,638,400 bytes: **2,964 KiB extra
static storage** in total (about 2.9 MiB). No additional runtime event-storage allocation.
Applications that do not drain input indefinitely still overflow the rings.
Event filters/watches retain their existing semantics; a reentrant watcher that
itself fills the queue can consume the reserved headroom.

Backpressure delays SDL state changes along with their events when the queue is
full. It does not block the worker, recover old history, synthesize motion, or
prioritize releases over preceding motion. Increased buffering can exchange
loss for latency. The conservative three-slot reserve can leave two slots unused.

## Before/after high-load comparison

Same rootless harness, compiler (`GCC 12.2`, `-O2 -g`), host (Linux 6.1.158+,
2 CPUs, uid 1001), and workload. The pre-change production library/binary were
preserved before edits. Each row below combines **three repetitions × both
poll/wait modes × one million reports**: 18M native records and 12M SDL motions
per row per build. Two axes and SYN_REPORT per report. Baseline ran first, then
modified build; these are not randomized or simultaneous experiments.

| Offered load | Raw records missed before → after | SDL motions missed before → after |
| --- | ---: | ---: |
| 64k reports/s, burst 1 | 2,730 → **0** | 2,498 → **0** |
| 256k reports/s, burst 16 | 96,328 → **0** | 78,072 → **0** |
| Saturation, burst 64 | 1,486,465 → **0** | 1,882,426 → **0** |

Across these profiles: **1,585,523 → 0 missing raw records**, and
**1,962,996 → 0 missing SDL motions**, out of 54M native / 36M SDL records per
build. All 54M callbacks arrived in each build. All 18 modified trials passed
count, timestamp-validity and offered-rate checks. At least one mode failed in
every baseline command; raw output preserves those failures.

Paced profiles achieved their requested mean rates within the existing 5%
shortfall gate. Absolute-deadline pacing catches up after scheduler delays;
these are not uniformly spaced physical arrivals. Saturation rates varied:

| Mode | Before achieved reports/s | After achieved reports/s |
| --- | ---: | ---: |
| poll | 390,961–423,611 | 346,064–431,697 |
| wait | 600,251–616,690 | 565,793–617,103 |

Thus this is a loss reduction result, **not a demonstrated maximum-throughput
improvement**. Delivering events that were formerly discarded costs consumer
work. Saturation is not identical offered throughput across builds.

Raw results (every trial, including latency and producer lateness):
- [Before](benchmarks/2026-11-21-input-loss-before.txt)
- [After](benchmarks/2026-11-21-input-loss-after.txt)

Reproduce on a supported build (repeat each command three times per build):

```sh
./build/bench_raw_latency_pipe --reports 1000000 --rate 64000 --burst 1 --axes 2
./build/bench_raw_latency_pipe --reports 1000000 --rate 256000 --burst 16 --axes 2
./build/bench_raw_latency_pipe --reports 1000000 --stress --axes 2
```

### Callback latency and API overhead checks

Separate interleaved before/after checks used 100,000 reports/mode at 16kHz,
three repetitions per build. All native/SDL records arrived in all trials.
Medians of per-trial callback latency percentiles (microseconds):

| Mode | p50 before → after | p95 before → after |
| --- | ---: | ---: |
| poll | 3.4 → 3.4 | 4.6 → 4.9 (+6.5%) |
| wait | 23.8 → 24.2 (+1.7%) | 33.6 → 35.2 (+4.8%) |

**The requested ~5% p50/p95 regression gate is not demonstrated: poll p95
exceeds it.** These measurements include userspace pipe transport and scheduling,
not isolated dispatch cost or genuine kernel/hardware latency. High-load callback
results are also mixed (e.g. median 256k/wait p95 16.2 → 20.5us); there is no
blanket no-slowdown claim. Dedicated hardware/uinput confirmation remains open.
Direct empty-API and push/poll measurements were also run interleaved, and are
preserved without dropping unfavorable results in
[the latency/API log](benchmarks/2026-11-21-input-loss-latency.txt).

## Tests

- `test_evdev_backpressure`: real worker plus pipe-backed discovery. Pauses SDL
  consumption for 600 two-axis reports with button down/up, then verifies native
  counts, SDL motion/button FIFO order and no stuck button. Repeats with the SDL
  queue filled to its **actual capacity**, pumps four times while full, verifies
  SDL relative state has not advanced, removes existing sentinels, and checks
  every pending event. Thus the test exercises backpressure even with larger
  buffers. Original build: 1,024/1,804 raw records, 512/1,200 SDL motions in the
  first burst; 0/1,200 SDL motions in the full-queue case. Modified: all records
  and button pairs in both cases.
- `test_ring`: original 4M single-pop concurrent stress plus 4M bulk-pop stress,
  and empty/zero-limit/oversized-limit/wrap/FIFO/overflow cases.
- `test_events`: adds full-queue overdue-repeat deferral and subsequent delivery.
- Modified library built from 37 real dummy/fbdev translation units. Eighteen
  rootless executables passed: ring, backpressure, events, scancode, surface,
  web-native shim, no-XKB keycodes, core, main callbacks, misc, threads, VT mask,
  synthetic fbdev, options, drop-in compatibility, dummy window API, benchmark
  accounting, and real RAM/PPM fbdev path. X11 portions skipped without DISPLAY;
  runtime XKB/layout-specific coverage unavailable here.
- ASan+UBSan real-library builds passed ring, events, backpressure, and 100k
  reports/mode at 64kHz with complete delivery and no sanitizer diagnostics.
- CMake is unavailable in this image, so the registered CTest target was not
  exercised through CMake. Manual builds linked the real library, not stubs.

## Genuine uinput attempt and remaining limits

Attempted with sudo, not just a permissions probe. `/proc/config.gz` says
`CONFIG_INPUT_EVDEV=y` and `# CONFIG_INPUT_UINPUT is not set`. Creating a temporary
`/dev/uinput` character device (10:223) succeeded, but opening it returned
`ENODEV` (19); the node was removed. No loadable-module tree is present. The
rebuilt uinput benchmark exits 77 (SKIP). This host cannot run the kernel
uinput path; use the same commands with `bench_raw_latency` on a capable host.

These results exercise a **userspace pipe into the real evdev worker**, not
kernel evdev queue overflow, physical mouse timing, multiple input fds, joystick,
or VR. They do not implement `SYN_DROPPED` handling, EVIOCGKEY/ABS/MTSLOTS resync,
public drop counters, or recovery after a ring actually overflows. The existing
worker wakeup scheme is unchanged. Those are separate follow-ups: the focused
change here prevents avoidable downstream drops and tolerates larger bursts.
