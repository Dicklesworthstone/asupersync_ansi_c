/*
 * test_hft_microburst.c — HFT microburst fixture family (bd-1md.8)
 *
 * Validates deterministic overload behavior under market-open style
 * task bursts. Tests admission gates, backpressure, fairness, and
 * replay identity under high-concurrency load profiles.
 *
 * Fixture family: VERT-HFT-MICROBURST
 * Scenarios:
 *   hft-burst-admission     — capacity gate under rapid spawn
 *   hft-burst-fairness      — round-robin starvation check
 *   hft-burst-budget        — tight budget + many tasks
 *   hft-burst-mixed         — heterogeneous completion times
 *   hft-burst-cancel-storm  — mass cancellation during burst
 *   hft-burst-replay        — deterministic replay identity
 *   hft-burst-overload      — spawn beyond capacity, clean rejection
 *   hft-burst-drain         — quiescence after burst completion
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
#include <asx/core/outcome.h>
#include <asx/core/resource.h>
#include <asx/runtime/runtime.h>
#include <string.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define IGNORE(expr)                                                                               \
    do {                                                                                           \
        asx_status s_ = (expr);                                                                    \
        (void)s_;                                                                                  \
    } while (0)

/* -------------------------------------------------------------------
 * Poll functions for various task behaviors
 * ------------------------------------------------------------------- */

/* Completes immediately (single poll) */
static asx_status poll_immediate(void *ctx, asx_task_id id) {
    (void)ctx;
    (void)id;
    return ASX_OK;
}

/* Yields N times then completes */
typedef struct {
    int target_polls;
    int polls_done;
} yield_ctx;

static asx_status poll_yield_n(void *user_data, asx_task_id id) {
    yield_ctx *ctx = (yield_ctx *)user_data;
    (void)id;
    ctx->polls_done++;
    if (ctx->polls_done >= ctx->target_polls) return ASX_OK;
    return ASX_E_PENDING;
}

/* Checkpoint-aware: completes when cancelled */
static asx_status poll_checkpoint_comply(void *ctx, asx_task_id id) {
    asx_checkpoint_result cr;
    asx_status st;
    (void)ctx;

    st = asx_checkpoint(id, &cr);
    if (st == ASX_OK && cr.cancelled) return ASX_OK;
    return ASX_E_PENDING;
}

/* Fails with an error */
static asx_status poll_fail(void *ctx, asx_task_id id) {
    (void)ctx;
    (void)id;
    return ASX_E_INVALID_STATE;
}

static void reset_all(void) {
    asx_runtime_reset();
    asx_ghost_reset();
    asx_scheduler_event_reset();
}

/* -------------------------------------------------------------------
 * hft-burst-admission: rapid spawn up to capacity
 *
 * Simulates market-open where many order handlers spawn at once.
 * Verifies all slots are filled and capacity query is consistent.
 * ------------------------------------------------------------------- */

TEST(hft_burst_admission_fills_capacity) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id tids[ASX_MAX_TASKS];
    uint32_t i;
    uint32_t remaining;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Pre-check: capacity should be available */
    remaining = asx_resource_remaining(ASX_RESOURCE_TASK);
    ASSERT_EQ(remaining, (uint32_t)ASX_MAX_TASKS);

    /* Burst: spawn all slots */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_immediate, NULL, &tids[i]), ASX_OK);
    }

    /* All slots consumed */
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_TASK), 0u);
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_TASK), (uint32_t)ASX_MAX_TASKS);
}

/* -------------------------------------------------------------------
 * hft-burst-overload: spawn beyond capacity, clean rejection
 *
 * After capacity is full, additional spawns must fail cleanly
 * without corrupting existing task state.
 * ------------------------------------------------------------------- */

TEST(hft_burst_overload_rejected_cleanly) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id tid;
    asx_task_id overflow_tid = ASX_INVALID_ID;
    uint32_t i;
    asx_budget budget;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Fill all task slots */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_immediate, NULL, &tid), ASX_OK);
    }

    /* Overflow spawn must fail */
    ASSERT_EQ(asx_task_spawn(rid, poll_immediate, NULL, &overflow_tid), ASX_E_RESOURCE_EXHAUSTED);

    /* Existing tasks must still run successfully */
    budget = asx_budget_from_polls(ASX_MAX_TASKS * 2);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
}

TEST(hft_burst_admission_gate_precheck) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id tid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Pre-check: can we admit 32 tasks? */
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_TASK, 32), ASX_OK);

    /* Spawn 32 */
    for (i = 0; i < 32; i++) { ASSERT_EQ(asx_task_spawn(rid, poll_immediate, NULL, &tid), ASX_OK); }

    /* Pre-check: can we admit 33 more? No (32 + 33 = 65 > 64) */
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_TASK, 33), ASX_E_RESOURCE_EXHAUSTED);

    /* But 32 more is OK */
    ASSERT_EQ(asx_resource_admit(ASX_RESOURCE_TASK, 32), ASX_OK);
}

/* -------------------------------------------------------------------
 * hft-burst-fairness: round-robin under load
 *
 * 32 tasks that each need 2 polls to complete. Under round-robin,
 * each task should be polled once per round. After 2 rounds, all
 * should be done. Verify no starvation.
 * ------------------------------------------------------------------- */

static yield_ctx g_fairness_ctx[32];

TEST(hft_burst_fairness_no_starvation) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id tids[32];
    asx_budget budget;
    uint32_t i;
    uint32_t completed;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Spawn 32 tasks, each needing 2 polls */
    for (i = 0; i < 32; i++) {
        g_fairness_ctx[i].target_polls = 2;
        g_fairness_ctx[i].polls_done = 0;
        ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &g_fairness_ctx[i], &tids[i]), ASX_OK);
    }

    /* Budget: 32 tasks * 2 polls each + headroom */
    budget = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* All tasks should have completed */
    completed = 0;
    for (i = 0; i < 32; i++) {
        asx_task_state state;
        ASSERT_EQ(asx_task_get_state(tids[i], &state), ASX_OK);
        if (state == ASX_TASK_COMPLETED) completed++;
    }
    ASSERT_EQ(completed, 32u);

    /* Each task should have been polled exactly its target number */
    for (i = 0; i < 32; i++) { ASSERT_EQ(g_fairness_ctx[i].polls_done, 2); }
}

TEST(hft_burst_fairness_round_robin_order) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id tids[4];
    asx_budget budget;
    asx_scheduler_event ev;
    uint32_t i;

    reset_all();

    /* Spawn 4 yield-once tasks: we expect round 0 polls all 4,
     * round 1 completes all 4 */
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    for (i = 0; i < 4; i++) {
        g_fairness_ctx[i].target_polls = 2;
        g_fairness_ctx[i].polls_done = 0;
        ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &g_fairness_ctx[i], &tids[i]), ASX_OK);
    }

    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Verify ascending task order within round 0 */
    {
        uint32_t round0_polls = 0;
        uint32_t prev_seq = 0;
        uint32_t ev_count = asx_scheduler_event_count();

        for (i = 0; i < ev_count; i++) {
            ASSERT_TRUE(asx_scheduler_event_get(i, &ev));
            if (ev.round == 0 && ev.kind == ASX_SCHED_EVENT_POLL) {
                ASSERT_TRUE(ev.sequence >= prev_seq);
                prev_seq = ev.sequence;
                round0_polls++;
            }
        }
        /* All 4 tasks polled in round 0 */
        ASSERT_EQ(round0_polls, 4u);
    }
}

/* -------------------------------------------------------------------
 * hft-burst-budget: tight budget forces partial completion
 *
 * 16 tasks, budget of only 8 polls. Should process first 8 and
 * exhaust budget. Remaining tasks stay non-terminal.
 * ------------------------------------------------------------------- */

TEST(hft_burst_tight_budget_partial_completion) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id tids[16];
    asx_budget budget;
    asx_status result;
    uint32_t i;
    uint32_t completed;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* 16 immediate-complete tasks */
    for (i = 0; i < 16; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_immediate, NULL, &tids[i]), ASX_OK);
    }

    /* Only 8 polls allowed — should exhaust */
    budget = asx_budget_from_polls(8);
    result = asx_scheduler_run(rid, &budget);
    ASSERT_EQ(result, ASX_E_POLL_BUDGET_EXHAUSTED);

    /* Exactly 8 should have completed */
    completed = 0;
    for (i = 0; i < 16; i++) {
        asx_task_state state;
        ASSERT_EQ(asx_task_get_state(tids[i], &state), ASX_OK);
        if (state == ASX_TASK_COMPLETED) completed++;
    }
    ASSERT_EQ(completed, 8u);

    /* Budget event should appear */
    {
        uint32_t ev_count = asx_scheduler_event_count();
        asx_scheduler_event last_ev;
        ASSERT_TRUE(ev_count > 0);
        ASSERT_TRUE(asx_scheduler_event_get(ev_count - 1, &last_ev));
        ASSERT_EQ(last_ev.kind, ASX_SCHED_EVENT_BUDGET);
    }
}

/* -------------------------------------------------------------------
 * hft-burst-mixed: heterogeneous completion times
 *
 * Mix of immediate, yield-once, and yield-many tasks simulating
 * different order processing latencies. All must complete.
 * ------------------------------------------------------------------- */

static yield_ctx g_mixed_ctx[24];

TEST(hft_burst_mixed_completion_times) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id tids[24];
    asx_budget budget;
    uint32_t i;
    uint32_t completed;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* 8 immediate, 8 yield-once (2 polls), 8 yield-many (5 polls) */
    for (i = 0; i < 8; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_immediate, NULL, &tids[i]), ASX_OK);
    }
    for (i = 8; i < 16; i++) {
        g_mixed_ctx[i].target_polls = 2;
        g_mixed_ctx[i].polls_done = 0;
        ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &g_mixed_ctx[i], &tids[i]), ASX_OK);
    }
    for (i = 16; i < 24; i++) {
        g_mixed_ctx[i].target_polls = 5;
        g_mixed_ctx[i].polls_done = 0;
        ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &g_mixed_ctx[i], &tids[i]), ASX_OK);
    }

    /* Generous budget */
    budget = asx_budget_from_polls(500);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* All 24 must complete */
    completed = 0;
    for (i = 0; i < 24; i++) {
        asx_task_state state;
        ASSERT_EQ(asx_task_get_state(tids[i], &state), ASX_OK);
        if (state == ASX_TASK_COMPLETED) completed++;
    }
    ASSERT_EQ(completed, 24u);

    /* Quiescent event at end */
    {
        uint32_t ev_count = asx_scheduler_event_count();
        asx_scheduler_event last_ev;
        ASSERT_TRUE(ev_count > 0);
        ASSERT_TRUE(asx_scheduler_event_get(ev_count - 1, &last_ev));
        ASSERT_EQ(last_ev.kind, ASX_SCHED_EVENT_QUIESCENT);
    }
}

/* -------------------------------------------------------------------
 * hft-burst-cancel-storm: mass cancellation during burst
 *
 * 16 tasks spawned, then region-wide cancel propagation. All
 * checkpoint-compliant tasks should resolve to CANCELLED outcome.
 * ------------------------------------------------------------------- */

TEST(hft_burst_cancel_storm_resolves_all) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id tids[16];
    asx_budget budget;
    uint32_t i;
    uint32_t completed;
    uint32_t cancelled_count;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Spawn 16 checkpoint-compliant tasks */
    for (i = 0; i < 16; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_comply, NULL, &tids[i]), ASX_OK);
    }

    /* Propagate cancel to entire region */
    IGNORE(asx_cancel_propagate(rid, ASX_CANCEL_USER));

    /* Run scheduler — all should complete via cancel path */
    budget = asx_budget_from_polls(500);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* All 16 must be completed with CANCELLED outcome */
    completed = 0;
    cancelled_count = 0;
    for (i = 0; i < 16; i++) {
        asx_task_state state;
        asx_outcome out;
        ASSERT_EQ(asx_task_get_state(tids[i], &state), ASX_OK);
        if (state == ASX_TASK_COMPLETED) {
            completed++;
            ASSERT_EQ(asx_task_get_outcome(tids[i], &out), ASX_OK);
            if (out.severity == ASX_OUTCOME_CANCELLED) { cancelled_count++; }
        }
    }
    ASSERT_EQ(completed, 16u);
    ASSERT_EQ(cancelled_count, 16u);
}

/* -------------------------------------------------------------------
 * hft-burst-replay: deterministic replay identity
 *
 * Run the same burst scenario twice. The scheduler event digest
 * must be identical — proving deterministic scheduling.
 * ------------------------------------------------------------------- */

static yield_ctx g_replay_ctx_a[8];
static yield_ctx g_replay_ctx_b[8];

static void run_replay_scenario(yield_ctx *ctxs, asx_task_id *tids) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_budget budget;
    uint32_t i;

    IGNORE(asx_region_open(&rid));
    for (i = 0; i < 8; i++) {
        ctxs[i].target_polls = (int)(i % 3) + 1;
        ctxs[i].polls_done = 0;
        IGNORE(asx_task_spawn(rid, poll_yield_n, &ctxs[i], &tids[i]));
    }
    budget = asx_budget_from_polls(200);
    IGNORE(asx_scheduler_run(rid, &budget));
}

static uint64_t capture_event_digest(void) {
    uint64_t hash = 0x517cc1b727220a95ULL;
    uint32_t i;
    uint32_t ev_count = asx_scheduler_event_count();

    for (i = 0; i < ev_count; i++) {
        asx_scheduler_event ev;
        if (asx_scheduler_event_get(i, &ev)) {
            uint32_t k = (uint32_t)ev.kind;
            uint32_t r = ev.round;
            uint32_t s = ev.sequence;
            /* Mix into hash — FNV-1a byte-by-byte */
            const uint8_t *p;
            uint32_t j;

            p = (const uint8_t *)&k;
            for (j = 0; j < sizeof(k); j++) {
                hash ^= (uint64_t)p[j];
                hash *= 0x00000100000001B3ULL;
            }
            p = (const uint8_t *)&r;
            for (j = 0; j < sizeof(r); j++) {
                hash ^= (uint64_t)p[j];
                hash *= 0x00000100000001B3ULL;
            }
            p = (const uint8_t *)&s;
            for (j = 0; j < sizeof(s); j++) {
                hash ^= (uint64_t)p[j];
                hash *= 0x00000100000001B3ULL;
            }
        }
    }
    return hash;
}

TEST(hft_burst_replay_deterministic) {
    asx_task_id tids_a[8];
    asx_task_id tids_b[8];
    uint64_t digest_a;
    uint64_t digest_b;
    uint32_t count_a;
    uint32_t count_b;

    /* Run 1 */
    reset_all();
    run_replay_scenario(g_replay_ctx_a, tids_a);
    digest_a = capture_event_digest();
    count_a = asx_scheduler_event_count();

    /* Run 2 */
    reset_all();
    run_replay_scenario(g_replay_ctx_b, tids_b);
    digest_b = capture_event_digest();
    count_b = asx_scheduler_event_count();

    /* Event counts must match */
    ASSERT_EQ(count_a, count_b);

    /* Digests must match — deterministic scheduling */
    ASSERT_EQ(digest_a, digest_b);
}

/* -------------------------------------------------------------------
 * hft-burst-drain: quiescence after full burst
 *
 * After all tasks in a burst complete, the region should be
 * drainable and quiescence check should pass.
 * ------------------------------------------------------------------- */

TEST(hft_burst_drain_quiescence) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id tid;
    asx_budget budget;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Spawn 32 immediate tasks */
    for (i = 0; i < 32; i++) { ASSERT_EQ(asx_task_spawn(rid, poll_immediate, NULL, &tid), ASX_OK); }

    /* Run to completion */
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Region should be closable */
    ASSERT_EQ(asx_region_close(rid), ASX_OK);

    /* Drain should succeed */
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
}

/* -------------------------------------------------------------------
 * hft-burst-error-mixed: some tasks fail under burst load
 *
 * Mix of succeeding and failing tasks. Verify that failures
 * produce ERR outcomes and don't corrupt succeeding tasks.
 * ------------------------------------------------------------------- */

TEST(hft_burst_errors_do_not_corrupt_peers) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id ok_tids[8];
    asx_task_id fail_tids[8];
    asx_budget budget;
    uint32_t i;
    uint32_t ok_completed;
    uint32_t fail_completed;
    uint32_t fail_non_ok_outcome;
    asx_containment_policy policy;
    asx_status run_st;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* 8 successful tasks + 8 failing tasks interleaved */
    for (i = 0; i < 8; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_immediate, NULL, &ok_tids[i]), ASX_OK);
        ASSERT_EQ(asx_task_spawn(rid, poll_fail, NULL, &fail_tids[i]), ASX_OK);
    }

    budget = asx_budget_from_polls(200);
    run_st = asx_scheduler_run(rid, &budget);
    policy = asx_containment_policy_active();
    if (policy == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ(run_st, ASX_OK);
    } else {
        ASSERT_TRUE(run_st != ASX_OK);
    }

    ok_completed = 0;
    fail_completed = 0;
    fail_non_ok_outcome = 0;
    for (i = 0; i < 8; i++) {
        asx_task_state state;
        asx_outcome out;

        ASSERT_EQ(asx_task_get_state(ok_tids[i], &state), ASX_OK);
        if (state == ASX_TASK_COMPLETED) {
            ok_completed++;
            ASSERT_EQ(asx_task_get_outcome(ok_tids[i], &out), ASX_OK);
        }

        ASSERT_EQ(asx_task_get_state(fail_tids[i], &state), ASX_OK);
        if (state == ASX_TASK_COMPLETED) {
            fail_completed++;
            ASSERT_EQ(asx_task_get_outcome(fail_tids[i], &out), ASX_OK);
            if (out.severity == ASX_OUTCOME_ERR || out.severity == ASX_OUTCOME_CANCELLED) {
                fail_non_ok_outcome++;
            }
        }
    }

    if (policy == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ(ok_completed, 8u);
        ASSERT_EQ(fail_completed, 8u);
        ASSERT_EQ(fail_non_ok_outcome, 8u);
    } else {
        /* Fail-fast / error-only: first fault aborts run, but no state corruption. */
        ASSERT_TRUE(fail_completed >= 1u);
        ASSERT_EQ(fail_completed, fail_non_ok_outcome);
    }
}

/* -------------------------------------------------------------------
 * hft-burst-event-monotonicity: event sequence is strictly ordered
 *
 * Under burst load, scheduler events must maintain strict
 * monotonic sequencing — a key HFT requirement.
 * ------------------------------------------------------------------- */

TEST(hft_burst_event_monotonicity) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id tid;
    asx_budget budget;
    uint32_t i;
    uint32_t ev_count;
    uint32_t prev_seq;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    for (i = 0; i < 32; i++) { ASSERT_EQ(asx_task_spawn(rid, poll_immediate, NULL, &tid), ASX_OK); }

    budget = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Verify strict monotonic sequencing */
    ev_count = asx_scheduler_event_count();
    ASSERT_TRUE(ev_count > 0);

    prev_seq = 0;
    for (i = 0; i < ev_count; i++) {
        asx_scheduler_event ev;
        ASSERT_TRUE(asx_scheduler_event_get(i, &ev));
        if (i > 0) { ASSERT_TRUE(ev.sequence > prev_seq); }
        prev_seq = ev.sequence;
    }
}

/* -------------------------------------------------------------------
 * hft-burst-resource-snapshot: capacity tracking under load
 * ------------------------------------------------------------------- */

TEST(hft_burst_resource_snapshot_consistent) {
    asx_region_id rid = ASX_INVALID_ID;
    asx_task_id tid;
    asx_resource_snapshot snap;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Spawn 16 tasks and check snapshot at each step */
    for (i = 0; i < 16; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_immediate, NULL, &tid), ASX_OK);
        ASSERT_EQ(asx_resource_snapshot_get(ASX_RESOURCE_TASK, &snap), ASX_OK);
        ASSERT_EQ(snap.used, i + 1);
        ASSERT_EQ(snap.remaining, (uint32_t)ASX_MAX_TASKS - (i + 1));
        ASSERT_EQ(snap.capacity, (uint32_t)ASX_MAX_TASKS);
    }
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    fprintf(stderr, "=== test_hft_microburst (VERT-HFT-MICROBURST) ===\n");

    /* Admission and overload */
    RUN_TEST(hft_burst_admission_fills_capacity);
    RUN_TEST(hft_burst_overload_rejected_cleanly);
    RUN_TEST(hft_burst_admission_gate_precheck);

    /* Fairness and ordering */
    RUN_TEST(hft_burst_fairness_no_starvation);
    RUN_TEST(hft_burst_fairness_round_robin_order);

    /* Budget and backpressure */
    RUN_TEST(hft_burst_tight_budget_partial_completion);

    /* Mixed load profiles */
    RUN_TEST(hft_burst_mixed_completion_times);

    /* Cancel storm */
    RUN_TEST(hft_burst_cancel_storm_resolves_all);

    /* Deterministic replay */
    RUN_TEST(hft_burst_replay_deterministic);

    /* Quiescence and drain */
    RUN_TEST(hft_burst_drain_quiescence);

    /* Error isolation */
    RUN_TEST(hft_burst_errors_do_not_corrupt_peers);

    /* Event invariants */
    RUN_TEST(hft_burst_event_monotonicity);

    /* Resource tracking */
    RUN_TEST(hft_burst_resource_snapshot_consistent);

    TEST_REPORT();
    return test_failures;
}
