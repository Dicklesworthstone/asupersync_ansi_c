/*
 * sleep.c — sleep, timeout, and interval poll primitives
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/time/sleep.h>
#include <stdint.h>
#include <string.h>

/* ===================================================================
 * Sleep
 * =================================================================== */

asx_status asx_sleep_init(asx_sleep_state *state, uint64_t duration_ns) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(state, 0, sizeof(*state));
    state->duration_ns = duration_ns;
    state->initialized = 0;
    return ASX_OK;
}

asx_status asx_sleep_poll(void *user_data, asx_task_id self) {
    asx_sleep_state *s = (asx_sleep_state *)user_data;
    asx_time now;
    asx_status st;

    (void)self;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* First poll: compute deadline and arm timer */
    if (!s->initialized) {
        st = asx_deadline_after(&s->deadline, s->duration_ns);
        if (st != ASX_OK) return st;

        st = asx_deadline_arm(&s->deadline, NULL);
        if (st != ASX_OK) return st;

        s->initialized = 1;

        /* Zero-duration sleep completes immediately */
        if (s->duration_ns == 0) { return ASX_OK; }
        return ASX_E_PENDING;
    }

    /* Subsequent polls: check if deadline expired */
    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) return st;

    if (asx_deadline_is_expired_at(&s->deadline, now)) { return ASX_OK; }

    return ASX_E_PENDING;
}

/* ===================================================================
 * Timeout
 * =================================================================== */

asx_status asx_timeout_init(asx_timeout_state *state, uint64_t timeout_ns,
                            asx_task_poll_fn inner_poll, void *inner_data) {
    if (state == NULL || inner_poll == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(state, 0, sizeof(*state));
    state->timeout_ns = timeout_ns;
    state->inner_poll = inner_poll;
    state->inner_data = inner_data;
    state->initialized = 0;
    state->inner_done = 0;
    return ASX_OK;
}

asx_status asx_timeout_poll(void *user_data, asx_task_id self) {
    asx_timeout_state *s = (asx_timeout_state *)user_data;
    asx_time now;
    asx_status st;
    asx_status inner_st;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* First poll: set up the deadline */
    if (!s->initialized) {
        st = asx_deadline_after(&s->deadline, s->timeout_ns);
        if (st != ASX_OK) return st;

        st = asx_deadline_arm(&s->deadline, NULL);
        if (st != ASX_OK) return st;

        s->initialized = 1;
    }

    /* Check deadline first */
    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) return st;

    if (asx_deadline_is_expired_at(&s->deadline, now)) {
        if (!s->inner_done) { return ASX_E_TIMED_OUT; }
        return ASX_OK;
    }

    /* Poll the inner function */
    if (!s->inner_done) {
        inner_st = s->inner_poll(s->inner_data, self);
        if (inner_st == ASX_OK) {
            s->inner_done = 1;
            st = asx_deadline_disarm(&s->deadline);
            (void)st;
            return ASX_OK;
        }
        if (inner_st != ASX_E_PENDING) {
            /* Inner returned an error — disarm deadline and propagate */
            {
                asx_status d_st_ = asx_deadline_disarm(&s->deadline);
                (void)d_st_;
            }
            return inner_st;
        }
    }

    return ASX_E_PENDING;
}

/* ===================================================================
 * Interval
 * =================================================================== */

asx_status asx_interval_init(asx_interval_state *state, uint64_t period_ns, uint32_t max_ticks) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (period_ns == 0) return ASX_E_INVALID_ARGUMENT;

    memset(state, 0, sizeof(*state));
    state->period_ns = period_ns;
    state->max_ticks = max_ticks;
    state->ticks = 0;
    state->initialized = 0;
    return ASX_OK;
}

asx_status asx_interval_poll(void *user_data, asx_task_id self) {
    asx_interval_state *s = (asx_interval_state *)user_data;
    asx_time now;
    asx_status st;

    (void)self;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* First poll or after a tick: set up the next deadline */
    if (!s->initialized) {
        st = asx_deadline_after(&s->deadline, s->period_ns);
        if (st != ASX_OK) return st;

        st = asx_deadline_arm(&s->deadline, NULL);
        if (st != ASX_OK) return st;

        s->initialized = 1;
        return ASX_E_PENDING;
    }

    /* Check if current period has elapsed */
    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) return st;

    if (asx_deadline_is_expired_at(&s->deadline, now)) {
        asx_time next_target;

        s->ticks++;

        /* Check if we've reached max ticks */
        if (s->max_ticks > 0 && s->ticks >= s->max_ticks) {
            {
                asx_status d_st_ = asx_deadline_disarm(&s->deadline);
                (void)d_st_;
            }
            return ASX_OK;
        }

        /* Re-arm for next period. Check overflow first so the old
         * deadline state is preserved on failure. */
        if (s->period_ns > UINT64_MAX - now) return ASX_E_TIMER_DURATION_EXCEEDED;
        next_target = now + s->period_ns;

        /* Disarm old deadline via proper API, then init+arm the new one */
        {
            asx_status d_st_ = asx_deadline_disarm(&s->deadline);
            (void)d_st_;
        }
        st = asx_deadline_init(&s->deadline, next_target);
        if (st != ASX_OK) return st;
        st = asx_deadline_arm(&s->deadline, NULL);
        if (st != ASX_OK) return st;
    }

    return ASX_E_PENDING;
}

uint32_t asx_interval_ticks(const asx_interval_state *state) {
    if (state == NULL) return 0;
    return state->ticks;
}
