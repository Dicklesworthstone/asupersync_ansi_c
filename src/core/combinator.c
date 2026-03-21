/*
 * combinator.c — structured async control flow combinators
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/combinator.h>
#include <string.h>

/* Suppress warn_unused_result on deadline disarm (fire-and-forget cleanup) */
static void disarm_deadline(asx_deadline *dl) {
    asx_status st_ = asx_deadline_disarm(dl);
    (void)st_;
}

/* ------------------------------------------------------------------ */
/* Branch helpers                                                      */
/* ------------------------------------------------------------------ */

static void branch_init(asx_combinator_branch *b) {
    b->poll_fn = NULL;
    b->user_data = NULL;
    b->result = ASX_E_PENDING;
    b->done = 0;
}

static asx_status branch_poll(asx_combinator_branch *b, asx_task_id self) {
    asx_status st;
    if (b->done) return b->result;
    if (b->poll_fn == NULL) {
        b->done = 1;
        b->result = ASX_E_INVALID_STATE;
        return b->result;
    }
    st = b->poll_fn(b->user_data, self);
    if (st != ASX_E_PENDING) {
        b->done = 1;
        b->result = st;
    }
    return st;
}

static void branch_cancel_after_final_poll(asx_combinator_branch *b, asx_task_id self) {
    if (b->done) return;

    if (b->poll_fn != NULL) { (void)b->poll_fn(b->user_data, self); }

    b->done = 1;
    b->result = ASX_E_CANCELLED;
}

/* ------------------------------------------------------------------ */
/* Join — wait for ALL                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_join_init(asx_join_state *state) {
    uint32_t i;
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    state->count = 0;
    state->done_count = 0;
    state->combined = asx_outcome_make(ASX_OUTCOME_OK);
    for (i = 0; i < ASX_COMBINATOR_MAX_BRANCHES; i++) branch_init(&state->branches[i]);
    return ASX_OK;
}

asx_status asx_join_add(asx_join_state *state, asx_combinator_poll_fn poll_fn, void *user_data) {
    if (state == NULL || poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->count >= ASX_COMBINATOR_MAX_BRANCHES) return ASX_E_RESOURCE_EXHAUSTED;
    state->branches[state->count].poll_fn = poll_fn;
    state->branches[state->count].user_data = user_data;
    state->count++;
    return ASX_OK;
}

asx_status asx_join_poll(void *user_data, asx_task_id self) {
    asx_join_state *state = (asx_join_state *)user_data;
    uint32_t i;
    asx_status st;
    asx_outcome branch_outcome;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->count == 0) return ASX_OK;

    for (i = 0; i < state->count; i++) {
        if (state->branches[i].done) continue;
        st = branch_poll(&state->branches[i], self);
        if (st != ASX_E_PENDING) {
            state->done_count++;
            branch_outcome = asx_outcome_make((st == ASX_OK) ? ASX_OUTCOME_OK : ASX_OUTCOME_ERR);
            state->combined = asx_outcome_join(&state->combined, &branch_outcome);
        }
    }

    if (state->done_count >= state->count) return ASX_OK;

    return ASX_E_PENDING;
}

asx_outcome asx_join_outcome(const asx_join_state *state) {
    if (state == NULL) return asx_outcome_make(ASX_OUTCOME_ERR);
    return state->combined;
}

/* ------------------------------------------------------------------ */
/* Race — first completes, losers cancelled                            */
/* ------------------------------------------------------------------ */

asx_status asx_race_init(asx_race_state *state) {
    uint32_t i;
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    state->count = 0;
    state->winner = -1;
    state->draining = 0;
    state->drained = 0;
    for (i = 0; i < ASX_COMBINATOR_MAX_BRANCHES; i++) branch_init(&state->branches[i]);
    return ASX_OK;
}

asx_status asx_race_add(asx_race_state *state, asx_combinator_poll_fn poll_fn, void *user_data) {
    if (state == NULL || poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->count >= ASX_COMBINATOR_MAX_BRANCHES) return ASX_E_RESOURCE_EXHAUSTED;
    state->branches[state->count].poll_fn = poll_fn;
    state->branches[state->count].user_data = user_data;
    state->count++;
    return ASX_OK;
}

asx_status asx_race_poll(void *user_data, asx_task_id self) {
    asx_race_state *state = (asx_race_state *)user_data;
    uint32_t i;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->count == 0) return ASX_OK;

    /* Phase 1: poll until we find a winner */
    if (state->winner < 0) {
        for (i = 0; i < state->count; i++) {
            if (state->branches[i].done) continue;
            st = branch_poll(&state->branches[i], self);
            if (st != ASX_E_PENDING) {
                state->winner = (int32_t)i;
                state->draining = 1;
                break;
            }
        }
        if (state->winner < 0) return ASX_E_PENDING;
    }

    /* Phase 2: drain losers with one final cooperative poll before
     * resolving them as cancelled. */
    if (state->draining) {
        for (i = 0; i < state->count; i++) {
            if ((int32_t)i == state->winner) continue;
            if (!state->branches[i].done) {
                branch_cancel_after_final_poll(&state->branches[i], self);
                state->drained++;
            }
        }
        state->draining = 0;
    }

    return state->branches[state->winner].result;
}

int32_t asx_race_winner(const asx_race_state *state) {
    if (state == NULL) return -1;
    return state->winner;
}

asx_status asx_race_winner_result(const asx_race_state *state) {
    if (state == NULL || state->winner < 0) return ASX_E_INVALID_STATE;
    return state->branches[state->winner].result;
}

/* ------------------------------------------------------------------ */
/* Select — round-robin fairness                                       */
/* ------------------------------------------------------------------ */

asx_status asx_select_init(asx_select_state *state) {
    uint32_t i;
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    state->count = 0;
    state->winner = -1;
    state->poll_offset = 0;
    state->draining = 0;
    state->drained = 0;
    for (i = 0; i < ASX_COMBINATOR_MAX_BRANCHES; i++) branch_init(&state->branches[i]);
    return ASX_OK;
}

asx_status asx_select_add(asx_select_state *state, asx_combinator_poll_fn poll_fn,
                          void *user_data) {
    if (state == NULL || poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->count >= ASX_COMBINATOR_MAX_BRANCHES) return ASX_E_RESOURCE_EXHAUSTED;
    state->branches[state->count].poll_fn = poll_fn;
    state->branches[state->count].user_data = user_data;
    state->count++;
    return ASX_OK;
}

asx_status asx_select_poll(void *user_data, asx_task_id self) {
    asx_select_state *state = (asx_select_state *)user_data;
    uint32_t i, idx;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->count == 0) return ASX_OK;

    /* Phase 1: round-robin poll for winner */
    if (state->winner < 0) {
        for (i = 0; i < state->count; i++) {
            idx = (state->poll_offset + i) % state->count;
            if (state->branches[idx].done) continue;
            st = branch_poll(&state->branches[idx], self);
            if (st != ASX_E_PENDING) {
                state->winner = (int32_t)idx;
                state->draining = 1;
                break;
            }
        }
        if (state->winner < 0) {
            state->poll_offset = (state->poll_offset + 1) % state->count;
            return ASX_E_PENDING;
        }
    }

    /* Phase 2: drain losers with one final cooperative poll before
     * resolving them as cancelled. */
    if (state->draining) {
        for (i = 0; i < state->count; i++) {
            if ((int32_t)i == state->winner) continue;
            if (!state->branches[i].done) {
                branch_cancel_after_final_poll(&state->branches[i], self);
                state->drained++;
            }
        }
        state->draining = 0;
    }

    return state->branches[state->winner].result;
}

int32_t asx_select_winner(const asx_select_state *state) {
    if (state == NULL) return -1;
    return state->winner;
}

/* ------------------------------------------------------------------ */
/* Timeout combinator                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_timeout_combinator_init(asx_timeout_combinator_state *state,
                                       asx_combinator_poll_fn inner_poll, void *inner_data,
                                       uint64_t timeout_ns) {
    if (state == NULL || inner_poll == NULL) return ASX_E_INVALID_ARGUMENT;
    branch_init(&state->inner);
    state->inner.poll_fn = inner_poll;
    state->inner.user_data = inner_data;
    state->timeout_ns = timeout_ns;
    state->initialized = 0;
    state->timed_out = 0;
    return ASX_OK;
}

asx_status asx_timeout_combinator_poll(void *user_data, asx_task_id self) {
    asx_timeout_combinator_state *state = (asx_timeout_combinator_state *)user_data;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Lazy init: register deadline on first poll */
    if (!state->initialized) {
        st = asx_deadline_after(&state->deadline, state->timeout_ns);
        if (st != ASX_OK) return st;
        state->initialized = 1;
    }

    /* Check deadline first */
    if (asx_deadline_is_expired(&state->deadline)) {
        state->timed_out = 1;
        state->inner.done = 1;
        state->inner.result = ASX_E_TIMED_OUT;
        disarm_deadline(&state->deadline);
        return ASX_E_TIMED_OUT;
    }

    /* Poll inner */
    st = branch_poll(&state->inner, self);
    if (st != ASX_E_PENDING) {
        disarm_deadline(&state->deadline);
        return st;
    }

    /* Re-check deadline after inner poll */
    if (asx_deadline_is_expired(&state->deadline)) {
        state->timed_out = 1;
        state->inner.done = 1;
        state->inner.result = ASX_E_TIMED_OUT;
        disarm_deadline(&state->deadline);
        return ASX_E_TIMED_OUT;
    }

    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* FirstOk — first successful branch wins                              */
/* ------------------------------------------------------------------ */

asx_status asx_first_ok_init(asx_first_ok_state *state) {
    uint32_t i;
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    state->count = 0;
    state->current = 0;
    state->winner = -1;
    state->last_error = ASX_E_INVALID_STATE;
    for (i = 0; i < ASX_COMBINATOR_MAX_BRANCHES; i++) branch_init(&state->branches[i]);
    return ASX_OK;
}

asx_status asx_first_ok_add(asx_first_ok_state *state, asx_combinator_poll_fn poll_fn,
                            void *user_data) {
    if (state == NULL || poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->count >= ASX_COMBINATOR_MAX_BRANCHES) return ASX_E_RESOURCE_EXHAUSTED;
    state->branches[state->count].poll_fn = poll_fn;
    state->branches[state->count].user_data = user_data;
    state->count++;
    return ASX_OK;
}

asx_status asx_first_ok_poll(void *user_data, asx_task_id self) {
    asx_first_ok_state *state = (asx_first_ok_state *)user_data;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->count == 0) return ASX_E_INVALID_STATE;

    while (state->current < state->count) {
        st = branch_poll(&state->branches[state->current], self);
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        if (st == ASX_OK) {
            state->winner = (int32_t)state->current;
            return ASX_OK;
        }
        /* Branch failed — record error and move to next */
        state->last_error = st;
        state->current++;
    }

    /* All branches failed */
    return state->last_error;
}

int32_t asx_first_ok_winner(const asx_first_ok_state *state) {
    if (state == NULL) return -1;
    return state->winner;
}

/* ------------------------------------------------------------------ */
/* Quorum — N-of-M                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_quorum_init(asx_quorum_state *state, uint32_t threshold) {
    uint32_t i;
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (threshold == 0) return ASX_E_INVALID_ARGUMENT;
    state->count = 0;
    state->threshold = threshold;
    state->ok_count = 0;
    state->fail_count = 0;
    state->decided = 0;
    state->draining = 0;
    state->drained = 0;
    for (i = 0; i < ASX_COMBINATOR_MAX_BRANCHES; i++) branch_init(&state->branches[i]);
    return ASX_OK;
}

asx_status asx_quorum_add(asx_quorum_state *state, asx_combinator_poll_fn poll_fn,
                          void *user_data) {
    if (state == NULL || poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->count >= ASX_COMBINATOR_MAX_BRANCHES) return ASX_E_RESOURCE_EXHAUSTED;
    state->branches[state->count].poll_fn = poll_fn;
    state->branches[state->count].user_data = user_data;
    state->count++;
    return ASX_OK;
}

asx_status asx_quorum_poll(void *user_data, asx_task_id self) {
    asx_quorum_state *state = (asx_quorum_state *)user_data;
    uint32_t i;
    asx_status st;
    uint32_t remaining;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->count == 0) return ASX_E_INVALID_ARGUMENT;

    /* Phase 1: poll until decided */
    if (!state->decided) {
        for (i = 0; i < state->count; i++) {
            if (state->branches[i].done) continue;
            st = branch_poll(&state->branches[i], self);
            if (st == ASX_E_PENDING) continue;
            if (st == ASX_OK) {
                state->ok_count++;
            } else {
                state->fail_count++;
            }
        }

        /* Check if quorum reached */
        if (state->ok_count >= state->threshold) {
            state->decided = 1;
            state->draining = 1;
        }

        /* Check if quorum impossible */
        remaining = state->count - state->ok_count - state->fail_count;
        if (state->ok_count + remaining < state->threshold) {
            state->decided = 1;
            state->draining = 1;
        }
    }

    /* Phase 2: drain remaining branches with one final cooperative poll,
     * consistent with race and select drain semantics. */
    if (state->draining) {
        for (i = 0; i < state->count; i++) {
            if (!state->branches[i].done) {
                branch_cancel_after_final_poll(&state->branches[i], self);
                state->drained++;
            }
        }
        state->draining = 0;
    }

    if (!state->decided) return ASX_E_PENDING;

    return (state->ok_count >= state->threshold) ? ASX_OK : ASX_E_INVALID_STATE;
}

uint32_t asx_quorum_ok_count(const asx_quorum_state *state) {
    if (state == NULL) return 0;
    return state->ok_count;
}

uint32_t asx_quorum_fail_count(const asx_quorum_state *state) {
    if (state == NULL) return 0;
    return state->fail_count;
}

/* ------------------------------------------------------------------ */
/* Race with timeout                                                   */
/* ------------------------------------------------------------------ */

asx_status asx_race_timeout_init(asx_race_timeout_state *state, uint64_t timeout_ns) {
    asx_status st;
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_race_init(&state->race);
    if (st != ASX_OK) return st;
    state->timeout_ns = timeout_ns;
    state->initialized = 0;
    state->timed_out = 0;
    return ASX_OK;
}

asx_status asx_race_timeout_add(asx_race_timeout_state *state,
                                 asx_combinator_poll_fn poll_fn, void *user_data) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_race_add(&state->race, poll_fn, user_data);
}

asx_status asx_race_timeout_poll(void *user_data, asx_task_id self) {
    asx_race_timeout_state *state = (asx_race_timeout_state *)user_data;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Already timed out — return cached result without re-draining */
    if (state->timed_out) return ASX_E_TIMED_OUT;

    /* Lazy deadline init on first poll */
    if (!state->initialized) {
        st = asx_deadline_after(&state->deadline, state->timeout_ns);
        if (st != ASX_OK) {
            /* If clock unavailable, skip timeout checking */
            memset(&state->deadline, 0, sizeof(state->deadline));
        }
        state->initialized = 1;
    }

    /* Check timeout before polling */
    if (asx_deadline_is_expired(&state->deadline)) {
        uint32_t i;
        state->timed_out = 1;
        /* Drain all pending branches */
        for (i = 0; i < state->race.count; i++) {
            branch_cancel_after_final_poll(&state->race.branches[i], self);
        }
        return ASX_E_TIMED_OUT;
    }

    /* Delegate to normal race */
    st = asx_race_poll(&state->race, self);

    /* If race found a winner, disarm deadline and return winner result */
    if (st != ASX_E_PENDING) {
        disarm_deadline(&state->deadline);
        return st;
    }

    /* Check timeout after polling (only if race is still pending) */
    if (asx_deadline_is_expired(&state->deadline)) {
        uint32_t i;
        state->timed_out = 1;
        for (i = 0; i < state->race.count; i++) {
            branch_cancel_after_final_poll(&state->race.branches[i], self);
        }
        disarm_deadline(&state->deadline);
        return ASX_E_TIMED_OUT;
    }

    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* Retry with timeout                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_retry_timeout_init(asx_retry_timeout_state *state,
                                   asx_combinator_poll_fn poll_fn, void *user_data,
                                   uint32_t max_retries, uint64_t timeout_ns) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;

    state->poll_fn = poll_fn;
    state->user_data = user_data;
    state->max_retries = max_retries;
    state->attempt = 0;
    state->last_error = ASX_E_PENDING;
    state->done = 0;
    state->result = ASX_E_PENDING;
    state->timeout_ns = timeout_ns;
    state->deadline_initialized = 0;

    state->inner.poll_fn = poll_fn;
    state->inner.user_data = user_data;
    state->inner.result = ASX_E_PENDING;
    state->inner.done = 0;

    return ASX_OK;
}

asx_status asx_retry_timeout_poll(void *user_data, asx_task_id self) {
    asx_retry_timeout_state *state = (asx_retry_timeout_state *)user_data;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->done) return state->result;

    /* Lazy deadline init on first poll */
    if (!state->deadline_initialized) {
        st = asx_deadline_after(&state->deadline, state->timeout_ns);
        if (st != ASX_OK) {
            memset(&state->deadline, 0, sizeof(state->deadline));
        }
        state->deadline_initialized = 1;
    }

    /* Check overall timeout */
    if (asx_deadline_is_expired(&state->deadline)) {
        state->done = 1;
        state->result = ASX_E_TIMED_OUT;
        disarm_deadline(&state->deadline);
        return ASX_E_TIMED_OUT;
    }

    /* Poll inner */
    st = branch_poll(&state->inner, self);

    if (st == ASX_E_PENDING) return ASX_E_PENDING;

    if (st == ASX_OK) {
        state->done = 1;
        state->result = ASX_OK;
        state->attempt++;
        disarm_deadline(&state->deadline);
        return ASX_OK;
    }

    /* Failure — try retry */
    state->last_error = st;
    state->attempt++;

    if (state->attempt > state->max_retries) {
        state->done = 1;
        state->result = state->last_error;
        disarm_deadline(&state->deadline);
        return state->last_error;
    }

    /* Check timeout before retrying */
    if (asx_deadline_is_expired(&state->deadline)) {
        state->done = 1;
        state->result = ASX_E_TIMED_OUT;
        disarm_deadline(&state->deadline);
        return ASX_E_TIMED_OUT;
    }

    /* Reset inner for next attempt */
    state->inner.poll_fn = state->poll_fn;
    state->inner.user_data = state->user_data;
    state->inner.result = ASX_E_PENDING;
    state->inner.done = 0;

    return ASX_E_PENDING;
}

uint32_t asx_retry_timeout_attempts(const asx_retry_timeout_state *state) {
    if (state == NULL) return 0;
    return state->attempt;
}
