/*
 * asx/time/sleep.h — public sleep, timeout, and interval poll primitives
 *
 * These are task-level time primitives implemented as poll functions.
 * Each uses captured state allocated from a region's capture arena
 * (via asx_scope_spawn_captured or asx_task_spawn_captured).
 *
 * Sleep:    returns PENDING until deadline, then OK
 * Timeout:  wraps an inner poll_fn; returns ASX_E_TIMED_OUT if deadline
 *           expires before inner completes
 * Interval: fires repeatedly at fixed intervals; increments a counter
 *           each period; completes after max_ticks (0 = unlimited)
 * DelayQueue: values that each come due at their own deadline, earliest
 *           first, addressed by keys (Rust time::DelayQueue)
 *
 * All primitives integrate with the timer wheel for efficient scheduling
 * and support deterministic replay via virtual time.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TIME_SLEEP_H
#define ASX_TIME_SLEEP_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/runtime/runtime.h>
#include <asx/time/deadline.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Sleep
 *
 * A poll function that returns ASX_E_PENDING until the specified
 * duration has elapsed (or the specified time is reached), then returns
 * ASX_OK. It waits on the task's traced sleep timer (asx_task_wait_until).
 *
 * Like Rust's Sleep (time/sleep.rs:774-812), it is a cancellation point:
 * once the task is cancelled with a kind other than Timeout or Deadline
 * and is not masked, the poll acknowledges the cancel (asx_checkpoint),
 * drops the timer and returns ASX_OK early. A sleep first polled after
 * the cancel takes one trip through the scheduler first. Timeout and
 * Deadline cancels leave it sleeping: the budget owner reports them.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_deadline deadline;
    int initialized;      /* 1 after first poll registers timer */
    uint64_t duration_ns; /* requested sleep duration */
    int absolute;         /* 1: sleep until `until_ns` (asx_sleep_init_until) */
    asx_time until_ns;
    int polled; /* polled at least once (Rust Sleep::polled) */
} asx_sleep_state;

/* Initialize a sleep state for the given duration in nanoseconds,
 * measured from the first poll.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if state is NULL. */
ASX_API ASX_MUST_USE asx_status asx_sleep_init(asx_sleep_state *state, uint64_t duration_ns);

/* Initialize a sleep state that waits until runtime-clock time `until_ns`
 * (Rust time::sleep_until).
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if state is NULL. */
ASX_API ASX_MUST_USE asx_status asx_sleep_init_until(asx_sleep_state *state, asx_time until_ns);

/* Poll function for sleep. Use as the poll_fn for a task.
 * user_data must point to an initialized asx_sleep_state.
 * Returns ASX_E_PENDING while sleeping, ASX_OK when done (or ended early
 * by a cancel, see above). */
ASX_API asx_status asx_sleep_poll(void *user_data, asx_task_id self);

/* -------------------------------------------------------------------
 * Timeout
 *
 * Wraps an inner poll function with a deadline, as Rust's TimeoutFuture
 * (time/timeout_future.rs:286-315). Past the deadline (now > deadline) it
 * returns ASX_E_TIMED_OUT without polling the inner function. Otherwise
 * it polls the inner function first, so work that completes exactly at
 * the deadline wins, and it times out at the deadline only if that work
 * is still pending. A poll after it completed returns ASX_E_TIMED_OUT
 * (Rust's fail-closed repoll).
 * ------------------------------------------------------------------- */

typedef struct {
    asx_deadline deadline;
    int initialized;             /* 1 after first poll */
    uint64_t timeout_ns;         /* configured timeout duration */
    asx_task_poll_fn inner_poll; /* wrapped poll function */
    void *inner_data;            /* wrapped poll user_data */
    int inner_done;              /* 1 if inner returned ASX_OK */
    int completed;               /* 1 once a poll returned other than ASX_E_PENDING */
} asx_timeout_state;

/* Initialize a timeout state wrapping an inner poll function.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if state or
 * inner_poll is NULL. */
ASX_API ASX_MUST_USE asx_status asx_timeout_init(asx_timeout_state *state, uint64_t timeout_ns,
                                                 asx_task_poll_fn inner_poll, void *inner_data);

/* Poll function for timeout. Use as the poll_fn for a task.
 * user_data must point to an initialized asx_timeout_state.
 * Returns ASX_E_PENDING while waiting, ASX_OK if inner completes,
 *   ASX_E_TIMED_OUT if deadline expires. */
ASX_API asx_status asx_timeout_poll(void *user_data, asx_task_id self);

/* -------------------------------------------------------------------
 * Interval
 *
 * A periodic timer with the tick times of Rust's Interval
 * (time/interval.rs). The first tick is at the interval's start: the
 * first poll's time for asx_interval_init (Rust interval(now, period)) or
 * the given time for asx_interval_init_at (Rust interval_at). Each tick
 * sets the next deadline as its missed-tick behavior says (Burst by
 * default, as in Rust). Each tick increments a counter, and a poll counts
 * every tick due at its time, as back-to-back Rust ticks at that time
 * would: a Burst interval polled late catches up on the missed ticks at
 * once. The interval completes when max_ticks is reached (0 = unlimited:
 * it never returns ASX_OK).
 *
 * Rust saturates deadlines at the last representable time, fires a tick
 * there and then stays silent; a C interval that reaches that point
 * returns ASX_E_TIMER_DURATION_EXCEEDED instead of waiting forever.
 *
 * The tick counter can be read by external code for progress tracking.
 * ------------------------------------------------------------------- */

/* Where the deadline after a tick goes (Rust MissedTickBehavior). */
typedef enum {
    ASX_MISSED_TICK_BURST = 0, /* a period after the previous deadline (the default) */
    ASX_MISSED_TICK_DELAY = 1, /* a period after the tick's time */
    ASX_MISSED_TICK_SKIP = 2   /* the first period boundary after the tick's time */
} asx_missed_tick_behavior;

typedef struct {
    asx_deadline deadline; /* the next tick */
    int initialized;       /* 1 after first poll */
    uint64_t period_ns;    /* interval period */
    uint32_t ticks;        /* number of times interval has fired */
    uint32_t max_ticks;    /* stop after this many (0 = unlimited) */
    int has_start;         /* 1: the first tick is at start_ns */
    asx_time start_ns;
    asx_missed_tick_behavior missed_tick_behavior;
    int exhausted; /* the tick at the last representable time fired */
} asx_interval_state;

/* Initialize an interval whose first tick is at its first poll.
 * max_ticks=0 means unlimited (interval never completes on its own).
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if state is NULL
 *   or period_ns is 0. */
ASX_API ASX_MUST_USE asx_status asx_interval_init(asx_interval_state *state, uint64_t period_ns,
                                                  uint32_t max_ticks);

/* Initialize an interval whose first tick is at runtime-clock time
 * `start_ns` (Rust interval_at). Same returns as asx_interval_init. */
ASX_API ASX_MUST_USE asx_status asx_interval_init_at(asx_interval_state *state, asx_time start_ns,
                                                     uint64_t period_ns, uint32_t max_ticks);

/* Set the missed-tick behavior (Rust Interval::set_missed_tick_behavior);
 * it applies from the next tick on. Returns ASX_E_INVALID_ARGUMENT for a
 * NULL state or an unknown behavior. */
ASX_API ASX_MUST_USE asx_status
asx_interval_set_missed_tick_behavior(asx_interval_state *state, asx_missed_tick_behavior behavior);

/* Poll function for interval. Use as the poll_fn for a task.
 * user_data must point to an initialized asx_interval_state.
 * Returns ASX_E_PENDING between ticks, ASX_OK when max_ticks reached
 *   (never returns ASX_OK if max_ticks is 0), and
 *   ASX_E_TIMER_DURATION_EXCEEDED once no later tick exists. */
ASX_API asx_status asx_interval_poll(void *user_data, asx_task_id self);

/* Get the current tick count of an interval.
 * Returns 0 if state is NULL. */
ASX_API uint32_t asx_interval_ticks(const asx_interval_state *state);

/* One tick of the interval's state machine at time `now`, without waiting
 * (Rust Interval::tick / poll_tick, time/interval.rs:261-303). *out_time is
 * the tick's deadline as it was before this call. If a tick is due at `now`
 * (the interval is not exhausted and now >= that deadline), *out_due is 1
 * and the deadline moves on as the missed-tick behavior says: one tick per
 * call, so a Burst interval polled late is called again for each missed
 * tick. Otherwise *out_due is 0 and nothing changes (Rust's tick returns the
 * deadline without advancing). The first call fixes the first deadline as
 * the poll function does: the init_at start time, else `now`. max_ticks is
 * not consulted (Rust's Interval has no limit); ticks is incremented.
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT for a NULL argument. */
ASX_API ASX_MUST_USE asx_status asx_interval_tick_at(asx_interval_state *state, asx_time now,
                                                     asx_time *out_time, int *out_due);

/* -------------------------------------------------------------------
 * DelayQueue
 *
 * A queue of values that each come due at their own deadline, as Rust's
 * DelayQueue (time/delay_queue.rs): entries come out earliest deadline
 * first, ties in insertion order (a reset counts as a new insertion), and
 * each is addressed by the key its insertion returned, which stops being
 * valid when the entry expires or is removed. Storage is fixed:
 * ASX_DELAY_QUEUE_CAPACITY live entries (insert fails with
 * ASX_E_RESOURCE_EXHAUSTED beyond it). Values are uint64_t tokens.
 *
 * The queue is caller-owned and holds no clock: the pure functions take
 * `now`, as Rust's take wall_now(). asx_delay_queue_poll is the task-level
 * form: it reads the runtime clock and, while nothing is due, waits on the
 * calling task's traced sleep timer for the earliest deadline. Like
 * asx_sleep_poll it shares that one timer slot, so a task should not
 * interleave another sleep with a pending poll; Rust's queue owns a
 * separate Sleep.
 * ------------------------------------------------------------------- */

#define ASX_DELAY_QUEUE_CAPACITY 16u
/* Stale heap items (resets and removals) are rebuilt away before they
 * outnumber twice the live entries plus 64, as Rust's queue does. */
#define ASX_DELAY_QUEUE_HEAP_CAPACITY (2u * ASX_DELAY_QUEUE_CAPACITY + 65u)

/* Names one entry. Valid while it matches a live entry's generation. */
typedef struct {
    uint32_t index;
    uint64_t generation;
} asx_delay_key;

typedef struct {
    uint64_t value;
    asx_time deadline;
    uint64_t generation; /* fixed at insertion: a key is valid while it matches */
    uint64_t schedule;   /* changes on every reset: names the one live heap item */
    int live;
} asx_delay_entry;

typedef struct {
    asx_time deadline;
    uint64_t schedule;
    uint32_t index;
} asx_delay_heap_item;

typedef struct {
    asx_delay_entry entries[ASX_DELAY_QUEUE_CAPACITY];
    uint32_t len; /* live entries */
    asx_delay_heap_item heap[ASX_DELAY_QUEUE_HEAP_CAPACITY];
    uint32_t heap_len;
    uint64_t next_generation;
} asx_delay_queue;

/* What a poll found (Rust's Poll<Option<Expired<T>>>). */
typedef enum {
    ASX_DELAY_EMPTY = 0,   /* Ready(None): the queue has no entry */
    ASX_DELAY_EXPIRED = 1, /* Ready(Some): an entry's deadline has passed */
    ASX_DELAY_WAITING = 2  /* Pending: the earliest deadline is still ahead */
} asx_delay_poll_state;

/* An entry that expired or was removed. */
typedef struct {
    uint64_t value;
    asx_time deadline; /* the deadline the entry had */
    asx_delay_key key; /* no longer valid */
} asx_delay_expired;

/* Make `queue` empty. Returns ASX_E_INVALID_ARGUMENT for NULL. */
ASX_API ASX_MUST_USE asx_status asx_delay_queue_init(asx_delay_queue *queue);

/* Insert `value` due at `deadline` (Rust insert_at). The new entry's key is
 * stored in *out_key.
 * Returns ASX_E_RESOURCE_EXHAUSTED when the queue holds
 *   ASX_DELAY_QUEUE_CAPACITY entries, with nothing changed. */
ASX_API ASX_MUST_USE asx_status asx_delay_queue_insert_at(asx_delay_queue *queue, uint64_t value,
                                                          asx_time deadline,
                                                          asx_delay_key *out_key);

/* Insert `value` due `timeout_ns` after `now`, saturating at the last
 * representable time (Rust insert). Same returns as insert_at. */
ASX_API ASX_MUST_USE asx_status asx_delay_queue_insert(asx_delay_queue *queue, asx_time now,
                                                       uint64_t value, uint64_t timeout_ns,
                                                       asx_delay_key *out_key);

/* Remove the entry `key` names and return it (Rust try_remove).
 * Returns ASX_E_NOT_FOUND if the key is not valid (its entry expired or
 *   was removed). */
ASX_API ASX_MUST_USE asx_status asx_delay_queue_remove(asx_delay_queue *queue,
                                                       const asx_delay_key *key,
                                                       asx_delay_expired *out);

/* Move the entry `key` names to be due at `deadline` (Rust reset_at). The
 * key stays valid; the entry now orders after those already due at the same
 * time. Returns ASX_E_NOT_FOUND if the key is not valid (Rust panics). */
ASX_API ASX_MUST_USE asx_status asx_delay_queue_reset_at(asx_delay_queue *queue,
                                                         const asx_delay_key *key,
                                                         asx_time deadline);

/* Move the entry to be due `timeout_ns` after `now`, saturating (Rust
 * reset). Same returns as reset_at. */
ASX_API ASX_MUST_USE asx_status asx_delay_queue_reset(asx_delay_queue *queue,
                                                      const asx_delay_key *key, asx_time now,
                                                      uint64_t timeout_ns);

/* The deadline of the entry `key` names (Rust deadline).
 * Returns ASX_E_NOT_FOUND if the key is not valid (Rust panics). */
ASX_API ASX_MUST_USE asx_status asx_delay_queue_deadline(const asx_delay_queue *queue,
                                                         const asx_delay_key *key,
                                                         asx_time *out_deadline);

/* Whether `key` still names an entry of `queue` (Rust contains). */
ASX_API int asx_delay_queue_contains(const asx_delay_queue *queue, const asx_delay_key *key);

/* The number of entries (Rust len); 0 for NULL. */
ASX_API uint32_t asx_delay_queue_len(const asx_delay_queue *queue);

/* Remove every entry (Rust clear); keys already handed out stay invalid. */
ASX_API ASX_MUST_USE asx_status asx_delay_queue_clear(asx_delay_queue *queue);

/* Poll for the next entry due at `now`, earliest first (Rust poll_expired
 * without a timer). *out_state is ASX_DELAY_EXPIRED with *out filled (the
 * entry is removed), ASX_DELAY_EMPTY, or ASX_DELAY_WAITING with
 * out->deadline set to the earliest deadline (nothing is removed).
 * Returns ASX_E_INVALID_ARGUMENT for a NULL argument. */
ASX_API ASX_MUST_USE asx_status asx_delay_queue_poll_at(asx_delay_queue *queue, asx_time now,
                                                        asx_delay_poll_state *out_state,
                                                        asx_delay_expired *out);

/* Task-level poll: asx_delay_queue_poll_at at the runtime clock. While
 * nothing is due it waits on the task's traced sleep timer for the earliest
 * deadline, re-arming when an earlier entry arrived (the old timer is
 * cancelled and the new one registered, as Rust's Sleep::reset does).
 * Returns ASX_OK with *out_state ASX_DELAY_EXPIRED or ASX_DELAY_EMPTY, or
 *   ASX_E_PENDING (*out_state ASX_DELAY_WAITING). */
ASX_API asx_status asx_delay_queue_poll(asx_delay_queue *queue, asx_task_id self,
                                        asx_delay_poll_state *out_state, asx_delay_expired *out);

#ifdef __cplusplus
}
#endif

#endif /* ASX_TIME_SLEEP_H */
