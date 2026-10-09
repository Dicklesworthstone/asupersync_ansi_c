/*
 * wait_queue.h — internal wait queues for wake-driven channels and sync
 * primitives (NOT part of the public API).
 *
 * Channels and sync primitives expose poll-style operations that report
 * "not yet" (ASX_E_PENDING, ASX_E_WOULD_BLOCK, ASX_E_CHANNEL_FULL, ...).
 * When such an operation runs inside a scheduler poll
 * (asx_task_current() != ASX_INVALID_ID) the primitive records the calling
 * task in a bounded FIFO wait queue and parks it; the state change that can
 * satisfy the waiter wakes it with asx_task_wake(). Outside a scheduler poll
 * nothing is recorded or parked, so try-style callers keep their exact
 * pre-existing semantics.
 *
 * Contract shared by every queue:
 *   - FIFO by arrival. A task appears at most once per queue; a task that
 *     re-polls and still has to wait keeps its original position.
 *   - Entries hold generation-checked task handles (the state bits embedded
 *     in task handles are ignored for identity). Entries whose task has
 *     completed or whose slot was reclaimed are dropped lazily when the
 *     queue is scanned; waking them is never counted as a delivery.
 *   - A woken entry stays queued (flagged woken) until its task re-polls:
 *     success or a terminal result removes it (leave), another "not yet"
 *     re-arms it in place (park).
 *   - settle(q, available) hands out `available` units of progress in FIFO
 *     order: outstanding wakes of live tasks count against `available`, and
 *     un-woken waiters are woken until the two match. Primitives call it
 *     after every state change, so one unit wakes exactly one waiter, a
 *     waiter that dies after being woken is replaced on the next settle,
 *     and a cancel-pending waiter never absorbs a unit (it is woken, but
 *     the unit also goes to the next live waiter).
 *   - remove(q, task) withdraws a waiter that gives up (the analog of
 *     dropping a Rust wait future); re-settling then passes any wake it
 *     held on to the next waiter.
 *   - live_ahead(q) supports queue-jump prevention: competitive primitives
 *     let a caller take a unit only while fewer live waiters are queued
 *     ahead of it than there are units, so the units a settle handed out
 *     cannot be stolen before their waiters re-poll.
 *   - A full queue first drops dead entries, then evicts the oldest entry
 *     that already holds a wake; if every entry is a live, un-woken waiter
 *     the caller is not parked and simply yields cooperatively (it is
 *     re-polled next round), so overflow degrades to polling, never to a
 *     lost wakeup.
 *
 * All operations are deterministic: identical call sequences produce
 * identical wake sequences.
 *
 * Thread-safety: scheduler-thread state, like asx_task_wake(). Producers on
 * foreign threads (lock-free channel backend) only reach queue code when a
 * scheduler task is parked on that channel, and must not race with it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_WAIT_QUEUE_H
#define ASX_SYNC_WAIT_QUEUE_H

#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/sync/semaphore.h>
#include <stdint.h>

/* Maximum capacity of one wait queue (woken flags live in a 64-bit mask). */
#ifndef ASX_WAIT_QUEUE_MAX_CAPACITY
#define ASX_WAIT_QUEUE_MAX_CAPACITY 64u
#endif
#if (ASX_WAIT_QUEUE_MAX_CAPACITY) < 1 || (ASX_WAIT_QUEUE_MAX_CAPACITY) > 64
#error "ASX_WAIT_QUEUE_MAX_CAPACITY must be 1..64 (woken flags are a 64-bit mask)"
#endif

/* A bounded FIFO of parked tasks. Storage is owned by the primitive. */
typedef struct {
    asx_task_id *tasks; /* FIFO storage, index 0 = oldest */
    uint32_t cap;       /* usable entries (<= ASX_WAIT_QUEUE_MAX_CAPACITY) */
    uint32_t len;       /* queued entries */
    uint64_t woken;     /* bit i: tasks[i] holds an undelivered wake */
} asx_wait_queue;

/* Liveness classes reported by asx_wait_task_liveness(). */
typedef enum {
    ASX_WAIT_TASK_DEAD = 0,  /* completed, reclaimed, or invalid handle */
    ASX_WAIT_TASK_LIVE = 1,  /* created or running */
    ASX_WAIT_TASK_DOOMED = 2 /* cancel requested / cancelling / finalizing */
} asx_wait_task_liveness_kind;

/* Bind a queue to its storage and empty it. cap is clamped to
 * ASX_WAIT_QUEUE_MAX_CAPACITY. */
void asx_wait_queue_init(asx_wait_queue *q, asx_task_id *storage, uint32_t cap);

/* Number of queued entries (live or not yet reaped). */
uint32_t asx_wait_queue_len(const asx_wait_queue *q);

/* The calling operation is "not yet": enqueue the current task (or re-arm
 * its existing entry) and park it. Returns 1 if the caller was parked, 0 if
 * there is no current task or the queue is full of live un-woken waiters. */
int asx_wait_queue_park_current(asx_wait_queue *q);

/* The current task stopped waiting on this queue because it got what it
 * waited for or hit a terminal result: remove its entry if present (a
 * wake it held is consumed). */
void asx_wait_queue_leave_current(asx_wait_queue *q);

/* `task` gives up waiting: remove its entry. Returns 1 if it was queued.
 * The caller re-settles so a wake the entry held is passed on. */
int asx_wait_queue_remove(asx_wait_queue *q, asx_task_id task);

/* Hand out `available` units of progress in FIFO order (see above).
 * Returns the number of live waiters newly woken. */
uint32_t asx_wait_queue_settle(asx_wait_queue *q, uint32_t available);

/* FIFO gating for queue-jump prevention: the number of live waiters queued
 * ahead of the current task — all live waiters if the current task is not
 * queued or there is no current task. Cancel-pending and dead waiters do
 * not count (they never hold a place in line). */
uint32_t asx_wait_queue_live_ahead(const asx_wait_queue *q);

/* Wake every queued waiter in FIFO order (broadcast events, close,
 * disconnect). Returns the number of live tasks woken. */
uint32_t asx_wait_queue_wake_all(asx_wait_queue *q);

/* Classify a waiter task handle (see asx_wait_task_liveness_kind). */
asx_wait_task_liveness_kind asx_wait_task_liveness(asx_task_id task);

/* Explicit-waiter helpers (primitives whose waiters are slot records):
 * record the current task in *slot and park it. Returns 1 if parked, 0 if
 * there is no current task (*slot is then set to ASX_INVALID_ID). */
int asx_wait_park_current(asx_task_id *slot);

/* Wake a recorded waiter task; invalid or stale handles are ignored. */
void asx_wait_wake_task(asx_task_id task);

/* A semaphore whose permits never register a SemaphorePermit obligation:
 * the mutex is a one-permit semaphore, and Rust's Mutex guard is not an
 * obligation (sync/mutex.rs registers none). Otherwise as
 * asx_semaphore_create. */
asx_status asx_semaphore_create_untracked(uint32_t initial_permits, asx_semaphore_handle *out);

/* 1 if both handles name the same task slot generation (the state bits
 * embedded in task handles are ignored). */
int asx_wait_same_task(asx_task_id a, asx_task_id b);

#endif /* ASX_SYNC_WAIT_QUEUE_H */
