/*
 * asx/core/join_set.h — dynamic task collection with completion iteration
 *
 * A JoinSet collects spawned tasks and allows iterating over their
 * completions. Unlike the fixed-size join combinator, JoinSet supports
 * adding tasks after initialization and polling for the next completed
 * task in completion order (not insertion order).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_JOIN_SET_H
#define ASX_CORE_JOIN_SET_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/outcome.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ASX_JOIN_SET_MAX_TASKS
#define ASX_JOIN_SET_MAX_TASKS 16u
#endif

typedef struct {
    asx_task_id task_id;
    asx_outcome outcome;
    uint8_t active;
    uint8_t completed;
} asx_join_set_entry;

typedef struct {
    asx_join_set_entry entries[ASX_JOIN_SET_MAX_TASKS];
    uint32_t count;
    uint32_t completed_count;
    uint32_t next_poll_idx;
} asx_join_set;

/* Result of join_next: identifies which task completed */
typedef struct {
    asx_task_id task_id;
    asx_outcome outcome;
    uint32_t index;
} asx_join_set_result;

/* Initialize an empty JoinSet. */
ASX_API void asx_join_set_init(asx_join_set *js);

/* Add a task to the set. Can be called after init, even while polling. */
ASX_API ASX_MUST_USE asx_status asx_join_set_add(asx_join_set *js, asx_task_id task_id);

/* Poll for the next completed task. Returns ASX_OK with result when a task
 * completes, ASX_E_PENDING if all tasks are still running, ASX_E_NOT_FOUND
 * if the set is empty or all completed tasks have been consumed. */
ASX_API ASX_MUST_USE asx_status asx_join_set_poll_next(asx_join_set *js, asx_join_set_result *out);

/* Cancel all remaining tasks in the set. */
ASX_API asx_status asx_join_set_abort_all(asx_join_set *js);

/* Get the number of tasks still active (not yet completed). */
ASX_API uint32_t asx_join_set_active_count(const asx_join_set *js);

/* Get the total number of tasks (active + completed). */
ASX_API uint32_t asx_join_set_total_count(const asx_join_set *js);

/* Check if all tasks have completed. */
ASX_API int asx_join_set_is_empty(const asx_join_set *js);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_JOIN_SET_H */
