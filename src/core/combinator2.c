/*
 * combinator2.c — second-wave async control flow combinators
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/combinator2.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Branch helper (reused from combinator.c pattern)                    */
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

/* ------------------------------------------------------------------ */
/* Retry                                                               */
/* ------------------------------------------------------------------ */

asx_status asx_retry_init(asx_retry_state *state, asx_combinator_poll_fn poll_fn, void *user_data,
                          uint32_t max_retries) {
    if (state == NULL || poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    state->poll_fn = poll_fn;
    state->user_data = user_data;
    state->max_retries = max_retries;
    state->attempt = 0;
    state->last_error = ASX_OK;
    state->done = 0;
    state->result = ASX_E_PENDING;

    /* Set up first attempt */
    branch_init(&state->inner);
    state->inner.poll_fn = poll_fn;
    state->inner.user_data = user_data;
    return ASX_OK;
}

asx_status asx_retry_poll(void *user_data, asx_task_id self) {
    asx_retry_state *state = (asx_retry_state *)user_data;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->done) return state->result;

    st = branch_poll(&state->inner, self);

    if (st == ASX_E_PENDING) return ASX_E_PENDING;

    if (st == ASX_OK) {
        state->done = 1;
        state->result = ASX_OK;
        state->attempt++;
        return ASX_OK;
    }

    /* Failed — try again? */
    state->last_error = st;
    state->attempt++;

    if (state->attempt <= state->max_retries) {
        /* Reset inner for next attempt */
        branch_init(&state->inner);
        state->inner.poll_fn = state->poll_fn;
        state->inner.user_data = state->user_data;
        return ASX_E_PENDING;
    }

    /* Exhausted retries */
    state->done = 1;
    state->result = state->last_error;
    return state->last_error;
}

uint32_t asx_retry_attempts(const asx_retry_state *state) {
    if (state == NULL) return 0;
    return state->attempt;
}

asx_status asx_retry_last_error(const asx_retry_state *state) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    return state->last_error;
}

/* ------------------------------------------------------------------ */
/* Bracket                                                             */
/* ------------------------------------------------------------------ */

asx_status asx_bracket_init(asx_bracket_state *state, asx_bracket_fn acquire_fn,
                            asx_bracket_fn use_fn, asx_bracket_fn release_fn, void *user_data) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (acquire_fn == NULL || use_fn == NULL || release_fn == NULL) return ASX_E_INVALID_ARGUMENT;

    state->acquire_fn = acquire_fn;
    state->use_fn = use_fn;
    state->release_fn = release_fn;
    state->user_data = user_data;
    state->phase = ASX_BRACKET_PHASE_ACQUIRE;
    state->use_result = ASX_OK;
    state->release_result = ASX_OK;
    state->done = 0;
    return ASX_OK;
}

asx_status asx_bracket_poll(void *user_data, asx_task_id self) {
    asx_bracket_state *state = (asx_bracket_state *)user_data;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->done) {
        /* Return the more severe error */
        if (state->use_result != ASX_OK) return state->use_result;
        return state->release_result;
    }

    switch (state->phase) {
    case ASX_BRACKET_PHASE_ACQUIRE:
        st = state->acquire_fn(state->user_data, self);
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        if (st != ASX_OK) {
            state->done = 1;
            return st; /* acquire failed, no cleanup needed */
        }
        state->phase = ASX_BRACKET_PHASE_USE;
        /* fall through to use */
        /* FALLTHROUGH */

    case ASX_BRACKET_PHASE_USE:
        st = state->use_fn(state->user_data, self);
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        state->use_result = st;
        state->phase = ASX_BRACKET_PHASE_RELEASE;
        /* fall through to release */
        /* FALLTHROUGH */

    case ASX_BRACKET_PHASE_RELEASE:
        st = state->release_fn(state->user_data, self);
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        state->release_result = st;
        state->phase = ASX_BRACKET_PHASE_DONE;
        state->done = 1;
        /* Return the use result (errors from use take priority) */
        if (state->use_result != ASX_OK) return state->use_result;
        return state->release_result;

    case ASX_BRACKET_PHASE_DONE:
        if (state->use_result != ASX_OK) return state->use_result;
        return state->release_result;
    }

    return ASX_E_INVALID_STATE;
}

asx_bracket_phase asx_bracket_current_phase(const asx_bracket_state *state) {
    if (state == NULL) return ASX_BRACKET_PHASE_DONE;
    return state->phase;
}

/* ------------------------------------------------------------------ */
/* Pipeline                                                            */
/* ------------------------------------------------------------------ */

asx_status asx_pipeline_init(asx_pipeline_state *state) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(state, 0, sizeof(*state));
    branch_init(&state->active);
    state->last_status = ASX_OK;
    return ASX_OK;
}

asx_status asx_pipeline_add_stage(asx_pipeline_state *state, asx_combinator_poll_fn stage_fn,
                                  void *user_data) {
    if (state == NULL || stage_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->stage_count >= ASX_PIPELINE_MAX_STAGES) return ASX_E_RESOURCE_EXHAUSTED;
    state->stages[state->stage_count] = stage_fn;
    state->stage_data[state->stage_count] = user_data;
    state->stage_count++;
    return ASX_OK;
}

asx_status asx_pipeline_poll(void *user_data, asx_task_id self) {
    asx_pipeline_state *state = (asx_pipeline_state *)user_data;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->done) return state->last_status;
    if (state->stage_count == 0) {
        state->done = 1;
        return ASX_OK;
    }

    /* Initialize first stage if needed */
    if (state->active.poll_fn == NULL && state->current < state->stage_count) {
        state->active.poll_fn = state->stages[state->current];
        state->active.user_data = state->stage_data[state->current];
        state->active.done = 0;
        state->active.result = ASX_E_PENDING;
    }

    st = branch_poll(&state->active, self);
    if (st == ASX_E_PENDING) return ASX_E_PENDING;

    state->last_status = st;

    if (st != ASX_OK) {
        state->done = 1;
        return st;
    }

    state->current++;
    if (state->current >= state->stage_count) {
        state->done = 1;
        return ASX_OK;
    }

    /* Set up next stage */
    branch_init(&state->active);
    state->active.poll_fn = state->stages[state->current];
    state->active.user_data = state->stage_data[state->current];
    return ASX_E_PENDING;
}

uint32_t asx_pipeline_completed_stages(const asx_pipeline_state *state) {
    if (state == NULL) return 0;
    return state->current;
}

/* ------------------------------------------------------------------ */
/* Bulkhead                                                            */
/* ------------------------------------------------------------------ */

asx_status asx_bulkhead_init(asx_bulkhead_state *state, uint32_t max_concurrent) {
    if (state == NULL || max_concurrent == 0) return ASX_E_INVALID_ARGUMENT;
    state->max_concurrent = max_concurrent;
    state->active_count = 0;
    return ASX_OK;
}

asx_status asx_bulkhead_try_enter(asx_bulkhead_state *state) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->active_count >= state->max_concurrent) return ASX_E_WOULD_BLOCK;
    state->active_count++;
    return ASX_OK;
}

asx_status asx_bulkhead_leave(asx_bulkhead_state *state) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->active_count == 0) return ASX_E_INVALID_STATE;
    state->active_count--;
    return ASX_OK;
}

uint32_t asx_bulkhead_active(const asx_bulkhead_state *state) {
    if (state == NULL) return 0;
    return state->active_count;
}

uint32_t asx_bulkhead_remaining(const asx_bulkhead_state *state) {
    if (state == NULL) return 0;
    return state->max_concurrent - state->active_count;
}

/* ------------------------------------------------------------------ */
/* RateLimit                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_rate_limit_init(asx_rate_limit_state *state, uint32_t capacity,
                               uint32_t refill_amount) {
    if (state == NULL || capacity == 0) return ASX_E_INVALID_ARGUMENT;
    state->capacity = capacity;
    state->tokens = capacity; /* start full */
    state->refill_amount = refill_amount;
    return ASX_OK;
}

asx_status asx_rate_limit_try_acquire(asx_rate_limit_state *state) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->tokens == 0) return ASX_E_WOULD_BLOCK;
    state->tokens--;
    return ASX_OK;
}

void asx_rate_limit_refill(asx_rate_limit_state *state) {
    uint32_t new_tokens;
    if (state == NULL) return;
    new_tokens = state->tokens + state->refill_amount;
    if (new_tokens > state->capacity) new_tokens = state->capacity;
    state->tokens = new_tokens;
}

uint32_t asx_rate_limit_available(const asx_rate_limit_state *state) {
    if (state == NULL) return 0;
    return state->tokens;
}

/* ------------------------------------------------------------------ */
/* Hedge                                                               */
/* ------------------------------------------------------------------ */

asx_status asx_hedge_init(asx_hedge_state *state, asx_combinator_poll_fn primary_fn,
                           void *primary_data, asx_combinator_poll_fn backup_fn,
                           void *backup_data, uint64_t deadline_ns) {
    if (state == NULL || primary_fn == NULL || backup_fn == NULL) return ASX_E_INVALID_ARGUMENT;

    branch_init(&state->primary);
    state->primary.poll_fn = primary_fn;
    state->primary.user_data = primary_data;

    branch_init(&state->backup);
    state->backup_fn = backup_fn;
    state->backup_data = backup_data;

    state->deadline_ns = deadline_ns;
    state->deadline_armed = 0;
    state->phase = ASX_HEDGE_PRIMARY_ONLY;
    state->winner = -1;
    state->result = ASX_E_PENDING;
    return ASX_OK;
}

asx_status asx_hedge_poll(void *user_data, asx_task_id self) {
    asx_hedge_state *state = (asx_hedge_state *)user_data;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->phase == ASX_HEDGE_DECIDED) return state->result;

    if (state->phase == ASX_HEDGE_PRIMARY_ONLY) {
        /* Arm deadline on first poll */
        if (!state->deadline_armed) {
            asx_status dst = asx_deadline_after(&state->deadline, state->deadline_ns);
            if (dst == ASX_OK) {
                dst = asx_deadline_arm(&state->deadline, NULL);
                if (dst == ASX_OK) {
                    state->deadline_armed = 1;
                }
            }
            /* If deadline cannot be armed, skip to racing immediately */
            if (!state->deadline_armed) {
                state->phase = ASX_HEDGE_RACING;
                state->backup.poll_fn = state->backup_fn;
                state->backup.user_data = state->backup_data;
                goto racing;
            }
        }

        /* Poll primary */
        st = branch_poll(&state->primary, self);
        if (st != ASX_E_PENDING) {
            /* Primary completed before deadline */
            state->phase = ASX_HEDGE_DECIDED;
            state->winner = 0;
            state->result = st;
            { asx_status dd_ = asx_deadline_disarm(&state->deadline); (void)dd_; }
            return st;
        }

        /* Check deadline */
        if (asx_deadline_is_expired(&state->deadline)) {
            { asx_status dd_ = asx_deadline_disarm(&state->deadline); (void)dd_; }
            state->phase = ASX_HEDGE_RACING;
            state->backup.poll_fn = state->backup_fn;
            state->backup.user_data = state->backup_data;
        } else {
            return ASX_E_PENDING;
        }
    }

racing:
    /* ASX_HEDGE_RACING — poll both branches, first to complete wins */
    if (!state->primary.done) {
        st = branch_poll(&state->primary, self);
        if (st != ASX_E_PENDING) {
            state->phase = ASX_HEDGE_DECIDED;
            state->winner = 0;
            state->result = st;
            return st;
        }
    }

    if (!state->backup.done) {
        st = branch_poll(&state->backup, self);
        if (st != ASX_E_PENDING) {
            state->phase = ASX_HEDGE_DECIDED;
            state->winner = 1;
            state->result = st;
            return st;
        }
    }

    return ASX_E_PENDING;
}

asx_hedge_phase asx_hedge_current_phase(const asx_hedge_state *state) {
    if (state == NULL) return ASX_HEDGE_DECIDED;
    return state->phase;
}

int asx_hedge_winner(const asx_hedge_state *state) {
    if (state == NULL) return -1;
    return state->winner;
}

/* ------------------------------------------------------------------ */
/* Adaptive hedge policy                                               */
/* ------------------------------------------------------------------ */

void asx_adaptive_hedge_init(asx_adaptive_hedge_policy *policy, uint32_t alpha_pct,
                              uint64_t min_delay_ns, uint64_t max_delay_ns) {
    uint32_t i;
    if (policy == NULL) return;
    for (i = 0; i < ASX_ADAPTIVE_HEDGE_WINDOW; i++) policy->history[i] = 0;
    policy->count = 0;
    policy->alpha_pct = (alpha_pct > 0 && alpha_pct < 100) ? alpha_pct : 5u;
    policy->min_delay_ns = min_delay_ns;
    policy->max_delay_ns = max_delay_ns;
}

void asx_adaptive_hedge_record(asx_adaptive_hedge_policy *policy, uint64_t latency_ns) {
    if (policy == NULL) return;
    policy->history[policy->count % ASX_ADAPTIVE_HEDGE_WINDOW] = latency_ns;
    if (policy->count < UINT32_MAX) policy->count++;
}

uint64_t asx_adaptive_hedge_delay(const asx_adaptive_hedge_policy *policy) {
    uint32_t n, rank, i, j;
    uint64_t sorted[ASX_ADAPTIVE_HEDGE_WINDOW];
    uint64_t delay;
    double q;

    if (policy == NULL) return 0;

    n = policy->count < ASX_ADAPTIVE_HEDGE_WINDOW ? policy->count : ASX_ADAPTIVE_HEDGE_WINDOW;
    if (n < 10) return policy->max_delay_ns; /* not enough data */

    /* Copy window into sorted buffer */
    for (i = 0; i < n; i++) sorted[i] = policy->history[i];

    /* Insertion sort — small N, deterministic */
    for (i = 1; i < n; i++) {
        uint64_t tmp = sorted[i];
        j = i;
        while (j > 0 && sorted[j - 1] > tmp) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = tmp;
    }

    /* Conformal rank: ceil((n+1) * (1 - alpha/100)) - 1, clamped to [0, n-1] */
    q = ((double)(n + 1u) * (1.0 - (double)policy->alpha_pct / 100.0));
    if (q < 1.0) {
        rank = 0;
    } else if (q >= (double)n) {
        rank = n - 1u;
    } else {
        /* ceil(q) - 1 */
        uint32_t ceiled = (uint32_t)q;
        if ((double)ceiled < q) ceiled++;
        rank = ceiled > 0 ? ceiled - 1u : 0u;
    }

    delay = sorted[rank];
    if (delay < policy->min_delay_ns) delay = policy->min_delay_ns;
    if (delay > policy->max_delay_ns) delay = policy->max_delay_ns;
    return delay;
}

uint32_t asx_adaptive_hedge_observations(const asx_adaptive_hedge_policy *policy) {
    if (policy == NULL) return 0;
    return policy->count;
}

/* ------------------------------------------------------------------ */
/* MapReduce                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_map_reduce_init(asx_map_reduce_state *state, asx_map_reduce_fn reduce_fn,
                                void *reduce_data) {
    if (state == NULL || reduce_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(state, 0, sizeof(*state));
    state->reduce_fn = reduce_fn;
    state->reduce_data = reduce_data;
    state->result = ASX_E_PENDING;
    return ASX_OK;
}

asx_status asx_map_reduce_add(asx_map_reduce_state *state, asx_combinator_poll_fn map_fn,
                               void *user_data) {
    if (state == NULL || map_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->count >= ASX_COMBINATOR_MAX_BRANCHES) return ASX_E_RESOURCE_EXHAUSTED;
    branch_init(&state->branches[state->count]);
    state->branches[state->count].poll_fn = map_fn;
    state->branches[state->count].user_data = user_data;
    state->count++;
    return ASX_OK;
}

asx_status asx_map_reduce_poll(void *user_data, asx_task_id self) {
    asx_map_reduce_state *state = (asx_map_reduce_state *)user_data;
    uint32_t i;
    uint32_t completed;
    asx_status results[ASX_COMBINATOR_MAX_BRANCHES];

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->done) return state->result;

    /* Poll all incomplete branches */
    completed = 0;
    for (i = 0; i < state->count; i++) {
        if (!state->branches[i].done) {
            (void)branch_poll(&state->branches[i], self);
        }
        if (state->branches[i].done) completed++;
    }

    if (completed < state->count) return ASX_E_PENDING;

    /* All map tasks done — collect results and reduce */
    for (i = 0; i < state->count; i++) {
        results[i] = state->branches[i].result;
    }

    state->result = state->reduce_fn(results, state->count, state->reduce_data);
    state->done = 1;
    return state->result;
}

uint32_t asx_map_reduce_completed(const asx_map_reduce_state *state) {
    uint32_t i, count = 0;
    if (state == NULL) return 0;
    for (i = 0; i < state->count; i++) {
        if (state->branches[i].done) count++;
    }
    return count;
}
