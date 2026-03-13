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

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/asx_ids.h>
#include <asx/actor/actor.h>

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

/* -------------------------------------------------------------------
 * Restart strategy — what to do when a child dies
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_SUPERVISOR_ONE_FOR_ONE  = 0,  /* restart only the failed child */
    ASX_SUPERVISOR_ONE_FOR_ALL  = 1,  /* stop all, restart all */
    ASX_SUPERVISOR_REST_FOR_ONE = 2   /* stop+restart failed and later */
} asx_supervisor_strategy;

/* -------------------------------------------------------------------
 * Child restart policy — when to restart a child
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_CHILD_PERMANENT = 0,   /* always restart */
    ASX_CHILD_TRANSIENT = 1,   /* restart only on abnormal exit */
    ASX_CHILD_TEMPORARY = 2    /* never restart */
} asx_child_restart;

/* -------------------------------------------------------------------
 * Child start function
 *
 * Called by the supervisor to start (or restart) a child.
 * Must spawn an actor and write its handle to *out.
 * ------------------------------------------------------------------- */

typedef asx_status (*asx_child_start_fn)(
    void *user_data,
    asx_region_id region,
    asx_actor_handle *out);

/* -------------------------------------------------------------------
 * Child spec — describes how to start and manage a child
 * ------------------------------------------------------------------- */

typedef struct {
    asx_child_start_fn start_fn;    /* how to start this child */
    void              *user_data;   /* passed to start_fn */
    asx_child_restart  restart;     /* restart policy */
} asx_child_spec;

/* -------------------------------------------------------------------
 * Supervisor handle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_supervisor_handle;

/* -------------------------------------------------------------------
 * Supervisor config
 * ------------------------------------------------------------------- */

typedef struct {
    asx_supervisor_strategy strategy;
    uint32_t max_restarts;    /* max restarts before escalation */
} asx_supervisor_config;

/* -------------------------------------------------------------------
 * Supervisor lifecycle
 * ------------------------------------------------------------------- */

/* Start a supervisor with the given config and child specs.
 * Children are started in order (index 0 first).
 * If any child fails to start, all previously started children
 * are stopped and the supervisor fails. */
ASX_API ASX_MUST_USE asx_status asx_supervisor_start(
    asx_supervisor_handle *out,
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
ASX_API int asx_supervisor_child_alive(
    asx_supervisor_handle sup, uint32_t index);

/* Get the number of restarts the supervisor has performed. */
ASX_API uint32_t asx_supervisor_restart_count(asx_supervisor_handle sup);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

ASX_API void asx_supervisor_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_ACTOR_SUPERVISOR_H */
