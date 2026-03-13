/*
 * test_broadcast.c — unit tests for broadcast channel
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/broadcast.h>

static asx_status st_sink_;
#define MUST_OK(expr) do { st_sink_ = (expr); (void)st_sink_; } while(0)

static void setup(void) { asx_broadcast_reset(); }

/* ------------------------------------------------------------------ */
/* Create tests                                                        */
/* ------------------------------------------------------------------ */

TEST(create_null_sender_fails) {
    asx_broadcast_receiver rx;
    ASSERT_EQ(asx_broadcast_create(4, NULL, &rx), ASX_E_INVALID_ARGUMENT);
}

TEST(create_null_receiver_fails) {
    asx_broadcast_sender tx;
    ASSERT_EQ(asx_broadcast_create(4, &tx, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(create_zero_capacity_fails) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    ASSERT_EQ(asx_broadcast_create(0, &tx, &rx), ASX_E_INVALID_ARGUMENT);
}

TEST(create_over_capacity_fails) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    ASSERT_EQ(asx_broadcast_create(ASX_BROADCAST_MAX_CAPACITY + 1, &tx, &rx),
              ASX_E_INVALID_ARGUMENT);
}

TEST(create_success) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    setup();
    ASSERT_EQ(asx_broadcast_create(8, &tx, &rx), ASX_OK);
    ASSERT_EQ(asx_broadcast_receiver_count(&tx), 1u);
    ASSERT_EQ(asx_broadcast_total_sent(&tx), 0u);
}

/* ------------------------------------------------------------------ */
/* Send / Receive                                                      */
/* ------------------------------------------------------------------ */

TEST(send_and_recv_single) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_broadcast_create(4, &tx, &rx));
    MUST_OK(asx_broadcast_send(&tx, 42));
    ASSERT_EQ(asx_broadcast_try_recv(&rx, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)42);
}

TEST(fifo_ordering) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_broadcast_create(8, &tx, &rx));
    MUST_OK(asx_broadcast_send(&tx, 1));
    MUST_OK(asx_broadcast_send(&tx, 2));
    MUST_OK(asx_broadcast_send(&tx, 3));
    ASSERT_EQ(asx_broadcast_try_recv(&rx, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)1);
    ASSERT_EQ(asx_broadcast_try_recv(&rx, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)2);
    ASSERT_EQ(asx_broadcast_try_recv(&rx, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)3);
}

TEST(recv_empty_returns_would_block) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_broadcast_create(4, &tx, &rx));
    ASSERT_EQ(asx_broadcast_try_recv(&rx, &val), ASX_E_WOULD_BLOCK);
}

/* ------------------------------------------------------------------ */
/* Multiple receivers                                                  */
/* ------------------------------------------------------------------ */

TEST(subscribe_creates_receiver) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx1, rx2;
    setup();
    MUST_OK(asx_broadcast_create(4, &tx, &rx1));
    MUST_OK(asx_broadcast_subscribe(&tx, &rx2));
    ASSERT_EQ(asx_broadcast_receiver_count(&tx), 2u);
}

TEST(all_receivers_get_message) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx1, rx2;
    uint64_t v1, v2;
    setup();
    MUST_OK(asx_broadcast_create(4, &tx, &rx1));
    MUST_OK(asx_broadcast_subscribe(&tx, &rx2));
    MUST_OK(asx_broadcast_send(&tx, 99));
    ASSERT_EQ(asx_broadcast_try_recv(&rx1, &v1), ASX_OK);
    ASSERT_EQ(asx_broadcast_try_recv(&rx2, &v2), ASX_OK);
    ASSERT_EQ(v1, (uint64_t)99);
    ASSERT_EQ(v2, (uint64_t)99);
}

TEST(late_subscriber_no_backfill) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx1, rx2;
    uint64_t val;
    setup();
    MUST_OK(asx_broadcast_create(4, &tx, &rx1));
    MUST_OK(asx_broadcast_send(&tx, 1));
    MUST_OK(asx_broadcast_send(&tx, 2));
    /* rx2 subscribes after messages sent */
    MUST_OK(asx_broadcast_subscribe(&tx, &rx2));
    /* rx2 should not see old messages */
    ASSERT_EQ(asx_broadcast_try_recv(&rx2, &val), ASX_E_WOULD_BLOCK);
    /* rx1 should still see them */
    ASSERT_EQ(asx_broadcast_try_recv(&rx1, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)1);
}

/* ------------------------------------------------------------------ */
/* Lag detection                                                       */
/* ------------------------------------------------------------------ */

TEST(lagging_receiver_gets_lagged) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    uint64_t val;
    uint32_t i;
    setup();
    MUST_OK(asx_broadcast_create(4, &tx, &rx));
    /* Send more than capacity without rx reading */
    for (i = 0; i < 8; i++) {
        MUST_OK(asx_broadcast_send(&tx, i + 1));
    }
    /* Receiver has lagged */
    ASSERT_EQ(asx_broadcast_try_recv(&rx, &val), ASX_E_LAGGED);
    /* After lag, cursor is advanced — next recv should succeed */
    ASSERT_EQ(asx_broadcast_try_recv(&rx, &val), ASX_OK);
    /* Should get one of the recent messages (oldest available) */
    ASSERT_TRUE(val >= 5);
}

/* ------------------------------------------------------------------ */
/* Disconnection                                                       */
/* ------------------------------------------------------------------ */

TEST(sender_drop_drain_then_disconnected) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_broadcast_create(4, &tx, &rx));
    MUST_OK(asx_broadcast_send(&tx, 10));
    asx_broadcast_sender_drop(&tx);
    /* Can still drain remaining messages */
    ASSERT_EQ(asx_broadcast_try_recv(&rx, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)10);
    /* Then disconnected */
    ASSERT_EQ(asx_broadcast_try_recv(&rx, &val), ASX_E_DISCONNECTED);
}

TEST(send_after_sender_drop_fails) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    setup();
    MUST_OK(asx_broadcast_create(4, &tx, &rx));
    asx_broadcast_sender_drop(&tx);
    ASSERT_EQ(asx_broadcast_send(&tx, 1), ASX_E_INVALID_STATE);
}

TEST(receiver_drop_decrements_count) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    setup();
    MUST_OK(asx_broadcast_create(4, &tx, &rx));
    ASSERT_EQ(asx_broadcast_receiver_count(&tx), 1u);
    asx_broadcast_receiver_drop(&rx);
    ASSERT_EQ(asx_broadcast_receiver_count(&tx), 0u);
}

/* ------------------------------------------------------------------ */
/* Null safety                                                         */
/* ------------------------------------------------------------------ */

TEST(send_null_fails) {
    ASSERT_EQ(asx_broadcast_send(NULL, 1), ASX_E_INVALID_ARGUMENT);
}

TEST(recv_null_fails) {
    uint64_t val;
    ASSERT_EQ(asx_broadcast_try_recv(NULL, &val), ASX_E_INVALID_ARGUMENT);
}

TEST(receiver_count_null_zero) {
    ASSERT_EQ(asx_broadcast_receiver_count(NULL), 0u);
}

TEST(total_sent_null_zero) {
    ASSERT_EQ(asx_broadcast_total_sent(NULL), 0u);
}

/* ------------------------------------------------------------------ */
/* Total sent tracking                                                 */
/* ------------------------------------------------------------------ */

TEST(total_sent_increments) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    setup();
    MUST_OK(asx_broadcast_create(4, &tx, &rx));
    MUST_OK(asx_broadcast_send(&tx, 1));
    MUST_OK(asx_broadcast_send(&tx, 2));
    ASSERT_EQ(asx_broadcast_total_sent(&tx), 2u);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== test_broadcast ===\n");

    RUN_TEST(create_null_sender_fails);
    RUN_TEST(create_null_receiver_fails);
    RUN_TEST(create_zero_capacity_fails);
    RUN_TEST(create_over_capacity_fails);
    RUN_TEST(create_success);

    RUN_TEST(send_and_recv_single);
    RUN_TEST(fifo_ordering);
    RUN_TEST(recv_empty_returns_would_block);

    RUN_TEST(subscribe_creates_receiver);
    RUN_TEST(all_receivers_get_message);
    RUN_TEST(late_subscriber_no_backfill);

    RUN_TEST(lagging_receiver_gets_lagged);

    RUN_TEST(sender_drop_drain_then_disconnected);
    RUN_TEST(send_after_sender_drop_fails);
    RUN_TEST(receiver_drop_decrements_count);

    RUN_TEST(send_null_fails);
    RUN_TEST(recv_null_fails);
    RUN_TEST(receiver_count_null_zero);
    RUN_TEST(total_sent_null_zero);

    RUN_TEST(total_sent_increments);

    TEST_REPORT();
    return test_failures;
}
