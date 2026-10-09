/*
 * test_channel_wake.c — wake-driven channel waiting
 *
 * Tasks that hit a "not yet" result on a channel inside a scheduler poll
 * are parked and woken by the state change that can satisfy them, instead
 * of being re-polled every round. Covers mpsc (recv + reserve), oneshot,
 * broadcast, watch and session; FIFO wake order; close/disconnect waking
 * everyone; pass-on when a woken waiter gives up or is cancelled; queue
 * overflow degrading to cooperative polling; and run-to-run determinism.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/core/broadcast.h>
#include <asx/core/channel.h>
#include <asx/core/oneshot.h>
#include <asx/core/session.h>
#include <asx/core/watch.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <asx/time/sleep.h>
#include <string.h>

#define MS ((uint64_t)1000000u)

static asx_runtime g_rt;
static asx_region_id g_region;

/* Virtual-clock runtime (default stub hooks) and a fresh open region. */
static int setup(void) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    if (st == ASX_OK) st = asx_region_open(&g_region);
    return st == ASX_OK;
}

/* Polls consumed by a run that started with `start` polls of budget. */
static uint32_t polls_used(uint32_t start, const asx_budget *b) {
    return start - asx_budget_polls(b);
}

/* 1 if the task's outcome is CANCELLED. */
static int task_cancelled(asx_task_id t) {
    asx_outcome out;
    if (asx_task_get_outcome(t, &out) != ASX_OK) return 0;
    return asx_outcome_severity_of(&out) == ASX_OUTCOME_CANCELLED;
}

/* Cancel-aware prologue: 1 if `self` has a cancel request pending. */
static int cancel_requested(asx_task_id self) {
    asx_checkpoint_result cr;
    if (asx_checkpoint(self, &cr) != ASX_OK) return 0;
    return cr.cancelled;
}

/* ===================================================================
 * MPSC fixtures
 * =================================================================== */

typedef struct {
    asx_channel_id ch;
    asx_sleep_state sleep;
    int slept;
    uint64_t delay_ns; /* 0 = no initial sleep */
    uint32_t polls;
    uint32_t want; /* messages to receive before completing */
    uint32_t got;
    uint64_t values[64];
    asx_status last; /* terminal status if not OK */
} recv_state;

/* Sleep `delay_ns` once (if non-zero) before the task's real work. */
static asx_status initial_sleep(asx_sleep_state *sleep, int *slept, uint64_t delay_ns,
                                asx_task_id self) {
    asx_status st;
    if (delay_ns == 0u || *slept) return ASX_OK;
    if (sleep->duration_ns == 0u) {
        st = asx_sleep_init(sleep, delay_ns);
        if (st != ASX_OK) return st;
    }
    st = asx_sleep_poll(sleep, self);
    if (st == ASX_OK) *slept = 1;
    return st;
}

/* Receive `want` messages; DISCONNECTED ends the task early. */
static asx_status poll_receiver(void *ud, asx_task_id self) {
    recv_state *s = (recv_state *)ud;
    asx_status st0;
    s->polls++;
    st0 = initial_sleep(&s->sleep, &s->slept, s->delay_ns, self);
    if (st0 != ASX_OK) return st0;
    while (s->got < s->want) {
        uint64_t v = 0;
        asx_status st = asx_channel_recv(s->ch, NULL, &v); /* parks when empty */
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        if (st != ASX_OK) {
            s->last = st;
            return ASX_OK;
        }
        s->values[s->got++] = v;
    }
    return ASX_OK;
}

typedef struct {
    asx_channel_id ch;
    asx_sleep_state sleep;
    int slept;
    uint64_t delay_ns; /* 0 = no initial sleep */
    uint32_t polls;
    uint32_t to_send;
    uint32_t sent;
    uint64_t base; /* value of the first message */
    int give_up;   /* stop waiting (withdraw) on the next poll */
    int cancel_aware;
    asx_status last;
} send_state;

/* Optionally sleep, then reserve+send `to_send` values (base, base+1..). */
static asx_status poll_sender(void *ud, asx_task_id self) {
    send_state *s = (send_state *)ud;
    asx_status st0;
    s->polls++;
    if (s->cancel_aware && cancel_requested(self)) return ASX_OK;
    if (s->give_up) {
        s->last = asx_channel_wait_cancel(s->ch, self);
        return ASX_OK;
    }
    st0 = initial_sleep(&s->sleep, &s->slept, s->delay_ns, self);
    if (st0 != ASX_OK) return st0;
    while (s->sent < s->to_send) {
        asx_send_permit permit;
        asx_status st = asx_channel_reserve(s->ch, NULL, &permit); /* parks when full */
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        if (st != ASX_OK) {
            s->last = st;
            return ASX_OK;
        }
        st = asx_send_permit_send(&permit, s->base + s->sent);
        if (st != ASX_OK) {
            s->last = st;
            return ASX_OK;
        }
        s->sent++;
    }
    return ASX_OK;
}

static recv_state g_rx[4];
static send_state g_tx[40];

static void reset_fixtures(void) {
    memset(g_rx, 0, sizeof(g_rx));
    memset(g_tx, 0, sizeof(g_tx));
}

/* ===================================================================
 * MPSC tests
 * =================================================================== */

TEST(receiver_parks_until_sender_sends) {
    asx_task_id rx;
    asx_task_id tx;
    asx_budget budget;

    ASSERT_TRUE(setup());
    reset_fixtures();
    ASSERT_EQ(asx_channel_create(g_region, 4, &g_rx[0].ch), ASX_OK);
    g_rx[0].want = 1;
    g_tx[0].ch = g_rx[0].ch;
    g_tx[0].delay_ns = 5u * MS;
    g_tx[0].to_send = 1;
    g_tx[0].base = 77;

    /* Receiver first in arena order, so it polls the empty channel. */
    ASSERT_EQ(asx_task_spawn(g_region, poll_receiver, &g_rx[0], &rx), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_region, poll_sender, &g_tx[0], &tx), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);

    /* Receiver: park on empty, then woken by the commit. Sender: park on
     * its timer, then send. Nothing is re-polled in between, so virtual
     * time can jump to the sender's deadline. */
    ASSERT_EQ(g_rx[0].polls, 2u);
    ASSERT_EQ(g_rx[0].got, 1u);
    ASSERT_EQ(g_rx[0].values[0], (uint64_t)77);
    ASSERT_EQ(g_tx[0].polls, 2u);
    ASSERT_EQ(polls_used(100, &budget), 4u);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)(5u * MS));
}

TEST(receiver_without_sender_would_block) {
    asx_task_id rx;
    asx_budget budget;

    ASSERT_TRUE(setup());
    reset_fixtures();
    ASSERT_EQ(asx_channel_create(g_region, 4, &g_rx[0].ch), ASX_OK);
    g_rx[0].want = 1;
    ASSERT_EQ(asx_task_spawn(g_region, poll_receiver, &g_rx[0], &rx), ASX_OK);

    /* Nothing can ever wake it: the run reports WOULD_BLOCK after a single
     * poll instead of exhausting the budget. */
    budget = asx_budget_from_polls(1000);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(g_rx[0].polls, 1u);
    ASSERT_EQ(polls_used(1000, &budget), 1u);

    /* A send from outside the scheduler wakes it. */
    {
        asx_send_permit permit;
        ASSERT_EQ(asx_channel_try_reserve(g_rx[0].ch, &permit), ASX_OK);
        ASSERT_EQ(asx_send_permit_send(&permit, 5u), ASX_OK);
    }
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(g_rx[0].polls, 2u);
    ASSERT_EQ(g_rx[0].values[0], (uint64_t)5);
}

TEST(full_channel_parks_producers_and_dequeue_wakes_one) {
    asx_channel_id ch;
    asx_task_id tx[3];
    asx_budget budget;
    asx_send_permit permit;
    uint64_t v;
    uint32_t i;

    ASSERT_TRUE(setup());
    reset_fixtures();
    ASSERT_EQ(asx_channel_create(g_region, 1, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&permit, 100u), ASX_OK);

    for (i = 0; i < 3u; i++) {
        g_tx[i].ch = ch;
        g_tx[i].to_send = 1;
        g_tx[i].base = i;
        ASSERT_EQ(asx_task_spawn(g_region, poll_sender, &g_tx[i], &tx[i]), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(polls_used(100, &budget), 3u);

    /* One dequeue frees one slot: exactly one producer (the oldest) runs. */
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)100);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(polls_used(100, &budget), 1u);
    ASSERT_EQ(g_tx[0].sent, 1u);
    ASSERT_EQ(g_tx[1].polls, 1u);
    ASSERT_EQ(g_tx[2].polls, 1u);

    /* Drain the rest one at a time: FIFO, one producer per slot. */
    for (i = 1; i < 3u; i++) {
        ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
        ASSERT_EQ(v, (uint64_t)(i - 1u));
        budget = asx_budget_from_polls(100);
        ASSERT_EQ(asx_scheduler_run(g_region, &budget), i == 2u ? ASX_OK : ASX_E_WOULD_BLOCK);
        ASSERT_EQ(polls_used(100, &budget), 1u);
        ASSERT_EQ(g_tx[i].sent, 1u);
    }
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)2);
}

/* Producers arrive at a full channel in a different order than their
 * arena slots (they sleep different amounts first); a consumer that wakes
 * after all of them parked then frees one slot at a time. Returns the
 * received sequence in out[0..3]. */
static void run_fifo_arrival_scenario(uint64_t *out, uint32_t *out_count) {
    static const uint64_t delays[3] = {3u * MS, 1u * MS, 2u * MS};
    asx_channel_id ch;
    asx_task_id t;
    asx_send_permit permit;
    asx_budget budget;
    uint32_t i;

    *out_count = 0;
    if (!setup()) return;
    reset_fixtures();
    if (asx_channel_create(g_region, 1, &ch) != ASX_OK) return;
    if (asx_channel_try_reserve(ch, &permit) != ASX_OK) return;
    if (asx_send_permit_send(&permit, 99u) != ASX_OK) return;

    for (i = 0; i < 3u; i++) {
        g_tx[i].ch = ch;
        g_tx[i].delay_ns = delays[i];
        g_tx[i].to_send = 1;
        g_tx[i].base = i;
        if (asx_task_spawn(g_region, poll_sender, &g_tx[i], &t) != ASX_OK) return;
    }
    g_rx[0].ch = ch;
    g_rx[0].delay_ns = 10u * MS;
    g_rx[0].want = 4;
    if (asx_task_spawn(g_region, poll_receiver, &g_rx[0], &t) != ASX_OK) return;

    budget = asx_budget_from_polls(200);
    if (asx_scheduler_run(g_region, &budget) != ASX_OK) return;
    for (i = 0; i < g_rx[0].got && i < 4u; i++) out[i] = g_rx[0].values[i];
    *out_count = g_rx[0].got;
}

TEST(reserve_waiters_served_in_arrival_order_deterministically) {
    uint64_t first[4];
    uint64_t second[4];
    uint32_t n1;
    uint32_t n2;

    run_fifo_arrival_scenario(first, &n1);
    run_fifo_arrival_scenario(second, &n2);
    ASSERT_EQ(n1, 4u);
    ASSERT_EQ(n2, 4u);
    ASSERT_EQ(memcmp(first, second, sizeof(first)), 0);

    /* Producers parked at 1, 2 and 3 ms (slots 1, 2, 0); once the consumer
     * starts draining at 10 ms each freed slot goes to the oldest parked
     * producer: arrival order, not arena order. */
    ASSERT_EQ(first[0], (uint64_t)99);
    ASSERT_EQ(first[1], (uint64_t)1);
    ASSERT_EQ(first[2], (uint64_t)2);
    ASSERT_EQ(first[3], (uint64_t)0);
}

TEST(try_reserve_never_jumps_parked_producer) {
    asx_channel_id ch;
    asx_send_permit permit;
    asx_task_id t;
    asx_budget budget;
    uint64_t v;

    ASSERT_TRUE(setup());
    reset_fixtures();
    ASSERT_EQ(asx_channel_create(g_region, 1, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&permit, 1u), ASX_OK);
    g_tx[0].ch = ch;
    g_tx[0].to_send = 1;
    g_tx[0].base = 50;
    ASSERT_EQ(asx_task_spawn(g_region, poll_sender, &g_tx[0], &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    /* The freed slot belongs to the parked producer: a try_reserve from
     * anyone else reports FULL even though raw capacity is available. */
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_E_CHANNEL_FULL);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(g_tx[0].sent, 1u);
    ASSERT_EQ(g_tx[0].polls, 2u);

    /* Line empty again: plain try semantics. */
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)50);
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);
    asx_send_permit_abort(&permit);
}

TEST(close_wakes_every_waiter) {
    asx_channel_id full;
    asx_channel_id empty;
    asx_send_permit permit;
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    reset_fixtures();
    ASSERT_EQ(asx_channel_create(g_region, 1, &full), ASX_OK);
    ASSERT_EQ(asx_channel_create(g_region, 1, &empty), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(full, &permit), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&permit, 1u), ASX_OK);

    for (i = 0; i < 2u; i++) {
        g_tx[i].ch = full;
        g_tx[i].to_send = 1;
        ASSERT_EQ(asx_task_spawn(g_region, poll_sender, &g_tx[i], &t), ASX_OK);
        g_rx[i].ch = empty;
        g_rx[i].want = 1;
        ASSERT_EQ(asx_task_spawn(g_region, poll_receiver, &g_rx[i], &t), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    /* Closing wakes all four parked tasks; each observes the closed state
     * on its second poll. */
    ASSERT_EQ(asx_channel_close_receiver(full), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(empty), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(polls_used(100, &budget), 4u);
    for (i = 0; i < 2u; i++) {
        ASSERT_EQ(g_tx[i].polls, 2u);
        ASSERT_EQ(g_tx[i].last, ASX_E_DISCONNECTED);
        ASSERT_EQ(g_rx[i].polls, 2u);
        ASSERT_EQ(g_rx[i].last, ASX_E_DISCONNECTED);
    }
}

TEST(woken_waiter_that_gives_up_passes_wake_on) {
    asx_channel_id ch;
    asx_send_permit permit;
    asx_task_id t;
    asx_budget budget;
    uint64_t v;
    uint32_t i;

    ASSERT_TRUE(setup());
    reset_fixtures();
    ASSERT_EQ(asx_channel_create(g_region, 1, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&permit, 1u), ASX_OK);
    for (i = 0; i < 2u; i++) {
        g_tx[i].ch = ch;
        g_tx[i].to_send = 1;
        g_tx[i].base = 10u + i;
        ASSERT_EQ(asx_task_spawn(g_region, poll_sender, &g_tx[i], &t), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    /* The freed slot wakes the oldest producer, which no longer wants it:
     * withdrawing hands the wake to the second producer. */
    g_tx[0].give_up = 1;
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(g_tx[0].last, ASX_OK);
    ASSERT_EQ(g_tx[0].sent, 0u);
    ASSERT_EQ(g_tx[1].sent, 1u);
    ASSERT_EQ(g_tx[1].polls, 2u);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)11);
}

TEST(cancelled_waiter_does_not_absorb_wake) {
    asx_channel_id ch;
    asx_send_permit permit;
    asx_task_id t[2];
    asx_budget budget;
    uint64_t v;
    uint32_t i;

    ASSERT_TRUE(setup());
    reset_fixtures();
    ASSERT_EQ(asx_channel_create(g_region, 1, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&permit, 1u), ASX_OK);
    for (i = 0; i < 2u; i++) {
        g_tx[i].ch = ch;
        g_tx[i].to_send = 1;
        g_tx[i].base = 20u + i;
        g_tx[i].cancel_aware = 1;
        ASSERT_EQ(asx_task_spawn(g_region, poll_sender, &g_tx[i], &t[i]), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    /* The oldest queued producer is cancelled: the freed slot still goes to
     * the second one, without anyone calling wait_cancel. */
    ASSERT_EQ(asx_task_cancel(t[0], ASX_CANCEL_USER), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_TRUE(task_cancelled(t[0]));
    ASSERT_EQ(g_tx[0].sent, 0u);
    ASSERT_EQ(g_tx[1].sent, 1u);
    ASSERT_EQ(g_tx[1].polls, 2u);
}

TEST(wait_cancel_rejects_bad_channel) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_channel_wait_cancel(ASX_INVALID_ID, ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);
}

TEST(waiter_overflow_degrades_to_polling) {
    enum { PRODUCERS = ASX_CHANNEL_MAX_WAITERS + 2 };
    asx_channel_id ch;
    asx_task_id t;
    asx_budget budget;
    uint32_t i;
    uint32_t seen = 0;

    ASSERT_TRUE(setup());
    reset_fixtures();
    ASSERT_EQ(asx_channel_create(g_region, 1, &ch), ASX_OK);
    for (i = 0; i < (uint32_t)PRODUCERS; i++) {
        g_tx[i].ch = ch;
        g_tx[i].to_send = 1;
        g_tx[i].base = i;
        ASSERT_EQ(asx_task_spawn(g_region, poll_sender, &g_tx[i], &t), ASX_OK);
    }
    g_rx[0].ch = ch;
    g_rx[0].want = (uint32_t)PRODUCERS;
    ASSERT_EQ(asx_task_spawn(g_region, poll_receiver, &g_rx[0], &t), ASX_OK);

    /* More producers than queue slots: the overflow yields instead of
     * parking, and every message still gets through. */
    budget = asx_budget_from_polls(100000);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(g_rx[0].got, (uint32_t)PRODUCERS);
    for (i = 0; i < (uint32_t)PRODUCERS; i++) {
        ASSERT_EQ(g_tx[i].sent, 1u);
        seen += (uint32_t)g_rx[0].values[i];
    }
    ASSERT_EQ(seen, (uint32_t)(PRODUCERS * (PRODUCERS - 1) / 2));
}

TEST(try_ops_outside_scheduler_never_park) {
    asx_channel_id ch;
    asx_send_permit permit;
    uint64_t v;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_channel_create(g_region, 1, &ch), ASX_OK);
    ASSERT_EQ(asx_task_current(), (asx_task_id)ASX_INVALID_ID);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_E_CHANNEL_FULL);
    /* Nothing was queued: withdrawing an unknown task is a no-op. */
    ASSERT_EQ(asx_channel_wait_cancel(ch, ASX_INVALID_ID), ASX_OK);
}

/* Inside a poll too, try_recv / try_reserve never park (Rust's try_* register
 * no waker; bd-vc1v): a task that gets EMPTY or FULL and yields is polled
 * again on the next round, and nothing waits in the channel's lines. */
typedef struct {
    asx_channel_id ch;
    uint32_t polls;
    asx_status recv;
    asx_status reserve;
} try_state;

static asx_status poll_try_then_yield(void *ud, asx_task_id self) {
    try_state *s = (try_state *)ud;
    asx_send_permit permit;
    uint64_t v;
    (void)self;
    s->polls++;
    s->recv = asx_channel_try_recv(s->ch, &v);
    s->reserve = asx_channel_try_reserve(s->ch, &permit);
    return s->polls < 3u ? ASX_E_PENDING : ASX_OK; /* yield twice */
}

TEST(try_ops_inside_a_poll_never_park) {
    asx_send_permit permit;
    asx_task_id t;
    asx_budget budget;
    try_state s;

    ASSERT_TRUE(setup());
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_channel_create(g_region, 1, &s.ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(s.ch, &permit), ASX_OK); /* full from now on */
    ASSERT_EQ(asx_task_spawn(g_region, poll_try_then_yield, &s, &t), ASX_OK);
    budget = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(s.polls, 3u);
    ASSERT_EQ(s.recv, ASX_E_WOULD_BLOCK);
    ASSERT_EQ(s.reserve, ASX_E_CHANNEL_FULL);
    /* Nothing queued: the outstanding permit's abort frees the slot, and a
     * reserve from outside any task gets it at once. */
    asx_send_permit_abort(&permit);
    ASSERT_EQ(asx_channel_try_reserve(s.ch, &permit), ASX_OK);
}

/* ===================================================================
 * Oneshot
 * =================================================================== */

typedef struct {
    asx_oneshot_receiver rx;
    uint32_t polls;
    uint64_t value;
    asx_status result;
} oneshot_rx_state;

static asx_status poll_oneshot_rx(void *ud, asx_task_id self) {
    oneshot_rx_state *s = (oneshot_rx_state *)ud;
    asx_status st;
    (void)self;
    s->polls++;
    st = asx_oneshot_recv(&s->rx, NULL, &s->value); /* parks until a send or drop */
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    s->result = st;
    return ASX_OK;
}

TEST(oneshot_receiver_parks_until_send_or_drop) {
    asx_oneshot_sender tx1;
    asx_oneshot_sender tx2;
    oneshot_rx_state a;
    oneshot_rx_state b;
    asx_task_id t;
    asx_budget budget;

    ASSERT_TRUE(setup());
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    ASSERT_EQ(asx_oneshot_create(&tx1, &a.rx), ASX_OK);
    ASSERT_EQ(asx_oneshot_create(&tx2, &b.rx), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_region, poll_oneshot_rx, &a, &t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_region, poll_oneshot_rx, &b, &t), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(polls_used(100, &budget), 2u);

    ASSERT_EQ(asx_oneshot_try_send(&tx1, 42u), ASX_OK);
    asx_oneshot_sender_drop(&tx2);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(a.polls, 2u);
    ASSERT_EQ(a.result, ASX_OK);
    ASSERT_EQ(a.value, (uint64_t)42);
    ASSERT_EQ(b.polls, 2u);
    ASSERT_EQ(b.result, ASX_E_DISCONNECTED);
}

/* ===================================================================
 * Broadcast
 * =================================================================== */

typedef struct {
    asx_broadcast_receiver rx;
    uint32_t polls;
    uint32_t got;
    uint64_t last_value;
    asx_status result;
} bcast_rx_state;

/* Receive messages until the sender is dropped. */
static asx_status poll_bcast_rx(void *ud, asx_task_id self) {
    bcast_rx_state *s = (bcast_rx_state *)ud;
    (void)self;
    s->polls++;
    for (;;) {
        uint64_t v;
        asx_status st = asx_broadcast_recv(&s->rx, NULL, &v); /* parks until a send */
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        if (st != ASX_OK) {
            s->result = st;
            return ASX_OK;
        }
        s->got++;
        s->last_value = v;
    }
}

TEST(broadcast_send_wakes_every_receiver) {
    asx_broadcast_sender tx;
    bcast_rx_state r[3];
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    memset(r, 0, sizeof(r));
    ASSERT_EQ(asx_broadcast_create(4, &tx, &r[0].rx), ASX_OK);
    ASSERT_EQ(asx_broadcast_subscribe(&tx, &r[1].rx), ASX_OK);
    ASSERT_EQ(asx_broadcast_subscribe(&tx, &r[2].rx), ASX_OK);
    for (i = 0; i < 3u; i++) {
        ASSERT_EQ(asx_task_spawn(g_region, poll_bcast_rx, &r[i], &t), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    ASSERT_EQ(asx_broadcast_send(&tx, NULL, 7u), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(polls_used(100, &budget), 3u);
    for (i = 0; i < 3u; i++) {
        ASSERT_EQ(r[i].polls, 2u);
        ASSERT_EQ(r[i].got, 1u);
        ASSERT_EQ(r[i].last_value, (uint64_t)7);
    }

    asx_broadcast_sender_drop(&tx);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    for (i = 0; i < 3u; i++) {
        ASSERT_EQ(r[i].polls, 3u);
        ASSERT_EQ(r[i].result, ASX_E_DISCONNECTED);
    }
}

/* ===================================================================
 * Watch
 * =================================================================== */

typedef struct {
    asx_watch_receiver rx;
    uint32_t polls;
    uint32_t changes;
    uint64_t value;
    asx_status result;
} watch_rx_state;

/* Observe changes until the sender is dropped. */
static asx_status poll_watch_rx(void *ud, asx_task_id self) {
    watch_rx_state *s = (watch_rx_state *)ud;
    (void)self;
    s->polls++;
    for (;;) {
        asx_status st = asx_watch_poll_changed(&s->rx, NULL);
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        if (st != ASX_OK) {
            s->result = st;
            return ASX_OK;
        }
        s->changes++;
        if (asx_watch_recv(&s->rx, &s->value) != ASX_OK) return ASX_E_INVALID_STATE;
    }
}

TEST(watch_publish_wakes_receivers) {
    asx_watch_sender tx;
    watch_rx_state r[2];
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    memset(r, 0, sizeof(r));
    ASSERT_EQ(asx_watch_create(1u, &tx, &r[0].rx), ASX_OK);
    ASSERT_EQ(asx_watch_subscribe(&tx, &r[1].rx), ASX_OK);
    for (i = 0; i < 2u; i++) {
        ASSERT_EQ(asx_task_spawn(g_region, poll_watch_rx, &r[i], &t), ASX_OK);
    }

    /* The initial value is not a change: the first poll parks at once. */
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(r[0].changes, 0u);
    ASSERT_EQ(r[1].changes, 0u);

    ASSERT_EQ(asx_watch_send(&tx, 2u), ASX_OK);
    ASSERT_EQ(asx_watch_send(&tx, 3u), ASX_OK); /* latest value wins */
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(polls_used(100, &budget), 2u);
    for (i = 0; i < 2u; i++) {
        ASSERT_EQ(r[i].changes, 1u); /* two sends, observed as one change */
        ASSERT_EQ(r[i].value, (uint64_t)3);
    }

    asx_watch_sender_drop(&tx);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    for (i = 0; i < 2u; i++) {
        ASSERT_EQ(r[i].polls, 3u);
        ASSERT_EQ(r[i].result, ASX_E_DISCONNECTED);
    }
}

TEST(watch_poll_changed_outside_scheduler) {
    asx_watch_sender tx;
    asx_watch_receiver rx;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_watch_create(5u, &tx, &rx), ASX_OK);
    ASSERT_EQ(asx_watch_poll_changed(&rx, NULL), ASX_E_PENDING); /* initial value is not a change */
    ASSERT_FALSE(asx_watch_has_changed(&rx));
    ASSERT_EQ(asx_watch_poll_changed(&rx, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_watch_send(&tx, 6u), ASX_OK);
    ASSERT_EQ(asx_watch_poll_changed(&rx, NULL), ASX_OK);
    asx_watch_sender_drop(&tx);
    ASSERT_EQ(asx_watch_poll_changed(&rx, NULL), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_watch_poll_changed(NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ===================================================================
 * Session
 * =================================================================== */

typedef struct {
    asx_session_endpoint ep;
    uint32_t polls;
    uint32_t want;
    uint32_t got;
    uint64_t last;
    asx_status result;
} session_rx_state;

static asx_status poll_session_rx(void *ud, asx_task_id self) {
    session_rx_state *s = (session_rx_state *)ud;
    (void)self;
    s->polls++;
    while (s->got < s->want) {
        uint64_t v;
        asx_status st = asx_session_try_recv(&s->ep, &v);
        if (st == ASX_E_WOULD_BLOCK) return ASX_E_PENDING;
        if (st != ASX_OK) {
            s->result = st;
            return ASX_OK;
        }
        s->got++;
        s->last = v;
    }
    return ASX_OK;
}

typedef struct {
    asx_session_endpoint ep;
    uint32_t polls;
    uint32_t to_send;
    uint32_t sent;
} session_tx_state;

static asx_status poll_session_tx(void *ud, asx_task_id self) {
    session_tx_state *s = (session_tx_state *)ud;
    (void)self;
    s->polls++;
    while (s->sent < s->to_send) {
        asx_status st = asx_session_send(&s->ep, 1000u + s->sent);
        if (st == ASX_E_CHANNEL_FULL) return ASX_E_PENDING;
        if (st != ASX_OK) return st;
        s->sent++;
    }
    return ASX_OK;
}

TEST(session_directions_wake_each_other) {
    asx_session_endpoint ini;
    session_rx_state rx;
    session_tx_state tx;
    asx_task_id t;
    asx_budget budget;

    ASSERT_TRUE(setup());
    memset(&rx, 0, sizeof(rx));
    memset(&tx, 0, sizeof(tx));
    ASSERT_EQ(asx_session_create(g_region, 2, &ini, &rx.ep), ASX_OK);
    tx.ep = ini;
    tx.to_send = 5;
    rx.want = 5;

    /* Responder receives (parks when empty); initiator sends 5 requests
     * through a 2-slot direction (parks when full). */
    ASSERT_EQ(asx_task_spawn(g_region, poll_session_rx, &rx, &t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_region, poll_session_tx, &tx, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(rx.got, 5u);
    ASSERT_EQ(rx.last, (uint64_t)1004);
    ASSERT_EQ(tx.sent, 5u);
    /* Ping-pong, not spinning: a handful of polls per side. */
    ASSERT_TRUE(rx.polls <= 4u);
    ASSERT_TRUE(tx.polls <= 4u);
}

TEST(session_drop_wakes_receiver) {
    asx_session_endpoint ini;
    session_rx_state rx;
    asx_task_id t;
    asx_budget budget;

    ASSERT_TRUE(setup());
    memset(&rx, 0, sizeof(rx));
    ASSERT_EQ(asx_session_create(g_region, 2, &ini, &rx.ep), ASX_OK);
    rx.want = 1;
    ASSERT_EQ(asx_task_spawn(g_region, poll_session_rx, &rx, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    asx_session_endpoint_drop(&ini);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(rx.polls, 2u);
    ASSERT_EQ(rx.result, ASX_E_DISCONNECTED);
}

/* ===================================================================
 * Cx-aware oneshot / broadcast / watch: Rust's check order, user traces
 * and SendPermit obligations (vocabulary §3; DSL §3.6)
 * =================================================================== */

#define CAP_MAX 16u

/* User traces and obligation events recorded while capturing, in order. */
static struct {
    asx_trace_event_kind kind;
    uint64_t entity;
    char text[48];
} g_cap[CAP_MAX];
static uint32_t g_ncap;

static void cap_observe(void *ctx, const asx_trace_event *ev, const asx_trace_payload *p) {
    (void)ctx;
    if (ev->kind != ASX_TRACE_USER && ev->kind != ASX_TRACE_OBLIGATION_RESERVE &&
        ev->kind != ASX_TRACE_OBLIGATION_COMMIT && ev->kind != ASX_TRACE_OBLIGATION_ABORT) {
        return;
    }
    if (g_ncap >= CAP_MAX) return;
    g_cap[g_ncap].kind = ev->kind;
    g_cap[g_ncap].entity = ev->entity_id;
    g_cap[g_ncap].text[0] = '\0';
    if (p->text != NULL) {
        size_t n = strlen(p->text);
        if (n >= sizeof(g_cap[0].text)) n = sizeof(g_cap[0].text) - 1u;
        memcpy(g_cap[g_ncap].text, p->text, n);
        g_cap[g_ncap].text[n] = '\0';
    }
    g_ncap++;
}

/* Start (1: forgetting earlier captures) or stop (0) capturing. */
static void capture(int on) {
    if (on) g_ncap = 0;
    asx_trace_set_observer(on ? cap_observe : NULL, NULL);
}

static int cap_user(uint32_t i, asx_task_id task, const char *message) {
    return i < g_ncap && g_cap[i].kind == ASX_TRACE_USER && g_cap[i].entity == task &&
           strcmp(g_cap[i].text, message) == 0;
}

/* Event i is `kind` on a SendPermit held by `task`, now aborted with
 * `reason` (ASX_OBLIGATION_ABORT_NONE: not aborted). */
static int cap_permit(uint32_t i, asx_trace_event_kind kind, asx_task_id task,
                      asx_obligation_abort_reason reason) {
    asx_obligation_info info;
    if (i >= g_ncap || g_cap[i].kind != kind) return 0;
    if (asx_obligation_get_info((asx_obligation_id)g_cap[i].entity, &info) != ASX_OK) return 0;
    return info.kind == ASX_OBLIGATION_KIND_SEND_PERMIT && info.abort_reason == reason &&
           asx_handle_index(info.holder) == asx_handle_index(task);
}

static asx_status poll_idle(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_PENDING;
}

/* A task (never polled) of g_region and its Cx. */
static asx_task_id cx_task(asx_cx *cx) {
    asx_task_id t = ASX_INVALID_ID;
    if (asx_task_spawn(g_region, poll_idle, NULL, &t) != ASX_OK) return ASX_INVALID_ID;
    asx_cx_init(cx, g_region, t, ASX_CAP_CANCEL_CHECK);
    return t;
}

TEST(oneshot_send_with_cx_traces_and_commits_a_send_permit) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    asx_cx a;
    asx_cx b;
    asx_task_id ta;
    asx_task_id tb;
    uint64_t v = 0;

    ASSERT_TRUE(setup());
    ta = cx_task(&a);
    tb = cx_task(&b);
    ASSERT_EQ(asx_oneshot_create(&tx, &rx), ASX_OK);
    capture(1);
    ASSERT_EQ(asx_oneshot_recv(&rx, &b, &v), ASX_E_PENDING); /* nothing traced */
    ASSERT_EQ(asx_oneshot_send(&tx, &a, 6u), ASX_OK);
    ASSERT_EQ(asx_oneshot_recv(&rx, &b, &v), ASX_OK);
    capture(0);
    ASSERT_EQ(v, 6u);
    ASSERT_EQ(g_ncap, 4u);
    ASSERT_TRUE(cap_user(0, ta, "oneshot::reserve creating permit"));
    ASSERT_TRUE(cap_permit(1, ASX_TRACE_OBLIGATION_RESERVE, ta, ASX_OBLIGATION_ABORT_NONE));
    ASSERT_TRUE(cap_permit(2, ASX_TRACE_OBLIGATION_COMMIT, ta, ASX_OBLIGATION_ABORT_NONE));
    ASSERT_TRUE(cap_user(3, tb, "oneshot::recv received value"));
}

TEST(oneshot_cancelled_send_closes_and_value_beats_cancel_on_recv) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    asx_oneshot_sender tx2;
    asx_oneshot_receiver rx2;
    asx_cx a;
    asx_cx b;
    asx_task_id ta;
    asx_task_id tb;
    uint64_t v = 0;

    ASSERT_TRUE(setup());
    ta = cx_task(&a);
    tb = cx_task(&b);
    ASSERT_EQ(asx_oneshot_create(&tx, &rx), ASX_OK);
    ASSERT_EQ(asx_oneshot_create(&tx2, &rx2), ASX_OK);
    ASSERT_EQ(asx_task_cancel(ta, ASX_CANCEL_USER), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tb, ASX_CANCEL_USER), ASX_OK);
    capture(1);
    /* A cancelled send consumes the sender: the receiver sees it closed,
     * which (like a value) is reported before its own cancellation. */
    ASSERT_EQ(asx_oneshot_send(&tx, &a, 1u), ASX_E_CANCELLED);
    ASSERT_EQ(asx_oneshot_recv(&rx, &b, &v), ASX_E_DISCONNECTED);
    /* With nothing sent yet, the receiver's cancellation ends the wait. */
    ASSERT_EQ(asx_oneshot_recv(&rx2, &b, &v), ASX_E_CANCELLED);
    capture(0);
    ASSERT_EQ(g_ncap, 3u);
    ASSERT_TRUE(cap_user(0, ta, "oneshot::reserve cancelled"));
    ASSERT_TRUE(cap_user(1, tb, "oneshot::recv channel closed"));
    ASSERT_TRUE(cap_user(2, tb, "oneshot::recv cancelled while waiting"));
    /* A value already sent is received despite the cancel. */
    ASSERT_EQ(asx_oneshot_send(&tx2, NULL, 9u), ASX_OK);
    ASSERT_EQ(asx_oneshot_recv(&rx2, &b, &v), ASX_OK);
    ASSERT_EQ(v, 9u);
}

TEST(oneshot_send_to_dropped_receiver_aborts_the_permit_with_error) {
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    asx_cx a;
    asx_task_id ta;

    ASSERT_TRUE(setup());
    ta = cx_task(&a);
    ASSERT_EQ(asx_oneshot_create(&tx, &rx), ASX_OK);
    asx_oneshot_receiver_drop(&rx);
    capture(1);
    ASSERT_EQ(asx_oneshot_send(&tx, &a, 1u), ASX_E_DISCONNECTED);
    capture(0);
    ASSERT_EQ(g_ncap, 3u);
    ASSERT_TRUE(cap_user(0, ta, "oneshot::reserve creating permit"));
    ASSERT_TRUE(cap_permit(2, ASX_TRACE_OBLIGATION_ABORT, ta, ASX_OBLIGATION_ABORT_ERROR));
}

TEST(broadcast_send_checks_cancel_then_receivers_and_commits_a_permit) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    asx_cx a;
    asx_task_id ta;
    uint64_t v = 0;

    ASSERT_TRUE(setup());
    ta = cx_task(&a);
    ASSERT_EQ(asx_broadcast_create(2u, &tx, &rx), ASX_OK);
    capture(1);
    ASSERT_EQ(asx_broadcast_send(&tx, &a, 1u), ASX_OK);
    capture(0);
    ASSERT_EQ(g_ncap, 2u);
    ASSERT_TRUE(cap_permit(0, ASX_TRACE_OBLIGATION_RESERVE, ta, ASX_OBLIGATION_ABORT_NONE));
    ASSERT_TRUE(cap_permit(1, ASX_TRACE_OBLIGATION_COMMIT, ta, ASX_OBLIGATION_ABORT_NONE));
    /* Lag: two more sends into capacity 2 drop the first; the receiver
     * learns how many it missed (Rust Lagged(n)). */
    ASSERT_EQ(asx_broadcast_send(&tx, NULL, 2u), ASX_OK);
    ASSERT_EQ(asx_broadcast_send(&tx, NULL, 3u), ASX_OK);
    ASSERT_EQ(asx_broadcast_recv(&rx, NULL, &v), ASX_E_LAGGED);
    ASSERT_EQ(v, 1u);
    ASSERT_EQ(asx_broadcast_recv(&rx, NULL, &v), ASX_OK);
    ASSERT_EQ(v, 2u);
    /* Cancelled: no trace on the send side, nothing published. */
    ASSERT_EQ(asx_task_cancel(ta, ASX_CANCEL_USER), ASX_OK);
    capture(1);
    ASSERT_EQ(asx_broadcast_send(&tx, &a, 4u), ASX_E_CANCELLED);
    capture(0);
    ASSERT_EQ(g_ncap, 0u);
    ASSERT_EQ(asx_broadcast_total_sent(&tx), 3u);
    /* No receiver left: Rust's Closed. */
    asx_broadcast_receiver_drop(&rx);
    ASSERT_EQ(asx_broadcast_send(&tx, NULL, 5u), ASX_E_DISCONNECTED);
}

TEST(broadcast_recv_checks_cancel_first) {
    asx_broadcast_sender tx;
    asx_broadcast_receiver rx;
    asx_cx b;
    asx_task_id tb;
    uint64_t v = 0;

    ASSERT_TRUE(setup());
    tb = cx_task(&b);
    ASSERT_EQ(asx_broadcast_create(4u, &tx, &rx), ASX_OK);
    ASSERT_EQ(asx_broadcast_recv(&rx, &b, &v), ASX_E_PENDING);
    ASSERT_EQ(asx_broadcast_send(&tx, NULL, 7u), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tb, ASX_CANCEL_USER), ASX_OK);
    capture(1);
    ASSERT_EQ(asx_broadcast_recv(&rx, &b, &v), ASX_E_CANCELLED);
    capture(0);
    ASSERT_EQ(g_ncap, 1u);
    ASSERT_TRUE(cap_user(0, tb, "broadcast::recv cancelled"));
    ASSERT_EQ(asx_broadcast_recv(&rx, NULL, &v), ASX_OK); /* the value stays */
    ASSERT_EQ(v, 7u);
}

TEST(watch_changed_traces_each_rust_outcome) {
    asx_watch_sender tx;
    asx_watch_receiver rx;
    asx_cx b;
    asx_cx c;
    asx_task_id tb;
    asx_task_id tc;
    asx_watch_receiver rc;

    ASSERT_TRUE(setup());
    tb = cx_task(&b);
    tc = cx_task(&c);
    ASSERT_EQ(asx_watch_create(0u, &tx, &rx), ASX_OK);
    ASSERT_EQ(asx_watch_subscribe(&tx, &rc), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tc, ASX_CANCEL_USER), ASX_OK);
    capture(1);
    asx_watch_changed_begin(&rx, &b);
    ASSERT_EQ(asx_watch_poll_changed(&rx, &b), ASX_E_PENDING);
    ASSERT_EQ(asx_watch_send(&tx, 5u), ASX_OK);
    ASSERT_EQ(asx_watch_poll_changed(&rx, &b), ASX_OK);
    asx_watch_changed_begin(&rc, &c);
    ASSERT_EQ(asx_watch_poll_changed(&rc, &c), ASX_E_CANCELLED); /* cancel first */
    asx_watch_sender_drop(&tx);
    asx_watch_changed_begin(&rx, &b);
    ASSERT_EQ(asx_watch_poll_changed(&rx, &b), ASX_E_DISCONNECTED);
    capture(0);
    ASSERT_EQ(g_ncap, 6u);
    ASSERT_TRUE(cap_user(0, tb, "watch::changed starting wait"));
    ASSERT_TRUE(cap_user(1, tb, "watch::changed received update"));
    ASSERT_TRUE(cap_user(2, tc, "watch::changed starting wait"));
    ASSERT_TRUE(cap_user(3, tc, "watch::changed cancelled"));
    ASSERT_TRUE(cap_user(4, tb, "watch::changed starting wait"));
    ASSERT_TRUE(cap_user(5, tb, "watch::changed sender dropped"));
}

int main(void) {
    fprintf(stderr, "=== test_channel_wake ===\n");

    RUN_TEST(receiver_parks_until_sender_sends);
    RUN_TEST(receiver_without_sender_would_block);
    RUN_TEST(full_channel_parks_producers_and_dequeue_wakes_one);
    RUN_TEST(reserve_waiters_served_in_arrival_order_deterministically);
    RUN_TEST(try_reserve_never_jumps_parked_producer);
    RUN_TEST(close_wakes_every_waiter);
    RUN_TEST(woken_waiter_that_gives_up_passes_wake_on);
    RUN_TEST(cancelled_waiter_does_not_absorb_wake);
    RUN_TEST(wait_cancel_rejects_bad_channel);
    RUN_TEST(waiter_overflow_degrades_to_polling);
    RUN_TEST(try_ops_outside_scheduler_never_park);
    RUN_TEST(try_ops_inside_a_poll_never_park);
    RUN_TEST(oneshot_receiver_parks_until_send_or_drop);
    RUN_TEST(broadcast_send_wakes_every_receiver);
    RUN_TEST(watch_publish_wakes_receivers);
    RUN_TEST(watch_poll_changed_outside_scheduler);
    RUN_TEST(session_directions_wake_each_other);
    RUN_TEST(session_drop_wakes_receiver);
    RUN_TEST(oneshot_send_with_cx_traces_and_commits_a_send_permit);
    RUN_TEST(oneshot_cancelled_send_closes_and_value_beats_cancel_on_recv);
    RUN_TEST(oneshot_send_to_dropped_receiver_aborts_the_permit_with_error);
    RUN_TEST(broadcast_send_checks_cancel_then_receivers_and_commits_a_permit);
    RUN_TEST(broadcast_recv_checks_cancel_first);
    RUN_TEST(watch_changed_traces_each_rust_outcome);

    TEST_REPORT();
    return test_failures;
}
