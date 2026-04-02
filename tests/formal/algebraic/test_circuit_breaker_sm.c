/*
 * test_circuit_breaker_sm.c — algebraic state machine properties for circuit breaker
 *
 * Verifies invariants of the circuit breaker state machine by exhaustively
 * testing transition sequences across parameterized configurations:
 *
 *   P1. Trip threshold: exactly failure_threshold consecutive failures
 *       in CLOSED transitions to OPEN.
 *   P2. Recovery threshold: exactly success_threshold consecutive successes
 *       in HALF_OPEN transitions to CLOSED.
 *   P3. Fragile probe: any failure in HALF_OPEN immediately reopens.
 *   P4. Counter monotonicity: total_trips and total_rejected never decrease.
 *   P5. Reset idempotence: reset always produces CLOSED with zeroed counters.
 *   P6. Open rejection: all calls in OPEN state are rejected.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("Algebraic property test — exhaustive enumeration") */

#include "../../test_harness.h"
#include <asx/core/circuit_breaker.h>

/* ------------------------------------------------------------------ */
/* P1. Trip threshold                                                  */
/* ------------------------------------------------------------------ */

TEST(trip_at_exact_threshold) {
    /* For various thresholds, verify trip happens at exactly N failures */
    uint32_t thresh;
    for (thresh = 1; thresh <= 10; thresh++) {
        asx_circuit_breaker cb;
        asx_breaker_config cfg = {thresh, 2, 1};
        uint32_t i;

        ASSERT_EQ(asx_breaker_init_with(&cb, &cfg), ASX_OK);

        /* N-1 failures: still CLOSED */
        for (i = 0; i < thresh - 1; i++) {
            ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
            asx_breaker_record_failure(&cb);
            ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);
        }

        /* Nth failure: trips to OPEN */
        ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
        asx_breaker_record_failure(&cb);
        ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);
        ASSERT_EQ(asx_breaker_total_trips(&cb), 1u);
    }
}

/* ------------------------------------------------------------------ */
/* P2. Recovery threshold                                              */
/* ------------------------------------------------------------------ */

TEST(recovery_at_exact_threshold) {
    /* For various success thresholds, verify recovery at exactly N successes */
    uint32_t thresh;
    for (thresh = 1; thresh <= 10; thresh++) {
        asx_circuit_breaker cb;
        asx_breaker_config cfg = {1, thresh, thresh};
        uint32_t i;

        ASSERT_EQ(asx_breaker_init_with(&cb, &cfg), ASX_OK);

        /* Trip to OPEN */
        ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
        asx_breaker_record_failure(&cb);
        ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);

        /* Probe */
        ASSERT_EQ(asx_breaker_half_open(&cb), ASX_OK);

        /* N-1 successes: still HALF_OPEN */
        for (i = 0; i < thresh - 1; i++) {
            ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
            asx_breaker_record_success(&cb);
            ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_HALF_OPEN);
        }

        /* Nth success: recovers to CLOSED */
        ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
        asx_breaker_record_success(&cb);
        ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);
    }
}

/* ------------------------------------------------------------------ */
/* P3. Fragile probe                                                   */
/* ------------------------------------------------------------------ */

TEST(half_open_failure_always_reopens) {
    /* Any single failure in HALF_OPEN immediately transitions to OPEN,
     * regardless of how many successes preceded it. */
    uint32_t prior_successes;
    for (prior_successes = 0; prior_successes < 5; prior_successes++) {
        asx_circuit_breaker cb;
        asx_breaker_config cfg = {1, 10, 10}; /* high success threshold */
        uint32_t i;

        ASSERT_EQ(asx_breaker_init_with(&cb, &cfg), ASX_OK);

        /* Trip and probe */
        ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
        asx_breaker_record_failure(&cb);
        ASSERT_EQ(asx_breaker_half_open(&cb), ASX_OK);

        /* Record some successes */
        for (i = 0; i < prior_successes; i++) {
            ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
            asx_breaker_record_success(&cb);
        }
        ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_HALF_OPEN);

        /* Single failure: back to OPEN */
        ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
        asx_breaker_record_failure(&cb);
        ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);
    }
}

/* ------------------------------------------------------------------ */
/* P4. Counter monotonicity                                            */
/* ------------------------------------------------------------------ */

TEST(counter_monotonicity_across_cycles) {
    /* total_trips and total_rejected never decrease across multiple
     * trip/recovery cycles. */
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {2, 1, 1};
    uint32_t cycle;
    uint32_t prev_trips = 0;
    uint32_t prev_rejected = 0;

    ASSERT_EQ(asx_breaker_init_with(&cb, &cfg), ASX_OK);

    for (cycle = 0; cycle < 5; cycle++) {
        /* Trip: 2 failures */
        ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
        asx_breaker_record_failure(&cb);
        ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
        asx_breaker_record_failure(&cb);
        ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);

        /* Try a call while OPEN (rejected) */
        ASSERT_EQ(asx_breaker_allow(&cb), ASX_E_ADMISSION_CLOSED);

        /* Check monotonicity */
        ASSERT_TRUE(asx_breaker_total_trips(&cb) >= prev_trips);
        ASSERT_TRUE(asx_breaker_total_rejected(&cb) >= prev_rejected);
        prev_trips = asx_breaker_total_trips(&cb);
        prev_rejected = asx_breaker_total_rejected(&cb);

        /* Recover: probe + 1 success */
        ASSERT_EQ(asx_breaker_half_open(&cb), ASX_OK);
        ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
        asx_breaker_record_success(&cb);
        ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);

        /* Still monotone */
        ASSERT_TRUE(asx_breaker_total_trips(&cb) >= prev_trips);
        ASSERT_TRUE(asx_breaker_total_rejected(&cb) >= prev_rejected);
        prev_trips = asx_breaker_total_trips(&cb);
        prev_rejected = asx_breaker_total_rejected(&cb);
    }

    ASSERT_EQ(asx_breaker_total_trips(&cb), 5u);
    ASSERT_TRUE(asx_breaker_total_rejected(&cb) >= 5u);
}

/* ------------------------------------------------------------------ */
/* P5. Reset idempotence                                               */
/* ------------------------------------------------------------------ */

TEST(reset_always_produces_closed) {
    /* Reset from any state produces CLOSED with zeroed failure/success counts. */
    asx_breaker_state states_to_test[] = {ASX_BREAKER_CLOSED, ASX_BREAKER_OPEN,
                                          ASX_BREAKER_HALF_OPEN};
    int si;

    for (si = 0; si < 3; si++) {
        asx_circuit_breaker cb;
        asx_breaker_config cfg = {1, 1, 1};

        ASSERT_EQ(asx_breaker_init_with(&cb, &cfg), ASX_OK);

        /* Reach target state */
        if (states_to_test[si] == ASX_BREAKER_OPEN || states_to_test[si] == ASX_BREAKER_HALF_OPEN) {
            ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
            asx_breaker_record_failure(&cb);
        }
        if (states_to_test[si] == ASX_BREAKER_HALF_OPEN) {
            ASSERT_EQ(asx_breaker_half_open(&cb), ASX_OK);
        }
        ASSERT_EQ(asx_breaker_get_state(&cb), states_to_test[si]);

        /* Reset */
        asx_breaker_reset(&cb);
        ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);
        ASSERT_EQ(asx_breaker_failure_count(&cb), 0u);

        /* Double reset is idempotent */
        asx_breaker_reset(&cb);
        ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);
    }
}

/* ------------------------------------------------------------------ */
/* P6. Open rejection                                                  */
/* ------------------------------------------------------------------ */

TEST(open_rejects_all_calls) {
    /* Once OPEN, every call attempt is rejected until probe. */
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {1, 1, 1};
    uint32_t i;

    ASSERT_EQ(asx_breaker_init_with(&cb, &cfg), ASX_OK);

    /* Trip */
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);

    /* Multiple rejection attempts */
    for (i = 0; i < 20; i++) {
        ASSERT_EQ(asx_breaker_allow(&cb), ASX_E_ADMISSION_CLOSED);
        ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);
    }

    ASSERT_EQ(asx_breaker_total_rejected(&cb), 20u);
}

/* ------------------------------------------------------------------ */
/* P7. Success in CLOSED resets failure counter                        */
/* ------------------------------------------------------------------ */

TEST(success_resets_failure_progress) {
    /* A success in CLOSED resets the failure counter, requiring
     * failure_threshold consecutive failures to trip. */
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {3, 1, 1};

    ASSERT_EQ(asx_breaker_init_with(&cb, &cfg), ASX_OK);

    /* 2 failures (below threshold) */
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_failure_count(&cb), 2u);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);

    /* 1 success: resets */
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    asx_breaker_record_success(&cb);
    ASSERT_EQ(asx_breaker_failure_count(&cb), 0u);

    /* Now need another 3 consecutive failures to trip */
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);

    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "[circuit-breaker-sm] algebraic state machine properties\n");

    RUN_TEST(trip_at_exact_threshold);
    RUN_TEST(recovery_at_exact_threshold);
    RUN_TEST(half_open_failure_always_reopens);
    RUN_TEST(counter_monotonicity_across_cycles);
    RUN_TEST(reset_always_produces_closed);
    RUN_TEST(open_rejects_all_calls);
    RUN_TEST(success_resets_failure_progress);

    TEST_REPORT();
    return test_failures;
}
