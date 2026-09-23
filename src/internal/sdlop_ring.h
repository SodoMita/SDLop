/*
  SDLop - single-producer / single-consumer lock-free ring buffer.

  The hot path (worker thread publishing raw input events, main thread
  draining them) never takes a lock and never allocates:

    - one writer thread: only touches head (relaxed) + publishes with
      release store
    - one reader thread: only touches tail (relaxed) + reads with acquire
      load
    - cache-line padded indices to avoid false sharing
    - power-of-two capacity, wrap with a mask

  On overflow the producer drops the incoming event and counts it.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_ring_h_
#define sdlop_ring_h_

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SDLOP_CACHELINE 64

typedef struct SDLOP_Ring
{
    _Alignas(SDLOP_CACHELINE) atomic_uint_least64_t head; /* producer writes */
    _Alignas(SDLOP_CACHELINE) atomic_uint_least64_t tail; /* consumer reads */
    _Alignas(SDLOP_CACHELINE) uint64_t dropped;           /* producer-side stats */
    uint32_t mask;
    uint32_t elem_size;
    unsigned char *data; /* capacity elements, capacity = mask + 1 */
} SDLOP_Ring;

static inline void SDLOP_RingInit(SDLOP_Ring *ring, void *storage, uint32_t capacity, uint32_t elem_size)
{
    /* capacity must be a power of two */
    atomic_store(&ring->head, 0);
    atomic_store(&ring->tail, 0);
    ring->dropped = 0;
    ring->mask = capacity - 1;
    ring->elem_size = elem_size;
    ring->data = (unsigned char *)storage;
}

static inline bool SDLOP_RingPush(SDLOP_Ring *ring, const void *elem)
{
    uint64_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    if (head - tail > ring->mask) { /* full */
        ring->dropped++;
        return false;
    }
    memcpy(ring->data + (uint32_t)(head & ring->mask) * ring->elem_size, elem, ring->elem_size);
    atomic_store_explicit(&ring->head, head + 1, memory_order_release);
    return true;
}

static inline bool SDLOP_RingPop(SDLOP_Ring *ring, void *elem)
{
    uint64_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (tail == head) {
        return false;
    }
    memcpy(elem, ring->data + (uint32_t)(tail & ring->mask) * ring->elem_size, ring->elem_size);
    atomic_store_explicit(&ring->tail, tail + 1, memory_order_release);
    return true;
}

static inline bool SDLOP_RingEmpty(SDLOP_Ring *ring)
{
    return atomic_load_explicit(&ring->head, memory_order_acquire) ==
           atomic_load_explicit(&ring->tail, memory_order_acquire);
}

static inline uint64_t SDLOP_RingCount(SDLOP_Ring *ring)
{
    uint64_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    return head - tail;
}

static inline uint64_t SDLOP_RingDropped(SDLOP_Ring *ring)
{
    return ring->dropped;
}

#endif /* sdlop_ring.h_ */
