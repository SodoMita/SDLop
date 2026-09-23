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

    printf("test_ring: PASS (%d events transferred, overflow drop OK)\n", TOTAL);
    return 0;
}
