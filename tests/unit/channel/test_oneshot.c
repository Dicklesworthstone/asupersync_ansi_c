/*
 * test_oneshot.c — unit tests for oneshot channel
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/oneshot.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void setup(void) { asx_oneshot_reset(); }

/* Suppress warn_unused_result */
static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Create tests                                                        */
/* ------------------------------------------------------------------ */

TEST(create_null_sender_fails) {
    asx_oneshot_receiver rx;
    ASSERT_EQ(asx_oneshot_create(NULL, &rx), ASX_E_INVALID_ARGUMENT);
}

TEST(create_null_receiver_fails) {
    asx_oneshot_sender tx;
    ASSERT_EQ(asx_oneshot_create(&tx, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(create_success) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    setup();
    ASSERT_EQ(asx_oneshot_create(&tx, &rx), ASX_OK);
    ASSERT_EQ(tx.slot, rx.slot);
    ASSERT_EQ(tx.generation, rx.generation);
}

/* ------------------------------------------------------------------ */
/* Send/Receive tests                                                  */
/* ------------------------------------------------------------------ */

TEST(send_and_recv) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    ASSERT_EQ(asx_oneshot_try_send(&tx, 42), ASX_OK);
    ASSERT_EQ(asx_oneshot_try_recv(&rx, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)42);
}

TEST(recv_before_send_returns_would_block) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    ASSERT_EQ(asx_oneshot_try_recv(&rx, &val), ASX_E_WOULD_BLOCK);
}

TEST(double_send_fails) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_oneshot_try_send(&tx, 1));
    /* Sender is consumed after first send */
    ASSERT_EQ(asx_oneshot_try_send(&tx, 2), ASX_E_INVALID_STATE);
}

TEST(double_recv_fails) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_oneshot_try_send(&tx, 99));
    MUST_OK(asx_oneshot_try_recv(&rx, &val));
    /* Receiver is consumed after first recv */
    ASSERT_EQ(asx_oneshot_try_recv(&rx, &val), ASX_E_INVALID_STATE);
}

/* ------------------------------------------------------------------ */
/* Disconnection tests                                                 */
/* ------------------------------------------------------------------ */

TEST(sender_drop_gives_disconnected_recv) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    asx_oneshot_sender_drop(&tx);
    ASSERT_EQ(asx_oneshot_try_recv(&rx, &val), ASX_E_DISCONNECTED);
}

TEST(receiver_drop_gives_disconnected_send) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    asx_oneshot_receiver_drop(&rx);
    ASSERT_EQ(asx_oneshot_try_send(&tx, 1), ASX_E_DISCONNECTED);
}

/* ------------------------------------------------------------------ */
/* Null argument tests                                                 */
/* ------------------------------------------------------------------ */

TEST(send_null_fails) { ASSERT_EQ(asx_oneshot_try_send(NULL, 1), ASX_E_INVALID_ARGUMENT); }

TEST(recv_null_fails) {
    uint64_t val;
    ASSERT_EQ(asx_oneshot_try_recv(NULL, &val), ASX_E_INVALID_ARGUMENT);
}

TEST(recv_null_out_fails) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    ASSERT_EQ(asx_oneshot_try_recv(&rx, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* State query tests                                                   */
/* ------------------------------------------------------------------ */

TEST(state_empty_after_create) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    ASSERT_EQ(asx_oneshot_get_state(tx.slot, tx.generation), ASX_ONESHOT_EMPTY);
}

TEST(state_filled_after_send) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_oneshot_try_send(&tx, 1));
    ASSERT_EQ(asx_oneshot_get_state(rx.slot, rx.generation), ASX_ONESHOT_FILLED);
}

TEST(state_consumed_after_recv) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_oneshot_try_send(&tx, 1));
    MUST_OK(asx_oneshot_try_recv(&rx, &val));
    ASSERT_EQ(asx_oneshot_get_state(rx.slot, rx.generation), ASX_ONESHOT_CONSUMED);
}

TEST(state_sender_dropped) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    asx_oneshot_sender_drop(&tx);
    ASSERT_EQ(asx_oneshot_get_state(tx.slot, tx.generation), ASX_ONESHOT_SENDER_DROPPED);
}

/* ------------------------------------------------------------------ */
/* Resource exhaustion                                                 */
/* ------------------------------------------------------------------ */

TEST(arena_exhaustion) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    uint32_t i;
    asx_status st;
    setup();
    for (i = 0; i < ASX_MAX_ONESHOTS; i++) {
        st = asx_oneshot_create(&tx, &rx);
        ASSERT_EQ(st, ASX_OK);
    }
    st = asx_oneshot_create(&tx, &rx);
    ASSERT_EQ(st, ASX_E_RESOURCE_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_oneshot ===\n");

    RUN_TEST(create_null_sender_fails);
    RUN_TEST(create_null_receiver_fails);
    RUN_TEST(create_success);

    RUN_TEST(send_and_recv);
    RUN_TEST(recv_before_send_returns_would_block);
    RUN_TEST(double_send_fails);
    RUN_TEST(double_recv_fails);

    RUN_TEST(sender_drop_gives_disconnected_recv);
    RUN_TEST(receiver_drop_gives_disconnected_send);

    RUN_TEST(send_null_fails);
    RUN_TEST(recv_null_fails);
    RUN_TEST(recv_null_out_fails);

    RUN_TEST(state_empty_after_create);
    RUN_TEST(state_filled_after_send);
    RUN_TEST(state_consumed_after_recv);
    RUN_TEST(state_sender_dropped);

    RUN_TEST(arena_exhaustion);

    TEST_REPORT();
    return test_failures;
}
