/*
 * test_resource.c — exhaustion boundary tests for resource contract engine
 *
 * Tests resource capacity queries, admission gates, arena exhaustion
 * for all resource kinds, per-region queries, and failure-atomic
 * rollback on multi-step operations.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/core/resource.h>
#include <asx/runtime/runtime.h>

/* Trivial poll function for spawning tasks */
static asx_status noop_poll(void *user_data, asx_task_id self)
{
    (void)user_data;
    (void)self;
    return ASX_OK;
}

/* ---- Resource query accuracy ---- */

TEST(resource_capacity_region) {
    ASSERT_EQ(asx_resource_capacity(ASX_RESOURCE_REGION),
              (uint32_t)ASX_MAX_REGIONS);
}

TEST(resource_capacity_task) {
    ASSERT_EQ(asx_resource_capacity(ASX_RESOURCE_TASK),
              (uint32_t)ASX_MAX_TASKS);
}

TEST(resource_capacity_obligation) {
    ASSERT_EQ(asx_resource_capacity(ASX_RESOURCE_OBLIGATION),
              (uint32_t)ASX_MAX_OBLIGATIONS);
}

TEST(resource_capacity_unknown) {
    ASSERT_EQ(asx_resource_capacity(ASX_RESOURCE_KIND_COUNT), (uint32_t)0);
}

TEST(resource_used_after_reset) {
    asx_runtime_reset();
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_REGION), (uint32_t)0);
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_TASK), (uint32_t)0);
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_OBLIGATION), (uint32_t)0);
}

TEST(resource_remaining_matches_capacity_minus_used) {
    asx_region_id rid;
    asx_runtime_reset();

    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_REGION),
              (uint32_t)ASX_MAX_REGIONS);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_REGION), (uint32_t)1);
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_REGION),
              (uint32_t)(ASX_MAX_REGIONS - 1));
}

/* ---- Snapshot consistency ---- */

TEST(resource_snapshot_matches_queries) {
    asx_resource_snapshot snap;
    asx_region_id rid;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_resource_snapshot_get(ASX_RESOURCE_REGION, &snap), ASX_OK);
    ASSERT_EQ(snap.kind, ASX_RESOURCE_REGION);
    ASSERT_EQ(snap.capacity, (uint32_t)ASX_MAX_REGIONS);
    ASSERT_EQ(snap.used, (uint32_t)1);
    ASSERT_EQ(snap.remaining, (uint32_t)(ASX_MAX_REGIONS - 1));
}

TEST(resource_snapshot_null_output) {
    ASSERT_EQ(asx_resource_snapshot_get(ASX_RESOURCE_REGION, NULL),
              ASX_E_INVALID_ARGUMENT);
}

TEST(resource_snapshot_invalid_kind) {
    asx_resource_snapshot snap;
    ASSERT_EQ(asx_resource_snapshot_get(ASX_RESOURCE_KIND_COUNT, &snap),
              ASX_E_INVALID_ARGUMENT);
}

/* ---- Admission gate ---- */

TEST(resource_admit_succeeds_when_available) {
    asx_runtime_reset();
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_REGION, 1), ASX_OK);
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_REGION, ASX_MAX_REGIONS), ASX_OK);
}

TEST(resource_admit_fails_when_exhausted) {
    asx_runtime_reset();
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_REGION, ASX_MAX_REGIONS + 1u),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(resource_admit_tracks_allocation) {
    asx_region_id rid;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    /* One region used, so can only admit MAX-1 more */
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_REGION, ASX_MAX_REGIONS),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_REGION, ASX_MAX_REGIONS - 1u),
              ASX_OK);
}

TEST(resource_admit_zero_count) {
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_REGION, 0),
              ASX_E_INVALID_ARGUMENT);
}

TEST(resource_admit_invalid_kind) {
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_KIND_COUNT, 1),
              ASX_E_INVALID_ARGUMENT);
}

/* ---- Region arena exhaustion ---- */

TEST(resource_region_arena_exhaustion) {
    asx_region_id ids[ASX_MAX_REGIONS];
    asx_region_id overflow;
    uint32_t i;
    asx_runtime_reset();

    /* Fill all region slots */
    for (i = 0; i < ASX_MAX_REGIONS; i++) {
        ASSERT_EQ(asx_region_open(&ids[i]), ASX_OK);
    }
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_REGION), (uint32_t)0);

    /* Next allocation should fail */
    ASSERT_EQ(asx_region_open(&overflow), ASX_E_RESOURCE_EXHAUSTED);

    /* State should be consistent: still exactly MAX_REGIONS in use */
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_REGION),
              (uint32_t)ASX_MAX_REGIONS);
}

TEST(resource_region_reclaim_after_drain) {
    asx_region_id ids[ASX_MAX_REGIONS];
    asx_region_id recycled;
    asx_budget budget;
    uint32_t i;
    asx_runtime_reset();

    /* Fill all region slots */
    for (i = 0; i < ASX_MAX_REGIONS; i++) {
        ASSERT_EQ(asx_region_open(&ids[i]), ASX_OK);
    }

    /* Drain one region to make it reclaimable */
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(ids[0], &budget), ASX_OK);

    /* Now a new region can be opened (slot recycled) */
    ASSERT_EQ(asx_region_open(&recycled), ASX_OK);

    /* The recycled handle should be different from the original */
    ASSERT_NE(recycled, ids[0]);
}

/* ---- Task arena exhaustion ---- */

TEST(resource_task_arena_exhaustion) {
    asx_region_id rid;
    asx_task_id tids[ASX_MAX_TASKS];
    asx_task_id overflow;
    uint32_t i;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Fill all task slots */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tids[i]), ASX_OK);
    }
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_TASK), (uint32_t)0);

    /* Next spawn should fail */
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &overflow),
              ASX_E_RESOURCE_EXHAUSTED);

    /* Counter should not have advanced */
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_TASK), (uint32_t)ASX_MAX_TASKS);
}

/* ---- Obligation arena exhaustion ---- */

TEST(resource_obligation_arena_exhaustion) {
    asx_region_id rid;
    asx_obligation_id oids[ASX_MAX_OBLIGATIONS];
    asx_obligation_id overflow;
    uint32_t i;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Fill all obligation slots */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        ASSERT_EQ(asx_obligation_reserve(rid, &oids[i]), ASX_OK);
    }
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_OBLIGATION), (uint32_t)0);

    /* Next reservation should fail */
    ASSERT_EQ(asx_obligation_reserve(rid, &overflow),
              ASX_E_RESOURCE_EXHAUSTED);

    /* Counter should not have advanced */
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_OBLIGATION),
              (uint32_t)ASX_MAX_OBLIGATIONS);
}

/* ---- Cleanup stack exhaustion ---- */

static void dummy_cleanup(void *data)
{
    (void)data;
}

TEST(resource_cleanup_stack_exhaustion) {
    asx_region_id rid;
    asx_cleanup_handle h;
    uint32_t i, remaining;
    asx_cleanup_stack stack;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Fill cleanup stack directly */
    asx_cleanup_init(&stack);
    for (i = 0; i < ASX_CLEANUP_STACK_CAPACITY; i++) {
        ASSERT_EQ(asx_cleanup_push(&stack, dummy_cleanup, NULL, &h), ASX_OK);
    }

    /* Next push should fail */
    ASSERT_EQ(asx_cleanup_push(&stack, dummy_cleanup, NULL, &h),
              ASX_E_RESOURCE_EXHAUSTED);

    /* Query remaining via region API */
    ASSERT_EQ(asx_resource_region_cleanup_remaining(rid, &remaining), ASX_OK);
    ASSERT_EQ(remaining, (uint32_t)ASX_CLEANUP_STACK_CAPACITY);
}

/* ---- Capture arena exhaustion ---- */

TEST(resource_capture_arena_exhaustion) {
    asx_region_id rid;
    asx_task_id tid;
    void *state;
    uint32_t capture_remaining;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Check initial capture remaining */
    ASSERT_EQ(asx_resource_region_capture_remaining(rid, &capture_remaining),
              ASX_OK);
    ASSERT_EQ(capture_remaining, (uint32_t)ASX_REGION_CAPTURE_ARENA_BYTES);

    /* Allocate a large chunk from the capture arena */
    ASSERT_EQ(asx_task_spawn_captured(rid, noop_poll,
              ASX_REGION_CAPTURE_ARENA_BYTES / 2u, NULL, &tid, &state),
              ASX_OK);

    /* Check reduced remaining */
    ASSERT_EQ(asx_resource_region_capture_remaining(rid, &capture_remaining),
              ASX_OK);
    ASSERT_TRUE(capture_remaining <= ASX_REGION_CAPTURE_ARENA_BYTES / 2u);

    /* Try to allocate more than remaining — should fail */
    ASSERT_EQ(asx_task_spawn_captured(rid, noop_poll,
              capture_remaining + 1u, NULL, &tid, &state),
              ASX_E_RESOURCE_EXHAUSTED);
}

/* ---- Failure-atomic rollback: capture arena on task exhaustion ---- */

TEST(resource_capture_rollback_on_task_exhaustion) {
    asx_region_id rid;
    asx_task_id tid;
    void *state;
    uint32_t capture_before, capture_after;
    uint32_t i;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Fill the task arena to near-capacity, leaving 1 slot */
    for (i = 0; i < ASX_MAX_TASKS - 1u; i++) {
        ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid), ASX_OK);
    }

    /* Use the last task slot with a captured spawn */
    ASSERT_EQ(asx_resource_region_capture_remaining(rid, &capture_before),
              ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(rid, noop_poll, 64u, NULL, &tid, &state),
              ASX_OK);

    /* Now tasks are exhausted. Try another captured spawn. */
    ASSERT_EQ(asx_resource_region_capture_remaining(rid, &capture_before),
              ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(rid, noop_poll, 64u, NULL, &tid, &state),
              ASX_E_RESOURCE_EXHAUSTED);

    /* Capture arena should be rolled back — no bytes consumed */
    ASSERT_EQ(asx_resource_region_capture_remaining(rid, &capture_after),
              ASX_OK);
    ASSERT_EQ(capture_after, capture_before);
}

/* ---- Per-region queries ---- */

TEST(resource_region_capture_remaining_invalid_region) {
    uint32_t remaining;
    asx_runtime_reset();

    ASSERT_NE(asx_resource_region_capture_remaining(ASX_INVALID_ID,
                                                      &remaining), ASX_OK);
}

TEST(resource_region_cleanup_remaining_fresh) {
    asx_region_id rid;
    uint32_t remaining;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_resource_region_cleanup_remaining(rid, &remaining), ASX_OK);
    ASSERT_EQ(remaining, (uint32_t)ASX_CLEANUP_STACK_CAPACITY);
}

TEST(resource_region_capture_remaining_null_output) {
    asx_region_id rid;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_resource_region_capture_remaining(rid, NULL),
              ASX_E_INVALID_ARGUMENT);
}

TEST(resource_region_cleanup_remaining_null_output) {
    asx_region_id rid;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_resource_region_cleanup_remaining(rid, NULL),
              ASX_E_INVALID_ARGUMENT);
}

/* ---- Diagnostic ---- */

TEST(resource_kind_str_coverage) {
    ASSERT_STR_EQ(asx_resource_kind_str(ASX_RESOURCE_REGION), "region");
    ASSERT_STR_EQ(asx_resource_kind_str(ASX_RESOURCE_TASK), "task");
    ASSERT_STR_EQ(asx_resource_kind_str(ASX_RESOURCE_OBLIGATION), "obligation");
    ASSERT_STR_EQ(asx_resource_kind_str(ASX_RESOURCE_KIND_COUNT), "unknown");
}

/* ---- Determinism: exhaustion error is stable across calls ---- */

TEST(resource_exhaustion_deterministic) {
    asx_region_id ids[ASX_MAX_REGIONS];
    asx_region_id overflow;
    uint32_t i;

    /* Run twice to verify same exhaustion behavior */
    for (i = 0; i < 2u; i++) {
        uint32_t j;
        asx_runtime_reset();
        for (j = 0; j < ASX_MAX_REGIONS; j++) {
            ASSERT_EQ(asx_region_open(&ids[j]), ASX_OK);
        }
        ASSERT_EQ(asx_region_open(&overflow), ASX_E_RESOURCE_EXHAUSTED);
        ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_REGION), (uint32_t)0);
    }
}

/* ---- Integration: admit + allocate consistency ---- */

TEST(resource_admit_then_allocate) {
    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Admit check should predict success */
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_TASK, 1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid), ASX_OK);

    /* Fill the rest */
    for (i = 1; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid), ASX_OK);
    }

    /* Admit check should predict failure */
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_TASK, 1),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid),
              ASX_E_RESOURCE_EXHAUSTED);
}

int main(void)
{
    fprintf(stderr, "=== test_resource ===\n");

    /* Resource query accuracy */
    RUN_TEST(resource_capacity_region);
    RUN_TEST(resource_capacity_task);
    RUN_TEST(resource_capacity_obligation);
    RUN_TEST(resource_capacity_unknown);
    RUN_TEST(resource_used_after_reset);
    RUN_TEST(resource_remaining_matches_capacity_minus_used);

    /* Snapshot consistency */
    RUN_TEST(resource_snapshot_matches_queries);
    RUN_TEST(resource_snapshot_null_output);
    RUN_TEST(resource_snapshot_invalid_kind);

    /* Admission gate */
    RUN_TEST(resource_admit_succeeds_when_available);
    RUN_TEST(resource_admit_fails_when_exhausted);
    RUN_TEST(resource_admit_tracks_allocation);
    RUN_TEST(resource_admit_zero_count);
    RUN_TEST(resource_admit_invalid_kind);

    /* Arena exhaustion */
    RUN_TEST(resource_region_arena_exhaustion);
    RUN_TEST(resource_region_reclaim_after_drain);
    RUN_TEST(resource_task_arena_exhaustion);
    RUN_TEST(resource_obligation_arena_exhaustion);

    /* Cleanup stack exhaustion */
    RUN_TEST(resource_cleanup_stack_exhaustion);

    /* Capture arena exhaustion */
    RUN_TEST(resource_capture_arena_exhaustion);

    /* Failure-atomic rollback */
    RUN_TEST(resource_capture_rollback_on_task_exhaustion);

    /* Per-region queries */
    RUN_TEST(resource_region_capture_remaining_invalid_region);
    RUN_TEST(resource_region_cleanup_remaining_fresh);
    RUN_TEST(resource_region_capture_remaining_null_output);
    RUN_TEST(resource_region_cleanup_remaining_null_output);

    /* Diagnostic */
    RUN_TEST(resource_kind_str_coverage);

    /* Determinism */
    RUN_TEST(resource_exhaustion_deterministic);

    /* Integration */
    RUN_TEST(resource_admit_then_allocate);

    TEST_REPORT();
    return test_failures;
}
