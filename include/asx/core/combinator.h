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

ASX_API asx_status asx_join_init(asx_join_state *state);

ASX_API asx_status asx_join_add(asx_join_state *state, asx_combinator_poll_fn poll_fn,
                                void *user_data);

ASX_API asx_status asx_join_poll(void *user_data, asx_task_id self);

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

ASX_API asx_status asx_race_init(asx_race_state *state);

ASX_API asx_status asx_race_add(asx_race_state *state, asx_combinator_poll_fn poll_fn,
                                void *user_data);

ASX_API asx_status asx_race_poll(void *user_data, asx_task_id self);

ASX_API int32_t asx_race_winner(const asx_race_state *state);

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

ASX_API asx_status asx_select_init(asx_select_state *state);

ASX_API asx_status asx_select_add(asx_select_state *state, asx_combinator_poll_fn poll_fn,
                                  void *user_data);

ASX_API asx_status asx_select_poll(void *user_data, asx_task_id self);

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

ASX_API asx_status asx_timeout_combinator_init(asx_timeout_combinator_state *state,
                                               asx_combinator_poll_fn inner_poll, void *inner_data,
                                               uint64_t timeout_ns);

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

ASX_API asx_status asx_first_ok_init(asx_first_ok_state *state);

ASX_API asx_status asx_first_ok_add(asx_first_ok_state *state, asx_combinator_poll_fn poll_fn,
                                    void *user_data);

ASX_API asx_status asx_first_ok_poll(void *user_data, asx_task_id self);

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

ASX_API asx_status asx_quorum_init(asx_quorum_state *state, uint32_t threshold);

ASX_API asx_status asx_quorum_add(asx_quorum_state *state, asx_combinator_poll_fn poll_fn,
                                  void *user_data);

ASX_API asx_status asx_quorum_poll(void *user_data, asx_task_id self);

ASX_API uint32_t asx_quorum_ok_count(const asx_quorum_state *state);

ASX_API uint32_t asx_quorum_fail_count(const asx_quorum_state *state);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_COMBINATOR_H */
