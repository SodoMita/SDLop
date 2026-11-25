# Sustained rootless input-load measurements

The original 2,000-report runs were smoke tests, not evidence of sustained
high-rate input handling. The benchmark now supports much longer, rate-controlled
trials. **Larger runs exposed polling/SDL losses that the smoke tests missed.**

This report records the **pre-fix** worker and consumers, using an ordinary
pipe in place of evdev. It is not a USB/HID, kernel-input, joystick or VR-device
test. Subsequent production changes and before/after results are documented in
[input loss reduction](input-loss-reduction.md); the numbers below remain the
historical baseline, not results from the modified library.

Verified before extending the harness:

- `bench/bench_raw_latency.c:produce` used a fixed `EVENTS=2000` limit and relative
  200-microsecond sleeps, with a fixed batch size for stress.
- `bench/bench_raw_latency.c:main` had no count, rate, axis or burst controls.
- `bench/bench_raw_latency.c:record` / `report` counted REL_X probes in fixed
  arrays, not all native records or achieved injection throughput.

## Workload controls

Both `bench_raw_latency_pipe` (rootless) and `bench_raw_latency` (uinput) accept:

| Option | Meaning | Default / limit |
| --- | --- | --- |
| `--reports N` (alias `--events N`) | Number of reports / unique REL_X latency probes **per trial** | 160,000; 1–4,000,000 |
| `--rate Hz` | Target **reports** per second; 0 means saturation | 16,000; 0–1,000,000 |
| `--burst N` | Reports per scheduled batch | 1; 1–4,096 |
| `--axes 1\|2` | REL_X, optionally REL_Y, followed by SYN_REPORT | 2 |
| `--mode poll\|wait\|all` | Consumer strategy; `all` runs independent poll/wait trials | all |
| `--stress` | Defaults to rate 0, burst 64; explicit rate/burst options win regardless of order | off |
| `--api-overhead` | Separate direct-call microbenchmark; not an input traffic test | off |

A report with two axes contains **three native records** (X, Y, SYN) and produces
two SDL motions in the current adapter. At 16,000 reports/s that is 48,000 native
records/s and 32,000 SDL motions/s. At 64,000 reports/s it is 192,000 native
records/s. The `n=` latency distributions count the unique X probes, not every
native record; separate counters check all native records and SDL motions.

The default runs for approximately 10 seconds per mode. One million reports at
16 kHz run for 62.5 seconds per mode, or 125 seconds for `--mode all`, excluding
setup, final draining and percentile sorting.

```sh
# Each command runs both consumer strategies unless --mode is supplied.
./build/bench_raw_latency_pipe --events 1000000 --rate 16000 --axes 2
./build/bench_raw_latency_pipe --events 1000000 --rate 64000 --axes 2
./build/bench_raw_latency_pipe --events 1000000 --rate 256000 --burst 16 --axes 2
./build/bench_raw_latency_pipe --events 1000000 --stress --axes 2
```

Higher rates approximate aggregate traffic on **one synthetic evdev fd**. They
do not emulate simultaneous physical mouse, keyboard, joystick and headset
sources, multiple-fd epoll overhead, or their device-specific SDL translation.
A pass is not evidence that joystick/VR APIs are implemented or sufficiently
fast. Additional device/event mixes would be separate coverage.

## Pacing and accounting

- Samples are dynamically allocated and prefaulted **before worker activity**.
  At four million reports, arrays use approximately 172 MB (43 bytes/report).
  Collection uses no per-event allocation or new library worker syscalls.
  Producer/consumer accounting metadata is cache-line separated.
- Unique sequence values remain exactly representable in SDL's float `xrel`.
  The discovery marker is outside the supported sequence range. Tests exercise
  indexing beyond one million, including the previous marker value.
- Pacing uses absolute CLOCK_MONOTONIC deadlines, not an accumulated relative
  sleep. Batches that wake late catch up; they are not silently skipped.
- Output includes actual injection duration, achieved reports/s and native
  records/s, batches more than one batch interval late, and maximum lateness.
- A requested mean rate missed by more than 5% fails with `UNDER-RATE` even if
  every event arrived. This prevents a slower producer from making consumer
  results look artificially good. Rate 0 has no requested-rate constraint. This
  offered-load check is separate from the plan's callback p50/p95 regression gate.
- Missing probes, missing native X/Y/SYN records, missing SDL motions, invalid
  timestamps or producer failure make the trial fail with `INCOMPLETE`.
  Losing only SYN or Y cannot pass just because every X probe arrived.
- `--mode all` still runs wait after a lossy poll trial, preserving failure in
  the overall exit status. A successful later trial cannot hide an earlier loss.
- Callback-owned source timestamps are paired with main-thread receipt times
  only after worker join, avoiding the existing SDL timestamp-origin mismatch.

**Important:** mean offered rate is not uniform inter-arrival timing. Linux
scheduling and sleep granularity cause jitter, especially at 62.5 microseconds
per report. The 16 kHz runs below saw maximum scheduling lateness of roughly
24–28 ms, then caught up. Source-to-consumer latency starts at the actual
userspace write timestamp, not the desired scheduling deadline, so it excludes
that producer scheduling delay. Pacing diagnostics must be read with latency
statistics. These are not hard-real-time or hardware-polling guarantees.

## Results — 2026-11-21

Real, unchanged SDLop dummy/fbdev library: 37 translation units, GCC 12.2.0,
`-O2 -g`, Linux 6.1.158+, two visible CPUs. Ran as uid 1001 without sudo or input
devices. The headless build method and limitations are described in the
[baseline report](input-latency-baseline.md#validation).

One repetition of each profile, **one million reports per mode**, for eight
million reports / **24 million injected native records in total**. These are
observations in a shared sandbox, not certified throughput limits. Every
callback trial observed all three million native records and all million X
probes. The table records losses separately from latency; lossy distributions
are not usable as a successful latency gate.

| Requested reports/s | Mode | Achieved reports/s | Duration | Raw native records missing / 3M | SDL motions missing / 2M | Complete? |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 16,000 | poll | 16,000.0 | 62.500 s | 0 | 0 | Yes |
| 16,000 | wait | 16,000.0 | 62.500 s | 0 | 0 | Yes |
| 64,000 | poll | 63,999.7 | 15.625 s | 59 | 218 | **No** |
| 64,000 | wait | 63,999.9 | 15.625 s | 0 | 0 | Yes |
| 256,000, burst 16 | poll | 255,994.2 | 3.906 s | 13,359 | 12,362 | **No** |
| 256,000, burst 16 | wait | 255,996.2 | 3.906 s | 0 | 0 | Yes |
| Saturation | poll | 418,975.3 | 2.387 s | 526,417 | 565,999 | **No** |
| Saturation | wait | 561,597.4 | 1.781 s | 264,371 | 301,870 | **No** |

The 64 kHz, 256 kHz and saturation commands exited 1, as intended, because at
least one mode was incomplete. The complete 16 kHz command exited 0.

Latency of **complete** trials, microseconds, per one million X probes:

| Trial | Consumer | p50 | p95 | p99 | Max |
| --- | --- | ---: | ---: | ---: | ---: |
| 16 kHz/poll | callback | 3.4 | 4.5 | 5.4 | 1,551.0 |
| 16 kHz/poll | raw poll | 3.6 | 5.0 | 6.2 | 9,513.3 |
| 16 kHz/poll | SDL poll | 3.7 | 5.0 | 6.3 | 9,537.4 |
| 16 kHz/wait | callback | 23.5 | 33.6 | 41.4 | 18,578.8 |
| 16 kHz/wait | raw poll | 40.1 | 54.3 | 81.9 | 18,602.6 |
| 16 kHz/wait | SDL wait | 40.2 | 54.1 | 82.3 | 18,602.5 |
| 64 kHz/wait | callback | 12.7 | 29.2 | 39.3 | 1,106.7 |
| 64 kHz/wait | raw poll | 18.7 | 51.9 | 80.7 | 1,242.7 |
| 64 kHz/wait | SDL wait | 19.2 | 52.1 | 83.8 | 1,241.8 |
| 256 kHz/wait | callback | 1.9 | 14.2 | 26.1 | 535.0 |
| 256 kHz/wait | raw poll | 39.7 | 84.4 | 106.6 | 649.1 |
| 256 kHz/wait | SDL wait | 43.0 | 86.1 | 108.6 | 648.5 |

[Full commands, exit statuses, pacing diagnostics, all latency distributions and failures](benchmarks/2026-11-21-sustained-rootless.txt).

There is no basis here to say wait is generally faster or more reliable than
poll: runs were sequential, scheduling jitter differs, the producer/worker/main
share two CPUs, and the input is synthetic. Likewise, losing no callbacks does
not imply no ring or SDL event-queue loss. Locating the exact loss stages and
implementing overflow/resynchronization handling remain follow-up items.

## Validation of the larger harness

- Expanded accounting tests pass normally and with ASan+UBSan: dynamic buffer
  indexing beyond one million, marker separation, float sequence precision,
  integer/overflow/bounds parsing, absolute-deadline arithmetic, target-rate
  acceptance/rejection, duplicate/invalid timestamps, post-join correlation,
  and native-record-only/SDL-axis loss detection.
- The real 37-unit library and harness pass ASan+UBSan with 20,000 reports/mode
  at 16 kHz, including all 60,000 native records and 40,000 SDL motions per mode.
  API-overhead mode also passes. Sanitized runs are correctness checks, not the
  measurements in the results table.
- One-axis and partial final burst coverage: 4,099 reports at 16 kHz, burst 7,
  both modes, all 8,198 native records and 4,099 SDL motions delivered per mode.
- CLI validation rejects missing, malformed, negative, overflowing and out-of-
  range option values. Explicit rate/burst settings override `--stress` in
  either argument order.
- Existing library input and public API source files remain unchanged.

The original short-run outputs are retained for provenance but should be read
only as smoke-test and API-cost data, not sustained-input evidence. Neither
these longer rootless runs nor their loss-free cases replace the plan's eventual
evdev/hardware before-and-after regression gate.
