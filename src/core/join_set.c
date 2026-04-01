/*
 * join_set.c — dynamic task collection with completion iteration
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/join_set.h>
#include <asx/runtime/runtime.h>
#include <string.h>

void asx_join_set_init(asx_join_set *js) {
    if (js == NULL) return;
    memset(js, 0, sizeof(*js));
}

asx_status asx_join_set_add(asx_join_set *js, asx_task_id task_id) {
    if (js == NULL) return ASX_E_INVALID_ARGUMENT;
    if (task_id == ASX_INVALID_ID) return ASX_E_INVALID_ARGUMENT;
    if (js->count >= ASX_JOIN_SET_MAX_TASKS) return ASX_E_RESOURCE_EXHAUSTED;

    js->entries[js->count].task_id = task_id;
    js->entries[js->count].active = 1u;
    js->entries[js->count].completed = 0u;
    js->count++;
    return ASX_OK;
}

asx_status asx_join_set_poll_next(asx_join_set *js, asx_join_set_result *out) {
    uint32_t i, idx;
    asx_task_state state;
    asx_status st;

    if (js == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    /* First check for any newly completed tasks */
    for (i = 0u; i < js->count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        idx = (js->next_poll_idx + i) % js->count;
        if (!js->entries[idx].active) continue;

        st = asx_task_get_state(js->entries[idx].task_id, &state);
        if (st != ASX_OK) {
            /* Task lookup failed — mark as completed with error */
            js->entries[idx].active = 0u;
            js->entries[idx].completed = 1u;
            js->entries[idx].outcome = asx_outcome_make(ASX_OUTCOME_ERR);
            js->completed_count++;

            out->task_id = js->entries[idx].task_id;
            out->outcome = js->entries[idx].outcome;
            out->index = idx;
            js->next_poll_idx = (idx + 1u) % js->count;
            return ASX_OK;
        }

        if (state == ASX_TASK_COMPLETED) {
            asx_outcome outcome;
            st = asx_task_get_outcome(js->entries[idx].task_id, &outcome);
            if (st != ASX_OK) { outcome = asx_outcome_make(ASX_OUTCOME_ERR); }

            js->entries[idx].active = 0u;
            js->entries[idx].completed = 1u;
            js->entries[idx].outcome = outcome;
            js->completed_count++;

            out->task_id = js->entries[idx].task_id;
            out->outcome = outcome;
            out->index = idx;
            js->next_poll_idx = (idx + 1u) % js->count;
            return ASX_OK;
        }
    }

    /* No tasks completed yet */
    if (js->completed_count >= js->count) return ASX_E_NOT_FOUND;
    return ASX_E_PENDING;
}

asx_status asx_join_set_abort_all(asx_join_set *js) {
    uint32_t i;

    if (js == NULL) return ASX_E_INVALID_ARGUMENT;
    for (i = 0u; i < js->count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (js->entries[i].active) {
            asx_status st = asx_task_cancel(js->entries[i].task_id, ASX_CANCEL_USER);
            (void)st;
        }
    }
    return ASX_OK;
}

uint32_t asx_join_set_active_count(const asx_join_set *js) {
    if (js == NULL) return 0u;
    return js->count - js->completed_count;
}

uint32_t asx_join_set_total_count(const asx_join_set *js) {
    if (js == NULL) return 0u;
    return js->count;
}

int asx_join_set_is_empty(const asx_join_set *js) {
    if (js == NULL) return 1;
    return js->completed_count >= js->count;
}
