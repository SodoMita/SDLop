# Worker wakeup and clock selection

2026-11-21. First two follow-ups to the [input loss reduction](input-loss-reduction.md)
work: suggestions 1.1 and 1.2. No new dependencies or raw callback filtering.
These are implemented candidates, not a claim that the complete suggestion
list or genuine hardware timing validation has been completed. This report
records that stage; subsequent [drop statistics and state recovery](input-state-recovery.md)
have their own results and acceptance limits.

## Verified before changing

- `src/internal/sdlop_evdev.c:worker_main` sampled `ring_was_empty` once before
  processing an epoll batch, and notified only when that initial sample was
  empty. If the consumer drained existing input after that sample, then waited
  while the worker published new input, no notification followed. The 10ms
  polling cap in `src/SDL_events.c:SDL_WaitEventTimeout` eventually recovered.
- `src/internal/sdlop_evdev.c:open_input_devices` did not request an event clock.
  The worker guessed realtime stamps by comparing them with monotonic time and
  applied an offset sampled at startup, regardless of which device produced
  them.

The comparison baseline is the **previous loss-reduction build**, not the
original small-ring library. It was copied before these edits. Tests below
were also linked against that copy and failed on the actual old behavior.

## Changes

### Wakeup

The sole producer snapshots the SDL ring's publication head before and after
processing a batch. A changed head means at least one push succeeded, even if
the consumer emptied the ring midway. This adds **no per-event bookkeeping**.

If successful publication leaves pending raw input, the worker notifies once
at the end of the epoll batch, regardless of the ring's initial empty state.
If the consumer has already consumed everything, no notification is necessary:
that consumer is handling those records before its next wait. Checking the
final empty state avoids unnecessary write syscalls while actively polling.
Full-ring failed pushes alone do not request a notification.

Interrupted eventfd writes retry on EINTR. EAGAIN means the counter is already
readable. Eventfd notification remains batched, not an additional syscall in
the inner event loop. No changes to callback dispatch or ring publication.

This fixes the **initial-empty-sample race**, not every potential delay. The
worker still drains readable devices until EAGAIN before finishing the epoll
batch. A continuously readable source or slow user callback can delay batch
completion and notification. The existing 10ms wait cap remains.

### Clock selection

Immediately after opening each input fd, before its first read,
`open_input_devices` requests `EVIOCSCLOCKID(CLOCK_MONOTONIC)` and records success
**per device**. Rejected requests are not fatal and retain the previous
realtime-offset heuristic. Negotiated devices bypass that heuristic.

The clock-mode choice is hoisted outside the per-event loop: the effective
fallback offset is UINT64_MAX for negotiated devices, making the existing
comparison false without adding a per-event clock-mode branch. No per-event
ioctl, clock_gettime, allocation, or lock was added. The old worker-level clock
sampling remains. Rejected-clock devices still inherit the heuristic's limits
(e.g. offset sampled at startup, no tracking of subsequent realtime changes).

## Deterministic regressions

`tests/test_evdev_worker.c` links the real library and reuses the process-local
pipe transport. Linker wrappers, **only in this test executable**, force the
race and emulate clock ioctl outcomes. No production test hooks or sleeps are
used to decide which interleaving occurred; condition-variable barriers order
the worker and consumer. Conditions have bounded waits, plus CTest timeouts.

For each of 100 wakeup iterations:
1. Leave an older input record in the ring.
2. Pause the worker after its next batch begins, before it reads fresh input.
3. Drain the older input and eventfd; enter SDL_WaitEventTimeout on an empty
   queue/ring.
4. At the wait's poll boundary, release the worker and let its batch finish.
5. Check actual eventfd readiness, SDL motion content, and callback count.

The first notification also receives an injected EINTR, checking retry. A
20ms idle check confirms that no extra notification arrives during that interval.
The barriers deliberately add test overhead; this is not a hardware latency
benchmark. Readiness, rather than a fragile sub-ms timing threshold, determines
pass/fail.

| Forced mid-batch drain, 100 iterations | Before | After |
| --- | ---: | ---: |
| Eventfd ready at wait | 0/100 | 100/100 |
| SDL_WaitEvent average | 10,241.5us | 38.5us |
| SDL_WaitEvent maximum | 10,552.2us | 123.6us |

Clock cases test:
- Accepted monotonic clock, exact callback/poll timestamp preservation.
- An artificial realtime-looking stamp after accepted negotiation, proving
  negotiation is authoritative rather than merely passing the old heuristic.
- Rejected ioctl with monotonic stamps, preserved exactly.
- Rejected ioctl with realtime stamps, normalized near corresponding monotonic
  time (5ms tolerance for the separate clock samples, not event delivery time).
- Exactly one setup request before any worker reads that device.

All five registered test cases pass in optimized and ASan+UBSan builds. The old
build fails all five contracts: it lacks clock setup and misses the forced
wakes. Its existing fallback timestamp behavior passes the ordinary clock
values, as expected; the tests do not mislabel that as previously broken.

Raw test results: [before](benchmarks/2026-11-21-worker-before.txt),
[after](benchmarks/2026-11-21-worker-after.txt).

## Load comparison and timing gate

Same rootless pipe harness, real 37-TU dummy/fbdev library, GCC12.2 -O2 -g,
Linux6.1.158+, two CPUs. Three repetitions/build, both poll and wait modes:
- 100,000 two-axis reports/mode at 16,000 reports/s.
- 1,000,000 two-axis reports/mode at 256,000 reports/s, burst 16.

**Every trial in both builds delivered all native records, callbacks and SDL
motions and met the mean-rate gate.** Per build this is 6.6M reports / 19.8M
native records / 13.2M SDL motions. The pipe transport rejects EVIOCSCLOCKID;
these load timings exercise the fallback path. Accepted negotiation is covered
by contract tests, not a genuine evdev timing measurement.

Medians of per-trial worker-callback percentiles, microseconds:

| Reports/s | Mode | p50 before → after | p95 before → after |
| --- | --- | ---: | ---: |
| 16,000 | poll | 3.4 → 3.4 | 5.3 → 4.9 |
| 16,000 | wait | 26.3 → 24.7 | 41.8 → 36.5 |
| 256,000 | poll | 58.7 → 54.3 | 558.5 → 360.0 |
| 256,000 | wait | 3.5 → 2.5 | 19.7 → 22.0 |

**The initial sequential comparison did not clear the ~5% callback gate:**
256k/wait p95 increased 11.7%. The three individual before p95 values were
25.8/19.7/18.5us; after values were 23.7/17.6/22.0us. Before and after were
sequential sets, not randomized experiments. This is mixed userspace scheduling
evidence, not proof of an intrinsic 11.7% dispatch-cost change. Those results
are retained, not replaced by the good forced-race result.

To investigate that specific high-load result, a **balanced interleaved**
follow-up alternated pair order (before/after, after/before) for six trials per
build, each one million reports at 256kHz in wait mode. All trials passed
complete-delivery and offered-rate checks. Medians of callback percentiles:

| Interleaved 256k/wait | Before | After | Change |
| --- | ---: | ---: | ---: |
| p50 | 2.35us | 2.45us | +4.3% |
| p95 | 17.7us | 16.6us | -6.2% |

**This follow-up clears the ~5% gate on these rootless median measurements.**
It suggests scheduling/run-order variation mattered in the sequential result;
it does not establish genuine evdev/hardware latency or erase the earlier
unfavorable run. The p50 comparison uses rounded per-trial output, and sits
near the threshold. Dedicated hardware confirmation remains necessary.
[All interleaved results](benchmarks/2026-11-21-worker-load-interleaved.txt)
are preserved, including unfavorable individual pairs.

Logs preserve every trial, including an earlier implementation attempt that
notified even after the consumer had consumed all records and performed a
clock-mode check per event:
- [Baseline load](benchmarks/2026-11-21-worker-load-before.txt)
- [First attempt](benchmarks/2026-11-21-worker-load-first-attempt.txt)
- [Final load](benchmarks/2026-11-21-worker-load-after.txt)

## Validation and remaining sequence

- All 18 existing rootless regression executables were rebuilt against the
  updated real library and passed, including backpressure, 8M-record ring
  stress, event/repeat tests, no-XKB keycodes and the RAM/PPM framebuffer path.
  X11/display-specific portions remain skipped; runtime XKB/layout coverage
  remains unavailable here.
- Optimized and sanitized worker contract cases passed. No ASan/UBSan reports.
- Changed production source passes -Wall -Wextra -Werror syntax checks;
  `git diff --check` passes.
- CMake is unavailable here; CTest registration was added but validation used
  manual real-library builds. Kernel uinput remains unavailable for the reason
  documented in the loss-reduction report; no new kernel/hardware claim.

At the time of these measurements, drop statistics and kernel-state recovery
were not implemented; see their subsequent [implementation report](input-state-recovery.md).
SDL report aggregation and hotplug/device identity remain pending. Focus routing is not being replaced with compositor input. KMS,
Vulkan offload and new Wayland protocols remain separate work. The balanced
rootless callback check passes the median gate, but the mixed sequential
results and missing hardware validation preclude a universal no-slowdown claim.
