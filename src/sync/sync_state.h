/*
 * sync_state.h — the state of the sync primitives that lives in a runtime's
 * kernel (internal, NOT part of the public API)
 *
 * Each primitive's arena moves here from its own file's statics as it
 * becomes per runtime (K18, bd-9kll.3.20): runtime_internal.h embeds these
 * types in asx_kernel, a handle of the primitive names its arena by the
 * runtime tag in the high 16 bits of its slot field (tag 0: the index
 * alone), and a zeroed state is an empty arena.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_SYNC_STATE_H
#define ASX_SYNC_SYNC_STATE_H

#include "wait_queue.h"
#include <asx/asx_config.h>
#include <asx/sync/semaphore.h>
#include <stdint.h>

/* A slot field of a sync handle: the arena index in the low half, the tag
 * of the runtime whose arena it is in the high half. */
#define ASX_SYNC_SLOT_TAG_SHIFT 16u
#define ASX_SYNC_SLOT_INDEX_MASK 0xFFFFu

/* ------------------------------------------------------------------ */
/* Semaphores (and the lock under asx_mutex)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    uint32_t permits;       /* available permits */
    uint32_t max_permits;   /* initial capacity (for queries) */
    uint32_t outstanding;   /* permits held by handed-out permit values */
    asx_wait_queue waiters; /* waiter records, arrival order */
    /* The lock under asx_mutex: Rust's Mutex hand-off, and no obligation
     * (Rust's Mutex registers none). Otherwise a Semaphore, whose permits
     * reserve SemaphorePermit obligations. */
    int is_mutex;
    /* Mutex only: a holder's guard was dropped while its task panicked
     * (Rust's MutexGuard drop, sync/mutex.rs:758-763). Every later lock
     * fails with LockError::Poisoned, ASX_E_INVALID_STATE. */
    int poisoned;
    /* Mutex only: the serial of the guard holding the lock (0 while
     * nobody holds it) and the last serial handed out. A guard unlocks
     * only while it holds the lock, as Rust's guard is consumed by its
     * drop. */
    uint32_t holder_serial;
    uint32_t last_serial;
} asx_sem_slot;

typedef struct {
    asx_sem_slot slots[ASX_SEMAPHORE_MAX];
    uint32_t slot_count;
} asx_sem_state;

#endif /* ASX_SYNC_SYNC_STATE_H */
