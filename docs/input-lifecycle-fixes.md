# Fixes for the follow-up input audit

Code: **bccc4bd**. Baseline: **da605a4** (same production code as 0043738).
Addresses the eight findings in [the audit](input-followup-audit.md); no new
runtime dependencies, ordinary-input compositor rerouting or synthetic focus.

## Changes and positive regressions

| Audit finding | Implementation | Verification |
|---|---|---|
| Terminal raw-device errors | `sdlop_evdev.c:retire_device` removes the fd from epoll on EOF/terminal read error. Worker release-publishes removal and wakes the main thread; fd stays open until join to avoid close/reuse races. Main-thread recovery releases only that device's physical state under the logical union rules. With a full SDL queue, the state release still completes while its notification may be dropped. | `input_lifecycle_unplug`: one ENODEV read, no spin or held A/button. `test_evdev_recovery`: pipe EOF, full SDL queue, surviving keyboard keeps A while removed B/button state releases immediately; no stale notification replay. Raw callbacks/counters contain no synthetic releases. |
| Repeats during external keyboard grabs | The common native repeat generator cancels repeats without focus, during an external grab, when disabled, or when raw console/adapter ownership is lost. X11's external-grab transition cancels immediately as well. | `input_lifecycle_repeat-grab`: real raw A press, modeled X11 grab state, due repeat produces zero key/text events. Existing clock, recovery and repeat-text tests pass. |
| Composition crossing text sessions | Native compose reset is shared with start/stop/clear for the focused text session. Inactive text input no longer feeds compose. Clearing another window does not reset the focused window's composition. Web receives the required internal no-op hook; DOM still owns its composition. | `input_lifecycle_compose`: a real German acute+E control first verifies data, then disabled/start, stop/start and explicit clear all produce fresh `e`, not `é`. Existing layout/dead-key/compose suite passes. |
| Wayland repeat_info semantics | Explicit disabled state represents interval=0; delay=0 is legal. The listener accepts nonnegative values and clamps positive rate to 1000 Hz before conversion. Disabling cancels a running repeat; changing an active timer rebases its deadline. | Actual listener diagnostics and new mini-compositor tests for disabled repeat and rate20/delay0; key/text counts checked. |
| Paired raw wheel representations | Discovery selects high-res support independently for each axis. Legacy detents for a high-res axis are ignored in the SDL adapter, not in native callbacks/polling. | `input_lifecycle_wheel`: +120 plus legacy +1 yields one detent. Also verifies vertical high-res fractions with horizontal legacy scrolling and reversed sign. |
| Partial wheel frames after focus loss | Frame flush rechecks focus/grab and owner. Wayland leave, X11 leave/external pointer grab and window removal clear partial raw pointer frames; changing owners also clears accumulated wheel values. | `input_lifecycle_wheel-focus`: loss before SYN emits nothing; explicit backend-transition reset prevents a later frame from reviving discarded values. |
| Scancode aliases | Main-thread, setup-built alias lists union native key codes by delivered scancode, retaining per-device native state for recovery. Final release uses the original delivered native code so the corresponding xkb press is balanced. | `input_lifecycle_key-alias`: SYSRQ/PRINT keep PRINTSCREEN down until the final alias release, then emit exactly one up. Native raw records remain unchanged. |
| Held Wayland modifiers | After legitimate seat focus-enter, restore only modifier scancodes from the held-key list, with no held action-key replay or repeat. Skip restoration while evdev owns SDL input. | Actual-listener diagnostic and `test_wayland_keyboard`: held Shift restored, held B deliberately not replayed, subsequent A text is uppercase. |

### Source failover and lifecycle limits

On X11/Wayland, if removal loses the keyboard or pointer class, the SDL adapter
releases **all** remaining raw-held state before enabling window-system keyboard
**and** mouse. The release is state-first even under queue pressure; notifications
that cannot fit are dropped, and compositor input remains suppressed until all
raw state is cleared. The worker may keep serving native callbacks/polling on
surviving fds, but no subsequent native records enter the SDL adapter after
fallback.
`SDLop_RawInputAvailable` still reports whether that observer worker is running;
its public documentation now explains the fallback exception to source selection.

This is terminal-device retirement, not a new hot-add manager. Startup-local
indices/fds remain stable until join; new devices require video reinitialization.
Raw focus/VT startup snapshots beyond damage/removal recovery, IME integration,
multitouch translation and physical-device tests remain separate work. No new
kernel latency or live VT validation is claimed.

## Before/after correctness evidence

[Raw diagnostic output](benchmarks/2026-11-21-lifecycle-fixes/followup-before-after.txt)
runs all eight modes against the preserved baseline library/listener source and
the fixed implementation. Every baseline mode reproduces its gap (diagnostic
exit 1); every fixed mode reports no reproduction (exit 0). The wheel fixture
now advertises the high-res capability it emits; it still reproduces against
the baseline. Positive CTest cases assert correct outcomes, not merely the
absence of the old compound diagnostic condition.

Local optimized and fortified ASan+UBSan/leak suites pass: new lifecycle modes,
expanded two-device removal/queue-pressure recovery, ring (8 million records,
30 layouts), scancodes, events, layouts, keycodes, core, surfaces, transport,
ABS pointers, exact native-drain boundaries, six worker clock/wake modes,
callback lifetime/OOM/churn, backpressure and SYN_DROPPED.

Selected TSan tests pass: the expanded recovery/removal suite and all six new
worker/core lifecycle modes. GCC's generic fence-instrumentation limitations
still apply; this is not a weak-memory proof. Existing runtime-dlopen xkbcommon
and layout/Compose data were supplied in the ignored validation directory.
No Emscripten compiler or physical uinput device was used.

All three Linux CI jobs for bccc4bd pass, including the full CTest suite with
**actual mini-compositor held-modifier/repeat tests**, Xvfb/no WM, openbox and
ASan+UBSan:
https://github.com/SodoMita/SDLop/actions/runs/45016344077 .
Local shared-library export gate passes: **737 SDL_* + 7 SDLop_***; internal
helpers remain hidden.

## Rootless before/after timings

Three alternating pairs, 256,000 reports/mode/binary at 16,000 reports/s,
2 axes plus SYN_REPORT (768,000 native callbacks per case, ~16 seconds per
case). GCC12.2 `-O2`, same current benchmark/checked transport, no sanitizers or
other validation workloads running concurrently. Every callback/raw-poll/SDL
count matched; probes had zero missing/invalid samples.

Medians of whole-run callback percentiles, **userspace pipe stamp to worker
callback**, microseconds:

| Mode | p50 baseline -> fixed | p95 baseline -> fixed |
|---|---:|---:|
| poll | 2.1 -> 2.1 | 3.0 -> 3.0 |
| wait | 20.7 -> 20.4 | 32.2 -> 30.0 |

No median callback regression in this local approximately 5% check. Keep
individual-run jitter in view (wait p95 baseline 32.2/27.7/32.3; fixed
31.6/29.1/30.0). No extra per-native-event lock, allocation or syscall was added;
retirement syscalls occur only on the terminal/error path. Main-thread adapter
work and per-service checks are not presented as zero-cost changes.

One additional 256k saturation pair: poll 561,379 -> 560,596 reports/s; wait
411,245 -> 406,661. These are unmatched-rate observations, not speedup evidence
or a matched-rate latency gate. Direct rootless API block-average samples are
retained too; e.g. empty SDL_PollEvent p50 11.3 -> 13.3 ns. Those single-run CPU
samples are not native callback latency and do not establish an API-overhead
improvement.

All raw timing/test evidence:
[`docs/benchmarks/2026-11-21-lifecycle-fixes/`](benchmarks/2026-11-21-lifecycle-fixes/).
The earlier **PR-wide performance gate remains uncleared**; these correctness
fixes and local checks do not supersede the recorded historical regressions.
PR #11 remains draft.
