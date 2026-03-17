/*
 * asx/runtime/local.h — explicit current-thread local task surface
 *
 * Provides a public "local task" API for current-thread runtimes without
 * introducing a second scheduler. In the ANSI C walking skeleton, all tasks
 * execute on the owning region's single-threaded scheduler, so local tasks are
 * pinned by construction and never migrate across workers.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_LOCAL_H
#define ASX_RUNTIME_LOCAL_H

#include <asx/asx_export.h>
#include <asx/cx/scope.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    asx_task_handle inner;
} asx_local_task_handle;

typedef struct {
    asx_scope inner;
} asx_local_scope;

/* Initialize a local scope bound to the current-thread runtime lane. */
ASX_API ASX_MUST_USE asx_status asx_local_scope_init(asx_local_scope *scope, asx_region_id region,
                                                     const asx_cx *cx, asx_budget budget);

/* Spawn a local task pinned to the current-thread scheduler lane. */
ASX_API ASX_MUST_USE asx_status asx_local_scope_spawn(asx_local_scope *scope,
                                                      asx_task_poll_fn poll_fn, void *user_data,
                                                      asx_local_task_handle *out_handle);

/* Spawn a local task and derive a task-bound child capability context. */
ASX_API ASX_MUST_USE asx_status asx_local_scope_spawn_with_cx(
    asx_local_scope *scope, asx_cap_flags child_caps, asx_task_poll_fn poll_fn, void *user_data,
    asx_local_task_handle *out_handle, asx_cx *out_cx);

/* Spawn a local task with region-owned captured state. */
ASX_API ASX_MUST_USE asx_status asx_local_scope_spawn_captured(
    asx_local_scope *scope, asx_task_poll_fn poll_fn, uint32_t state_size,
    asx_task_state_dtor_fn state_dtor, asx_local_task_handle *out_handle, void **out_state);

/* Drive or drain the local scope through the owning region scheduler. */
ASX_API ASX_MUST_USE asx_status asx_local_scope_run(asx_local_scope *scope);
ASX_API ASX_MUST_USE asx_status asx_local_scope_drain(asx_local_scope *scope);

/* Query the local scope.
 *
 * Uninitialized or corrupted scope values fail closed: count queries return 0
 * and region queries return ASX_INVALID_ID.
 */
ASX_API uint32_t asx_local_scope_spawned_count(const asx_local_scope *scope);
ASX_API asx_region_id asx_local_scope_region(const asx_local_scope *scope);

/* Local handle helpers mirror the generic task-handle surface.
 *
 * Uninitialized or corrupted local-handle values fail closed: query helpers
 * return safe sentinels and stateful operations return ASX_E_INVALID_STATE.
 */
ASX_API int asx_local_task_handle_is_finished(const asx_local_task_handle *handle);
ASX_API asx_task_id asx_local_task_handle_task_id(const asx_local_task_handle *handle);
ASX_API ASX_MUST_USE asx_status asx_local_task_handle_try_join(asx_local_task_handle *handle,
                                                               asx_outcome *out);
ASX_API asx_join_error asx_local_task_handle_join_error(const asx_outcome *outcome);
ASX_API asx_status asx_local_task_handle_abort(asx_local_task_handle *handle);
ASX_API asx_status asx_local_task_handle_abort_with_kind(asx_local_task_handle *handle,
                                                         asx_cancel_kind kind);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_LOCAL_H */
