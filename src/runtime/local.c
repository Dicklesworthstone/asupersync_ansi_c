/*
 * local.c — current-thread local task wrappers
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/local.h>
#include <stddef.h>

static asx_task_handle *local_inner_mut(asx_local_task_handle *handle) {
    if (handle == NULL) return NULL;
    return &handle->inner;
}

static const asx_task_handle *local_inner(const asx_local_task_handle *handle) {
    if (handle == NULL) return NULL;
    return &handle->inner;
}

asx_status asx_local_scope_init(asx_local_scope *scope, asx_region_id region, const asx_cx *cx,
                                asx_budget budget) {
    if (scope == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_scope_init(&scope->inner, region, cx, budget);
}

asx_status asx_local_scope_spawn(asx_local_scope *scope, asx_task_poll_fn poll_fn, void *user_data,
                                 asx_local_task_handle *out_handle) {
    if (scope == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_scope_spawn(&scope->inner, poll_fn, user_data, &out_handle->inner);
}

asx_status asx_local_scope_spawn_with_cx(asx_local_scope *scope, asx_cap_flags child_caps,
                                         asx_task_poll_fn poll_fn, void *user_data,
                                         asx_local_task_handle *out_handle, asx_cx *out_cx) {
    if (scope == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_scope_spawn_with_cx(&scope->inner, child_caps, poll_fn, user_data,
                                   &out_handle->inner, out_cx);
}

asx_status asx_local_scope_spawn_captured(asx_local_scope *scope, asx_task_poll_fn poll_fn,
                                          uint32_t state_size, asx_task_state_dtor_fn state_dtor,
                                          asx_local_task_handle *out_handle, void **out_state) {
    if (scope == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_scope_spawn_captured(&scope->inner, poll_fn, state_size, state_dtor,
                                    &out_handle->inner, out_state);
}

asx_status asx_local_scope_run(asx_local_scope *scope) {
    if (scope == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_scope_run(&scope->inner);
}

asx_status asx_local_scope_drain(asx_local_scope *scope) {
    if (scope == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_scope_drain(&scope->inner);
}

uint32_t asx_local_scope_spawned_count(const asx_local_scope *scope) {
    if (scope == NULL) return 0u;
    return asx_scope_spawned_count(&scope->inner);
}

asx_region_id asx_local_scope_region(const asx_local_scope *scope) {
    if (scope == NULL) return ASX_INVALID_ID;
    return asx_scope_region(&scope->inner);
}

int asx_local_task_handle_is_finished(const asx_local_task_handle *handle) {
    return asx_task_handle_is_finished(local_inner(handle));
}

asx_task_id asx_local_task_handle_task_id(const asx_local_task_handle *handle) {
    return asx_task_handle_task_id(local_inner(handle));
}

asx_status asx_local_task_handle_try_join(asx_local_task_handle *handle, asx_outcome *out) {
    return asx_task_handle_try_join(local_inner_mut(handle), out);
}

asx_join_error asx_local_task_handle_join_error(const asx_outcome *outcome) {
    return asx_task_handle_join_error(outcome);
}

asx_status asx_local_task_handle_abort(asx_local_task_handle *handle) {
    return asx_task_handle_abort(local_inner_mut(handle));
}

asx_status asx_local_task_handle_abort_with_kind(asx_local_task_handle *handle,
                                                 asx_cancel_kind kind) {
    return asx_task_handle_abort_with_kind(local_inner_mut(handle), kind);
}
