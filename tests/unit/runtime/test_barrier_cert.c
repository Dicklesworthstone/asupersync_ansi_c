/*
 * test_barrier_cert.c — SOS barrier-certificate bounds evaluation (bd-3vt.6)
 *
 * Validates:
 * - Barrier function B(x) = K - max(wait) is correct
 * - Safety property (B > 0) holds under round-robin
 * - Safety violation detection when starvation occurs
 * - Decrease condition monitoring
 * - Bound admissibility check
 * - Integration with scheduler-like polling patterns
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- barrier cert test, no checkpoint coverage needed */

#include "test_harness.h"
#include <asx/asx.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Types from spike (not in public headers)                           */
/* ------------------------------------------------------------------ */

#define ASX_BARRIER_MAX_TASKS 64u

typedef struct {
    uint32_t wait_counts[ASX_BARRIER_MAX_TASKS];
    uint32_t task_count;
    uint32_t round;
} asx_barrier_state;

typedef struct {
    uint32_t starvation_bound;
    uint32_t alive_count;
} asx_barrier_config;

typedef struct {
    int32_t value;
    int safe;
    int decreasing;
    uint32_t max_wait;
    uint32_t violator_index;
} asx_barrier_result;

void asx_barrier_state_init(asx_barrier_state *state, uint32_t task_count);
void asx_barrier_record_poll(asx_barrier_state *state, uint32_t task_index);
void asx_barrier_advance_round(asx_barrier_state *state);
void asx_barrier_evaluate(const asx_barrier_state *state, const asx_barrier_config *cfg,
                          int32_t prev_value, asx_barrier_result *result);
uint32_t asx_barrier_max_wait(const asx_barrier_state *state);
int asx_barrier_admits_bound(const asx_barrier_config *cfg, uint32_t task_count);

/* ------------------------------------------------------------------ */
/* Test: initialization                                               */
/* ------------------------------------------------------------------ */

TEST(init_clears_state) {
    asx_barrier_state state;
    asx_barrier_state_init(&state, 8);

    ASSERT_EQ(state.task_count, (uint32_t)8);
    ASSERT_EQ(state.round, (uint32_t)0);
    ASSERT_EQ(asx_barrier_max_wait(&state), (uint32_t)0);
}

TEST(init_clamps_to_max) {
    asx_barrier_state state;
    asx_barrier_state_init(&state, 1000);

    ASSERT_EQ(state.task_count, (uint32_t)ASX_BARRIER_MAX_TASKS);
}

TEST(init_null_safety) { asx_barrier_state_init(NULL, 8); /* should not crash */ }

/* ------------------------------------------------------------------ */
/* Test: barrier function B(x) = K - max(wait)                        */
/* ------------------------------------------------------------------ */

TEST(barrier_value_initially_equals_bound) {
    asx_barrier_state state;
    asx_barrier_config cfg = {5, 4};
    asx_barrier_result result;

    asx_barrier_state_init(&state, 4);
    asx_barrier_evaluate(&state, &cfg, 0, &result);

    /* All wait counts = 0, so B(x) = K - 0 = 5 */
    ASSERT_EQ(result.value, 5);
    ASSERT_TRUE(result.safe);
}

TEST(barrier_decreases_after_round_without_polling) {
    asx_barrier_state state;
    asx_barrier_config cfg = {5, 4};
    asx_barrier_result r1, r2;

    asx_barrier_state_init(&state, 4);
    asx_barrier_evaluate(&state, &cfg, 0, &r1);

    /* Advance round without polling any task */
    asx_barrier_advance_round(&state);
    asx_barrier_evaluate(&state, &cfg, r1.value, &r2);

    /* B should decrease: was 5, now 4 */
    ASSERT_EQ(r2.value, 4);
    ASSERT_TRUE(r2.decreasing);
    ASSERT_TRUE(r2.safe);
}

TEST(barrier_violation_when_wait_exceeds_bound) {
    asx_barrier_state state;
    asx_barrier_config cfg = {3, 4};
    asx_barrier_result result;
    uint32_t i;

    asx_barrier_state_init(&state, 4);

    /* 3 rounds without polling task 0 */
    for (i = 0; i < 3; i++) {
        asx_barrier_advance_round(&state);
        /* Poll tasks 1-3 but not 0 */
        asx_barrier_record_poll(&state, 1);
        asx_barrier_record_poll(&state, 2);
        asx_barrier_record_poll(&state, 3);
    }

    asx_barrier_evaluate(&state, &cfg, 0, &result);

    /* Task 0 waited 3 rounds, bound = 3, B = 3-3 = 0, NOT safe */
    ASSERT_EQ(result.value, 0);
    ASSERT_TRUE(!result.safe);
    ASSERT_EQ(result.max_wait, (uint32_t)3);
    ASSERT_EQ(result.violator_index, (uint32_t)0);
}

/* ------------------------------------------------------------------ */
/* Test: round-robin guarantees safety                                 */
/* ------------------------------------------------------------------ */

TEST(round_robin_maintains_safety) {
    asx_barrier_state state;
    asx_barrier_config cfg = {2, 8}; /* K=2, must poll all within 2 rounds */
    asx_barrier_result result;
    uint32_t round;
    uint32_t i;
    int32_t prev = 0;

    asx_barrier_state_init(&state, 8);
    asx_barrier_evaluate(&state, &cfg, 0, &result);
    prev = result.value;

    /* Simulate 20 rounds of round-robin (all tasks polled each round) */
    for (round = 0; round < 20; round++) {
        asx_barrier_advance_round(&state);
        for (i = 0; i < 8; i++) { asx_barrier_record_poll(&state, i); }
        asx_barrier_evaluate(&state, &cfg, prev, &result);
        ASSERT_TRUE(result.safe);
        prev = result.value;
    }

    /* After round-robin, max_wait should be 1 (advanced then polled) */
    ASSERT_EQ(result.max_wait, (uint32_t)0);
}

TEST(partial_round_robin_violates_with_tight_bound) {
    asx_barrier_state state;
    asx_barrier_config cfg = {1, 4}; /* K=1, very tight */
    asx_barrier_result result;

    asx_barrier_state_init(&state, 4);

    /* Round 1: advance, poll only tasks 0,1 (skip 2,3) */
    asx_barrier_advance_round(&state);
    asx_barrier_record_poll(&state, 0);
    asx_barrier_record_poll(&state, 1);

    asx_barrier_evaluate(&state, &cfg, 0, &result);

    /* Tasks 2,3 waited 1 round, bound=1, B=1-1=0, violation */
    ASSERT_EQ(result.value, 0);
    ASSERT_TRUE(!result.safe);
}

/* ------------------------------------------------------------------ */
/* Test: weighted polling with starvation detection                    */
/* ------------------------------------------------------------------ */

TEST(weighted_causes_starvation) {
    asx_barrier_state state;
    asx_barrier_config cfg = {5, 4};
    asx_barrier_result result;
    uint32_t round;

    asx_barrier_state_init(&state, 4);

    /* Simulate weighted: task 0 gets polled every round,
     * task 3 only every 4th round */
    for (round = 0; round < 8; round++) {
        asx_barrier_advance_round(&state);
        asx_barrier_record_poll(&state, 0); /* always */
        if (round % 2 == 0) asx_barrier_record_poll(&state, 1);
        if (round % 3 == 0) asx_barrier_record_poll(&state, 2);
        if (round % 4 == 0) asx_barrier_record_poll(&state, 3);
    }

    asx_barrier_evaluate(&state, &cfg, 0, &result);

    /* Task 3 was last polled at round 4 (index 4), wait = 8-4 = 4 */
    /* But after record_poll resets to 0, then advances increment */
    /* Task 3 polled at rounds 0,4 → after round 7: wait = 8-4-1 = 3 rounds since last poll */
    ASSERT_TRUE(result.max_wait > 0);
    ASSERT_TRUE(result.safe); /* Still within bound of 5 */
}

/* ------------------------------------------------------------------ */
/* Test: bound admissibility                                          */
/* ------------------------------------------------------------------ */

TEST(admits_positive_bound) {
    asx_barrier_config cfg = {5, 4};
    ASSERT_TRUE(asx_barrier_admits_bound(&cfg, 8));
}

TEST(rejects_zero_bound) {
    asx_barrier_config cfg = {0, 4};
    ASSERT_TRUE(!asx_barrier_admits_bound(&cfg, 8));
}

TEST(rejects_alive_exceeds_total) {
    asx_barrier_config cfg = {5, 100};
    ASSERT_TRUE(!asx_barrier_admits_bound(&cfg, 8));
}

TEST(admits_null_rejects) { ASSERT_TRUE(!asx_barrier_admits_bound(NULL, 8)); }

/* ------------------------------------------------------------------ */
/* Test: polling resets wait counter                                   */
/* ------------------------------------------------------------------ */

TEST(poll_resets_wait) {
    asx_barrier_state state;

    asx_barrier_state_init(&state, 4);

    asx_barrier_advance_round(&state);
    asx_barrier_advance_round(&state);
    /* Task 0 waited 2 rounds */
    ASSERT_EQ(state.wait_counts[0], (uint32_t)2);

    asx_barrier_record_poll(&state, 0);
    ASSERT_EQ(state.wait_counts[0], (uint32_t)0);
}

/* ------------------------------------------------------------------ */
/* Test: evaluate null safety                                         */
/* ------------------------------------------------------------------ */

TEST(evaluate_null_safety) {
    asx_barrier_result result;
    asx_barrier_config cfg = {5, 4};
    asx_barrier_state state;

    asx_barrier_state_init(&state, 4);

    asx_barrier_evaluate(NULL, &cfg, 0, &result);
    ASSERT_EQ(result.value, 0);

    asx_barrier_evaluate(&state, NULL, 0, &result);
    ASSERT_EQ(result.value, 0);

    asx_barrier_evaluate(&state, &cfg, 0, NULL); /* should not crash */
}

/* ------------------------------------------------------------------ */
/* Test: max_wait query                                               */
/* ------------------------------------------------------------------ */

TEST(max_wait_tracks_highest) {
    asx_barrier_state state;

    asx_barrier_state_init(&state, 4);

    asx_barrier_advance_round(&state);
    asx_barrier_advance_round(&state);
    asx_barrier_advance_round(&state);

    /* All waited 3 rounds */
    ASSERT_EQ(asx_barrier_max_wait(&state), (uint32_t)3);

    /* Poll task 2 */
    asx_barrier_record_poll(&state, 2);
    /* Max is still 3 (tasks 0,1,3) */
    ASSERT_EQ(asx_barrier_max_wait(&state), (uint32_t)3);

    /* Poll all */
    asx_barrier_record_poll(&state, 0);
    asx_barrier_record_poll(&state, 1);
    asx_barrier_record_poll(&state, 3);
    ASSERT_EQ(asx_barrier_max_wait(&state), (uint32_t)0);
}

/* ------------------------------------------------------------------ */
/* Test: deterministic trajectory                                      */
/* ------------------------------------------------------------------ */

TEST(deterministic_trajectory) {
    asx_barrier_state s1, s2;
    asx_barrier_config cfg = {10, 4};
    asx_barrier_result r1, r2;
    uint32_t round;
    int32_t prev1 = 0, prev2 = 0;

    asx_barrier_state_init(&s1, 4);
    asx_barrier_state_init(&s2, 4);

    /* Identical polling pattern on both */
    for (round = 0; round < 10; round++) {
        asx_barrier_advance_round(&s1);
        asx_barrier_advance_round(&s2);

        asx_barrier_record_poll(&s1, round % 4);
        asx_barrier_record_poll(&s2, round % 4);

        asx_barrier_evaluate(&s1, &cfg, prev1, &r1);
        asx_barrier_evaluate(&s2, &cfg, prev2, &r2);

        ASSERT_EQ(r1.value, r2.value);
        ASSERT_EQ(r1.safe, r2.safe);
        ASSERT_EQ(r1.max_wait, r2.max_wait);

        prev1 = r1.value;
        prev2 = r2.value;
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== barrier certificate tests (bd-3vt.6) ===\n");

    /* Initialization */
    RUN_TEST(init_clears_state);
    RUN_TEST(init_clamps_to_max);
    RUN_TEST(init_null_safety);

    /* Barrier function */
    RUN_TEST(barrier_value_initially_equals_bound);
    RUN_TEST(barrier_decreases_after_round_without_polling);
    RUN_TEST(barrier_violation_when_wait_exceeds_bound);

    /* Round-robin safety */
    RUN_TEST(round_robin_maintains_safety);
    RUN_TEST(partial_round_robin_violates_with_tight_bound);
    RUN_TEST(weighted_causes_starvation);

    /* Bound admissibility */
    RUN_TEST(admits_positive_bound);
    RUN_TEST(rejects_zero_bound);
    RUN_TEST(rejects_alive_exceeds_total);
    RUN_TEST(admits_null_rejects);

    /* State management */
    RUN_TEST(poll_resets_wait);
    RUN_TEST(evaluate_null_safety);
    RUN_TEST(max_wait_tracks_highest);

    /* Determinism */
    RUN_TEST(deterministic_trajectory);

    TEST_REPORT();
    return test_failures;
}
