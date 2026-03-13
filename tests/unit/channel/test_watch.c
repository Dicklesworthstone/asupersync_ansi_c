/*
 * test_watch.c — unit tests for watch channel
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/watch.h>

static asx_status st_sink_;
#define MUST_OK(expr) do { st_sink_ = (expr); (void)st_sink_; } while(0)

static void setup(void) { asx_watch_reset(); }

/* ------------------------------------------------------------------ */
/* Create tests                                                        */
/* ------------------------------------------------------------------ */

TEST(create_null_sender_fails) {
    asx_watch_receiver rx;
    ASSERT_EQ(asx_watch_create(0, NULL, &rx), ASX_E_INVALID_ARGUMENT);
}

TEST(create_null_receiver_fails) {
    asx_watch_sender tx;
    ASSERT_EQ(asx_watch_create(0, &tx, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(create_success) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    setup();
    ASSERT_EQ(asx_watch_create(42, &tx, &rx), ASX_OK);
    ASSERT_EQ(tx.slot, rx.slot);
    ASSERT_EQ(asx_watch_receiver_count(&tx), 1u);
}

/* ------------------------------------------------------------------ */
/* Read initial value                                                  */
/* ------------------------------------------------------------------ */

TEST(read_initial_value) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_watch_create(100, &tx, &rx));
    MUST_OK(asx_watch_recv(&rx, &val));
    ASSERT_EQ(val, (uint64_t)100);
}

TEST(has_changed_initially_true) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    setup();
    MUST_OK(asx_watch_create(1, &tx, &rx));
    /* Receiver hasn't read yet, so value is "new" */
    ASSERT_TRUE(asx_watch_has_changed(&rx));
}

TEST(has_changed_false_after_read) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_watch_create(1, &tx, &rx));
    MUST_OK(asx_watch_recv(&rx, &val));
    ASSERT_FALSE(asx_watch_has_changed(&rx));
}

/* ------------------------------------------------------------------ */
/* Send and read tests                                                 */
/* ------------------------------------------------------------------ */

TEST(send_updates_value) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    MUST_OK(asx_watch_recv(&rx, &val));  /* read initial */
    MUST_OK(asx_watch_send(&tx, 77));
    ASSERT_TRUE(asx_watch_has_changed(&rx));
    MUST_OK(asx_watch_recv(&rx, &val));
    ASSERT_EQ(val, (uint64_t)77);
    ASSERT_FALSE(asx_watch_has_changed(&rx));
}

TEST(latest_value_wins) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    MUST_OK(asx_watch_send(&tx, 1));
    MUST_OK(asx_watch_send(&tx, 2));
    MUST_OK(asx_watch_send(&tx, 3));
    MUST_OK(asx_watch_recv(&rx, &val));
    ASSERT_EQ(val, (uint64_t)3);  /* only latest survives */
}

/* ------------------------------------------------------------------ */
/* Multiple receivers                                                  */
/* ------------------------------------------------------------------ */

TEST(subscribe_creates_receiver) {
    asx_watch_sender tx;
    asx_watch_receiver rx1, rx2;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx1));
    MUST_OK(asx_watch_subscribe(&tx, &rx2));
    ASSERT_EQ(asx_watch_receiver_count(&tx), 2u);
}

TEST(multiple_receivers_see_same_value) {
    asx_watch_sender tx;
    asx_watch_receiver rx1, rx2;
    uint64_t v1, v2;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx1));
    MUST_OK(asx_watch_subscribe(&tx, &rx2));
    MUST_OK(asx_watch_send(&tx, 55));
    MUST_OK(asx_watch_recv(&rx1, &v1));
    MUST_OK(asx_watch_recv(&rx2, &v2));
    ASSERT_EQ(v1, (uint64_t)55);
    ASSERT_EQ(v2, (uint64_t)55);
}

TEST(independent_changed_tracking) {
    asx_watch_sender tx;
    asx_watch_receiver rx1, rx2;
    uint64_t val;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx1));
    MUST_OK(asx_watch_subscribe(&tx, &rx2));
    MUST_OK(asx_watch_send(&tx, 10));
    MUST_OK(asx_watch_recv(&rx1, &val));  /* rx1 reads */
    ASSERT_FALSE(asx_watch_has_changed(&rx1));
    ASSERT_TRUE(asx_watch_has_changed(&rx2));  /* rx2 hasn't read yet */
}

/* ------------------------------------------------------------------ */
/* Drop semantics                                                      */
/* ------------------------------------------------------------------ */

TEST(sender_drop_allows_last_read) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    uint64_t val;
    setup();
    MUST_OK(asx_watch_create(42, &tx, &rx));
    asx_watch_sender_drop(&tx);
    /* Receiver can still read the last value */
    MUST_OK(asx_watch_recv(&rx, &val));
    ASSERT_EQ(val, (uint64_t)42);
}

TEST(subscribe_after_sender_drop_fails) {
    asx_watch_sender tx;
    asx_watch_receiver rx, rx2;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    asx_watch_sender_drop(&tx);
    ASSERT_EQ(asx_watch_subscribe(&tx, &rx2), ASX_E_DISCONNECTED);
}

TEST(send_after_drop_fails) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    asx_watch_sender_drop(&tx);
    ASSERT_EQ(asx_watch_send(&tx, 1), ASX_E_INVALID_STATE);
}

TEST(receiver_drop_decrements_count) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    ASSERT_EQ(asx_watch_receiver_count(&tx), 1u);
    asx_watch_receiver_drop(&rx);
    ASSERT_EQ(asx_watch_receiver_count(&tx), 0u);
}

/* ------------------------------------------------------------------ */
/* Null safety                                                         */
/* ------------------------------------------------------------------ */

TEST(has_changed_null_false) {
    ASSERT_FALSE(asx_watch_has_changed(NULL));
}

TEST(receiver_count_null_zero) {
    ASSERT_EQ(asx_watch_receiver_count(NULL), 0u);
}

TEST(recv_null_fails) {
    uint64_t val;
    ASSERT_EQ(asx_watch_recv(NULL, &val), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== test_watch ===\n");

    RUN_TEST(create_null_sender_fails);
    RUN_TEST(create_null_receiver_fails);
    RUN_TEST(create_success);

    RUN_TEST(read_initial_value);
    RUN_TEST(has_changed_initially_true);
    RUN_TEST(has_changed_false_after_read);

    RUN_TEST(send_updates_value);
    RUN_TEST(latest_value_wins);

    RUN_TEST(subscribe_creates_receiver);
    RUN_TEST(multiple_receivers_see_same_value);
    RUN_TEST(independent_changed_tracking);

    RUN_TEST(sender_drop_allows_last_read);
    RUN_TEST(subscribe_after_sender_drop_fails);
    RUN_TEST(send_after_drop_fails);
    RUN_TEST(receiver_drop_decrements_count);

    RUN_TEST(has_changed_null_false);
    RUN_TEST(receiver_count_null_zero);
    RUN_TEST(recv_null_fails);

    TEST_REPORT();
    return test_failures;
}
