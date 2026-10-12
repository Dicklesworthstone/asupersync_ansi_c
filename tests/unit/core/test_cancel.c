/*
 * test_cancel.c — unit tests for cancellation severity, budget, and strengthen
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/core/cancel.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Per-kind severity lookup                                            */
/* ------------------------------------------------------------------ */

TEST(cancel_severity_user) { ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_USER), (int)0); }

TEST(cancel_severity_monotone) {
    /* Rust's ordinals 0-9 are severity-ordered; LinkedExit, which Rust
     * declares last (Spork link propagation), is severity 3 (CancelKind,
     * types/cancel.rs:264), so the order is not monotone at 10. */
    int prev = 0;
    int k;
    for (k = 0; k <= 9; k++) {
        int sev = asx_cancel_severity((asx_cancel_kind)k);
        ASSERT_TRUE(sev >= prev);
        prev = sev;
    }
    ASSERT_EQ(asx_cancel_severity((asx_cancel_kind)10), (int)3);
}

TEST(cancel_kind_ordinals_are_rusts) {
    /* The discriminants of Rust's CancelKind, which derives Ord, Hash and
     * serde on them (types/cancel.rs:264; bd-9kll.2.18, DEC-006). */
    ASSERT_EQ((int)ASX_CANCEL_USER, 0);
    ASSERT_EQ((int)ASX_CANCEL_TIMEOUT, 1);
    ASSERT_EQ((int)ASX_CANCEL_DEADLINE, 2);
    ASSERT_EQ((int)ASX_CANCEL_POLL_QUOTA, 3);
    ASSERT_EQ((int)ASX_CANCEL_COST_BUDGET, 4);
    ASSERT_EQ((int)ASX_CANCEL_FAIL_FAST, 5);
    ASSERT_EQ((int)ASX_CANCEL_RACE_LOST, 6);
    ASSERT_EQ((int)ASX_CANCEL_PARENT, 7);
    ASSERT_EQ((int)ASX_CANCEL_RESOURCE, 8);
    ASSERT_EQ((int)ASX_CANCEL_SHUTDOWN, 9);
    ASSERT_EQ((int)ASX_CANCEL_LINKED_EXIT, 10);
}

TEST(cancel_severity_known_values) {
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_TIMEOUT), (int)1);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_DEADLINE), (int)1);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_POLL_QUOTA), (int)2);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_COST_BUDGET), (int)2);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_FAIL_FAST), (int)3);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_RACE_LOST), (int)3);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_LINKED_EXIT), (int)3);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_PARENT), (int)4);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_RESOURCE), (int)4);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_SHUTDOWN), (int)5);
}

/* ------------------------------------------------------------------ */
/* Per-kind quota and priority                                         */
/* ------------------------------------------------------------------ */

TEST(cancel_quota_decreasing) {
    /* Higher severity kinds get tighter (lower) cleanup quotas */
    asx_budget user_b = asx_cancel_cleanup_budget(ASX_CANCEL_USER);
    asx_budget shut_b = asx_cancel_cleanup_budget(ASX_CANCEL_SHUTDOWN);
    ASSERT_TRUE(user_b.poll_quota > shut_b.poll_quota);
}

TEST(cancel_priority_increasing) {
    /* Higher severity kinds get higher cleanup priority */
    asx_budget user_b = asx_cancel_cleanup_budget(ASX_CANCEL_USER);
    asx_budget shut_b = asx_cancel_cleanup_budget(ASX_CANCEL_SHUTDOWN);
    ASSERT_TRUE(shut_b.priority > user_b.priority);
}

/* ------------------------------------------------------------------ */
/* Cleanup budget construction                                         */
/* ------------------------------------------------------------------ */

TEST(cancel_cleanup_budget_sets_quota) {
    asx_budget b = asx_cancel_cleanup_budget(ASX_CANCEL_USER);
    ASSERT_TRUE(b.poll_quota > 0);
    ASSERT_TRUE(b.poll_quota < UINT32_MAX); /* not infinite */
}

TEST(cancel_cleanup_budget_shutdown_tight) {
    asx_budget user = asx_cancel_cleanup_budget(ASX_CANCEL_USER);
    asx_budget shut = asx_cancel_cleanup_budget(ASX_CANCEL_SHUTDOWN);
    ASSERT_TRUE(shut.poll_quota < user.poll_quota);
    ASSERT_TRUE(shut.priority > user.priority);
}

/* ------------------------------------------------------------------ */
/* Reason strengthening                                                */
/* ------------------------------------------------------------------ */

TEST(cancel_strengthen_higher_severity_wins) {
    asx_cancel_reason user = {ASX_CANCEL_USER, 0, 0, 100, "user", NULL, 0};
    asx_cancel_reason shut = {ASX_CANCEL_SHUTDOWN, 0, 0, 200, "shut", NULL, 0};
    asx_cancel_reason result = asx_cancel_strengthen(&user, &shut);
    ASSERT_EQ(result.kind, ASX_CANCEL_SHUTDOWN);
}

TEST(cancel_strengthen_commutative_for_different_severity) {
    asx_cancel_reason user = {ASX_CANCEL_USER, 0, 0, 100, "user", NULL, 0};
    asx_cancel_reason shut = {ASX_CANCEL_SHUTDOWN, 0, 0, 200, "shut", NULL, 0};
    asx_cancel_reason ab = asx_cancel_strengthen(&user, &shut);
    asx_cancel_reason ba = asx_cancel_strengthen(&shut, &user);
    ASSERT_EQ(ab.kind, ba.kind);
}

TEST(cancel_strengthen_equal_severity_earlier_wins) {
    asx_cancel_reason a = {ASX_CANCEL_TIMEOUT, 0, 0, 100, "early", NULL, 0};
    asx_cancel_reason b = {ASX_CANCEL_DEADLINE, 0, 0, 200, "late", NULL, 0};
    /* Both severity 1 — earlier timestamp wins */
    asx_cancel_reason result = asx_cancel_strengthen(&a, &b);
    ASSERT_EQ(result.timestamp, (asx_time)100);
}

TEST(cancel_severity_out_of_range_clamps) {
    /* Values beyond ASX_CANCEL_SHUTDOWN (10) should return max severity (5) */
    ASSERT_EQ(asx_cancel_severity((asx_cancel_kind)11), (int)5);
    ASSERT_EQ(asx_cancel_severity((asx_cancel_kind)100), (int)5);
    ASSERT_EQ(asx_cancel_severity((asx_cancel_kind)255), (int)5);
    ASSERT_EQ(asx_cancel_severity((asx_cancel_kind)-1), (int)5);
}

TEST(cancel_strengthen_equal_severity_same_timestamp) {
    /* Equal severity, equal timestamp, larger candidate message: kept. */
    asx_cancel_reason a = {ASX_CANCEL_TIMEOUT, 0, 0, 100, "a", NULL, 0};
    asx_cancel_reason b = {ASX_CANCEL_DEADLINE, 0, 0, 100, "b", NULL, 0};
    asx_cancel_reason result = asx_cancel_strengthen(&a, &b);
    ASSERT_EQ(result.kind, ASX_CANCEL_TIMEOUT);
}

TEST(cancel_reason_default_has_rust_default_attribution) {
    /* Rust's Cx-free constructors (types/cancel.rs:596-631): the region at
     * arena index 0, no task, stamped 1 s, the given message. */
    asx_cancel_reason r = asx_cancel_reason_default(ASX_CANCEL_TIMEOUT, "late");
    ASSERT_EQ(r.kind, ASX_CANCEL_TIMEOUT);
    ASSERT_EQ(asx_handle_index(r.origin_region), 0u);
    ASSERT_TRUE(r.origin_region != ASX_INVALID_ID);
    ASSERT_EQ(r.origin_task, ASX_INVALID_ID);
    ASSERT_EQ(r.timestamp, (asx_time)1000000000u);
    ASSERT_TRUE(r.message != NULL && strcmp(r.message, "late") == 0);
    ASSERT_TRUE(r.cause == NULL);
    ASSERT_EQ(r.truncated, 0);
    r = asx_cancel_reason_default(ASX_CANCEL_RACE_LOST, NULL);
    ASSERT_EQ(r.kind, ASX_CANCEL_RACE_LOST);
    ASSERT_TRUE(r.message == NULL);
}

TEST(cancel_strengthen_tie_prefers_a_message_then_the_smaller_one) {
    /* Rust CancelReason::strengthen (types/cancel.rs:991-1007): on equal
     * severity and timestamp a reason with a message replaces one without,
     * and the smaller message replaces the larger, whole reason included. */
    asx_cancel_reason none = {ASX_CANCEL_TIMEOUT, 0, 0, 100, NULL, NULL, 0};
    asx_cancel_reason late = {ASX_CANCEL_DEADLINE, 0, 0, 100, "late", NULL, 0};
    asx_cancel_reason early = {ASX_CANCEL_TIMEOUT, 0, 0, 100, "early", NULL, 0};
    asx_cancel_reason result = asx_cancel_strengthen(&none, &late);
    ASSERT_EQ(result.kind, ASX_CANCEL_DEADLINE);
    ASSERT_STR_EQ(result.message, "late");
    result = asx_cancel_strengthen(&late, &none);
    ASSERT_STR_EQ(result.message, "late");
    result = asx_cancel_strengthen(&late, &early);
    ASSERT_EQ(result.kind, ASX_CANCEL_TIMEOUT);
    ASSERT_STR_EQ(result.message, "early");
    result = asx_cancel_strengthen(&early, &late);
    ASSERT_STR_EQ(result.message, "early");
}

/* ------------------------------------------------------------------ */
/* Cancel witness lifecycle                                            */
/* ------------------------------------------------------------------ */

TEST(witness_create_and_query_phase) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 100, "test", NULL, 0};
    asx_cancel_phase phase;

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);
    ASSERT_NE(w, ASX_INVALID_ID);
    ASSERT_TRUE(asx_cancel_witness_is_valid(w));

    ASSERT_EQ(asx_cancel_witness_phase(w, &phase), ASX_OK);
    ASSERT_EQ(phase, ASX_CANCEL_PHASE_REQUESTED);

    asx_cancel_witness_release(w);
}

TEST(witness_query_reason) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_SHUTDOWN, 0, 0, 42, "shutdown-test", NULL, 0};
    asx_cancel_reason out;

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);

    memset(&out, 0, sizeof(out));
    ASSERT_EQ(asx_cancel_witness_reason(w, &out), ASX_OK);
    ASSERT_EQ(out.kind, ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(out.timestamp, (asx_time)42);

    asx_cancel_witness_release(w);
}

TEST(witness_advance_monotone_phases) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "adv", NULL, 0};
    asx_cancel_phase phase;

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);

    /* requested → cancelling */
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_CANCELLING), ASX_OK);
    ASSERT_EQ(asx_cancel_witness_phase(w, &phase), ASX_OK);
    ASSERT_EQ(phase, ASX_CANCEL_PHASE_CANCELLING);

    /* cancelling → finalizing */
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_FINALIZING), ASX_OK);
    ASSERT_EQ(asx_cancel_witness_phase(w, &phase), ASX_OK);
    ASSERT_EQ(phase, ASX_CANCEL_PHASE_FINALIZING);

    /* finalizing → completed */
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_COMPLETED), ASX_OK);
    ASSERT_EQ(asx_cancel_witness_phase(w, &phase), ASX_OK);
    ASSERT_EQ(phase, ASX_CANCEL_PHASE_COMPLETED);

    asx_cancel_witness_release(w);
}

TEST(witness_advance_backward_rejected) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "back", NULL, 0};

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_CANCELLING), ASX_OK);

    /* Backward transition must fail */
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_REQUESTED), ASX_E_INVALID_TRANSITION);

    /* Same-phase transition must fail (not strictly increasing) */
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_CANCELLING), ASX_E_INVALID_TRANSITION);

    asx_cancel_witness_release(w);
}

TEST(witness_release_invalidates) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "rel", NULL, 0};
    asx_cancel_phase phase;

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);
    ASSERT_TRUE(asx_cancel_witness_is_valid(w));

    ASSERT_EQ(asx_cancel_witness_release(w), ASX_OK);
    ASSERT_TRUE(!asx_cancel_witness_is_valid(w));

    /* Queries on released witness should fail */
    ASSERT_EQ(asx_cancel_witness_phase(w, &phase), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_CANCELLING), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_cancel_witness_release(w), ASX_E_NOT_FOUND);
}

TEST(witness_create_null_args_rejected) {
    asx_cancel_witness_id w;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "null", NULL, 0};

    ASSERT_EQ(asx_cancel_witness_create(NULL, 1, &reason), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_cancel_witness_create(&w, 1, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(witness_phase_null_out_rejected) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "np", NULL, 0};

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);
    ASSERT_EQ(asx_cancel_witness_phase(w, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_cancel_witness_reason(w, NULL), ASX_E_INVALID_ARGUMENT);
    asx_cancel_witness_release(w);
}

TEST(witness_invalid_id_rejected) {
    asx_cancel_phase phase;
    asx_cancel_reason out;

    ASSERT_TRUE(!asx_cancel_witness_is_valid(ASX_INVALID_ID));
    ASSERT_EQ(asx_cancel_witness_phase(ASX_INVALID_ID, &phase), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_cancel_witness_reason(ASX_INVALID_ID, &out), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_cancel_witness_advance(ASX_INVALID_ID, ASX_CANCEL_PHASE_CANCELLING),
              ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_cancel_witness_release(ASX_INVALID_ID), ASX_E_NOT_FOUND);
}

/* ------------------------------------------------------------------ */
/* Cancel kind/phase string helpers                                    */
/* ------------------------------------------------------------------ */

TEST(cancel_kind_str_known) {
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_USER), "user");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_SHUTDOWN), "shutdown");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_TIMEOUT), "timeout");
}

TEST(cancel_kind_str_unknown) {
    ASSERT_STR_EQ(asx_cancel_kind_str((asx_cancel_kind)99), "unknown");
}

TEST(cancel_phase_str_known) {
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_REQUESTED), "requested");
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_CANCELLING), "cancelling");
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_FINALIZING), "finalizing");
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_COMPLETED), "completed");
}

TEST(cancel_phase_str_unknown) {
    ASSERT_STR_EQ(asx_cancel_phase_str((asx_cancel_phase)99), "unknown");
}

/* ------------------------------------------------------------------ */
/* Cause-chain bound (Rust with_cause_limited, types/cancel.rs:706-761) */
/* ------------------------------------------------------------------ */

#define CHAIN_N 24u

/* chain[0] is a root reason; chain[i] is PARENT caused by chain[i - 1],
 * built as the runtime builds a region subtree's reasons (state.rs:7737). */
static void build_chain(asx_cancel_reason *chain, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        memset(&chain[i], 0, sizeof chain[i]);
        chain[i].kind = i == 0u ? ASX_CANCEL_USER : ASX_CANCEL_PARENT;
        chain[i].origin_task = ASX_INVALID_ID;
        chain[i].timestamp = 1u;
        if (i > 0u) asx_cancel_reason_with_cause_limited(&chain[i], &chain[i - 1u]);
    }
}

/* chain[i] has i causes until the bound: from there it keeps `limit`. */
static uint32_t expect_links(uint32_t i, uint32_t limit) { return i < limit ? i : limit; }

TEST(chain_link_limit_follows_the_depth_and_memory_bounds) {
    /* Rust's defaults, depth 16 and 4096 bytes, keep 15 links: depth binds. */
    ASSERT_EQ(asx_cancel_chain_link_limit(16u, 4096u), (uint32_t)15u);
    /* The runtime profiles' pairs (builder.c): depth binds in both. */
    ASSERT_EQ(asx_cancel_chain_link_limit(8u, 2048u), (uint32_t)7u);
    ASSERT_EQ(asx_cancel_chain_link_limit(32u, 8192u), (uint32_t)31u);
    /* A depth of 1 leaves no room for a cause. */
    ASSERT_EQ(asx_cancel_chain_link_limit(1u, 4096u), (uint32_t)0u);
    ASSERT_EQ(asx_cancel_chain_link_limit(0u, 4096u), (uint32_t)0u);
    /* Memory binds when it is the tighter: a chain of d reasons costs
     * 88 d - 8 bytes (80 a reason, 8 a link), so 4096 holds 46 reasons. */
    ASSERT_EQ(asx_cancel_chain_link_limit(100u, 4096u), (uint32_t)45u);
    ASSERT_EQ(asx_cancel_chain_link_limit(65535u, 4096u), (uint32_t)45u);
    /* Two reasons cost 168 bytes: one link at 168, none below. */
    ASSERT_EQ(asx_cancel_chain_link_limit(16u, 168u), (uint32_t)1u);
    ASSERT_EQ(asx_cancel_chain_link_limit(16u, 167u), (uint32_t)0u);
    ASSERT_EQ(asx_cancel_chain_link_limit(16u, 79u), (uint32_t)0u);
    ASSERT_EQ(asx_cancel_chain_link_limit(16u, 0u), (uint32_t)0u);
}

TEST(chain_is_cut_at_the_default_depth) {
    /* Depth 16: a reason keeps 15 causes. The reason that would be the
     * 17th is the first flagged truncated (Rust: total_depth 17 > 16). */
    asx_cancel_reason chain[CHAIN_N];
    const asx_cancel_reason *c;
    uint32_t i;
    uint32_t raw = 0u;

    asx_cancel_chain_limits_reset_internal();
    build_chain(chain, CHAIN_N);
    for (i = 0; i < CHAIN_N; i++) {
        ASSERT_EQ(asx_cancel_reason_chain_links(&chain[i]), expect_links(i, 15u));
        ASSERT_EQ(chain[i].truncated, i >= 16u ? 1 : 0);
    }
    /* The stored causes are the ancestors' own reasons and go on past the
     * cut; the link count is where the chain ends. */
    for (c = chain[CHAIN_N - 1u].cause; c != NULL; c = c->cause) raw++;
    ASSERT_EQ(raw, CHAIN_N - 1u);
    ASSERT_EQ(asx_cancel_reason_chain_links(&chain[CHAIN_N - 1u]), (uint32_t)15u);
}

TEST(chain_is_cut_at_a_configured_depth) {
    asx_cancel_reason chain[CHAIN_N];
    uint32_t i;

    asx_cancel_chain_limits_set_internal(4u, 4096u);
    build_chain(chain, 10u);
    for (i = 0; i < 10u; i++) {
        ASSERT_EQ(asx_cancel_reason_chain_links(&chain[i]), expect_links(i, 3u));
        ASSERT_EQ(chain[i].truncated, i >= 4u ? 1 : 0);
    }
    asx_cancel_chain_limits_reset_internal();
}

TEST(chain_is_cut_at_a_configured_memory_bound) {
    /* 300 bytes hold 3 reasons (256) and not 4 (344): 2 links, though the
     * depth bound of 100 would allow far more. */
    asx_cancel_reason chain[CHAIN_N];
    uint32_t i;

    asx_cancel_chain_limits_set_internal(100u, 300u);
    build_chain(chain, 10u);
    for (i = 0; i < 10u; i++) {
        ASSERT_EQ(asx_cancel_reason_chain_links(&chain[i]), expect_links(i, 2u));
        ASSERT_EQ(chain[i].truncated, i >= 3u ? 1 : 0);
    }
    asx_cancel_chain_limits_reset_internal();
}

TEST(chain_bound_with_no_room_keeps_no_cause) {
    /* Depth 1, or memory under two reasons: the reason is flagged and has
     * no cause (Rust: "No room for any cause - mark as truncated"). */
    asx_cancel_reason chain[3];

    asx_cancel_chain_limits_set_internal(1u, 4096u);
    build_chain(chain, 3u);
    ASSERT_TRUE(chain[1].cause == NULL);
    ASSERT_EQ(chain[1].truncated, 1);
    ASSERT_EQ(asx_cancel_reason_chain_links(&chain[2]), (uint32_t)0u);

    asx_cancel_chain_limits_set_internal(16u, 0u);
    build_chain(chain, 3u);
    ASSERT_TRUE(chain[1].cause == NULL);
    ASSERT_EQ(chain[1].truncated, 1);
    asx_cancel_chain_limits_reset_internal();
}

TEST(callers_own_chain_is_counted_whole) {
    /* Rust's with_cause sets no bound: only a chain the runtime built and
     * flagged truncated is cut. A hand-linked chain counts every link. */
    asx_cancel_reason chain[30];
    uint32_t i;

    asx_cancel_chain_limits_reset_internal();
    ASSERT_EQ(asx_cancel_reason_chain_links(NULL), (uint32_t)0u);
    for (i = 0; i < 30u; i++) {
        memset(&chain[i], 0, sizeof chain[i]);
        chain[i].kind = ASX_CANCEL_USER;
        chain[i].origin_task = ASX_INVALID_ID;
        chain[i].cause = i > 0u ? &chain[i - 1u] : NULL;
    }
    ASSERT_EQ(asx_cancel_reason_chain_links(&chain[0]), (uint32_t)0u);
    ASSERT_EQ(asx_cancel_reason_chain_links(&chain[29]), (uint32_t)29u);

    /* Flagged, the same chain ends at the bound. */
    chain[29].truncated = 1;
    ASSERT_EQ(asx_cancel_reason_chain_links(&chain[29]), (uint32_t)15u);
}

int main(void) {
    fprintf(stderr, "=== test_cancel ===\n");
    RUN_TEST(cancel_severity_user);
    RUN_TEST(cancel_severity_monotone);
    RUN_TEST(cancel_severity_known_values);
    RUN_TEST(cancel_kind_ordinals_are_rusts);
    RUN_TEST(cancel_quota_decreasing);
    RUN_TEST(cancel_priority_increasing);
    RUN_TEST(cancel_cleanup_budget_sets_quota);
    RUN_TEST(cancel_cleanup_budget_shutdown_tight);
    RUN_TEST(cancel_strengthen_higher_severity_wins);
    RUN_TEST(cancel_strengthen_commutative_for_different_severity);
    RUN_TEST(cancel_strengthen_equal_severity_earlier_wins);
    RUN_TEST(cancel_severity_out_of_range_clamps);
    RUN_TEST(cancel_strengthen_equal_severity_same_timestamp);
    RUN_TEST(cancel_strengthen_tie_prefers_a_message_then_the_smaller_one);
    RUN_TEST(cancel_reason_default_has_rust_default_attribution);

    /* Cause-chain bound */
    RUN_TEST(chain_link_limit_follows_the_depth_and_memory_bounds);
    RUN_TEST(chain_is_cut_at_the_default_depth);
    RUN_TEST(chain_is_cut_at_a_configured_depth);
    RUN_TEST(chain_is_cut_at_a_configured_memory_bound);
    RUN_TEST(chain_bound_with_no_room_keeps_no_cause);
    RUN_TEST(callers_own_chain_is_counted_whole);

    /* Witness lifecycle */
    RUN_TEST(witness_create_and_query_phase);
    RUN_TEST(witness_query_reason);
    RUN_TEST(witness_advance_monotone_phases);
    RUN_TEST(witness_advance_backward_rejected);
    RUN_TEST(witness_release_invalidates);
    RUN_TEST(witness_create_null_args_rejected);
    RUN_TEST(witness_phase_null_out_rejected);
    RUN_TEST(witness_invalid_id_rejected);

    /* String helpers */
    RUN_TEST(cancel_kind_str_known);
    RUN_TEST(cancel_kind_str_unknown);
    RUN_TEST(cancel_phase_str_known);
    RUN_TEST(cancel_phase_str_unknown);

    TEST_REPORT();
    return test_failures;
}
