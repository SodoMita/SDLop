# Input reliability audit and SDL3 comparison

2026-11-21. Scope: the accumulated evdev loss-reduction, wakeup/clock and
key/button-recovery work on this branch. No rendering/backend redesign, new
runtime dependencies, compositor input rerouting, or upstream code port.
**Correctness regressions pass; the requested ~5% callback p50/p95 gate remains
uncleared. This change is for draft review, not performance acceptance.**

The subsequent [native-dispatch optimization](input-dispatch-optimization.md)
removes the single-writer counter RMW, caches conservative SPSC index limits,
and fixes fortified-poll test interception. It improves some high-load results
but retains an uncleared overall gate and reports saturation throughput costs.
The historical measurements below remain specific to the audit stage.
A later [regression/allocation follow-up](input-regression-followup.md) adds
TSan and broader queue tests, fixes no-op callback allocations and post-join
snapshot leaks, and rejects two additional native-path candidates that failed
the timing gate.

## Audit findings and fixes

| Verified location | Reproduction / consequence | Fix / coverage |
| --- | --- | --- |
| `src/SDL_events.c:SDL_PumpEvents` | An event watch recursively pumping while a button is being dispatched consumes the following motion before the outer button is queued. Nested recovery can also advance mutable snapshot state. | Only the outermost main-thread pump advances producers. `test_evdev_recovery` checks ordering and recursive pumping while recovery releases are delivered. |
| `src/internal/sdlop_evdev.c:drain_device_events` | An always-readable fd never reaches EAGAIN; notifications, control requests and other devices can be postponed indefinitely. | Limit service to 32 reads of up to 64 native records/device/pass; level-triggered epoll revisits remaining input. Budget exhaustion is NOT a valid snapshot boundary: publish a failed query attempt and retry later. `evdev_worker_busy` verifies notification before EAGAIN and shutdown while still continuously readable. |
| `src/internal/sdlop_evdev.c:SDLOP_RawInputQuit` | An interrupted control-eventfd write could leave the worker asleep during join. | Retry EINTR; busy-worker test injects the first shutdown-write interruption. |
| `tests/uinput_synth.c:emit`, benchmark producer | Writes silently ignored errors; producer accounting could claim complete injection despite transport failure. | Checked bool transport API shared with the pipe helper; reject short writes/errors, retry EINTR/backpressure under an absolute one-second deadline. Count only complete reports; fail discovery/benchmark on errors. New transport fault tests and benchmark producer-failure accounting regression. |
| `examples/raw_latency.c:main` | Unbounded drains could prevent `--seconds`/signal checks under continuous load. | Bound each drain iteration; join worker before sorting callback samples. Reject zero timestamps; use a floating sum rather than overflowing an integer latency total. |

The ordering and endless-drain failures reproduce against the preserved
pre-audit library and pass with these fixes:
[reproductions](benchmarks/2026-11-21-audit-repros.txt),
[optimized tests](benchmarks/2026-11-21-audit-tests.txt),
[ASan+UBSan](benchmarks/2026-11-21-audit-sanitizers.txt).
No per-native-event lock, allocation or syscall was added to callback dispatch.
The fairness budget is per read, not per event. This is a source property, not
a substitute for the failed timing gate.

## Comparison with pinned upstream SDL3

Reference: **release-3.4.20**, commit
[`f00dc919ca35b9756316e86c4777392de442f9b2`](https://github.com/libsdl-org/SDL/tree/f00dc919ca35b9756316e86c4777392de442f9b2).
This matches the repository's declared SDL API/CI reference; it is not a claim
about the latest SDL release. Upstream static Release was built locally with
dummy/offscreen video and unavailable desktop/audio/device dependencies disabled.
Paths/functions below are source evidence at that exact revision.

| Area | SDLop implementation | Upstream reference / difference |
| --- | --- | --- |
| Input delivery | `sdlop_evdev.c:worker_main` uses epoll and batched reads; publishes two fixed SPSC rings, then global worker callbacks. | [`SDL_evdev.c:SDL_EVDEV_Poll`](https://github.com/libsdl-org/SDL/blob/f00dc919ca35b9756316e86c4777392de442f9b2/src/core/linux/SDL_evdev.c#L300) reads/translates synchronously when polled. No directly equivalent public raw-worker callback to benchmark. This comparison concerns its Linux evdev path, not all SDL input backends. |
| Timestamp domain | `scan_devices` requests EVIOCSCLOCKID/CLOCK_MONOTONIC before reads and tracks success/device. Worker heuristic remains only for failures. Raw timestamps are monotonic-domain nanoseconds, not SDL init-relative ticks. | [`SDL_EVDEV_GetEventTimestamp`](https://github.com/libsdl-org/SDL/blob/f00dc919ca35b9756316e86c4777392de442f9b2/src/core/linux/SDL_evdev.c#L1025) derives an initial offset to SDL_GetTicksNS and adjusts future timestamps. That file does not negotiate EVIOCSCLOCKID. Different clock contracts; no hardware latency inference from direct subtraction across libraries. |
| Damage recovery | `SDLOP_RawInputPump` discards damaged device input through SYN_REPORT; worker EVIOCGKEY snapshot/boundary handshake repairs supported key/button state, including SDL-ring overflow. Failed queries preserve uncertainty. | [`SDL_EVDEV_sync_device`](https://github.com/libsdl-org/SDL/blob/f00dc919ca35b9756316e86c4777392de442f9b2/src/core/linux/SDL_evdev.c#L777) handles touchscreen ABS/MT state and explicitly has `TODO: sync devices other than touchscreen`. SDLop does NOT implement ABS/MT translation/recovery, so neither implementation is a superset. |
| Relative frames | `pump_event` currently emits a separate SDL motion for REL_X and REL_Y. Raw callbacks remain native/per-event. | [`SDL_EVDEV_Poll`, SYN_REPORT](https://github.com/libsdl-org/SDL/blob/f00dc919ca35b9756316e86c4777392de442f9b2/src/core/linux/SDL_evdev.c#L490) emits both axes together. SDLop frame aggregation is still a separate pending item. |
| Wheel selection | SDLop accumulates high-resolution wheel values/device, clears partial state after damage, but still immediately emits legacy wheel records too. | Upstream `SDL_EVDEV_Poll` ignores legacy wheel values when high-resolution capability was detected (`high_res_wheel` / `high_res_hwheel`, lines 461–482; capability scan at 937). SDLop can double-count devices producing both; verified by source, deferred with report aggregation rather than silently claiming parity. |
| Queue/backpressure | `SDL_events.c` has a fixed 8192-slot main queue and side queue; raw translation reserves three slots and resumes later. Two native rings are 32768 slots each. Overflow is counted, not impossible. | [`SDL_events.c:SDL_AddEvent`](https://github.com/libsdl-org/SDL/blob/f00dc919ca35b9756316e86c4777392de442f9b2/src/events/SDL_events.c#L1043) uses allocated/reused linked entries, a 65535-event limit and queue locking; polling uses a sentinel. SDLop's lower microbenchmark cost comes with different queue/threading contracts. |
| Device lifetime | `scan_devices` runs at worker initialization; SDLop indices are startup-local. No hotplug identity/reopen state machine. | Upstream Linux evdev has add/remove handling and optional udev integration. This PR adds no libudev; direct kernel-UAPI hotplug remains separate work. |

### Identical-source queue benchmark (rootless)

Both executables use unchanged `bench/bench_events.c`, dummy video, static
libraries, GCC12.2 and optimized builds; raw input is disabled. Five paired
trials, alternating order. Medians of whole-run averages:

| API workload | SDLop | SDL3 3.4.20 |
| --- | ---: | ---: |
| Push + drain user event | 18.3 ns/event | 50.1 ns/event |
| Empty pump/poll loop in benchmark | 16.1 ns/call | 39.9 ns/call |

Every measured run returned all 1,000,000 user events in order. These are NOT
per-event latency percentiles, a same-contract queue contest, device latency,
or proof that SDLop input is faster. Queue capacity, locking, allocation and
poll semantics differ. Full output:
[SDL3 comparison](benchmarks/2026-11-21-audit-sdl3.txt).
Rootless SDLop API block-average percentiles are separately recorded in
[audit API overhead](benchmarks/2026-11-21-audit-api.txt); they do not test the
worker's callback cost.

ABI probe against the pinned upstream headers: **196 layout values and 525
keyboard constants identical**. Shared export gate: **737 SDL functions + 7
SDLop extensions** declared/exported. These are ABI/symbol checks, not behavioral
parity guarantees.

## Audit-only before/after load measurements

Baseline: preserved pre-audit library (already includes earlier loss, wakeup and
recovery work). Both binaries link the **same current checked transport/harness**.
Three paired repetitions/profile, alternating pair order, two axes, poll and
wait: 100k reports/mode at 16k/s, 1M at 256k/s burst16, 1M saturation burst64.
Linux6.1.158+, two CPUs, GCC12.2 -O2 -g. No concurrent validation jobs/sanitizers.
These userspace pipe timestamps include scheduling/write/backpressure, not
kernel/hardware latency. Saturation is achieved throughput, not matched load.

Medians of complete-trial callback percentiles, microseconds:

| Profile | Mode | p50 before → after | p95 before → after |
| --- | --- | ---: | ---: |
| 16k/s | poll | 3.5 → 3.4 | 5.2 → 5.2 |
| 16k/s | wait | 24.9 → 24.6 | 42.1 → 36.7 |
| 256k/s burst16 | poll | 55.5 → 58.3 | 467.0 → 692.6 |
| 256k/s burst16 | wait | 2.7 → 5.3 | 20.9 → 19.7 |
| Saturation | poll | 605.5 → 586.4 | 1007.4 → 841.5 |
| Saturation | wait | 1.7 → 2.4 | 67.9 → 17.7 |

Saturation/poll baseline has only **two complete trials**: the third lost 5758
raw polling records and returned 1,652,350/2,000,000 SDL motions (recovery can
discard additional uncertain motion after a raw-ring loss). That incomplete
trial is retained in the log, excluded from latency-gate medians, and prevents
treating this row as a clean three-pair comparison. All other baseline modes
and all current-build modes delivered fully, with no invalid probes; paced mean
rates met the harness's 5% shortfall tolerance. Per build, 12.6M reports and
37.8M callback records were injected/delivered; all 25.2M expected SDL motions
arrived on the current build. This is not a zero-loss guarantee for finite queues.

**The gate remains uncleared**: e.g. 256k/poll p50 +5.0%, p95 +48.3%;
256k/wait p50 +96.3%; saturation/wait p50 +41.2%, despite better p95.
Absolute differences and noisy scheduling do not justify declaring no slowdown.
The earlier recovery-stage gate also remains uncleared; these audit-only trials
do not erase it. All trials: [audit load log](benchmarks/2026-11-21-audit-load.txt).

## Remaining gaps / review risks (not implemented here)

- **Wheel duplicate suppression and report aggregation:** described above;
  add per-device capability selection and frame ordering tests in that item.
- **Focus/VT lifecycle snapshots:** recovery is triggered by damage/overflow,
  not every focus acquire, VT reacquire or device-open. Already-held state at
  startup/focus changes is not guaranteed to be restored.
- **Lost relative history:** state snapshots cannot reconstruct motion. No
  pending-relative accumulator is implemented. Recovery intentionally discards
  uncertain native records from the SDL view; raw callbacks stay native.
- **Continuous-load recovery:** the fairness budget keeps worker control live,
  but a device that never reaches EAGAIN can defer snapshot recovery indefinitely.
  Callback code must itself remain nonblocking; budgets cannot preempt user code.
- **Callback registration lifetime:** pre-existing snapshot replacement retains
  prior callback allocations. The max active callback count does not bound
  churn allocations. The subsequent [allocation fix](input-regression-followup.md)
  avoids absent-unregister churn and frees retained versions after join; active
  lifetime churn can still retain memory. Unregister is not a worker quiescence barrier. Public docs
  now warn against freeing userdata until worker stop/join; no per-event
  reclamation mechanism/lock was introduced. Init/quit must not race API use;
  never quit from the worker callback (self-join).
- **Finite queue pressure and reentrant application callbacks:** only nested
  producer pumping is guarded, not arbitrary application enqueue/filter/watch
  behavior. The three-slot reserve is for normal adapter output, not an
  unlimited recursive application producer.
- **Hardware and backend coverage:** uinput is unavailable in this sandbox
  (kernel CONFIG_INPUT_UINPUT unset, open ENODEV); no physical evdev or display
  server exists. No kernel/device latency, live X11/Wayland/VT transition, ABS/MT,
  or hotplug claim. Local build exercised 37 real dummy/fbdev TUs, not the full
  desktop backend build; the normal CI matrix must validate that build.
- **Local test environment:** xkb data/runtime is absent, so explicit
  `SDLOP_LIB_XKB=none` exercises the built-in table and layout tests skip. An
  initial default-keymap test attempt failed text checks in this environment;
  it was not a successful xkb validation. ASan needs
  `allocator_may_return_null=1` for the pre-existing oversized-surface test;
  leak detection remained enabled. Native `test_web`/C++ linkage passed, but no
  Emscripten toolchain was run.

## Reproduction in a normal development environment

Use existing project prerequisites, not new runtime libraries:

```sh
cmake -S . -B build -DSDLop_BUILD_TESTS=ON -DSDLop_BUILD_BENCHMARKS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure -R 'evdev_|input_transport|raw_latency|test_ring|test_events'
# Repeat with -DSDLOP_SANITIZERS=ON in a separate build directory.
build/bench_raw_latency_pipe --api-overhead
build/bench_raw_latency_pipe --reports 100000 --rate 16000 --axes 2
build/bench_raw_latency_pipe --reports 1000000 --rate 256000 --burst 16 --axes 2
build/bench_raw_latency_pipe --reports 1000000 --stress --axes 2
sh tests/run_abi_check.sh /path/to/SDL-3.4.20/include
```

All fault tests use process-local pipe discovery/ioctl/linker wrappers and the
real library worker; no production fault hooks. Transport faults are a separate
standalone test. Preserve every failed/incomplete measurement when comparing.
Earlier evidence remains in [loss reduction](input-loss-reduction.md),
[sustained loads](input-load-sustained.md),
[wakeup/clock](input-worker-wakeup-clock.md) and
[state recovery](input-state-recovery.md); each measures its own stage/baseline.
