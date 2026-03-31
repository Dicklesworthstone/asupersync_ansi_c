/*
 * asx/actor/supervisor.h — supervision trees and restart strategies
 *
 * Provides OTP-style supervision:
 *   - Restart strategies: one_for_one, one_for_all, rest_for_one
 *   - Child restart policies: permanent, transient, temporary
 *   - Restart intensity limits with escalation
 *   - Ordered startup and reverse-order shutdown
 *
 * A supervisor runs as a task that monitors its child actors.
 * When a child dies, the supervisor applies the configured restart
 * strategy. If the restart intensity limit is exceeded, the
 * supervisor itself terminates (escalation).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_ACTOR_SUPERVISOR_H
#define ASX_ACTOR_SUPERVISOR_H

#include <asx/actor/actor.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Arena limits
 * ------------------------------------------------------------------- */

#ifndef ASX_MAX_SUPERVISORS
#define ASX_MAX_SUPERVISORS 8u
#endif

#ifndef ASX_SUPERVISOR_MAX_CHILDREN
#define ASX_SUPERVISOR_MAX_CHILDREN 8u
#endif

/* Bitmask-based restart tracking limits children to 32 */
#if ASX_SUPERVISOR_MAX_CHILDREN > 32
#error "ASX_SUPERVISOR_MAX_CHILDREN must be <= 32 (bitmask limit)"
#endif

/* -------------------------------------------------------------------
 * Restart strategy — what to do when a child dies
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_SUPERVISOR_ONE_FOR_ONE = 0, /* restart only the failed child */
    ASX_SUPERVISOR_ONE_FOR_ALL = 1, /* stop all, restart all */
    ASX_SUPERVISOR_REST_FOR_ONE = 2 /* stop+restart failed and later */
} asx_supervisor_strategy;

/* -------------------------------------------------------------------
 * Child restart policy — when to restart a child
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_CHILD_PERMANENT = 0, /* always restart */
    ASX_CHILD_TRANSIENT = 1, /* restart only on abnormal exit */
    ASX_CHILD_TEMPORARY = 2  /* never restart */
} asx_child_restart;

/* -------------------------------------------------------------------
 * Child start function
 *
 * Called by the supervisor to start (or restart) a child.
 * Must spawn an actor and write its handle to *out.
 * ------------------------------------------------------------------- */

typedef asx_status (*asx_child_start_fn)(void *user_data, asx_region_id region,
                                         asx_actor_handle *out);

/* -------------------------------------------------------------------
 * Child spec — describes how to start and manage a child
 * ------------------------------------------------------------------- */

typedef struct {
    asx_child_start_fn start_fn; /* how to start this child */
    void *user_data;             /* passed to start_fn */
    asx_child_restart restart;   /* restart policy */
} asx_child_spec;

/* -------------------------------------------------------------------
 * Supervisor handle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_supervisor_handle;

/* -------------------------------------------------------------------
 * Child spec extended — dependency and shutdown budget
 * ------------------------------------------------------------------- */

#ifndef ASX_SUPERVISOR_MAX_DEPS
#define ASX_SUPERVISOR_MAX_DEPS 4u
#endif

#ifndef ASX_CHILD_NAME_MAX
#define ASX_CHILD_NAME_MAX 32u
#endif

typedef struct {
    asx_child_start_fn start_fn;
    void *user_data;
    asx_child_restart restart;
    char name[ASX_CHILD_NAME_MAX];
    uint32_t depends_on[ASX_SUPERVISOR_MAX_DEPS]; /* indices of children this depends on */
    uint32_t dep_count;
    uint32_t shutdown_budget_polls; /* max polls during shutdown (0 = default) */
    uint8_t required;               /* if 1, supervisor fails if this child can't start */
    uint8_t start_immediately;      /* if 1, start with supervisor (default). if 0, lazy start */
} asx_child_spec_ext;

/* Initialize an extended child spec with defaults. */
ASX_API void asx_child_spec_ext_init(asx_child_spec_ext *spec, const char *name,
                                     asx_child_start_fn start_fn, void *user_data);

/* Add a dependency (index of another child). */
ASX_API asx_status asx_child_spec_ext_depends_on(asx_child_spec_ext *spec, uint32_t dep_index);

/* -------------------------------------------------------------------
 * Restart intensity — time-windowed restart tracking
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t restart_timestamps[64]; /* ring buffer of restart times */
    uint32_t head;
    uint32_t count;
    uint32_t max_restarts;
    uint64_t window_ns; /* time window for restart counting */
} asx_restart_intensity;

/* Initialize restart intensity tracker. */
ASX_API void asx_restart_intensity_init(asx_restart_intensity *ri, uint32_t max_restarts,
                                        uint64_t window_ns);

/* Record a restart. Returns 1 if intensity exceeded (too many restarts in window). */
ASX_API int asx_restart_intensity_record(asx_restart_intensity *ri, uint64_t now_ns);

/* Get the number of restarts within the current window. */
ASX_API uint32_t asx_restart_intensity_count(const asx_restart_intensity *ri, uint64_t now_ns);

/* Check if restart intensity is in "storm" state. */
ASX_API int asx_restart_intensity_is_storm(const asx_restart_intensity *ri, uint64_t now_ns);

/* -------------------------------------------------------------------
 * Supervisor config (extended)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_supervisor_strategy strategy;
    uint32_t max_restarts;          /* max restarts before escalation */
    uint64_t restart_window_ns;     /* time window for restart intensity (0 = no window) */
    uint32_t shutdown_budget_polls; /* default per-child shutdown budget */
} asx_supervisor_config;

/* -------------------------------------------------------------------
 * Supervisor lifecycle
 * ------------------------------------------------------------------- */

/* Start a supervisor with the given config and child specs.
 * Children are started in order (index 0 first).
 * If any child fails to start, all previously started children
 * are stopped and the supervisor fails. */
ASX_API ASX_MUST_USE asx_status asx_supervisor_start(asx_supervisor_handle *out,
                                                     asx_region_id region,
                                                     const asx_supervisor_config *config,
                                                     const asx_child_spec *children,
                                                     uint32_t child_count);

/* Request graceful shutdown of the supervisor.
 * Children are stopped in reverse order. */
ASX_API asx_status asx_supervisor_stop(asx_supervisor_handle sup);

/* Check if a supervisor is alive. */
ASX_API int asx_supervisor_is_alive(asx_supervisor_handle sup);

/* Get the number of children. */
ASX_API uint32_t asx_supervisor_child_count(asx_supervisor_handle sup);

/* Check if a specific child is alive (by index). */
ASX_API int asx_supervisor_child_alive(asx_supervisor_handle sup, uint32_t index);

/* Get the number of restarts the supervisor has performed. */
ASX_API uint32_t asx_supervisor_restart_count(asx_supervisor_handle sup);

/* Start a supervisor with extended child specs (supports deps, names, budgets). */
ASX_API ASX_MUST_USE asx_status asx_supervisor_start_ext(asx_supervisor_handle *out,
                                                         asx_region_id region,
                                                         const asx_supervisor_config *config,
                                                         const asx_child_spec_ext *children,
                                                         uint32_t child_count);

/* Get a child's name (or empty string if unnamed). */
ASX_API const char *asx_supervisor_child_name(asx_supervisor_handle sup, uint32_t index);

/* Get the supervisor's exit reason (only valid after stop). */
ASX_API asx_status asx_supervisor_exit_reason(asx_supervisor_handle sup);

/* Check if restart intensity has been exceeded. */
ASX_API int asx_supervisor_intensity_exceeded(asx_supervisor_handle sup);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset all supervisor arena state. For tests only. */
ASX_API void asx_supervisor_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_ACTOR_SUPERVISOR_H */
