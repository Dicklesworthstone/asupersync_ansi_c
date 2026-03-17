/*
 * test_cx.c — unit tests for capability context (Cx)
 *
 * Tests lifecycle, capability narrowing, budget/clock/entropy binding,
 * and cooperative checkpoints.
 *
 * We stub asx_task_get_state() so these tests don't depend on the
 * full runtime being initialized.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/budget.h>
#include <asx/cx/cx.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Stub for asx_task_get_state — avoids pulling in runtime/lifecycle   */
/* ------------------------------------------------------------------ */

static asx_task_state g_stub_task_state = ASX_TASK_RUNNING;
static asx_task_id g_stub_task_id = ASX_INVALID_ID;

asx_status asx_task_get_state(asx_task_id id, asx_task_state *out_state);

asx_status asx_task_get_state(asx_task_id id, asx_task_state *out_state) {
    if (out_state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (id == ASX_INVALID_ID || id != g_stub_task_id) return ASX_E_NOT_FOUND;
    *out_state = g_stub_task_state;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle tests                                                     */
/* ------------------------------------------------------------------ */

TEST(init_null_returns_invalid_argument) {
    ASSERT_EQ(asx_cx_init(NULL, 1, 1, ASX_CAP_NONE), ASX_E_INVALID_ARGUMENT);
}

TEST(init_invalid_region_returns_invalid_argument) {
    asx_cx cx;
    ASSERT_EQ(asx_cx_init(&cx, ASX_INVALID_ID, 1, ASX_CAP_NONE), ASX_E_INVALID_ARGUMENT);
}

TEST(init_sets_fields) {
    asx_cx cx;
    ASSERT_EQ(asx_cx_init(&cx, 42, 7, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY), ASX_OK);
    ASSERT_EQ(asx_cx_region(&cx), (asx_region_id)42);
    ASSERT_EQ(asx_cx_task(&cx), (asx_task_id)7);
    ASSERT_TRUE(asx_cx_has_cap(&cx, ASX_CAP_CLOCK_READ));
    ASSERT_TRUE(asx_cx_has_cap(&cx, ASX_CAP_ENTROPY));
    ASSERT_FALSE(asx_cx_has_cap(&cx, ASX_CAP_SPAWN));
    ASSERT_TRUE(asx_cx_is_valid(&cx));
    ASSERT_TRUE(cx.budget == NULL);
    ASSERT_TRUE(cx.clock == NULL);
    ASSERT_TRUE(cx.entropy_state == NULL);
}

TEST(init_generation_increments) {
    asx_cx a, b;
    asx_cx_init(&a, 1, 1, ASX_CAP_NONE);
    asx_cx_init(&b, 1, 1, ASX_CAP_NONE);
    ASSERT_NE(a.generation, b.generation);
    ASSERT_TRUE(b.generation > a.generation);
}

TEST(invalidate_clears_all) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_ALL);
    ASSERT_TRUE(asx_cx_is_valid(&cx));
    asx_cx_invalidate(&cx);
    ASSERT_FALSE(asx_cx_is_valid(&cx));
    ASSERT_EQ(cx.generation, 0u);
    ASSERT_EQ(cx.caps, ASX_CAP_NONE);
}

TEST(invalidate_null_safe) {
    /* Should not crash */
    asx_cx_invalidate(NULL);
}

TEST(is_valid_null) { ASSERT_FALSE(asx_cx_is_valid(NULL)); }

/* ------------------------------------------------------------------ */
/* Identity accessor tests                                             */
/* ------------------------------------------------------------------ */

TEST(region_null_returns_invalid) { ASSERT_EQ(asx_cx_region(NULL), ASX_INVALID_ID); }

TEST(task_null_returns_invalid) { ASSERT_EQ(asx_cx_task(NULL), ASX_INVALID_ID); }

/* ------------------------------------------------------------------ */
/* Capability query tests                                              */
/* ------------------------------------------------------------------ */

TEST(has_cap_null_returns_zero) { ASSERT_FALSE(asx_cx_has_cap(NULL, ASX_CAP_SPAWN)); }

TEST(caps_null_returns_none) { ASSERT_EQ(asx_cx_caps(NULL), ASX_CAP_NONE); }

TEST(caps_returns_all_set) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_ALL);
    ASSERT_EQ(asx_cx_caps(&cx), ASX_CAP_ALL);
}

TEST(has_cap_multi_flag_check) {
    asx_cx cx;
    asx_cap_flags combo = ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY;
    asx_cx_init(&cx, 1, 1, combo);
    ASSERT_TRUE(asx_cx_has_cap(&cx, combo));
    ASSERT_FALSE(asx_cx_has_cap(&cx, ASX_CAP_ALL));
}

/* ------------------------------------------------------------------ */
/* Narrowing tests                                                     */
/* ------------------------------------------------------------------ */

TEST(narrow_null_parent_fails) {
    asx_cx child;
    ASSERT_EQ(asx_cx_narrow(NULL, &child, ASX_CAP_NONE), ASX_E_INVALID_ARGUMENT);
}

TEST(narrow_null_child_fails) {
    asx_cx parent;
    asx_cx_init(&parent, 1, 1, ASX_CAP_ALL);
    ASSERT_EQ(asx_cx_narrow(&parent, NULL, ASX_CAP_NONE), ASX_E_INVALID_ARGUMENT);
}

TEST(narrow_invalid_parent_fails) {
    asx_cx parent, child;
    memset(&parent, 0, sizeof(parent));
    ASSERT_EQ(asx_cx_narrow(&parent, &child, ASX_CAP_NONE), ASX_E_INVALID_STATE);
}

TEST(narrow_escalation_fails) {
    asx_cx parent, child;
    asx_cx_init(&parent, 1, 1, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_narrow(&parent, &child, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN),
              ASX_E_INVALID_ARGUMENT);
}

TEST(narrow_subset_succeeds) {
    asx_cx parent, child;
    asx_budget budget = asx_budget_from_polls(10);
    asx_time clock = 42;
    uint64_t entropy = 0xDEAD;

    asx_cx_init(&parent, 5, 3, ASX_CAP_ALL);
    asx_cx_bind_budget(&parent, &budget);
    asx_cx_bind_clock(&parent, &clock);
    asx_cx_bind_entropy(&parent, &entropy);

    ASSERT_EQ(asx_cx_narrow(&parent, &child, ASX_CAP_CLOCK_READ), ASX_OK);
    ASSERT_EQ(asx_cx_region(&child), (asx_region_id)5);
    ASSERT_EQ(asx_cx_task(&child), (asx_task_id)3);
    ASSERT_TRUE(asx_cx_has_cap(&child, ASX_CAP_CLOCK_READ));
    ASSERT_FALSE(asx_cx_has_cap(&child, ASX_CAP_SPAWN));
    /* Borrowed pointers propagate */
    ASSERT_TRUE(child.budget == &budget);
    ASSERT_TRUE(child.clock == &clock);
    ASSERT_TRUE(child.entropy_state == &entropy);
    ASSERT_TRUE(asx_cx_is_valid(&child));
    ASSERT_NE(child.generation, parent.generation);
}

TEST(narrow_to_none_succeeds) {
    asx_cx parent, child;
    asx_cx_init(&parent, 1, 1, ASX_CAP_ALL);
    ASSERT_EQ(asx_cx_narrow(&parent, &child, ASX_CAP_NONE), ASX_OK);
    ASSERT_EQ(asx_cx_caps(&child), ASX_CAP_NONE);
}

TEST(attenuate_masks_parent_caps) {
    asx_cx parent, child;
    asx_cap_flags caps = ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN;

    asx_cx_init(&parent, 9, 4, caps);
    ASSERT_EQ(asx_cx_attenuate(&parent, &child, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN), ASX_OK);
    ASSERT_EQ(asx_cx_caps(&child), (asx_cap_flags)(ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN));
}

/* ------------------------------------------------------------------ */
/* Wrapper / registry / macaroon tests                                */
/* ------------------------------------------------------------------ */

TEST(wrap_and_unwrap_subset_succeeds) {
    asx_cx parent, child;
    asx_cx_wrapper wrapper;

    asx_cx_init(&parent, 5, 8, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_wrap(&parent, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN, &wrapper), ASX_OK);
    ASSERT_EQ(asx_cx_unwrap(&parent, &wrapper, &child), ASX_OK);
    ASSERT_EQ(asx_cx_region(&child), (asx_region_id)5);
    ASSERT_EQ(asx_cx_task(&child), (asx_task_id)8);
    ASSERT_TRUE(asx_cx_has_cap(&child, ASX_CAP_CLOCK_READ));
    ASSERT_TRUE(asx_cx_has_cap(&child, ASX_CAP_SPAWN));
    ASSERT_FALSE(asx_cx_has_cap(&child, ASX_CAP_ENTROPY));
}

TEST(wrap_escalation_fails) {
    asx_cx parent;
    asx_cx_wrapper wrapper;

    asx_cx_init(&parent, 5, 8, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_wrap(&parent, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN, &wrapper),
              ASX_E_INVALID_ARGUMENT);
}

TEST(unwrap_stale_wrapper_fails_closed) {
    asx_cx parent, child;
    asx_cx_wrapper wrapper;

    asx_cx_init(&parent, 5, 8, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_wrap(&parent, ASX_CAP_CLOCK_READ, &wrapper), ASX_OK);
    wrapper.parent_generation++;
    ASSERT_EQ(asx_cx_unwrap(&parent, &wrapper, &child), ASX_E_STALE_HANDLE);
}

TEST(registry_issue_materialize_and_revoke) {
    asx_cx parent, child;
    asx_cx_registry registry;
    asx_cx_registry_entry entries[2];
    asx_cx_grant grant;

    asx_cx_init(&parent, 7, 3, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_registry_init(&registry, entries, 2u), ASX_OK);
    ASSERT_EQ(asx_cx_registry_issue(&parent, &registry, ASX_CAP_ENTROPY, &grant), ASX_OK);
    ASSERT_EQ(asx_cx_registry_materialize(&parent, &registry, grant, &child), ASX_OK);
    ASSERT_EQ(asx_cx_caps(&child), ASX_CAP_ENTROPY);
    ASSERT_EQ(asx_cx_registry_revoke(&registry, grant), ASX_OK);
    ASSERT_EQ(asx_cx_registry_materialize(&parent, &registry, grant, &child), ASX_E_NOT_FOUND);
}

TEST(registry_materialize_wrong_parent_fails_closed) {
    asx_cx parent, wrong_parent, child;
    asx_cx_registry registry;
    asx_cx_registry_entry entries[2];
    asx_cx_grant grant;

    asx_cx_init(&parent, 7, 3, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN);
    asx_cx_init(&wrong_parent, 7, 3, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_registry_init(&registry, entries, 2u), ASX_OK);
    ASSERT_EQ(asx_cx_registry_issue(&parent, &registry, ASX_CAP_ENTROPY, &grant), ASX_OK);
    ASSERT_EQ(asx_cx_registry_materialize(&wrong_parent, &registry, grant, &child),
              ASX_E_STALE_HANDLE);
}

TEST(registry_capacity_exhaustion_fails) {
    asx_cx parent;
    asx_cx_registry registry;
    asx_cx_registry_entry entries[1];
    asx_cx_grant grant_a, grant_b;

    asx_cx_init(&parent, 7, 3, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY);
    ASSERT_EQ(asx_cx_registry_init(&registry, entries, 1u), ASX_OK);
    ASSERT_EQ(asx_cx_registry_issue(&parent, &registry, ASX_CAP_CLOCK_READ, &grant_a), ASX_OK);
    ASSERT_EQ(asx_cx_registry_issue(&parent, &registry, ASX_CAP_ENTROPY, &grant_b),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(registry_issue_escalation_fails) {
    asx_cx parent;
    asx_cx_registry registry;
    asx_cx_registry_entry entries[1];
    asx_cx_grant grant;

    asx_cx_init(&parent, 7, 3, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_registry_init(&registry, entries, 1u), ASX_OK);
    ASSERT_EQ(asx_cx_registry_issue(&parent, &registry, ASX_CAP_CLOCK_READ | ASX_CAP_SPAWN, &grant),
              ASX_E_INVALID_ARGUMENT);
}

TEST(macaroon_issue_attenuate_and_bind) {
    asx_cx parent, child;
    asx_cx_macaroon macaroon;

    asx_cx_init(&parent, 11, 13, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY | ASX_CAP_SPAWN);
    ASSERT_EQ(asx_cx_macaroon_issue(&parent, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY, &macaroon),
              ASX_OK);
    ASSERT_EQ(asx_cx_macaroon_attenuate(&macaroon, ASX_CAP_CLOCK_READ), ASX_OK);
    ASSERT_EQ(macaroon.caveat_count, 1u);
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, &macaroon, &child), ASX_OK);
    ASSERT_EQ(asx_cx_caps(&child), ASX_CAP_CLOCK_READ);
}

TEST(macaroon_bind_stale_parent_fails_closed) {
    asx_cx parent, child;
    asx_cx_macaroon macaroon;

    asx_cx_init(&parent, 11, 13, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY);
    ASSERT_EQ(asx_cx_macaroon_issue(&parent, ASX_CAP_CLOCK_READ, &macaroon), ASX_OK);
    macaroon.parent_generation++;
    ASSERT_EQ(asx_cx_macaroon_bind(&parent, &macaroon, &child), ASX_E_STALE_HANDLE);
}

/* ------------------------------------------------------------------ */
/* Budget binding tests                                                */
/* ------------------------------------------------------------------ */

TEST(bind_budget_null_cx_fails) {
    asx_budget b = asx_budget_from_polls(5);
    ASSERT_EQ(asx_cx_bind_budget(NULL, &b), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_budget_null_budget_fails) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_READ);
    ASSERT_EQ(asx_cx_bind_budget(&cx, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_budget_no_cap_fails) {
    asx_cx cx;
    asx_budget b = asx_budget_from_polls(5);
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_bind_budget(&cx, &b), ASX_E_PERMISSION_DENIED);
}

TEST(bind_budget_and_read) {
    asx_cx cx;
    asx_budget b = asx_budget_from_polls(10);
    asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_READ | ASX_CAP_BUDGET_CONSUME);
    ASSERT_EQ(asx_cx_bind_budget(&cx, &b), ASX_OK);
    ASSERT_TRUE(asx_cx_budget(&cx) != NULL);
    ASSERT_EQ(asx_budget_polls(asx_cx_budget(&cx)), 10u);
}

TEST(budget_null_cx_returns_null) { ASSERT_TRUE(asx_cx_budget(NULL) == NULL); }

TEST(budget_no_read_cap_returns_null) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_CONSUME);
    /* Can't bind without read cap, so budget stays NULL */
    ASSERT_TRUE(asx_cx_budget(&cx) == NULL);
}

/* ------------------------------------------------------------------ */
/* Consume poll tests                                                  */
/* ------------------------------------------------------------------ */

TEST(consume_poll_null_fails) { ASSERT_EQ(asx_cx_consume_poll(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(consume_poll_no_cap_fails) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_consume_poll(&cx), ASX_E_PERMISSION_DENIED);
}

TEST(consume_poll_no_budget_unlimited) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_CONSUME);
    ASSERT_EQ(asx_cx_consume_poll(&cx), ASX_OK);
}

TEST(consume_poll_decrements) {
    asx_cx cx;
    asx_budget b = asx_budget_from_polls(2);
    asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_READ | ASX_CAP_BUDGET_CONSUME);
    asx_cx_bind_budget(&cx, &b);
    ASSERT_EQ(asx_cx_consume_poll(&cx), ASX_OK);
    ASSERT_EQ(asx_budget_polls(&b), 1u);
    ASSERT_EQ(asx_cx_consume_poll(&cx), ASX_OK);
    ASSERT_EQ(asx_budget_polls(&b), 0u);
    ASSERT_EQ(asx_cx_consume_poll(&cx), ASX_E_POLL_BUDGET_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Clock binding tests                                                 */
/* ------------------------------------------------------------------ */

TEST(bind_clock_null_cx_fails) {
    asx_time t = 0;
    ASSERT_EQ(asx_cx_bind_clock(NULL, &t), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_clock_null_clock_fails) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_bind_clock(&cx, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_clock_no_cap_fails) {
    asx_cx cx;
    asx_time t = 0;
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_bind_clock(&cx, &t), ASX_E_PERMISSION_DENIED);
}

TEST(now_reads_bound_clock) {
    asx_cx cx;
    asx_time t = 12345;
    asx_cx_init(&cx, 1, 1, ASX_CAP_CLOCK_READ);
    asx_cx_bind_clock(&cx, &t);
    ASSERT_EQ(asx_cx_now(&cx), (asx_time)12345);
    t = 99999;
    ASSERT_EQ(asx_cx_now(&cx), (asx_time)99999);
}

TEST(now_no_clock_returns_zero) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_CLOCK_READ);
    ASSERT_EQ(asx_cx_now(&cx), (asx_time)0);
}

TEST(now_null_returns_zero) { ASSERT_EQ(asx_cx_now(NULL), (asx_time)0); }

TEST(now_no_cap_returns_zero) {
    asx_cx cx;
    asx_time t = 42;
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    /* Can't bind without cap, clock stays NULL */
    ASSERT_EQ(asx_cx_now(&cx), (asx_time)0);
    (void)t;
}

/* ------------------------------------------------------------------ */
/* Entropy binding tests                                               */
/* ------------------------------------------------------------------ */

TEST(bind_entropy_null_cx_fails) {
    uint64_t e = 0;
    ASSERT_EQ(asx_cx_bind_entropy(NULL, &e), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_entropy_null_state_fails) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_ENTROPY);
    ASSERT_EQ(asx_cx_bind_entropy(&cx, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(bind_entropy_no_cap_fails) {
    asx_cx cx;
    uint64_t e = 0;
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_bind_entropy(&cx, &e), ASX_E_PERMISSION_DENIED);
}

TEST(random_u64_draws_nonzero) {
    asx_cx cx;
    uint64_t entropy = 0xCAFEBABE;
    uint64_t r1, r2;
    asx_cx_init(&cx, 1, 1, ASX_CAP_ENTROPY);
    asx_cx_bind_entropy(&cx, &entropy);
    r1 = asx_cx_random_u64(&cx);
    r2 = asx_cx_random_u64(&cx);
    /* Should produce different values */
    ASSERT_NE(r1, r2);
    /* State should have been mutated */
    ASSERT_NE(entropy, (uint64_t)0xCAFEBABE);
}

TEST(random_u64_null_returns_zero) { ASSERT_EQ(asx_cx_random_u64(NULL), 0u); }

TEST(random_u64_no_cap_returns_zero) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_random_u64(&cx), 0u);
}

TEST(random_u64_no_state_returns_zero) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_ENTROPY);
    ASSERT_EQ(asx_cx_random_u64(&cx), 0u);
}

/* ------------------------------------------------------------------ */
/* Cancellation tests                                                  */
/* ------------------------------------------------------------------ */

TEST(is_cancelled_null_returns_zero) { ASSERT_FALSE(asx_cx_is_cancelled(NULL)); }

TEST(is_cancelled_no_cap_returns_zero) {
    asx_cx cx;
    asx_cx_init(&cx, 1, 1, ASX_CAP_NONE);
    ASSERT_FALSE(asx_cx_is_cancelled(&cx));
}

TEST(is_cancelled_no_task_returns_zero) {
    asx_cx cx;
    asx_cx_init(&cx, 1, ASX_INVALID_ID, ASX_CAP_CANCEL_CHECK);
    ASSERT_FALSE(asx_cx_is_cancelled(&cx));
}

TEST(is_cancelled_running_returns_zero) {
    asx_cx cx;
    g_stub_task_id = 10;
    g_stub_task_state = ASX_TASK_RUNNING;
    asx_cx_init(&cx, 1, 10, ASX_CAP_CANCEL_CHECK);
    ASSERT_FALSE(asx_cx_is_cancelled(&cx));
}

TEST(is_cancelled_cancel_requested_returns_nonzero) {
    asx_cx cx;
    g_stub_task_id = 11;
    g_stub_task_state = ASX_TASK_CANCEL_REQUESTED;
    asx_cx_init(&cx, 1, 11, ASX_CAP_CANCEL_CHECK);
    ASSERT_TRUE(asx_cx_is_cancelled(&cx));
}

TEST(is_cancelled_cancelling_returns_nonzero) {
    asx_cx cx;
    g_stub_task_id = 12;
    g_stub_task_state = ASX_TASK_CANCELLING;
    asx_cx_init(&cx, 1, 12, ASX_CAP_CANCEL_CHECK);
    ASSERT_TRUE(asx_cx_is_cancelled(&cx));
}

/* ------------------------------------------------------------------ */
/* Checkpoint tests                                                    */
/* ------------------------------------------------------------------ */

TEST(checkpoint_null_fails) { ASSERT_EQ(asx_cx_checkpoint(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(checkpoint_ok_no_cancel_no_budget) {
    asx_cx cx;
    g_stub_task_id = 20;
    g_stub_task_state = ASX_TASK_RUNNING;
    asx_cx_init(&cx, 1, 20, ASX_CAP_CANCEL_CHECK);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_OK);
}

TEST(checkpoint_cancelled_returns_cancelled) {
    asx_cx cx;
    g_stub_task_id = 21;
    g_stub_task_state = ASX_TASK_CANCEL_REQUESTED;
    asx_cx_init(&cx, 1, 21, ASX_CAP_CANCEL_CHECK);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
}

TEST(checkpoint_budget_exhaustion) {
    asx_cx cx;
    asx_budget b = asx_budget_from_polls(1);
    g_stub_task_id = 22;
    g_stub_task_state = ASX_TASK_RUNNING;
    asx_cx_init(&cx, 1, 22, ASX_CAP_CANCEL_CHECK | ASX_CAP_BUDGET_READ | ASX_CAP_BUDGET_CONSUME);
    asx_cx_bind_budget(&cx, &b);
    /* First checkpoint consumes the one poll */
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_OK);
    /* Second checkpoint — budget exhausted */
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_POLL_BUDGET_EXHAUSTED);
}

TEST(checkpoint_cancel_takes_priority_over_budget) {
    asx_cx cx;
    asx_budget b = asx_budget_from_polls(0);
    g_stub_task_id = 23;
    g_stub_task_state = ASX_TASK_CANCEL_REQUESTED;
    asx_cx_init(&cx, 1, 23, ASX_CAP_CANCEL_CHECK | ASX_CAP_BUDGET_READ | ASX_CAP_BUDGET_CONSUME);
    asx_cx_bind_budget(&cx, &b);
    /* Cancel should be checked before budget */
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
}

/* ------------------------------------------------------------------ */
/* Permission denied status string test                                */
/* ------------------------------------------------------------------ */

TEST(permission_denied_status_str) {
    ASSERT_STR_EQ(asx_status_str(ASX_E_PERMISSION_DENIED), "capability not granted");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_cx ===\n");

    /* Lifecycle */
    RUN_TEST(init_null_returns_invalid_argument);
    RUN_TEST(init_invalid_region_returns_invalid_argument);
    RUN_TEST(init_sets_fields);
    RUN_TEST(init_generation_increments);
    RUN_TEST(invalidate_clears_all);
    RUN_TEST(invalidate_null_safe);
    RUN_TEST(is_valid_null);

    /* Identity */
    RUN_TEST(region_null_returns_invalid);
    RUN_TEST(task_null_returns_invalid);

    /* Capabilities */
    RUN_TEST(has_cap_null_returns_zero);
    RUN_TEST(caps_null_returns_none);
    RUN_TEST(caps_returns_all_set);
    RUN_TEST(has_cap_multi_flag_check);

    /* Narrowing */
    RUN_TEST(narrow_null_parent_fails);
    RUN_TEST(narrow_null_child_fails);
    RUN_TEST(narrow_invalid_parent_fails);
    RUN_TEST(narrow_escalation_fails);
    RUN_TEST(narrow_subset_succeeds);
    RUN_TEST(narrow_to_none_succeeds);
    RUN_TEST(attenuate_masks_parent_caps);
    RUN_TEST(wrap_and_unwrap_subset_succeeds);
    RUN_TEST(wrap_escalation_fails);
    RUN_TEST(unwrap_stale_wrapper_fails_closed);
    RUN_TEST(registry_issue_materialize_and_revoke);
    RUN_TEST(registry_materialize_wrong_parent_fails_closed);
    RUN_TEST(registry_capacity_exhaustion_fails);
    RUN_TEST(registry_issue_escalation_fails);
    RUN_TEST(macaroon_issue_attenuate_and_bind);
    RUN_TEST(macaroon_bind_stale_parent_fails_closed);

    /* Budget */
    RUN_TEST(bind_budget_null_cx_fails);
    RUN_TEST(bind_budget_null_budget_fails);
    RUN_TEST(bind_budget_no_cap_fails);
    RUN_TEST(bind_budget_and_read);
    RUN_TEST(budget_null_cx_returns_null);
    RUN_TEST(budget_no_read_cap_returns_null);
    RUN_TEST(consume_poll_null_fails);
    RUN_TEST(consume_poll_no_cap_fails);
    RUN_TEST(consume_poll_no_budget_unlimited);
    RUN_TEST(consume_poll_decrements);

    /* Clock */
    RUN_TEST(bind_clock_null_cx_fails);
    RUN_TEST(bind_clock_null_clock_fails);
    RUN_TEST(bind_clock_no_cap_fails);
    RUN_TEST(now_reads_bound_clock);
    RUN_TEST(now_no_clock_returns_zero);
    RUN_TEST(now_null_returns_zero);
    RUN_TEST(now_no_cap_returns_zero);

    /* Entropy */
    RUN_TEST(bind_entropy_null_cx_fails);
    RUN_TEST(bind_entropy_null_state_fails);
    RUN_TEST(bind_entropy_no_cap_fails);
    RUN_TEST(random_u64_draws_nonzero);
    RUN_TEST(random_u64_null_returns_zero);
    RUN_TEST(random_u64_no_cap_returns_zero);
    RUN_TEST(random_u64_no_state_returns_zero);

    /* Cancellation */
    RUN_TEST(is_cancelled_null_returns_zero);
    RUN_TEST(is_cancelled_no_cap_returns_zero);
    RUN_TEST(is_cancelled_no_task_returns_zero);
    RUN_TEST(is_cancelled_running_returns_zero);
    RUN_TEST(is_cancelled_cancel_requested_returns_nonzero);
    RUN_TEST(is_cancelled_cancelling_returns_nonzero);

    /* Checkpoint */
    RUN_TEST(checkpoint_null_fails);
    RUN_TEST(checkpoint_ok_no_cancel_no_budget);
    RUN_TEST(checkpoint_cancelled_returns_cancelled);
    RUN_TEST(checkpoint_budget_exhaustion);
    RUN_TEST(checkpoint_cancel_takes_priority_over_budget);

    /* Status string */
    RUN_TEST(permission_denied_status_str);

    TEST_REPORT();
    return test_failures;
}
