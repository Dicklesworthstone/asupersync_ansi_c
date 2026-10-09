/*
 * test_lab_dispatch.c — the Rust LabRuntime dispatch model
 * (asx_scheduler_use_lab_dispatch, bd-9kll.4.2).
 *
 * Expected values come from asupersync 5e60b1c4c: xorshift64 outputs for a
 * seed (src/util/det_rng.rs) and the pick of entry r % n among the
 * highest-priority entries in generation order (scheduler/priority.rs).
 * The Rust-vs-C fixtures (make conformance, make fuzz-differential) check
 * the same model end to end against captured Rust runs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../../src/runtime/runtime_internal.h"
#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <string.h>

static asx_runtime g_rt;

/* A runtime on the virtual clock (default stub hooks). */
static asx_status setup(void) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    return st;
}

/* Poll order record. */
static char g_order[32];
static uint32_t g_order_n = 0;

typedef struct {
    char name;
    uint32_t yields; /* polls that yield before completing */
    int checkpoint;  /* complete when a checkpoint reports a cancel */
} probe;

static asx_status poll_probe(void *ud, asx_task_id self) {
    probe *p = (probe *)ud;
    asx_checkpoint_result cr;
    if (g_order_n + 1u < sizeof(g_order)) g_order[g_order_n++] = p->name;
    if (p->checkpoint && asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_OK;
    if (p->yields > 0u) {
        p->yields--;
        return ASX_E_PENDING; /* not parked: a yield */
    }
    return ASX_OK;
}

static void order_reset(void) {
    memset(g_order, 0, sizeof(g_order));
    g_order_n = 0;
}

TEST(rng_replicates_rust_xorshift64) {
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    /* DetRng::new(42).next_u64() x5 */
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(45454805674));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(11532217803599905471));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(10021416941527320954));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(2899061411254629736));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(5661411637479084162));
}

TEST(rng_remaps_degenerate_seeds) {
    ASSERT_EQ(setup(), ASX_OK);
    /* Seed 0 starts from state 1. */
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(0u), ASX_OK);
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(1082269761));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(1152992998833853505));
    /* All ones is offset by the golden-ratio constant. */
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(UINT64_C(0xFFFFFFFFFFFFFFFF)), ASX_OK);
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(15860402103189073388));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(8426428137925997623));
}

/* Seed 42, three host-spawned tasks at priority 0 (generations 0, 1, 2).
 * Step 1: r1 % 3 = 1 picks B, which yields (generation 3). Step 2:
 * [A, C, B], r2 % 3 = 1 picks C. Step 3: [A, B], r3 % 2 = 0 picks A.
 * Step 4: B. */
TEST(ties_are_broken_by_the_step_value_in_generation_order) {
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    probe a = {'A', 0u, 0};
    probe b = {'B', 1u, 0};
    probe c = {'C', 0u, 0};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    order_reset();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &b, &t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &c, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BCAB"), 0);
}

/* A cancelled task goes to the cancel lane, which is served before the
 * ready lane whatever the step value. */
TEST(cancel_lane_is_served_first) {
    asx_region_id r;
    asx_task_id ta;
    asx_task_id tb;
    asx_budget budget;
    probe a = {'A', 0u, 1};
    probe b = {'B', 0u, 1};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    order_reset();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &ta), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &b, &tb), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tb, ASX_CANCEL_USER), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BA"), 0);
}

/* run_with_auto_advance: with nothing scheduled the clock moves to the next
 * timer, which wakes the sleeper. */
typedef struct {
    asx_time woke_at;
    int armed;
} sleeper;

static asx_status poll_sleeper(void *ud, asx_task_id self) {
    sleeper *s = (sleeper *)ud;
    asx_status st = asx_task_wait_until(self, 100u);
    s->armed = 1;
    if (st == ASX_OK) {
        s->woke_at = asx_runtime_virtual_now();
        return ASX_OK;
    }
    return st;
}

TEST(auto_advance_moves_the_clock_to_the_next_timer) {
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    sleeper s;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(7u), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_sleeper, &s, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    /* run_until_idle stops with the sleeper parked and the clock at 0. */
    ASSERT_EQ(asx_scheduler_run_until_idle(r, &budget), ASX_E_PENDING);
    ASSERT_TRUE(s.armed);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)0);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(s.woke_at, (asx_time)100);
}

TEST(use_lab_dispatch_requires_no_live_task) {
    asx_region_id r;
    asx_task_id t;
    probe a = {'A', 0u, 0};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &t), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(1u), ASX_E_INVALID_STATE);
    /* A runtime reset turns it off again. */
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_lab_dispatch_active(), 0);
}

int main(void) {
    fprintf(stderr, "=== test_lab_dispatch ===\n");
    RUN_TEST(rng_replicates_rust_xorshift64);
    RUN_TEST(rng_remaps_degenerate_seeds);
    RUN_TEST(ties_are_broken_by_the_step_value_in_generation_order);
    RUN_TEST(cancel_lane_is_served_first);
    RUN_TEST(auto_advance_moves_the_clock_to_the_next_timer);
    RUN_TEST(use_lab_dispatch_requires_no_live_task);

    TEST_REPORT();
    return test_failures;
}
