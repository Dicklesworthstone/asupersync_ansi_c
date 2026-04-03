/*
 * mpsc_lockfree_spike.c — Lock-free MPSC queue spike (bd-3vt.2)
 *
 * Research spike: Vyukov-style bounded MPSC ring buffer with portable
 * atomic abstraction layer. Uses per-cell sequence numbers to coordinate
 * producers and consumer without locks.
 *
 * Design rationale:
 * - Bounded (fixed capacity, no allocation on hot path)
 * - FIFO ordering preserved
 * - Producers: CAS on cell sequence, then write value
 * - Consumer: load sequence, read value, advance head
 * - Single-consumer (MPSC, not MPMC)
 *
 * In single-threaded mode (ASX_LOCKFREE_SINGLE_THREAD), atomics degrade
 * to plain loads/stores for isomorphic testing against baseline queue.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/platform/atomics.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Lock-free MPSC ring buffer                                         */
/* ------------------------------------------------------------------ */

#define LOCKFREE_MAX_CAPACITY 64u

typedef struct {
    asx_atomic_u32 sequence;
    uint64_t value;
} asx_lockfree_cell;

typedef struct {
    asx_lockfree_cell cells[LOCKFREE_MAX_CAPACITY];
    uint32_t capacity;
    uint32_t mask; /* capacity - 1 (requires power of 2) */

    /* Producer side: shared among multiple producers (would be atomic) */
    asx_atomic_u32 enqueue_pos;

    /* Consumer side: single consumer only */
    uint32_t dequeue_pos;

    /* Metadata */
    uint32_t len; /* tracking for single-threaded equiv */
    int alive;
} asx_lockfree_queue;

/* Round up to next power of 2 */
static uint32_t next_pow2(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

void asx_lockfree_queue_init(asx_lockfree_queue *q, uint32_t capacity) {
    uint32_t i;
    uint32_t actual_cap;

    if (q == NULL || capacity == 0) return;

    actual_cap = next_pow2(capacity);
    if (actual_cap > LOCKFREE_MAX_CAPACITY) { actual_cap = LOCKFREE_MAX_CAPACITY; }

    q->capacity = actual_cap;
    q->mask = actual_cap - 1u;
    asx_atomic_u32_store(&q->enqueue_pos, 0);
    q->dequeue_pos = 0;
    q->len = 0;
    q->alive = 1;

    for (i = 0; i < actual_cap; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: actual_cap <= ASX_LOCKFREE_MAX_CAPACITY");
        asx_atomic_u32_store(&q->cells[i].sequence, i);
        q->cells[i].value = 0;
    }
}

void asx_lockfree_queue_destroy(asx_lockfree_queue *q) {
    if (q == NULL) return;
    q->alive = 0;
    q->len = 0;
}

/*
 * Enqueue: Vyukov bounded MPSC algorithm
 *
 * 1. Load enqueue_pos
 * 2. Load cell[pos].sequence
 * 3. If sequence == pos, CAS enqueue_pos to pos+1
 * 4. Write value, store sequence = pos+1 (signals consumer)
 *
 * Returns: ASX_OK on success
 *          ASX_E_CHANNEL_FULL if ring is full
 */
asx_status asx_lockfree_enqueue(asx_lockfree_queue *q, uint64_t value) {
    uint32_t pos;
    asx_lockfree_cell *cell;
    uint32_t seq;
    int32_t diff;

    if (q == NULL || !q->alive) { return ASX_E_INVALID_ARGUMENT; }

    /* In the real multi-threaded version this is a CAS loop */
    pos = asx_atomic_u32_load(&q->enqueue_pos);
    cell = &q->cells[pos & q->mask];
    seq = asx_atomic_u32_load(&cell->sequence);
    diff = (int32_t)(seq - pos);

    if (diff < 0) {
        /* Queue is full */
        return ASX_E_CHANNEL_FULL;
    }

    if (diff == 0) {
        /* Claim the slot */
        if (!asx_atomic_u32_cas(&q->enqueue_pos, pos, pos + 1u)) {
            /* Contention (can't happen in single-threaded mode) */
            return ASX_E_CHANNEL_FULL;
        }
    }

    /* Write value and signal consumer */
    cell->value = value;
    asx_atomic_u32_store(&cell->sequence, pos + 1u);
    q->len++;

    return ASX_OK;
}

/*
 * Dequeue: Single-consumer side
 *
 * 1. Load cell[dequeue_pos].sequence
 * 2. If sequence == dequeue_pos + 1, value is ready
 * 3. Read value, store sequence = dequeue_pos + capacity (recycles slot)
 *
 * Returns: ASX_OK on success
 *          ASX_E_WOULD_BLOCK if queue is empty
 */
asx_status asx_lockfree_dequeue(asx_lockfree_queue *q, uint64_t *out_value) {
    asx_lockfree_cell *cell;
    uint32_t seq;
    int32_t diff;

    if (q == NULL || out_value == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (!q->alive) { return ASX_E_DISCONNECTED; }

    cell = &q->cells[q->dequeue_pos & q->mask];
    seq = asx_atomic_u32_load(&cell->sequence);
    diff = (int32_t)(seq - (q->dequeue_pos + 1u));

    if (diff < 0) {
        /* Queue is empty */
        return ASX_E_WOULD_BLOCK;
    }

    /* Read value and recycle slot */
    *out_value = cell->value;
    asx_atomic_u32_store(&cell->sequence, q->dequeue_pos + q->capacity);
    q->dequeue_pos++;
    q->len--;

    return ASX_OK;
}

/* Query: current length (single-threaded only — not meaningful under contention) */
uint32_t asx_lockfree_queue_len(const asx_lockfree_queue *q) {
    if (q == NULL) return 0;
    return q->len;
}

/* Query: is queue full? */
int asx_lockfree_queue_is_full(const asx_lockfree_queue *q) {
    if (q == NULL) return 1;
    return q->len >= q->capacity;
}
