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
