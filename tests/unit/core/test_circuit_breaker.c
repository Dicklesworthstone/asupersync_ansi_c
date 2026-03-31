/*
 * test_circuit_breaker.c — unit tests for circuit breaker
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/circuit_breaker.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

TEST(init_defaults) {
    asx_circuit_breaker cb;
    asx_breaker_init(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);
    ASSERT_EQ(asx_breaker_failure_count(&cb), 0u);
    ASSERT_EQ(asx_breaker_total_trips(&cb), 0u);
}

TEST(init_custom) {
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {3, 1, 2};
    ASSERT_EQ(asx_breaker_init_with(&cb, &cfg), ASX_OK);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);
}

TEST(init_null) {
    ASSERT_EQ(asx_breaker_init_with(NULL, NULL), ASX_E_INVALID_ARGUMENT);
    asx_circuit_breaker cb;
    ASSERT_EQ(asx_breaker_init_with(&cb, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(init_zero_threshold) {
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {0, 1, 1}; /* zero failure_threshold invalid */
    ASSERT_EQ(asx_breaker_init_with(&cb, &cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(init_zero_half_open_max_calls) {
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {1, 1, 0};
    ASSERT_EQ(asx_breaker_init_with(&cb, &cfg), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Closed state — tracking failures                                    */
/* ------------------------------------------------------------------ */

TEST(closed_allows_calls) {
    asx_circuit_breaker cb;
    asx_breaker_init(&cb);
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
}

TEST(closed_success_resets_failures) {
    asx_circuit_breaker cb;
    asx_breaker_init(&cb);
    asx_breaker_record_failure(&cb);
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_failure_count(&cb), 2u);
    asx_breaker_record_success(&cb);
    ASSERT_EQ(asx_breaker_failure_count(&cb), 0u);
}

/* ------------------------------------------------------------------ */
/* Trip to OPEN                                                        */
/* ------------------------------------------------------------------ */

TEST(trip_after_threshold) {
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {3, 2, 1};
    uint32_t i;
    MUST_OK(asx_breaker_init_with(&cb, &cfg));
    for (i = 0; i < 3; i++) { asx_breaker_record_failure(&cb); }
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);
    ASSERT_EQ(asx_breaker_total_trips(&cb), 1u);
}

TEST(open_rejects_calls) {
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {2, 1, 1};
    MUST_OK(asx_breaker_init_with(&cb, &cfg));
    asx_breaker_record_failure(&cb);
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_E_ADMISSION_CLOSED);
    ASSERT_EQ(asx_breaker_total_rejected(&cb), 1u);
}

/* ------------------------------------------------------------------ */
/* Half-open probing                                                   */
/* ------------------------------------------------------------------ */

TEST(half_open_transition) {
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {2, 2, 1};
    MUST_OK(asx_breaker_init_with(&cb, &cfg));
    asx_breaker_record_failure(&cb);
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);
    MUST_OK(asx_breaker_half_open(&cb));
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_HALF_OPEN);
}

TEST(half_open_success_closes) {
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {2, 2, 1};
    MUST_OK(asx_breaker_init_with(&cb, &cfg));
    asx_breaker_record_failure(&cb);
    asx_breaker_record_failure(&cb);
    MUST_OK(asx_breaker_half_open(&cb));
    asx_breaker_record_success(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_HALF_OPEN);
    asx_breaker_record_success(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);
}

TEST(half_open_failure_reopens) {
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {2, 2, 1};
    MUST_OK(asx_breaker_init_with(&cb, &cfg));
    asx_breaker_record_failure(&cb);
    asx_breaker_record_failure(&cb);
    MUST_OK(asx_breaker_half_open(&cb));
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);
    ASSERT_EQ(asx_breaker_total_trips(&cb), 2u);
}

TEST(half_open_limits_calls) {
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {2, 2, 1}; /* max 1 call in half-open */
    MUST_OK(asx_breaker_init_with(&cb, &cfg));
    asx_breaker_record_failure(&cb);
    asx_breaker_record_failure(&cb);
    MUST_OK(asx_breaker_half_open(&cb));
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_E_WOULD_BLOCK);
}

TEST(half_open_from_closed_fails) {
    asx_circuit_breaker cb;
    asx_breaker_init(&cb);
    ASSERT_EQ(asx_breaker_half_open(&cb), ASX_E_INVALID_STATE);
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

TEST(reset_to_closed) {
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {2, 2, 1};
    MUST_OK(asx_breaker_init_with(&cb, &cfg));
    asx_breaker_record_failure(&cb);
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);
    asx_breaker_reset(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);
    ASSERT_EQ(asx_breaker_failure_count(&cb), 0u);
}

/* ------------------------------------------------------------------ */
/* String helpers                                                      */
/* ------------------------------------------------------------------ */

TEST(state_str) {
    ASSERT_STR_EQ(asx_breaker_state_str(ASX_BREAKER_CLOSED), "closed");
    ASSERT_STR_EQ(asx_breaker_state_str(ASX_BREAKER_OPEN), "open");
    ASSERT_STR_EQ(asx_breaker_state_str(ASX_BREAKER_HALF_OPEN), "half_open");
}

/* ------------------------------------------------------------------ */
/* Full lifecycle scenario                                             */
/* ------------------------------------------------------------------ */

TEST(full_lifecycle) {
    asx_circuit_breaker cb;
    asx_breaker_config cfg = {3, 2, 1};
    MUST_OK(asx_breaker_init_with(&cb, &cfg));

    /* Phase 1: Normal operation */
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    asx_breaker_record_success(&cb);

    /* Phase 2: Failures accumulate */
    MUST_OK(asx_breaker_allow(&cb));
    asx_breaker_record_failure(&cb);
    MUST_OK(asx_breaker_allow(&cb));
    asx_breaker_record_failure(&cb);
    MUST_OK(asx_breaker_allow(&cb));
    asx_breaker_record_failure(&cb);
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_OPEN);

    /* Phase 3: Open rejects */
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_E_ADMISSION_CLOSED);

    /* Phase 4: Half-open probe */
    MUST_OK(asx_breaker_half_open(&cb));
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    asx_breaker_record_success(&cb);
    ASSERT_EQ(asx_breaker_allow(&cb), ASX_OK);
    asx_breaker_record_success(&cb);

    /* Phase 5: Recovered */
    ASSERT_EQ(asx_breaker_get_state(&cb), ASX_BREAKER_CLOSED);
    ASSERT_EQ(asx_breaker_total_trips(&cb), 1u);
}

/* ------------------------------------------------------------------ */
/* Null safety                                                         */
/* ------------------------------------------------------------------ */

TEST(null_queries) {
    ASSERT_EQ(asx_breaker_get_state(NULL), ASX_BREAKER_CLOSED);
    ASSERT_EQ(asx_breaker_failure_count(NULL), 0u);
    ASSERT_EQ(asx_breaker_total_trips(NULL), 0u);
    ASSERT_EQ(asx_breaker_total_rejected(NULL), 0u);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "test_circuit_breaker\n");

    RUN_TEST(init_defaults);
    RUN_TEST(init_custom);
    RUN_TEST(init_null);
    RUN_TEST(init_zero_threshold);
    RUN_TEST(init_zero_half_open_max_calls);
    RUN_TEST(closed_allows_calls);
    RUN_TEST(closed_success_resets_failures);
    RUN_TEST(trip_after_threshold);
    RUN_TEST(open_rejects_calls);
    RUN_TEST(half_open_transition);
    RUN_TEST(half_open_success_closes);
    RUN_TEST(half_open_failure_reopens);
    RUN_TEST(half_open_limits_calls);
    RUN_TEST(half_open_from_closed_fails);
    RUN_TEST(reset_to_closed);
    RUN_TEST(state_str);
    RUN_TEST(full_lifecycle);
    RUN_TEST(null_queries);

    TEST_REPORT();
    return test_failures;
}
