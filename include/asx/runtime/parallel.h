/*
 * asx/runtime/parallel.h — optional parallel profile with worker model and lane rules
 *
 * Provides a lane-based worker scheduling model for the optional parallel
 * profile. Tasks are assigned to lanes by work class (ready, cancel, timed).
 * Each lane has bounded fairness controls to prevent starvation.
 *
 * Provides deterministic worker-lane scheduling with replay-stable commit
 * order. Generic CORE/POSIX/WIN32/PARALLEL builds reserve enough worker
 * metadata for 64-core deployments; constrained profiles keep smaller
 * compile-time caps.
 *
 * Feature-gated: compile with -DASX_PROFILE_PARALLEL to enable.
 * When disabled, all APIs compile to zero-overhead stubs.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_PARALLEL_H
#define ASX_RUNTIME_PARALLEL_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/affinity.h>
#include <asx/core/budget.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Lane capacity limits
 * ------------------------------------------------------------------- */

#ifndef ASX_MAX_WORKERS
#if defined(ASX_PROFILE_FREESTANDING) || defined(ASX_PROFILE_BROWSER)
#define ASX_MAX_WORKERS 1u
#elif defined(ASX_PROFILE_EMBEDDED_ROUTER)
#define ASX_MAX_WORKERS 4u
#elif defined(ASX_PROFILE_HFT) || defined(ASX_PROFILE_AUTOMOTIVE)
#define ASX_MAX_WORKERS 16u
#else
#define ASX_MAX_WORKERS 64u
#endif
#endif

#if ASX_MAX_WORKERS < 1u
#error "ASX_MAX_WORKERS must be at least 1"
#endif

#define ASX_PARALLEL_GENERIC_TARGET_WORKERS 64u
#define ASX_MAX_LANES 3u /* READY, CANCEL, TIMED */
#define ASX_LANE_TASK_CAPACITY 64u

/* -------------------------------------------------------------------
 * Lane classification
 *
 * Tasks are assigned to lanes by their work class:
 *   READY  — tasks with no pending cancel or timer dependency
 *   CANCEL — tasks in cancel phase (CancelRequested, Cancelling)
 *   TIMED  — tasks blocked on timer deadlines
 * ------------------------------------------------------------------- */

typedef enum { ASX_LANE_READY = 0, ASX_LANE_CANCEL = 1, ASX_LANE_TIMED = 2 } asx_lane_class;

/* -------------------------------------------------------------------
 * Fairness policy
 *
 * Controls how poll budget is distributed across lanes:
 *   ROUND_ROBIN — each lane gets equal share per round
 *   WEIGHTED    — lanes get budget proportional to assigned weights
 *   PRIORITY    — cancel lane drains first, then ready, then timed
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_FAIRNESS_ROUND_ROBIN = 0,
    ASX_FAIRNESS_WEIGHTED = 1,
    ASX_FAIRNESS_PRIORITY = 2
} asx_fairness_policy;

/* -------------------------------------------------------------------
 * Lane descriptor (per-lane operational state)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_lane_class lane_class;
    uint32_t weight;           /* relative budget weight (1-100) */
    uint32_t task_count;       /* current tasks assigned */
    uint32_t polls_this_round; /* polls consumed in current round */
    uint32_t starvation_count; /* consecutive rounds with no polls */
    uint32_t max_starvation;   /* starvation threshold for alerts */
} asx_lane_state;

/* -------------------------------------------------------------------
 * Worker lifecycle
 *
 * Workers are deterministic scheduler lanes in the ANSI C core. A live
 * platform adapter may execute these lanes concurrently only when it can
 * preserve the same externally committed event order.
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_WORKER_STOPPED = 0,
    ASX_WORKER_RUNNING = 1,
    ASX_WORKER_DRAINING = 2,
    ASX_WORKER_DRAINED = 3
} asx_worker_lifecycle;

/* -------------------------------------------------------------------
 * Worker descriptor (per-worker state)
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t id;
    asx_affinity_domain domain; /* affinity domain for this worker */
    int active;                 /* 1 if worker is running */
    uint32_t polls_total;       /* lifetime poll count */
    uint32_t tasks_completed;   /* lifetime completions */
    uint32_t steals_total;      /* lifetime deterministic steals accepted */
    uint32_t commits_total;     /* externally committed scheduler events */
    uint32_t last_commit_sequence;
    asx_worker_lifecycle lifecycle;
    uint32_t lane_depths[ASX_MAX_LANES];
} asx_worker_state;

/* -------------------------------------------------------------------
 * Parallel scheduler configuration
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t worker_count; /* number of workers (1 = single-threaded) */
    asx_fairness_policy fairness;
    uint32_t lane_weights[ASX_MAX_LANES]; /* per-lane weights */
    uint32_t starvation_limit;            /* max rounds without polls before alert */
} asx_parallel_config;

/* -------------------------------------------------------------------
 * API: Parallel scheduler lifecycle
 * ------------------------------------------------------------------- */

/* Initialize the parallel scheduler with the given configuration.
 * Must be called before asx_parallel_run().
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if cfg is NULL
 * or worker_count is 0, and ASX_E_RESOURCE_EXHAUSTED if worker_count
 * exceeds the active profile's ASX_MAX_WORKERS cap. */
ASX_API ASX_MUST_USE asx_status asx_parallel_init(const asx_parallel_config *cfg);

/* Reset all parallel scheduler state (test support). */
ASX_API void asx_parallel_reset(void);

/* -------------------------------------------------------------------
 * API: Lane management
 * ------------------------------------------------------------------- */

/* Assign a task to a lane based on its work class.
 * Returns ASX_OK on success, ASX_E_INVALID_STATE if the scheduler has not
 * been initialized, ASX_E_INVALID_ARGUMENT for malformed/stale/non-task
 * handles or lane classes, or ASX_E_RESOURCE_EXHAUSTED if lane is full. */
ASX_API ASX_MUST_USE asx_status asx_lane_assign(asx_task_id tid, asx_lane_class lane);

/* Remove a task from its lane (on completion or reclassification).
 * Returns ASX_OK on success, ASX_E_INVALID_STATE if the scheduler has not
 * been initialized, or ASX_E_NOT_FOUND if task not in any lane. */
ASX_API ASX_MUST_USE asx_status asx_lane_remove(asx_task_id tid);

/* Query the lane state for a given lane class.
 * Returns ASX_OK on success, ASX_E_INVALID_STATE if the scheduler has not
 * been initialized, or ASX_E_INVALID_ARGUMENT if out is NULL. */
ASX_API ASX_MUST_USE asx_status asx_lane_get_state(asx_lane_class lane, asx_lane_state *out);

/* Query the total task count across all lanes. */
ASX_API uint32_t asx_lane_total_tasks(void);

/* -------------------------------------------------------------------
 * API: Worker management
 * ------------------------------------------------------------------- */

/* Query the state of a worker by index.
 * Returns ASX_OK on success, ASX_E_INVALID_STATE if the scheduler has not
 * been initialized, or ASX_E_INVALID_ARGUMENT if out is NULL
 * or index exceeds configured worker_count. */
ASX_API ASX_MUST_USE asx_status asx_worker_get_state(uint32_t worker_index, asx_worker_state *out);

/* Query the configured worker count. */
ASX_API uint32_t asx_parallel_worker_count(void);

/* -------------------------------------------------------------------
 * API: Parallel scheduler run
 * ------------------------------------------------------------------- */

/* Run the parallel scheduler for a region with lane-based fairness.
 *
 * In single-worker mode (worker_count=1), produces identical event
 * streams to asx_scheduler_run() for deterministic parity.
 *
 * Returns ASX_OK when all tasks complete,
 *   ASX_E_POLL_BUDGET_EXHAUSTED if budget runs out,
 *   ASX_E_INVALID_ARGUMENT if budget is NULL.
 *
 * Fairness guarantee: no lane is starved for more than
 * starvation_limit consecutive rounds. */
ASX_API ASX_MUST_USE asx_status asx_parallel_run(asx_region_id region, asx_budget *budget);

/* -------------------------------------------------------------------
 * API: Fairness queries
 * ------------------------------------------------------------------- */

/* Check if any lane has been starved beyond the configured limit.
 * Returns 1 if starvation detected, 0 otherwise. */
ASX_API int asx_parallel_starvation_detected(void);

/* Get the maximum starvation count across all lanes. */
ASX_API uint32_t asx_parallel_max_starvation(void);

/* -------------------------------------------------------------------
 * API: Global injector
 *
 * Inject tasks into the scheduler by work class. These provide
 * the cross-boundary submission interface for structured concurrency.
 * In single-threaded mode, injection is immediate lane assignment.
 * ------------------------------------------------------------------- */

/* Inject a task into the cancel lane (highest priority).
 * Used when a task transitions to cancel-requested or cancelling.
 * Returns ASX_E_INVALID_ARGUMENT for malformed/stale/non-task handles. */
ASX_API ASX_MUST_USE asx_status asx_inject_cancel(asx_task_id tid);

/* Inject a task into the timed lane (deadline-ordered).
 * Used for tasks blocked on timer deadlines.
 * Returns ASX_E_INVALID_ARGUMENT for malformed/stale/non-task handles. */
ASX_API ASX_MUST_USE asx_status asx_inject_timed(asx_task_id tid);

/* Inject a task into the ready lane (default work class).
 * Used for newly spawned tasks and tasks that become runnable.
 * Returns ASX_E_INVALID_ARGUMENT for malformed/stale/non-task handles. */
ASX_API ASX_MUST_USE asx_status asx_inject_ready(asx_task_id tid);

/* -------------------------------------------------------------------
 * API: Scheduling metrics
 *
 * Per-lane dispatch counters and cancel-streak tracking for
 * fairness analysis and observability.
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t cancel_dispatches; /* total cancel-lane polls */
    uint32_t timed_dispatches;  /* total timed-lane polls */
    uint32_t ready_dispatches;  /* total ready-lane polls */
    uint32_t cancel_streak;     /* current consecutive cancel polls */
    uint32_t cancel_streak_max; /* peak cancel streak observed */
    uint32_t fairness_yields;   /* times cancel was skipped for fairness */
    uint32_t steal_attempts;    /* deterministic worker-lane steal probes */
    uint32_t steals_succeeded;  /* probes that transferred lane ownership */
    uint32_t worker_yields;     /* idle worker turns while peer lanes had work */
    uint32_t commit_sequence;   /* replay-stable committed event counter */
} asx_scheduling_metrics;

/* Read the current scheduling metrics.
 * Returns ASX_E_INVALID_STATE if the scheduler has not been initialized. */
ASX_API ASX_MUST_USE asx_status asx_parallel_get_metrics(asx_scheduling_metrics *out);

/* Reset scheduling metrics to zero. */
ASX_API void asx_parallel_reset_metrics(void);

/* -------------------------------------------------------------------
 * API: Cancel-streak fairness configuration
 * ------------------------------------------------------------------- */

/* Set the cancel-streak limit. When the scheduler dispatches this
 * many consecutive cancel-lane tasks, it yields to other lanes.
 * Default is 16. Set to 0 to disable (unlimited cancel streak). */
ASX_API void asx_parallel_set_cancel_streak_limit(uint32_t limit);

/* Get the current cancel-streak limit. */
ASX_API uint32_t asx_parallel_cancel_streak_limit(void);

/* -------------------------------------------------------------------
 * API: Configuration queries
 * ------------------------------------------------------------------- */

/* Query the active fairness policy. */
ASX_API asx_fairness_policy asx_parallel_fairness_policy(void);

/* Check if parallel mode is initialized. */
ASX_API int asx_parallel_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_PARALLEL_H */
