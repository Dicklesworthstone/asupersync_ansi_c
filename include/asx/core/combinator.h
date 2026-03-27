/*
 * asx/core/combinator.h — structured async control flow combinators
 *
 * Provides join, race, select, timeout, first_ok, and quorum
 * orchestration primitives. Each combinator is a poll state machine
 * compatible with asx_combinator_poll_fn.
 *
 * All state is inline (no dynamic allocation). Inner futures are
 * represented as (poll_fn, user_data) pairs polled cooperatively.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_COMBINATOR_H
#define ASX_CORE_COMBINATOR_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/outcome.h>
#include <asx/time/deadline.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration for task poll function type */
typedef asx_status (*asx_combinator_poll_fn)(void *user_data, asx_task_id self);

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Maximum branches per combinator
 * ------------------------------------------------------------------- */

#ifndef ASX_COMBINATOR_MAX_BRANCHES
#define ASX_COMBINATOR_MAX_BRANCHES 8u
#endif

/* -------------------------------------------------------------------
 * Branch — a single inner future
 * ------------------------------------------------------------------- */

typedef struct {
    asx_combinator_poll_fn poll_fn;
    void *user_data;
    asx_status result; /* final status when done */
    int done;          /* 1 when poll returned non-PENDING */
} asx_combinator_branch;

/* -------------------------------------------------------------------
 * Join — wait for ALL branches to complete
 *
 * Returns ASX_OK when all branches are done.
 * Combined outcome uses asx_outcome_join (max severity).
 * ------------------------------------------------------------------- */

typedef struct {
    asx_combinator_branch branches[ASX_COMBINATOR_MAX_BRANCHES];
    uint32_t count;
    uint32_t done_count;
    asx_outcome combined;
} asx_join_state;

/* Initialize a join combinator. */
ASX_API asx_status asx_join_init(asx_join_state *state);

/* Add a branch to the join. */
ASX_API asx_status asx_join_add(asx_join_state *state, asx_combinator_poll_fn poll_fn,
                                void *user_data);

/* Poll the join combinator. Returns ASX_OK when all branches complete. */
ASX_API asx_status asx_join_poll(void *user_data, asx_task_id self);

/* Get the combined outcome of all joined branches. */
ASX_API asx_outcome asx_join_outcome(const asx_join_state *state);

/* -------------------------------------------------------------------
 * Race — wait for FIRST branch to complete, cancel losers
 *
 * Returns the winning branch's terminal status.
 * The winner index is available via asx_race_winner().
 * Losers receive one final cooperative drain poll; if they remain
 * pending afterward, the combinator resolves them as ASX_E_CANCELLED.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_combinator_branch branches[ASX_COMBINATOR_MAX_BRANCHES];
    uint32_t count;
    int32_t winner; /* -1 until decided */
    int draining;   /* 1 when cancelling losers */
    uint32_t drained;
} asx_race_state;

/* Initialize a race combinator. */
ASX_API asx_status asx_race_init(asx_race_state *state);

/* Add a branch to the race. */
ASX_API asx_status asx_race_add(asx_race_state *state, asx_combinator_poll_fn poll_fn,
                                void *user_data);

/* Poll the race combinator. Returns ASX_OK when a winner is found. */
ASX_API asx_status asx_race_poll(void *user_data, asx_task_id self);

/* Get the winning branch index (-1 if undecided). */
ASX_API int32_t asx_race_winner(const asx_race_state *state);

/* Get the winning branch's result status. */
ASX_API asx_status asx_race_winner_result(const asx_race_state *state);

/* -------------------------------------------------------------------
 * Select — poll all branches, return first ready
 *
 * Like race but uses round-robin fairness. Each poll cycle checks
 * all branches starting from a rotating offset, then gives losing
 * branches one final cooperative drain poll before cancelling them.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_combinator_branch branches[ASX_COMBINATOR_MAX_BRANCHES];
    uint32_t count;
    int32_t winner;
    uint32_t poll_offset; /* fairness rotation */
    int draining;
    uint32_t drained;
} asx_select_state;

/* Initialize a select combinator with round-robin fairness. */
ASX_API asx_status asx_select_init(asx_select_state *state);

/* Add a branch to the select. */
ASX_API asx_status asx_select_add(asx_select_state *state, asx_combinator_poll_fn poll_fn,
                                  void *user_data);

/* Poll the select combinator. Returns ASX_OK when a winner is found. */
ASX_API asx_status asx_select_poll(void *user_data, asx_task_id self);

/* Get the winning branch index (-1 if undecided). */
ASX_API int32_t asx_select_winner(const asx_select_state *state);

/* -------------------------------------------------------------------
 * Timeout — wrap a single future with a deadline
 *
 * Returns ASX_OK if inner completes before deadline.
 * Returns ASX_E_TIMED_OUT if deadline expires first.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_combinator_branch inner;
    asx_deadline deadline;
    uint64_t timeout_ns;
    int initialized;
    int timed_out;
} asx_timeout_combinator_state;

/* Initialize a timeout combinator wrapping an inner poll function. */
ASX_API asx_status asx_timeout_combinator_init(asx_timeout_combinator_state *state,
                                               asx_combinator_poll_fn inner_poll, void *inner_data,
                                               uint64_t timeout_ns);

/* Poll the timeout combinator. Returns ASX_E_TIMED_OUT on deadline expiry. */
ASX_API asx_status asx_timeout_combinator_poll(void *user_data, asx_task_id self);

/* -------------------------------------------------------------------
 * FirstOk — wait for first successful (ASX_OK) branch
 *
 * Tries branches in order. If a branch fails, tries the next.
 * Returns ASX_OK with the first success, or the last error
 * if all branches fail.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_combinator_branch branches[ASX_COMBINATOR_MAX_BRANCHES];
    uint32_t count;
    uint32_t current;      /* index of currently polling branch */
    int32_t winner;        /* -1 until success found */
    asx_status last_error; /* last error for fallback */
} asx_first_ok_state;

/* Initialize a first-ok combinator. */
ASX_API asx_status asx_first_ok_init(asx_first_ok_state *state);

/* Add a branch to the first-ok combinator. */
ASX_API asx_status asx_first_ok_add(asx_first_ok_state *state, asx_combinator_poll_fn poll_fn,
                                    void *user_data);

/* Poll the first-ok combinator. Returns ASX_OK on first successful branch. */
ASX_API asx_status asx_first_ok_poll(void *user_data, asx_task_id self);

/* Get the index of the first successful branch (-1 if none). */
ASX_API int32_t asx_first_ok_winner(const asx_first_ok_state *state);

/* -------------------------------------------------------------------
 * Quorum — wait for N-of-M branches to complete successfully
 *
 * Returns ASX_OK when threshold is reached.
 * Returns error if too many branches fail to reach quorum.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_combinator_branch branches[ASX_COMBINATOR_MAX_BRANCHES];
    uint32_t count;
    uint32_t threshold;  /* required successes */
    uint32_t ok_count;   /* successful completions */
    uint32_t fail_count; /* failed completions */
    int decided;         /* 1 when quorum reached or impossible */
    int draining;
    uint32_t drained;
} asx_quorum_state;

/* Initialize a quorum combinator requiring threshold successes. */
ASX_API asx_status asx_quorum_init(asx_quorum_state *state, uint32_t threshold);

/* Add a branch to the quorum. */
ASX_API asx_status asx_quorum_add(asx_quorum_state *state, asx_combinator_poll_fn poll_fn,
                                  void *user_data);

/* Poll the quorum combinator. Returns ASX_OK when threshold reached. */
ASX_API asx_status asx_quorum_poll(void *user_data, asx_task_id self);

/* Get the number of successful completions. */
ASX_API uint32_t asx_quorum_ok_count(const asx_quorum_state *state);

/* Get the number of failed completions. */
ASX_API uint32_t asx_quorum_fail_count(const asx_quorum_state *state);

/* -------------------------------------------------------------------
 * Cancel token — cooperative cancellation check for combinators
 *
 * Combinators that accept a cancel token check it on each poll cycle.
 * If the token is triggered, the combinator drains in-flight branches
 * and returns ASX_E_CANCELLED.
 * ------------------------------------------------------------------- */

typedef struct {
    const int *cancelled; /* pointer to external flag; NULL = no cancel check */
} asx_cancel_token;

/* Initialize a cancel token from an external flag.
 * When *flag becomes nonzero, combinators using this token will cancel.
 * Pass NULL flag to create an inert (never-cancelled) token. */
static inline asx_cancel_token asx_cancel_token_from(const int *flag) {
    asx_cancel_token t;
    t.cancelled = flag;
    return t;
}

/* Check if a cancel token has been triggered. */
static inline int asx_cancel_token_is_cancelled(const asx_cancel_token *t) {
    if (t == NULL || t->cancelled == NULL) return 0;
    return *t->cancelled != 0;
}

/* Create an inert cancel token that is never cancelled. */
static inline asx_cancel_token asx_cancel_token_none(void) {
    asx_cancel_token t;
    t.cancelled = NULL;
    return t;
}

/* -------------------------------------------------------------------
 * Race with timeout — race all branches against a deadline
 *
 * Returns the winning branch's status if one completes before timeout.
 * Returns ASX_E_TIMED_OUT if deadline expires first.
 * All losers receive one cooperative drain poll before cancellation.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_race_state race;
    asx_deadline deadline;
    uint64_t timeout_ns;
    int initialized;
    int timed_out;
} asx_race_timeout_state;

/* Initialize a race-with-timeout combinator. */
ASX_API asx_status asx_race_timeout_init(asx_race_timeout_state *state, uint64_t timeout_ns);

/* Add a branch to the race-with-timeout. */
ASX_API asx_status asx_race_timeout_add(asx_race_timeout_state *state,
                                         asx_combinator_poll_fn poll_fn, void *user_data);

/* Poll the race-with-timeout. Returns ASX_E_TIMED_OUT on deadline expiry. */
ASX_API asx_status asx_race_timeout_poll(void *user_data, asx_task_id self);

/* -------------------------------------------------------------------
 * Retry with timeout — retry with per-overall-operation deadline
 *
 * Retries up to max_retries on failure. If the total elapsed time
 * exceeds timeout_ns, stops retrying and returns ASX_E_TIMED_OUT.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_combinator_poll_fn poll_fn;
    void *user_data;
    uint32_t max_retries;
    uint32_t attempt;
    asx_status last_error;
    int done;
    asx_status result;
    asx_deadline deadline;
    uint64_t timeout_ns;
    int deadline_initialized;
    asx_combinator_branch inner;
} asx_retry_timeout_state;

/* Initialize a retry-with-timeout combinator. */
ASX_API asx_status asx_retry_timeout_init(asx_retry_timeout_state *state,
                                           asx_combinator_poll_fn poll_fn, void *user_data,
                                           uint32_t max_retries, uint64_t timeout_ns);

/* Poll the retry-with-timeout. Returns ASX_E_TIMED_OUT on overall deadline expiry. */
ASX_API asx_status asx_retry_timeout_poll(void *user_data, asx_task_id self);

/* Get the number of attempts made so far. */
ASX_API uint32_t asx_retry_timeout_attempts(const asx_retry_timeout_state *state);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_COMBINATOR_H */
