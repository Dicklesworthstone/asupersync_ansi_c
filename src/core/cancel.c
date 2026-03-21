/*
 * cancel.c — cancellation cleanup budget and strengthen implementation
 *
 * Severity lookup is provided by the inline asx_cancel_severity() in asx_ids.h.
 * This file implements cleanup budget construction and reason strengthening.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/cancel.h>
#include <asx/runtime/runtime.h>
#include <stddef.h>

/* Default cleanup poll quota per severity group (0-5) */
static const uint32_t quota_by_severity[] = {
    1000, /* severity 0: User */
    500,  /* severity 1: Timeout/Deadline */
    300,  /* severity 2: PollQuota/CostBudget */
    200,  /* severity 3: FailFast/RaceLost/LinkedExit */
    200,  /* severity 4: Parent/Resource */
    50    /* severity 5: Shutdown */
};

/* Default cleanup priority per severity group (0-5) */
static const uint8_t priority_by_severity[] = {
    200, /* severity 0: User */
    210, /* severity 1: Timeout/Deadline */
    215, /* severity 2: PollQuota/CostBudget */
    220, /* severity 3: FailFast/RaceLost/LinkedExit */
    220, /* severity 4: Parent/Resource */
    255  /* severity 5: Shutdown */
};

asx_budget asx_cancel_cleanup_budget(asx_cancel_kind kind) {
    asx_budget b = asx_budget_infinite();
    int sev = asx_cancel_severity(kind);
    if (sev < 0 || sev > 5) return b;
    b.poll_quota = quota_by_severity[sev];
    b.priority = priority_by_severity[sev];
    return b;
}

asx_cancel_reason asx_cancel_strengthen(const asx_cancel_reason *a, const asx_cancel_reason *b) {
    int sev_a, sev_b;
    asx_cancel_reason fallback;
    if (!a && !b) {
        fallback.kind = ASX_CANCEL_USER;
        fallback.timestamp = 0;
        fallback.origin_region = ASX_INVALID_ID;
        fallback.origin_task = ASX_INVALID_ID;
        fallback.message = NULL;
        fallback.cause = NULL;
        fallback.truncated = 0;
        return fallback;
    }
    if (!a) return *b;
    if (!b) return *a;
    sev_a = asx_cancel_severity(a->kind);
    sev_b = asx_cancel_severity(b->kind);
    if (sev_a > sev_b) return *a;
    if (sev_b > sev_a) return *b;
    /* Equal severity: earlier timestamp wins */
    if (a->timestamp <= b->timestamp) return *a;
    return *b;
}

/* -------------------------------------------------------------------
 * Cancel witness protocol (walking skeleton)
 *
 * The walking skeleton provides slot-based witness tracking
 * for validation purposes. Full multi-threaded witness protocol
 * is deferred to Phase 4.
 * ------------------------------------------------------------------- */

#define ASX_MAX_CANCEL_WITNESSES 64u

typedef struct {
    asx_cancel_witness_id id;
    asx_cancel_phase phase;
    asx_cancel_reason reason;
    uint16_t generation;
    int active;
} asx_witness_slot;

static asx_witness_slot g_witnesses[ASX_MAX_CANCEL_WITNESSES];
static uint32_t g_witness_count = 0;

static asx_witness_slot *witness_find(asx_cancel_witness_id w) {
    uint32_t i;
    if (w == ASX_INVALID_ID) return NULL;
    for (i = 0; i < g_witness_count; i++) {
        if (g_witnesses[i].id == w && g_witnesses[i].active) return &g_witnesses[i];
    }
    return NULL;
}

static uint32_t g_witness_next_id = 1;

asx_status asx_cancel_witness_create(asx_cancel_witness_id *out, asx_task_id task,
                                      const asx_cancel_reason *reason) {
    uint32_t i;
    asx_witness_slot *s;

    if (out == NULL || reason == NULL) return ASX_E_INVALID_ARGUMENT;
    *out = ASX_INVALID_ID;
    (void)task;

    /* Find free slot */
    for (i = 0; i < ASX_MAX_CANCEL_WITNESSES; i++) {
        if (!g_witnesses[i].active) {
            s = &g_witnesses[i];
            s->generation++;
            if (s->generation == 0) s->generation = 1;
            s->id = (asx_cancel_witness_id)g_witness_next_id++;
            s->phase = ASX_CANCEL_PHASE_REQUESTED;
            s->reason = *reason;
            s->active = 1;
            if (i >= g_witness_count) g_witness_count = i + 1;
            *out = s->id;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_cancel_witness_advance(asx_cancel_witness_id w, asx_cancel_phase new_phase) {
    asx_witness_slot *s;

    s = witness_find(w);
    if (s == NULL) return ASX_E_NOT_FOUND;
    /* Phase must be monotone non-decreasing */
    if ((int)new_phase <= (int)s->phase) return ASX_E_INVALID_TRANSITION;
    s->phase = new_phase;
    return ASX_OK;
}

asx_status asx_cancel_witness_release(asx_cancel_witness_id w) {
    asx_witness_slot *s;

    s = witness_find(w);
    if (s == NULL) return ASX_E_NOT_FOUND;
    s->active = 0;
    return ASX_OK;
}

asx_status asx_cancel_witness_phase(asx_cancel_witness_id w, asx_cancel_phase *out_phase) {
    asx_witness_slot *s;
    if (out_phase == NULL) return ASX_E_INVALID_ARGUMENT;
    s = witness_find(w);
    if (s == NULL) return ASX_E_NOT_FOUND;
    *out_phase = s->phase;
    return ASX_OK;
}

asx_status asx_cancel_witness_reason(asx_cancel_witness_id w, asx_cancel_reason *out_reason) {
    asx_witness_slot *s;
    if (out_reason == NULL) return ASX_E_INVALID_ARGUMENT;
    s = witness_find(w);
    if (s == NULL) return ASX_E_NOT_FOUND;
    *out_reason = s->reason;
    return ASX_OK;
}

int asx_cancel_witness_is_valid(asx_cancel_witness_id w) {
    return witness_find(w) != NULL;
}

/* -------------------------------------------------------------------
 * User-facing cancellation request
 * ------------------------------------------------------------------- */

asx_status asx_cancel_request(asx_task_id task, const asx_cancel_reason *reason) {
    if (reason == NULL) return ASX_E_INVALID_ARGUMENT;
    if (task == ASX_INVALID_ID) return ASX_E_NOT_FOUND;

    /* Delegate to the runtime's task cancellation mechanism, which handles
     * state transitions, cleanup budget, witness creation, and ghost checks. */
    return asx_task_cancel(task, reason->kind);
}

/* -------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------- */

const char *asx_cancel_kind_str(asx_cancel_kind kind) {
    switch (kind) {
    case ASX_CANCEL_USER: return "user";
    case ASX_CANCEL_TIMEOUT: return "timeout";
    case ASX_CANCEL_DEADLINE: return "deadline";
    case ASX_CANCEL_POLL_QUOTA: return "poll_quota";
    case ASX_CANCEL_COST_BUDGET: return "cost_budget";
    case ASX_CANCEL_FAIL_FAST: return "fail_fast";
    case ASX_CANCEL_RACE_LOST: return "race_lost";
    case ASX_CANCEL_LINKED_EXIT: return "linked_exit";
    case ASX_CANCEL_PARENT: return "parent";
    case ASX_CANCEL_RESOURCE: return "resource";
    case ASX_CANCEL_SHUTDOWN: return "shutdown";
    default: return "unknown";
    }
}

const char *asx_cancel_phase_str(asx_cancel_phase phase) {
    switch (phase) {
    case ASX_CANCEL_PHASE_REQUESTED: return "requested";
    case ASX_CANCEL_PHASE_CANCELLING: return "cancelling";
    case ASX_CANCEL_PHASE_FINALIZING: return "finalizing";
    case ASX_CANCEL_PHASE_COMPLETED: return "completed";
    default: return "unknown";
    }
}
