/*
  SDLop test: SPSC lock-free ring correctness under producer/consumer
  stress across threads.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_ring.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define CAPACITY 1024
#define TOTAL 4000000

static uint64_t storage[CAPACITY];
static SDLOP_Ring ring;

static void *producer(void *arg)
{
    (void)arg;
    for (uint64_t i = 0; i < TOTAL; i++) {
        while (!SDLOP_RingPush(&ring, &i)) {
            /* spin: SPSC contract is that the consumer keeps up */
        }
    }
    return NULL;
}

static void test_batch(void)
{
    uint64_t batch[137]; /* deliberately not a divisor of the capacity */
    SDLOP_RingInit(&ring, storage, CAPACITY, sizeof(uint64_t));
    assert(SDLOP_RingPopBatch(&ring, batch, 137) == 0);
    uint64_t value = 99;
    assert(SDLOP_RingPush(&ring, &value));
    assert(SDLOP_RingPopBatch(&ring, batch, 0) == 0);
    assert(SDLOP_RingCount(&ring) == 1);
    assert(SDLOP_RingPopBatch(&ring, batch, 137) == 1 && batch[0] == 99);

    /* Full wrapped read spanning the end of the backing storage. */
    uint64_t full[CAPACITY + 7];
    for (uint64_t i = 0; i < CAPACITY; i++) {
        assert(SDLOP_RingPush(&ring, &i));
    }
    assert(SDLOP_RingPopBatch(&ring, full, CAPACITY + 7) == CAPACITY);
    for (uint64_t i = 0; i < CAPACITY; i++) { assert(full[i] == i); }
    assert(SDLOP_RingEmpty(&ring));

    /* The same four-million-record concurrent producer, with bulk reads. */
    SDLOP_RingInit(&ring, storage, CAPACITY, sizeof(uint64_t));
    pthread_t t;
    assert(pthread_create(&t, NULL, producer, NULL) == 0);
    uint64_t expected = 0;
    while (expected < TOTAL) {
        uint32_t n;
        if (expected & 1) {
            n = SDLOP_RingPop(&ring, batch) ? 1 : 0;
        } else {
            n = SDLOP_RingPopBatch(&ring, batch, 137);
        }
        for (uint32_t i = 0; i < n; i++) { assert(batch[i] == expected++); }
    }
    pthread_join(t, NULL);
    assert(SDLOP_RingEmpty(&ring));
}

/* Mixed single/bulk reads must share one coherent consumer cache. Start
 * close to UINT64_MAX so both publication indices wrap during the test. */
static void test_mixed_wrap(void)
{
    SDLOP_RingInit(&ring, storage, CAPACITY, sizeof(uint64_t));
    uint64_t start = UINT64_MAX - CAPACITY;
    atomic_store(&ring.head, start);
    atomic_store(&ring.tail, start);
    ring.cached_head = ring.cached_tail = start;
    uint64_t written = 0, read = 0, rejected = 0, batch[137];
    uint32_t random = 1;
    for (int i = 0; i < 200000; i++) {
        random = random * 1664525u + 1013904223u;
        if (random & 0x10000u) {
            bool ok = SDLOP_RingPush(&ring, &written);
            assert(ok == (written - read < CAPACITY));
            if (ok) { written++; } else { rejected++; }
        } else if (random & 0x20000u) {
            uint32_t max = random % 138;
            uint32_t n = SDLOP_RingPopBatch(&ring, batch, max);
            uint64_t available = written - read;
            assert(n == (available < max ? available : max));
            for (uint32_t j = 0; j < n; j++) { assert(batch[j] == read++); }
        } else {
            uint64_t value;
            bool ok = SDLOP_RingPop(&ring, &value);
            assert(ok == (written != read));
            if (ok) { assert(value == read++); }
        }
        assert(SDLOP_RingCount(&ring) == written - read);
        assert(SDLOP_RingDropped(&ring) == rejected);
    }
    assert(written > 2 * CAPACITY); /* actually crossed the wrap point */
    while (SDLOP_RingPop(&ring, batch)) { assert(batch[0] == read++); }
    assert(read == written && SDLOP_RingEmpty(&ring));
}

int main(void)
{
    SDLOP_RingInit(&ring, storage, CAPACITY, sizeof(uint64_t));
    assert(SDLOP_RingEmpty(&ring));

    pthread_t t;
    assert(pthread_create(&t, NULL, producer, NULL) == 0);

    uint64_t expected = 0;
    while (expected < TOTAL) {
        uint64_t v;
        if (SDLOP_RingPop(&ring, &v)) {
            assert(v == expected); /* ordering + no corruption */
            expected++;
        }
    }
    pthread_join(t, NULL);
    assert(SDLOP_RingEmpty(&ring));
    assert(expected == TOTAL);

    /* overflow behavior: push CAPACITY+1 items without consuming */
    SDLOP_RingInit(&ring, storage, CAPACITY, sizeof(uint64_t));
    for (uint64_t i = 0; i < CAPACITY; i++) {
        assert(SDLOP_RingPush(&ring, &i));
    }
    uint64_t overflow_item = 0xDEAD;
    assert(!SDLOP_RingPush(&ring, &overflow_item)); /* full -> drops */
    assert(SDLOP_RingDropped(&ring) == 1);
    assert(SDLOP_RingCount(&ring) == CAPACITY);

    /* drain and verify oldest-first survived */
    uint64_t v;
    assert(SDLOP_RingPop(&ring, &v) && v == 0);
    for (uint64_t i = 1; i < CAPACITY; i++) {
        assert(SDLOP_RingPop(&ring, &v) && v == i);
    }
    assert(!SDLOP_RingPop(&ring, &v));

    test_batch();
    test_mixed_wrap();
    printf("test_ring: PASS (%d single + %d mixed/batch events, cached limits, 64-bit wrap/empty/overflow OK)\n", TOTAL, TOTAL);
    return 0;
}
