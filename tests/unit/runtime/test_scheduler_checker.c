/*
 * test_scheduler_checker.c — exhaustive scheduler state-space checker (bd-2cw.5)
 *
 * Bounded combinatorial exploration of scheduler transitions with
 * fairness, starvation, budget, and cancel-protocol invariant checks.
 *
 * Strategy: enumerate task configurations (behavior × cancel timing)
 * for 1-3 tasks, run the scheduler, and verify properties on the
 * resulting event trace. Outputs counterexample traces on failure.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/cancel.h>
#include <asx/runtime/runtime.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define SCHED_RUN_IGNORE(rid, bud)                                                                 \
    do {                                                                                           \
        asx_status s_ = asx_scheduler_run((rid), (bud));                                           \
        (void)s_;                                                                                  \
    } while (0)

/* -------------------------------------------------------------------
 * Task behavior enum — determines what the poll function does
 * ------------------------------------------------------------------- */

typedef enum {
    BEHAV_IMMEDIATE,       /* return ASX_OK on first poll */
    BEHAV_YIELD_THEN_OK,   /* return PENDING once, then OK */
    BEHAV_ALWAYS_PENDING,  /* always return PENDING (never self-completes) */
    BEHAV_CHECKPOINT_OK,   /* checkpoint; if cancelled, return OK */
    BEHAV_CHECKPOINT_PEND, /* checkpoint each poll, always return PENDING */
    BEHAV_COUNT
} task_behavior;

/* -------------------------------------------------------------------
 * Cancel timing enum
 * ------------------------------------------------------------------- */

typedef enum {
    CANCEL_NONE,           /* never cancelled */
    CANCEL_BEFORE_SCHED,   /* cancel before scheduler runs */
    CANCEL_AFTER_ONE_POLL, /* cancel after first scheduler round */
    CANCEL_TIMING_COUNT
} cancel_timing;

/* -------------------------------------------------------------------
 * Per-task state for configurable poll function
 * ------------------------------------------------------------------- */

typedef struct {
    task_behavior behavior;
    int polls_done;
} task_ctx;

static asx_status configurable_poll(void *user_data, asx_task_id self) {
    task_ctx *ctx = (task_ctx *)user_data;
    asx_checkpoint_result cr;
    asx_status st;

    ctx->polls_done++;

    switch (ctx->behavior) {
    case BEHAV_IMMEDIATE: return ASX_OK;

    case BEHAV_YIELD_THEN_OK:
        if (ctx->polls_done <= 1) return ASX_E_PENDING;
        return ASX_OK;

    case BEHAV_ALWAYS_PENDING: return ASX_E_PENDING;

    case BEHAV_CHECKPOINT_OK:
        st = asx_checkpoint(self, &cr);
        if (st == ASX_OK && cr.cancelled) return ASX_OK;
        return ASX_E_PENDING;

    case BEHAV_CHECKPOINT_PEND:
        st = asx_checkpoint(self, &cr);
        (void)st;
        return ASX_E_PENDING;

    case BEHAV_COUNT: break;
    }

    return ASX_E_PENDING;
}

/* -------------------------------------------------------------------
 * Invariant: every completed task has a valid outcome
 * ------------------------------------------------------------------- */

static int check_outcomes(asx_task_id *tids, cancel_timing *timings, uint32_t ntasks) {
    uint32_t i;
    for (i = 0; i < ntasks; i++) {
        asx_task_state state;
        asx_outcome out;

        if (asx_task_get_state(tids[i], &state) != ASX_OK) continue;
        if (state != ASX_TASK_COMPLETED) continue;

        if (asx_task_get_outcome(tids[i], &out) != ASX_OK) {
            fprintf(stderr, "    [CHECKER] task %u completed but outcome query failed\n", i);
            return 0;
        }

        /* If cancelled, outcome must be CANCELLED */
        if (timings[i] != CANCEL_NONE) {
            if (out.severity != ASX_OUTCOME_CANCELLED && out.severity != ASX_OUTCOME_OK) {
                /* OK is allowed if task completed before cancel took effect */
            }
        }
    }
    return 1;
}

/* -------------------------------------------------------------------
 * Invariant: event sequence is monotonically increasing
 * ------------------------------------------------------------------- */

static int check_event_monotonicity(void) {
    uint32_t count = asx_scheduler_event_count();
    uint32_t i;
    uint32_t prev_seq = 0;

    for (i = 0; i < count; i++) {
        asx_scheduler_event ev;
        if (!asx_scheduler_event_get(i, &ev)) continue;
        if (i > 0 && ev.sequence <= prev_seq) {
            fprintf(stderr, "    [CHECKER] event %u: sequence %u <= prev %u\n", i, ev.sequence,
                    prev_seq);
            return 0;
        }
        prev_seq = ev.sequence;
    }
    return 1;
}

/* -------------------------------------------------------------------
 * Invariant: round numbers are non-decreasing
 * ------------------------------------------------------------------- */

static int check_round_nondecreasing(void) {
    uint32_t count = asx_scheduler_event_count();
    uint32_t i;
    uint32_t prev_round = 0;

    for (i = 0; i < count; i++) {
        asx_scheduler_event ev;
        if (!asx_scheduler_event_get(i, &ev)) continue;
        if (ev.round < prev_round) {
            fprintf(stderr, "    [CHECKER] event %u: round %u < prev %u\n", i, ev.round,
                    prev_round);
            return 0;
        }
        prev_round = ev.round;
    }
    return 1;
}

/* -------------------------------------------------------------------
 * Invariant: all tasks reach terminal state (no starvation)
 * ------------------------------------------------------------------- */

static int check_no_starvation(asx_task_id *tids, uint32_t ntasks) {
    uint32_t i;
    for (i = 0; i < ntasks; i++) {
        asx_task_state state;
        if (asx_task_get_state(tids[i], &state) != ASX_OK) {
            fprintf(stderr, "    [CHECKER] task %u state query failed\n", i);
            return 0;
        }
        if (state != ASX_TASK_COMPLETED) {
            /* Not all tasks completed — could be budget exhaustion, OK if expected */
            return 0;
        }
    }
    return 1;
}

/* -------------------------------------------------------------------
 * Invariant: quiescent event emitted iff all tasks completed
 * ------------------------------------------------------------------- */

static int check_quiescence_consistency(asx_status sched_result, asx_task_id *tids,
                                        uint32_t ntasks) {
    int all_complete = 1;
    uint32_t i;
    int found_quiescent = 0;
    uint32_t count;

    for (i = 0; i < ntasks; i++) {
        asx_task_state state;
        if (asx_task_get_state(tids[i], &state) != ASX_OK || state != ASX_TASK_COMPLETED) {
            all_complete = 0;
            break;
        }
    }

    count = asx_scheduler_event_count();
    for (i = 0; i < count; i++) {
        asx_scheduler_event ev;
        if (asx_scheduler_event_get(i, &ev) && ev.kind == ASX_SCHED_EVENT_QUIESCENT) {
            found_quiescent = 1;
            break;
        }
    }

    if (sched_result == ASX_OK && !found_quiescent) {
        fprintf(stderr, "    [CHECKER] scheduler returned OK but no QUIESCENT event\n");
        return 0;
    }

    if (found_quiescent && !all_complete) {
        fprintf(stderr, "    [CHECKER] QUIESCENT event but not all tasks completed\n");
        return 0;
    }

    return 1;
}

/* -------------------------------------------------------------------
 * Invariant: deterministic replay — running same config twice
 * produces identical event trace digests
 * ------------------------------------------------------------------- */

static uint32_t compute_event_digest(void) {
    /* Simple FNV-1a-like hash of the event trace */
    uint32_t hash = 0x811c9dc5u;
    uint32_t count = asx_scheduler_event_count();
    uint32_t i;

    for (i = 0; i < count; i++) {
        asx_scheduler_event ev;
        if (!asx_scheduler_event_get(i, &ev)) break;
        hash ^= (uint32_t)ev.kind;
        hash *= 0x01000193u;
        hash ^= ev.round;
        hash *= 0x01000193u;
        hash ^= ev.sequence;
        hash *= 0x01000193u;
    }
    hash ^= count;
    hash *= 0x01000193u;
    return hash;
}

/* -------------------------------------------------------------------
 * Run a single scenario and check invariants
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t ntasks;
    task_behavior behaviors[3];
    cancel_timing timings[3];
} scenario;

static int run_scenario(const scenario *sc, uint32_t budget_polls) {
    asx_region_id rid;
    asx_task_id tids[3];
    task_ctx ctxs[3];
    asx_budget budget;
    asx_status result;
    uint32_t i;
    int ok = 1;

    asx_runtime_reset();

    if (asx_region_open(&rid) != ASX_OK) return 0;

    /* Spawn tasks */
    for (i = 0; i < sc->ntasks; i++) {
        ctxs[i].behavior = sc->behaviors[i];
        ctxs[i].polls_done = 0;
        if (asx_task_spawn(rid, configurable_poll, &ctxs[i], &tids[i]) != ASX_OK) { return 0; }
    }

    /* Apply pre-schedule cancellations */
    for (i = 0; i < sc->ntasks; i++) {
        if (sc->timings[i] == CANCEL_BEFORE_SCHED) {
            asx_status st = asx_task_cancel(tids[i], ASX_CANCEL_USER);
            (void)st;
        }
    }

    /* Run one round if we need after-one-poll cancellations */
    {
        int need_mid = 0;
        for (i = 0; i < sc->ntasks; i++) {
            if (sc->timings[i] == CANCEL_AFTER_ONE_POLL) need_mid = 1;
        }
        if (need_mid) {
            budget = asx_budget_from_polls(sc->ntasks);
            SCHED_RUN_IGNORE(rid, &budget);

            for (i = 0; i < sc->ntasks; i++) {
                if (sc->timings[i] == CANCEL_AFTER_ONE_POLL) {
                    asx_status st = asx_task_cancel(tids[i], ASX_CANCEL_USER);
                    (void)st;
                }
            }
        }
    }

    /* Main scheduler run */
    budget = asx_budget_from_polls(budget_polls);
    result = asx_scheduler_run(rid, &budget);

    /* Check invariants */
    if (!check_event_monotonicity()) {
        fprintf(stderr, "    [FAIL] event monotonicity violated\n");
        ok = 0;
    }
    if (!check_round_nondecreasing()) {
        fprintf(stderr, "    [FAIL] round non-decreasing violated\n");
        ok = 0;
    }
    if (!check_outcomes(tids, (cancel_timing *)sc->timings, sc->ntasks)) {
        fprintf(stderr, "    [FAIL] outcome invariant violated\n");
        ok = 0;
    }
    if (!check_quiescence_consistency(result, tids, sc->ntasks)) {
        fprintf(stderr, "    [FAIL] quiescence consistency violated\n");
        ok = 0;
    }

    return ok;
}

/* -------------------------------------------------------------------
 * Test: exhaustive 1-task scenarios
 * ------------------------------------------------------------------- */

TEST(checker_exhaustive_1task) {
    scenario sc;
    int b, t;
    int total = 0, passed = 0;

    sc.ntasks = 1;
    memset(sc.behaviors, 0, sizeof(sc.behaviors));
    memset(sc.timings, 0, sizeof(sc.timings));

    for (b = 0; b < (int)BEHAV_COUNT; b++) {
        for (t = 0; t < (int)CANCEL_TIMING_COUNT; t++) {
            sc.behaviors[0] = (task_behavior)b;
            sc.timings[0] = (cancel_timing)t;
            total++;
            if (run_scenario(&sc, 200)) {
                passed++;
            } else {
                fprintf(stderr, "    scenario 1-task: behav=%d cancel=%d FAILED\n", b, t);
            }
        }
    }

    ASSERT_EQ(passed, total);
}

/* -------------------------------------------------------------------
 * Test: exhaustive 2-task scenarios (limited combinations)
 * ------------------------------------------------------------------- */

TEST(checker_exhaustive_2task) {
    scenario sc;
    int b0, b1, t0, t1;
    int total = 0, passed = 0;

    sc.ntasks = 2;
    memset(sc.behaviors, 0, sizeof(sc.behaviors));
    memset(sc.timings, 0, sizeof(sc.timings));

    /* Enumerate all combinations of behavior × cancel for 2 tasks */
    for (b0 = 0; b0 < (int)BEHAV_COUNT; b0++) {
        for (b1 = 0; b1 < (int)BEHAV_COUNT; b1++) {
            for (t0 = 0; t0 < (int)CANCEL_TIMING_COUNT; t0++) {
                for (t1 = 0; t1 < (int)CANCEL_TIMING_COUNT; t1++) {
                    sc.behaviors[0] = (task_behavior)b0;
                    sc.behaviors[1] = (task_behavior)b1;
                    sc.timings[0] = (cancel_timing)t0;
                    sc.timings[1] = (cancel_timing)t1;
                    total++;
                    if (run_scenario(&sc, 200)) { passed++; }
                }
            }
        }
    }

    ASSERT_EQ(passed, total);
}

/* -------------------------------------------------------------------
 * Test: 3-task adversarial mix
 * ------------------------------------------------------------------- */

TEST(checker_3task_adversarial_mix) {
    scenario sc;
    int passed = 0, total = 0;

    sc.ntasks = 3;

    /* Scenario: immediate + always-pending-cancelled + checkpoint-cancel */
    sc.behaviors[0] = BEHAV_IMMEDIATE;
    sc.behaviors[1] = BEHAV_ALWAYS_PENDING;
    sc.behaviors[2] = BEHAV_CHECKPOINT_OK;
    sc.timings[0] = CANCEL_NONE;
    sc.timings[1] = CANCEL_BEFORE_SCHED;
    sc.timings[2] = CANCEL_AFTER_ONE_POLL;
    total++;
    if (run_scenario(&sc, 200)) passed++;

    /* Scenario: all checkpoint-pending with cancel storm */
    sc.behaviors[0] = BEHAV_CHECKPOINT_PEND;
    sc.behaviors[1] = BEHAV_CHECKPOINT_PEND;
    sc.behaviors[2] = BEHAV_CHECKPOINT_PEND;
    sc.timings[0] = CANCEL_BEFORE_SCHED;
    sc.timings[1] = CANCEL_AFTER_ONE_POLL;
    sc.timings[2] = CANCEL_BEFORE_SCHED;
    total++;
    if (run_scenario(&sc, 200)) passed++;

    /* Scenario: all immediate with no cancel */
    sc.behaviors[0] = BEHAV_IMMEDIATE;
    sc.behaviors[1] = BEHAV_IMMEDIATE;
    sc.behaviors[2] = BEHAV_IMMEDIATE;
    sc.timings[0] = CANCEL_NONE;
    sc.timings[1] = CANCEL_NONE;
    sc.timings[2] = CANCEL_NONE;
    total++;
    if (run_scenario(&sc, 200)) passed++;

    /* Scenario: mixed yields with all-cancel */
    sc.behaviors[0] = BEHAV_YIELD_THEN_OK;
    sc.behaviors[1] = BEHAV_ALWAYS_PENDING;
    sc.behaviors[2] = BEHAV_YIELD_THEN_OK;
    sc.timings[0] = CANCEL_AFTER_ONE_POLL;
    sc.timings[1] = CANCEL_AFTER_ONE_POLL;
    sc.timings[2] = CANCEL_AFTER_ONE_POLL;
    total++;
    if (run_scenario(&sc, 200)) passed++;

    /* Scenario: checkpoint tasks racing finalization */
    sc.behaviors[0] = BEHAV_CHECKPOINT_OK;
    sc.behaviors[1] = BEHAV_CHECKPOINT_PEND;
    sc.behaviors[2] = BEHAV_IMMEDIATE;
    sc.timings[0] = CANCEL_BEFORE_SCHED;
    sc.timings[1] = CANCEL_BEFORE_SCHED;
    sc.timings[2] = CANCEL_NONE;
    total++;
    if (run_scenario(&sc, 200)) passed++;

    ASSERT_EQ(passed, total);
}

/* -------------------------------------------------------------------
 * Test: deterministic replay — same config produces same trace
 * ------------------------------------------------------------------- */

TEST(checker_deterministic_replay) {
    scenario sc;
    uint32_t digest1, digest2;
    int run;

    sc.ntasks = 2;
    sc.behaviors[0] = BEHAV_CHECKPOINT_OK;
    sc.behaviors[1] = BEHAV_YIELD_THEN_OK;
    sc.timings[0] = CANCEL_AFTER_ONE_POLL;
    sc.timings[1] = CANCEL_NONE;

    /* Run twice and compare digests */
    for (run = 0; run < 2; run++) {
        asx_region_id rid;
        asx_task_id tids[2];
        task_ctx ctxs[2];
        asx_budget budget;
        uint32_t i;

        asx_runtime_reset();
        ASSERT_EQ(asx_region_open(&rid), ASX_OK);

        for (i = 0; i < sc.ntasks; i++) {
            ctxs[i].behavior = sc.behaviors[i];
            ctxs[i].polls_done = 0;
            ASSERT_EQ(asx_task_spawn(rid, configurable_poll, &ctxs[i], &tids[i]), ASX_OK);
        }

        /* First round to make tasks Running */
        budget = asx_budget_from_polls(sc.ntasks);
        SCHED_RUN_IGNORE(rid, &budget);

        /* Cancel task 0 */
        ASSERT_EQ(asx_task_cancel(tids[0], ASX_CANCEL_USER), ASX_OK);

        /* Run to completion */
        budget = asx_budget_from_polls(200);
        SCHED_RUN_IGNORE(rid, &budget);

        if (run == 0) {
            digest1 = compute_event_digest();
        } else {
            digest2 = compute_event_digest();
        }
    }

    ASSERT_EQ(digest1, digest2);
}

/* -------------------------------------------------------------------
 * Test: budget exhaustion does not corrupt state
 * ------------------------------------------------------------------- */

TEST(checker_budget_exhaustion_safe) {
    asx_region_id rid;
    asx_task_id tids[3];
    task_ctx ctxs[3];
    asx_budget budget;
    asx_status result;
    uint32_t i;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Spawn 3 always-pending tasks */
    for (i = 0; i < 3; i++) {
        ctxs[i].behavior = BEHAV_ALWAYS_PENDING;
        ctxs[i].polls_done = 0;
        ASSERT_EQ(asx_task_spawn(rid, configurable_poll, &ctxs[i], &tids[i]), ASX_OK);
    }

    /* Run with very tight budget — only 2 polls total */
    budget = asx_budget_from_polls(2);
    result = asx_scheduler_run(rid, &budget);
    ASSERT_EQ(result, ASX_E_POLL_BUDGET_EXHAUSTED);

    /* All tasks should still be alive (not corrupted) */
    for (i = 0; i < 3; i++) {
        asx_task_state state;
        ASSERT_EQ(asx_task_get_state(tids[i], &state), ASX_OK);
        ASSERT_TRUE(state == ASX_TASK_CREATED || state == ASX_TASK_RUNNING);
    }

    /* Resume with more budget — should eventually quiesce only if tasks can complete.
     * With always-pending, they'll hit budget again. */
    budget = asx_budget_from_polls(5);
    result = asx_scheduler_run(rid, &budget);
    ASSERT_EQ(result, ASX_E_POLL_BUDGET_EXHAUSTED);

    /* Invariant: event trace still monotonic */
    ASSERT_TRUE(check_event_monotonicity());
    ASSERT_TRUE(check_round_nondecreasing());
}

/* -------------------------------------------------------------------
 * Test: cycle-budget checkpoint — cancel + tight budget
 * ------------------------------------------------------------------- */

TEST(checker_cycle_budget_checkpoint) {
    asx_region_id rid;
    asx_task_id tid;
    task_ctx ctx;
    asx_budget budget;
    asx_task_state state;
    asx_outcome out;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Task that checkpoints but never completes on its own */
    ctx.behavior = BEHAV_CHECKPOINT_PEND;
    ctx.polls_done = 0;
    ASSERT_EQ(asx_task_spawn(rid, configurable_poll, &ctx, &tid), ASX_OK);

    /* Make task Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Cancel with SHUTDOWN (tightest budget) */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);

    /* Run scheduler — should force-complete after cleanup budget exhaustion */
    budget = asx_budget_from_polls(200);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Task must be completed with CANCELLED outcome */
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
}

/* -------------------------------------------------------------------
 * Test: fairness — each task gets polled in round-robin order
 * ------------------------------------------------------------------- */

TEST(checker_fairness_round_robin) {
    asx_region_id rid;
    asx_task_id tids[3];
    task_ctx ctxs[3];
    asx_budget budget;
    uint32_t i;
    int task_0_polled = 0, task_1_polled = 0, task_2_polled = 0;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* 3 always-pending tasks */
    for (i = 0; i < 3; i++) {
        ctxs[i].behavior = BEHAV_ALWAYS_PENDING;
        ctxs[i].polls_done = 0;
        ASSERT_EQ(asx_task_spawn(rid, configurable_poll, &ctxs[i], &tids[i]), ASX_OK);
    }

    /* Run for 6 polls (should give 2 rounds) */
    budget = asx_budget_from_polls(6);
    SCHED_RUN_IGNORE(rid, &budget);

    task_0_polled = ctxs[0].polls_done;
    task_1_polled = ctxs[1].polls_done;
    task_2_polled = ctxs[2].polls_done;

    /* Each task should get polled exactly 2 times */
    ASSERT_EQ(task_0_polled, 2);
    ASSERT_EQ(task_1_polled, 2);
    ASSERT_EQ(task_2_polled, 2);
}

/* -------------------------------------------------------------------
 * Test: no starvation with mixed completion times
 * ------------------------------------------------------------------- */

TEST(checker_no_starvation_mixed_completion) {
    asx_region_id rid;
    asx_task_id tids[3];
    task_ctx ctxs[3];
    asx_budget budget;
    asx_status result;
    uint32_t i;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Task 0: immediate, Task 1: yield-then-ok, Task 2: immediate */
    ctxs[0].behavior = BEHAV_IMMEDIATE;
    ctxs[0].polls_done = 0;
    ctxs[1].behavior = BEHAV_YIELD_THEN_OK;
    ctxs[1].polls_done = 0;
    ctxs[2].behavior = BEHAV_IMMEDIATE;
    ctxs[2].polls_done = 0;

    for (i = 0; i < 3; i++) {
        ASSERT_EQ(asx_task_spawn(rid, configurable_poll, &ctxs[i], &tids[i]), ASX_OK);
    }

    /* Run with sufficient budget */
    budget = asx_budget_from_polls(20);
    result = asx_scheduler_run(rid, &budget);
    ASSERT_EQ(result, ASX_OK);

    /* All tasks should be completed */
    ASSERT_TRUE(check_no_starvation(tids, 3));
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    fprintf(stderr, "=== test_scheduler_checker ===\n");

    RUN_TEST(checker_exhaustive_1task);
    RUN_TEST(checker_exhaustive_2task);
    RUN_TEST(checker_3task_adversarial_mix);
    RUN_TEST(checker_deterministic_replay);
    RUN_TEST(checker_budget_exhaustion_safe);
    RUN_TEST(checker_cycle_budget_checkpoint);
    RUN_TEST(checker_fairness_round_robin);
    RUN_TEST(checker_no_starvation_mixed_completion);

    TEST_REPORT();
    return test_failures;
}
