/*
 * test_session.c — unit tests for bidirectional session channel
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/session.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static asx_runtime g_rt;

static void setup(void) {
    MUST_OK(asx_runtime_init_default(&g_rt));
    asx_session_reset();
}

static void teardown(void) { asx_runtime_shutdown(&g_rt); }

/* ------------------------------------------------------------------ */
/* Create tests                                                        */
/* ------------------------------------------------------------------ */

TEST(create_null_initiator_fails) {
    asx_session_endpoint resp;
    asx_region_id rid;
    setup();
    MUST_OK(asx_region_open(&rid));
    ASSERT_EQ(asx_session_create(rid, 4, NULL, &resp), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(create_null_responder_fails) {
    asx_session_endpoint init;
    asx_region_id rid;
    setup();
    MUST_OK(asx_region_open(&rid));
    ASSERT_EQ(asx_session_create(rid, 4, &init, NULL), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(create_zero_capacity_fails) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    setup();
    MUST_OK(asx_region_open(&rid));
    ASSERT_EQ(asx_session_create(rid, 0, &init, &resp), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(create_success) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    setup();
    MUST_OK(asx_region_open(&rid));
    ASSERT_EQ(asx_session_create(rid, 4, &init, &resp), ASX_OK);
    ASSERT_EQ(init.slot, resp.slot);
    ASSERT_TRUE(init.is_initiator);
    ASSERT_FALSE(resp.is_initiator);
    ASSERT_EQ(asx_session_get_state(init.slot, init.generation), ASX_SESSION_OPEN);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Bidirectional send/recv                                             */
/* ------------------------------------------------------------------ */

TEST(initiator_sends_responder_receives) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    uint64_t val;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    MUST_OK(asx_session_send(&init, 100));
    ASSERT_EQ(asx_session_try_recv(&resp, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)100);
    teardown();
}

TEST(responder_sends_initiator_receives) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    uint64_t val;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    MUST_OK(asx_session_send(&resp, 200));
    ASSERT_EQ(asx_session_try_recv(&init, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)200);
    teardown();
}

TEST(bidirectional_exchange) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    uint64_t val;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    /* Initiator sends request */
    MUST_OK(asx_session_send(&init, 10));
    /* Responder receives request */
    MUST_OK(asx_session_try_recv(&resp, &val));
    ASSERT_EQ(val, (uint64_t)10);
    /* Responder sends response */
    MUST_OK(asx_session_send(&resp, 20));
    /* Initiator receives response */
    MUST_OK(asx_session_try_recv(&init, &val));
    ASSERT_EQ(val, (uint64_t)20);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Obligation tracking                                                 */
/* ------------------------------------------------------------------ */

TEST(obligations_increment_on_initiator_send) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    ASSERT_EQ(asx_session_obligations(init.slot, init.generation), 0u);
    MUST_OK(asx_session_send(&init, 1));
    ASSERT_EQ(asx_session_obligations(init.slot, init.generation), 1u);
    MUST_OK(asx_session_send(&init, 2));
    ASSERT_EQ(asx_session_obligations(init.slot, init.generation), 2u);
    teardown();
}

TEST(obligations_decrement_on_responder_recv) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    uint64_t val;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    MUST_OK(asx_session_send(&init, 1));
    MUST_OK(asx_session_send(&init, 2));
    ASSERT_EQ(asx_session_obligations(init.slot, init.generation), 2u);
    MUST_OK(asx_session_try_recv(&resp, &val));
    ASSERT_EQ(asx_session_obligations(init.slot, init.generation), 1u);
    MUST_OK(asx_session_try_recv(&resp, &val));
    ASSERT_EQ(asx_session_obligations(init.slot, init.generation), 0u);
    teardown();
}

TEST(responder_send_no_obligation_change) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    MUST_OK(asx_session_send(&resp, 50));
    ASSERT_EQ(asx_session_obligations(init.slot, init.generation), 0u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Disconnection                                                       */
/* ------------------------------------------------------------------ */

TEST(initiator_drop_half_closes) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    asx_session_endpoint_drop(&init);
    ASSERT_EQ(asx_session_get_state(resp.slot, resp.generation), ASX_SESSION_HALF_CLOSED);
    teardown();
}

TEST(both_drop_closes) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    asx_session_endpoint_drop(&init);
    asx_session_endpoint_drop(&resp);
    ASSERT_EQ(asx_session_get_state(resp.slot, resp.generation), ASX_SESSION_CLOSED);
    teardown();
}

TEST(send_to_dropped_peer_disconnected) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    asx_session_endpoint_drop(&resp);
    ASSERT_EQ(asx_session_send(&init, 1), ASX_E_DISCONNECTED);
    teardown();
}

TEST(recv_from_dropped_peer_empty_disconnected) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    uint64_t val;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    asx_session_endpoint_drop(&init);
    ASSERT_EQ(asx_session_try_recv(&resp, &val), ASX_E_DISCONNECTED);
    teardown();
}

TEST(recv_drains_before_disconnected) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    uint64_t val;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    MUST_OK(asx_session_send(&init, 99));
    asx_session_endpoint_drop(&init);
    /* Message still available */
    ASSERT_EQ(asx_session_try_recv(&resp, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)99);
    /* Now disconnected */
    ASSERT_EQ(asx_session_try_recv(&resp, &val), ASX_E_DISCONNECTED);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Peer alive query                                                    */
/* ------------------------------------------------------------------ */

TEST(peer_alive_true_initially) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    ASSERT_TRUE(asx_session_peer_alive(&init));
    ASSERT_TRUE(asx_session_peer_alive(&resp));
    teardown();
}

TEST(peer_alive_false_after_drop) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    asx_session_endpoint_drop(&resp);
    ASSERT_FALSE(asx_session_peer_alive(&init));
    ASSERT_TRUE(asx_session_peer_alive(&resp)); /* initiator still alive from resp's POV... wait */
    teardown();
}

/* ------------------------------------------------------------------ */
/* Null safety                                                         */
/* ------------------------------------------------------------------ */

TEST(send_null_fails) { ASSERT_EQ(asx_session_send(NULL, 1), ASX_E_INVALID_ARGUMENT); }

TEST(recv_null_fails) {
    uint64_t val;
    ASSERT_EQ(asx_session_try_recv(NULL, &val), ASX_E_INVALID_ARGUMENT);
}

TEST(peer_alive_null_false) { ASSERT_FALSE(asx_session_peer_alive(NULL)); }

/* ------------------------------------------------------------------ */
/* Capacity                                                            */
/* ------------------------------------------------------------------ */

TEST(channel_full_on_capacity) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    uint32_t i;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    for (i = 0; i < 4; i++) { MUST_OK(asx_session_send(&init, i)); }
    ASSERT_EQ(asx_session_send(&init, 99), ASX_E_CHANNEL_FULL);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Would block                                                         */
/* ------------------------------------------------------------------ */

TEST(recv_empty_would_block) {
    asx_session_endpoint init, resp;
    asx_region_id rid;
    uint64_t val;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_session_create(rid, 4, &init, &resp));
    ASSERT_EQ(asx_session_try_recv(&resp, &val), ASX_E_WOULD_BLOCK);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_session ===\n");

    RUN_TEST(create_null_initiator_fails);
    RUN_TEST(create_null_responder_fails);
    RUN_TEST(create_zero_capacity_fails);
    RUN_TEST(create_success);

    RUN_TEST(initiator_sends_responder_receives);
    RUN_TEST(responder_sends_initiator_receives);
    RUN_TEST(bidirectional_exchange);

    RUN_TEST(obligations_increment_on_initiator_send);
    RUN_TEST(obligations_decrement_on_responder_recv);
    RUN_TEST(responder_send_no_obligation_change);

    RUN_TEST(initiator_drop_half_closes);
    RUN_TEST(both_drop_closes);
    RUN_TEST(send_to_dropped_peer_disconnected);
    RUN_TEST(recv_from_dropped_peer_empty_disconnected);
    RUN_TEST(recv_drains_before_disconnected);

    RUN_TEST(peer_alive_true_initially);
    RUN_TEST(peer_alive_false_after_drop);

    RUN_TEST(send_null_fails);
    RUN_TEST(recv_null_fails);
    RUN_TEST(peer_alive_null_false);

    RUN_TEST(channel_full_on_capacity);
    RUN_TEST(recv_empty_would_block);

    TEST_REPORT();
    return test_failures;
}
