# Linux raw-input ThreadSanitizer audit

Date: 2026-11-21

Scope: `src/internal/sdlop_evdev.c`, the SPSC raw-event ring, and worker/callback tests. No Windows sources or tests were touched.

These are results from the earlier worker/callback snapshot used when the TSan
option was introduced. Later state-first queue-pressure, recovery and lifecycle
edits in the current worktree were not run under TSan; these logs must not be
read as validation of those later changes.

## CMake option

A dedicated `SDLOP_TSAN` option now adds `-fsanitize=thread -fno-omit-frame-pointer` to C compilation and `-fsanitize=thread` to executable/shared link flags. It is mutually exclusive with `SDLOP_SANITIZERS` (ASan/UBSan).

Reproduce on a machine with the project's normal Linux build prerequisites:

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSDLOP_TSAN=ON
cmake --build build-tsan -j3
TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 history_size=7" \
  SDLOP_TEST_STRICT=1 ctest --test-dir build-tsan -j1 --timeout 120 --output-on-failure
```

Configuration with both sanitizer options enabled was checked and fails with the intended mutually-exclusive-options diagnostic. `tools/options_check.sh .` passes.

## Run and results

GCC 12.2's TSan runtime smoke test passed. The checked-in `tests/test_ring.c`, `tests/test_evdev_worker.c`, and `tests/test_evdev_callbacks.c` were compiled with TSan and strict warnings (`-Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes`) and run against the production `src/internal/sdlop_evdev.c` and the process-local pipe transport. A temporary minimal SDL-core shim under ignored `build-validation/tsan/` supplied only the event/API services needed by those existing test sources; it does not replace or instrument other SDLop modules.

With `TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 history_size=7"`:

- `test_ring`: PASS; 4,000,000 single pops plus 4,000,000 mixed/batch operations, wrap/overflow/layout checks. No TSan report.
- `test_evdev_worker`: all six modes (`wake`, `busy`, `monotonic`, `negotiated-authoritative`, `fallback-monotonic`, `fallback-realtime`) passed in three rounds (18 process runs). The wake mode forces 100 consumer-drains-mid-batch interleavings per run: 300/300 wake checks passed. No TSan report.
- `test_evdev_callbacks`: five runs passed, including 500 callback-list register/unregister churn iterations per run, a held in-flight reader, allocation-failure cases, and the added callback-self-unregister regression. No TSan report.
- No TSan suppressions were added; there was no raw TSan warning output to suppress or classify.
- The directly compiled production worker and test sources emitted no warnings with the strict warning set above.

This is a focused, rootless worker/ring validation, **not** a full SDLop build or full CTest result. In this environment, a normal CMake configure stops at `find_package(PkgConfig)` because `pkg-config` is absent. The machine also lacks Wayland/XKB/EGL/GBM/DRM development headers, `wayland-scanner`, `xvfb-run`, and `/dev/uinput`; consequently the actual-uinput, X11, and full SDL event/thread/core integration tests were not run. A configure using temporary fake package metadata was used only to inspect generated flags (`-fsanitize=thread` is present); it was not used for a build. The API export/subset scripts need a built shared library (and, for subset checking, an SDL3 reference library), so those checks were not run here. No public header or symbol-list changes were made for TSan.

## Classification of the suspected shared accesses

No data races were reported in the runs above. The following source audit classifies the specific prompt leads; these are classifications, not suppressed TSan reports.

| Suspected access | Classification and evidence |
|---|---|
| `sdlop.vt_signals[]` | **Safe by lifecycle.** The KMS driver sets these during video initialization before `SDLOP_RawInputInit()` starts the worker. Video shutdown calls `SDLOP_RawInputQuit()` and joins the worker before the driver's `Quit` clears them. They are immutable while the worker runs. |
| `sdlop.wake_fd` | **Safe by lifecycle.** It is established on the main thread before worker creation (or created by raw init before `pthread_create`) and is not closed until after raw-input shutdown joins the worker. |
| `sdlop.video` | **Not a worker access.** `video_has_window_system_input()` reads it on the initializing thread to set `raw.winsys` before worker creation. The worker uses that immutable raw-input mode instead. |
| Window/focus/grab fields, `mouse_buttons`, `keystate`, and mouse coordinates | **Main-thread-owned.** Translation and focus filtering occur in `SDLOP_RawInputPump()` (`pump_event()` / `pump_key()`), called from the SDL pump thread. `worker_main()` only reads devices, publishes native records, updates atomic counters/snapshots, notifies the eventfd, and invokes raw callbacks. |
| Repeat fields and `SDLOP_RawInputSetRepeatInfo()` | **Not a public any-thread API and not worker-owned.** The setter is internal (not declared in `include/SDL3/SDLop.h`) and its current X11/Wayland call sites run while dispatching window-system events on the main thread. Repeat processing and recovery writes are also on the pump thread. Calling this internal helper concurrently from an arbitrary application thread would need a separate contract/synchronization change; no such caller exists in the audited source. |
| Callback list / callback self-unregister | **Synchronized and lifetime-safe.** The published pointer is now declared as an actual `_Atomic(SDLOP_CallbackSet *)`; writers publish immutable snapshots with release stores and the worker loads with acquire semantics. The writer mutex serializes replacements. Previous snapshots remain allocated until after worker join, and dispatch holds no writer lock while invoking application code. The callback test now exercises unregistering itself on the worker. |
| SPSC rings, event counters, resync requests, and device snapshots | **Synchronized by ownership/atomics.** Each ring has one producer and one consumer with acquire/release head/tail publication. Shared counters and requests are atomic. Snapshot payload is published by `snapshot_ready` release and read after acquire; disconnection is an atomic release/acquire flag. Device descriptors/capabilities are fixed before thread start and are closed only after join. |

The only production adjustment found useful in this review was making the callback pointer's atomic type explicit instead of accessing a plain pointer through an `_Atomic` cast; this preserves the existing lock-free dispatch design and adds no per-event lock. No other race fix or blanket suppression was justified by the evidence.
