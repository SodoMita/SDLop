# Raw drop statistics and SDL key/button recovery

2026-11-21. Follow-up to [worker wakeup/clock selection](input-worker-wakeup-clock.md).
Implements public per-ring drop reporting and recovery of the input states
currently supported by the evdev SDL adapter. No new dependencies.

The optimized/sanitizer results below apply to the revision and queue policy
recorded when they were collected. They are historical and do not validate the
later state-first queue-pressure change. A focused native headless build and
queue-pressure test run for the current worktree is recorded in
[Current targeted validation](#current-targeted-validation). The callback
performance gate remains uncleared. This is not a blanket performance
improvement or a claim to recover missing event history.

## Verified gaps

Before these edits:
- `src/internal/sdlop_evdev.c:worker_main` ignored ring-push results. Each ring
  already counted drops, but `sdlop_ring.h:SDLOP_RingDropped` read a plain
  producer-owned integer. Exposing that directly to other threads would race.
- `src/internal/sdlop_evdev.c:pump_event` handled SYN_REPORT only for wheel
  flushing. SYN_DROPPED had no discard/recovery behavior and no EVIOCGKEY query.
- State was global SDL state, without a per-device key/button cache suitable
  for snapshot diffing. High-resolution wheel accumulation was also global.
- `pump_event` has no ABS or multitouch translation. Consequently EVIOCGABS and
  EVIOCGMTSLOTS recovery are not added for unsupported SDL input paths.

## Public counters

`include/SDL3/SDLop.h` adds:

```c
SDLop_RawInputStats stats;
if (SDLop_GetRawInputStats(&stats)) {
    /* v1 identifiers plus Uint64 counters:
       stats.version, stats.size
       stats.events_seen
       stats.sdl_ring_dropped
       stats.poll_ring_dropped */
}
```

The query is non-consuming and safe from any thread, including the raw
callback, provided init/quit is not concurrent. NULL is rejected with an SDL
error. Counters are independently sampled, not a transactional snapshot.
`events_seen` follows the existing event counter (including its existing
MSC_SCAN exclusion). Drop snapshots are published at completed worker batch
boundaries, **only when changed**, so they can lag live event dispatch during a
batch. They report rejected native records, not discarded SDL recovery records,
kernel losses, event-filter rejection, or synthesized transitions.

Final counters survive quit until raw input is initialized again. Initially
uninitialized counters are zero; web counters are always zero. The public stats
structure is frozen as v1 and reports its version and size; future incompatible
layouts must use a new API rather than changing this struct. The web stub,
shared-library version script, C++ linkage in the extension header, and C++
link regression were updated along with the declaration. Shared export check:
737 SDL functions + 7 SDLop extensions declared and exported.

The ring's producer-only counters are not made contended per-event atomics.
Separate cache lines isolate public drop snapshots from `event_count`, recovery
requests and main-thread recovery bookkeeping. Initial unconditional stores
caused avoidable cache invalidations; that implementation and its less favorable
timings remain in the first-attempt log.

## Recovery protocol

### SYN_DROPPED

`SDLOP_RawInputPump` marks the affected device as uncertain and discards its SDL
input through the next SYN_REPORT. It also clears that device's partial wheel
frame and stops uncertain autorepeat. Other devices remain usable.

After the report, the SDL thread requests a snapshot through a per-device bit
in an atomic request mask and wakes the worker on its existing control eventfd.
The control channel now distinguishes requests from shutdown. No new input is
required to service a request or notify the SDL waiter that a snapshot is ready.

The worker drains that fd through EAGAIN using the **same native publication and
callback path**, then queries EVIOCGKEY. The subsequent audit bounds each service
pass to 32 reads/device; if that budget is exhausted before EAGAIN, the query is
not attempted and recovery retries later (see [audit](input-audit-sdl3.md)). On
success it stores an immutable snapshot plus the SDL ring's publication-head boundary and the current drop count, and publishes
readiness with release ordering. The SDL thread acquires readiness, discards
uncertain device records preceding that boundary, then diffs cached state and
synthesizes necessary key/button transitions through the normal focus-gated SDL
adapter. No published SPSC record is modified.

The main-thread recovery loop acquires the ring head **before** checking snapshot
readiness and does not pop beyond that head if it was empty. This ensures a
post-snapshot publication cannot be consumed as uncertain merely because an
earlier readiness check missed the snapshot. Each device permits only one
outstanding snapshot; a new request cannot overwrite one still being applied.

### SDL raw-ring overflow

A changed SDL-ring drop count makes every device uncertain: rejected records
have no identity in the ring, so guessing a single affected device is unsafe.
The same snapshot protocol recovers current supported state. Snapshot generation
and drop-epoch checks reject stale recovery attempts if another loss occurs.
Overflow of **only the raw polling ring does not trigger SDL recovery**.

### Applying state safely

- Snapshot diffs apply the complete supported state regardless of SDL queue
  headroom. Keyboard delivery preflights three slots for a possible
  keymap/key/text sequence (so key-only input may also be dropped with one or
  two slots free); standalone notifications use the remaining capacity.
  Anything that cannot fit is dropped, but state does not stall and dropped
  transitions are not replayed.
- A failed query is **not** treated as an all-up bitmap. The device remains
  uncertain and retries at most every 100ms while SDL is pumped. EINTR retries.
- Per-device physical key/button caches preserve another device's held key
  when one keyboard is recovered. Duplicate/stale transitions are suppressed.
  A prior focus-filtered press does not hide another keyboard's fresh focused
  press. The existing focus gates still determine delivery.
- High-resolution wheel accumulation is per device and reset on damage/init.
- Recovery updates VT modifier bookkeeping but does not execute a synthetic
  Ctrl+Alt+Fn VT switch. Synthetic SDL transitions use recovery-time timestamps,
  not fabricated timestamps for the lost physical edges.

`drain_device_events` contains the existing per-event publication/callback loop,
shared between ordinary reads and the explicitly requested slow path. Raw
callbacks and raw polling receive the same native records as before, including
SYN_DROPPED and damaged packets, subject to existing polling-ring capacity.
Synthesized recovery transitions go only to SDL. Normal worker events gain no
per-event locks, allocation, state-query syscall, or extra atomic counter update.
Recovery queries do run on the worker, so exceptional recovery work can delay
callbacks on other devices; this is not claimed to be free.

## Fault tests: before and after

`test_evdev_syn_dropped` links unchanged against the preserved wakeup/clock
baseline and against the modified library. A delivered left-button down is
followed by SYN_DROPPED, invalid motion, and SYN_REPORT. Mocked kernel state says
the button is now up, but no native release was delivered.

| Result | Before | After |
| --- | ---: | ---: |
| Stuck button | yes | no |
| Damaged SDL motion events | 1 | 0 |
| Recovery release events | 0 | 1 |
| EVIOCGKEY queries | 0 | 1 |
| Native raw / callbacks | 4 / 4 | 4 / 4 |

This is a **rootless fault injection into the real worker**, not a kernel evdev
or hardware test. [Fault log](benchmarks/2026-11-21-resync-faults.txt).

`test_evdev_recovery` adds two synthetic fds and mocked kernel state. It tests:
- discard through SYN_REPORT, missing key-down synthesis, missing key/button-up
  synthesis, no damaged motion/key leakage, partial-wheel reset;
- unchanged native callback/poll stream, including native damage markers;
- per-device held-key union and focus-filtered/fresh-press interaction;
- interrupted query retry, failed query preserving state, subsequent retry
  without new native input;
- full SDL queue during snapshot recovery: key/button state is updated from the
  complete snapshot even with no event capacity; notifications that cannot fit
  are dropped and are not replayed after the application drains the queue;
- 40,004 records without consumption: exactly **7,236 rejects per 32,768-slot
  ring**, then recovery of releases that were actually lost;
- independent 40,000-record poll-only / SDL-only overflow: exactly 7,232 drops
  in the selected ring, none in the other; no SDL recovery for poll-only loss;
- stats queries from callbacks, NULL rejection, exact totals, retention over quit
  and reset on worker reinitialization.

The archived optimized and ASan+UBSan runs predate the current state-first
queue-pressure contract. They remain historical snapshots, not evidence for
this later change. See the [evidence index](benchmarks/README.md); rerun the
current test targets before making a current-build validation claim.

## Sustained before/after measurements

Preserved baseline: preceding wakeup/clock implementation, not the original
small-ring build. Same harness and host: Linux6.1.158+, two CPUs, GCC12.2 -O2 -g,
real 37-TU dummy/fbdev library. Three paired repetitions/profile, alternating
before/after and after/before pair order, both poll and wait modes:
- 100,000 reports/mode at 16k reports/s, two axes.
- 1,000,000 reports/mode at 256k reports/s, burst 16, two axes.
- 1,000,000 reports/mode at saturation, burst 64, two axes.

**All final before/after trials delivered every callback, raw record and SDL
motion**, and all paced trials met the mean offered-rate gate. Per build:
12.6M reports, 37.8M native records, 25.2M SDL motions. These no-fault trials
exercise normal operation, not repeated recovery cost. Pipe scheduling and
backpressure are included; physical device/kernel timing is not.

Medians of per-trial callback percentiles, microseconds:

| Load | Mode | p50 before → after | p95 before → after |
| --- | --- | ---: | ---: |
| 16k | poll | 3.4 → 3.5 | 5.1 → 5.2 |
| 16k | wait | 24.9 → 26.8 | 39.8 → 41.9 |
| 256k burst 16 | poll | 59.1 → 59.5 | 632.2 → 878.9 |
| 256k burst 16 | wait | 3.2 → 3.1 | 27.0 → 22.8 |
| Saturation | poll | 588.1 → 587.2 | 1126.4 → 968.5 |
| Saturation | wait | 1.8 → 2.0 | 55.2 → 64.1 |

**The ~5% p50/p95 callback acceptance gate is not cleared.** Examples:
16k/wait p50 +7.6%, p95 +5.3%; 256k/poll p95 +39.0%. Distributions are noisy
(e.g. one baseline 256k/poll p50 was 2.6ms), but that is not grounds to erase
unfavorable outcomes or assert no slowdown. The final implementation avoids
obvious counter false sharing, but further profiling/controlled timing is needed
before performance acceptance. Saturation rates are achieved throughput, not
matched offered load between builds. No isolated callback instruction-cost claim.

Logs include every trial:
- [First attempt](benchmarks/2026-11-21-resync-load-first-attempt.txt)
- [Final comparison](benchmarks/2026-11-21-resync-load.txt)

## Current targeted validation

For the current worktree, a temporary headless native build was compiled from
repository sources because this environment lacks `pkg-config` and the required
platform development packages for the normal CMake build. It used the dummy video
driver, temporary xkbcommon declarations to compile the dynamic xkb integration,
and no-op GL/Vulkan offload link stubs; xkb runtime/layout behavior and graphics
drivers were not exercised. The real current evdev worker was used by the
pipe-backed input tests. In that focused run, these test targets passed:

- `test_events`, including overdue-repeat behavior, whole keyboard notification
  suppression with two queue slots left (a watcher injects a TEXT_INPUT
  independently of XKB; its nested watch is observed before queue suppression),
  and final-slot standalone mouse input;
- `test_evdev_backpressure` and `test_evdev_recovery`, including full-queue state
  updates, exact raw/drop accounting, snapshot recovery, and no replay (the
  backpressure test passed three runs and recovery passed three runs);
- `test_cxx_link` C++ compile/link/run, plus `test_web` against both native and
  web-keyboard headless variants. A direct test of the web-keyboard variant also
  checked no text before start, text while active, and no text after stop.

After that run, `test_events` gained explicit counters/assertions confirming that
both the key-down watch and its nested watcher-injected TEXT_INPUT watch run
before queue suppression. The revised test passed strict syntax checking, but its
runtime assertion has not been rerun and is not current runtime evidence.

Strict-warning syntax-only checks passed for the changed native input sources and
focused tests, the web keyboard implementation, and the C++ test; native
`SDL_keyboard.c` used the same temporary xkb declarations. The browser-only
`test_web.c` path also syntax-checked with a small Emscripten-macro shim. `git
diff --check` also passed after these edits. This is a focused manual build,
**not** a full CMake/CTest, Emscripten/browser, platform-driver, XKB-runtime, or
graphics build. No current performance benchmark was run, so the callback
acceptance gate remains uncleared.

## Historical validation and limits

The following results belong to the earlier state-recovery revision and its
queue policy; they are retained as dated evidence, not as validation of the
current queue-pressure edits:

- Eighteen general regression executables were rebuilt and passed, including the
  C++ linkage test, native web shim, ring stress, event/repeat tests, no-XKB
  keycodes, dummy window API and real RAM/PPM framebuffer test. Alongside damage,
  recovery, backpressure and five worker cases, this was 26 optimized test runs.
- ASan+UBSan real-library load check: 100,000 reports/mode at 64kHz, complete
  native/SDL delivery and no sanitizer diagnostics. Sanitized timings are not
  used for acceptance.
- Shared export check passed (737 SDL + 7 SDLop functions). Web stub was
  syntax-checked, not run under Emscripten/browser.
- CMake/CTest, X11/Wayland focus integration, real VT switches, runtime
  XKB/layout coverage and full graphics configuration were unavailable in that
  validation environment.
- That revision added 8,896 bytes of static raw-device/statistics state on
  x86-64 (`nm -S`), with no new event-storage allocation.
- Kernel uinput was disabled on that host, as verified then. Mocked EVIOCGKEY
  tests do not establish correctness under real kernel buffer overflow.

Recovery restores **current supported state**, not a sequence of missed edges,
clicks, typed characters, or relative motion. It deliberately discards uncertain
queued SDL input and may synthesize a down for a currently held key; that uses
normal SDL key delivery, including enabled text/repeat behavior. Query failures
preserve the prior state and retry; terminal fd removal releases that device's
held state and can trigger all-or-nothing window-system fallback. The worker does
not discover/reopen replacement devices, so newly added devices require video
reinitialization. A continuously readable fd or slow callback can delay the
worker's drain/query boundary. Uncertain autorepeat is stopped conservatively;
repeat on an unchanged held key need not resume until a fresh press.

Still separate: pending-REL overflow preservation, SDL frame aggregation,
hot-add/reopen with stable device identity, wiring snapshot recovery to focus/VT
acquisition, and ABS/multitouch support. Do not describe this as completion of
every Phase 1 or focus-lifecycle suggestion. The callback timing gate should be
addressed before adding further hot-path performance claims.
