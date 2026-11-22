# Input report follow-up

Baseline: `2dda7f1`; correctness changes only, no new runtime dependencies.
The Luanti report's latency figures are not a reason to change dispatch timing.

## Verified findings and fixes

* `src/video/wayland/sdlop_wayland.c:pointer_axis_discrete` discarded detents;
  `pointer_axis_value120` added to the continuous fallback and used the wrong
  vertical sign. Each frame now selects value120, otherwise discrete, otherwise
  continuous **per axis**, never adds alternative representations, and inverts
  vertical consistently. Continuous-only input retains its distance/10 fallback:
  a compositor sending only distance cannot communicate a detent count.
  Pre-v5 pointers flush immediately (they have no frame event). Focus/device
  transitions discard partial wheel frames.
* `src/internal/sdlop_evdev.c:open_input_devices` accepted only REL_X+REL_Y
  pointers. It now also accepts ABS_X/Y with valid queried ranges and a mouse
  button or pen tool. Joysticks, direct touchscreens, finger touchpads and
  multitouch devices do not accidentally satisfy that classification.
  `pump_absolute` normalizes to `[0,w-1]`/`[0,h-1]`, clamps out-of-range samples,
  combines axes at SYN_REPORT, updates position before pointer buttons, and
  derives relative-mode deltas from normalized samples. Pen BTN_TOUCH maps to
  the left button, including union state with BTN_LEFT. Lost-sync recovery
  queries both absolute axes with the existing key snapshot, with no invented
  relative history. Focus/grab/console ownership gates remain in force.
* The text-repeat report is **not reproduced on this baseline**. In
  `src/SDL_keyboard.c:SDLOP_SendKeyboardKey`, repeats already translate and send
  text. In `src/internal/sdlop_evdev.c:pump_event`, window-system raw mode already
  ignores kernel value=2 and uses software repeats, so the report's combined
  software+kernel count also differs from this source. A new real-worker test
  verifies one TEXT_INPUT per repeat, rejects kernel duplicates, and verifies
  release stops repeats. It passes against both baseline and patched libraries
  with real libxkbcommon and US layout data. No speculative keyboard change was
  made. Reproducing the Luanti divergence still requires its exact SDLop build,
  keymap/repeat settings and hook log (including text-input/focus transitions).

Headless sway keyboard-enter/focus and sway pointer acceleration remain outside
these fixes; no artificial focus or compositor rerouting was introduced.
Single-contact pointer translation is not a tablet pressure/proximity or touch
API implementation. Device hotplug policy is unchanged.

## Verification

Local GCC 12.2, C11, `-O2`, Linux x86-64; tests use rootless native input_event
pipes with the actual worker, rings, raw callbacks and SDL adapter. Existing
runtime-dlopen dependency libxkbcommon 1.5.0 was built in an ignored validation
folder; upstream xkeyboard-config and libX11 Compose data supplied the missing
host data. No dependency was added to the project.

* Optimized and ASan+UBSan/leak builds: ring (8 million records, 30 layouts),
  scancodes, events, layouts/compose, core, keycodes, surfaces, transport errors,
  wheel frame arithmetic, absolute pointers, exact native-drain boundaries,
  all six worker wake/clock modes, callback lifetime/OOM/churn, backpressure,
  SYN_DROPPED and recovery tests pass.
* New absolute test covers discovery rejection, mouse and pen translation,
  frame batching, clamping, button coordinates, unfocused input suppression,
  relative deltas, snapshot restoration and unfiltered raw ABS callbacks.
* `test_pointer_lock`/mini compositor now exchange and assert real paired
  axis/value120 frames and smooth fallback. Driver and mini compositor compile
  against scanner-generated Wayland 1.23.1/protocols 1.38 headers locally.
  Live mini-compositor execution is delegated to normal Linux CI (the local
  image has no Wayland runtime). Unit arithmetic also covers discrete detents,
  reversed signs, fractions and per-frame reset.
* Sanitizer run uses `allocator_may_return_null=1` for the pre-existing surface
  oversized-allocation test, and preserves fortification. The first attempt
  without that setting aborted on that deliberate huge allocation, not on an
  input error.

## Before/after rootless timings

Raw logs: `docs/benchmarks/2026-11-21-input-fixes/`. Three alternating pairs,
256,000 reports per poll/wait mode per binary, target 16,000 reports/s,
2 axes + SYN_REPORT = 768,000 native callbacks per case, about 16 seconds/case.
Every native callback/raw-poll/SDL count matched; no missing or invalid probes.
These measure **userspace pipe stamp to callback**, not hardware/kernel latency.

Median of the three per-run percentiles, microseconds:

| mode | callback p50 before -> after | callback p95 before -> after |
|---|---:|---:|
| poll | 2.1 -> 2.1 | 3.0 -> 3.1 (+3.3%) |
| wait | 20.8 -> 19.8 | 31.4 -> 30.0 (-4.5%) |

This local median check is within the requested approximately 5% callback gate;
individual runs are noisy (patched poll p95 3.1/4.2/2.9 us). The native drain's
717-byte machine-code body is byte-identical in the two optimized objects;
only discovery/recovery and main-thread translation were changed. Raw callback
semantics remain global, per-event, on the worker, without new per-event locks,
allocations or syscalls.

One additional 256k saturation case per mode: poll throughput 461,514 -> 500,476
reports/s, wait 569,088 -> 559,577. These are unmatched-rate observations, not a
speedup claim or callback gate. Direct rootless API block-average measurements
(1,000 blocks x 1,000 calls) are retained separately and are not input latency.
The earlier PR-wide performance gate documented in `input-dispatch-optimization.md`
is **not cleared** by this focused check; PR #11 should remain draft.
