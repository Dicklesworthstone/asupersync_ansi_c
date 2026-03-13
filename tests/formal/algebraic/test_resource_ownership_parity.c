/*
 * test_resource_ownership_parity.c — comprehensive parity tests for resource,
 *     ownership, task-context, and builder/error contracts (bd-1eqo.15.2)
 *
 * Covers gaps not hit by existing unit tests:
 *   - Resource snapshot invariants (capacity >= used + remaining)
 *   - Resource admission gate monotonicity and idempotence
 *   - Error ledger multi-task isolation and overflow semantics
 *   - Ghost borrow monitor ownership contracts
 *   - Checkpoint result semantics in cancel lifecycle
 *   - Cross-API integration (resource + cancel, checkpoint + ghost)
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/core/resource.h>
#include <asx/core/transition.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* Suppress unused-result for intentionally-ignored scheduler calls */
#define IGNORE(expr)                                                                               \
    do {                                                                                           \
        asx_status s_ = (expr);                                                                    \
        (void)s_;                                                                                  \
    } while (0)

static asx_status poll_checkpoint_then_complete(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)data;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) { return ASX_OK; }
    return ASX_E_PENDING;
}

static asx_status poll_always_pending(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

/* ================================================================== */
/* Section 1: Resource snapshot invariants                             */
/* ================================================================== */

TEST(resource_snapshot_capacity_equals_sum) {
    /* capacity == used + remaining, always */
    asx_resource_kind kinds[] = {ASX_RESOURCE_REGION, ASX_RESOURCE_TASK, ASX_RESOURCE_OBLIGATION};
    unsigned i;

    asx_runtime_reset();

    for (i = 0; i < 3; i++) {
        asx_resource_snapshot snap;
        ASSERT_EQ(asx_resource_snapshot_get(kinds[i], &snap), ASX_OK);
        ASSERT_EQ(snap.capacity, snap.used + snap.remaining);
    }
}

TEST(resource_snapshot_after_allocation) {
    asx_region_id rid;
    asx_resource_snapshot before, after;

    asx_runtime_reset();

    ASSERT_EQ(asx_resource_snapshot_get(ASX_RESOURCE_REGION, &before), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_resource_snapshot_get(ASX_RESOURCE_REGION, &after), ASX_OK);

    ASSERT_EQ(after.used, before.used + 1);
    ASSERT_EQ(after.remaining, before.remaining - 1);
    ASSERT_EQ(after.capacity, before.capacity);
}

TEST(resource_snapshot_invalid_kind) {
    asx_resource_snapshot snap;
    ASSERT_EQ(asx_resource_snapshot_get((asx_resource_kind)99, &snap), ASX_E_INVALID_ARGUMENT);
}

TEST(resource_snapshot_null_output) {
    ASSERT_EQ(asx_resource_snapshot_get(ASX_RESOURCE_REGION, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(resource_queries_consistent) {
    /* capacity(), used(), remaining() must agree with snapshot */
    asx_resource_kind kinds[] = {ASX_RESOURCE_REGION, ASX_RESOURCE_TASK, ASX_RESOURCE_OBLIGATION};
    unsigned i;

    asx_runtime_reset();

    for (i = 0; i < 3; i++) {
        uint32_t cap = asx_resource_capacity(kinds[i]);
        uint32_t used = asx_resource_used(kinds[i]);
        uint32_t rem = asx_resource_remaining(kinds[i]);
        ASSERT_EQ(cap, used + rem);
    }
}

/* ================================================================== */
/* Section 2: Admission gate contracts                                */
/* ================================================================== */

TEST(admission_idempotent) {
    /* Admission is a pure query — calling twice gives same result */
    asx_status s1, s2;

    asx_runtime_reset();

    s1 = asx_resource_admit(ASX_RESOURCE_REGION, 1);
    s2 = asx_resource_admit(ASX_RESOURCE_REGION, 1);
    ASSERT_EQ(s1, s2);
    ASSERT_EQ(s1, ASX_OK);
}

TEST(admission_rejects_zero_count) {
    asx_runtime_reset();
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_REGION, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(admission_rejects_unknown_kind) {
    asx_runtime_reset();
    ASSERT_EQ(asx_resource_admit((asx_resource_kind)99, 1), ASX_E_INVALID_ARGUMENT);
}

TEST(admission_rejects_over_capacity) {
    uint32_t cap;

    asx_runtime_reset();

    cap = asx_resource_capacity(ASX_RESOURCE_REGION);
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_REGION, cap + 1), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(admission_tracks_allocation) {
    /* After allocating regions, admission gate tightens */
    asx_region_id rid;
    uint32_t cap;
    asx_status s;

    asx_runtime_reset();

    cap = asx_resource_capacity(ASX_RESOURCE_REGION);
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_REGION, cap), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    /* Now one slot used — can't admit full capacity */
    s = asx_resource_admit(ASX_RESOURCE_REGION, cap);
    ASSERT_EQ(s, ASX_E_RESOURCE_EXHAUSTED);
    /* But can admit cap-1 */
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_REGION, cap - 1), ASX_OK);
}

/* ================================================================== */
/* Section 3: Error ledger task isolation                             */
/* ================================================================== */

TEST(error_ledger_task_isolation) {
    /* Errors recorded for task A do not appear in task B */
    asx_region_id rid;
    asx_task_id t1, t2;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_always_pending, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_always_pending, NULL, &t2), ASX_OK);

    /* Record error for t1 */
    asx_error_ledger_record_for_task(t1, ASX_E_CANCELLED, "test_op", __FILE__, __LINE__);

    ASSERT_EQ(asx_error_ledger_count(t1), 1u);
    ASSERT_EQ(asx_error_ledger_count(t2), 0u);
}

TEST(error_ledger_bind_switch) {
    /* Binding changes which task receives errors */
    asx_region_id rid;
    asx_task_id t1, t2;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_always_pending, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_always_pending, NULL, &t2), ASX_OK);

    asx_error_ledger_bind_task(t1);
    ASSERT_EQ(asx_error_ledger_bound_task(), t1);

    asx_error_ledger_bind_task(t2);
    ASSERT_EQ(asx_error_ledger_bound_task(), t2);
}

TEST(error_ledger_overflow_deterministic) {
    /* After 16+ entries, ring wraps but doesn't crash */
    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_always_pending, NULL, &tid), ASX_OK);

    for (i = 0; i < 20; i++) {
        asx_error_ledger_record_for_task(tid, ASX_E_CANCELLED, "overflow", __FILE__, __LINE__);
    }

    /* Count capped at ring depth */
    ASSERT_TRUE(asx_error_ledger_count(tid) <= 16u);
    ASSERT_TRUE(asx_error_ledger_overflowed(tid));
}

TEST(error_ledger_entry_fields) {
    /* Recorded entry preserves all fields */
    asx_region_id rid;
    asx_task_id tid;
    asx_error_ledger_entry entry;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_always_pending, NULL, &tid), ASX_OK);

    asx_error_ledger_record_for_task(tid, ASX_E_INVALID_ARGUMENT, "my_op", "myfile.c", 42);

    ASSERT_EQ(asx_error_ledger_count(tid), 1u);
    ASSERT_TRUE(asx_error_ledger_get(tid, 0, &entry));
    ASSERT_EQ(entry.task_id, tid);
    ASSERT_EQ(entry.status, ASX_E_INVALID_ARGUMENT);
    ASSERT_STR_EQ(entry.operation, "my_op");
    ASSERT_STR_EQ(entry.file, "myfile.c");
    ASSERT_EQ(entry.line, 42u);
}

/* ================================================================== */
/* Section 4: Ghost borrow ownership contracts                        */
/* ================================================================== */

TEST(ghost_borrow_shared_allows_multiple) {
    uint64_t entity = 0x1234;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_ghost_borrow_shared(entity), 1u);
    ASSERT_EQ(asx_ghost_borrow_shared(entity), 2u);
    ASSERT_EQ(asx_ghost_borrow_shared_count(entity), 2u);
    ASSERT_FALSE(asx_ghost_borrow_is_exclusive(entity));
}

TEST(ghost_borrow_exclusive_rejects_during_shared) {
    uint64_t entity = 0x5678;

    asx_runtime_reset();
    asx_ghost_reset();

    asx_ghost_borrow_shared(entity);
    /* Exclusive during active shared = conflict */
    ASSERT_FALSE(asx_ghost_borrow_exclusive(entity));
    /* Ghost records a violation */
    ASSERT_TRUE(asx_ghost_violation_count() > 0);
}

TEST(ghost_borrow_shared_rejects_during_exclusive) {
    uint64_t entity = 0xABCD;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_TRUE(asx_ghost_borrow_exclusive(entity));
    /* Shared during active exclusive = conflict */
    asx_ghost_borrow_shared(entity);
    ASSERT_TRUE(asx_ghost_violation_count() > 0);
}

TEST(ghost_borrow_release_all_reclaims_slot) {
    uint64_t entity = 0xDEAD;

    asx_runtime_reset();
    asx_ghost_reset();

    asx_ghost_borrow_shared(entity);
    asx_ghost_borrow_shared(entity);
    asx_ghost_borrow_release_all(entity);

    ASSERT_EQ(asx_ghost_borrow_shared_count(entity), 0u);
    ASSERT_FALSE(asx_ghost_borrow_is_exclusive(entity));
}

TEST(ghost_borrow_exclusive_after_full_release_succeeds) {
    uint64_t entity = 0xBEEF;

    asx_runtime_reset();
    asx_ghost_reset();

    asx_ghost_borrow_shared(entity);
    asx_ghost_borrow_release(entity);

    /* No borrows active — exclusive should succeed */
    ASSERT_TRUE(asx_ghost_borrow_exclusive(entity));
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

/* ================================================================== */
/* Section 5: Checkpoint result in cancel lifecycle                   */
/* ================================================================== */

TEST(checkpoint_non_cancelled_returns_clean) {
    asx_region_id rid;
    asx_task_id tid;
    asx_checkpoint_result cr;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_always_pending, NULL, &tid), ASX_OK);

    /* Prime task to Running */
    budget = asx_budget_from_polls(1);
    IGNORE(asx_scheduler_run(rid, &budget));

    /* Checkpoint on non-cancelled task: clean */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_FALSE(cr.cancelled);
}

TEST(checkpoint_after_cancel_shows_cancelled) {
    asx_region_id rid;
    asx_task_id tid;
    asx_checkpoint_result cr;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_always_pending, NULL, &tid), ASX_OK);

    /* Prime to Running */
    budget = asx_budget_from_polls(1);
    IGNORE(asx_scheduler_run(rid, &budget));

    /* Cancel the task */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    /* Checkpoint: should see cancelled */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_TRUE(cr.cancelled);
    ASSERT_EQ(cr.phase, ASX_CANCEL_PHASE_CANCELLING);
    ASSERT_EQ(cr.kind, ASX_CANCEL_USER);
}

TEST(checkpoint_invalid_task_returns_error) {
    asx_checkpoint_result cr;
    asx_runtime_reset();
    ASSERT_EQ(asx_checkpoint(0xDEADu, &cr), ASX_E_NOT_FOUND);
}

TEST(checkpoint_null_output_returns_error) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_always_pending, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    IGNORE(asx_scheduler_run(rid, &budget));

    ASSERT_EQ(asx_checkpoint(tid, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Section 6: Cancel strengthen in live tasks                         */
/* ================================================================== */

TEST(cancel_strengthen_upgrades_severity) {
    asx_region_id rid;
    asx_task_id tid;
    asx_checkpoint_result cr;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_always_pending, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    IGNORE(asx_scheduler_run(rid, &budget));

    /* Cancel with USER (severity 0) */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    /* Strengthen with SHUTDOWN (severity 5) */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);

    /* Checkpoint should show SHUTDOWN */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_TRUE(cr.cancelled);
    ASSERT_EQ(cr.kind, ASX_CANCEL_SHUTDOWN);
}

TEST(cancel_completed_task_is_noop) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);

    /* Run: prime, cancel, complete */
    budget = asx_budget_from_polls(1);
    IGNORE(asx_scheduler_run(rid, &budget));
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);
    budget = asx_budget_from_polls(16);
    IGNORE(asx_scheduler_run(rid, &budget));
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);

    /* Cancel on completed task: should succeed but be no-op */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
}

/* ================================================================== */
/* Section 7: Resource exhaustion and recovery                        */
/* ================================================================== */

TEST(resource_exhaustion_all_regions) {
    asx_region_id rids[8]; /* ASX_MAX_REGIONS = 8 */
    uint32_t cap;
    uint32_t i;
    asx_region_id extra;

    asx_runtime_reset();

    cap = asx_resource_capacity(ASX_RESOURCE_REGION);
    ASSERT_TRUE(cap <= 8);

    for (i = 0; i < cap; i++) { ASSERT_EQ(asx_region_open(&rids[i]), ASX_OK); }

    /* All slots used */
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_REGION), 0u);
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_REGION), cap);

    /* Next allocation should fail */
    ASSERT_EQ(asx_region_open(&extra), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(resource_kind_str_coverage) {
    ASSERT_TRUE(strcmp(asx_resource_kind_str(ASX_RESOURCE_REGION), "unknown") != 0);
    ASSERT_TRUE(strcmp(asx_resource_kind_str(ASX_RESOURCE_TASK), "unknown") != 0);
    ASSERT_TRUE(strcmp(asx_resource_kind_str(ASX_RESOURCE_OBLIGATION), "unknown") != 0);
    ASSERT_STR_EQ(asx_resource_kind_str((asx_resource_kind)99), "unknown");
}

/* ================================================================== */
/* Section 8: Ghost determinism monitor                               */
/* ================================================================== */

TEST(ghost_determinism_identical_runs_match) {
    asx_runtime_reset();
    asx_ghost_reset();

    /* Run 1 */
    asx_ghost_determinism_reset();
    asx_ghost_determinism_record(1);
    asx_ghost_determinism_record(2);
    asx_ghost_determinism_record(3);
    asx_ghost_determinism_seal();

    /* Run 2 (identical) */
    asx_ghost_determinism_record(1);
    asx_ghost_determinism_record(2);
    asx_ghost_determinism_record(3);

    ASSERT_EQ(asx_ghost_determinism_check(), 0u);
}

TEST(ghost_determinism_different_runs_drift) {
    uint32_t drifts;

    asx_runtime_reset();
    asx_ghost_reset();

    /* Run 1 */
    asx_ghost_determinism_reset();
    asx_ghost_determinism_record(1);
    asx_ghost_determinism_record(2);
    asx_ghost_determinism_record(3);
    asx_ghost_determinism_seal();

    /* Run 2 (different order) */
    asx_ghost_determinism_record(1);
    asx_ghost_determinism_record(3); /* swapped with 2 */
    asx_ghost_determinism_record(2);

    drifts = asx_ghost_determinism_check();
    ASSERT_TRUE(drifts > 0);
}

TEST(ghost_determinism_digest_stable) {
    uint64_t d1, d2;

    asx_runtime_reset();
    asx_ghost_reset();

    asx_ghost_determinism_reset();
    asx_ghost_determinism_record(42);
    asx_ghost_determinism_record(99);
    d1 = asx_ghost_determinism_digest();

    asx_ghost_determinism_reset();
    asx_ghost_determinism_record(42);
    asx_ghost_determinism_record(99);
    d2 = asx_ghost_determinism_digest();

    ASSERT_EQ(d1, d2);
}

/* ================================================================== */
/* Section 9: Ghost protocol monitor                                  */
/* ================================================================== */

TEST(ghost_protocol_legal_region_transition_clean) {
    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_ghost_check_region_transition(0x1234, ASX_REGION_OPEN, ASX_REGION_CLOSING),
              ASX_OK);
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

TEST(ghost_protocol_illegal_region_transition_records_violation) {
    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_ghost_check_region_transition(0x1234, ASX_REGION_CLOSED, ASX_REGION_OPEN),
              ASX_E_INVALID_TRANSITION);
    ASSERT_TRUE(asx_ghost_violation_count() > 0);

    {
        asx_ghost_violation v;
        ASSERT_TRUE(asx_ghost_violation_get(0, &v));
        ASSERT_EQ(v.kind, ASX_GHOST_PROTOCOL_REGION);
    }
}

/* ================================================================== */
/* main                                                               */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "[formal] resource/ownership/task-context parity (bd-1eqo.15.2)\n");

    /* Resource snapshot invariants */
    asx_runtime_reset();
    RUN_TEST(resource_snapshot_capacity_equals_sum);
    asx_runtime_reset();
    RUN_TEST(resource_snapshot_after_allocation);
    asx_runtime_reset();
    RUN_TEST(resource_snapshot_invalid_kind);
    asx_runtime_reset();
    RUN_TEST(resource_snapshot_null_output);
    asx_runtime_reset();
    RUN_TEST(resource_queries_consistent);

    /* Admission gate */
    asx_runtime_reset();
    RUN_TEST(admission_idempotent);
    asx_runtime_reset();
    RUN_TEST(admission_rejects_zero_count);
    asx_runtime_reset();
    RUN_TEST(admission_rejects_unknown_kind);
    asx_runtime_reset();
    RUN_TEST(admission_rejects_over_capacity);
    asx_runtime_reset();
    RUN_TEST(admission_tracks_allocation);

    /* Error ledger */
    asx_runtime_reset();
    RUN_TEST(error_ledger_task_isolation);
    asx_runtime_reset();
    RUN_TEST(error_ledger_bind_switch);
    asx_runtime_reset();
    RUN_TEST(error_ledger_overflow_deterministic);
    asx_runtime_reset();
    RUN_TEST(error_ledger_entry_fields);

    /* Ghost borrow */
    RUN_TEST(ghost_borrow_shared_allows_multiple);
    RUN_TEST(ghost_borrow_exclusive_rejects_during_shared);
    RUN_TEST(ghost_borrow_shared_rejects_during_exclusive);
    RUN_TEST(ghost_borrow_release_all_reclaims_slot);
    RUN_TEST(ghost_borrow_exclusive_after_full_release_succeeds);

    /* Checkpoint */
    asx_runtime_reset();
    RUN_TEST(checkpoint_non_cancelled_returns_clean);
    asx_runtime_reset();
    RUN_TEST(checkpoint_after_cancel_shows_cancelled);
    asx_runtime_reset();
    RUN_TEST(checkpoint_invalid_task_returns_error);
    asx_runtime_reset();
    RUN_TEST(checkpoint_null_output_returns_error);

    /* Cancel strengthen */
    asx_runtime_reset();
    RUN_TEST(cancel_strengthen_upgrades_severity);
    asx_runtime_reset();
    RUN_TEST(cancel_completed_task_is_noop);

    /* Resource exhaustion */
    asx_runtime_reset();
    RUN_TEST(resource_exhaustion_all_regions);
    asx_runtime_reset();
    RUN_TEST(resource_kind_str_coverage);

    /* Ghost determinism */
    RUN_TEST(ghost_determinism_identical_runs_match);
    RUN_TEST(ghost_determinism_different_runs_drift);
    RUN_TEST(ghost_determinism_digest_stable);

    /* Ghost protocol */
    RUN_TEST(ghost_protocol_legal_region_transition_clean);
    RUN_TEST(ghost_protocol_illegal_region_transition_records_violation);

    TEST_REPORT();
    return test_failures;
}
