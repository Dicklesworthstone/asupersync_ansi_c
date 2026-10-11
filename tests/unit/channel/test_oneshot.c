/*
 * test_oneshot.c — unit tests for oneshot channel
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/oneshot.h>
#include <asx/runtime/runtime.h>

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
/* Reserve and permits (Rust Sender::reserve, SendPermit)              */
/* ------------------------------------------------------------------ */

TEST(reserve_consumes_the_sender_until_the_permit_sends) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    asx_oneshot_permit p;
    asx_oneshot_permit again;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    ASSERT_EQ(asx_oneshot_reserve(&tx, NULL, &p), ASX_OK);
    ASSERT_EQ(p.obligation, ASX_INVALID_ID); /* no Cx: untracked */
    ASSERT_EQ(asx_oneshot_try_send(&tx, 1), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_oneshot_reserve(&tx, NULL, &again), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_oneshot_reserve(&tx, NULL, &p), ASX_E_INVALID_STATE);
    ASSERT_EQ(p.live, 1);         /* the refused reserve left the live permit alone */
    asx_oneshot_sender_drop(&tx); /* the permit holds the sending side */
    ASSERT_EQ(asx_oneshot_try_recv(&rx, &val), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_oneshot_permit_send(&p, 7), ASX_OK);
    ASSERT_EQ(asx_oneshot_permit_send(&p, 8), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_oneshot_try_recv(&rx, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)7);
}

TEST(permit_abort_closes_the_channel) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    asx_oneshot_permit p;
    uint64_t val = 0;
    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_oneshot_reserve(&tx, NULL, &p));
    asx_oneshot_permit_abort(&p);
    ASSERT_EQ(asx_oneshot_try_recv(&rx, &val), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_oneshot_permit_send(&p, 1), ASX_E_INVALID_STATE);
}

/* A task (never polled) whose Cx holds obligations in its region. */
static asx_status poll_idle(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_PENDING;
}

static asx_task_id cx_task(asx_cx *cx) {
    asx_region_id r = ASX_INVALID_ID;
    asx_task_id t = ASX_INVALID_ID;
    asx_runtime_reset();
    setup();
    MUST_OK(asx_region_open(&r));
    MUST_OK(asx_task_spawn(r, poll_idle, NULL, &t));
    MUST_OK(asx_cx_init(cx, r, t, ASX_CAP_CANCEL_CHECK));
    return t;
}

static int obligation_is(asx_obligation_id id, asx_obligation_state state,
                         asx_obligation_abort_reason reason) {
    asx_obligation_info info;
    if (id == ASX_INVALID_ID || asx_obligation_get_info(id, &info) != ASX_OK) return 0;
    return info.kind == ASX_OBLIGATION_KIND_SEND_PERMIT && info.state == state &&
           info.abort_reason == reason;
}

TEST(tracked_permit_resolves_its_obligation) {
    /* The permit's SendPermit obligation (oneshot.rs:528): committed by a
     * delivered send, aborted ERROR when the receiver is gone, EXPLICIT by
     * an abort (:731-799). */
    asx_cx cx;
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    asx_oneshot_permit p;
    asx_obligation_id ob;
    (void)cx_task(&cx);

    MUST_OK(asx_oneshot_create(&tx, &rx));
    ASSERT_EQ(asx_oneshot_reserve(&tx, &cx, &p), ASX_OK);
    ob = p.obligation;
    ASSERT_TRUE(obligation_is(ob, ASX_OBLIGATION_RESERVED, ASX_OBLIGATION_ABORT_NONE));
    ASSERT_EQ(asx_oneshot_permit_send(&p, 1), ASX_OK);
    ASSERT_TRUE(obligation_is(ob, ASX_OBLIGATION_COMMITTED, ASX_OBLIGATION_ABORT_NONE));

    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_oneshot_reserve(&tx, &cx, &p));
    ob = p.obligation;
    asx_oneshot_receiver_drop(&rx);
    ASSERT_EQ(asx_oneshot_permit_send(&p, 2), ASX_E_DISCONNECTED);
    ASSERT_TRUE(obligation_is(ob, ASX_OBLIGATION_ABORTED, ASX_OBLIGATION_ABORT_ERROR));

    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_oneshot_reserve(&tx, &cx, &p));
    ob = p.obligation;
    asx_oneshot_permit_abort(&p);
    ASSERT_TRUE(obligation_is(ob, ASX_OBLIGATION_ABORTED, ASX_OBLIGATION_ABORT_EXPLICIT));
}

TEST(cancelled_reserve_closes_the_channel) {
    /* A cancelled Cx consumes the sender and closes the channel
     * (oneshot.rs:497-510). */
    asx_cx cx;
    asx_task_id t = cx_task(&cx);
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    asx_oneshot_permit p;
    uint64_t val = 0;
    MUST_OK(asx_oneshot_create(&tx, &rx));
    MUST_OK(asx_task_cancel(t, ASX_CANCEL_USER));
    ASSERT_EQ(asx_oneshot_reserve(&tx, &cx, &p), ASX_E_CANCELLED);
    ASSERT_EQ(p.live, 0);
    ASSERT_EQ(asx_oneshot_try_recv(&rx, &val), ASX_E_DISCONNECTED);
}

/* ------------------------------------------------------------------ */
/* Closed notification (Rust is_closed / is_ready / poll_closed)       */
/* ------------------------------------------------------------------ */

TEST(closed_predicates_follow_rust) {
    /* Sender::is_closed and SendPermit::is_closed: the receiver is gone
     * (oneshot.rs:596, :801); Receiver::is_ready: a value is waiting
     * (:1350); Receiver::is_closed: sender consumed, no permit, no value
     * (:236, :1357), so also after the value is received. */
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    asx_oneshot_permit p;
    uint64_t val = 0;

    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    ASSERT_FALSE(asx_oneshot_sender_is_closed(&tx));
    ASSERT_FALSE(asx_oneshot_receiver_is_ready(&rx));
    ASSERT_FALSE(asx_oneshot_receiver_is_closed(&rx));
    ASSERT_EQ(asx_oneshot_try_send(&tx, 5), ASX_OK);
    ASSERT_TRUE(asx_oneshot_receiver_is_ready(&rx));
    ASSERT_FALSE(asx_oneshot_receiver_is_closed(&rx));
    ASSERT_EQ(asx_oneshot_try_recv(&rx, &val), ASX_OK);
    ASSERT_FALSE(asx_oneshot_receiver_is_ready(&rx));
    ASSERT_TRUE(asx_oneshot_receiver_is_closed(&rx));

    MUST_OK(asx_oneshot_create(&tx, &rx));
    asx_oneshot_receiver_drop(&rx);
    ASSERT_TRUE(asx_oneshot_sender_is_closed(&tx));

    MUST_OK(asx_oneshot_create(&tx, &rx));
    ASSERT_EQ(asx_oneshot_reserve(&tx, NULL, &p), ASX_OK);
    ASSERT_FALSE(asx_oneshot_receiver_is_closed(&rx)); /* permit outstanding */
    ASSERT_FALSE(asx_oneshot_permit_is_closed(&p));
    asx_oneshot_receiver_drop(&rx);
    ASSERT_TRUE(asx_oneshot_permit_is_closed(&p));

    MUST_OK(asx_oneshot_create(&tx, &rx));
    asx_oneshot_sender_drop(&tx);
    ASSERT_TRUE(asx_oneshot_receiver_is_closed(&rx));
}

TEST(poll_closed_outside_a_task) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;

    setup();
    MUST_OK(asx_oneshot_create(&tx, &rx));
    ASSERT_EQ(asx_oneshot_poll_closed(&tx), ASX_E_PENDING);
    ASSERT_EQ(asx_oneshot_receiver_poll_closed(&rx), ASX_E_PENDING);
    asx_oneshot_receiver_drop(&rx);
    ASSERT_EQ(asx_oneshot_poll_closed(&tx), ASX_OK);
    ASSERT_EQ(asx_oneshot_receiver_poll_closed(&rx), ASX_E_INVALID_STATE);

    MUST_OK(asx_oneshot_create(&tx, &rx));
    asx_oneshot_sender_drop(&tx);
    ASSERT_EQ(asx_oneshot_receiver_poll_closed(&rx), ASX_OK);
    ASSERT_EQ(asx_oneshot_poll_closed(&tx), ASX_E_INVALID_STATE); /* consumed */
}

/* Waits in poll_closed on one side; records its polls and the result. */
typedef struct {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    int receiver_side;
    uint32_t polls;
    asx_status result;
} closed_waiter;

static asx_status poll_closed_waiter(void *ud, asx_task_id self) {
    closed_waiter *w = (closed_waiter *)ud;
    (void)self;
    w->polls++;
    w->result = w->receiver_side ? asx_oneshot_receiver_poll_closed(&w->rx)
                                 : asx_oneshot_poll_closed(&w->tx);
    return w->result == ASX_E_PENDING ? ASX_E_PENDING : ASX_OK;
}

TEST(closed_waiters_wake_as_rust_wakers_do) {
    /* The sender's waiter wakes on the receiver drop (:1440). The
     * receiver's does not wake on a send (:731-760), and wakes when the
     * value is taken (:1318-1340). */
    static closed_waiter w[2];
    asx_oneshot_sender tx_a;
    asx_oneshot_receiver rx_a;
    asx_oneshot_sender tx_b;
    asx_oneshot_receiver rx_b;
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    uint64_t val = 0;

    asx_runtime_reset();
    setup();
    memset(w, 0, sizeof(w));
    MUST_OK(asx_region_open(&r));
    MUST_OK(asx_oneshot_create(&tx_a, &rx_a));
    MUST_OK(asx_oneshot_create(&tx_b, &rx_b));
    w[0].tx = tx_a;
    w[1].rx = rx_b;
    w[1].receiver_side = 1;
    MUST_OK(asx_task_spawn(r, poll_closed_waiter, &w[0], &t));
    MUST_OK(asx_task_spawn(r, poll_closed_waiter, &w[1], &t));
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(w[0].polls, 1u);
    ASSERT_EQ(w[1].polls, 1u);

    ASSERT_EQ(asx_oneshot_try_send(&tx_b, 7), ASX_OK);
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(w[1].polls, 1u);

    ASSERT_EQ(asx_oneshot_try_recv(&rx_b, &val), ASX_OK);
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(w[1].polls, 2u);
    ASSERT_EQ(w[1].result, ASX_OK);
    ASSERT_EQ(w[0].polls, 1u);

    asx_oneshot_receiver_drop(&rx_a);
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(w[0].polls, 2u);
    ASSERT_EQ(w[0].result, ASX_OK);
}

TEST(receiver_closed_waiter_wakes_on_sender_drop_and_abort) {
    static closed_waiter w[2];
    asx_oneshot_sender tx_a;
    asx_oneshot_receiver rx_a;
    asx_oneshot_sender tx_b;
    asx_oneshot_receiver rx_b;
    asx_oneshot_permit p;
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;

    asx_runtime_reset();
    setup();
    memset(w, 0, sizeof(w));
    MUST_OK(asx_region_open(&r));
    MUST_OK(asx_oneshot_create(&tx_a, &rx_a));
    MUST_OK(asx_oneshot_create(&tx_b, &rx_b));
    w[0].rx = rx_a;
    w[0].receiver_side = 1;
    w[1].rx = rx_b;
    w[1].receiver_side = 1;
    MUST_OK(asx_task_spawn(r, poll_closed_waiter, &w[0], &t));
    MUST_OK(asx_task_spawn(r, poll_closed_waiter, &w[1], &t));
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);

    asx_oneshot_sender_drop(&tx_a);
    ASSERT_EQ(asx_oneshot_reserve(&tx_b, NULL, &p), ASX_OK);
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(w[0].result, ASX_OK);
    ASSERT_EQ(w[1].polls, 1u); /* a reserve is no close */

    asx_oneshot_permit_abort(&p);
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(w[1].polls, 2u);
    ASSERT_EQ(w[1].result, ASX_OK);
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

    RUN_TEST(reserve_consumes_the_sender_until_the_permit_sends);
    RUN_TEST(permit_abort_closes_the_channel);
    RUN_TEST(tracked_permit_resolves_its_obligation);
    RUN_TEST(cancelled_reserve_closes_the_channel);

    RUN_TEST(closed_predicates_follow_rust);
    RUN_TEST(poll_closed_outside_a_task);
    RUN_TEST(closed_waiters_wake_as_rust_wakers_do);
    RUN_TEST(receiver_closed_waiter_wakes_on_sender_drop_and_abort);

    TEST_REPORT();
    return test_failures;
}
