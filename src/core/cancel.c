/*
 * cancel.c — cancellation cleanup budget and strengthen implementation
 *
 * Severity lookup is provided by the inline asx_cancel_severity() in asx_ids.h.
 * This file implements cleanup budget construction and reason strengthening.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/cancel.h>
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
