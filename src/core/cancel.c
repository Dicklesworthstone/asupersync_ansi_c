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
#include <string.h>

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

/* Whether candidate `b` replaces current reason `a` (Rust
 * CancelReason::strengthen, types/cancel.rs:956-1008; vocabulary §4):
 * the more severe kind wins; on equal severity the earlier timestamp wins;
 * on equal timestamps a message beats none and the smaller message (byte
 * order) beats the larger. Otherwise the current reason is kept. */
static int strengthen_replaces(const asx_cancel_reason *a, const asx_cancel_reason *b) {
    int sev_a = asx_cancel_severity(a->kind);
    int sev_b = asx_cancel_severity(b->kind);
    if (sev_b != sev_a) return sev_b > sev_a;
    if (b->timestamp != a->timestamp) return b->timestamp < a->timestamp;
    if (b->message == NULL) return 0;
    if (a->message == NULL) return 1;
    return strcmp(b->message, a->message) < 0;
}

asx_cancel_reason asx_cancel_strengthen(const asx_cancel_reason *a, const asx_cancel_reason *b) {
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
#ifdef ASX_MUTANT_CANCEL_NO_STRENGTHEN
    /* Oracle self-test mutant (tests/gates/test_oracle_mutants.sh): the
     * current reason is never replaced. Never in a release build. */
    (void)strengthen_replaces(a, b);
    return *a;
#else
    return strengthen_replaces(a, b) ? *b : *a;
#endif
}

/* -------------------------------------------------------------------
 * Cancel witness protocol (walking skeleton)
 *
 * The walking skeleton provides slot-based witness tracking
 * for validation purposes. Full multi-threaded witness protocol
 * is deferred to Phase 4.
 * ------------------------------------------------------------------- */

#if (ASX_MAX_CANCEL_WITNESSES) < 1
#error "ASX_MAX_CANCEL_WITNESSES must be at least 1"
#endif

typedef struct {
    asx_cancel_witness_id id;
    asx_cancel_phase phase;
    asx_cancel_reason reason;
    asx_task_id task;     /* the task cancelled (Rust CancelWitness::task_id) */
    asx_region_id region; /* its region (region_id); ASX_INVALID_ID if unknown */
    uint32_t epoch;       /* its cancel epoch (epoch); 0 if unknown */
    uint16_t generation;
    int active;
} asx_witness_slot;

static asx_witness_slot g_witnesses[ASX_MAX_CANCEL_WITNESSES];
static uint32_t g_witness_count = 0;

static asx_witness_slot *witness_find(asx_cancel_witness_id w) {
    uint32_t i;
    if (w == ASX_INVALID_ID) return NULL;
    for (i = 0; i < g_witness_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
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

    /* Find free slot */
    for (i = 0; i < ASX_MAX_CANCEL_WITNESSES; i++) {
        if (!g_witnesses[i].active) {
            s = &g_witnesses[i];
            s->generation++;
            if (s->generation == 0) s->generation = 1;
            s->id = (asx_cancel_witness_id)g_witness_next_id++;
            s->phase = ASX_CANCEL_PHASE_REQUESTED;
            s->reason = *reason;
            /* The identity Rust's witness carries, read from the runtime's
             * task record: a task the runtime holds no record for has
             * none. The runtime creates its witnesses after it sets the
             * epoch. */
            s->task = task;
            if (asx_task_get_region(task, &s->region) != ASX_OK) s->region = ASX_INVALID_ID;
            if (asx_task_get_cancel_epoch(task, &s->epoch) != ASX_OK) s->epoch = 0u;
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

int asx_cancel_witness_is_valid(asx_cancel_witness_id w) { return witness_find(w) != NULL; }

/* -------------------------------------------------------------------
 * User-facing cancellation request
 * ------------------------------------------------------------------- */

asx_status asx_cancel_request(asx_task_id task, const asx_cancel_reason *reason) {
    if (reason == NULL) return ASX_E_INVALID_ARGUMENT;
    if (task == ASX_INVALID_ID) return ASX_E_NOT_FOUND;

    /* Delegate to the runtime's task cancellation mechanism, which handles
     * state transitions, cleanup budget, witness creation, and ghost checks.
     * The whole reason travels (origin, timestamp, message, cause). */
    return asx_task_cancel_with_reason(task, reason);
}

/* -------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------- */

/* Rust's `impl Display for CancelKind` (types/cancel.rs:485-501). */
const char *asx_cancel_kind_str(asx_cancel_kind kind) {
    switch (kind) {
    case ASX_CANCEL_USER: return "user";
    case ASX_CANCEL_TIMEOUT: return "timeout";
    case ASX_CANCEL_DEADLINE: return "deadline";
    case ASX_CANCEL_POLL_QUOTA: return "poll quota";
    case ASX_CANCEL_COST_BUDGET: return "cost budget";
    case ASX_CANCEL_FAIL_FAST: return "fail-fast";
    case ASX_CANCEL_RACE_LOST: return "race lost";
    case ASX_CANCEL_LINKED_EXIT: return "linked exit";
    case ASX_CANCEL_PARENT: return "parent cancelled";
    case ASX_CANCEL_RESOURCE: return "resource unavailable";
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

/* -------------------------------------------------------------------
 * Cause-chain bound (Rust CancelAttributionConfig, types/cancel.rs:184-262;
 * CancelReason::with_cause_limited, :706-761)
 * ------------------------------------------------------------------- */

/* Rust's single_reason_cost() and the per-link Box overhead
 * (estimated_chain_cost: 80 * depth + 8 * (depth - 1)). */
#define CHAIN_REASON_COST 80u
#define CHAIN_LINK_COST 8u
/* The most links a walk follows in a chain no bound cut: a guard against a
 * cyclic chain a caller built. */
#define CHAIN_WALK_MAX 1024u

/* CancelAttributionConfig::default(): DEFAULT_MAX_DEPTH, DEFAULT_MAX_MEMORY.
 * The runtime config's defaults (hooks.c) are the same pair. */
#define CHAIN_DEFAULT_DEPTH 16u
#define CHAIN_DEFAULT_MEMORY 4096u

static uint32_t g_chain_max_depth = CHAIN_DEFAULT_DEPTH;
static uint32_t g_chain_max_memory = CHAIN_DEFAULT_MEMORY;

static uint64_t chain_cost(uint64_t depth) {
    if (depth == 0u) return 0u;
    return depth * (CHAIN_REASON_COST + CHAIN_LINK_COST) - CHAIN_LINK_COST;
}

uint32_t asx_cancel_chain_link_limit(uint32_t max_depth, uint32_t max_memory) {
    uint32_t by_depth = max_depth > 0u ? max_depth - 1u : 0u;
    uint32_t by_memory = 0u;
    if (max_memory >= CHAIN_REASON_COST) {
        by_memory = (max_memory - CHAIN_REASON_COST) / (CHAIN_REASON_COST + CHAIN_LINK_COST);
    }
    return by_depth < by_memory ? by_depth : by_memory;
}

void asx_cancel_chain_limits_set_internal(uint32_t max_depth, uint32_t max_memory) {
    g_chain_max_depth = max_depth;
    g_chain_max_memory = max_memory;
}

void asx_cancel_chain_limits_reset_internal(void) {
    g_chain_max_depth = CHAIN_DEFAULT_DEPTH;
    g_chain_max_memory = CHAIN_DEFAULT_MEMORY;
}

uint32_t asx_cancel_reason_chain_links(const asx_cancel_reason *r) {
    const asx_cancel_reason *c;
    uint32_t limit;
    uint32_t n = 0u;

    if (r == NULL) return 0u;
    limit = r->truncated ? asx_cancel_chain_link_limit(g_chain_max_depth, g_chain_max_memory)
                         : CHAIN_WALK_MAX;
    for (c = r->cause; c != NULL && n < limit; c = c->cause) {
        /* ASX_CHECKPOINT_WAIVER("bounded: n < CHAIN_WALK_MAX") */
        n++;
    }
    return n;
}

void asx_cancel_reason_with_cause_limited(asx_cancel_reason *r, const asx_cancel_reason *cause) {
    uint64_t total;

    if (r == NULL) return;
    if (cause == NULL) {
        r->cause = NULL;
        return;
    }
    /* The reason itself, the cause, and the cause's own links
     * (with_cause_limited: current_depth 1 + cause.chain_depth()). */
    total = (uint64_t)asx_cancel_reason_chain_links(cause) + 2u;
    if (total > g_chain_max_depth || chain_cost(total) > g_chain_max_memory) {
        r->truncated = 1;
        r->cause = asx_cancel_chain_link_limit(g_chain_max_depth, g_chain_max_memory) == 0u
                       ? NULL
                       : (asx_cancel_reason *)cause;
        return;
    }
    r->cause = (asx_cancel_reason *)cause;
}

/* -------------------------------------------------------------------
 * Witness values (Rust CancelWitness, types/cancel.rs:323-412)
 * ------------------------------------------------------------------- */

asx_status asx_cancel_witness_info_get(asx_cancel_witness_id w, asx_cancel_witness_info *out) {
    asx_witness_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = witness_find(w);
    if (s == NULL) return ASX_E_NOT_FOUND;
    out->task = s->task;
    out->region = s->region;
    out->epoch = s->epoch;
    out->phase = s->phase;
    out->reason = s->reason;
    return ASX_OK;
}

asx_status asx_cancel_witness_validate_initial(const asx_cancel_witness_info *w) {
    if (w == NULL) return ASX_E_INVALID_ARGUMENT;
    /* validate_initial (:369-373): epoch 0 is the "no cancel ever
     * requested" sentinel. */
    if (w->epoch == 0u) return ASX_E_INVALID_TRANSITION;
    return ASX_OK;
}

asx_status asx_cancel_witness_validate_transition(const asx_cancel_witness_info *prev,
                                                  const asx_cancel_witness_info *next) {
    if (next == NULL) return ASX_E_INVALID_ARGUMENT;
    if (prev == NULL) return ASX_OK;
    /* validate_transition (:382-411), in Rust's order. */
    if (prev->task != next->task) return ASX_E_WITNESS_TASK_MISMATCH;
    if (prev->region != next->region) return ASX_E_WITNESS_REGION_MISMATCH;
    if (prev->epoch != next->epoch) return ASX_E_WITNESS_EPOCH_MISMATCH;
    if ((int)next->phase < (int)prev->phase) return ASX_E_WITNESS_PHASE_REGRESSION;
    if (asx_cancel_severity(next->reason.kind) < asx_cancel_severity(prev->reason.kind)) {
        return ASX_E_WITNESS_REASON_WEAKENED;
    }
    return ASX_OK;
}
