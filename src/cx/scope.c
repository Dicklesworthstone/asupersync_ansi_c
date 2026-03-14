/*
 * scope.c — structured concurrency scope and task handles
 *
 * Zero dynamic allocation. All types are value types or borrow
 * from the runtime's static arenas.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/cx/scope.h>
#include <asx/runtime/runtime.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Join error strings                                                  */
/* ------------------------------------------------------------------ */

const char *asx_join_error_str(asx_join_error err) {
    switch (err) {
    case ASX_JOIN_OK: return "ok";
    case ASX_JOIN_CANCELLED: return "cancelled";
    case ASX_JOIN_PANICKED: return "panicked";
    case ASX_JOIN_POLLED_AFTER_COMPLETE: return "polled after completion";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Task handle                                                         */
/* ------------------------------------------------------------------ */

int asx_task_handle_is_finished(const asx_task_handle *h) {
    asx_task_state state;
    if (h == NULL) return 0;
    if (asx_task_get_state(h->task_id, &state) != ASX_OK) return 0;
    return state == ASX_TASK_COMPLETED;
}

asx_task_id asx_task_handle_task_id(const asx_task_handle *h) {
    if (h == NULL) return ASX_INVALID_ID;
    return h->task_id;
}

asx_status asx_task_handle_try_join(asx_task_handle *h, asx_outcome *out) {
    asx_task_state state;
    asx_status st;

    if (h == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    if (h->joined) return ASX_E_INVALID_STATE;

    st = asx_task_get_state(h->task_id, &state);
    if (st != ASX_OK) return st;

    if (state != ASX_TASK_COMPLETED) return ASX_E_PENDING;

    st = asx_task_get_outcome(h->task_id, out);
    if (st != ASX_OK) return st;

    h->joined = 1;
    return ASX_OK;
}

asx_join_error asx_task_handle_join_error(const asx_outcome *outcome) {
    if (outcome == NULL) return ASX_JOIN_OK;

    switch (outcome->severity) {
    case ASX_OUTCOME_OK: return ASX_JOIN_OK;
    case ASX_OUTCOME_ERR: return ASX_JOIN_OK; /* error is still "joined" */
    case ASX_OUTCOME_CANCELLED: return ASX_JOIN_CANCELLED;
    case ASX_OUTCOME_PANICKED: return ASX_JOIN_PANICKED;
    default: return ASX_JOIN_OK;
    }
}

asx_status asx_task_handle_abort(asx_task_handle *h) {
    if (h == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_task_cancel(h->task_id, ASX_CANCEL_USER);
}

asx_status asx_task_handle_abort_with_kind(asx_task_handle *h, asx_cancel_kind kind) {
    if (h == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_task_cancel(h->task_id, kind);
}

/* ------------------------------------------------------------------ */
/* Scope lifecycle                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_scope_init(asx_scope *scope, asx_region_id region, const asx_cx *cx,
                          asx_budget budget) {
    if (scope == NULL || cx == NULL) return ASX_E_INVALID_ARGUMENT;
    if (region == ASX_INVALID_ID) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_has_cap(cx, ASX_CAP_SPAWN)) return ASX_E_PERMISSION_DENIED;
    if (cx->region_id != region) return ASX_E_PERMISSION_DENIED;

    scope->region_id = region;
    scope->cx = *cx;
    scope->cx.region_id = region;
    scope->cx.task_id = ASX_INVALID_ID;
    scope->budget = budget;
    scope->spawned = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Scope spawn                                                         */
/* ------------------------------------------------------------------ */

static asx_status scope_bind_spawned_cx(const asx_scope *scope, asx_task_id tid,
                                        asx_cap_flags child_caps, asx_cx *out_cx) {
    asx_status st;

    if (out_cx == NULL) return ASX_OK;

    st = asx_cx_narrow(&scope->cx, out_cx, child_caps);
    if (st != ASX_OK) return st;

    out_cx->region_id = scope->region_id;
    out_cx->task_id = tid;
    return ASX_OK;
}

asx_status asx_scope_spawn(asx_scope *scope, asx_task_poll_fn poll_fn, void *user_data,
                           asx_task_handle *out_handle) {
    return asx_scope_spawn_with_cx(scope, scope != NULL ? scope->cx.caps : ASX_CAP_NONE, poll_fn,
                                   user_data, out_handle, NULL);
}

asx_status asx_scope_spawn_with_cx(asx_scope *scope, asx_cap_flags child_caps,
                                   asx_task_poll_fn poll_fn, void *user_data,
                                   asx_task_handle *out_handle, asx_cx *out_cx) {
    asx_task_id tid;
    asx_status st;

    if (scope == NULL || poll_fn == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_has_cap(&scope->cx, ASX_CAP_SPAWN)) return ASX_E_PERMISSION_DENIED;

    st = asx_task_spawn(scope->region_id, poll_fn, user_data, &tid);
    if (st != ASX_OK) return st;

    out_handle->task_id = tid;
    out_handle->region_id = scope->region_id;
    out_handle->joined = 0;
    st = scope_bind_spawned_cx(scope, tid, child_caps, out_cx);
    if (st != ASX_OK) return st;
    scope->spawned++;
    return ASX_OK;
}

asx_status asx_scope_spawn_captured(asx_scope *scope, asx_task_poll_fn poll_fn, uint32_t state_size,
                                    asx_task_state_dtor_fn state_dtor, asx_task_handle *out_handle,
                                    void **out_state) {
    asx_task_id tid;
    asx_status st;

    if (scope == NULL || poll_fn == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_has_cap(&scope->cx, ASX_CAP_SPAWN)) return ASX_E_PERMISSION_DENIED;

    st =
        asx_task_spawn_captured(scope->region_id, poll_fn, state_size, state_dtor, &tid, out_state);
    if (st != ASX_OK) return st;

    out_handle->task_id = tid;
    out_handle->region_id = scope->region_id;
    out_handle->joined = 0;
    scope->spawned++;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Scope execution                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_scope_run(asx_scope *scope) {
    if (scope == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_scheduler_run(scope->region_id, &scope->budget);
}

asx_status asx_scope_drain(asx_scope *scope) {
    if (scope == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_region_drain(scope->region_id, &scope->budget);
}

/* ------------------------------------------------------------------ */
/* Scope queries                                                       */
/* ------------------------------------------------------------------ */

uint32_t asx_scope_spawned_count(const asx_scope *scope) {
    if (scope == NULL) return 0u;
    return scope->spawned;
}

asx_region_id asx_scope_region(const asx_scope *scope) {
    if (scope == NULL) return ASX_INVALID_ID;
    return scope->region_id;
}
