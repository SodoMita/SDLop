# Regression testing and allocation optimization

2026-11-21. Baseline: PR #11 head **9b19243**. All three Linux CI jobs at that
baseline passed (bare Xvfb, Openbox, Openbox + ASan/UBSan). This follow-up adds
regressions, retains only an off-event-path allocation/lifetime fix, and records
**rejected** throughput experiments. The earlier native callback performance gate
is still uncleared; no new native callback speedup is claimed here.

## Accepted fix: callback snapshot ownership and no-op unregister

Verified in `src/internal/sdlop_evdev.c`:

- `SDLop_UnregisterRawEventCallback` allocated and published a replacement even
  when the requested callback wasn't registered, including an empty list.
- Register/unregister preserved old immutable lists for in-flight readers, but
  `SDLOP_RawInputQuit` freed only the current list after joining the worker.
  Superseded lists leaked across completed shutdowns. This was a pre-existing
  gap identified by the earlier audit, not a newly introduced ring-cache bug.

Changes:

1. Under the existing registration mutex, check for a match before allocating.
   An absent callback now causes no allocation or publication.
2. Each new immutable list links to its predecessor, before release publication.
   The worker does not traverse this ownership link. No previously published
   list is modified or prematurely freed.
3. After `pthread_join`, shutdown walks and frees the entire chain, then clears
   the root. Allocation-failure paths preserve the old list/chain.

There is **no new per-native-event operation**: no epoch, reference count, lock,
allocation or syscall. The event dispatch fields keep their offsets. In the
validation GCC12.2 -O2 object files, `drain_device_events` was **byte-identical**
to baseline (720 bytes). This is specific compiler evidence, not a universal
architecture/performance guarantee. Shutdown does more reclamation work, linear
in the number of successful list replacements.

Unregister is still **not** a worker quiescence barrier. Userdata must remain
alive until worker stop/join. Genuine list churn retains old versions during the
worker lifetime; it is not bounded by the active callback limit. The fix prevents
no-op churn and reclaims versions at shutdown, not during active readers.
Init/quit must not race public API use. Never quit from the worker callback.
Public documentation now states these limits explicitly.

### Regression and rootless measurements

`tests/test_evdev_callbacks.c` exercises the real pipe-backed worker:

- Pause a callback while main unregisters it and replaces the list; no tracked
  snapshot may be freed while that reader holds the old version.
- 10,000 absent unregisters and unregistering NULL/empty lists.
- 500 registration/removal cycles concurrent with native dispatch; every one
  of 501 subsequent native records reaches the persistent callback.
- The maximum active callback limit, duplicate removal, and forced allocation
  failures in both registration and unregistration, without losing the live list.
- Reclaim every tracked successful snapshot at shutdown, then repeat init/list
  replacement/quit for three additional lifetimes without a dangling chain.

Same test linked against baseline reproduces both failures. Five alternating-order
paired trials, all baseline exits expected to be 1 and all fixed exits 0:

| Result per trial | Before | After |
| --- | ---: | ---: |
| Allocations for 10,000 absent unregisters | 10,000 | **0** |
| Allocations for empty unregister | 1 | **0** |
| Total tracked snapshots allocated | 11,020 | 1,018 |
| Snapshots freed after joins, four lifetimes | 4 | **1,018** |
| Persistent native callback delivery | 501/501 | 501/501 |

Thread-CPU block-average ns/absent-unregister, medians of five trial percentiles:
**p50 87.9 → 37.5; p95 300.4 → 53.5**. Each trial brackets 1,000 blocks of ten
calls using CLOCK_THREAD_CPUTIME_ID. Boundary timer cost is amortized, not
subtracted. The allocator wrapper records allocations, so these are instrumented
registration-API costs, **not native callback/device latency** or the callback
gate. Allocation and reclamation counts are the stronger deterministic result.
[All paired trials](benchmarks/2026-11-21-opt2-callbacks.txt).
The [initial reproduction](benchmarks/2026-11-21-opt2-callback-before.txt) is
retained separately; it preceded the added three restart lifetimes.

## Additional regression coverage

- `test_ring`: all previous 8M concurrent single/mixed records and index-wrap
  checks, plus **30 capacity/record-size combinations**. Capacities 1, 2, 4, 64,
  1024 and 32768; sizes 1, 3, 24, 31 and 64 bytes. Checks split byte copies,
  exact capacity/full-drop behavior, single/bulk mixing, zero-length requests
  with a NULL destination, and refreshing a cached partial prefix rather than
  returning a stale short batch.
- New `test_evdev_dispatch`: direct execution of the real production drain with
  fake reads, complementing threaded worker tests. Checks the exact existing
  32-read/2048-native-record service bound, interrupted/short reads, genuine
  EAGAIN vs budget/EOF/error results, MSC_SCAN exclusion, ordered per-event
  publication into **both** rings before callback, and both public event counts
  inside and after callbacks. No production testing hooks.
- Existing queue pressure, SYN_DROPPED/overflow recovery, independent loss counts,
  snapshot boundaries, query failure/retry, recursive pump ordering, clock modes,
  forced missed-wakeup interleaving, busy shutdown and broader headless tests pass.

### Sanitizers and limits

Final accepted code:

- [Optimized tests](benchmarks/2026-11-21-opt2-tests.txt): pass.
- [ASan+UBSan with FORTIFY_SOURCE=3](benchmarks/2026-11-21-opt2-sanitizers.txt):
  pass; leak detection enabled, including the callback ownership regression.
- [ThreadSanitizer](benchmarks/2026-11-21-opt2-tsan.txt): ring stress, recovery,
  all six worker modes and callback churn/lifetime tests pass with no reported
  races. GCC warns that the generic SDL memory-barrier API's atomic_thread_fence
  is unsupported by TSan; the ring/publication paths use instrumented C11 atomic
  loads/stores. This is not exhaustive race or weak-memory-architecture proof.
- Shared exports: 737 SDL functions + 7 SDLop extensions, unchanged.
- Local real 37-TU dummy/fbdev library, GCC12.2; no-xkb mode/layout skips. The
  existing ASan key-name truncation compile warning remains unrelated. No
  physical uinput/display server/weakly ordered hardware test is asserted.
- Fresh full-backend CI runs are tracked on PR #11; local results do not replace
  those checks or erase the historical performance gate failures.

## Rejected experiments: no production changes retained

Two candidates were tested against 9b19243, with the same checked rootless pipe
harness and all records delivered:

1. Larger reads (64 → 256 native records), independent 2048-record and 32-read
   caps preserving bounded service, together with cached full-batch raw polling.
2. Cached full-batch polling alone, retaining original reads. It reused a cached
   acquired head only when enough records were present to fill the entire request,
   refreshed otherwise, and avoided cache writes on empty/zero-length calls.

Both passed correctness/sanitizer testing but **failed the callback timing gate**.
Each experiment used three alternating pairs/profile, two axes, poll + wait:
100k reports/mode at 16k/s, 1M at 256k/s burst16, 1M saturation burst64. Per build
per experiment: 37.8M native callback records and all expected raw records/SDL
motions. Every paced trial met the mean rate requirement. No missing/invalid
probes. These userspace pipe measurements include writes/backpressure/scheduling,
not kernel/device timing. Saturation is not a matched offered load.

Medians of per-trial callback percentiles, microseconds, before → candidate:

| Candidate | Profile/mode | p50 | p95 | Decision |
| --- | --- | ---: | ---: | --- |
| Larger reads + bulk cache | 16k/poll | 3.5 → 3.5 | 5.3 → 5.3 | within gate |
| Larger reads + bulk cache | 16k/wait | 26.5 → 26.6 | 42.6 → 42.5 | within gate |
| Larger reads + bulk cache | 256k/poll | 52.8 → 58.8 | 606.2 → 710.0 | **+11.4% / +17.1%, reject** |
| Larger reads + bulk cache | 256k/wait | 5.1 → 5.2 | 20.1 → 19.3 | within gate |
| Larger reads + bulk cache | saturation/poll | 405.9 → 380.6 | 875.9 → 832.8 | not sufficient for acceptance |
| Larger reads + bulk cache | saturation/wait | 2.5 → 2.2 | 18.2 → 13.8 | not sufficient for acceptance |
| Bulk cache only | 256k/poll | 48.9 → 53.3 | 441.0 → 745.6 | **+9.0% / +69.1%, reject** |

The separately timed baselines vary substantially. We cannot attribute these
results solely to a specific instruction or to read sizing; the bulk-only
experiment also failed. That uncertainty is not grounds to keep candidates as
accepted optimizations or replace unfavorable trials with a favorable rerun.
The original read sizing and original bulk-pop implementation were restored
before the accepted allocation fix and all final regression/sanitizer runs.

Full logs: [combined candidate](benchmarks/2026-11-21-opt2-load.txt),
[bulk-only candidate](benchmarks/2026-11-21-opt2-bulk-load.txt).
A [rejected patch](benchmarks/2026-11-21-opt2-rejected.patch) preserves the exact
combined candidate against 9b19243 for reproduction; it is **not applied**.
Older performance reports remain historical stage-specific comparisons.

## Reproduce

Using existing project prerequisites:

```sh
cmake -S . -B build -DSDLop_BUILD_TESTS=ON -DSDLop_BUILD_BENCHMARKS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure -R 'test_ring|test_events|evdev_|input_transport|raw_latency|raw_dispatch'
# Separate ASan/UBSan build: -DSDLOP_SANITIZERS=ON
# Standalone ring TSan example, no input devices needed:
cc -D_GNU_SOURCE -std=c11 -O1 -g -UNDEBUG -fsanitize=thread \
  -fno-omit-frame-pointer -no-pie -Isrc tests/test_ring.c -pthread -o /tmp/test_ring_tsan
/tmp/test_ring_tsan
```

The callback regression is a CTest, not a latency acceptance benchmark. It
intentionally blocks a test callback to hold an old reader version; real user
callbacks should remain fast, nonblocking and thread-safe. No change to global,
worker-thread raw delivery, no new dependencies and no changes to motion/report
aggregation, focus/VT recovery integration or hotplug scope.
