# Reducing high-load input losses

2026-11-21. Production changes following the [sustained baseline](input-load-sustained.md).
No new dependencies, no motion coalescing, no changed raw callback filtering.
This report describes the loss-reduction stage before the subsequent
[worker wakeup/clock changes](input-worker-wakeup-clock.md); measurements below
are retained for that stage, not relabeled as current-build results. The
queue-gated state behavior in those original measurements was later corrected:
the current adapter advances SDL state while full and drops input notifications
that cannot be queued. See the current pressure tests below.

## Verified loss sites and changes

- `src/internal/sdlop_evdev.c:SDLOP_RawInputPump` originally deferred raw
  records until there were three free slots in the SDL queue. Under sustained
  pressure that also froze key/button/mouse state queries. The current pump
  always translates records and applies state. `SDLOP_PushInputEvent` preserves
  event-watch/filter behavior and drops a notification quietly if no slot is
  available. Keyboard delivery preflights three slots for a possible
  keymap/key/text sequence; key-only input is also dropped if fewer than three
  slots remain. This keeps state current without an unbounded event backlog; it
  intentionally does not reconstruct dropped edges or motion events.
- `src/SDL_keyboard.c:SDLOP_KeyboardProcessRepeats` at this stage retained due
  repeats while the queue was full. The current generator instead cancels that
  deadline, schedules a fresh interval, and emits at most one repeat per pump;
  it cannot replay a pressure-sized backlog after the queue drains. The web
  generator follows the same bounded policy.
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

At the loss-reduction-stage x86-64 revision (not the current branch), `nm -S`
reported fixed raw state/rings at 1,573,760 bytes and the 8,192-slot SDL event
queue at 1,638,400 bytes: 3,212,160 bytes (about 3.06 MiB) of bounded static
storage, **2,964 KiB more** than that stage's earlier 74,624-byte raw state plus
512-slot queue. These are historical stage figures, not a current-branch BSS
measurement; later recovery/lifecycle work enlarged raw state. The arrays keep
event storage allocation-free on the hot path; raw storage is cleared at worker
initialization. Applications that never drain input can still overflow the raw
rings. Event filters/watches retain their semantics; a reentrant watcher can
itself consume queue headroom.

The previous queue gate delayed state changes with event delivery; the current
implementation keeps state current and drops notifications when they cannot fit.
Keyboard layout/key/text sequences require three slots; standalone mouse events
can use any remaining slot. It does not block the worker, recover old history,
synthesize missing motion, or prioritize releases over earlier records. Larger
buffers still exchange loss for latency.

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

- `test_evdev_backpressure`: the first pipe-backed burst checks native counts,
  SDL motion/button FIFO order, and no stuck button. The pressure phase fills
  the SDL queue to its **actual capacity**, waits for worker callbacks without
  pumping, then pumps while full. It verifies current relative/key/button state,
  intact raw callbacks/poll records, preservation of existing sentinels, and no
  replay of the deliberately dropped SDL notifications after draining. These
  assertions replace the older expectation that state remained deferred.
- `test_ring`: original 4M single-pop concurrent stress plus 4M bulk-pop stress,
  and empty/zero-limit/oversized-limit/wrap/FIFO/overflow cases.
- `test_events`: verifies full-queue repeat cancellation, no immediate replay
  after draining, and resumption on a fresh interval.
- The archived optimized and ASan+UBSan suites passed the original test set.
  Those captured logs predate the current queue-state contract; they are not
  evidence that this later change has passed those suites. See the evidence
  [index](benchmarks/README.md) for dated snapshots and their scope.
- CMake availability and full-project integration coverage are environment
  dependent; see the current turn's test results rather than reusing old logs.

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
