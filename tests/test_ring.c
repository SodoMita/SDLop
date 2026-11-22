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

static void pattern(unsigned char *record, size_t size, uint64_t value)
{
    for (size_t i = 0; i < size; i++) { record[i] = (unsigned char)(value * 37 + i * 13); }
    size_t n = size < sizeof(value) ? size : sizeof(value);
    memcpy(record, &value, n);
}

/* Exercise byte copies, split batches and tiny capacities, not just aligned
 * uint64_t records in the 1024-slot throughput test. */
static void test_sizes(void)
{
    const uint32_t capacities[] = { 1, 2, 4, 64, 1024, 32768 };
    const uint32_t sizes[] = { 1, 3, 24, 31, 64 };
    for (size_t c = 0; c < sizeof(capacities) / sizeof(*capacities); c++) {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); s++) {
            uint32_t capacity = capacities[c], size = sizes[s];
            unsigned char *data = malloc((size_t)capacity * size);
            assert(data);
            SDLOP_Ring local;
            SDLOP_RingInit(&local, data, capacity, size);
            uint64_t written = 0, read = 0, drops = 0;
            unsigned char record[64], expected[64], batch[17 * 64];
            /* Fill/reject once even in large rings, then wrap the storage. */
            for (; written < capacity; written++) {
                pattern(record, size, written);
                assert(SDLOP_RingPush(&local, record));
            }
            assert(!SDLOP_RingPush(&local, record));
            drops++;
            uint32_t random = 7;
            for (int i = 0; i < 20000; i++) {
                random = random * 1664525u + 1013904223u;
                if (random & 0x10000u) {
                    pattern(record, size, written);
                    bool ok = SDLOP_RingPush(&local, record);
                    assert(ok == (written - read < capacity));
                    if (ok) { written++; } else { drops++; }
                } else {
                    uint32_t max = random % 18;
                    uint32_t n = SDLOP_RingPopBatch(&local, max ? batch : NULL, max);
                    uint64_t available = written - read;
                    assert(n == (available < max ? available : max));
                    for (uint32_t j = 0; j < n; j++) {
                        pattern(expected, size, read++);
                        assert(!memcmp(batch + (size_t)j * size, expected, size));
                    }
                }
                assert(SDLOP_RingCount(&local) == written - read);
                assert(SDLOP_RingDropped(&local) == drops);
            }
            while (SDLOP_RingPop(&local, record)) {
                pattern(expected, size, read++);
                assert(!memcmp(record, expected, size));
            }
            assert(read == written);
            free(data);
        }
    }
    /* A cached partial prefix must not cause an undersized bulk return when
     * more records have been published since that cache was acquired. */
    SDLOP_RingInit(&ring, storage, CAPACITY, sizeof(uint64_t));
    for (uint64_t i = 0; i < 10; i++) { assert(SDLOP_RingPush(&ring, &i)); }
    uint64_t values[12];
    assert(SDLOP_RingPop(&ring, values) && values[0] == 0);
    for (uint64_t i = 10; i < 20; i++) { assert(SDLOP_RingPush(&ring, &i)); }
    assert(SDLOP_RingPopBatch(&ring, values, 12) == 12);
    for (uint64_t i = 0; i < 12; i++) { assert(values[i] == i + 1); }
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
    test_sizes();
    printf("test_ring: PASS (%d single + %d mixed/batch events, 30 capacity/record layouts, cached limits, 64-bit wrap/empty/overflow OK)\n", TOTAL, TOTAL);
    return 0;
}
