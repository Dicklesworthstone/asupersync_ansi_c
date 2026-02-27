/*
 * handle_safety_test.c — generation-based handle safety and cleanup-stack tests
 *
 * Tests for bd-hwb.4: stale handle detection, type mismatch rejection,
 * cleanup-stack LIFO ordering, push/pop/drain semantics, and capacity limits.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>

/* Internal header for test access to arena slots */
#include "../../../src/runtime/runtime_internal.h"

/* ------------------------------------------------------------------ */
/* Test poll function                                                   */
/* ------------------------------------------------------------------ */

static asx_status noop_poll(void *user_data, asx_task_id self)
{
    (void)user_data;
    (void)self;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Cleanup tracking state                                              */
/* ------------------------------------------------------------------ */

#define MAX_CLEANUP_LOG 32

static int g_cleanup_log[MAX_CLEANUP_LOG];
static int g_cleanup_log_count = 0;

static void cleanup_log_reset(void)
{
    int i;
    g_cleanup_log_count = 0;
    for (i = 0; i < MAX_CLEANUP_LOG; i++) g_cleanup_log[i] = 0;
}

static void cleanup_record(void *ctx)
{
    int val = *(int *)ctx;
    if (g_cleanup_log_count < MAX_CLEANUP_LOG) {
        g_cleanup_log[g_cleanup_log_count++] = val;
    }
}

/* ------------------------------------------------------------------ */
/* Handle safety tests                                                 */
/* ------------------------------------------------------------------ */

TEST(stale_region_handle_after_recycle)
{
    asx_region_id rid1, rid2;
    asx_region_state state;
    asx_budget budget;

    /* Open, drain, and close a region to make slot 0 recyclable */
    ASSERT_EQ(asx_region_open(&rid1), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid1, &budget), ASX_OK);

    /* Recycle slot 0 with a new region */
    ASSERT_EQ(asx_region_open(&rid2), ASX_OK);

    /* Old handle should be stale */
    ASSERT_EQ(asx_region_get_state(rid1, &state), ASX_E_STALE_HANDLE);

    /* New handle should work */
    ASSERT_EQ(asx_region_get_state(rid2, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);

    /* Handles should differ (different generation) */
    ASSERT_NE(rid1, rid2);
}

TEST(stale_handle_on_region_close)
{
    asx_region_id rid;
    asx_budget budget;
    asx_status st;
    asx_region_state state;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);

    /* After drain, region is CLOSED but handle is still valid
     * (same generation, slot alive) */
    st = asx_region_get_state(rid, &state);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(state, ASX_REGION_CLOSED);
}

TEST(stale_handle_spawn_rejected)
{
    asx_region_id rid1, rid2;
    asx_task_id tid;
    asx_budget budget;

    ASSERT_EQ(asx_region_open(&rid1), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid1, &budget), ASX_OK);

    /* Recycle the slot */
    ASSERT_EQ(asx_region_open(&rid2), ASX_OK);

    /* Spawning with stale handle should fail */
    ASSERT_EQ(asx_task_spawn(rid1, noop_poll, NULL, &tid),
              ASX_E_STALE_HANDLE);

    /* Spawning with fresh handle should work */
    ASSERT_EQ(asx_task_spawn(rid2, noop_poll, NULL, &tid), ASX_OK);
}

TEST(invalid_handle_returns_not_found)
{
    asx_region_state state;

    /* Zero handle is invalid */
    ASSERT_EQ(asx_region_get_state(ASX_INVALID_ID, &state), ASX_E_NOT_FOUND);

    /* Fabricated handle with wrong type tag */
    {
        uint64_t fake = asx_handle_pack(ASX_TYPE_TASK, 0, 0);
        ASSERT_EQ(asx_region_get_state(fake, &state), ASX_E_NOT_FOUND);
    }
}

TEST(task_handle_type_mismatch)
{
    asx_region_id rid;
    asx_task_state tstate;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Using a region handle where a task handle is expected */
    ASSERT_EQ(asx_task_get_state(rid, &tstate), ASX_E_NOT_FOUND);
}

TEST(generation_preserved_in_handle_round_trip)
{
    asx_region_id rid;
    uint16_t gen, slot_idx, tag;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    tag      = asx_handle_type_tag(rid);
    gen      = asx_handle_generation(rid);
    slot_idx = asx_handle_slot(rid);

    ASSERT_EQ(tag, ASX_TYPE_REGION);
    ASSERT_EQ(gen, (uint16_t)0);
    ASSERT_EQ(slot_idx, (uint16_t)0);
}

TEST(generation_increments_on_recycle)
{
    asx_region_id rid1, rid2, rid3;
    asx_budget budget;

    /* First generation */
    ASSERT_EQ(asx_region_open(&rid1), ASX_OK);
    ASSERT_EQ(asx_handle_generation(rid1), (uint16_t)0);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid1, &budget), ASX_OK);

    /* Second generation */
    ASSERT_EQ(asx_region_open(&rid2), ASX_OK);
    ASSERT_EQ(asx_handle_generation(rid2), (uint16_t)1);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid2, &budget), ASX_OK);

    /* Third generation */
    ASSERT_EQ(asx_region_open(&rid3), ASX_OK);
    ASSERT_EQ(asx_handle_generation(rid3), (uint16_t)2);
}

/* ------------------------------------------------------------------ */
/* Cleanup-stack tests                                                 */
/* ------------------------------------------------------------------ */

TEST(cleanup_stack_lifo_order)
{
    asx_cleanup_stack stack;
    asx_cleanup_handle h1, h2, h3;
    int v1 = 10, v2 = 20, v3 = 30;

    cleanup_log_reset();
    asx_cleanup_init(&stack);

    ASSERT_EQ(asx_cleanup_push(&stack, cleanup_record, &v1, &h1), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&stack, cleanup_record, &v2, &h2), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&stack, cleanup_record, &v3, &h3), ASX_OK);

    ASSERT_EQ(asx_cleanup_pending(&stack), (uint32_t)3);

    asx_cleanup_drain(&stack);

    /* LIFO: 30, 20, 10 */
    ASSERT_EQ(g_cleanup_log_count, 3);
    ASSERT_EQ(g_cleanup_log[0], 30);
    ASSERT_EQ(g_cleanup_log[1], 20);
    ASSERT_EQ(g_cleanup_log[2], 10);
}

TEST(cleanup_pop_skips_during_drain)
{
    asx_cleanup_stack stack;
    asx_cleanup_handle h1, h2, h3;
    int v1 = 1, v2 = 2, v3 = 3;

    cleanup_log_reset();
    asx_cleanup_init(&stack);

    ASSERT_EQ(asx_cleanup_push(&stack, cleanup_record, &v1, &h1), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&stack, cleanup_record, &v2, &h2), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&stack, cleanup_record, &v3, &h3), ASX_OK);

    /* Pop the middle entry */
    ASSERT_EQ(asx_cleanup_pop(&stack, h2), ASX_OK);
    ASSERT_EQ(asx_cleanup_pending(&stack), (uint32_t)2);

    asx_cleanup_drain(&stack);

    /* Only entries 1 and 3 should fire, in LIFO order: 3, 1 */
    ASSERT_EQ(g_cleanup_log_count, 2);
    ASSERT_EQ(g_cleanup_log[0], 3);
    ASSERT_EQ(g_cleanup_log[1], 1);
}

TEST(cleanup_drain_idempotent)
{
    asx_cleanup_stack stack;
    asx_cleanup_handle h;
    int v = 42;

    cleanup_log_reset();
    asx_cleanup_init(&stack);

    ASSERT_EQ(asx_cleanup_push(&stack, cleanup_record, &v, &h), ASX_OK);

    asx_cleanup_drain(&stack);
    ASSERT_EQ(g_cleanup_log_count, 1);

    /* Second drain is a no-op */
    asx_cleanup_drain(&stack);
    ASSERT_EQ(g_cleanup_log_count, 1);
}

TEST(cleanup_capacity_limit)
{
    asx_cleanup_stack stack;
    asx_cleanup_handle h;
    int v = 0;
    uint32_t i;

    asx_cleanup_init(&stack);

    /* Fill to capacity */
    for (i = 0; i < ASX_CLEANUP_STACK_CAPACITY; i++) {
        ASSERT_EQ(asx_cleanup_push(&stack, cleanup_record, &v, &h), ASX_OK);
    }

    /* One more should fail */
    ASSERT_EQ(asx_cleanup_push(&stack, cleanup_record, &v, &h),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(cleanup_pop_invalid_handle)
{
    asx_cleanup_stack stack;
    asx_cleanup_handle h;
    int v = 1;

    asx_cleanup_init(&stack);
    ASSERT_EQ(asx_cleanup_push(&stack, cleanup_record, &v, &h), ASX_OK);

    /* Pop with out-of-range handle */
    ASSERT_EQ(asx_cleanup_pop(&stack, 99), ASX_E_NOT_FOUND);

    /* Double pop same handle */
    ASSERT_EQ(asx_cleanup_pop(&stack, h), ASX_OK);
    ASSERT_EQ(asx_cleanup_pop(&stack, h), ASX_E_NOT_FOUND);
}

TEST(cleanup_drain_during_region_finalize)
{
    asx_region_id rid;
    asx_budget budget;
    int v1 = 100, v2 = 200;
    asx_cleanup_handle h1, h2;

    cleanup_log_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Push cleanups onto the region's internal stack */
    {
        uint16_t slot_idx = asx_handle_slot(rid);
        asx_cleanup_stack *stk = &g_regions[slot_idx].cleanup;

        ASSERT_EQ(asx_cleanup_push(stk, cleanup_record, &v1, &h1), ASX_OK);
        ASSERT_EQ(asx_cleanup_push(stk, cleanup_record, &v2, &h2), ASX_OK);
    }

    /* Drain should invoke cleanups during finalization */
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);

    /* Cleanup callbacks should have fired in LIFO order */
    ASSERT_EQ(g_cleanup_log_count, 2);
    ASSERT_EQ(g_cleanup_log[0], 200);
    ASSERT_EQ(g_cleanup_log[1], 100);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== handle_safety_test ===\n");

    /* Handle safety tests */
    asx_runtime_reset(); RUN_TEST(stale_region_handle_after_recycle);
    asx_runtime_reset(); RUN_TEST(stale_handle_on_region_close);
    asx_runtime_reset(); RUN_TEST(stale_handle_spawn_rejected);
    asx_runtime_reset(); RUN_TEST(invalid_handle_returns_not_found);
    asx_runtime_reset(); RUN_TEST(task_handle_type_mismatch);
    asx_runtime_reset(); RUN_TEST(generation_preserved_in_handle_round_trip);
    asx_runtime_reset(); RUN_TEST(generation_increments_on_recycle);

    /* Cleanup-stack tests */
    RUN_TEST(cleanup_stack_lifo_order);
    RUN_TEST(cleanup_pop_skips_during_drain);
    RUN_TEST(cleanup_drain_idempotent);
    RUN_TEST(cleanup_capacity_limit);
    RUN_TEST(cleanup_pop_invalid_handle);
    asx_runtime_reset(); RUN_TEST(cleanup_drain_during_region_finalize);

    TEST_REPORT();
    return test_failures;
}
