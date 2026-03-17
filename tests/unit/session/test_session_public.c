/*
 * test_session_public.c — unit tests for public session helper surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/session/session.h>

static asx_runtime g_rt;
static asx_status st_sink_;

#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static void setup(void) {
    MUST_OK(asx_runtime_init_default(&g_rt));
    asx_session_reset();
}

static void teardown(void) { asx_runtime_shutdown(&g_rt); }

TEST(open_null_pair_fails) {
    asx_region_id region;

    setup();
    MUST_OK(asx_region_open(&region));
    ASSERT_EQ(asx_session_open(region, 4u, NULL), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(open_request_response_roundtrip) {
    asx_region_id region;
    asx_session_pair pair;
    uint64_t value;

    setup();
    MUST_OK(asx_region_open(&region));
    ASSERT_EQ(asx_session_open(region, 4u, &pair), ASX_OK);
    ASSERT_EQ(asx_session_pair_state(&pair), ASX_SESSION_OPEN);

    ASSERT_EQ(asx_session_send_request(&pair, 11u), ASX_OK);
    ASSERT_EQ(asx_session_pair_obligations(&pair), 1u);
    ASSERT_EQ(asx_session_recv_request(&pair, &value), ASX_OK);
    ASSERT_EQ(value, 11u);
    ASSERT_EQ(asx_session_pair_obligations(&pair), 0u);

    ASSERT_EQ(asx_session_send_response(&pair, 22u), ASX_OK);
    ASSERT_EQ(asx_session_recv_response(&pair, &value), ASX_OK);
    ASSERT_EQ(value, 22u);

    asx_session_close_initiator(&pair);
    asx_session_close_responder(&pair);
    teardown();
}

TEST(close_helpers_transition_pair_state) {
    asx_region_id region;
    asx_session_pair pair;

    setup();
    MUST_OK(asx_region_open(&region));
    ASSERT_EQ(asx_session_open(region, 4u, &pair), ASX_OK);

    asx_session_close_initiator(&pair);
    ASSERT_EQ(asx_session_pair_state(&pair), ASX_SESSION_HALF_CLOSED);
    asx_session_close_responder(&pair);
    ASSERT_EQ(asx_session_pair_state(&pair), ASX_SESSION_CLOSED);
    teardown();
}

TEST(null_helpers_fail_closed_or_empty) {
    uint64_t value = 0u;

    ASSERT_EQ(asx_session_send_request(NULL, 1u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_session_recv_request(NULL, &value), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_session_send_response(NULL, 2u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_session_recv_response(NULL, &value), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_session_pair_state(NULL), ASX_SESSION_CLOSED);
    ASSERT_EQ(asx_session_pair_obligations(NULL), 0u);

    asx_session_close_initiator(NULL);
    asx_session_close_responder(NULL);
}

TEST(state_name_covers_public_values) {
    ASSERT_STR_EQ(asx_session_state_name(ASX_SESSION_OPEN), "open");
    ASSERT_STR_EQ(asx_session_state_name(ASX_SESSION_HALF_CLOSED), "half_closed");
    ASSERT_STR_EQ(asx_session_state_name(ASX_SESSION_CLOSED), "closed");
    ASSERT_STR_EQ(asx_session_state_name((asx_session_state)99), "unknown");
}

int main(void) {
    RUN_TEST(open_null_pair_fails);
    RUN_TEST(open_request_response_roundtrip);
    RUN_TEST(close_helpers_transition_pair_state);
    RUN_TEST(null_helpers_fail_closed_or_empty);
    RUN_TEST(state_name_covers_public_values);
    TEST_REPORT();
    return test_failures;
}
