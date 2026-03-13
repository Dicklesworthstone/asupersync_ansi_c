/*
 * test_safety_posture.c — security and safety posture tests (bd-hwb.9)
 *
 * Tests for: pointer ownership safety, stale-handle misuse defenses,
 * illegal transitions with invalid handles, malformed-input boundaries,
 * region poison/containment, and deterministic failure behavior.
 *
 * Threat model coverage:
 *   IN-SCOPE (kernel responsibility):
 *     - Handle generation validation (stale-handle detection)
 *     - Type-tag enforcement (cross-entity misuse rejected)
 *     - State transition legality (ghost protocol monitor)
 *     - Obligation linearity (ghost linearity monitor)
 *     - Region poison containment (bounded failure)
 *     - Null/invalid argument rejection at API boundary
 *
 *   OUT-OF-SCOPE (user responsibility):
 *     - Thread-safety (tracked by affinity guards, not enforced here)
 *     - User-data pointer validity (kernel never dereferences user_data)
 *     - Poll function correctness (user code)
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/core/transition.h>
#include <asx/runtime/runtime.h>

/* Internal header for direct arena access in tests */
#include "../../../src/runtime/runtime_internal.h"

/* Suppress warn_unused_result by assigning to volatile */
#define IGNORE_RESULT(expr)                                                                        \
    do {                                                                                           \
        volatile asx_status _ign = (expr);                                                         \
        (void)_ign;                                                                                \
    } while (0)

/* ---- Poll helpers ---- */

static asx_status noop_poll(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

static asx_status pending_poll(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

/* ==================================================================
 * Section 1: Crafted handle misuse
 * ================================================================== */

TEST(crafted_handle_wrong_type_tag_region) {
    asx_region_state state;
    uint64_t fake = asx_handle_pack(ASX_TYPE_TASK, 0x0001, asx_handle_pack_index(0, 0));
    ASSERT_EQ(asx_region_get_state(fake, &state), ASX_E_NOT_FOUND);
}

TEST(crafted_handle_wrong_type_tag_task) {
    asx_task_state tstate;
    uint64_t fake = asx_handle_pack(ASX_TYPE_REGION, 0x0001, asx_handle_pack_index(0, 0));
    ASSERT_EQ(asx_task_get_state(fake, &tstate), ASX_E_NOT_FOUND);
}

TEST(crafted_handle_wrong_type_tag_obligation) {
    asx_obligation_state ostate;
    uint64_t fake = asx_handle_pack(ASX_TYPE_TIMER, 0x0001, asx_handle_pack_index(0, 0));
    ASSERT_EQ(asx_obligation_get_state(fake, &ostate), ASX_E_NOT_FOUND);
}

TEST(crafted_handle_out_of_range_slot) {
    asx_region_state state;
    uint64_t fake = asx_handle_pack(ASX_TYPE_REGION, 0x0001, asx_handle_pack_index(0, 255));
    ASSERT_EQ(asx_region_get_state(fake, &state), ASX_E_NOT_FOUND);
}

TEST(crafted_handle_dead_slot) {
    asx_region_state state;
    uint64_t fake = asx_handle_pack(ASX_TYPE_REGION, 0x0001, asx_handle_pack_index(0, 0));
    ASSERT_EQ(asx_region_get_state(fake, &state), ASX_E_NOT_FOUND);
}

TEST(crafted_handle_wrong_generation) {
    asx_region_id rid;
    asx_region_state state;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    {
        uint64_t fake = asx_handle_pack(ASX_TYPE_REGION, 0x0001, asx_handle_pack_index(99, 0));
        ASSERT_EQ(asx_region_get_state(fake, &state), ASX_E_STALE_HANDLE);
    }
}

TEST(zero_handle_rejected_everywhere) {
    asx_region_state rstate;
    asx_task_state tstate;
    asx_obligation_state ostate;
    asx_task_id tid;
    asx_obligation_id oid;
    asx_outcome outcome;

    ASSERT_EQ(asx_region_get_state(ASX_INVALID_ID, &rstate), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_close(ASX_INVALID_ID), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_task_get_state(ASX_INVALID_ID, &tstate), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_task_get_outcome(ASX_INVALID_ID, &outcome), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_task_spawn(ASX_INVALID_ID, noop_poll, NULL, &tid), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_get_state(ASX_INVALID_ID, &ostate), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_commit(ASX_INVALID_ID), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_abort(ASX_INVALID_ID), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_reserve(ASX_INVALID_ID, &oid), ASX_E_NOT_FOUND);
}

/* ==================================================================
 * Section 2: Null pointer boundary
 * ================================================================== */

TEST(null_out_pointers_rejected) {
    asx_region_id rid;

    ASSERT_EQ(asx_region_open(NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_get_state(ASX_INVALID_ID, NULL), ASX_E_INVALID_ARGUMENT);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_spawn(rid, NULL, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_get_state(ASX_INVALID_ID, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_get_outcome(ASX_INVALID_ID, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_reserve(rid, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_get_state(ASX_INVALID_ID, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(null_poll_fn_rejected) {
    asx_region_id rid;
    asx_task_id tid;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, NULL, NULL, &tid), ASX_E_INVALID_ARGUMENT);
}

TEST(null_budget_rejected) {
    asx_region_id rid;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(rid, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_drain(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ==================================================================
 * Section 3: Stale handle misuse across operations
 * ================================================================== */

TEST(stale_handle_spawn_after_recycle) {
    asx_region_id rid1, rid2;
    asx_task_id tid;
    asx_budget budget;

    ASSERT_EQ(asx_region_open(&rid1), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid1, &budget), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid2), ASX_OK);

    ASSERT_EQ(asx_task_spawn(rid1, noop_poll, NULL, &tid), ASX_E_STALE_HANDLE);
}

TEST(stale_handle_close_after_recycle) {
    asx_region_id rid1, rid2;
    asx_budget budget;

    ASSERT_EQ(asx_region_open(&rid1), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid1, &budget), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid2), ASX_OK);

    ASSERT_EQ(asx_region_close(rid1), ASX_E_STALE_HANDLE);
}

TEST(stale_handle_obligation_reserve_after_recycle) {
    asx_region_id rid1, rid2;
    asx_obligation_id oid;
    asx_budget budget;

    ASSERT_EQ(asx_region_open(&rid1), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid1, &budget), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid2), ASX_OK);

    ASSERT_EQ(asx_obligation_reserve(rid1, &oid), ASX_E_STALE_HANDLE);
}

TEST(stale_handle_scheduler_after_recycle) {
    asx_region_id rid1, rid2;
    asx_budget budget;

    ASSERT_EQ(asx_region_open(&rid1), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid1, &budget), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid2), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid1, &budget), ASX_E_STALE_HANDLE);
}

/* ==================================================================
 * Section 4: Region poison / containment
 * ================================================================== */

TEST(poison_blocks_spawn) {
    asx_region_id rid;
    asx_task_id tid;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid), ASX_E_REGION_POISONED);
}

TEST(poison_blocks_close) {
    asx_region_id rid;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_E_REGION_POISONED);
}

TEST(poison_blocks_obligation_reserve) {
    asx_region_id rid;
    asx_obligation_id oid;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_E_REGION_POISONED);
}

TEST(poison_allows_state_query) {
    asx_region_id rid;
    asx_region_state state;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_region_get_state(rid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);
}

TEST(poison_is_queryable) {
    asx_region_id rid;
    int is_poisoned;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_is_poisoned(rid, &is_poisoned), ASX_OK);
    ASSERT_EQ(is_poisoned, 0);

    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_region_is_poisoned(rid, &is_poisoned), ASX_OK);
    ASSERT_EQ(is_poisoned, 1);
}

TEST(poison_is_idempotent) {
    asx_region_id rid;
    int is_poisoned;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_region_is_poisoned(rid, &is_poisoned), ASX_OK);
    ASSERT_EQ(is_poisoned, 1);
}

TEST(poison_on_invalid_handle_fails) {
    ASSERT_EQ(asx_region_poison(ASX_INVALID_ID), ASX_E_NOT_FOUND);
}

TEST(poison_is_poisoned_null_out) {
    asx_region_id rid;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_is_poisoned(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ==================================================================
 * Section 5: Illegal transition attempts
 * ================================================================== */

TEST(double_close_rejected) {
    asx_region_id rid;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_E_INVALID_TRANSITION);
}

TEST(spawn_on_closed_region_rejected) {
    asx_region_id rid;
    asx_task_id tid;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid), ASX_E_REGION_NOT_OPEN);
}

TEST(obligation_double_commit_rejected) {
    asx_region_id rid;
    asx_obligation_id oid;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_E_INVALID_TRANSITION);
}

TEST(obligation_commit_then_abort_rejected) {
    asx_region_id rid;
    asx_obligation_id oid;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_E_INVALID_TRANSITION);
}

TEST(task_outcome_before_completion_rejected) {
    asx_region_id rid;
    asx_task_id tid;
    asx_outcome outcome;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, pending_poll, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(tid, &outcome), ASX_E_TASK_NOT_COMPLETED);
}

/* ==================================================================
 * Section 6: Ghost violation recording under misuse
 * ================================================================== */

TEST(ghost_records_illegal_region_transition) {
    asx_region_id rid;
    uint32_t violations_before;
    uint32_t violations_after;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);

    violations_before = asx_ghost_violation_count();
    IGNORE_RESULT(asx_region_close(rid));
    violations_after = asx_ghost_violation_count();

    ASSERT_TRUE(violations_after > violations_before);
}

TEST(ghost_records_obligation_double_resolution) {
    asx_region_id rid;
    asx_obligation_id oid;
    uint32_t violations_before;
    uint32_t violations_after;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);

    violations_before = asx_ghost_violation_count();
    IGNORE_RESULT(asx_obligation_commit(oid));
    violations_after = asx_ghost_violation_count();

    ASSERT_TRUE(violations_after > violations_before);
}

/* ==================================================================
 * Section 7: Deterministic containment behavior
 * ================================================================== */

TEST(poison_status_string_available) {
    const char *s = asx_status_str(ASX_E_REGION_POISONED);
    ASSERT_TRUE(s != NULL);
    ASSERT_STR_EQ(s, "region poisoned");
}

TEST(stale_handle_error_is_deterministic) {
    asx_region_id rid1, rid2;
    asx_region_state state;
    asx_budget budget;
    int i;

    ASSERT_EQ(asx_region_open(&rid1), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid1, &budget), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid2), ASX_OK);

    for (i = 0; i < 5; i++) { ASSERT_EQ(asx_region_get_state(rid1, &state), ASX_E_STALE_HANDLE); }
}

TEST(poison_does_not_corrupt_region_state) {
    asx_region_id rid;
    asx_region_state state;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_get_state(rid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);

    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_region_get_state(rid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);
}

TEST(poison_reset_clears_on_new_region) {
    asx_region_id rid;
    int is_poisoned;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    asx_runtime_reset();
    asx_ghost_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_region_is_poisoned(rid, &is_poisoned), ASX_OK);
    ASSERT_EQ(is_poisoned, 0);
}

TEST(containment_policy_mapping_by_profile) {
    ASSERT_EQ(asx_containment_policy_for_profile(ASX_SAFETY_DEBUG), ASX_CONTAIN_FAIL_FAST);
    ASSERT_EQ(asx_containment_policy_for_profile(ASX_SAFETY_HARDENED), ASX_CONTAIN_POISON_REGION);
    ASSERT_EQ(asx_containment_policy_for_profile(ASX_SAFETY_RELEASE), ASX_CONTAIN_ERROR_ONLY);
}

TEST(contain_fault_returns_fault_and_follows_active_policy) {
    asx_region_id rid;
    asx_containment_policy policy;
    asx_status fault = ASX_E_INVALID_STATE;
    int is_poisoned;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    policy = asx_containment_policy_active();

    ASSERT_EQ(asx_region_contain_fault(rid, fault), fault);
    ASSERT_EQ(asx_region_is_poisoned(rid, &is_poisoned), ASX_OK);

    if (policy == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ(is_poisoned, 1);
    } else {
        ASSERT_EQ(is_poisoned, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_safety_posture ===\n");

    /* Section 1: Crafted handle misuse */
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(crafted_handle_wrong_type_tag_region);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(crafted_handle_wrong_type_tag_task);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(crafted_handle_wrong_type_tag_obligation);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(crafted_handle_out_of_range_slot);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(crafted_handle_dead_slot);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(crafted_handle_wrong_generation);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(zero_handle_rejected_everywhere);

    /* Section 2: Null pointer boundary */
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(null_out_pointers_rejected);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(null_poll_fn_rejected);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(null_budget_rejected);

    /* Section 3: Stale handle misuse */
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(stale_handle_spawn_after_recycle);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(stale_handle_close_after_recycle);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(stale_handle_obligation_reserve_after_recycle);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(stale_handle_scheduler_after_recycle);

    /* Section 4: Region poison / containment */
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(poison_blocks_spawn);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(poison_blocks_close);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(poison_blocks_obligation_reserve);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(poison_allows_state_query);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(poison_is_queryable);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(poison_is_idempotent);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(poison_on_invalid_handle_fails);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(poison_is_poisoned_null_out);

    /* Section 5: Illegal transitions */
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(double_close_rejected);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(spawn_on_closed_region_rejected);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(obligation_double_commit_rejected);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(obligation_commit_then_abort_rejected);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(task_outcome_before_completion_rejected);

    /* Section 6: Ghost violation recording */
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(ghost_records_illegal_region_transition);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(ghost_records_obligation_double_resolution);

    /* Section 7: Deterministic containment */
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(poison_status_string_available);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(stale_handle_error_is_deterministic);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(poison_does_not_corrupt_region_state);
    RUN_TEST(poison_reset_clears_on_new_region);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(containment_policy_mapping_by_profile);
    asx_runtime_reset();
    asx_ghost_reset();
    RUN_TEST(contain_fault_returns_fault_and_follows_active_policy);

    TEST_REPORT();
    return test_failures;
}
