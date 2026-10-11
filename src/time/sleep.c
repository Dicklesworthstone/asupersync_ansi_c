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
    if (asx_deadline_is_expired_at(&s->deadline, now)) {
        /* Completed at its deadline: the sleep records the fire (Rust
         * complete_ready_registration, sleep.rs:677). */
        st = asx_task_complete_timer(self);
        (void)st;
        return ASX_OK;
    }

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

    /* A repoll after completion fails closed (TimeoutFuture::poll). */
    if (s->completed) return ASX_E_TIMED_OUT;

    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) return st;

    /* Past the deadline: timed out without polling the inner function. */
    if (now > asx_deadline_target(&s->deadline)) {
        (void)asx_deadline_is_expired_at(&s->deadline, now); /* latches expired */
        s->completed = 1;
        return ASX_E_TIMED_OUT;
    }

    /* Completed work wins at the deadline exactly: poll it first. */
    inner_st = s->inner_poll(s->inner_data, self);
    if (inner_st != ASX_E_PENDING) {
        s->completed = 1;
        if (inner_st == ASX_OK) s->inner_done = 1;
        return inner_st;
    }

    /* Still pending at the deadline: timed out. */
    if (asx_deadline_is_expired_at(&s->deadline, now)) {
        s->completed = 1;
        return ASX_E_TIMED_OUT;
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
    state->missed_tick_behavior = ASX_MISSED_TICK_BURST;
    return ASX_OK;
}

asx_status asx_interval_init_at(asx_interval_state *state, asx_time start_ns, uint64_t period_ns,
                                uint32_t max_ticks) {
    asx_status st = asx_interval_init(state, period_ns, max_ticks);
    if (st != ASX_OK) return st;
    state->has_start = 1;
    state->start_ns = start_ns;
    return ASX_OK;
}

asx_status asx_interval_set_missed_tick_behavior(asx_interval_state *state,
                                                 asx_missed_tick_behavior behavior) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((int)behavior < (int)ASX_MISSED_TICK_BURST || (int)behavior > (int)ASX_MISSED_TICK_SKIP) {
        return ASX_E_INVALID_ARGUMENT;
    }
    state->missed_tick_behavior = behavior;
    return ASX_OK;
}

static asx_time interval_add(asx_time t, uint64_t ns) {
    return ns > UINT64_MAX - t ? UINT64_MAX : t + ns;
}

/* Count the ticks due at `now` from the current deadline on and move the
 * deadline past them (Rust Interval::tick and advance_deadline,
 * time/interval.rs:293-303, :385-420), at most `room` of them. Burst
 * steps a period from the deadline, so the ticks due are counted at once;
 * Delay and Skip leave the next deadline after `now`, so one tick is due.
 * Deadlines saturate: the tick at UINT64_MAX is the last one. */
static uint64_t interval_take_due(asx_interval_state *s, asx_time now, uint64_t room) {
    asx_time d = asx_deadline_target(&s->deadline);
    uint64_t due = 1u;
    asx_time next;
    if (d == UINT64_MAX) {
        s->exhausted = 1;
        return 1u;
    }
    switch (s->missed_tick_behavior) {
    case ASX_MISSED_TICK_DELAY: next = interval_add(now, s->period_ns); break;
    case ASX_MISSED_TICK_SKIP: {
        uint64_t skip = (now - d) / s->period_ns;
        skip = skip == UINT64_MAX ? skip : skip + 1u;
        next = skip > UINT64_MAX / s->period_ns ? UINT64_MAX : interval_add(d, skip * s->period_ns);
        break;
    }
    case ASX_MISSED_TICK_BURST:
    default: {
        uint64_t q = (now - d) / s->period_ns;
        due = q == UINT64_MAX ? q : q + 1u;
        if (due > room) due = room;
        next = due > UINT64_MAX / s->period_ns ? UINT64_MAX : interval_add(d, due * s->period_ns);
        break;
    }
    }
    {
        asx_status d_st_ = asx_deadline_init(&s->deadline, next); /* fails only for NULL */
        (void)d_st_;
    }
    return due;
}

asx_status asx_interval_poll(void *user_data, asx_task_id self) {
    asx_interval_state *s = (asx_interval_state *)user_data;
    asx_time now;
    asx_status st;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->max_ticks > 0u && s->ticks >= s->max_ticks) return ASX_OK; /* completed */

    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) return st;

    /* First poll: the first tick is at the start (Rust Interval::new). */
    if (!s->initialized) {
        st = asx_deadline_init(&s->deadline, s->has_start ? s->start_ns : now);
        if (st != ASX_OK) return st;
        s->initialized = 1;
    }

    /* Every tick due now. Each pass leaves the deadline after `now` or at
     * UINT64_MAX, whose tick ends the interval: at most three passes. */
    while (!s->exhausted && now >= asx_deadline_target(&s->deadline)) {
        uint64_t room = s->max_ticks > 0u ? (uint64_t)(s->max_ticks - s->ticks) : UINT64_MAX;
        uint64_t taken;
        ASX_CHECKPOINT_WAIVER("bounded: each pass passes now or exhausts the interval");
        taken = interval_take_due(s, now, room);
        s->ticks =
            taken > (uint64_t)(UINT32_MAX - s->ticks) ? UINT32_MAX : s->ticks + (uint32_t)taken;
        if (s->max_ticks > 0u && s->ticks >= s->max_ticks) return ASX_OK;
    }
    if (s->exhausted) return ASX_E_TIMER_DURATION_EXCEEDED;

    return park_until(self, asx_deadline_target(&s->deadline));
}

uint32_t asx_interval_ticks(const asx_interval_state *state) {
    if (state == NULL) return 0;
    return state->ticks;
}
