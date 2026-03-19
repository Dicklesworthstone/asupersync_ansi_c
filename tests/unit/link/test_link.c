/*
 * test_link.c — unit tests for public link coordination surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/link/link.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>

static asx_runtime g_rt;

static void setup(void) {
    ASSERT_EQ(asx_runtime_init_default(&g_rt), ASX_OK);
    asx_session_reset();
}

static void teardown(void) { asx_runtime_shutdown(&g_rt); }

TEST(open_and_exchange_roundtrip) {
    asx_region_id region;
    asx_link link;
    uint64_t value;

    setup();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_link_open(region, 4u, &link), ASX_OK);
    ASSERT_EQ(asx_link_send_request(&link, 11u), ASX_OK);
    ASSERT_EQ(asx_link_recv_request(&link, &value), ASX_OK);
    ASSERT_EQ(value, 11u);
    ASSERT_EQ(asx_link_send_response(&link, 22u), ASX_OK);
    ASSERT_EQ(asx_link_recv_response(&link, &value), ASX_OK);
    ASSERT_EQ(value, 22u);
    asx_link_close(&link);
    teardown();
}

TEST(summary_tracks_counts_and_obligations) {
    asx_region_id region;
    asx_link link;
    asx_link_summary summary;
    uint64_t value;

    setup();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_link_open(region, 4u, &link), ASX_OK);
    ASSERT_EQ(asx_link_send_request(&link, 7u), ASX_OK);
    ASSERT_EQ(asx_link_capture_summary(&link, &summary), ASX_OK);
    ASSERT_EQ(summary.outstanding_obligations, 1u);
    ASSERT_EQ(summary.requests_sent, 1u);
    ASSERT_EQ(asx_link_recv_request(&link, &value), ASX_OK);
    ASSERT_EQ(asx_link_capture_summary(&link, &summary), ASX_OK);
    /* Obligations persist until initiator receives the response. */
    ASSERT_EQ(summary.outstanding_obligations, 1u);
    ASSERT_EQ(summary.requests_received, 1u);
    /* Complete the cycle: respond and receive response. */
    ASSERT_EQ(asx_link_send_response(&link, 7u), ASX_OK);
    ASSERT_EQ(asx_link_recv_response(&link, &value), ASX_OK);
    ASSERT_EQ(asx_link_capture_summary(&link, &summary), ASX_OK);
    ASSERT_EQ(summary.outstanding_obligations, 0u);
    asx_link_close(&link);
    teardown();
}

TEST(close_transitions_state) {
    asx_region_id region;
    asx_link link;
    asx_link_summary summary;

    setup();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_link_open(region, 4u, &link), ASX_OK);
    ASSERT_EQ(asx_link_capture_summary(&link, &summary), ASX_OK);
    ASSERT_EQ(summary.state, ASX_SESSION_OPEN);
    asx_link_close(&link);
    ASSERT_EQ(asx_link_capture_summary(&link, &summary), ASX_OK);
    ASSERT_EQ(summary.state, ASX_SESSION_CLOSED);
    teardown();
}

int main(void) {
    RUN_TEST(open_and_exchange_roundtrip);
    RUN_TEST(summary_tracks_counts_and_obligations);
    RUN_TEST(close_transitions_state);
    TEST_REPORT();
    return test_failures;
}
