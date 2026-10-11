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

#ifdef __cplusplus
}
#endif

#endif /* ASX_TIME_SLEEP_H */
