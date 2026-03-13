/*
 * test_foundational_parity.c — comprehensive parity tests for foundational
 *                               type contracts (bd-1eqo.15.1)
 *
 * Fills gaps not covered by existing unit and algebraic tests:
 *   - Cancel strengthen NULL handling and reason chain construction
 *   - Budget deadline semantics, consume atomicity, NULL paths
 *   - Handle slot/generation decomposition round-trips
 *   - Exhaustive transition matrix verification (every cell)
 *   - Status code completeness and classification
 *   - Policy enum ordering and containment mapping
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
#include <asx/core/transition.h>
#include <string.h>

/* ================================================================== */
/* Section 1: Cancel strengthen — NULL and chain edge cases            */
/* ================================================================== */

TEST(cancel_strengthen_both_null_returns_user) {
    asx_cancel_reason result = asx_cancel_strengthen(NULL, NULL);
    ASSERT_EQ(result.kind, ASX_CANCEL_USER);
    ASSERT_EQ(result.timestamp, (asx_time)0);
    ASSERT_EQ(result.origin_region, ASX_INVALID_ID);
    ASSERT_EQ(result.origin_task, ASX_INVALID_ID);
}

TEST(cancel_strengthen_a_null_returns_b) {
    asx_cancel_reason b;
    asx_cancel_reason result;
    memset(&b, 0, sizeof(b));
    b.kind = ASX_CANCEL_SHUTDOWN;
    b.timestamp = 42;
    result = asx_cancel_strengthen(NULL, &b);
    ASSERT_EQ(result.kind, ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(result.timestamp, (asx_time)42);
}

TEST(cancel_strengthen_b_null_returns_a) {
    asx_cancel_reason a;
    asx_cancel_reason result;
    memset(&a, 0, sizeof(a));
    a.kind = ASX_CANCEL_PARENT;
    a.timestamp = 7;
    result = asx_cancel_strengthen(&a, NULL);
    ASSERT_EQ(result.kind, ASX_CANCEL_PARENT);
    ASSERT_EQ(result.timestamp, (asx_time)7);
}

TEST(cancel_reason_chain_linkage) {
    /* Verify cause chain struct field linkage works */
    asx_cancel_reason root;
    asx_cancel_reason child;
    memset(&root, 0, sizeof(root));
    memset(&child, 0, sizeof(child));

    root.kind = ASX_CANCEL_SHUTDOWN;
    root.timestamp = 1;
    root.cause = NULL;
    root.truncated = 0;

    child.kind = ASX_CANCEL_LINKED_EXIT;
    child.timestamp = 2;
    child.cause = &root;
    child.truncated = 0;

    /* Walk the chain */
    ASSERT_EQ(child.kind, ASX_CANCEL_LINKED_EXIT);
    ASSERT_TRUE(child.cause != NULL);
    ASSERT_EQ(child.cause->kind, ASX_CANCEL_SHUTDOWN);
    ASSERT_TRUE(child.cause->cause == NULL);
}

TEST(cancel_reason_chain_truncation_flag) {
    asx_cancel_reason r;
    memset(&r, 0, sizeof(r));
    r.kind = ASX_CANCEL_USER;
    r.truncated = 1;

    /* Truncated flag signals depth limit was hit */
    ASSERT_TRUE(r.truncated);
    ASSERT_TRUE(r.cause == NULL); /* chain was cut */
}

TEST(cancel_strengthen_preserves_attribution) {
    /* Verify that strengthen preserves origin_region/origin_task */
    asx_cancel_reason a;
    asx_cancel_reason b;
    asx_cancel_reason result;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    a.kind = ASX_CANCEL_SHUTDOWN;
    a.timestamp = 10;
    a.origin_region = 0xAAAA;
    a.origin_task = 0xBBBB;

    b.kind = ASX_CANCEL_USER;
    b.timestamp = 5;
    b.origin_region = 0xCCCC;
    b.origin_task = 0xDDDD;

    result = asx_cancel_strengthen(&a, &b);
    /* SHUTDOWN > USER severity, so a wins */
    ASSERT_EQ(result.origin_region, (asx_region_id)0xAAAA);
    ASSERT_EQ(result.origin_task, (asx_task_id)0xBBBB);
}

TEST(cancel_cleanup_budget_all_kinds_valid) {
    /* Every cancel kind produces a valid, non-infinite budget */
    int k;
    for (k = 0; k <= 10; k++) {
        asx_budget b = asx_cancel_cleanup_budget((asx_cancel_kind)k);
        ASSERT_TRUE(b.poll_quota > 0);
        ASSERT_TRUE(b.poll_quota < UINT32_MAX);
        ASSERT_TRUE(b.priority > 0);
    }
}

/* ================================================================== */
/* Section 2: Budget deadline and consume semantics                   */
/* ================================================================== */

TEST(budget_deadline_zero_means_unconstrained) {
    asx_budget b = asx_budget_infinite();
    ASSERT_EQ(b.deadline, (asx_time)0);
    /* deadline=0 is never past */
    ASSERT_FALSE(asx_budget_is_past_deadline(&b, 0));
    ASSERT_FALSE(asx_budget_is_past_deadline(&b, UINT64_MAX));
}

TEST(budget_deadline_finite_boundary) {
    asx_budget b = asx_budget_infinite();
    b.deadline = 100;
    /* now < deadline: not past */
    ASSERT_FALSE(asx_budget_is_past_deadline(&b, 99));
    /* now == deadline: past (>=) */
    ASSERT_TRUE(asx_budget_is_past_deadline(&b, 100));
    /* now > deadline: past */
    ASSERT_TRUE(asx_budget_is_past_deadline(&b, 101));
}

TEST(budget_deadline_one) {
    /* deadline=1 is the earliest finite deadline (used by budget_zero) */
    asx_budget b = asx_budget_zero();
    ASSERT_EQ(b.deadline, (asx_time)1);
    ASSERT_FALSE(asx_budget_is_past_deadline(&b, 0));
    ASSERT_TRUE(asx_budget_is_past_deadline(&b, 1));
}

TEST(budget_is_past_deadline_null_safe) { ASSERT_FALSE(asx_budget_is_past_deadline(NULL, 100)); }

TEST(budget_consume_poll_decrement) {
    asx_budget b = asx_budget_from_polls(3);
    ASSERT_EQ(asx_budget_consume_poll(&b), 3u);
    ASSERT_EQ(asx_budget_polls(&b), 2u);
    ASSERT_EQ(asx_budget_consume_poll(&b), 2u);
    ASSERT_EQ(asx_budget_polls(&b), 1u);
    ASSERT_EQ(asx_budget_consume_poll(&b), 1u);
    ASSERT_EQ(asx_budget_polls(&b), 0u);
    /* Exhausted: returns 0 */
    ASSERT_EQ(asx_budget_consume_poll(&b), 0u);
    ASSERT_EQ(asx_budget_polls(&b), 0u);
}

TEST(budget_consume_poll_null_safe) { ASSERT_EQ(asx_budget_consume_poll(NULL), 0u); }

TEST(budget_consume_cost_atomic) {
    asx_budget b = asx_budget_infinite();
    b.cost_quota = 100;

    /* Successful consume */
    ASSERT_TRUE(asx_budget_consume_cost(&b, 30));
    ASSERT_EQ(b.cost_quota, (uint64_t)70);

    /* Exact exhaust */
    ASSERT_TRUE(asx_budget_consume_cost(&b, 70));
    ASSERT_EQ(b.cost_quota, (uint64_t)0);

    /* Over-consume: fails without mutation */
    ASSERT_FALSE(asx_budget_consume_cost(&b, 1));
    ASSERT_EQ(b.cost_quota, (uint64_t)0);
}

TEST(budget_consume_cost_unconstrained_always_succeeds) {
    asx_budget b = asx_budget_infinite();
    /* UINT64_MAX cost_quota = unconstrained */
    ASSERT_TRUE(asx_budget_consume_cost(&b, 999999));
    ASSERT_EQ(b.cost_quota, UINT64_MAX); /* unchanged */
}

TEST(budget_consume_cost_null_safe) { ASSERT_FALSE(asx_budget_consume_cost(NULL, 1)); }

TEST(budget_is_exhausted_poll_zero) {
    asx_budget b = asx_budget_from_polls(0);
    ASSERT_TRUE(asx_budget_is_exhausted(&b));
}

TEST(budget_is_exhausted_cost_zero) {
    asx_budget b = asx_budget_infinite();
    b.cost_quota = 0;
    ASSERT_TRUE(asx_budget_is_exhausted(&b));
}

TEST(budget_is_exhausted_cost_unconstrained_not_exhausted) {
    /* cost_quota = UINT64_MAX is NOT zero, so not exhausted on cost alone */
    asx_budget b = asx_budget_from_polls(1);
    ASSERT_FALSE(asx_budget_is_exhausted(&b));
}

TEST(budget_is_exhausted_null) { ASSERT_TRUE(asx_budget_is_exhausted(NULL)); }

TEST(budget_polls_null_safe) { ASSERT_EQ(asx_budget_polls(NULL), 0u); }

TEST(budget_meet_deadline_unconstrained_identity) {
    /* meet(deadline=0, deadline=X) = deadline=X */
    asx_budget a = asx_budget_infinite();
    asx_budget b = asx_budget_infinite();
    asx_budget result;

    b.deadline = 500;
    result = asx_budget_meet(&a, &b);
    ASSERT_EQ(result.deadline, (asx_time)500);

    /* Reverse */
    result = asx_budget_meet(&b, &a);
    ASSERT_EQ(result.deadline, (asx_time)500);
}

TEST(budget_meet_deadline_both_finite_picks_earlier) {
    asx_budget a = asx_budget_infinite();
    asx_budget b = asx_budget_infinite();
    asx_budget result;

    a.deadline = 100;
    b.deadline = 200;
    result = asx_budget_meet(&a, &b);
    ASSERT_EQ(result.deadline, (asx_time)100);
}

TEST(budget_meet_null_identity) {
    asx_budget a = asx_budget_from_polls(42);
    asx_budget result;

    result = asx_budget_meet(&a, NULL);
    ASSERT_EQ(result.poll_quota, 42u);

    result = asx_budget_meet(NULL, &a);
    ASSERT_EQ(result.poll_quota, 42u);
}

TEST(budget_meet_both_null_returns_infinite) {
    asx_budget result = asx_budget_meet(NULL, NULL);
    asx_budget inf = asx_budget_infinite();
    ASSERT_EQ(result.deadline, inf.deadline);
    ASSERT_EQ(result.poll_quota, inf.poll_quota);
    ASSERT_EQ(result.cost_quota, inf.cost_quota);
    ASSERT_EQ(result.priority, inf.priority);
}

/* ================================================================== */
/* Section 3: Handle slot/generation decomposition                    */
/* ================================================================== */

TEST(handle_slot_generation_roundtrip) {
    uint16_t gen = 0x1234;
    uint16_t slot = 0x5678;
    uint32_t idx = asx_handle_pack_index(gen, slot);
    uint64_t h = asx_handle_pack(ASX_TYPE_TASK, 0x000F, idx);

    ASSERT_EQ(asx_handle_slot(h), slot);
    ASSERT_EQ(asx_handle_generation(h), gen);
    ASSERT_EQ(asx_handle_index(h), idx);
}

TEST(handle_slot_generation_zero) {
    uint32_t idx = asx_handle_pack_index(0, 0);
    uint64_t h = asx_handle_pack(ASX_TYPE_REGION, 0x0001, idx);

    ASSERT_EQ(asx_handle_slot(h), (uint16_t)0);
    ASSERT_EQ(asx_handle_generation(h), (uint16_t)0);
}

TEST(handle_slot_generation_max) {
    uint32_t idx = asx_handle_pack_index(0xFFFF, 0xFFFF);
    uint64_t h = asx_handle_pack(ASX_TYPE_CHANNEL, 0xFFFF, idx);

    ASSERT_EQ(asx_handle_slot(h), (uint16_t)0xFFFF);
    ASSERT_EQ(asx_handle_generation(h), (uint16_t)0xFFFF);
}

TEST(handle_different_types_same_slot_differ) {
    uint32_t idx = asx_handle_pack_index(1, 5);
    uint64_t h1 = asx_handle_pack(ASX_TYPE_REGION, 0x0001, idx);
    uint64_t h2 = asx_handle_pack(ASX_TYPE_TASK, 0x0001, idx);

    /* Same slot+generation but different type tags -> different handles */
    ASSERT_NE(h1, h2);
    ASSERT_EQ(asx_handle_slot(h1), asx_handle_slot(h2));
    ASSERT_EQ(asx_handle_generation(h1), asx_handle_generation(h2));
    ASSERT_NE(asx_handle_type_tag(h1), asx_handle_type_tag(h2));
}

TEST(handle_stale_generation_differs) {
    /* Same slot, different generation -> stale handle detection */
    uint32_t idx_old = asx_handle_pack_index(1, 5);
    uint32_t idx_new = asx_handle_pack_index(2, 5);
    uint64_t h_old = asx_handle_pack(ASX_TYPE_TASK, 0x0001, idx_old);
    uint64_t h_new = asx_handle_pack(ASX_TYPE_TASK, 0x0001, idx_new);

    ASSERT_NE(h_old, h_new);
    ASSERT_EQ(asx_handle_slot(h_old), asx_handle_slot(h_new));
    ASSERT_NE(asx_handle_generation(h_old), asx_handle_generation(h_new));
}

/* ================================================================== */
/* Section 4: Exhaustive transition matrix verification               */
/* ================================================================== */

/*
 * Verify every cell in the region transition table matches the spec.
 * region_transitions[5][5]: legal edges are
 *   Open->Closing, Closing->Draining, Closing->Finalizing,
 *   Draining->Finalizing, Finalizing->Closed
 */
TEST(region_transition_matrix_exhaustive) {
    /* Expected legal transitions encoded as bitmask per row */
    /* Row = from state, bit position = to state */
    static const uint8_t legal[5] = {/* Open:       -> Closing */
                                     0x02,
                                     /* Closing:    -> Draining | Finalizing */
                                     0x04 | 0x08,
                                     /* Draining:   -> Finalizing */
                                     0x08,
                                     /* Finalizing: -> Closed */
                                     0x10,
                                     /* Closed:     (none) */
                                     0x00};

    int from, to;
    for (from = 0; from < 5; from++) {
        for (to = 0; to < 5; to++) {
            asx_status s =
                asx_region_transition_check((asx_region_state)from, (asx_region_state)to);
            if (legal[from] & (1u << to)) {
                ASSERT_EQ(s, ASX_OK);
            } else {
                ASSERT_EQ(s, ASX_E_INVALID_TRANSITION);
            }
        }
    }
}

/*
 * Verify every cell in the task transition table.
 * task_transitions[6][6]: legal edges from transition_tables.c comments.
 */
TEST(task_transition_matrix_exhaustive) {
    static const uint8_t legal[6] = {
        /* Created:          -> Running | CancelReq | Completed */
        0x02 | 0x04 | 0x20,
        /* Running:          -> CancelReq | Completed */
        0x04 | 0x20,
        /* CancelRequested:  -> CancelReq(self) | Cancelling | Completed */
        0x04 | 0x08 | 0x20,
        /* Cancelling:       -> Cancelling(self) | Finalizing | Completed */
        0x08 | 0x10 | 0x20,
        /* Finalizing:       -> Finalizing(self) | Completed */
        0x10 | 0x20,
        /* Completed:        (none) */
        0x00};

    int from, to;
    for (from = 0; from < 6; from++) {
        for (to = 0; to < 6; to++) {
            asx_status s = asx_task_transition_check((asx_task_state)from, (asx_task_state)to);
            if (legal[from] & (1u << to)) {
                ASSERT_EQ(s, ASX_OK);
            } else {
                ASSERT_EQ(s, ASX_E_INVALID_TRANSITION);
            }
        }
    }
}

/*
 * Verify every cell in the obligation transition table.
 * obligation_transitions[4][4]: Reserved -> Committed|Aborted|Leaked
 */
TEST(obligation_transition_matrix_exhaustive) {
    static const uint8_t legal[4] = {/* Reserved:  -> Committed | Aborted | Leaked */
                                     0x02 | 0x04 | 0x08,
                                     /* Committed: (none) */
                                     0x00,
                                     /* Aborted:   (none) */
                                     0x00,
                                     /* Leaked:    (none) */
                                     0x00};

    int from, to;
    for (from = 0; from < 4; from++) {
        for (to = 0; to < 4; to++) {
            asx_status s = asx_obligation_transition_check((asx_obligation_state)from,
                                                           (asx_obligation_state)to);
            if (legal[from] & (1u << to)) {
                ASSERT_EQ(s, ASX_OK);
            } else {
                ASSERT_EQ(s, ASX_E_INVALID_TRANSITION);
            }
        }
    }
}

/* ================================================================== */
/* Section 5: Status code completeness                                */
/* ================================================================== */

TEST(status_all_error_codes_are_errors) {
    /* Every code >= 100 should be classified as error */
    static const asx_status error_codes[] = {ASX_E_INVALID_ARGUMENT,
                                             ASX_E_INVALID_STATE,
                                             ASX_E_INVALID_TRANSITION,
                                             ASX_E_REGION_CLOSED,
                                             ASX_E_REGION_POISONED,
                                             ASX_E_POLL_BUDGET_EXHAUSTED,
                                             ASX_E_OBLIGATION_ALREADY_RESOLVED,
                                             ASX_E_CANCELLED,
                                             ASX_E_WITNESS_PHASE_REGRESSION,
                                             ASX_E_WITNESS_REASON_WEAKENED,
                                             ASX_E_STALE_HANDLE,
                                             ASX_E_RESOURCE_EXHAUSTED,
                                             ASX_E_NOT_FOUND,
                                             ASX_E_QUIESCENCE_NOT_REACHED,
                                             ASX_E_QUIESCENCE_TASKS_LIVE,
                                             ASX_E_OBLIGATIONS_UNRESOLVED};
    unsigned i;
    for (i = 0; i < sizeof(error_codes) / sizeof(error_codes[0]); i++) {
        ASSERT_TRUE(asx_is_error(error_codes[i]));
    }
}

TEST(status_ok_is_not_error) { ASSERT_FALSE(asx_is_error(ASX_OK)); }

TEST(status_pending_is_error) {
    /* asx_is_error returns true for anything != ASX_OK, including PENDING */
    ASSERT_TRUE(asx_is_error(ASX_E_PENDING));
}

TEST(status_all_codes_have_string) {
    /* No known code should map to "unknown status" */
    static const asx_status known[] = {ASX_OK,
                                       ASX_E_PENDING,
                                       ASX_E_INVALID_ARGUMENT,
                                       ASX_E_INVALID_STATE,
                                       ASX_E_INVALID_TRANSITION,
                                       ASX_E_REGION_CLOSED,
                                       ASX_E_REGION_POISONED,
                                       ASX_E_POLL_BUDGET_EXHAUSTED,
                                       ASX_E_OBLIGATION_ALREADY_RESOLVED,
                                       ASX_E_CANCELLED,
                                       ASX_E_WITNESS_PHASE_REGRESSION,
                                       ASX_E_WITNESS_REASON_WEAKENED,
                                       ASX_E_STALE_HANDLE,
                                       ASX_E_RESOURCE_EXHAUSTED,
                                       ASX_E_NOT_FOUND};
    unsigned i;
    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        const char *s = asx_status_str(known[i]);
        ASSERT_TRUE(s != NULL);
        ASSERT_TRUE(strcmp(s, "unknown status") != 0);
    }
}

/* ================================================================== */
/* Section 6: Policy enum ordering contracts                          */
/* ================================================================== */

TEST(safety_profile_ordering) {
    /* Debug < Hardened < Release (ascending restrictiveness) */
    ASSERT_TRUE(ASX_SAFETY_DEBUG < ASX_SAFETY_HARDENED);
    ASSERT_TRUE(ASX_SAFETY_HARDENED < ASX_SAFETY_RELEASE);
}

TEST(containment_policy_ordering) {
    /* FailFast < PoisonRegion < ErrorOnly (ascending permissiveness) */
    ASSERT_TRUE(ASX_CONTAIN_FAIL_FAST < ASX_CONTAIN_POISON_REGION);
    ASSERT_TRUE(ASX_CONTAIN_POISON_REGION < ASX_CONTAIN_ERROR_ONLY);
}

TEST(resource_class_ordering) {
    ASSERT_TRUE(ASX_CLASS_R1 < ASX_CLASS_R2);
    ASSERT_TRUE(ASX_CLASS_R2 < ASX_CLASS_R3);
}

TEST(leak_response_ordering) {
    ASSERT_TRUE(ASX_LEAK_PANIC < ASX_LEAK_LOG);
    ASSERT_TRUE(ASX_LEAK_LOG < ASX_LEAK_SILENT);
    ASSERT_TRUE(ASX_LEAK_SILENT < ASX_LEAK_RECOVER);
}

TEST(cancel_phase_ordering) {
    /* Phases are strictly forward: Requested < Cancelling < Finalizing < Completed */
    ASSERT_TRUE(ASX_CANCEL_PHASE_REQUESTED < ASX_CANCEL_PHASE_CANCELLING);
    ASSERT_TRUE(ASX_CANCEL_PHASE_CANCELLING < ASX_CANCEL_PHASE_FINALIZING);
    ASSERT_TRUE(ASX_CANCEL_PHASE_FINALIZING < ASX_CANCEL_PHASE_COMPLETED);
}

/* ================================================================== */
/* Section 7: Cross-type contract interactions                        */
/* ================================================================== */

TEST(budget_from_cancel_cleanup_is_not_exhausted) {
    /* Every cancel cleanup budget should NOT be exhausted */
    int k;
    for (k = 0; k <= 10; k++) {
        asx_budget b = asx_cancel_cleanup_budget((asx_cancel_kind)k);
        ASSERT_FALSE(asx_budget_is_exhausted(&b));
    }
}

TEST(budget_meet_of_two_cleanup_budgets_tightens) {
    /* meet of a lenient and strict cleanup budget = stricter */
    asx_budget user_b = asx_cancel_cleanup_budget(ASX_CANCEL_USER);
    asx_budget shut_b = asx_cancel_cleanup_budget(ASX_CANCEL_SHUTDOWN);
    asx_budget result = asx_budget_meet(&user_b, &shut_b);

    ASSERT_TRUE(result.poll_quota <= user_b.poll_quota);
    ASSERT_TRUE(result.poll_quota <= shut_b.poll_quota);
    ASSERT_TRUE(result.priority <= user_b.priority);
    ASSERT_TRUE(result.priority <= shut_b.priority);
}

TEST(cancel_severity_groups_have_monotone_budgets) {
    /* Across severity groups, quota is non-increasing and priority non-decreasing */
    int prev_sev = -1;
    uint32_t prev_quota = UINT32_MAX;
    uint8_t prev_priority = 0;
    int k;
    for (k = 0; k <= 10; k++) {
        int sev = asx_cancel_severity((asx_cancel_kind)k);
        if (sev != prev_sev) {
            asx_budget b = asx_cancel_cleanup_budget((asx_cancel_kind)k);
            ASSERT_TRUE(b.poll_quota <= prev_quota);
            ASSERT_TRUE(b.priority >= prev_priority);
            prev_quota = b.poll_quota;
            prev_priority = b.priority;
            prev_sev = sev;
        }
    }
}

/* ================================================================== */
/* main                                                               */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "[formal] foundational type parity verification (bd-1eqo.15.1)\n");

    /* Cancel strengthen NULL/chain tests */
    RUN_TEST(cancel_strengthen_both_null_returns_user);
    RUN_TEST(cancel_strengthen_a_null_returns_b);
    RUN_TEST(cancel_strengthen_b_null_returns_a);
    RUN_TEST(cancel_reason_chain_linkage);
    RUN_TEST(cancel_reason_chain_truncation_flag);
    RUN_TEST(cancel_strengthen_preserves_attribution);
    RUN_TEST(cancel_cleanup_budget_all_kinds_valid);

    /* Budget deadline and consume tests */
    RUN_TEST(budget_deadline_zero_means_unconstrained);
    RUN_TEST(budget_deadline_finite_boundary);
    RUN_TEST(budget_deadline_one);
    RUN_TEST(budget_is_past_deadline_null_safe);
    RUN_TEST(budget_consume_poll_decrement);
    RUN_TEST(budget_consume_poll_null_safe);
    RUN_TEST(budget_consume_cost_atomic);
    RUN_TEST(budget_consume_cost_unconstrained_always_succeeds);
    RUN_TEST(budget_consume_cost_null_safe);
    RUN_TEST(budget_is_exhausted_poll_zero);
    RUN_TEST(budget_is_exhausted_cost_zero);
    RUN_TEST(budget_is_exhausted_cost_unconstrained_not_exhausted);
    RUN_TEST(budget_is_exhausted_null);
    RUN_TEST(budget_polls_null_safe);
    RUN_TEST(budget_meet_deadline_unconstrained_identity);
    RUN_TEST(budget_meet_deadline_both_finite_picks_earlier);
    RUN_TEST(budget_meet_null_identity);
    RUN_TEST(budget_meet_both_null_returns_infinite);

    /* Handle slot/generation tests */
    RUN_TEST(handle_slot_generation_roundtrip);
    RUN_TEST(handle_slot_generation_zero);
    RUN_TEST(handle_slot_generation_max);
    RUN_TEST(handle_different_types_same_slot_differ);
    RUN_TEST(handle_stale_generation_differs);

    /* Exhaustive transition matrices */
    RUN_TEST(region_transition_matrix_exhaustive);
    RUN_TEST(task_transition_matrix_exhaustive);
    RUN_TEST(obligation_transition_matrix_exhaustive);

    /* Status completeness */
    RUN_TEST(status_all_error_codes_are_errors);
    RUN_TEST(status_ok_is_not_error);
    RUN_TEST(status_pending_is_error);
    RUN_TEST(status_all_codes_have_string);

    /* Policy ordering */
    RUN_TEST(safety_profile_ordering);
    RUN_TEST(containment_policy_ordering);
    RUN_TEST(resource_class_ordering);
    RUN_TEST(leak_response_ordering);
    RUN_TEST(cancel_phase_ordering);

    /* Cross-type interactions */
    RUN_TEST(budget_from_cancel_cleanup_is_not_exhausted);
    RUN_TEST(budget_meet_of_two_cleanup_budgets_tightens);
    RUN_TEST(cancel_severity_groups_have_monotone_budgets);

    TEST_REPORT();
    return test_failures;
}
