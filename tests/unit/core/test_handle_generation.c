/*
 * test_handle_generation.c — unit tests for generation-safe handle validation
 *
 * Tests stale-handle detection: after slot reclaim, old handles must fail
 * lookups because the generation counter has incremented.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include "test_runtime.h"
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

    asx_runtime_reset(test_runtime());

    /* Open and drain a region to CLOSED */
    ASSERT_EQ(asx_region_open(test_runtime(), &first_id), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(first_id, &budget), ASX_OK);

    /* The slot is now CLOSED — open a new region that reclaims it */
    ASSERT_EQ(asx_region_open(test_runtime(), &second_id), ASX_OK);

    /* The new handle should work */
    ASSERT_EQ(asx_region_get_state(second_id, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);

    /* The old handle should fail — generation mismatch */
    ASSERT_NE(asx_region_get_state(first_id, &state), ASX_OK);
}

TEST(region_stale_handle_different_generation) {
    asx_region_id first_id, second_id;

    asx_runtime_reset(test_runtime());

    /* Open and drain */
    ASSERT_EQ(asx_region_open(test_runtime(), &first_id), ASX_OK);
    {
        asx_budget b = asx_budget_infinite();
        ASSERT_EQ(asx_region_drain(first_id, &b), ASX_OK);
    }

    /* Reclaim */
    ASSERT_EQ(asx_region_open(test_runtime(), &second_id), ASX_OK);

    /* Verify generations differ */
    ASSERT_NE(asx_handle_generation(first_id), asx_handle_generation(second_id));

    /* But same slot */
    ASSERT_EQ(asx_handle_slot(first_id), asx_handle_slot(second_id));
}

TEST(region_fresh_handle_generation_zero) {
    asx_region_id id;
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &id), ASX_OK);
    ASSERT_EQ(asx_handle_generation(id), (uint16_t)0);
}

TEST(region_reclaimed_handle_generation_increments) {
    asx_region_id ids[3];
    asx_budget budget;
    int i;

    asx_runtime_reset(test_runtime());

    /* Open, drain, and reclaim the same slot 3 times */
    for (i = 0; i < 3; i++) {
        ASSERT_EQ(asx_region_open(test_runtime(), &ids[i]), ASX_OK);
        budget = asx_budget_infinite();
        ASSERT_EQ(asx_region_drain(ids[i], &budget), ASX_OK);
    }

    /* Each generation should be higher than the previous */
    ASSERT_EQ(asx_handle_generation(ids[0]), (uint16_t)0);
    ASSERT_EQ(asx_handle_generation(ids[1]), (uint16_t)1);
    ASSERT_EQ(asx_handle_generation(ids[2]), (uint16_t)2);
}

/* ---- Task handle uses generation from spawn ---- */

static asx_status noop_poll(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

TEST(task_handle_has_generation_zero) {
    asx_region_id rid;
    asx_task_id tid;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_handle_generation(tid), (uint16_t)0);
}

TEST(task_lookup_validates_generation) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid), ASX_OK);

    /* Valid handle works */
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_CREATED);

    /* Forge a handle with wrong generation — should fail */
    {
        uint64_t forged = asx_handle_pack(ASX_TYPE_TASK, asx_handle_state_mask(tid),
                                          asx_handle_pack_index(99, asx_handle_slot(tid)));
        ASSERT_EQ(asx_task_get_state(forged, &state), ASX_E_STALE_HANDLE);
    }
}

/* A completed task nobody joined keeps its closed region's slot: the new
 * region gets another slot, and the outcome stays joinable (a Rust
 * JoinHandle outlives its region's record). Once joined, the slot is
 * recycled. */
TEST(unjoined_task_keeps_closed_region_slot) {
    asx_region_id first;
    asx_region_id second;
    asx_region_id third;
    asx_task_id tid;
    asx_budget budget;
    asx_outcome out;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &first), ASX_OK);
    ASSERT_EQ(asx_task_spawn(first, noop_poll, NULL, &tid), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(first, &budget), ASX_OK); /* completes OK */
    ASSERT_EQ(asx_region_drain(first, &budget), ASX_OK);

    ASSERT_EQ(asx_region_open(test_runtime(), &second), ASX_OK);
    ASSERT_NE(asx_handle_slot(second), asx_handle_slot(first));
    ASSERT_EQ(asx_task_join(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_OK);

    ASSERT_EQ(asx_region_open(test_runtime(), &third), ASX_OK);
    ASSERT_EQ(asx_handle_slot(third), asx_handle_slot(first));
    ASSERT_NE(asx_handle_generation(third), asx_handle_generation(first));
}

/* K18: a handle names the runtime that issued it. One carrying another
 * runtime's tag is not found by any lookup, even though its slot and
 * generation are exactly those of a live entity, so it cannot resolve
 * through a coincident slot of the runtime that did not issue it. */
TEST(handle_of_another_runtime_is_not_found_through_a_coincident_slot) {
    asx_region_id rid;
    asx_task_id tid;
    asx_obligation_id oid;
    asx_region_id foreign_region;
    asx_task_id foreign_task;
    asx_obligation_id foreign_obligation;
    asx_region_state rstate;
    asx_task_state tstate;
    asx_obligation_state ostate;
    asx_region_id child;
    asx_task_id other;
    asx_obligation_id extra;
    asx_cancel_reason req;
    asx_budget drain_budget;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(rid, ASX_OBLIGATION_KIND_GENERIC, tid, &oid), ASX_OK);

    /* What this runtime issues carries its own tag. */
    ASSERT_EQ(asx_handle_runtime_tag(rid), (uint16_t)0);
    ASSERT_EQ(asx_handle_runtime_tag(tid), (uint16_t)0);
    ASSERT_EQ(asx_handle_runtime_tag(oid), (uint16_t)0);

    /* The same handles with another runtime's tag differ only in the tag. */
    foreign_region = asx_handle_with_runtime_tag(rid, 1u);
    foreign_task = asx_handle_with_runtime_tag(tid, 1u);
    foreign_obligation = asx_handle_with_runtime_tag(oid, 1u);
    ASSERT_NE(foreign_region, rid);
    ASSERT_EQ(asx_handle_slot(foreign_region), asx_handle_slot(rid));
    ASSERT_EQ(asx_handle_generation(foreign_region), asx_handle_generation(rid));

    ASSERT_EQ(asx_region_get_state(foreign_region, &rstate), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_task_get_state(foreign_task, &tstate), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_get_state(foreign_obligation, &ostate), ASX_E_NOT_FOUND);

    /* Used as the target of an operation, it is refused and creates nothing. */
    ASSERT_EQ(asx_task_spawn(foreign_region, noop_poll, NULL, &other), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_open_child(foreign_region, &child), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_reserve_ex(rid, ASX_OBLIGATION_KIND_GENERIC, foreign_task, &extra),
              ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_commit(foreign_obligation), ASX_E_NOT_FOUND);
    /* A region cancel and a drain of it name no kernel to read: the same
     * refusal, before either walks a subtree. */
    req = asx_cancel_reason_default(test_runtime(), ASX_CANCEL_SHUTDOWN, NULL);
    drain_budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_cancel(foreign_region, &req, NULL), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_drain(foreign_region, &drain_budget), ASX_E_NOT_FOUND);
    /* A scheduler run of it is refused where its kernel is taken, before the
     * lab or sweep walk reads one (the analyzer's NULL-kernel paths). */
    ASSERT_EQ(asx_scheduler_run(foreign_region, &drain_budget), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_scheduler_run_until_idle(foreign_region, &drain_budget), ASX_E_NOT_FOUND);

    /* The runtime's own handles are untouched by all of it. */
    ASSERT_EQ(asx_region_get_state(rid, &rstate), ASX_OK);
    ASSERT_EQ(rstate, ASX_REGION_OPEN);
    ASSERT_EQ(asx_task_get_state(tid, &tstate), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(oid, &ostate), ASX_OK);
    ASSERT_EQ(ostate, ASX_OBLIGATION_RESERVED);
}

/* Two handles name one entity when kind, runtime, slot and generation agree;
 * the state bits move with the entity and take no part. */
TEST(handle_same_entity_ignores_state_bits_and_tells_runtimes_apart) {
    uint64_t a = asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << 1), asx_handle_pack_index(3u, 7u));
    uint64_t same_other_state =
        asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << 4), asx_handle_pack_index(3u, 7u));

    ASSERT_TRUE(asx_handle_same_entity(a, a));
    ASSERT_TRUE(asx_handle_same_entity(a, same_other_state));
    /* Another slot, another generation, another kind. */
    ASSERT_FALSE(asx_handle_same_entity(a, asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << 1),
                                                           asx_handle_pack_index(3u, 8u))));
    ASSERT_FALSE(asx_handle_same_entity(a, asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << 1),
                                                           asx_handle_pack_index(4u, 7u))));
    ASSERT_FALSE(asx_handle_same_entity(a, asx_handle_pack(ASX_TYPE_REGION, (uint16_t)(1u << 1),
                                                           asx_handle_pack_index(3u, 7u))));
    /* Another runtime's handle with the same slot and generation, in any
     * state, is another entity. */
    ASSERT_FALSE(asx_handle_same_entity(a, asx_handle_with_runtime_tag(a, 1u)));
    ASSERT_FALSE(asx_handle_same_entity(a, asx_handle_with_runtime_tag(same_other_state, 2u)));
    ASSERT_TRUE(asx_handle_same_entity(asx_handle_with_runtime_tag(a, 1u),
                                       asx_handle_with_runtime_tag(same_other_state, 1u)));
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
    RUN_TEST(unjoined_task_keeps_closed_region_slot);
    RUN_TEST(handle_of_another_runtime_is_not_found_through_a_coincident_slot);
    RUN_TEST(handle_same_entity_ignores_state_bits_and_tells_runtimes_apart);
    TEST_REPORT();
    return test_failures;
}
