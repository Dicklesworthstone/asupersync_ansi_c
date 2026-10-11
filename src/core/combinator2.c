/*
 * combinator2.c — second-wave async control flow combinators
 *
 * SPDX-License-Identifier: MIT
 */

#include "../runtime/runtime_internal.h"
#include <asx/core/combinator2.h>
#include <asx/runtime/runtime.h>
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

/* The owner task's checkpoint: nonzero once it observes a cancel (a mask
 * defers it). A poll outside a runtime task never sees one. */
static int owner_cancelled(asx_task_id self) {
    asx_checkpoint_result cp;
    return asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled;
}

/* ------------------------------------------------------------------ */
/* Retry                                                               */
/* ------------------------------------------------------------------ */

asx_retry_policy asx_retry_policy_new(void) {
    asx_retry_policy p;
    p.max_attempts = 3u;
    p.initial_delay_ns = UINT64_C(100000000); /* 100 ms */
    p.max_delay_ns = UINT64_C(30000000000);   /* 30 s */
    p.multiplier = 2.0;
    p.jitter = 0.1;
    return p;
}

asx_retry_policy asx_retry_policy_immediate(uint32_t max_attempts) {
    asx_retry_policy p;
    p.max_attempts = max_attempts > 0u ? max_attempts : 1u;
    p.initial_delay_ns = 0u;
    p.max_delay_ns = 0u;
    p.multiplier = 1.0;
    p.jitter = 0.0;
    return p;
}

/* Nonzero for a finite double (NaN and the infinities give NaN here). */
static int retry_finite(double x) { return x - x == 0.0; }

/* f64::powi as Rust compiles it (compiler-builtins __powidf2): square and
 * multiply over the exponent's bits, the exponent capped at i32::MAX
 * (retry.rs:221). */
static double retry_powi(double a, uint32_t exponent) {
    double r = 1.0;
    uint32_t b = exponent > (uint32_t)INT32_MAX ? (uint32_t)INT32_MAX : exponent;
    for (;;) {
        ASX_CHECKPOINT_WAIVER("bounded: at most 31 exponent bits");
        if ((b & 1u) != 0u) r *= a;
        b >>= 1;
        if (b == 0u) break;
        a *= a;
    }
    return r;
}

/* DetRng::new(seed) and its first next_u64 (util/det_rng.rs:63-68,
 * 129-158, 267-276): seed correction, then one xorshift64 step. */
static uint64_t retry_det_rng_first(uint64_t seed) {
    uint64_t x = seed;
    if (x == 0u) {
        x = 1u;
    } else if (x == UINT64_C(0xFFFFFFFFFFFFFFFF) || x == UINT64_C(0x00000000FFFFFFFF) ||
               x == UINT64_C(0xFFFFFFFF00000000) || x == UINT64_C(0x5555555555555555) ||
               x == UINT64_C(0xAAAAAAAAAAAAAAAA)) {
        x += UINT64_C(0x9E3779B97F4A7C15);
    }
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

uint64_t asx_retry_policy_delay(const asx_retry_policy *policy, uint32_t attempt,
                                const uint64_t *jitter_seed) {
    uint32_t exponent;
    double factor = 1.0;
    int factor_infinite = 0;
    double initial;
    double base;
    double max_nanos;
    double capped;
    double final_nanos;

    if (policy == NULL || attempt == 0u) return 0u;

    /* multiplier^(attempt - 1); an invalid multiplier or an overflow is an
     * infinite factor. */
    exponent = attempt - 1u;
    if (exponent != 0u) {
        if (policy->multiplier <= 0.0 || !retry_finite(policy->multiplier)) {
            factor_infinite = 1;
        } else {
            factor = retry_powi(policy->multiplier, exponent);
            if (!retry_finite(factor)) factor_infinite = 1;
        }
    }

    /* The initial delay as f64: exact up to 2^53 ns, else through
     * Duration::as_secs_f64 (retry.rs:233-238). */
    if (policy->initial_delay_ns <= (UINT64_C(1) << 53)) {
        initial = (double)policy->initial_delay_ns;
    } else {
        initial = ((double)(policy->initial_delay_ns / UINT64_C(1000000000)) +
                   (double)(policy->initial_delay_ns % UINT64_C(1000000000)) / 1000000000.0) *
                  1000000000.0;
    }

    /* A zero initial delay stays zero at every attempt (retry.rs:240-245);
     * an infinite base caps at max_delay. */
    max_nanos = (double)policy->max_delay_ns;
    if (policy->initial_delay_ns == 0u) {
        base = 0.0;
        capped = base < max_nanos ? base : max_nanos;
    } else if (factor_infinite) {
        capped = max_nanos;
    } else {
        base = initial * factor;
        capped = base < max_nanos ? base : max_nanos;
    }

    /* Jitter: capped * (1 + u * jitter), u the DetRng draw scaled to [0, 1]
     * (retry.rs:256-279). */
    final_nanos = capped;
    if (policy->jitter > 0.0 && jitter_seed != NULL) {
        uint64_t r = retry_det_rng_first(*jitter_seed);
        double normalized = r == UINT64_MAX ? 1.0 : (double)r / (double)UINT64_MAX;
        double result = capped * (1.0 + normalized * policy->jitter);
        if (retry_finite(result)) final_nanos = result;
    }

    /* clamp_nanos_f64 (retry.rs:284-294). */
    if (!retry_finite(final_nanos) || final_nanos <= 0.0) return 0u;
    if (final_nanos >= (double)UINT64_MAX) return UINT64_MAX;
    return (uint64_t)final_nanos;
}

/* RetryPolicy::validate (retry.rs:158-170): a NaN multiplier passes it, as
 * in Rust (calculate_delay then caps every retry at max_delay). */
static int retry_policy_valid(const asx_retry_policy *p) {
    if (p->max_attempts == 0u) return 0;
    if (p->multiplier < 1.0) return 0;
    if (!(p->jitter >= 0.0 && p->jitter <= 1.0)) return 0;
    return 1;
}

asx_status asx_retry_init(asx_retry_state *state, asx_combinator_poll_fn poll_fn, void *user_data,
                          const asx_retry_policy *policy) {
    if (state == NULL || poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (policy != NULL && !retry_policy_valid(policy)) return ASX_E_INVALID_ARGUMENT;
    memset(state, 0, sizeof(*state));
    state->poll_fn = poll_fn;
    state->user_data = user_data;
    state->policy = policy != NULL ? *policy : asx_retry_policy_new();
    state->phase = ASX_RETRY_IDLE;
    state->last_error = ASX_OK;
    state->result = ASX_E_PENDING;
    branch_init(&state->inner);
    return ASX_OK;
}

static asx_status retry_finish(asx_retry_state *state, asx_status result) {
    state->phase = ASX_RETRY_DONE;
    state->result = result;
    return result;
}

/* Idle -> the next attempt: RetryState::next_attempt (retry.rs:676-693)
 * with a DetRng seeded from the owner's entropy, drawn on every Idle pass
 * as Rust's Cx::current().map(|c| DetRng::new(c.random_u64())) is (none
 * outside a task). The first attempt has no delay. */
static asx_status retry_start_next(asx_retry_state *state, asx_task_id self) {
    uint64_t seed = 0;
    int have_seed = asx_task_random_u64_internal(self, &seed);
    uint64_t delay;
    asx_time now = 0;

    state->attempt++;
    delay = state->attempt == 1u ? 0u
                                 : asx_retry_policy_delay(&state->policy, state->attempt - 1u,
                                                          have_seed ? &seed : NULL);
    if (delay == 0u) {
        branch_init(&state->inner);
        state->inner.poll_fn = state->poll_fn;
        state->inner.user_data = state->user_data;
        state->phase = ASX_RETRY_POLLING;
        return ASX_OK;
    }
    state->total_delay_ns =
        UINT64_MAX - state->total_delay_ns < delay ? UINT64_MAX : state->total_delay_ns + delay;
    /* Sleep::after(now, delay): the deadline saturates. */
    if (asx_runtime_now_ns(&now) != ASX_OK) now = 0;
    {
        asx_status st =
            asx_sleep_init_until(&state->sleep,
                                 UINT64_MAX - now < delay ? (asx_time)UINT64_MAX : now + delay);
        (void)st; /* state is non-NULL */
    }
    state->phase = ASX_RETRY_SLEEPING;
    return ASX_OK;
}

asx_status asx_retry_poll(void *user_data, asx_task_id self) {
    asx_retry_state *state = (asx_retry_state *)user_data;
    asx_status st;
    int cancelled;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->phase == ASX_RETRY_DONE) return state->result;

    /* Rust's Retry::poll (retry.rs:837-967). */
    for (;;) {
        ASX_CHECKPOINT_WAIVER("bounded: max_attempts attempts, each pass advances the phase");
        /* Every pass checkpoints the owner first, also while an attempt
         * runs (the attempt is not stopped: it observes the cancel itself). */
        cancelled = owner_cancelled(self);
        switch (state->phase) {
        case ASX_RETRY_IDLE:
            if (cancelled) return retry_finish(state, ASX_E_CANCELLED);
            st = retry_start_next(state, self);
            if (st != ASX_OK) return retry_finish(state, st);
            break;
        case ASX_RETRY_SLEEPING:
            if (cancelled) {
                /* The abandoned Sleep cancels its timer (dropping it). */
                st = asx_task_cancel_timer(self);
                (void)st;
                return retry_finish(state, ASX_E_CANCELLED);
            }
            st = asx_sleep_poll(&state->sleep, self);
            if (st == ASX_E_PENDING) return ASX_E_PENDING;
            if (st != ASX_OK) return retry_finish(state, st);
            /* A Sleep also completes early on a cancel of its task: that
             * must not start another attempt (retry.rs:899-907). */
            if (owner_cancelled(self)) return retry_finish(state, ASX_E_CANCELLED);
            branch_init(&state->inner);
            state->inner.poll_fn = state->poll_fn;
            state->inner.user_data = state->user_data;
            state->phase = ASX_RETRY_POLLING;
            break;
        case ASX_RETRY_POLLING:
            st = branch_poll(&state->inner, self);
            if (st == ASX_E_PENDING) return ASX_E_PENDING;
            if (st == ASX_OK || st == ASX_E_CANCELLED) return retry_finish(state, st);
            /* Err: retried while attempts remain (AlwaysRetry). */
            state->last_error = st;
            if (state->attempt >= state->policy.max_attempts) return retry_finish(state, st);
            state->phase = ASX_RETRY_IDLE;
            break;
        case ASX_RETRY_DONE: return state->result;
        }
    }
}

uint32_t asx_retry_attempts(const asx_retry_state *state) {
    if (state == NULL) return 0;
    return state->attempt;
}

uint64_t asx_retry_total_delay_ns(const asx_retry_state *state) {
    if (state == NULL) return 0;
    return state->total_delay_ns;
}

asx_status asx_retry_last_error(const asx_retry_state *state) {
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    return state->last_error;
}

/* ------------------------------------------------------------------ */
/* Retry with timeout                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_retry_timeout_init(asx_retry_timeout_state *state, asx_combinator_poll_fn poll_fn,
                                  void *user_data, const asx_retry_policy *policy,
                                  uint64_t timeout_ns) {
    asx_status st;
    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_retry_init(&state->retry, poll_fn, user_data, policy);
    if (st != ASX_OK) return st;
    memset(&state->deadline, 0, sizeof(state->deadline));
    state->timeout_ns = timeout_ns;
    state->deadline_initialized = 0;
    state->done = 0;
    state->result = ASX_E_PENDING;
    return ASX_OK;
}

static asx_status retry_timeout_finish(asx_retry_timeout_state *state, asx_status result) {
    asx_status st = asx_deadline_disarm(&state->deadline);
    (void)st;
    state->done = 1;
    state->result = result;
    return result;
}

asx_status asx_retry_timeout_poll(void *user_data, asx_task_id self) {
    asx_retry_timeout_state *state = (asx_retry_timeout_state *)user_data;
    asx_status st;
    asx_time target;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->done) return state->result;

    /* The deadline is fixed at the first poll. */
    if (!state->deadline_initialized) {
        st = asx_deadline_after(&state->deadline, state->timeout_ns);
        if (st != ASX_OK) memset(&state->deadline, 0, sizeof(state->deadline));
        state->deadline_initialized = 1;
    }

    /* Past it, the retry is abandoned without another poll (Rust's timeout
     * drops it): a pending delay's sleep cancels its timer. */
    if (asx_deadline_is_expired(&state->deadline)) {
        if (state->retry.phase == ASX_RETRY_SLEEPING) {
            st = asx_task_cancel_timer(self);
            (void)st;
        }
        state->retry.phase = ASX_RETRY_DONE;
        state->retry.result = ASX_E_TIMED_OUT;
        return retry_timeout_finish(state, ASX_E_TIMED_OUT);
    }

    st = asx_retry_poll(&state->retry, self);
    if (st != ASX_E_PENDING) return retry_timeout_finish(state, st);

    /* Still pending: wake the owner at the deadline, as the timeout's
     * Sleep does, so it fires while the retry is parked. */
    target = asx_deadline_target(&state->deadline);
    if (target != 0u) {
        st = asx_task_arm_timer(self, target);
        (void)st; /* no runtime task: nothing to wake */
    }
    return ASX_E_PENDING;
}

uint32_t asx_retry_timeout_attempts(const asx_retry_timeout_state *state) {
    if (state == NULL) return 0;
    return state->retry.attempt;
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
            /* Acquire failed: no cleanup, and every later poll reports the
             * same error (before, a re-poll returned ASX_OK). */
            state->use_result = st;
            state->phase = ASX_BRACKET_PHASE_DONE;
            state->done = 1;
            return st;
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

    /* Rust's pipeline! awaits each stage in turn in one async block
     * (combinator/pipeline.rs:1770-1782): a stage that completes starts
     * the next in the same poll; a failed stage stops the chain. */
    for (;;) {
        /* ASX_CHECKPOINT_WAIVER("bounded: stage_count <= ASX_PIPELINE_MAX_STAGES") */
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
    }
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
                          void *primary_data, asx_combinator_poll_fn backup_fn, void *backup_data,
                          uint64_t deadline_ns) {
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

/* The caller was cancelled: both attempts are dropped, never polled again,
 * and the hedge ends ASX_E_CANCELLED with the primary as the deterministic
 * representative, as Rust's finish_cancelled (combinator/hedge.rs:637-658). */
static asx_status hedge_cancelled(asx_hedge_state *state) {
    asx_status dd = asx_deadline_disarm(&state->deadline);
    (void)dd;
    state->phase = ASX_HEDGE_DECIDED;
    state->winner = 0;
    state->result = ASX_E_CANCELLED;
    return ASX_E_CANCELLED;
}

asx_status asx_hedge_poll(void *user_data, asx_task_id self) {
    asx_hedge_state *state = (asx_hedge_state *)user_data;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->phase == ASX_HEDGE_DECIDED) return state->result;

    /* Rust's HedgeFuture::poll (combinator/hedge.rs:682-783) checks the
     * caller's cancel first, again before it starts the backup, and again
     * before it returns Pending: a cancel drops both attempts. */
    if (owner_cancelled(self)) return hedge_cancelled(state);

    if (state->phase == ASX_HEDGE_PRIMARY_ONLY) {
        /* Arm deadline on first poll */
        if (!state->deadline_armed) {
            asx_status dst = asx_deadline_after(&state->deadline, state->deadline_ns);
            if (dst == ASX_OK) {
                dst = asx_deadline_arm(&state->deadline, NULL);
                if (dst == ASX_OK) { state->deadline_armed = 1; }
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
            {
                asx_status dd_ = asx_deadline_disarm(&state->deadline);
                (void)dd_;
            }
            return st;
        }

        /* Check deadline */
        if (asx_deadline_is_expired(&state->deadline)) {
            {
                asx_status dd_ = asx_deadline_disarm(&state->deadline);
                (void)dd_;
            }
            if (owner_cancelled(self)) return hedge_cancelled(state);
            state->phase = ASX_HEDGE_RACING;
            state->backup.poll_fn = state->backup_fn;
            state->backup.user_data = state->backup_data;
            /* The primary was polled in this poll already: the backup
             * runs next (hedge.rs:731-765). */
            goto poll_backup;
        } else {
            /* The hedge delay wakes the owner, as the Sleep of Rust's hedge
             * does, so the backup starts on time even while the primary is
             * parked. */
            asx_status wake_st = asx_task_arm_timer(self, asx_deadline_target(&state->deadline));
            (void)wake_st; /* no runtime task: nothing to wake */
            if (owner_cancelled(self)) return hedge_cancelled(state);
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

poll_backup:
    if (!state->backup.done) {
        st = branch_poll(&state->backup, self);
        if (st != ASX_E_PENDING) {
            state->phase = ASX_HEDGE_DECIDED;
            state->winner = 1;
            state->result = st;
            return st;
        }
    }

    if (owner_cancelled(self)) return hedge_cancelled(state);
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
    for (i = 0; i < ASX_ADAPTIVE_HEDGE_WINDOW; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded init over static array") */
        policy->history[i] = 0;
    }
    policy->count = 0;
    policy->alpha_pct = (alpha_pct > 0 && alpha_pct < 100) ? alpha_pct : 5u;
    policy->min_delay_ns = min_delay_ns;
    policy->max_delay_ns = max_delay_ns;
}

void asx_adaptive_hedge_record(asx_adaptive_hedge_policy *policy, uint64_t latency_ns) {
    if (policy == NULL) return;
    policy->history[policy->count % ASX_ADAPTIVE_HEDGE_WINDOW] = latency_ns;
    /* Wrap-around after UINT32_MAX causes brief recalibration (10 records).
     * Acceptable tradeoff for zero-allocation circular buffer simplicity. */
    policy->count++;
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
    for (i = 0; i < n; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded init over static array") */
        sorted[i] = policy->history[i];
    }

    /* Insertion sort — small N, deterministic */
    for (i = 1; i < n; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        uint64_t tmp = sorted[i];
        j = i;
        while (j > 0 && sorted[j - 1] > tmp) {
            /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
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

    /* Poll the incomplete branches in index order. The first failure stops
     * the map-reduce (Rust's executing map-reduce stops admission and
     * drains the rest, map_reduce.rs:7-11): the branches still pending are
     * dropped, and that failure is the result, without a reduce. */
    completed = 0;
    for (i = 0; i < state->count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded poll loop") */
        if (!state->branches[i].done) {
            asx_status st = branch_poll(&state->branches[i], self);
            if (st != ASX_E_PENDING && st != ASX_OK) {
                uint32_t j;
                for (j = 0; j < state->count; j++) {
                    /* ASX_CHECKPOINT_WAIVER("bounded drop loop") */
                    if (!state->branches[j].done) {
                        state->branches[j].done = 1;
                        state->branches[j].result = ASX_E_CANCELLED;
                        state->dropped++;
                    }
                }
                state->result = st;
                state->done = 1;
                return st;
            }
        }
        if (state->branches[i].done) completed++;
    }

    if (completed < state->count) return ASX_E_PENDING;

    /* All map tasks done — collect results and reduce */
    for (i = 0; i < state->count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded aggregation") */
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
        /* ASX_CHECKPOINT_WAIVER("bounded aggregation") */
        if (state->branches[i].done) count++;
    }
    return count - state->dropped;
}
