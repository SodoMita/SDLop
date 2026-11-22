# Benchmark and validation evidence index

This directory preserves command output and raw trial logs for the input
reports. These are historical snapshots, not a benchmark suite or a claim that
the current callback performance gate passes. Read the narrative report for
host, workload, build, and limitations; the exact logs below preserve individual
runs and unfavorable results.

## Narrative reports

- [`input-latency-baseline.md`](../input-latency-baseline.md) — rootless baseline
  methodology, workload definitions, and current gate status.
- [`input-load-sustained.md`](../input-load-sustained.md) — pre-fix sustained
  loss evidence.
- [`input-loss-reduction.md`](../input-loss-reduction.md) — bounded rings,
  queue sizing, pressure behavior, memory cost, and before/after trials.
- [`input-worker-wakeup-clock.md`](../input-worker-wakeup-clock.md) — wakeup
  race and timestamp-source changes.
- [`input-state-recovery.md`](../input-state-recovery.md) — drop counters,
  state-recovery fault tests, and the still-uncleared performance gate.
- [`input-dispatch-optimization.md`](../input-dispatch-optimization.md) —
  dispatch cost experiments and explicit latency/throughput tradeoffs.
- [`input-lifecycle-fixes.md`](../input-lifecycle-fixes.md) — lifecycle,
  repeat, wheel, alias, and compose regressions.

## Raw logs by investigation

- **Initial audit and validation:** `2026-11-21-audit-*`,
  `2026-11-21-input-audit-repros*.txt`, `2026-11-21-rootless.txt`, and
  `2026-11-21-sustained-rootless.txt`.
- **Worker wakeup and clock:** `2026-11-21-worker-before.txt`,
  `2026-11-21-worker-after.txt`, and the `2026-11-21-worker-load-*` trials.
- **Loss reduction / recovery:** `2026-11-21-input-loss-{before,after,latency}.txt`
  and `2026-11-21-resync-{faults,tests,load*}.txt`.
- **Dispatch experiments:** `2026-11-21-opt-*`, `2026-11-21-opt2-*`, and
  `2026-11-21-opt2-rejected.patch`; the rejected experiment is retained for
  auditability, not as production code.
- **Input-fix snapshots:** [`2026-11-21-input-fixes/`](2026-11-21-input-fixes/)
  contains before/after load, API, and optimized/sanitizer test output.
- **Lifecycle follow-up snapshots:**
  [`2026-11-21-lifecycle-fixes/`](2026-11-21-lifecycle-fixes/) contains
  diagnostic, workload, export, and optimized/ASan/TSan logs.

All dates refer to the recorded runs. For the queue-pressure state contract,
repeat behavior, and current regression commands, use the narrative reports and
tests in the source tree rather than inferring current behavior from an older
captured log.
