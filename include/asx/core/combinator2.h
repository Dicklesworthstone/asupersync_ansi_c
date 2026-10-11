/*
 * asx/core/combinator2.h — second-wave async control flow combinators
 *
 * Provides retry (with backoff, optionally under a deadline), bracket
 * (resource-safe cleanup), pipeline (sequential
 * transform chain), bulkhead (concurrency limiter), and rate_limit
 * (token-bucket throttle) orchestration primitives.
 *
 * All state is inline (no dynamic allocation). Compatible with
 * asx_combinator_poll_fn from combinator.h.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_COMBINATOR2_H
#define ASX_CORE_COMBINATOR2_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/combinator.h>
#include <asx/time/deadline.h>
#include <asx/time/sleep.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Retry — poll inner again after a failure, with backoff
 *
 * Rust's retry(policy, AlwaysRetry, factory) (combinator/retry.rs:
 * 786-967). Every pass first runs the owner's checkpoint. Before each
 * retry the owner sleeps for asx_retry_policy_delay(policy, attempt - 1,
 * seed), the seed a draw from the owner's entropy (its lab stream under
 * lab dispatch, as Rust's Cx::random_u64), on its traced sleep timer
 * (asx_sleep). A cancel seen before an attempt, during the sleep or right
 * after it ends the retry ASX_E_CANCELLED (an abandoned sleep cancels its
 * timer); an attempt already running continues. An attempt that returns
 * ASX_E_CANCELLED is not retried; every other failure is retried while
 * attempts remain. A zero delay starts the next attempt in the same poll.
 * ------------------------------------------------------------------- */

/* Rust's RetryPolicy (retry.rs:47-170). */
typedef struct {
    uint32_t max_attempts;     /* attempts in all, the first included (>= 1) */
    uint64_t initial_delay_ns; /* before the first retry */
    uint64_t max_delay_ns;     /* cap on the exponential growth */
    double multiplier;         /* growth per retry (>= 1.0) */
    double jitter;             /* [0, 1]: up to this fraction of the delay is added */
} asx_retry_policy;

/* RetryPolicy::new(): 3 attempts, 100 ms initial delay, 30 s cap,
 * multiplier 2, jitter 0.1. */
ASX_API asx_retry_policy asx_retry_policy_new(void);

/* RetryPolicy::immediate(max_attempts): no delay between attempts
 * (max_attempts 0 counts as 1). */
ASX_API asx_retry_policy asx_retry_policy_immediate(uint32_t max_attempts);

/* Rust's calculate_delay (retry.rs:201-294): the delay before retry
 * number `attempt` (1 = the first retry; 0 gives 0). initial_delay *
 * multiplier^(attempt - 1), capped at max_delay; with jitter > 0 and a
 * non-NULL jitter_seed, scaled by 1 + jitter * u, u the first draw of a
 * DetRng seeded with *jitter_seed mapped to [0, 1]. Computed in IEEE
 * double as Rust's f64 (powi by square-and-multiply). */
ASX_API uint64_t asx_retry_policy_delay(const asx_retry_policy *policy, uint32_t attempt,
                                        const uint64_t *jitter_seed);

typedef enum {
    ASX_RETRY_IDLE = 0,     /* the next attempt (or its delay) starts on the next pass */
    ASX_RETRY_POLLING = 1,  /* an attempt is running */
    ASX_RETRY_SLEEPING = 2, /* waiting out the delay before the next attempt */
    ASX_RETRY_DONE = 3
} asx_retry_phase;

typedef struct {
    asx_combinator_poll_fn poll_fn;
    void *user_data;
    asx_retry_policy policy;
    asx_retry_phase phase;
    uint32_t attempt;            /* attempt number reached (Rust RetryState::attempt) */
    uint64_t total_delay_ns;     /* delays taken before retries, summed */
    asx_status last_error;       /* last failure status */
    asx_status result;           /* final result (phase DONE) */
    asx_sleep_state sleep;       /* the delay before the next attempt */
    asx_combinator_branch inner; /* current attempt branch */
    /* Releases an attempt abandoned in flight (a retry timeout's deadline
     * passing while it runs), as Rust drops the attempt's future: called
     * once with user_data. NULL after asx_retry_init; set it after. */
    asx_combinator_drop_fn drop_fn;
} asx_retry_state;

/* Initialize a retry of poll_fn under `policy` (NULL: asx_retry_policy_new).
 * Returns ASX_E_INVALID_ARGUMENT for a NULL state or poll_fn, or a policy
 * Rust's RetryPolicy::validate refuses: max_attempts 0, multiplier below
 * 1.0, jitter outside [0, 1] (NaN included). */
ASX_API asx_status asx_retry_init(asx_retry_state *state, asx_combinator_poll_fn poll_fn,
                                  void *user_data, const asx_retry_policy *policy);

/* Poll function — compatible with asx_combinator_poll_fn. */
ASX_API asx_status asx_retry_poll(void *user_data, asx_task_id self);

/* The attempt number reached (Rust RetryState::attempt): an attempt
 * counts once it starts or the delay before it begins, so a retry
 * cancelled during its first delay reports 2 after one attempt ran. */
ASX_API uint32_t asx_retry_attempts(const asx_retry_state *state);

/* The delays taken before retries, summed (Rust RetryError::total_delay). */
ASX_API uint64_t asx_retry_total_delay_ns(const asx_retry_state *state);

/* Get the last error (meaningful if retry exhausted). */
ASX_API asx_status asx_retry_last_error(const asx_retry_state *state);

/* -------------------------------------------------------------------
 * Retry with timeout — a retry under an overall deadline
 *
 * Rust's timeout(retry(...)): the deadline is fixed at the first poll;
 * each poll past it ends the retry ASX_E_TIMED_OUT (an attempt in flight
 * is dropped, its retry.drop_fn called, a pending delay's timer
 * cancelled); before it, the retry
 * is polled and, while pending, the owner is woken at the deadline.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_retry_state retry;
    asx_deadline deadline;
    uint64_t timeout_ns;
    int deadline_initialized;
    int done;
    asx_status result;
} asx_retry_timeout_state;

/* Initialize a retry-with-timeout (policy as asx_retry_init). */
ASX_API asx_status asx_retry_timeout_init(asx_retry_timeout_state *state,
                                          asx_combinator_poll_fn poll_fn, void *user_data,
                                          const asx_retry_policy *policy, uint64_t timeout_ns);

/* Poll the retry-with-timeout. Returns ASX_E_TIMED_OUT on overall deadline expiry. */
ASX_API asx_status asx_retry_timeout_poll(void *user_data, asx_task_id self);

/* The attempt number reached (as asx_retry_attempts). */
ASX_API uint32_t asx_retry_timeout_attempts(const asx_retry_timeout_state *state);

/* -------------------------------------------------------------------
 * Bracket — resource acquisition, use, and guaranteed cleanup
 *
 * Three-phase pattern:
 *   1. acquire_fn: get the resource (ASX_OK or error)
 *   2. use_fn: use the resource (ASX_OK/error/PENDING)
 *   3. release_fn: always called, even on failure (ASX_OK or error)
 *
 * The user_data is passed to all three functions. Release is called
 * regardless of whether use succeeds or fails.
 * ------------------------------------------------------------------- */

typedef asx_status (*asx_bracket_fn)(void *user_data, asx_task_id self);

typedef enum {
    ASX_BRACKET_PHASE_ACQUIRE,
    ASX_BRACKET_PHASE_USE,
    ASX_BRACKET_PHASE_RELEASE,
    ASX_BRACKET_PHASE_DONE
} asx_bracket_phase;

typedef struct {
    asx_bracket_fn acquire_fn;
    asx_bracket_fn use_fn;
    asx_bracket_fn release_fn;
    void *user_data;
    asx_bracket_phase phase;
    asx_status use_result;     /* saved for after release */
    asx_status release_result; /* release outcome */
    int done;
} asx_bracket_state;

/* Initialize a bracket combinator (acquire, use, release). */
ASX_API asx_status asx_bracket_init(asx_bracket_state *state, asx_bracket_fn acquire_fn,
                                    asx_bracket_fn use_fn, asx_bracket_fn release_fn,
                                    void *user_data);

/* Poll function — compatible with asx_combinator_poll_fn. */
ASX_API asx_status asx_bracket_poll(void *user_data, asx_task_id self);

/* Get the phase the bracket is currently in. */
ASX_API asx_bracket_phase asx_bracket_current_phase(const asx_bracket_state *state);

/* -------------------------------------------------------------------
 * Pipeline — sequential chain of transform stages
 *
 * Each stage is polled in order. When stage N completes with ASX_OK,
 * stage N+1 begins in the same poll (Rust's pipeline! awaits the stages
 * in one async block). If any stage fails, the pipeline stops.
 * ------------------------------------------------------------------- */

#ifndef ASX_PIPELINE_MAX_STAGES
#define ASX_PIPELINE_MAX_STAGES 8u
#endif

typedef struct {
    asx_combinator_poll_fn stages[ASX_PIPELINE_MAX_STAGES];
    void *stage_data[ASX_PIPELINE_MAX_STAGES];
    uint32_t stage_count;
    uint32_t current;       /* index of active stage */
    asx_status last_status; /* last completed stage status */
    int done;
    asx_combinator_branch active; /* current stage branch */
} asx_pipeline_state;

/* Initialize a pipeline combinator. */
ASX_API asx_status asx_pipeline_init(asx_pipeline_state *state);

/* Add a stage to the pipeline. */
ASX_API asx_status asx_pipeline_add_stage(asx_pipeline_state *state,
                                          asx_combinator_poll_fn stage_fn, void *user_data);

/* Poll function — compatible with asx_combinator_poll_fn. */
ASX_API asx_status asx_pipeline_poll(void *user_data, asx_task_id self);

/* Get the number of completed stages. */
ASX_API uint32_t asx_pipeline_completed_stages(const asx_pipeline_state *state);

/* -------------------------------------------------------------------
 * Bulkhead — concurrency limiter
 *
 * Limits the number of concurrently active inner polls. When the
 * limit is reached, additional submissions return ASX_E_WOULD_BLOCK.
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t max_concurrent;
    uint32_t active_count;
} asx_bulkhead_state;

/* Initialize a bulkhead concurrency limiter. */
ASX_API asx_status asx_bulkhead_init(asx_bulkhead_state *state, uint32_t max_concurrent);

/* Try to enter the bulkhead. Returns ASX_OK if admitted,
 * ASX_E_WOULD_BLOCK if at capacity. */
ASX_API asx_status asx_bulkhead_try_enter(asx_bulkhead_state *state);

/* Leave the bulkhead (frees one slot). */
ASX_API asx_status asx_bulkhead_leave(asx_bulkhead_state *state);

/* Get the number of currently active entries. */
ASX_API uint32_t asx_bulkhead_active(const asx_bulkhead_state *state);

/* Get the number of remaining slots. */
ASX_API uint32_t asx_bulkhead_remaining(const asx_bulkhead_state *state);

/* -------------------------------------------------------------------
 * RateLimit — token-bucket throttle
 *
 * Allows up to `capacity` operations per refill interval. Tokens
 * are consumed on try_acquire. The caller must call refill() to
 * add tokens back (typically driven by a timer tick).
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t capacity;      /* max tokens */
    uint32_t tokens;        /* currently available */
    uint32_t refill_amount; /* tokens added per refill */
} asx_rate_limit_state;

/* Initialize a token-bucket rate limiter. */
ASX_API asx_status asx_rate_limit_init(asx_rate_limit_state *state, uint32_t capacity,
                                       uint32_t refill_amount);

/* Try to consume one token. Returns ASX_OK if allowed,
 * ASX_E_WOULD_BLOCK if rate limited. */
ASX_API asx_status asx_rate_limit_try_acquire(asx_rate_limit_state *state);

/* Add refill_amount tokens (capped at capacity). */
ASX_API void asx_rate_limit_refill(asx_rate_limit_state *state);

/* Get the number of available tokens. */
ASX_API uint32_t asx_rate_limit_available(const asx_rate_limit_state *state);

/* -------------------------------------------------------------------
 * Hedge — latency hedging (speculative execution)
 *
 * Start a primary task. If it does not complete within deadline_ns,
 * launch a backup. Return whichever completes first. The owner's
 * checkpoint at each poll, before the backup starts and before returning
 * pending ends the hedge with ASX_E_CANCELLED, both attempts dropped
 * (Rust's HedgeFuture; the winner reads as the primary). The attempt that
 * does not win is dropped, a backup never started included (Rust drops
 * its factory): set primary.drop_fn / backup.drop_fn after asx_hedge_init
 * to release what their user_data holds (see asx_combinator_branch).
 *
 * Phases:
 *   PRIMARY_ONLY — polling primary, deadline not expired
 *   RACING       — deadline expired, polling both primary and backup
 *   DECIDED      — winner chosen, done
 * ------------------------------------------------------------------- */

typedef enum { ASX_HEDGE_PRIMARY_ONLY, ASX_HEDGE_RACING, ASX_HEDGE_DECIDED } asx_hedge_phase;

typedef struct {
    asx_combinator_branch primary;
    asx_combinator_branch backup;
    asx_combinator_poll_fn backup_fn;
    void *backup_data;
    uint64_t deadline_ns;
    asx_deadline deadline;
    int deadline_armed;
    asx_hedge_phase phase;
    int winner;        /* 0 = primary, 1 = backup */
    asx_status result; /* final result */
} asx_hedge_state;

/* Initialize a hedge combinator.
 * primary_fn/primary_data: the primary task
 * backup_fn/backup_data: the backup task (launched on deadline)
 * deadline_ns: nanoseconds to wait before hedging */
ASX_API asx_status asx_hedge_init(asx_hedge_state *state, asx_combinator_poll_fn primary_fn,
                                  void *primary_data, asx_combinator_poll_fn backup_fn,
                                  void *backup_data, uint64_t deadline_ns);

/* Poll function — compatible with asx_combinator_poll_fn. */
ASX_API asx_status asx_hedge_poll(void *user_data, asx_task_id self);

/* Get the phase the hedge is currently in. */
ASX_API asx_hedge_phase asx_hedge_current_phase(const asx_hedge_state *state);

/* Get which branch won (0 = primary, 1 = backup). Only valid after DECIDED. */
ASX_API int asx_hedge_winner(const asx_hedge_state *state);

/* -------------------------------------------------------------------
 * Adaptive hedge policy — conformal prediction calibration
 *
 * Dynamically adjusts hedge delay based on observed primary latencies.
 * Uses marginal conformal prediction to compute the (1-alpha) quantile
 * of the sliding window, providing a finite-sample guarantee that the
 * hedge fires only on true statistical outliers.
 * ------------------------------------------------------------------- */

#ifndef ASX_ADAPTIVE_HEDGE_WINDOW
#define ASX_ADAPTIVE_HEDGE_WINDOW 64u
#endif

typedef struct {
    uint64_t history[ASX_ADAPTIVE_HEDGE_WINDOW]; /* circular buffer of latencies (ns) */
    uint32_t count;                              /* total observations recorded */
    uint32_t alpha_pct;                          /* miscoverage target * 100 (e.g., 5 for P95) */
    uint64_t min_delay_ns;                       /* absolute minimum hedge delay */
    uint64_t max_delay_ns;                       /* absolute maximum hedge delay */
} asx_adaptive_hedge_policy;

/* Initialize an adaptive hedge policy.
 * alpha_pct: miscoverage target (5 = P95 hedging, 1 = P99). */
ASX_API void asx_adaptive_hedge_init(asx_adaptive_hedge_policy *policy, uint32_t alpha_pct,
                                     uint64_t min_delay_ns, uint64_t max_delay_ns);

/* Record a primary execution latency (nanoseconds). */
ASX_API void asx_adaptive_hedge_record(asx_adaptive_hedge_policy *policy, uint64_t latency_ns);

/* Compute the dynamically calibrated hedge delay.
 * Returns the conformal (1-alpha) quantile, clamped to [min, max]. */
ASX_API uint64_t asx_adaptive_hedge_delay(const asx_adaptive_hedge_policy *policy);

/* Get the number of observations recorded. */
ASX_API uint32_t asx_adaptive_hedge_observations(const asx_adaptive_hedge_policy *policy);

/* -------------------------------------------------------------------
 * MapReduce — map branches, then reduce their statuses
 *
 * Polls every map branch inline, in index order (like join), and once all
 * have succeeded calls reduce_fn once over their statuses; its status is
 * the result. As Rust's executing map-reduce (combinator/map_reduce.rs:
 * 1-12), the first failure stops it: the first branch to end with an
 * error or ASX_E_CANCELLED ends the map-reduce with that status without a
 * reduce, and the branches still pending are dropped as in race (never
 * polled again, resolved ASX_E_CANCELLED, each drop_fn called once;
 * dropping them is cleanup, not another failure). The failure is cached
 * before the releases, so a drop_fn that re-polls observes that failure
 * without polling another branch or running the reducer.
 * ------------------------------------------------------------------- */

typedef asx_status (*asx_map_reduce_fn)(const asx_status *results, uint32_t count, void *user_data);

typedef struct {
    asx_combinator_branch branches[ASX_COMBINATOR_MAX_BRANCHES];
    uint32_t count;
    asx_map_reduce_fn reduce_fn;
    void *reduce_data;
    int done;
    asx_status result;
    uint32_t dropped; /* branches dropped pending after the first failure */
} asx_map_reduce_state;

/* Initialize a map-reduce combinator with the given reduce function. */
ASX_API asx_status asx_map_reduce_init(asx_map_reduce_state *state, asx_map_reduce_fn reduce_fn,
                                       void *reduce_data);

/* Add a map branch to the map-reduce combinator. */
ASX_API asx_status asx_map_reduce_add(asx_map_reduce_state *state, asx_combinator_poll_fn map_fn,
                                      void *user_data);

/* Poll function — compatible with asx_combinator_poll_fn. */
ASX_API asx_status asx_map_reduce_poll(void *user_data, asx_task_id self);

/* Get number of map branches that completed (dropped ones excluded). */
ASX_API uint32_t asx_map_reduce_completed(const asx_map_reduce_state *state);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_COMBINATOR2_H */
