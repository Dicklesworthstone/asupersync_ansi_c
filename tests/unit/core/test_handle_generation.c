/*
 * test_handle_generation.c — unit tests for generation-safe handle validation
 *
 * Tests stale-handle detection: after slot reclaim, old handles must fail
 * lookups because the generation counter has incremented.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/runtime/runtime.h>

/* ---- Handle packing helpers ---- */

TEST(handle_pack_index_roundtrip) {
    uint16_t gen = 42;
    uint16_t slot = 7;
    uint32_t packed = asx_handle_pack_index(gen, slot);
    uint64_t h = asx_handle_pack(ASX_TYPE_REGION, 0x0001, packed);

    ASSERT_EQ(asx_handle_slot(h), slot);
    ASSERT_EQ(asx_handle_generation(h), gen);
    /* asx_handle_index returns the full composite */
    ASSERT_EQ(asx_handle_index(h), packed);
}

TEST(handle_pack_index_zero) {
    uint32_t packed = asx_handle_pack_index(0, 0);
    uint64_t h = asx_handle_pack(ASX_TYPE_TASK, 0x0001, packed);
    ASSERT_EQ(asx_handle_slot(h), (uint16_t)0);
    ASSERT_EQ(asx_handle_generation(h), (uint16_t)0);
}

TEST(handle_pack_index_max) {
    uint32_t packed = asx_handle_pack_index(0xFFFF, 0xFFFF);
    uint64_t h = asx_handle_pack(ASX_TYPE_REGION, 0xFFFF, packed);
    ASSERT_EQ(asx_handle_slot(h), (uint16_t)0xFFFF);
    ASSERT_EQ(asx_handle_generation(h), (uint16_t)0xFFFF);
}

TEST(handle_generation_independent_of_type_tag) {
    uint32_t packed = asx_handle_pack_index(5, 3);
    uint64_t h1 = asx_handle_pack(ASX_TYPE_REGION, 0, packed);
    uint64_t h2 = asx_handle_pack(ASX_TYPE_TASK, 0, packed);
    /* Same generation and slot regardless of type */
    ASSERT_EQ(asx_handle_generation(h1), asx_handle_generation(h2));
    ASSERT_EQ(asx_handle_slot(h1), asx_handle_slot(h2));
    /* But different type tags */
    ASSERT_NE(asx_handle_type_tag(h1), asx_handle_type_tag(h2));
}

/* ---- Region stale-handle detection ---- */

TEST(region_stale_handle_after_reclaim) {
    asx_region_id first_id, second_id;
    asx_region_state state;
    asx_budget budget;

    asx_runtime_reset();

    /* Open and drain a region to CLOSED */
    ASSERT_EQ(asx_region_open(&first_id), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(first_id, &budget), ASX_OK);

    /* The slot is now CLOSED — open a new region that reclaims it */
    ASSERT_EQ(asx_region_open(&second_id), ASX_OK);

    /* The new handle should work */
    ASSERT_EQ(asx_region_get_state(second_id, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);

    /* The old handle should fail — generation mismatch */
    ASSERT_NE(asx_region_get_state(first_id, &state), ASX_OK);
}

TEST(region_stale_handle_different_generation) {
    asx_region_id first_id, second_id;

    asx_runtime_reset();

    /* Open and drain */
    ASSERT_EQ(asx_region_open(&first_id), ASX_OK);
    {
        asx_budget b = asx_budget_infinite();
        ASSERT_EQ(asx_region_drain(first_id, &b), ASX_OK);
    }

    /* Reclaim */
    ASSERT_EQ(asx_region_open(&second_id), ASX_OK);

    /* Verify generations differ */
    ASSERT_NE(asx_handle_generation(first_id),
              asx_handle_generation(second_id));

    /* But same slot */
    ASSERT_EQ(asx_handle_slot(first_id),
              asx_handle_slot(second_id));
}

TEST(region_fresh_handle_generation_zero) {
    asx_region_id id;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&id), ASX_OK);
    ASSERT_EQ(asx_handle_generation(id), (uint16_t)0);
}

TEST(region_reclaimed_handle_generation_increments) {
    asx_region_id ids[3];
    asx_budget budget;
    int i;

    asx_runtime_reset();

    /* Open, drain, and reclaim the same slot 3 times */
    for (i = 0; i < 3; i++) {
        ASSERT_EQ(asx_region_open(&ids[i]), ASX_OK);
        budget = asx_budget_infinite();
        ASSERT_EQ(asx_region_drain(ids[i], &budget), ASX_OK);
    }

    /* Each generation should be higher than the previous */
    ASSERT_EQ(asx_handle_generation(ids[0]), (uint16_t)0);
    ASSERT_EQ(asx_handle_generation(ids[1]), (uint16_t)1);
    ASSERT_EQ(asx_handle_generation(ids[2]), (uint16_t)2);
}

/* ---- Task handle uses generation from spawn ---- */

static asx_status noop_poll(void *ud, asx_task_id self)
{
    (void)ud; (void)self;
    return ASX_OK;
}

TEST(task_handle_has_generation_zero) {
    asx_region_id rid;
    asx_task_id tid;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_handle_generation(tid), (uint16_t)0);
}

TEST(task_lookup_validates_generation) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid), ASX_OK);

    /* Valid handle works */
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_CREATED);

    /* Forge a handle with wrong generation — should fail */
    {
        uint64_t forged = asx_handle_pack(
            ASX_TYPE_TASK,
            asx_handle_state_mask(tid),
            asx_handle_pack_index(99, asx_handle_slot(tid)));
        ASSERT_EQ(asx_task_get_state(forged, &state), ASX_E_STALE_HANDLE);
    }
}

int main(void) {
    fprintf(stderr, "=== test_handle_generation ===\n");
    RUN_TEST(handle_pack_index_roundtrip);
    RUN_TEST(handle_pack_index_zero);
    RUN_TEST(handle_pack_index_max);
    RUN_TEST(handle_generation_independent_of_type_tag);
    RUN_TEST(region_stale_handle_after_reclaim);
    RUN_TEST(region_stale_handle_different_generation);
    RUN_TEST(region_fresh_handle_generation_zero);
    RUN_TEST(region_reclaimed_handle_generation_increments);
    RUN_TEST(task_handle_has_generation_zero);
    RUN_TEST(task_lookup_validates_generation);
    TEST_REPORT();
    return test_failures;
}
