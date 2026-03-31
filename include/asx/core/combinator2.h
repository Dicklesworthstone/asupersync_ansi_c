/*
 * asx/core/combinator2.h — second-wave async control flow combinators
 *
 * Provides retry, bracket (resource-safe cleanup), pipeline (sequential
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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Retry — poll inner with configurable retry on failure
 *
 * On failure, resets the inner branch and retries up to max_retries
 * times. An optional backoff_ns is added between retries (requires
 * runtime time support for actual delay; walking skeleton just
 * re-polls immediately).
 * ------------------------------------------------------------------- */

typedef struct {
    asx_combinator_poll_fn poll_fn;
    void *user_data;
    uint32_t max_retries;        /* 0 = no retries (try once) */
    uint32_t attempt;            /* current attempt (0-based) */
    asx_status last_error;       /* last failure status */
    int done;                    /* 1 when decided */
    asx_status result;           /* final result */
    asx_combinator_branch inner; /* current attempt branch */
} asx_retry_state;

/* Initialize a retry combinator with the given max retry count. */
ASX_API asx_status asx_retry_init(asx_retry_state *state, asx_combinator_poll_fn poll_fn,
                                  void *user_data, uint32_t max_retries);

/* Poll function — compatible with asx_combinator_poll_fn. */
ASX_API asx_status asx_retry_poll(void *user_data, asx_task_id self);

/* Get the number of attempts made (1 = succeeded on first try). */
ASX_API uint32_t asx_retry_attempts(const asx_retry_state *state);

/* Get the last error (meaningful if retry exhausted). */
ASX_API asx_status asx_retry_last_error(const asx_retry_state *state);

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
 * stage N+1 begins. If any stage fails, the pipeline stops.
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
 * launch a backup. Return whichever completes first.
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
 * MapReduce — parallel map + reduce
 *
 * Runs all map tasks concurrently (like join), then applies a
 * reduce function to combine results. Map tasks use standard
 * combinator poll functions. The reduce function is called once
 * after all maps complete.
 * ------------------------------------------------------------------- */

typedef asx_status (*asx_map_reduce_fn)(const asx_status *results, uint32_t count, void *user_data);

typedef struct {
    asx_combinator_branch branches[ASX_COMBINATOR_MAX_BRANCHES];
    uint32_t count;
    asx_map_reduce_fn reduce_fn;
    void *reduce_data;
    int done;
    asx_status result;
} asx_map_reduce_state;

/* Initialize a map-reduce combinator with the given reduce function. */
ASX_API asx_status asx_map_reduce_init(asx_map_reduce_state *state, asx_map_reduce_fn reduce_fn,
                                       void *reduce_data);

/* Add a map branch to the map-reduce combinator. */
ASX_API asx_status asx_map_reduce_add(asx_map_reduce_state *state, asx_combinator_poll_fn map_fn,
                                      void *user_data);

/* Poll function — compatible with asx_combinator_poll_fn. */
ASX_API asx_status asx_map_reduce_poll(void *user_data, asx_task_id self);

/* Get number of completed map branches. */
ASX_API uint32_t asx_map_reduce_completed(const asx_map_reduce_state *state);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_COMBINATOR2_H */
