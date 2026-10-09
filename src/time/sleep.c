/*
 * sleep.c — sleep, timeout, and interval poll primitives
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/time/sleep.h>
#include <stdint.h>
#include <string.h>

/* Wait on the calling task's traced sleep timer for `target` (registered
 * once, fired or cancelled in the trace like a Rust Sleep). Best-effort:
 * outside a scheduler poll (or with an invalid `self`) the primitive
 * degrades to plain re-polling. Callers have checked that `target` is
 * still ahead, so this returns ASX_E_PENDING. */
static asx_status park_until(asx_task_id self, asx_time target) {
    asx_status st = asx_task_wait_until(self, target);
    return st == ASX_OK ? ASX_E_PENDING : st;
}

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

asx_status asx_sleep_init_until(asx_sleep_state *state, asx_time until_ns) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(state, 0, sizeof(*state));
    state->absolute = 1;
    state->until_ns = until_ns;
    return ASX_OK;
}

/* Rust Sleep's cancellation point (time/sleep.rs:788-794): a cancel is
 * observed when one is requested, its kind is neither Timeout nor
 * Deadline, and a checkpoint reports it (an unmasked task); the checkpoint
 * acknowledges it. The kind is read first, so a Timeout or Deadline
 * cancel is not acknowledged here. */
static int sleep_observes_cancel(asx_task_id self) {
    asx_cancel_reason reason;
    asx_checkpoint_result cr;
    if (asx_task_get_cancel_reason(self, &reason) != ASX_OK) return 0;
    if (reason.kind == ASX_CANCEL_TIMEOUT || reason.kind == ASX_CANCEL_DEADLINE) return 0;
    if (asx_checkpoint(self, &cr) != ASX_OK) return 0;
    return cr.cancelled;
}

asx_status asx_sleep_poll(void *user_data, asx_task_id self) {
    asx_sleep_state *s = (asx_sleep_state *)user_data;
    asx_time now;
    asx_status st;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* First poll: fix the deadline. */
    if (!s->initialized) {
        st = s->absolute ? asx_deadline_init(&s->deadline, s->until_ns)
                         : asx_deadline_after(&s->deadline, s->duration_ns);
        if (st != ASX_OK) return st;
        s->initialized = 1;
    }

    if (sleep_observes_cancel(self)) {
        /* First polled after the cancel: one trip through the scheduler
         * (the task stays runnable), so a loop of fresh sleeps cannot spin
         * inside one poll. A sleep already waiting completes at once. */
        if (!s->polled) {
            s->polled = 1;
            return ASX_E_PENDING;
        }
        st = asx_task_cancel_timer(self);
        (void)st;
        return ASX_OK;
    }
    s->polled = 1;

    /* Due (or a zero-length sleep): done. Otherwise wait on the timer;
     * a later poll (timer fired or spurious wake) re-checks. */
    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) return st;
    if (asx_deadline_is_expired_at(&s->deadline, now)) { return ASX_OK; }

    return park_until(self, asx_deadline_target(&s->deadline));
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
            return ASX_OK;
        }
        if (inner_st != ASX_E_PENDING) return inner_st;
    }

    /* Inner is pending: guarantee a wake at the deadline without parking
     * (whether to park is the inner poll's decision). */
    {
        asx_status a_st_ = asx_task_arm_timer(self, asx_deadline_target(&s->deadline));
        (void)a_st_;
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

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* First poll or after a tick: set up the next deadline */
    if (!s->initialized) {
        st = asx_deadline_after(&s->deadline, s->period_ns);
        if (st != ASX_OK) return st;

        s->initialized = 1;
        return park_until(self, asx_deadline_target(&s->deadline));
    }

    /* Check if current period has elapsed */
    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) return st;

    if (asx_deadline_is_expired_at(&s->deadline, now)) {
        asx_time next_target;

        s->ticks++;

        /* Check if we've reached max ticks */
        if (s->max_ticks > 0 && s->ticks >= s->max_ticks) return ASX_OK;

        /* Next period. Check overflow first so the old deadline state is
         * preserved on failure. */
        if (s->period_ns > UINT64_MAX - now) return ASX_E_TIMER_DURATION_EXCEEDED;
        next_target = now + s->period_ns;

        st = asx_deadline_init(&s->deadline, next_target);
        if (st != ASX_OK) return st;
    }

    return park_until(self, asx_deadline_target(&s->deadline));
}

uint32_t asx_interval_ticks(const asx_interval_state *state) {
    if (state == NULL) return 0;
    return state->ticks;
}
