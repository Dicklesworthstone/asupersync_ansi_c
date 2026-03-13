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
#include <asx/asx_status.h>
#include <asx/asx_ids.h>
#include <asx/time/deadline.h>
#include <asx/runtime/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Sleep
 *
 * A poll function that returns ASX_E_PENDING until the specified
 * duration has elapsed, then returns ASX_OK.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_deadline  deadline;
    int           initialized;  /* 1 after first poll registers timer */
    uint64_t      duration_ns;  /* requested sleep duration */
} asx_sleep_state;

/* Initialize a sleep state for the given duration in nanoseconds.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if state is NULL. */
ASX_API ASX_MUST_USE asx_status asx_sleep_init(
    asx_sleep_state *state,
    uint64_t duration_ns);

/* Poll function for sleep. Use as the poll_fn for a task.
 * user_data must point to an initialized asx_sleep_state.
 * Returns ASX_E_PENDING while sleeping, ASX_OK when done. */
ASX_API asx_status asx_sleep_poll(void *user_data, asx_task_id self);

/* -------------------------------------------------------------------
 * Timeout
 *
 * Wraps an inner poll function with a deadline. If the inner function
 * completes (returns ASX_OK) before the deadline, the timeout
 * returns ASX_OK. If the deadline expires first, returns
 * ASX_E_TIMED_OUT.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_deadline      deadline;
    int               initialized;   /* 1 after first poll */
    uint64_t          timeout_ns;    /* configured timeout duration */
    asx_task_poll_fn  inner_poll;    /* wrapped poll function */
    void             *inner_data;    /* wrapped poll user_data */
    int               inner_done;    /* 1 if inner returned ASX_OK */
} asx_timeout_state;

/* Initialize a timeout state wrapping an inner poll function.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if state or
 * inner_poll is NULL. */
ASX_API ASX_MUST_USE asx_status asx_timeout_init(
    asx_timeout_state *state,
    uint64_t timeout_ns,
    asx_task_poll_fn inner_poll,
    void *inner_data);

/* Poll function for timeout. Use as the poll_fn for a task.
 * user_data must point to an initialized asx_timeout_state.
 * Returns ASX_E_PENDING while waiting, ASX_OK if inner completes,
 *   ASX_E_TIMED_OUT if deadline expires. */
ASX_API asx_status asx_timeout_poll(void *user_data, asx_task_id self);

/* -------------------------------------------------------------------
 * Interval
 *
 * A periodic timer that fires at fixed intervals. Each period
 * increments a tick counter. The interval completes when max_ticks
 * is reached (set to 0 for unlimited — will never return ASX_OK).
 *
 * The tick counter can be read by external code for progress tracking.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_deadline  deadline;
    int           initialized;   /* 1 after first poll */
    uint64_t      period_ns;     /* interval period */
    uint32_t      ticks;         /* number of times interval has fired */
    uint32_t      max_ticks;     /* stop after this many (0 = unlimited) */
} asx_interval_state;

/* Initialize an interval state for the given period.
 * max_ticks=0 means unlimited (interval never completes on its own).
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if state is NULL
 *   or period_ns is 0. */
ASX_API ASX_MUST_USE asx_status asx_interval_init(
    asx_interval_state *state,
    uint64_t period_ns,
    uint32_t max_ticks);

/* Poll function for interval. Use as the poll_fn for a task.
 * user_data must point to an initialized asx_interval_state.
 * Returns ASX_E_PENDING between ticks, ASX_OK when max_ticks reached
 *   (never returns ASX_OK if max_ticks is 0). */
ASX_API asx_status asx_interval_poll(void *user_data, asx_task_id self);

/* Get the current tick count of an interval.
 * Returns 0 if state is NULL. */
ASX_API uint32_t asx_interval_ticks(const asx_interval_state *state);

#ifdef __cplusplus
}
#endif

#endif /* ASX_TIME_SLEEP_H */
