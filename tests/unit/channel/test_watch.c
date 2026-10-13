/*
 * test_watch.c — unit tests for watch channel
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/watch.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

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

/* C4 regression: the initial value is version 0 and the first receiver
 * has seen it, so nothing has changed yet (Rust channel/watch.rs). The C
 * port used to start at version 1, reporting a change that never
 * happened to every new receiver. */
TEST(has_changed_initially_false) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    setup();
    MUST_OK(asx_watch_create(1, &tx, &rx));
    ASSERT_FALSE(asx_watch_has_changed(&rx));
    ASSERT_EQ(asx_watch_poll_changed(&rx, NULL), ASX_E_PENDING);
    MUST_OK(asx_watch_send(&tx, 2));
    ASSERT_TRUE(asx_watch_has_changed(&rx));
}

TEST(subscribe_sees_only_later_changes) {
    asx_watch_sender tx;
    asx_watch_receiver rx1, rx2;
    uint64_t val;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx1));
    MUST_OK(asx_watch_send(&tx, 1));
    MUST_OK(asx_watch_send(&tx, 2));
    MUST_OK(asx_watch_send(&tx, 3));
    /* A late subscriber has seen the current version */
    MUST_OK(asx_watch_subscribe(&tx, &rx2));
    ASSERT_FALSE(asx_watch_has_changed(&rx2));
    MUST_OK(asx_watch_recv(&rx2, &val));
    ASSERT_EQ(val, (uint64_t)3); /* but can still read the current value */
    /* The original receiver still sees the unread sends as a change */
    ASSERT_TRUE(asx_watch_has_changed(&rx1));
    MUST_OK(asx_watch_send(&tx, 4));
    ASSERT_TRUE(asx_watch_has_changed(&rx2));
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
    MUST_OK(asx_watch_recv(&rx, &val)); /* read initial */
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
    ASSERT_EQ(val, (uint64_t)3); /* only latest survives */
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
    MUST_OK(asx_watch_recv(&rx1, &val)); /* rx1 reads */
    ASSERT_FALSE(asx_watch_has_changed(&rx1));
    ASSERT_TRUE(asx_watch_has_changed(&rx2)); /* rx2 hasn't read yet */
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
/* borrow vs borrow_and_update, mark_seen (watch.rs:904, :917, :961)   */
/* ------------------------------------------------------------------ */

TEST(borrow_reads_without_marking_the_value_seen) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(1, &tx, &rx));
    MUST_OK(asx_watch_send(&tx, 2));
    MUST_OK(asx_watch_borrow(&rx, &val));
    ASSERT_EQ(val, (uint64_t)2);
    ASSERT_TRUE(asx_watch_has_changed(&rx));
    ASSERT_EQ(rx.last_seen_version, (uint64_t)0);
    ASSERT_EQ(asx_watch_poll_changed(&rx, NULL), ASX_OK); /* the change is still there */
}

TEST(borrow_and_update_marks_the_value_seen) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(1, &tx, &rx));
    MUST_OK(asx_watch_send(&tx, 2));
    MUST_OK(asx_watch_recv(&rx, &val));
    ASSERT_EQ(val, (uint64_t)2);
    ASSERT_FALSE(asx_watch_has_changed(&rx));
    ASSERT_EQ(asx_watch_poll_changed(&rx, NULL), ASX_E_PENDING);
}

TEST(mark_seen_acknowledges_the_latest_version_not_the_borrowed_one) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    MUST_OK(asx_watch_send(&tx, 1));
    MUST_OK(asx_watch_borrow(&rx, &val)); /* sees 1 */
    MUST_OK(asx_watch_send(&tx, 2));      /* arrives after the borrow */
    MUST_OK(asx_watch_mark_seen(&rx));    /* acknowledges 2 as well */
    ASSERT_FALSE(asx_watch_has_changed(&rx));
    ASSERT_EQ(asx_watch_poll_changed(&rx, NULL), ASX_E_PENDING);
    MUST_OK(asx_watch_send(&tx, 3));
    ASSERT_TRUE(asx_watch_has_changed(&rx));
}

TEST(borrow_and_mark_seen_argument_errors) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    ASSERT_EQ(asx_watch_borrow(NULL, &val), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_watch_borrow(&rx, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_watch_mark_seen(NULL), ASX_E_INVALID_ARGUMENT);
    asx_watch_reset();
    ASSERT_EQ(asx_watch_borrow(&rx, &val), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_watch_mark_seen(&rx), ASX_E_NOT_FOUND);
}

/* ------------------------------------------------------------------ */
/* send_modify (watch.rs:530)                                          */
/* ------------------------------------------------------------------ */

static void add_five(void *ctx, uint64_t *value) {
    (void)ctx;
    *value += 5u;
}

static void leave_alone(void *ctx, uint64_t *value) {
    (void)value;
    (*(int *)ctx)++;
}

TEST(send_modify_applies_in_place_as_one_version) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(10, &tx, &rx));
    MUST_OK(asx_watch_send_modify(&tx, add_five, NULL));
    MUST_OK(asx_watch_send_modify(&tx, add_five, NULL));
    ASSERT_TRUE(asx_watch_has_changed(&rx));
    MUST_OK(asx_watch_recv(&rx, &val));
    ASSERT_EQ(val, (uint64_t)20);
    ASSERT_EQ(rx.last_seen_version, (uint64_t)2);
}

TEST(send_modify_that_changes_nothing_still_publishes_a_version) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    int calls = 0;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(7, &tx, &rx));
    MUST_OK(asx_watch_send_modify(&tx, leave_alone, &calls));
    ASSERT_EQ(calls, 1);
    ASSERT_TRUE(asx_watch_has_changed(&rx));
    ASSERT_EQ(asx_watch_poll_changed(&rx, NULL), ASX_OK);
    MUST_OK(asx_watch_borrow(&rx, &val));
    ASSERT_EQ(val, (uint64_t)7);
}

TEST(send_modify_after_the_sender_dropped_is_refused) {
    asx_watch_sender tx;
    asx_watch_sender stale;
    asx_watch_receiver rx;
    int calls = 0;
    setup();
    MUST_OK(asx_watch_create(7, &tx, &rx));
    stale = tx;
    asx_watch_sender_drop(&tx);
    ASSERT_EQ(asx_watch_send_modify(&stale, leave_alone, &calls), ASX_E_INVALID_STATE);
    ASSERT_EQ(calls, 0); /* the closure never ran */
    ASSERT_FALSE(asx_watch_has_changed(&rx));
}

TEST(send_modify_argument_errors) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    ASSERT_EQ(asx_watch_send_modify(NULL, add_five, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_watch_send_modify(&tx, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_FALSE(asx_watch_has_changed(&rx));
}

/* ------------------------------------------------------------------ */
/* Receiver clone (watch.rs:1057)                                      */
/* ------------------------------------------------------------------ */

TEST(receiver_clone_continues_from_the_same_version) {
    asx_watch_sender tx;
    asx_watch_receiver rx1, clone, fresh;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx1));
    MUST_OK(asx_watch_send(&tx, 1));
    MUST_OK(asx_watch_recv(&rx1, &val)); /* rx1 has seen version 1 */
    MUST_OK(asx_watch_send(&tx, 2));
    MUST_OK(asx_watch_receiver_clone(&rx1, &clone));
    MUST_OK(asx_watch_subscribe(&tx, &fresh));
    ASSERT_EQ(asx_watch_receiver_count(&tx), 3u);
    ASSERT_EQ(clone.last_seen_version, rx1.last_seen_version);
    ASSERT_TRUE(asx_watch_has_changed(&clone));  /* continues: 2 is news to it */
    ASSERT_FALSE(asx_watch_has_changed(&fresh)); /* a subscriber starts at the current one */
    ASSERT_EQ(asx_watch_poll_changed(&clone, NULL), ASX_OK);
    ASSERT_TRUE(asx_watch_has_changed(&rx1)); /* the clone's read is its own */
}

TEST(receiver_clone_of_an_unread_receiver_has_the_same_unread_change) {
    asx_watch_sender tx;
    asx_watch_receiver rx, clone;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    MUST_OK(asx_watch_send(&tx, 9));
    MUST_OK(asx_watch_receiver_clone(&rx, &clone));
    ASSERT_TRUE(asx_watch_has_changed(&rx));
    ASSERT_TRUE(asx_watch_has_changed(&clone));
}

TEST(receiver_clone_survives_the_sender_and_obeys_the_limit) {
    asx_watch_sender tx;
    asx_watch_receiver rx, clone, more[ASX_WATCH_MAX_RECEIVERS];
    uint32_t i;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    asx_watch_sender_drop(&tx);
    MUST_OK(asx_watch_receiver_clone(&rx, &clone)); /* allowed: Receiver::clone needs no sender */
    ASSERT_TRUE(asx_watch_receiver_is_closed(&clone));
    for (i = 2u; i < ASX_WATCH_MAX_RECEIVERS; i++) {
        MUST_OK(asx_watch_receiver_clone(&rx, &more[i]));
    }
    ASSERT_EQ(asx_watch_receiver_clone(&rx, &more[0]), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_watch_receiver_clone(NULL, &more[0]), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_watch_receiver_clone(&rx, NULL), ASX_E_INVALID_ARGUMENT);
    asx_watch_reset();
    ASSERT_EQ(asx_watch_receiver_clone(&rx, &more[0]), ASX_E_NOT_FOUND);
}

/* ------------------------------------------------------------------ */
/* is_closed on both ends (watch.rs:634, :977)                         */
/* ------------------------------------------------------------------ */

TEST(is_closed_reports_the_other_end_gone) {
    asx_watch_sender tx;
    asx_watch_receiver rx, rx2;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    MUST_OK(asx_watch_subscribe(&tx, &rx2));
    ASSERT_FALSE(asx_watch_sender_is_closed(&tx));
    ASSERT_FALSE(asx_watch_receiver_is_closed(&rx));
    asx_watch_receiver_drop(&rx);
    ASSERT_FALSE(asx_watch_sender_is_closed(&tx)); /* one receiver is left */
    asx_watch_receiver_drop(&rx2);
    ASSERT_TRUE(asx_watch_sender_is_closed(&tx));
    ASSERT_FALSE(asx_watch_receiver_is_closed(&rx)); /* the sender is still there */
    asx_watch_sender_drop(&tx);
    ASSERT_TRUE(asx_watch_receiver_is_closed(&rx));
}

TEST(is_closed_of_a_null_or_stale_handle_reads_as_closed) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    ASSERT_TRUE(asx_watch_receiver_is_closed(NULL));
    ASSERT_TRUE(asx_watch_sender_is_closed(NULL));
    asx_watch_reset();
    ASSERT_TRUE(asx_watch_receiver_is_closed(&rx));
    ASSERT_TRUE(asx_watch_sender_is_closed(&tx));
}

/* ------------------------------------------------------------------ */
/* wait_for (watch.rs:858)                                             */
/* ------------------------------------------------------------------ */

static int at_least(void *ctx, uint64_t value) { return value >= *(const uint64_t *)ctx; }

TEST(wait_for_returns_at_once_when_the_value_already_matches) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    asx_watch_wait w;
    uint64_t want = 5u;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    MUST_OK(asx_watch_send(&tx, 6));
    memset(&w, 0, sizeof(w));
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, at_least, &want, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)6);
    ASSERT_FALSE(asx_watch_has_changed(&rx)); /* the accepted version is marked seen */
    ASSERT_EQ(asx_watch_receiver_count(&tx), 1u);
}

TEST(wait_for_waits_through_rejected_versions_on_a_clone) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    asx_watch_wait w;
    uint64_t want = 3u;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    memset(&w, 0, sizeof(w));
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, at_least, &want, &val), ASX_E_PENDING);
    ASSERT_EQ(asx_watch_receiver_count(&tx), 2u); /* Rust waits on self.clone() */
    MUST_OK(asx_watch_send(&tx, 1));
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, at_least, &want, &val), ASX_E_PENDING);
    MUST_OK(asx_watch_send(&tx, 2));
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, at_least, &want, &val), ASX_E_PENDING);
    ASSERT_TRUE(asx_watch_has_changed(&rx)); /* the receiver itself has seen nothing */
    MUST_OK(asx_watch_send(&tx, 3));
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, at_least, &want, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)3);
    ASSERT_FALSE(asx_watch_has_changed(&rx));
    ASSERT_EQ(asx_watch_receiver_count(&tx), 1u); /* the clone is gone */
}

TEST(wait_for_checks_the_value_current_when_it_wakes_not_each_send) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    asx_watch_wait w;
    uint64_t want = 4u;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    memset(&w, 0, sizeof(w));
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, at_least, &want, &val), ASX_E_PENDING);
    MUST_OK(asx_watch_send(&tx, 4)); /* matches, but replaced before the next poll */
    MUST_OK(asx_watch_send(&tx, 1));
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, at_least, &want, &val), ASX_E_PENDING);
    MUST_OK(asx_watch_send(&tx, 8));
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, at_least, &want, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)8);
}

TEST(wait_for_sender_drop_reports_a_final_match_first_then_disconnects) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    asx_watch_receiver rx2;
    asx_watch_wait w;
    asx_watch_wait w2;
    uint64_t want = 3u;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    MUST_OK(asx_watch_subscribe(&tx, &rx2));
    memset(&w, 0, sizeof(w));
    memset(&w2, 0, sizeof(w2));
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, at_least, &want, &val), ASX_E_PENDING);
    ASSERT_EQ(asx_watch_poll_wait_for(&rx2, NULL, &w2, at_least, &want, &val), ASX_E_PENDING);
    MUST_OK(asx_watch_send(&tx, 3)); /* a last value that matches ... */
    asx_watch_sender_drop(&tx);      /* ... then the sender is gone */
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, at_least, &want, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)3);
    want = 9u; /* nothing will ever match this */
    ASSERT_EQ(asx_watch_poll_wait_for(&rx2, NULL, &w2, at_least, &want, &val), ASX_E_DISCONNECTED);
    ASSERT_EQ(w2.waiting, 0); /* the wait is spent */
}

TEST(wait_for_argument_errors) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    asx_watch_wait w;
    uint64_t want = 0u;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    memset(&w, 0, sizeof(w));
    ASSERT_EQ(asx_watch_poll_wait_for(NULL, NULL, &w, at_least, &want, &val),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, NULL, at_least, &want, &val),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, NULL, &want, &val), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_watch_poll_wait_for(&rx, NULL, &w, at_least, &want, NULL),
              ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Null safety                                                         */
/* ------------------------------------------------------------------ */

TEST(has_changed_null_false) { ASSERT_FALSE(asx_watch_has_changed(NULL)); }

TEST(receiver_count_null_zero) { ASSERT_EQ(asx_watch_receiver_count(NULL), 0u); }

TEST(recv_null_fails) {
    uint64_t val;
    ASSERT_EQ(asx_watch_recv(NULL, &val), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_watch ===\n");

    RUN_TEST(create_null_sender_fails);
    RUN_TEST(create_null_receiver_fails);
    RUN_TEST(create_success);

    RUN_TEST(read_initial_value);
    RUN_TEST(has_changed_initially_false);
    RUN_TEST(subscribe_sees_only_later_changes);
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

    RUN_TEST(borrow_reads_without_marking_the_value_seen);
    RUN_TEST(borrow_and_update_marks_the_value_seen);
    RUN_TEST(mark_seen_acknowledges_the_latest_version_not_the_borrowed_one);
    RUN_TEST(borrow_and_mark_seen_argument_errors);

    RUN_TEST(send_modify_applies_in_place_as_one_version);
    RUN_TEST(send_modify_that_changes_nothing_still_publishes_a_version);
    RUN_TEST(send_modify_after_the_sender_dropped_is_refused);
    RUN_TEST(send_modify_argument_errors);

    RUN_TEST(receiver_clone_continues_from_the_same_version);
    RUN_TEST(receiver_clone_of_an_unread_receiver_has_the_same_unread_change);
    RUN_TEST(receiver_clone_survives_the_sender_and_obeys_the_limit);

    RUN_TEST(is_closed_reports_the_other_end_gone);
    RUN_TEST(is_closed_of_a_null_or_stale_handle_reads_as_closed);

    RUN_TEST(wait_for_returns_at_once_when_the_value_already_matches);
    RUN_TEST(wait_for_waits_through_rejected_versions_on_a_clone);
    RUN_TEST(wait_for_checks_the_value_current_when_it_wakes_not_each_send);
    RUN_TEST(wait_for_sender_drop_reports_a_final_match_first_then_disconnects);
    RUN_TEST(wait_for_argument_errors);

    RUN_TEST(has_changed_null_false);
    RUN_TEST(receiver_count_null_zero);
    RUN_TEST(recv_null_fails);

    TEST_REPORT();
    return test_failures;
}
