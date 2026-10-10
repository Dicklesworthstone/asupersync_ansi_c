/*
 * wait_queue.h — internal wait queues for wake-driven channels and sync
 * primitives (NOT part of the public API).
 *
 * Channels and sync primitives expose poll-style operations that report
 * "not yet" (ASX_E_PENDING, ASX_E_WOULD_BLOCK, ASX_E_CHANNEL_FULL, ...).
 * When such an operation runs inside a scheduler poll
 * (asx_task_current() != ASX_INVALID_ID) the primitive records the calling
 * task in a FIFO wait queue and parks it; the state change that can
 * satisfy the waiter wakes it with asx_task_wake(). Outside a scheduler poll
 * nothing is recorded or parked, so try-style callers keep their exact
 * pre-existing semantics.
 *
 * Storage. Every queue is a doubly linked list of nodes drawn from one
 * runtime-wide pool of ASX_WAIT_NODE_CAPACITY nodes, so a queue holds as
 * many waiters as there are tasks (Rust's waiter queues are unbounded
 * VecDeques/Slabs; there is no per-primitive cap). Only the pool as a
 * whole can run out; before reporting that, every node whose task has
 * died is reclaimed through its queue (see asx_wait_reap_fn).
 *
 * Plain queues (park / leave / remove / settle / wake_all), used by
 * channels, once and pool:
 *   - FIFO by arrival. A task appears at most once per queue; a task that
 *     re-polls and still has to wait keeps its original position.
 *   - Entries hold generation-checked task handles (the state bits embedded
 *     in task handles are ignored for identity). Entries whose task has
 *     completed or whose slot was reclaimed never count and are unlinked
 *     on the next park; waking them is never counted as a delivery.
 *   - A woken entry stays queued (flagged woken) until its task re-polls:
 *     success or a terminal result removes it (leave), another "not yet"
 *     re-arms it in place (park).
 *   - settle(q, available) hands out `available` units of progress in FIFO
 *     order: outstanding wakes of unfinished tasks count against
 *     `available`, and un-woken waiters are woken until the two match.
 *     Primitives call it after every state change, so one unit wakes
 *     exactly one waiter and a waiter that dies after being woken is
 *     replaced on the next settle. A cancel-pending waiter keeps its place
 *     and absorbs a unit like any other; it passes the unit on when its
 *     next poll observes the cancel and removes it (Rust's waiters stay
 *     queued until their future is polled or dropped).
 *   - remove(q, task) withdraws a waiter that gives up (the analog of
 *     dropping a Rust wait future); re-settling then passes any wake it
 *     held on to the next waiter.
 *   - live_ahead(q) supports queue-jump prevention: competitive primitives
 *     let a caller take a unit only while fewer unfinished waiters
 *     (cancel-pending ones included) are queued ahead of it than there are
 *     units, so the units a settle handed out cannot be stolen before their
 *     waiters re-poll.
 *   - If the pool is exhausted the caller is not parked and simply yields
 *     cooperatively (it is re-polled next round), so exhaustion degrades
 *     to polling, never to a lost wakeup.
 *
 * Waiter records (asx_wait_record_*), used by semaphore/mutex, rwlock,
 * notify and barrier, whose public waiter handles name a record: a record
 * is a node the primitive owns from *_begin until the waiter consumes its
 * result or gives up. List order is arrival order; the primitive keeps
 * its per-waiter state in the node's `flags` and `value`. A queue
 * of records registers a reap function that retires its dead waiters with
 * the primitive's own semantics (returning grants, passing notifications
 * on), so pool reclamation never bypasses them. Plain queues whose
 * primitive hands out units with settle register one too
 * (asx_wait_queue_init); both kinds are also reaped when a task holding
 * an entry finishes (asx_wait_task_finished).
 *
 * Lifetime. Queue storage must be zero-initialized or previously
 * initialized. Initializing a queue releases the nodes it still holds,
 * clear() releases them all, and asx_wait_pool_reset() (runtime reset)
 * empties every queue at once: each queue remembers the pool epoch it
 * was built in and treats itself as empty after a reset.
 *
 * All operations are deterministic: identical call sequences produce
 * identical wake sequences.
 *
 * Thread-safety: scheduler-thread state, like asx_task_wake(). Producers on
 * foreign threads (lock-free channel backend) only reach queue code when a
 * scheduler task is parked on that channel, and must not race with it;
 * settle and wake_all never allocate or release nodes.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_WAIT_QUEUE_H
#define ASX_SYNC_WAIT_QUEUE_H

#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/runtime/runtime.h>
#include <asx/sync/semaphore.h>
#include <stdint.h>

/* Wait nodes shared by every wait queue: by default four per task slot,
 * enough for a task to wait on several primitives at once plus waiter
 * records created outside a scheduler poll. */
#ifndef ASX_WAIT_NODE_CAPACITY
#define ASX_WAIT_NODE_CAPACITY (4u * (ASX_MAX_TASKS))
#endif
#if (ASX_WAIT_NODE_CAPACITY) < 1
#error "ASX_WAIT_NODE_CAPACITY must be at least 1"
#endif

/* No node: list ends and the "no record" result. */
#define ASX_WAIT_NIL UINT32_MAX

typedef struct asx_wait_queue asx_wait_queue;

/* Retire the dead waiters of a record queue with the primitive's own
 * semantics. Called when the pool needs nodes back. */
typedef void (*asx_wait_reap_fn)(asx_wait_queue *q);

/* A FIFO of wait nodes. Storage is owned by the primitive. */
struct asx_wait_queue {
    uint32_t head;         /* oldest node, ASX_WAIT_NIL if empty */
    uint32_t tail;         /* newest node, ASX_WAIT_NIL if empty */
    uint32_t len;          /* linked nodes */
    uint32_t epoch;        /* pool epoch the links belong to */
    asx_wait_reap_fn reap; /* record queues; NULL for plain queues */
};

/* One waiter. `flags` and `value` belong to the owning primitive. */
typedef struct {
    asx_task_id task;      /* parked task, ASX_INVALID_ID if none */
    asx_wait_queue *queue; /* owning queue, NULL when free */
    uint32_t prev;
    uint32_t next;
    uint32_t value;      /* e.g. the permits a semaphore waiter asks for */
    uint16_t generation; /* changes on every release */
    uint8_t woken;       /* plain queues: holds an undelivered wake */
    uint8_t flags;
} asx_wait_node;

/* Liveness classes reported by asx_wait_task_liveness(). */
typedef enum {
    ASX_WAIT_TASK_DEAD = 0,  /* completed, reclaimed, or invalid handle */
    ASX_WAIT_TASK_LIVE = 1,  /* created or running */
    ASX_WAIT_TASK_DOOMED = 2 /* cancel requested / cancelling / finalizing */
} asx_wait_task_liveness_kind;

/* ------------------------------------------------------------------ */
/* Pool                                                                */
/* ------------------------------------------------------------------ */

/* Release every node and empty every queue (runtime reset). */
void asx_wait_pool_reset(void);

/* Nodes currently held by queues (tests and diagnostics). */
uint32_t asx_wait_nodes_in_use(void);

/* `task` finished. As Rust drops a finished task's wait futures, each
 * queue still holding one of its entries is reaped (record queues retire
 * the waiter, plain queues drop it and re-settle), so a wake or grant the
 * entry held passes on; a queue without a reap function drops the entry.
 * The task must already be COMPLETED. */
void asx_wait_task_finished(asx_task_id task);

/* ------------------------------------------------------------------ */
/* Plain queues                                                        */
/* ------------------------------------------------------------------ */

/* Empty a queue, releasing the nodes it still holds. `reap` (NULL for
 * none) drops the queue's dead entries and re-settles the primitive, so a
 * wake a dead entry held passes on; it runs when a task holding an entry
 * finishes (asx_wait_task_finished) and when the pool needs nodes back.
 * Primitives that hand out units with settle register one. */
void asx_wait_queue_init(asx_wait_queue *q, asx_wait_reap_fn reap);

/* Drop every entry whose task is gone (for reap functions). */
void asx_wait_queue_drop_dead(asx_wait_queue *q);

/* Release every node (the primitive is closing; wake its waiters first). */
void asx_wait_queue_clear(asx_wait_queue *q);

/* Number of queued entries (live or not yet unlinked). */
uint32_t asx_wait_queue_len(const asx_wait_queue *q);

/* The calling operation is "not yet": enqueue the current task (or re-arm
 * its existing entry) and park it. Returns 1 if the caller was parked, 0 if
 * there is no current task or the node pool is exhausted. */
int asx_wait_queue_park_current(asx_wait_queue *q);

/* The current task stopped waiting on this queue because it got what it
 * waited for or hit a terminal result: remove its entry if present (a
 * wake it held is consumed). */
void asx_wait_queue_leave_current(asx_wait_queue *q);

/* `task` gives up waiting: remove its entry. Returns 1 if it was queued.
 * The caller re-settles so a wake the entry held is passed on. */
int asx_wait_queue_remove(asx_wait_queue *q, asx_task_id task);

/* Hand out `available` units of progress in FIFO order (see above).
 * Returns the number of waiters newly woken. */
uint32_t asx_wait_queue_settle(asx_wait_queue *q, uint32_t available);

/* FIFO gating for queue-jump prevention: the number of unfinished waiters
 * queued ahead of the current task — all of them if the current task is
 * not queued or there is no current task. A cancel-pending waiter counts
 * (it holds its place until its next poll); a dead one does not. */
uint32_t asx_wait_queue_live_ahead(const asx_wait_queue *q);

/* Wake every queued waiter in FIFO order (broadcast events, close,
 * disconnect). Returns the number of live tasks woken. */
uint32_t asx_wait_queue_wake_all(asx_wait_queue *q);

/* ------------------------------------------------------------------ */
/* Waiter records                                                      */
/* ------------------------------------------------------------------ */

/* Empty a record queue (releasing the nodes it still holds) and register
 * its reap function. */
void asx_wait_records_init(asx_wait_queue *q, asx_wait_reap_fn reap);

/* Append a new record (task ASX_INVALID_ID, flags and value 0). Returns
 * its index, or ASX_WAIT_NIL if the pool is exhausted even after
 * reclaiming the nodes of dead tasks. */
uint32_t asx_wait_record_add(asx_wait_queue *q);

/* The record `index` of queue `q` if `generation` still names it, else
 * NULL (released, reused, or another queue's). */
asx_wait_node *asx_wait_record_get(asx_wait_queue *q, uint32_t index, uint16_t generation);

/* Node `index` (from add, first or next) without validation. */
asx_wait_node *asx_wait_node_at(uint32_t index);

/* Release a record of `q`. */
void asx_wait_record_release(asx_wait_queue *q, uint32_t index);

/* Move a record of `q` to the tail (it arrives now). */
void asx_wait_record_move_to_tail(asx_wait_queue *q, uint32_t index);

/* Iterate a queue in arrival order: the oldest node, and the node after
 * `index`; ASX_WAIT_NIL at the end. Fetch the next index before
 * releasing the current record. */
uint32_t asx_wait_queue_first(const asx_wait_queue *q);
uint32_t asx_wait_queue_next(uint32_t index);

/* ------------------------------------------------------------------ */
/* Tasks                                                               */
/* ------------------------------------------------------------------ */

/* Classify a waiter task handle (see asx_wait_task_liveness_kind). */
asx_wait_task_liveness_kind asx_wait_task_liveness(asx_task_id task);

/* Record the current task in *slot and park it. Returns 1 if parked, 0 if
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
