/*
  Hardware-independent checks for the raw latency benchmark's accounting.
  Include its implementation to exercise exactly the sample/report code used
  by the executable, without exposing test-only API from the library.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#define main sdlop_latency_benchmark_main
#include "../bench/bench_raw_latency.c"
#undef main

#include <assert.h>

int main(void)
{
    enum { EVENTS = 2000 };
    Samples samples;
    assert(allocate_samples(&samples, EVENTS));

    /* Empty input must not index a percentile or appear to pass the gate. */
    assert(!report("empty", &samples));
    record(&samples, 0, 10, 20);
    record(&samples, EVENTS + 1, 10, 20);
    assert(samples.count == 0 && samples.invalid == 0);

    /* Duplicate observations cannot hide a missing event. Invalid clocks must
     * not become enormous unsigned latency samples or successful timeouts. */
    record(&samples, 1, 10, 20);
    record(&samples, 1, 10, 30);
    record(&samples, 2, 0, 20);
    record(&samples, 3, 30, 20);
    assert(samples.count == 1 && samples.latency[0] == 10);
    assert(samples.invalid == 2);
    assert(!report("partial", &samples));

    release_samples(&samples);
    assert(allocate_samples(&samples, EVENTS));
    for (int i = 1; i <= EVENTS; i++) {
        record(&samples, i, 1, 1 + (Uint64)(EVENTS - i) * 1000);
    }
    assert(report("complete", &samples));
    assert(samples.latency[0] == 0);
    assert(samples.latency[EVENTS / 2] == 1000000);
    assert(samples.latency[EVENTS * 95 / 100] == 1900000);
    assert(samples.latency[EVENTS * 99 / 100] == 1980000);
    assert(samples.latency[EVENTS - 1] == 1999000);

    /* A full set with an invalid timestamp must still fail. */
    release_samples(&samples);
    assert(allocate_samples(&samples, EVENTS));
    for (int i = 1; i < EVENTS; i++) {
        record(&samples, i, 10, 20);
    }
    record(&samples, EVENTS, 0, 20);
    assert(!report("invalid clock", &samples));

    release_samples(&samples);
    config.reports = EVENTS;
    assert(allocate_trial());
    reset_trial();
    device = 7;
    SDLop_RawEvent ev = { .device = 8, .type = SDLop_EV_REL, .code = REL_X,
                          .value = 1, .timestamp_ns = now_ns() };
    on_raw(&ev, NULL);
    assert(callbacks.count == 0);
    ev.device = device;
    ev.code = REL_Y;
    on_raw(&ev, NULL);
    assert(callbacks.count == 0);
    ev.code = REL_X;
    ev.value = DISCOVERY_MARKER;
    on_raw(&ev, NULL);
    assert(callbacks.count == 0);
    ev.value = 1;
    on_raw(&ev, NULL);
    assert(callbacks.count == 1);
    on_raw(&ev, NULL);
    assert(callbacks.count == 1);

    /* Correlate by sequence after worker join, independent of the SDL event
     * timestamp's origin. Raw source and receipt use CLOCK_MONOTONIC. */
    SDL_Event motion;
    memset(&motion, 0, sizeof(motion));
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.xrel = 1;
    motion.motion.timestamp = (Uint64)-1; /* deliberately incompatible clock */
    collect_sdl(&motion);
    assert(sdl_received == 1 && sdl_events.count == 0);
    collect_sdl(&motion);
    assert(sdl_received == 1);
    motion.motion.xrel = 2.5f;
    collect_sdl(&motion);
    assert(sdl_received == 1);
    motion.motion.xrel = 2; /* received without a matching source callback */
    collect_sdl(&motion);
    assert(sdl_received == 2);
    motion.motion.xrel = 3;
    motion.motion.yrel = 1;
    collect_sdl(&motion);
    assert(!sdl_receipts[2]);
    finalize_sdl_samples();
    assert(sdl_events.count == 1 && sdl_events.invalid == 1);
    assert(sdl_events.latency[0] == sdl_receipts[0] - source_timestamps[0]);
    finalize_sdl_samples(); /* duplicate finalization cannot add samples */
    assert(sdl_events.count == 1 && sdl_events.invalid == 1);

    /* Missing only an unmeasured SYN/axis record must fail even if every
     * REL_X latency probe arrived. */
    native_callbacks = native_polled = (Uint64)config.reports * (config.axes + 1);
    sdl_motions = (Uint64)config.reports * config.axes;
    assert(native_delivery_complete());
    native_polled--;
    assert(!native_delivery_complete());
    native_polled++;
    sdl_motions--;
    assert(!native_delivery_complete());
    sdl_motions++;
    native_callbacks--;
    assert(!native_delivery_complete());
    release_trial();

    /* Exercise dynamic indexing past the old fixed cap and old discovery
     * marker. The new marker must never collide with a valid sequence. */
    assert(allocate_samples(&samples, 1000001));
    record(&samples, 1000001, 10, 20);
    record(&samples, DISCOVERY_MARKER, 10, 20);
    assert(samples.count == 1 && samples.seen[1000000]);
    release_samples(&samples);
    assert(DISCOVERY_MARKER > MAX_REPORTS);
    assert((int)(float)MAX_REPORTS == MAX_REPORTS);

    int value = -1;
    assert(parse_uint_option("4000000", 1, MAX_REPORTS, &value) && value == MAX_REPORTS);
    assert(parse_uint_option("0", 0, 1000000, &value) && value == 0);
    assert(!parse_uint_option("4000001", 1, MAX_REPORTS, &value));
    assert(!parse_uint_option("0", 1, MAX_REPORTS, &value));
    assert(!parse_uint_option("-1", 0, MAX_REPORTS, &value));
    assert(!parse_uint_option("", 0, MAX_REPORTS, &value));
    assert(!parse_uint_option(" 1", 0, MAX_REPORTS, &value));
    assert(!parse_uint_option("1e6", 0, MAX_REPORTS, &value));
    assert(!parse_uint_option("99999999999999999999999999999", 0, MAX_REPORTS, &value));
    assert(report_deadline(100, 1, 16000) == 62600);
    assert(report_deadline(100, 16000, 16000) == 1000000100ull);
    assert(report_deadline(100, MAX_REPORTS, 1) == 4000000000000100ull);
    assert(meets_target_rate(16000, 1000000000ull, 16000));
    assert(meets_target_rate(15200, 1000000000ull, 16000));
    assert(!meets_target_rate(15199, 1000000000ull, 16000));
    assert(!meets_target_rate(16000, 2000000000ull, 16000));
    assert(meets_target_rate(1, 1000000000ull, 0));
    assert(!meets_target_rate(1, 0, 0));

    /* Even with no device, failed writes must terminate the producer and
     * report zero complete reports, never a plausible sent=expected count. */
    config.reports = 3;
    config.rate = 0;
    config.burst = 3;
    Producer failed = { .synth = NULL };
    atomic_store(&producer_done, false);
    produce(&failed);
    assert(failed.failed && failed.sent == 0 && failed.end >= failed.start);
    assert(atomic_load(&producer_done));

    puts("test_raw_latency_bench: PASS");
    return 0;
}
