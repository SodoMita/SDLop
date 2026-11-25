# Native dispatch optimization follow-up

2026-11-21. Follow-up to [the input audit](input-audit-sdl3.md), measured against
PR #11's initial commit **0e60878**, not against an older small-ring build.

**High-load callback latency improved in the paired trials, but the complete
~5% p50/p95 acceptance gate is still uncleared. Saturation throughput also
regressed in this sample. Keep the PR draft; do not call this universally faster.**
No new runtime dependency, syscall, allocation, callback batching/filtering, or
lock was added to the per-native-event path.
The later [regression/allocation follow-up](input-regression-followup.md) adds
TSan coverage, fixes snapshot reclamation, and records additional rejected
native-path candidates. This report's measurements remain stage-specific.

## Verified work and changes

### Single-writer event counter

`src/internal/sdlop_evdev.c:drain_device_events` previously performed
`atomic_fetch_add_explicit(&raw.event_count, 1, memory_order_relaxed)` after
every callback. Inspection of every write confirms the worker is the only
writer during its lifetime; initialization resets the counter before worker
start, and shutdown joins before tearing down state. On this x86-64 build the
old operation emits `lock addq` in the native dispatch loop.

The worker now loads a local count at entry to each drain, increments it locally,
and publishes it with a relaxed atomic store **after each callback**. There is
no per-batch visibility downgrade, no change to the existing MSC_SCAN exclusion,
and no change to count-reset/quit-retention semantics. The locked RMW is absent
from the new loop's disassembly. Reader calls remain atomic. Init/quit must still
not race public raw API use; callbacks must not invoke shutdown/self-join.

`test_evdev_worker` now queries both count APIs from every callback and verifies
they equal the number of previously completed callbacks, then verifies the final
count after worker join. This covers ordinary, continuously readable and clock
fallback cases. Existing recovery overflow/count/lifecycle tests also pass.

### Conservative SPSC index caches

`src/internal/sdlop_ring.h:SDLOP_RingPush` acquired the consumer tail for every
record, and `SDLOP_RingPop` acquired the producer head for every record. Each
thread writes its own index, so these repeated cross-thread index accesses are
avoidable when a previously acquired limit already proves the operation safe.

- The producer caches an acquired tail and refreshes it only when that limit
  says full. It rechecks before rejecting a push; no newly freed slot is falsely
  counted as a drop. A stale cached tail can only underestimate free space.
- The consumer caches an acquired head and refreshes at its boundary before
  returning empty. The original acquire protects all records below that head.
- `SDLOP_RingPopBatch` refreshes the consumer cache as well, so callers may mix
  single and bulk reads. Release publication/reclamation remains per successful
  single record, or per copied batch, exactly as before.
- The private cache fields occupy existing cache-line padding; ring capacity,
  backing storage, public ABI and drop semantics are unchanged. These caches
  are owner-only plain integers, not new shared atomics. This remains an SPSC
  contract, not a multi-producer/multi-consumer queue.
- Wakeup and recovery code still sample the **actual published indices**, not
  cached values. No published entry is rewritten.

`test_ring` now combines single/bulk concurrent consumption over 4M records,
in addition to the existing 4M single-record test. A deterministic mixed-operation
reference check exercises empty/full behavior, exact drops, zero-length bulk
reads and 64-bit index wraparound. The real-worker recovery tests verify snapshot
boundaries, ring overflow and failed-query retries with these caches enabled.

## Sanitizer CI failures were test interception gaps

The preceding PR's ASan CI job failed `evdev_worker_wake` and
`test_input_transport`. Reproducing the transport test locally with
`-O2 -D_FORTIFY_SOURCE=3 -fsanitize=address,undefined` caused the same assertion;
`nm -u` showed a call to **`__poll_chk`**, not `poll`.

Fortified glibc redirects these calls, bypassing the tests' `--wrap=poll` hooks.
The worker test then never releases its producer gate, while the transport test
polls a fake fd instead of its deterministic fault stub. Both tests now wrap
`__poll_chk` as well and validate the checked buffer length before delegating
to the existing wrapper. Fortification and sanitizers stay enabled; no production
poll code or timeout was changed to hide these failures.

After commits 16bd49d/49cacbf, **all three GitHub Linux jobs passed**: bare Xvfb,
Openbox, and Openbox with ASan+UBSan. That full build also validates the desktop
backends and normal xkb environment unavailable in the local headless sandbox.
Later documentation/benchmark commits have their own CI status in the PR.

## Sustained paired measurements

Same checked `bench_raw_latency_pipe` source, real 37-TU dummy/fbdev library,
GCC12.2 -O2 -g, Linux6.1.158+, two CPUs. Three paired trials per profile,
alternating baseline/current then current/baseline then baseline/current.
No concurrent validation jobs or sanitizers. Two axes, poll and wait modes:

- 100,000 reports/mode at 16k reports/s;
- 1,000,000 reports/mode at 256k reports/s, burst16;
- 1,000,000 reports/mode at saturation, burst64.

Every trial completed with all expected callback records, raw polling records
and SDL motions and no missing/invalid probes. Per build: **12.6M reports,
37.8M native callback records and 25.2M SDL motions**. All paced trials met the
mean offered-rate gate. These are userspace pipe timestamps including write,
backpressure and scheduler time, **not physical evdev/kernel latency**.

Medians of per-trial callback percentiles, microseconds:

| Profile | Mode | p50 before → after | p95 before → after | p50 / p95 change |
| --- | --- | ---: | ---: | ---: |
| 16k/s | poll | 3.4 → 3.5 | 4.8 → 5.0 | +2.9% / +4.2% |
| 16k/s | wait | 24.2 → 25.2 | 36.1 → 40.2 | +4.1% / **+11.4%** |
| 256k/s burst16 | poll | 59.4 → 48.8 | 672.7 → 240.2 | −17.8% / −64.3% |
| 256k/s burst16 | wait | 4.9 → 5.1 | 22.3 → 21.1 | +4.1% / −5.4% |
| Saturation | poll | 602.4 → 408.9 | 908.8 → 889.2 | −32.1% / −2.2% |
| Saturation | wait | 3.1 → 2.6 | 20.7 → 19.7 | −16.1% / −4.8% |

Five of six profile/mode median rows are within the ~5% callback gate or improve,
but **16k/wait p95 fails**. Earlier failed gates are not erased by this run.

**Throughput caveat:** median achieved saturation report rate fell from
399,976 to 352,196 reports/s in poll mode (−11.9%), and 633,323 to 588,975 in
wait mode (−7.0%). Offered load is not fixed in saturation, so its latency gains
are NOT a same-load speedup. The fixed-rate 256k row is the stronger end-to-end
result. No claim that caches improve every CPU workload or that removing one
locked operation can overcome transport/scheduler cost.

Full data, including all p99/max and injection-rate output:
[combined optimization](benchmarks/2026-11-21-opt-load.txt).

### Counter-only comparison retained

A separate, subsequently run three-pair experiment compares the same baseline
with only the counter change (old ring implementation). It was not uniformly
better and is not substituted for the combined implementation's measurements:

| Profile | Mode | p50 before → counter-only | p95 before → counter-only |
| --- | --- | ---: | ---: |
| 16k/s | poll | 3.5 → 3.6 | 5.1 → 5.6 |
| 16k/s | wait | 25.5 → 25.3 | 43.3 → 41.0 |
| 256k/s | poll | 57.4 → 58.1 | 616.3 → 970.9 |
| 256k/s | wait | 5.4 → 5.0 | 24.2 → 21.4 |
| Saturation | poll | 616.0 → 504.0 | 897.1 → 902.7 |
| Saturation | wait | 2.0 → 2.4 | 8.9 → 12.6 |

All records delivered; [full counter-only log](benchmarks/2026-11-21-opt-count-load.txt).
These separately timed experiments are exploratory attribution, not a three-way
controlled ranking. Baseline variation remains substantial.

## Isolated dispatch CPU-work proxy

New rootless `bench_raw_dispatch` includes the real private evdev implementation,
without production hooks. Fake reads supply full 64-record batches; simulated
consumers immediately reclaim the previous batch. A callback runs for **every
record**, both ring publications execute, and the benchmark validates exact
counts, callback checksum and zero drops. Five alternating-order trials/variant,
**20.48M native records/trial**, optimized unsanitized build.

`CLOCK_THREAD_CPUTIME_ID` brackets blocks of 20,480 events to measure thread CPU
work rather than scheduler waits; boundary timer cost is amortized, not
subtracted. This deliberately excludes actual read syscalls, transport,
cross-core contention and concurrent consumer work. It is a proxy for the native
loop's work, **not per-event latency, device timing or the callback gate**.

Medians of per-trial block-average percentiles, ns/event:

| Variant | p50 | p95 |
| --- | ---: | ---: |
| Baseline | 17.14 | 23.91 |
| Counter only | 10.73 | 19.56 |
| Counter + index caches | 13.23 | 21.88 |

The combined version reduces this proxy p50 by 22.8% and p95 by 8.5%. The cache
version is slower than counter-only without real contention, so the cache
benefit must not be inferred from this test; its intended tradeoff is reducing
cross-thread index traffic, with the end-to-end results above determining
acceptance. [CPU-time log](benchmarks/2026-11-21-opt-dispatch.txt).
An initial MONOTONIC-wall-clock attempt is also retained separately
[here](benchmarks/2026-11-21-opt-dispatch-wall.txt); its samples include preemption
and are not mixed with the CPU-time medians. ASan+UBSan/fortified dispatch checks
also passed; sanitizer timings are not used for performance comparison.

## Validation and reproduction

- [Optimized contracts](benchmarks/2026-11-21-opt-tests.txt) pass.
- [ASan+UBSan + FORTIFY_SOURCE=3 contracts](benchmarks/2026-11-21-opt-sanitizers.txt)
  pass, including forced wakeup interleaving, uninterrupted native callback
  counts, busy shutdown, recovery, exact loss accounting and cache wrap/mixing.
- Shared export gate passes: 737 SDL functions + 7 SDLop extensions. Link-time
  dependencies remain libc/libm/loader, not new optional libraries.
- [Rootless API overhead](benchmarks/2026-11-21-opt-api.txt), three alternating
  pairs, is also retained. Empty raw poll p50/p95 medians were 2.1/2.1 → 1.9/3.4
  ns; push/poll 11.9/14.1 → 12.0/18.6 ns. These tiny block averages are noisy
  and mixed, not evidence that every API got faster.
- Local layout tests explicitly use `SDLOP_LIB_XKB=none` and skip xkb layout
  coverage; full Linux CI provides the normal display/xkb environment. Physical
  uinput remains unavailable locally, and no hardware result is asserted.
- No TSan or weakly ordered hardware run. The acquire/release ownership proof
  and tests are not a substitute for all architecture coverage.

Normal project build (existing prerequisites only):

```sh
cmake -S . -B build -DSDLop_BUILD_TESTS=ON -DSDLop_BUILD_BENCHMARKS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure -R 'test_ring|test_events|evdev_|input_transport|raw_latency'
build/bench_raw_dispatch
build/bench_raw_latency_pipe --api-overhead
build/bench_raw_latency_pipe --reports 100000 --rate 16000 --axes 2
build/bench_raw_latency_pipe --reports 1000000 --rate 256000 --burst 16 --axes 2
build/bench_raw_latency_pipe --reports 1000000 --stress --axes 2
```

For isolated baseline dispatch reproduction, compile the same benchmark source
with `SDLOP_EVDEV_SOURCE` naming the 0e60878 `sdlop_evdev.c`, and put that commit's
`src` before the current `src` in the include path (so its ring header matches).
For counter-only, use current evdev source with the baseline ring header. Link
against the matching real static library with `-Wl,--wrap=read -UNDEBUG`.
Do not compare a cached-ring source with mismatched ring metadata layouts.
