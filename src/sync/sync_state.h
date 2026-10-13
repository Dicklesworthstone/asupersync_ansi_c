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

/* ------------------------------------------------------------------ */
/* Read-write locks                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    uint32_t readers;         /* readers holding the lock, granted ones included */
    int write_locked;         /* a writer holds the lock (or was granted it) */
    uint32_t writers_waiting; /* queued writers plus granted ones not yet polled */
    uint32_t writer_streak;   /* writer hand-offs in a row while readers were queued */
    int poisoned;             /* a write guard was dropped while its task panicked */
    asx_wait_queue waiters;   /* waiter records, arrival order */
} asx_rw_slot;

typedef struct {
    asx_rw_slot slots[ASX_RWLOCK_MAX];
    uint32_t slot_count;
} asx_rw_state;

/* ------------------------------------------------------------------ */
/* Barriers                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    uint32_t threshold;     /* N tasks required */
    uint32_t arrived;       /* tasks currently waiting */
    int tripped;            /* barrier released */
    asx_wait_queue waiters; /* waiter records, arrival order */
} asx_barrier_slot;

typedef struct {
    asx_barrier_slot slots[ASX_BARRIER_MAX];
    uint32_t slot_count;
} asx_barrier_state;

/* ------------------------------------------------------------------ */
/* Once cells                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    int initialized;
    uint64_t value;
    asx_task_id initializer; /* task running a pending init, or ASX_INVALID_ID */
    asx_wait_queue waiters;  /* tasks parked behind the initializer */
} asx_once_slot;

typedef struct {
    asx_once_slot slots[ASX_ONCE_MAX];
    uint32_t slot_count;
} asx_once_state;

/* ------------------------------------------------------------------ */
/* Notifies                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    asx_wait_queue waiters; /* waiter records, arrival order */
    /* notify_one notifications that found no waiter. They accumulate and
     * each later waiter consumes one (Rust sync/notify.rs
     * stored_notifications; unlike tokio, which stores at most one). */
    uint32_t stored;
} asx_notify_slot;

typedef struct {
    asx_notify_slot slots[ASX_NOTIFY_MAX];
    uint32_t slot_count;
} asx_notify_state;

#endif /* ASX_SYNC_SYNC_STATE_H */
