/*
 * test_discard.c — discard callbacks of the channels (bd-9kll.5.13)
 *
 * Rust's channels own their `T` and run its Drop for every value dropped
 * unreceived; C messages are uint64_t tokens, so each channel family has an
 * optional discard callback. These tests pin when it runs (exactly once, in
 * order) and when it does not (a received message, an eviction that hands the
 * value back, a send that failed). The recorders are file-scope because a
 * channel left alive by one test is reset by the next one's setup.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/broadcast.h>
#include <asx/core/channel.h>
#include <asx/core/oneshot.h>
#include <asx/core/watch.h>
#include <asx/runtime/runtime.h>
#include <string.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

#define REC_MAX 64u

typedef struct {
    uint64_t seen[REC_MAX];
    uint32_t n;
    asx_channel_id probe;       /* an mpsc channel whose state the call records */
    asx_channel_state at_first; /* its state at the first call */
} recorder;

static recorder g_rec;

static void rec_reset(void) { memset(&g_rec, 0, sizeof(g_rec)); }

static void rec_fn(void *ctx, uint64_t value) {
    recorder *r = (recorder *)ctx;
    if (r->n == 0u && r->probe != 0u) {
        asx_channel_state state = ASX_CHANNEL_OPEN;
        MUST_OK(asx_channel_get_state(r->probe, &state));
        r->at_first = state;
    }
    if (r->n < REC_MAX) r->seen[r->n] = value;
    r->n++;
}

static asx_region_id g_rid;

static void setup(void) {
    asx_runtime_reset();
    asx_channel_reset();
    asx_oneshot_reset();
    asx_broadcast_reset();
    asx_watch_reset();
    rec_reset();
    MUST_OK(asx_region_open(&g_rid));
}

static asx_channel_id make_channel(uint32_t capacity) {
    asx_channel_id ch = 0;
    MUST_OK(asx_channel_create(g_rid, capacity, &ch));
    MUST_OK(asx_channel_set_discard(ch, rec_fn, &g_rec));
    return ch;
}

static void put(asx_channel_id ch, uint64_t v) { MUST_OK(asx_channel_send(ch, NULL, v)); }

/* ------------------------------------------------------------------ */
/* MPSC                                                                */
/* ------------------------------------------------------------------ */

TEST(receiver_drop_discards_what_the_queue_holds_oldest_first) {
    asx_channel_id ch;
    setup();
    ch = make_channel(4);
    put(ch, 11);
    put(ch, 12);
    put(ch, 13);
    ASSERT_EQ(g_rec.n, 0u);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(g_rec.n, 3u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)11);
    ASSERT_EQ(g_rec.seen[1], (uint64_t)12);
    ASSERT_EQ(g_rec.seen[2], (uint64_t)13);
}

TEST(a_received_message_is_never_discarded) {
    asx_channel_id ch;
    uint64_t v = 0;
    setup();
    ch = make_channel(4);
    put(ch, 21);
    put(ch, 22);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)21);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(g_rec.n, 1u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)22);
}

TEST(seal_keeps_the_queue_and_the_drop_after_it_discards_the_rest) {
    asx_channel_id ch;
    uint64_t v = 0;
    setup();
    ch = make_channel(4);
    put(ch, 31);
    put(ch, 32);
    put(ch, 33);
    ASSERT_EQ(asx_channel_seal(ch), ASX_OK);
    ASSERT_EQ(g_rec.n, 0u); /* a seal drops nothing: the queue stays receivable */
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)31);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(g_rec.n, 2u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)32);
    ASSERT_EQ(g_rec.seen[1], (uint64_t)33);
}

TEST(eviction_hands_the_message_back_and_calls_nothing) {
    asx_channel_id ch;
    int evicted = 0;
    uint64_t old = 0;
    setup();
    ch = make_channel(2);
    put(ch, 41);
    put(ch, 42);
    ASSERT_EQ(asx_channel_send_evict_oldest(ch, 43, &evicted, &old), ASX_OK);
    ASSERT_EQ(evicted, 1);
    ASSERT_EQ(old, (uint64_t)41); /* the caller's to release */
    ASSERT_EQ(g_rec.n, 0u);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(g_rec.n, 2u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)42);
    ASSERT_EQ(g_rec.seen[1], (uint64_t)43);
}

TEST(a_wrapped_ring_is_discarded_in_fifo_order) {
    asx_channel_id ch;
    uint64_t v = 0;
    uint32_t i;
    setup();
    ch = make_channel(3);
    for (i = 0; i < 5u; i++) {
        put(ch, 100u + i);
        MUST_OK(asx_channel_try_recv(ch, &v)); /* the head moves round the ring */
    }
    put(ch, 201);
    put(ch, 202);
    put(ch, 203);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(g_rec.n, 3u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)201);
    ASSERT_EQ(g_rec.seen[1], (uint64_t)202);
    ASSERT_EQ(g_rec.seen[2], (uint64_t)203);
}

TEST(the_callback_sees_the_final_channel_state_and_runs_once) {
    asx_channel_id ch;
    setup();
    ch = make_channel(2);
    g_rec.probe = ch;
    put(ch, 51);
    MUST_OK(asx_channel_close_sender(ch));
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(g_rec.n, 1u);
    ASSERT_EQ(g_rec.at_first, ASX_CHANNEL_FULLY_CLOSED);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_E_INVALID_STATE); /* a second drop adds nothing */
    ASSERT_EQ(g_rec.n, 1u);
}

TEST(a_send_after_the_drop_keeps_its_value_with_the_caller) {
    asx_channel_id ch;
    setup();
    ch = make_channel(2);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 61), ASX_E_DISCONNECTED);
    ASSERT_EQ(g_rec.n, 0u);
}

TEST(no_callback_means_no_change) {
    asx_channel_id ch = 0;
    setup();
    MUST_OK(asx_channel_create(g_rid, 2, &ch));
    put(ch, 71);
    put(ch, 72);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(g_rec.n, 0u);
}

TEST(removing_the_callback_and_create_both_clear_it) {
    asx_channel_id ch;
    asx_channel_id second = 0;
    setup();
    ch = make_channel(2);
    put(ch, 81);
    MUST_OK(asx_channel_set_discard(ch, NULL, NULL));
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(g_rec.n, 0u);
    MUST_OK(asx_channel_create(g_rid, 2, &second)); /* may reuse the slot: starts clean */
    put(second, 82);
    ASSERT_EQ(asx_channel_close_receiver(second), ASX_OK);
    ASSERT_EQ(g_rec.n, 0u);
}

TEST(an_unbounded_channel_discards_everything_it_queued) {
    asx_channel_id ch = 0;
    uint32_t i;
    asx_status st;
    setup();
    st = asx_channel_create_unbounded(g_rid, &ch);
    if (st == ASX_E_PERMISSION_DENIED) return; /* a profile that forbids unbounded queues */
    ASSERT_EQ(st, ASX_OK);
    MUST_OK(asx_channel_set_discard(ch, rec_fn, &g_rec));
    for (i = 0; i < 5u; i++) put(ch, 300u + i);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(g_rec.n, 5u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)300);
    ASSERT_EQ(g_rec.seen[4], (uint64_t)304);
}

TEST(a_runtime_reset_drops_what_a_live_channel_still_queues_once) {
    asx_channel_id live;
    asx_channel_id closed;
    setup();
    live = make_channel(4);
    closed = make_channel(4);
    put(live, 91);
    put(live, 92);
    put(closed, 93);
    MUST_OK(asx_channel_close_receiver(closed)); /* 93 is discarded here */
    ASSERT_EQ(g_rec.n, 1u);
    asx_channel_reset();
    ASSERT_EQ(g_rec.n, 3u); /* 91, 92 now; 93 not again */
    ASSERT_EQ(g_rec.seen[1], (uint64_t)91);
    ASSERT_EQ(g_rec.seen[2], (uint64_t)92);
    asx_channel_reset();
    ASSERT_EQ(g_rec.n, 3u);
}

TEST(set_discard_argument_errors) {
    asx_channel_id ch;
    setup();
    ch = make_channel(2);
    ASSERT_EQ(asx_channel_set_discard(ASX_INVALID_ID, rec_fn, &g_rec), ASX_E_INVALID_ARGUMENT);
    MUST_OK(asx_channel_set_discard(ch, NULL, NULL));
    asx_channel_reset();
    ASSERT_TRUE(asx_channel_set_discard(ch, rec_fn, &g_rec) != ASX_OK); /* stale or gone */
}

/* ------------------------------------------------------------------ */
/* Oneshot                                                             */
/* ------------------------------------------------------------------ */

TEST(oneshot_receiver_drop_discards_a_sent_unreceived_value) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_oneshot_set_discard(&tx, rec_fn, &g_rec));
    MUST_OK(asx_oneshot_try_send(&tx, 401));
    ASSERT_EQ(g_rec.n, 0u);
    asx_oneshot_receiver_drop(&rx);
    ASSERT_EQ(g_rec.n, 1u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)401);
    asx_oneshot_receiver_drop(&rx); /* a second drop does nothing */
    ASSERT_EQ(g_rec.n, 1u);
}

TEST(oneshot_received_or_unsent_values_are_not_discarded) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    asx_oneshot_sender tx2;
    asx_oneshot_receiver rx2;
    uint64_t v = 0;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_oneshot_set_discard(&tx, rec_fn, &g_rec));
    MUST_OK(asx_oneshot_try_send(&tx, 411));
    ASSERT_EQ(asx_oneshot_try_recv(&rx, &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)411);
    asx_oneshot_receiver_drop(&rx);
    MUST_OK(asx_oneshot_create(&tx2, &rx2));
    MUST_OK(asx_oneshot_set_discard(&tx2, rec_fn, &g_rec));
    asx_oneshot_sender_drop(&tx2); /* nothing was ever sent */
    asx_oneshot_receiver_drop(&rx2);
    ASSERT_EQ(g_rec.n, 0u);
}

TEST(oneshot_send_to_a_dropped_receiver_keeps_the_value_with_the_caller) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_oneshot_set_discard(&tx, rec_fn, &g_rec));
    asx_oneshot_receiver_drop(&rx);
    ASSERT_EQ(asx_oneshot_try_send(&tx, 421), ASX_E_DISCONNECTED);
    ASSERT_EQ(g_rec.n, 0u);
}

TEST(oneshot_reset_drops_a_value_a_live_channel_still_holds) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_oneshot_set_discard(&tx, rec_fn, &g_rec));
    MUST_OK(asx_oneshot_try_send(&tx, 431));
    asx_oneshot_reset();
    ASSERT_EQ(g_rec.n, 1u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)431);
}

/* ------------------------------------------------------------------ */
/* Broadcast                                                           */
/* ------------------------------------------------------------------ */

TEST(broadcast_overwrite_drops_the_message_that_falls_off_the_ring) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    uint32_t reached = 0;
    setup();
    MUST_OK(asx_broadcast_create(2, &tx, &rx));
    MUST_OK(asx_broadcast_set_discard(&tx, rec_fn, &g_rec));
    MUST_OK(asx_broadcast_send(&tx, NULL, 1, &reached));
    MUST_OK(asx_broadcast_send(&tx, NULL, 2, &reached));
    ASSERT_EQ(g_rec.n, 0u); /* the ring still has room */
    MUST_OK(asx_broadcast_send(&tx, NULL, 3, &reached));
    ASSERT_EQ(g_rec.n, 1u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)1);
    MUST_OK(asx_broadcast_send(&tx, NULL, 4, &reached));
    ASSERT_EQ(g_rec.n, 2u);
    ASSERT_EQ(g_rec.seen[1], (uint64_t)2);
}

TEST(broadcast_last_receiver_drop_takes_the_whole_buffer_once) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    asx_broadcast_receiver rx2;
    uint32_t reached = 0;
    setup();
    MUST_OK(asx_broadcast_create(2, &tx, &rx));
    MUST_OK(asx_broadcast_subscribe(&tx, &rx2));
    MUST_OK(asx_broadcast_set_discard(&tx, rec_fn, &g_rec));
    MUST_OK(asx_broadcast_send(&tx, NULL, 1, &reached));
    MUST_OK(asx_broadcast_send(&tx, NULL, 2, &reached));
    MUST_OK(asx_broadcast_send(&tx, NULL, 3, &reached)); /* 1 falls off: one call */
    ASSERT_EQ(g_rec.n, 1u);
    asx_broadcast_receiver_drop(&rx);
    ASSERT_EQ(g_rec.n, 1u); /* a receiver is still there */
    asx_broadcast_receiver_drop(&rx2);
    ASSERT_EQ(g_rec.n, 3u); /* the buffered 2, 3, oldest first */
    ASSERT_EQ(g_rec.seen[1], (uint64_t)2);
    ASSERT_EQ(g_rec.seen[2], (uint64_t)3);
    asx_broadcast_receiver_drop(&rx2);
    ASSERT_EQ(g_rec.n, 3u);
}

TEST(broadcast_messages_dropped_with_the_buffer_are_not_dropped_again_on_overwrite) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    asx_broadcast_receiver again;
    uint32_t reached = 0;
    setup();
    MUST_OK(asx_broadcast_create(2, &tx, &rx));
    MUST_OK(asx_broadcast_set_discard(&tx, rec_fn, &g_rec));
    MUST_OK(asx_broadcast_send(&tx, NULL, 1, &reached));
    MUST_OK(asx_broadcast_send(&tx, NULL, 2, &reached));
    asx_broadcast_receiver_drop(&rx); /* 1 and 2 go with the buffer */
    ASSERT_EQ(g_rec.n, 2u);
    MUST_OK(asx_broadcast_subscribe(&tx, &again));
    MUST_OK(asx_broadcast_send(&tx, NULL, 3, &reached)); /* its slot held a dropped message */
    MUST_OK(asx_broadcast_send(&tx, NULL, 4, &reached));
    ASSERT_EQ(g_rec.n, 2u);
    MUST_OK(asx_broadcast_send(&tx, NULL, 5, &reached)); /* now 3 falls off */
    ASSERT_EQ(g_rec.n, 3u);
    ASSERT_EQ(g_rec.seen[2], (uint64_t)3);
}

TEST(broadcast_send_that_reaches_no_receiver_drops_its_message) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    asx_broadcast_permit permit;
    uint32_t reached = 7;
    setup();
    MUST_OK(asx_broadcast_create(2, &tx, &rx));
    MUST_OK(asx_broadcast_set_discard(&tx, rec_fn, &g_rec));
    MUST_OK(asx_broadcast_reserve(&tx, NULL, &permit));
    asx_broadcast_receiver_drop(&rx); /* nothing buffered, nothing dropped */
    ASSERT_EQ(g_rec.n, 0u);
    ASSERT_EQ(asx_broadcast_permit_send(&permit, 55, &reached), ASX_OK);
    ASSERT_EQ(reached, 0u);
    ASSERT_EQ(g_rec.n, 1u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)55);
}

TEST(broadcast_reset_drops_the_messages_a_live_ring_owns) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    uint32_t reached = 0;
    setup();
    MUST_OK(asx_broadcast_create(4, &tx, &rx));
    MUST_OK(asx_broadcast_set_discard(&tx, rec_fn, &g_rec));
    MUST_OK(asx_broadcast_send(&tx, NULL, 61, &reached));
    MUST_OK(asx_broadcast_send(&tx, NULL, 62, &reached));
    asx_broadcast_reset();
    ASSERT_EQ(g_rec.n, 2u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)61);
    ASSERT_EQ(g_rec.seen[1], (uint64_t)62);
}

/* ------------------------------------------------------------------ */
/* Watch                                                               */
/* ------------------------------------------------------------------ */

static void bump(void *ctx, uint64_t *value) {
    (void)ctx;
    *value += 1u;
}

static void leave(void *ctx, uint64_t *value) {
    (void)ctx;
    (void)value;
}

TEST(watch_send_drops_the_value_it_replaces) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    setup();
    MUST_OK(asx_watch_create(10, &tx, &rx));
    MUST_OK(asx_watch_set_discard(&tx, rec_fn, &g_rec));
    MUST_OK(asx_watch_send(&tx, 20));
    MUST_OK(asx_watch_send(&tx, 30));
    ASSERT_EQ(g_rec.n, 2u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)10); /* the initial value too */
    ASSERT_EQ(g_rec.seen[1], (uint64_t)20);
}

TEST(watch_send_modify_drops_the_old_value_unless_the_token_is_unchanged) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    setup();
    MUST_OK(asx_watch_create(40, &tx, &rx));
    MUST_OK(asx_watch_set_discard(&tx, rec_fn, &g_rec));
    MUST_OK(asx_watch_send_modify(&tx, bump, NULL));
    ASSERT_EQ(g_rec.n, 1u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)40);
    MUST_OK(asx_watch_send_modify(&tx, leave, NULL)); /* a new version, the same token */
    MUST_OK(asx_watch_send(&tx, 41));                 /* sending the held token again */
    ASSERT_EQ(g_rec.n, 1u);
}

TEST(watch_last_value_is_dropped_when_no_sender_and_no_receiver_is_left) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    asx_watch_receiver clone;
    setup();
    MUST_OK(asx_watch_create(50, &tx, &rx));
    MUST_OK(asx_watch_receiver_clone(&rx, &clone));
    MUST_OK(asx_watch_set_discard(&tx, rec_fn, &g_rec));
    asx_watch_sender_drop(&tx);
    asx_watch_receiver_drop(&rx);
    ASSERT_EQ(g_rec.n, 0u); /* the clone still reads it */
    asx_watch_receiver_drop(&clone);
    ASSERT_EQ(g_rec.n, 1u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)50);
    asx_watch_receiver_drop(&clone); /* a repeated drop adds nothing */
    asx_watch_sender_drop(&tx);
    ASSERT_EQ(g_rec.n, 1u);
}

TEST(watch_with_no_receiver_keeps_its_value_while_the_sender_lives) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    setup();
    MUST_OK(asx_watch_create(60, &tx, &rx));
    MUST_OK(asx_watch_set_discard(&tx, rec_fn, &g_rec));
    asx_watch_receiver_drop(&rx);
    ASSERT_EQ(g_rec.n, 0u);
    MUST_OK(asx_watch_send(&tx, 61)); /* only what it replaces */
    ASSERT_EQ(g_rec.n, 1u);
    ASSERT_EQ(g_rec.seen[0], (uint64_t)60);
}

TEST(watch_reset_drops_the_value_a_live_channel_holds_once) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    asx_watch_sender tx2;
    asx_watch_receiver rx2;
    setup();
    MUST_OK(asx_watch_create(70, &tx, &rx));
    MUST_OK(asx_watch_set_discard(&tx, rec_fn, &g_rec));
    MUST_OK(asx_watch_create(80, &tx2, &rx2));
    MUST_OK(asx_watch_set_discard(&tx2, rec_fn, &g_rec));
    asx_watch_sender_drop(&tx2);
    asx_watch_receiver_drop(&rx2); /* 80 is dropped here, with its channel */
    ASSERT_EQ(g_rec.n, 1u);
    asx_watch_reset();
    ASSERT_EQ(g_rec.n, 2u); /* 70 only */
    ASSERT_EQ(g_rec.seen[1], (uint64_t)70);
}

TEST(watch_set_discard_argument_errors) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    setup();
    MUST_OK(asx_watch_create(0, &tx, &rx));
    ASSERT_EQ(asx_watch_set_discard(NULL, rec_fn, &g_rec), ASX_E_INVALID_ARGUMENT);
    asx_watch_reset();
    ASSERT_EQ(asx_watch_set_discard(&tx, rec_fn, &g_rec), ASX_E_NOT_FOUND);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_discard ===\n");

    RUN_TEST(receiver_drop_discards_what_the_queue_holds_oldest_first);
    RUN_TEST(a_received_message_is_never_discarded);
    RUN_TEST(seal_keeps_the_queue_and_the_drop_after_it_discards_the_rest);
    RUN_TEST(eviction_hands_the_message_back_and_calls_nothing);
    RUN_TEST(a_wrapped_ring_is_discarded_in_fifo_order);
    RUN_TEST(the_callback_sees_the_final_channel_state_and_runs_once);
    RUN_TEST(a_send_after_the_drop_keeps_its_value_with_the_caller);
    RUN_TEST(no_callback_means_no_change);
    RUN_TEST(removing_the_callback_and_create_both_clear_it);
    RUN_TEST(an_unbounded_channel_discards_everything_it_queued);
    RUN_TEST(a_runtime_reset_drops_what_a_live_channel_still_queues_once);
    RUN_TEST(set_discard_argument_errors);

    RUN_TEST(oneshot_receiver_drop_discards_a_sent_unreceived_value);
    RUN_TEST(oneshot_received_or_unsent_values_are_not_discarded);
    RUN_TEST(oneshot_send_to_a_dropped_receiver_keeps_the_value_with_the_caller);
    RUN_TEST(oneshot_reset_drops_a_value_a_live_channel_still_holds);

    RUN_TEST(broadcast_overwrite_drops_the_message_that_falls_off_the_ring);
    RUN_TEST(broadcast_last_receiver_drop_takes_the_whole_buffer_once);
    RUN_TEST(broadcast_messages_dropped_with_the_buffer_are_not_dropped_again_on_overwrite);
    RUN_TEST(broadcast_send_that_reaches_no_receiver_drops_its_message);
    RUN_TEST(broadcast_reset_drops_the_messages_a_live_ring_owns);

    RUN_TEST(watch_send_drops_the_value_it_replaces);
    RUN_TEST(watch_send_modify_drops_the_old_value_unless_the_token_is_unchanged);
    RUN_TEST(watch_last_value_is_dropped_when_no_sender_and_no_receiver_is_left);
    RUN_TEST(watch_with_no_receiver_keeps_its_value_while_the_sender_lives);
    RUN_TEST(watch_reset_drops_the_value_a_live_channel_holds_once);
    RUN_TEST(watch_set_discard_argument_errors);

    TEST_REPORT();
    return test_failures;
}
