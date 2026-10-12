/*
 * test_mpsc.c — unit tests for bounded MPSC two-phase channel (bd-1md.5)
 *
 * Exercises: create/close lifecycle, two-phase reserve/send/abort,
 * FIFO ordering, capacity enforcement, disconnect detection, ring
 * buffer wraparound, stale handle rejection, and capacity exhaustion.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/core/channel.h>
#include <asx/platform/atomics.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <string.h>
/* A live POSIX build runs the channel on its lock-free backend with real
 * atomics, so producers on separate threads race on it for real. */
#if defined(ASX_PROFILE_POSIX) && !ASX_LOCKFREE_SINGLE_THREAD && !defined(ASX_MPSC_PTHREAD_STRESS)
#define ASX_MPSC_PTHREAD_STRESS 1
#endif
#if defined(ASX_MPSC_PTHREAD_STRESS)
#include <pthread.h>
#endif

/* Suppress warn_unused_result for intentionally-ignored calls */
#define CH_IGNORE(expr)                                                                            \
    do {                                                                                           \
        volatile asx_status _ch_ign = (expr);                                                      \
        (void)_ch_ign;                                                                             \
    } while (0)

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static asx_region_id g_rid;

static void setup(void) {
    asx_runtime_reset();
    asx_channel_reset();
    CH_IGNORE(asx_region_open(&g_rid));
}

/* -------------------------------------------------------------------
 * Lifecycle tests
 * ------------------------------------------------------------------- */

TEST(create_basic) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
}

TEST(create_null_out) {
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(create_zero_capacity) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 0, &ch), ASX_E_INVALID_ARGUMENT);
}

TEST(create_over_max_capacity) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, ASX_CHANNEL_MAX_CAPACITY + 1, &ch), ASX_E_INVALID_ARGUMENT);
}

TEST(create_max_capacity) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, ASX_CHANNEL_MAX_CAPACITY, &ch), ASX_OK);
}

TEST(create_exhaustion) {
    asx_channel_id ch;
    uint32_t i;
    setup();
    for (i = 0; i < ASX_MAX_CHANNELS; i++) { ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK); }
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(create_rejects_non_region_handle) {
    asx_channel_id ch;
    asx_region_id not_region;
    setup();

    not_region = asx_handle_pack(ASX_TYPE_TASK, 0u, 1u);
    ASSERT_EQ(asx_channel_create(not_region, 4, &ch), ASX_E_INVALID_ARGUMENT);
}

TEST(create_rejects_closed_region) {
    asx_channel_id ch;
    asx_region_id rid;
    asx_budget budget;
    setup();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_E_INVALID_STATE);
}

TEST(initial_state_is_open) {
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_OPEN);
}

TEST(initial_queue_empty) {
    asx_channel_id ch;
    uint32_t len;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, 0u);
}

TEST(initial_reserved_zero) {
    asx_channel_id ch;
    uint32_t res;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_reserved_count(ch, &res), ASX_OK);
    ASSERT_EQ(res, 0u);
}

/* -------------------------------------------------------------------
 * Close lifecycle tests
 * ------------------------------------------------------------------- */

TEST(close_sender_from_open) {
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_SENDER_CLOSED);
}

TEST(close_receiver_from_open) {
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_RECEIVER_CLOSED);
}

TEST(close_sender_then_receiver) {
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_FULLY_CLOSED);
}

TEST(close_receiver_then_sender) {
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_FULLY_CLOSED);
}

TEST(double_close_sender) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_E_INVALID_STATE);
}

TEST(double_close_receiver) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_E_INVALID_STATE);
}

TEST(sender_side_closes_when_the_last_counted_sender_drops) {
    /* Rust Sender::clone / Sender::drop (mpsc.rs:1231-1255): three senders,
     * two dropped: still open, a send still goes through; the third drop
     * closes the sender side, and the receiver drains then sees
     * DISCONNECTED. */
    asx_channel_id ch;
    asx_channel_state st;
    asx_send_permit p;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_sender_clone(ch), ASX_OK);
    ASSERT_EQ(asx_channel_sender_clone(ch), ASX_OK);

    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_OPEN);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 5u), ASX_OK);

    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_SENDER_CLOSED);
    /* No sender is left to reserve with (reserve_after_sender_closed). */
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, 5u);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_DISCONNECTED);

    /* No live sender left to clone or drop. */
    ASSERT_EQ(asx_channel_sender_clone(ch), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_E_INVALID_STATE);
}

TEST(counted_senders_outlive_a_receiver_close) {
    /* The receiver drops first; the channel becomes fully closed only when
     * the last of two senders drops. */
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_sender_clone(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_RECEIVER_CLOSED);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_FULLY_CLOSED);
}

/* -------------------------------------------------------------------
 * Two-phase send: reserve + send
 * ------------------------------------------------------------------- */

TEST(reserve_and_send_basic) {
    asx_channel_id ch;
    asx_send_permit permit;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);
    ASSERT_EQ(permit.consumed, 0);

    ASSERT_EQ(asx_send_permit_send(&permit, 42), ASX_OK);
    ASSERT_EQ(permit.consumed, 1);

    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, 42u);
}

TEST(fifo_ordering) {
    asx_channel_id ch;
    asx_send_permit p1, p2, p3;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 8, &ch), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &p1), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p2), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p3), ASX_OK);

    ASSERT_EQ(asx_send_permit_send(&p1, 100), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p2, 200), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p3, 300), ASX_OK);

    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, 100u);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, 200u);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, 300u);
}

TEST(trace_emits_send_and_recv) {
    asx_channel_id ch;
    asx_send_permit permit;
    asx_trace_event ev;
    uint64_t value;

    setup();
    asx_trace_reset();

    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&permit, 42u), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, &value), ASX_OK);
    ASSERT_EQ(value, 42u);

    ASSERT_EQ(asx_trace_event_count(), (uint32_t)2);

    ASSERT_TRUE(asx_trace_event_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_CHANNEL_SEND);
    ASSERT_EQ(ev.entity_id, (uint64_t)ch);
    ASSERT_EQ(ev.aux, (uint64_t)42);

    ASSERT_TRUE(asx_trace_event_get(1, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_CHANNEL_RECV);
    ASSERT_EQ(ev.entity_id, (uint64_t)ch);
    ASSERT_EQ(ev.aux, (uint64_t)42);
}

TEST(capacity_enforcement) {
    asx_channel_id ch;
    asx_send_permit permits[4];
    asx_send_permit overflow;
    uint32_t i;
    uint32_t res;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    for (i = 0; i < 4; i++) { ASSERT_EQ(asx_channel_try_reserve(ch, &permits[i]), ASX_OK); }

    ASSERT_EQ(asx_channel_try_reserve(ch, &overflow), ASX_E_CHANNEL_FULL);

    ASSERT_EQ(asx_channel_reserved_count(ch, &res), ASX_OK);
    ASSERT_EQ(res, 4u);

    for (i = 0; i < 4; i++) { asx_send_permit_abort(&permits[i]); }
}

TEST(capacity_mixed_reserved_and_queued) {
    asx_channel_id ch;
    asx_send_permit p1, p2, p3, overflow;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 3, &ch), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &p1), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p1, 10), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &p2), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p3), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &overflow), ASX_E_CHANNEL_FULL);

    ASSERT_EQ(asx_send_permit_send(&p2, 20), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p3, 30), ASX_OK);
}

TEST(capacity_invariant_interleaves_send_abort) {
    asx_channel_id ch;
    asx_send_permit permits[5];
    asx_send_permit overflow;
    uint32_t len;
    uint32_t res;
    uint64_t val;
    uint32_t i;

    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 5, &ch), ASX_OK);

    for (i = 0; i < 5; i++) { ASSERT_EQ(asx_channel_try_reserve(ch, &permits[i]), ASX_OK); }
    ASSERT_EQ(asx_channel_try_reserve(ch, &overflow), ASX_E_CHANNEL_FULL);

    ASSERT_EQ(asx_send_permit_send(&permits[0], 10u), ASX_OK);
    asx_send_permit_abort(&permits[1]);
    ASSERT_EQ(asx_send_permit_send(&permits[2], 30u), ASX_OK);

    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, 2u);
    ASSERT_EQ(asx_channel_reserved_count(ch, &res), ASX_OK);
    ASSERT_EQ(res, 2u);

    ASSERT_EQ(asx_channel_try_reserve(ch, &overflow), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&overflow, 60u), ASX_OK);
    asx_send_permit_abort(&permits[3]);
    ASSERT_EQ(asx_send_permit_send(&permits[4], 50u), ASX_OK);

    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, 4u);
    ASSERT_EQ(asx_channel_reserved_count(ch, &res), ASX_OK);
    ASSERT_EQ(res, 0u);

    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, 10u);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, 30u);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, 60u);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, 50u);
}

TEST(scripted_multi_producer_fifo_stress) {
    enum { PRODUCERS = 4, PER_PRODUCER = 16, TOTAL = PRODUCERS * PER_PRODUCER };
    asx_channel_id ch;
    asx_send_permit permit;
    uint64_t val;
    uint32_t producer;
    uint32_t seq;
    uint32_t expected_idx;

    setup();
    ASSERT_EQ(asx_channel_create(g_rid, TOTAL, &ch), ASX_OK);

    for (seq = 0; seq < PER_PRODUCER; seq++) {
        for (producer = 0; producer < PRODUCERS; producer++) {
            ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);
            ASSERT_EQ(asx_send_permit_send(&permit, (uint64_t)(producer * 1000u + seq)), ASX_OK);
        }
    }

    for (expected_idx = 0; expected_idx < TOTAL; expected_idx++) {
        producer = expected_idx % PRODUCERS;
        seq = expected_idx / PRODUCERS;
        ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
        ASSERT_EQ(val, (uint64_t)(producer * 1000u + seq));
    }
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_WOULD_BLOCK);
}

#if defined(ASX_MPSC_PTHREAD_STRESS)
enum { PTHREAD_PRODUCERS = 4, PTHREAD_PER_PRODUCER = ASX_CHANNEL_MAX_CAPACITY / 4 };

typedef struct {
    asx_channel_id ch;
    uint32_t producer;
    uint32_t failures;
} pthread_producer_args;

static void *pthread_producer_main(void *arg) {
    pthread_producer_args *pa;
    asx_send_permit permit;
    uint32_t seq;
    asx_status st;

    pa = (pthread_producer_args *)arg;
    for (seq = 0; seq < PTHREAD_PER_PRODUCER; seq++) {
        st = asx_channel_try_reserve(pa->ch, &permit);
        if (st != ASX_OK) {
            pa->failures++;
            return NULL;
        }
        st = asx_send_permit_send(&permit, (uint64_t)(pa->producer * 1000u + seq));
        if (st != ASX_OK) {
            pa->failures++;
            return NULL;
        }
    }

    return NULL;
}

TEST(pthread_multi_producer_single_consumer_stress) {
    enum { TOTAL = PTHREAD_PRODUCERS * PTHREAD_PER_PRODUCER };
    asx_channel_id ch;
    pthread_t threads[PTHREAD_PRODUCERS];
    pthread_producer_args args[PTHREAD_PRODUCERS];
    uint8_t seen[PTHREAD_PRODUCERS][PTHREAD_PER_PRODUCER];
    uint64_t val;
    uint32_t producer;
    uint32_t seq;
    uint32_t i;
    uint32_t len;

    setup();
    ASSERT_EQ(asx_channel_create(g_rid, TOTAL, &ch), ASX_OK);
    memset(seen, 0, sizeof(seen));

    for (i = 0; i < PTHREAD_PRODUCERS; i++) {
        args[i].ch = ch;
        args[i].producer = i;
        args[i].failures = 0u;
        ASSERT_EQ((uint32_t)pthread_create(&threads[i], NULL, pthread_producer_main, &args[i]), 0u);
    }

    for (i = 0; i < PTHREAD_PRODUCERS; i++) {
        ASSERT_EQ((uint32_t)pthread_join(threads[i], NULL), 0u);
        ASSERT_EQ(args[i].failures, 0u);
    }

    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, (uint32_t)TOTAL);

    for (i = 0; i < TOTAL; i++) {
        ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
        producer = (uint32_t)(val / 1000u);
        seq = (uint32_t)(val % 1000u);
        ASSERT_TRUE(producer < PTHREAD_PRODUCERS);
        ASSERT_TRUE(seq < PTHREAD_PER_PRODUCER);
        ASSERT_EQ(seen[producer][seq], (uint8_t)0u);
        seen[producer][seq] = 1u;
    }

    for (producer = 0; producer < PTHREAD_PRODUCERS; producer++) {
        for (seq = 0; seq < PTHREAD_PER_PRODUCER; seq++) { ASSERT_EQ(seen[producer][seq], 1u); }
    }
}
#endif

/* -------------------------------------------------------------------
 * Two-phase send: abort
 * ------------------------------------------------------------------- */

TEST(abort_returns_capacity) {
    asx_channel_id ch;
    asx_send_permit p1, p2;
    uint32_t res;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 1, &ch), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &p1), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p2), ASX_E_CHANNEL_FULL);

    asx_send_permit_abort(&p1);
    ASSERT_EQ(asx_channel_reserved_count(ch, &res), ASX_OK);
    ASSERT_EQ(res, 0u);

    ASSERT_EQ(asx_channel_try_reserve(ch, &p2), ASX_OK);
    asx_send_permit_abort(&p2);
}

TEST(abort_null_is_safe) {
    asx_send_permit_abort(NULL);
    ASSERT_TRUE(1);
}

TEST(abort_consumed_is_noop) {
    asx_channel_id ch;
    asx_send_permit p;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 99), ASX_OK);
    asx_send_permit_abort(&p);

    ASSERT_TRUE(1);
}

/* -------------------------------------------------------------------
 * Receive
 * ------------------------------------------------------------------- */

TEST(recv_empty_returns_would_block) {
    asx_channel_id ch;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_WOULD_BLOCK);
}

TEST(recv_null_out) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(recv_after_sender_closed_with_data) {
    asx_channel_id ch;
    asx_send_permit p;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 77), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);

    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, 77u);

    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_DISCONNECTED);
}

TEST(recv_after_sender_closed_empty) {
    asx_channel_id ch;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_DISCONNECTED);
}

TEST(recv_sender_closed_with_pending_reserve) {
    /* When sender is closed but a reserved permit is still outstanding,
     * recv should return WOULD_BLOCK (not DISCONNECTED) because the
     * permit holder may still call send. */
    asx_channel_id ch;
    asx_send_permit p;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);

    /* Queue is empty but there's a pending reserve → WOULD_BLOCK */
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_WOULD_BLOCK);

    /* Complete the send */
    ASSERT_EQ(asx_send_permit_send(&p, 55), ASX_OK);

    /* Now the data is available */
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, 55u);

    /* No more reserves, no more data → DISCONNECTED */
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_DISCONNECTED);
}

TEST(recv_sender_closed_pending_reserve_abort) {
    /* When the pending reserve is aborted (not sent), recv should
     * transition to DISCONNECTED once reserved count hits zero. */
    asx_channel_id ch;
    asx_send_permit p;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);

    /* Pending reserve → WOULD_BLOCK */
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_WOULD_BLOCK);

    /* Abort the permit instead of sending */
    asx_send_permit_abort(&p);

    /* No more reserves, queue empty → DISCONNECTED */
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_DISCONNECTED);
}

TEST(recv_after_receiver_closed) {
    /* Recv after the receiver side is closed → DISCONNECTED */
    asx_channel_id ch;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_DISCONNECTED);
}

TEST(recv_after_fully_closed) {
    /* Recv after both sides closed → DISCONNECTED */
    asx_channel_id ch;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_DISCONNECTED);
}

/* -------------------------------------------------------------------
 * Ring buffer wraparound
 * ------------------------------------------------------------------- */

TEST(ring_buffer_wraparound) {
    asx_channel_id ch;
    asx_send_permit p;
    uint64_t val;
    uint32_t i;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    for (i = 0; i < 20; i++) {
        ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
        ASSERT_EQ(asx_send_permit_send(&p, (uint64_t)(i + 1000)), ASX_OK);
        ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
        ASSERT_EQ(val, (uint64_t)(i + 1000));
    }
}

TEST(ring_buffer_batch_wraparound) {
    asx_channel_id ch;
    asx_send_permit permits[4];
    uint64_t val;
    uint32_t i, round;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    for (round = 0; round < 5; round++) {
        for (i = 0; i < 4; i++) {
            ASSERT_EQ(asx_channel_try_reserve(ch, &permits[i]), ASX_OK);
            ASSERT_EQ(asx_send_permit_send(&permits[i], (uint64_t)(round * 100 + i)), ASX_OK);
        }
        for (i = 0; i < 4; i++) {
            ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
            ASSERT_EQ(val, (uint64_t)(round * 100 + i));
        }
    }
}

/* -------------------------------------------------------------------
 * Disconnect scenarios
 * ------------------------------------------------------------------- */

TEST(reserve_after_sender_closed) {
    asx_channel_id ch;
    asx_send_permit p;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_E_INVALID_STATE);
}

TEST(reserve_after_receiver_closed) {
    asx_channel_id ch;
    asx_send_permit p;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_E_DISCONNECTED);
}

TEST(send_after_receiver_closed) {
    asx_channel_id ch;
    asx_send_permit p;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 123), ASX_E_DISCONNECTED);
}

TEST(close_receiver_discards_queued_messages) {
    asx_channel_id ch;
    asx_send_permit p;
    uint32_t len;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 42), ASX_OK);
    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, 1u);

    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, 0u);
}

TEST(close_receiver_discards_queue_but_not_pending_reserve) {
    asx_channel_id ch;
    asx_send_permit p1;
    asx_send_permit p2;
    asx_send_permit pending;
    uint32_t len;
    uint32_t res;

    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 3, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p1), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p1, 1u), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p2), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p2, 2u), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &pending), ASX_OK);

    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, 0u);
    ASSERT_EQ(asx_channel_reserved_count(ch, &res), ASX_OK);
    ASSERT_EQ(res, 1u);

    ASSERT_EQ(asx_send_permit_send(&pending, 3u), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_channel_reserved_count(ch, &res), ASX_OK);
    ASSERT_EQ(res, 0u);
}

/* -------------------------------------------------------------------
 * Query NULL safety
 * ------------------------------------------------------------------- */

TEST(get_state_null_out) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(queue_len_null_out) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_queue_len(ch, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(reserved_count_null_out) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_reserved_count(ch, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(reserve_null_out) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(send_null_permit) { ASSERT_EQ(asx_send_permit_send(NULL, 0), ASX_E_INVALID_ARGUMENT); }

TEST(double_send_same_permit) {
    asx_channel_id ch;
    asx_send_permit p;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 1), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 2), ASX_E_INVALID_STATE);
}

TEST(forged_permit_send_rejected) {
    asx_channel_id ch;
    asx_send_permit forged;
    uint32_t len;
    uint32_t res;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    forged.channel_id = ch;
    forged.token = 0xC0FFEEu;
    forged.consumed = 0;
    forged.obligation = ASX_INVALID_ID;

    ASSERT_EQ(asx_send_permit_send(&forged, 123u), ASX_E_INVALID_STATE);
    ASSERT_EQ(forged.consumed, 1);
    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, 0u);
    ASSERT_EQ(asx_channel_reserved_count(ch, &res), ASX_OK);
    ASSERT_EQ(res, 0u);
}

TEST(stale_permit_copy_cannot_send) {
    asx_channel_id ch;
    asx_send_permit original;
    asx_send_permit stale_copy;
    uint32_t len;
    uint32_t res;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &original), ASX_OK);
    stale_copy = original;
    asx_send_permit_abort(&original);

    ASSERT_EQ(asx_send_permit_send(&stale_copy, 999u), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, 0u);
    ASSERT_EQ(asx_channel_reserved_count(ch, &res), ASX_OK);
    ASSERT_EQ(res, 0u);
}

/* -------------------------------------------------------------------
 * Reset
 * ------------------------------------------------------------------- */

TEST(reset_clears_all) {
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    asx_channel_reset();

    ASSERT_NE(asx_channel_get_state(ch, &st), ASX_OK);
}

/* asx_channel_reset() visits only the slots created since the last reset.
 * After epochs of different footprint, a channel created in any slot must
 * start empty with all its capacity and no outstanding permit. */
#define CH_EPOCH_MAX ((uint32_t)(ASX_MAX_CHANNELS < 6u ? ASX_MAX_CHANNELS : 6u))

/* n channels with three queued values and one reserved permit each */
static int ch_epoch_dirty(uint32_t n) {
    uint32_t i;
    uint32_t k;

    for (i = 0u; i < n; i++) {
        asx_channel_id ch;
        asx_send_permit p;

        if (asx_channel_create(g_rid, 4, &ch) != ASX_OK) return 0;
        for (k = 0u; k < 3u; k++) {
            if (asx_channel_try_reserve(ch, &p) != ASX_OK) return 0;
            if (asx_send_permit_send(&p, 100u + k) != ASX_OK) return 0;
        }
        if (asx_channel_try_reserve(ch, &p) != ASX_OK) return 0;
    }
    return 1;
}

/* n channels that are empty, open, with their full capacity free */
static int ch_epoch_fresh(uint32_t n) {
    uint32_t i;
    uint32_t k;

    for (i = 0u; i < n; i++) {
        asx_channel_id ch;
        asx_channel_state state;
        asx_send_permit p;
        uint32_t len = 99u;
        uint32_t reserved = 99u;
        uint64_t val = 0u;

        if (asx_channel_create(g_rid, 4, &ch) != ASX_OK) return 0;
        if (asx_channel_get_state(ch, &state) != ASX_OK || state != ASX_CHANNEL_OPEN) return 0;
        if (asx_channel_queue_len(ch, &len) != ASX_OK || len != 0u) return 0;
        if (asx_channel_reserved_count(ch, &reserved) != ASX_OK || reserved != 0u) return 0;
        if (asx_channel_try_recv(ch, &val) != ASX_E_WOULD_BLOCK) return 0;
        for (k = 0u; k < 4u; k++) {
            if (asx_channel_try_reserve(ch, &p) != ASX_OK) return 0;
        }
        if (asx_channel_try_reserve(ch, &p) == ASX_OK) return 0; /* capacity is 4, not more */
    }
    return 1;
}

TEST(reset_after_heavy_use_leaves_no_stale_state) {
    setup();
    ASSERT_TRUE(ch_epoch_dirty(CH_EPOCH_MAX));
    asx_channel_reset();
    ASSERT_TRUE(ch_epoch_fresh(CH_EPOCH_MAX));

    /* a light epoch dirties only the low slots; the high ones were cleared
     * by the reset before and must still be clean */
    asx_channel_reset();
    ASSERT_TRUE(ch_epoch_dirty(2u));
    asx_channel_reset();
    ASSERT_TRUE(ch_epoch_fresh(CH_EPOCH_MAX));

    asx_channel_reset();
    ASSERT_TRUE(ch_epoch_dirty(CH_EPOCH_MAX));
    asx_channel_reset();
    ASSERT_TRUE(ch_epoch_fresh(CH_EPOCH_MAX));
}

/* -------------------------------------------------------------------
 * Cx-aware waits (Rust reserve / send / recv) and SendPermit obligations
 * ------------------------------------------------------------------- */

static asx_status poll_idle(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_PENDING;
}

/* Cancellation and untracked API probes can name a live, idle task.
 * Tracked permit resolution below runs inside the holder's real poll. */
static asx_task_id cx_task(asx_cx *cx) {
    asx_task_id t = ASX_INVALID_ID;
    CH_IGNORE(asx_task_spawn(g_rid, poll_idle, NULL, &t));
    asx_cx_init(cx, g_rid, t, ASX_CAP_NONE);
    return t;
}

static int obligation_is(asx_obligation_id id, asx_task_id holder, asx_obligation_state state,
                         asx_obligation_abort_reason reason) {
    asx_obligation_info info;
    if (id == ASX_INVALID_ID || asx_obligation_get_info(id, &info) != ASX_OK) return 0;
    return info.kind == ASX_OBLIGATION_KIND_SEND_PERMIT && info.state == state &&
           info.abort_reason == reason && asx_handle_index(info.holder) == asx_handle_index(holder);
}

/* The last event the trace observer saw, with a user trace's text. */
static asx_trace_event g_last_event;
static char g_last_text[64];

static void observe_last(void *ctx, const asx_trace_event *event,
                         const asx_trace_payload *payload) {
    (void)ctx;
    g_last_event = *event;
    g_last_text[0] = '\0';
    if (payload->text != NULL) {
        size_t n = strlen(payload->text);
        if (n >= sizeof(g_last_text)) n = sizeof(g_last_text) - 1u;
        memcpy(g_last_text, payload->text, n);
        g_last_text[n] = '\0';
    }
}

static int last_event_is_user_trace(asx_task_id task, const char *message) {
    return g_last_event.kind == ASX_TRACE_USER && g_last_event.entity_id == task &&
           strcmp(g_last_text, message) == 0;
}

static void check_tracked_permit_commit(asx_channel_id ch, asx_task_id t) {
    asx_send_permit p;
    asx_cx cx;
    uint64_t v = 0;
    ASSERT_EQ(asx_cx_init(&cx, g_rid, t, ASX_CAP_NONE), ASX_OK);
    ASSERT_EQ(asx_channel_reserve(ch, &cx, &p), ASX_OK);
    ASSERT_TRUE(obligation_is(p.obligation, t, ASX_OBLIGATION_RESERVED, ASX_OBLIGATION_ABORT_NONE));
    {
        asx_obligation_id ob = p.obligation;
        ASSERT_EQ(asx_send_permit_send(&p, 7u), ASX_OK);
        ASSERT_TRUE(obligation_is(ob, t, ASX_OBLIGATION_COMMITTED, ASX_OBLIGATION_ABORT_NONE));
    }
    ASSERT_EQ(asx_channel_recv(ch, &cx, &v), ASX_OK);
    ASSERT_EQ(v, 7u);
}

static asx_status poll_tracked_permit_commit(void *data, asx_task_id self) {
    check_tracked_permit_commit(*(asx_channel_id *)data, self);
    return test_current_failed ? ASX_E_INVALID_STATE : ASX_OK;
}

TEST(reserve_with_cx_registers_send_permit_committed_by_send) {
    asx_channel_id ch;
    asx_task_id t;
    asx_budget budget = asx_budget_from_polls(8u);
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_tracked_permit_commit, &ch, &t), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(g_rid, &budget), ASX_OK);
}

static void check_tracked_permit_abort_and_disconnect(asx_channel_id ch, asx_task_id t) {
    asx_send_permit p1;
    asx_send_permit p2;
    asx_obligation_id ob1;
    asx_obligation_id ob2;
    asx_cx cx;
    ASSERT_EQ(asx_cx_init(&cx, g_rid, t, ASX_CAP_NONE), ASX_OK);
    ASSERT_EQ(asx_channel_reserve(ch, &cx, &p1), ASX_OK);
    ASSERT_EQ(asx_channel_reserve(ch, &cx, &p2), ASX_OK);
    ob1 = p1.obligation;
    ob2 = p2.obligation;
    ASSERT_TRUE(ob1 != ob2);

    /* permit.abort(): Explicit (mpsc.rs:1633). */
    asx_send_permit_abort(&p1);
    ASSERT_TRUE(obligation_is(ob1, t, ASX_OBLIGATION_ABORTED, ASX_OBLIGATION_ABORT_EXPLICIT));

    /* Sending into a closed receiver: Disconnected, aborted Error (:1477). */
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p2, 1u), ASX_E_DISCONNECTED);
    ASSERT_TRUE(obligation_is(ob2, t, ASX_OBLIGATION_ABORTED, ASX_OBLIGATION_ABORT_ERROR));
}

static asx_status poll_tracked_permit_abort_and_disconnect(void *data, asx_task_id self) {
    check_tracked_permit_abort_and_disconnect(*(asx_channel_id *)data, self);
    return test_current_failed ? ASX_E_INVALID_STATE : ASX_OK;
}

TEST(reserved_permit_abort_and_disconnect_abort_the_obligation) {
    asx_channel_id ch;
    asx_task_id t;
    asx_budget budget = asx_budget_from_polls(8u);
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_tracked_permit_abort_and_disconnect, &ch, &t), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(g_rid, &budget), ASX_OK);
}

typedef struct {
    asx_channel_id channel;
    asx_send_permit permit;
    asx_obligation_id obligation;
    asx_task_id holder;
    uint32_t phase;
    int abort_after_rejection;
    int settle_in_other_task;
    int wrong_task_checked;
} tracked_permit_misuse;

static void check_tracked_permit_holder(tracked_permit_misuse *ctx, asx_task_id self) {
    asx_cx cx;
    uint32_t reserved;
    ASSERT_EQ(asx_handle_index(self), asx_handle_index(ctx->holder));
    if (ctx->phase == 0u) {
        ASSERT_EQ(asx_cx_init(&cx, g_rid, self, ASX_CAP_NONE), ASX_OK);
        ASSERT_EQ(asx_channel_reserve(ctx->channel, &cx, &ctx->permit), ASX_OK);
        ctx->obligation = ctx->permit.obligation;
        ASSERT_TRUE(obligation_is(ctx->obligation, self, ASX_OBLIGATION_RESERVED,
                                  ASX_OBLIGATION_ABORT_NONE));
        ctx->phase = 1u;
        ASSERT_EQ(asx_task_park(self), ASX_OK);
        return;
    }
    ASSERT_EQ(ctx->phase, 1u);
    ASSERT_TRUE(ctx->wrong_task_checked);
    if (ctx->abort_after_rejection && !ctx->settle_in_other_task) {
        asx_send_permit_abort(&ctx->permit);
    } else if (!ctx->settle_in_other_task) {
        ASSERT_EQ(asx_send_permit_send(&ctx->permit, 77u), ASX_OK);
    }
    ASSERT_TRUE(obligation_is(ctx->obligation, self,
                              ctx->abort_after_rejection ? ASX_OBLIGATION_ABORTED
                                                         : ASX_OBLIGATION_COMMITTED,
                              ctx->abort_after_rejection ? ASX_OBLIGATION_ABORT_EXPLICIT
                                                         : ASX_OBLIGATION_ABORT_NONE));
    ASSERT_EQ(ctx->permit.consumed, 1);
    ASSERT_EQ(asx_channel_reserved_count(ctx->channel, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 0u);
    ctx->phase = 2u;
}

static asx_status poll_tracked_permit_holder(void *data, asx_task_id self) {
    tracked_permit_misuse *ctx = (tracked_permit_misuse *)data;
    check_tracked_permit_holder(ctx, self);
    if (test_current_failed) return ASX_E_INVALID_STATE;
    return ctx->phase == 2u ? ASX_OK : ASX_E_PENDING;
}

static void check_forged_tracked_permit_binding(tracked_permit_misuse *ctx) {
    asx_channel_id foreign_channel;
    asx_send_permit forged;
    asx_send_permit before;
    asx_obligation_info before_info;
    asx_obligation_info info;
    uint32_t reserved;
    uint32_t queued;
    uint64_t emitted;
    uint64_t digest;
    memset(&forged, 0, sizeof(forged));
    ASSERT_EQ(asx_channel_create(g_rid, 1u, &foreign_channel), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(foreign_channel, &forged), ASX_OK);
    /* A genuine credit cannot authorize a different physical reservation. */
    forged.obligation = ctx->obligation;
    memcpy(&before, &forged, sizeof(before));
    ASSERT_EQ(asx_obligation_get_info(ctx->obligation, &before_info), ASX_OK);
    emitted = asx_trace_emitted_total();
    digest = asx_trace_digest();
    ASSERT_EQ(asx_send_permit_send(&forged, 999u), ASX_E_PERMISSION_DENIED);
    asx_send_permit_abort(&forged);
    ASSERT_EQ(memcmp(&forged, &before, sizeof(forged)), 0);
    ASSERT_TRUE(obligation_is(ctx->obligation, ctx->holder, ASX_OBLIGATION_RESERVED,
                              ASX_OBLIGATION_ABORT_NONE));
    ASSERT_EQ(asx_obligation_get_info(ctx->obligation, &info), ASX_OK);
    ASSERT_EQ(info.kind, before_info.kind);
    ASSERT_EQ(info.state, before_info.state);
    ASSERT_EQ(info.abort_reason, before_info.abort_reason);
    ASSERT_EQ(info.holder, before_info.holder);
    ASSERT_EQ(info.region, before_info.region);
    ASSERT_EQ(asx_channel_reserved_count(foreign_channel, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 1u);
    ASSERT_EQ(asx_channel_queue_len(foreign_channel, &queued), ASX_OK);
    ASSERT_EQ(queued, 0u);
    ASSERT_EQ(asx_channel_reserved_count(ctx->channel, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 1u);
    ASSERT_EQ(asx_trace_emitted_total(), emitted);
    ASSERT_EQ(asx_trace_digest(), digest);
    /* Restore the independently owned untracked permit and return its slot. */
    forged.obligation = ASX_INVALID_ID;
    asx_send_permit_abort(&forged);
    ASSERT_EQ(asx_channel_reserved_count(foreign_channel, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 0u);
    ASSERT_EQ(asx_channel_close_sender(foreign_channel), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(foreign_channel), ASX_OK);
}

static void check_wrong_task_permit_attempts(tracked_permit_misuse *ctx, asx_task_id self) {
    asx_send_permit before;
    asx_send_permit probe;
    asx_channel_state state;
    asx_obligation_info info;
    uint32_t queued;
    uint32_t reserved;
    uint64_t emitted;
    uint64_t digest;
    ASSERT_NE(asx_handle_index(self), asx_handle_index(ctx->holder));
    ASSERT_EQ(ctx->phase, 1u);
    memcpy(&before, &ctx->permit, sizeof(before));
    emitted = asx_trace_emitted_total();
    digest = asx_trace_digest();
    /* A raw ledger ID is not the owned physical permit capability. */
    ASSERT_EQ(asx_obligation_commit(ctx->obligation), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_obligation_abort(ctx->obligation), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_obligation_drop(ctx->obligation), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(ctx->permit.channel_id, before.channel_id);
    ASSERT_EQ(ctx->permit.token, before.token);
    ASSERT_EQ(ctx->permit.consumed, before.consumed);
    ASSERT_EQ(ctx->permit.obligation, before.obligation);
    ASSERT_EQ(memcmp(&ctx->permit, &before, sizeof(before)), 0);
    ASSERT_TRUE(obligation_is(ctx->obligation, ctx->holder, ASX_OBLIGATION_RESERVED,
                              ASX_OBLIGATION_ABORT_NONE));
    ASSERT_EQ(asx_obligation_get_info(ctx->obligation, &info), ASX_OK);
    ASSERT_EQ(info.region, g_rid);
    ASSERT_EQ(asx_channel_get_state(ctx->channel, &state), ASX_OK);
    ASSERT_EQ(state, ASX_CHANNEL_OPEN);
    ASSERT_EQ(asx_channel_queue_len(ctx->channel, &queued), ASX_OK);
    ASSERT_EQ(queued, 0u);
    ASSERT_EQ(asx_channel_reserved_count(ctx->channel, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 1u);
    ASSERT_EQ(asx_trace_emitted_total(), emitted);
    ASSERT_EQ(asx_trace_digest(), digest);
    ASSERT_EQ(asx_channel_try_reserve(ctx->channel, &probe), ASX_E_CHANNEL_FULL);
    check_forged_tracked_permit_binding(ctx);
    if (test_current_failed) return;
    if (ctx->settle_in_other_task) {
        /* Moving the genuine permit does not move holder liability, but
         * its physical channel/token capability may settle the credit. */
        if (ctx->abort_after_rejection) {
            asx_send_permit_abort(&ctx->permit);
        } else {
            ASSERT_EQ(asx_send_permit_send(&ctx->permit, 77u), ASX_OK);
        }
        ASSERT_TRUE(obligation_is(ctx->obligation, ctx->holder,
                                  ctx->abort_after_rejection ? ASX_OBLIGATION_ABORTED
                                                             : ASX_OBLIGATION_COMMITTED,
                                  ctx->abort_after_rejection ? ASX_OBLIGATION_ABORT_EXPLICIT
                                                             : ASX_OBLIGATION_ABORT_NONE));
    }
    ctx->wrong_task_checked = 1;
    ASSERT_EQ(asx_task_wake(ctx->holder), ASX_OK);
}

static asx_status poll_wrong_task_permit_attempts(void *data, asx_task_id self) {
    check_wrong_task_permit_attempts((tracked_permit_misuse *)data, self);
    return test_current_failed ? ASX_E_INVALID_STATE : ASX_OK;
}

static void run_tracked_permit_misuse(int abort_after_rejection, int settle_in_other_task) {
    tracked_permit_misuse ctx;
    asx_task_id other;
    asx_send_permit probe;
    asx_budget budget = asx_budget_from_polls(8u);
    uint64_t value = 0u;
    uint32_t reserved;
    setup();
    memset(&ctx, 0, sizeof(ctx));
    ctx.abort_after_rejection = abort_after_rejection;
    ctx.settle_in_other_task = settle_in_other_task;
    ASSERT_EQ(asx_channel_create(g_rid, 1u, &ctx.channel), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_tracked_permit_holder, &ctx, &ctx.holder), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_wrong_task_permit_attempts, &ctx, &other), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(g_rid, &budget), ASX_OK);
    ASSERT_EQ(ctx.phase, 2u);
    ASSERT_TRUE(ctx.wrong_task_checked);
    if (abort_after_rejection) {
        ASSERT_EQ(asx_channel_try_recv(ctx.channel, &value), ASX_E_WOULD_BLOCK);
    } else {
        ASSERT_EQ(asx_channel_try_recv(ctx.channel, &value), ASX_OK);
        ASSERT_EQ(value, 77u);
    }
    ASSERT_EQ(asx_channel_try_reserve(ctx.channel, &probe), ASX_OK);
    asx_send_permit_abort(&probe);
    ASSERT_EQ(asx_channel_reserved_count(ctx.channel, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 0u);
}

TEST(tracked_permit_credit_rejects_wrong_task_before_holder_send) {
    run_tracked_permit_misuse(0, 0);
}

TEST(tracked_permit_credit_rejects_wrong_task_before_holder_abort) {
    run_tracked_permit_misuse(1, 0);
}

TEST(tracked_permit_moved_to_another_task_can_send_without_reassigning_holder) {
    run_tracked_permit_misuse(0, 1);
}

TEST(tracked_permit_moved_to_another_task_can_abort_without_reassigning_holder) {
    run_tracked_permit_misuse(1, 1);
}

TEST(untracked_reserves_register_no_obligation) {
    /* try_reserve (Rust try_reserve, mpsc.rs:749), the reserve inside send
     * (TransientReserve, :1186) and a Cx without a task stay untracked. */
    asx_channel_id ch;
    asx_send_permit p;
    asx_cx cx;
    asx_cx no_task;
    asx_obligation_id probe;
    asx_obligation_id next;
    uint64_t v = 0;
    setup();
    (void)cx_task(&cx);
    asx_cx_init(&no_task, g_rid, ASX_INVALID_ID, ASX_CAP_NONE);
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(g_rid, &probe), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(p.obligation, ASX_INVALID_ID);
    ASSERT_EQ(asx_send_permit_send(&p, 1u), ASX_OK);
    ASSERT_EQ(asx_channel_reserve(ch, &no_task, &p), ASX_OK);
    ASSERT_EQ(p.obligation, ASX_INVALID_ID);
    ASSERT_EQ(asx_send_permit_send(&p, 2u), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, &cx, 3u), ASX_OK);

    /* No obligation was reserved between the two probes. */
    ASSERT_EQ(asx_obligation_reserve(g_rid, &next), ASX_OK);
    ASSERT_EQ(asx_handle_index(next), asx_handle_index(probe) + 1u);
    ASSERT_EQ(asx_obligation_commit(probe), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(next), ASX_OK);
    ASSERT_EQ(asx_channel_recv(ch, NULL, &v), ASX_OK);
    ASSERT_EQ(v, 1u);
    ASSERT_EQ(asx_channel_recv(ch, NULL, &v), ASX_OK);
    ASSERT_EQ(v, 2u);
    ASSERT_EQ(asx_channel_recv(ch, NULL, &v), ASX_OK);
    ASSERT_EQ(v, 3u);
    ASSERT_EQ(asx_channel_recv(ch, NULL, &v), ASX_E_PENDING);
}

TEST(cancelled_cx_wins_over_ready_channel_and_traces) {
    /* Rust checks cancellation first on every poll: a cancelled recv sees
     * ASX_E_CANCELLED although a value is queued (mpsc.rs:1786), a
     * cancelled reserve although there is room (:1061), and each records
     * its user trace. The queued value stays for a later receiver. */
    asx_channel_id ch;
    asx_send_permit p;
    asx_cx cx;
    asx_task_id t;
    uint64_t v = 0;
    uint32_t reserved = 99;
    setup();
    t = cx_task(&cx);
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 5u), ASX_OK);
    ASSERT_EQ(asx_task_cancel(t, ASX_CANCEL_USER), ASX_OK);
    asx_trace_set_observer(observe_last, NULL);

    ASSERT_EQ(asx_channel_recv(ch, &cx, &v), ASX_E_CANCELLED);
    ASSERT_TRUE(last_event_is_user_trace(t, "mpsc::recv cancelled"));
    ASSERT_EQ(asx_channel_reserve(ch, &cx, &p), ASX_E_CANCELLED);
    ASSERT_TRUE(last_event_is_user_trace(t, "mpsc::reserve cancelled"));
    ASSERT_EQ(asx_channel_send(ch, &cx, 6u), ASX_E_CANCELLED);
    ASSERT_TRUE(last_event_is_user_trace(t, "mpsc::reserve cancelled"));
    asx_trace_set_observer(NULL, NULL);
    ASSERT_EQ(asx_channel_reserved_count(ch, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 0u);

    ASSERT_EQ(asx_channel_recv(ch, NULL, &v), ASX_OK);
    ASSERT_EQ(v, 5u);
}

/* -------------------------------------------------------------------
 * Receiver close (seal), wake_receiver, creation in a closing region
 * ------------------------------------------------------------------- */

TEST(seal_keeps_the_queue_receivable) {
    /* Rust rx.close() (mpsc.rs:1698): sends and outstanding permits see
     * Disconnected, the queued messages stay receivable, then a receive
     * sees Disconnected. asx_channel_close_receiver (the drop) would have
     * discarded them. */
    asx_channel_id ch;
    asx_send_permit p;
    asx_channel_state cs;
    uint64_t v = 0;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 1u), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 2u), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_channel_seal(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &cs), ASX_OK);
    ASSERT_EQ(cs, ASX_CHANNEL_RECEIVER_CLOSED);
    ASSERT_EQ(asx_channel_send(ch, NULL, 3u), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_send_permit_send(&p, 4u), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, 1u);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, 2u);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_channel_seal(ch), ASX_E_INVALID_STATE);
}

TEST(sealed_channel_slot_is_not_reused_until_drained) {
    /* Sealed then sender-closed with a message left: the slot stays the
     * channel's until the message is received. */
    asx_channel_id ch;
    asx_channel_id other;
    uint64_t v = 0;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 9u), ASX_OK);
    ASSERT_EQ(asx_channel_seal(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_create(g_rid, 2, &other), ASX_OK);
    ASSERT_TRUE(other != ch);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, 9u);
}

TEST(receiver_drop_after_seal_discards_the_queue_and_frees_the_slot) {
    /* Rust's Receiver::drop after close() takes the queue (mpsc.rs:2138).
     * C kept it: a sealed channel whose receiver and last sender were
     * dropped with messages queued held its slot for good. */
    asx_channel_id ch;
    asx_channel_id other;
    uint32_t len = 99u;
    uint64_t v = 0;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 9u), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 10u), ASX_OK);
    ASSERT_EQ(asx_channel_seal(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, 0u);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_E_DISCONNECTED);
    /* A second drop, and a seal after the drop, are refused. */
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_channel_seal(ch), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    /* Fully closed and empty, the slot is handed out again: the old
     * handle is stale. */
    ASSERT_EQ(asx_channel_create(g_rid, 2, &other), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_E_STALE_HANDLE);
}

static uint32_t g_recv_polls;
static asx_channel_id g_recv_ch;

static asx_status poll_recv_counting(void *ud, asx_task_id self) {
    uint64_t v = 0;
    (void)ud;
    (void)self;
    g_recv_polls++;
    return asx_channel_recv(g_recv_ch, NULL, &v) == ASX_OK ? ASX_OK : ASX_E_PENDING;
}

static void run_region(void) {
    asx_budget b = asx_budget_from_polls(16);
    CH_IGNORE(asx_scheduler_run(g_rid, &b));
}

TEST(wake_receiver_repolls_a_parked_receiver) {
    /* Rust tx.wake_receiver() (mpsc.rs:808): the parked receiver is polled
     * again without a message; it parks again. */
    asx_task_id t;
    asx_task_state ts;
    setup();
    g_recv_polls = 0;
    ASSERT_EQ(asx_channel_create(g_rid, 2, &g_recv_ch), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_recv_counting, NULL, &t), ASX_OK);
    run_region();
    ASSERT_EQ(g_recv_polls, 1u);
    run_region();
    ASSERT_EQ(g_recv_polls, 1u); /* parked: nothing wakes it */
    ASSERT_EQ(asx_channel_wake_receiver(g_recv_ch), ASX_OK);
    run_region();
    ASSERT_EQ(g_recv_polls, 2u);
    ASSERT_EQ(asx_channel_send(g_recv_ch, NULL, 5u), ASX_OK);
    run_region();
    ASSERT_EQ(g_recv_polls, 3u);
    ASSERT_EQ(asx_task_get_state(t, &ts), ASX_OK);
    ASSERT_EQ(ts, ASX_TASK_COMPLETED);
}

TEST(create_accepts_a_closing_region) {
    /* Rust's channels belong to no region: only a closed one refuses
     * (create_rejects_closed_region). */
    asx_region_id r;
    asx_region_state rs;
    asx_task_id t;
    asx_channel_id ch;
    asx_cancel_reason reason;
    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_idle, NULL, &t), ASX_OK);
    reason = asx_cancel_reason_default(ASX_CANCEL_USER, NULL);
    ASSERT_EQ(asx_region_cancel(r, &reason, NULL), ASX_OK);
    ASSERT_EQ(asx_region_get_state(r, &rs), ASX_OK);
    ASSERT_TRUE(rs != ASX_REGION_OPEN && rs != ASX_REGION_CLOSED);
    ASSERT_EQ(asx_channel_create(r, 4, &ch), ASX_OK);
}

/* -------------------------------------------------------------------
 * send_evict_oldest (mpsc.rs:872)
 * ------------------------------------------------------------------- */

TEST(evict_oldest_sends_without_eviction_while_there_is_room) {
    asx_channel_id ch;
    uint64_t evicted_value = 77u;
    uint64_t v = 0;
    uint32_t len = 0;
    int evicted = 1;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_send_evict_oldest(ch, 5u, &evicted, &evicted_value), ASX_OK);
    ASSERT_EQ(evicted, 0);
    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, 1u);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, 5u);
}

TEST(evict_oldest_replaces_the_oldest_message_when_full) {
    /* Full: the oldest committed message is returned and the new one goes
     * to the back, keeping FIFO order for the rest. */
    asx_channel_id ch;
    uint64_t evicted_value = 0;
    uint64_t v = 0;
    int evicted = 0;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 3, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 1u), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 2u), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 3u), ASX_OK);
    ASSERT_EQ(asx_channel_send_evict_oldest(ch, 4u, &evicted, &evicted_value), ASX_OK);
    ASSERT_EQ(evicted, 1);
    ASSERT_EQ(evicted_value, 1u);
    ASSERT_EQ(asx_channel_send_evict_oldest(ch, 5u, &evicted, &evicted_value), ASX_OK);
    ASSERT_EQ(evicted, 1);
    ASSERT_EQ(evicted_value, 2u);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, 3u);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, 4u);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, 5u);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_E_WOULD_BLOCK);
}

TEST(evict_oldest_never_evicts_a_reserved_slot) {
    /* Reserved slots are not messages: with every slot reserved the send
     * is refused; with one committed message beside a reservation, that
     * message goes and the permit still owns its slot. */
    asx_channel_id ch;
    asx_send_permit p;
    asx_send_permit q;
    uint64_t evicted_value = 0;
    uint64_t v = 0;
    uint32_t reserved = 0;
    int evicted = 0;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &q), ASX_OK);
    ASSERT_EQ(asx_channel_send_evict_oldest(ch, 9u, &evicted, &evicted_value), ASX_E_CHANNEL_FULL);
    ASSERT_EQ(asx_send_permit_send(&q, 1u), ASX_OK);
    ASSERT_EQ(asx_channel_send_evict_oldest(ch, 2u, &evicted, &evicted_value), ASX_OK);
    ASSERT_EQ(evicted, 1);
    ASSERT_EQ(evicted_value, 1u);
    ASSERT_EQ(asx_channel_reserved_count(ch, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 1u);
    ASSERT_EQ(asx_send_permit_send(&p, 3u), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, 2u);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, 3u);
}

TEST(evict_oldest_errors) {
    asx_channel_id ch;
    asx_channel_id gone;
    uint64_t evicted_value = 0;
    uint64_t v = 0;
    int evicted = 0;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_send_evict_oldest(ch, 1u, NULL, &evicted_value), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_channel_send_evict_oldest(ch, 1u, &evicted, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_channel_send_evict_oldest(ASX_INVALID_ID, 1u, &evicted, &evicted_value),
              ASX_E_INVALID_ARGUMENT);
    /* A sealed receiver refuses, and the queue it keeps is not touched. */
    ASSERT_EQ(asx_channel_send(ch, NULL, 4u), ASX_OK);
    ASSERT_EQ(asx_channel_seal(ch), ASX_OK);
    ASSERT_EQ(asx_channel_send_evict_oldest(ch, 1u, &evicted, &evicted_value), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_channel_try_recv(ch, &v), ASX_OK);
    ASSERT_EQ(v, 4u);
    /* No live sender left. */
    ASSERT_EQ(asx_channel_create(g_rid, 2, &gone), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(gone), ASX_OK);
    ASSERT_EQ(asx_channel_send_evict_oldest(gone, 1u, &evicted, &evicted_value),
              ASX_E_INVALID_STATE);
}

/* A producer task: yields `delay` polls, then sends `value` with
 * asx_channel_send, parking while the channel is full. */
typedef struct {
    uint32_t delay;
    uint64_t value;
    uint32_t polls;
} mpsc_producer;

static asx_channel_id g_prod_ch;

static asx_status poll_producer(void *ud, asx_task_id self) {
    mpsc_producer *p = (mpsc_producer *)ud;
    (void)self;
    p->polls++;
    if (p->delay > 0u) {
        p->delay--;
        return ASX_E_PENDING;
    }
    return asx_channel_send(g_prod_ch, NULL, p->value) == ASX_OK ? ASX_OK : ASX_E_PENDING;
}

TEST(evict_oldest_refuses_a_slot_a_parked_producer_owns) {
    /* A dequeue frees a slot for the parked producer: send_evict_oldest
     * reports Full and evicts nothing although messages are queued
     * (mpsc.rs:906-911). */
    mpsc_producer prod = {0u, 8u, 0u};
    asx_task_id t;
    uint64_t evicted_value = 0;
    uint64_t v = 0;
    uint32_t len = 0;
    int evicted = 0;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &g_prod_ch), ASX_OK);
    ASSERT_EQ(asx_channel_send(g_prod_ch, NULL, 1u), ASX_OK);
    ASSERT_EQ(asx_channel_send(g_prod_ch, NULL, 2u), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_producer, &prod, &t), ASX_OK);
    run_region();
    ASSERT_EQ(prod.polls, 1u); /* parked on the full channel */
    ASSERT_EQ(asx_channel_try_recv(g_prod_ch, &v), ASX_OK);
    ASSERT_EQ(v, 1u);
    ASSERT_EQ(asx_channel_send_evict_oldest(g_prod_ch, 9u, &evicted, &evicted_value),
              ASX_E_CHANNEL_FULL);
    ASSERT_EQ(asx_channel_queue_len(g_prod_ch, &len), ASX_OK);
    ASSERT_EQ(len, 1u);
    run_region();
    ASSERT_EQ(prod.polls, 2u);
    ASSERT_EQ(asx_channel_try_recv(g_prod_ch, &v), ASX_OK);
    ASSERT_EQ(v, 2u);
    ASSERT_EQ(asx_channel_try_recv(g_prod_ch, &v), ASX_OK);
    ASSERT_EQ(v, 8u);
}

/* -------------------------------------------------------------------
 * recv_many (mpsc.rs:1754, poll :1867)
 * ------------------------------------------------------------------- */

TEST(recv_many_takes_up_to_limit_oldest_first) {
    asx_channel_id ch;
    uint64_t buf[4] = {0u, 0u, 0u, 0u};
    uint32_t n = 99u;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 1u), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 2u), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 3u), ASX_OK);
    ASSERT_EQ(asx_channel_recv_many(ch, NULL, buf, 2u, &n), ASX_OK);
    ASSERT_EQ(n, 2u);
    ASSERT_EQ(buf[0], 1u);
    ASSERT_EQ(buf[1], 2u);
    ASSERT_EQ(asx_channel_recv_many(ch, NULL, buf, 4u, &n), ASX_OK);
    ASSERT_EQ(n, 1u);
    ASSERT_EQ(buf[0], 3u);
    /* Empty and open: not yet. */
    ASSERT_EQ(asx_channel_recv_many(ch, NULL, buf, 4u, &n), ASX_E_PENDING);
    ASSERT_EQ(n, 0u);
}

TEST(recv_many_limit_zero_returns_before_the_cancel_check) {
    /* limit 0 is Ready(Ok(0)) before the checkpoint (mpsc.rs:1874); any
     * other limit sees the cancel first, traced, leaving the queue. */
    asx_channel_id ch;
    asx_cx cx;
    asx_task_id t;
    uint64_t buf[2] = {0u, 0u};
    uint32_t n = 99u;
    setup();
    t = cx_task(&cx);
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_send(ch, NULL, 6u), ASX_OK);
    ASSERT_EQ(asx_task_cancel(t, ASX_CANCEL_USER), ASX_OK);
    ASSERT_EQ(asx_channel_recv_many(ch, &cx, NULL, 0u, &n), ASX_OK);
    ASSERT_EQ(n, 0u);
    asx_trace_set_observer(observe_last, NULL);
    ASSERT_EQ(asx_channel_recv_many(ch, &cx, buf, 2u, &n), ASX_E_CANCELLED);
    ASSERT_TRUE(last_event_is_user_trace(t, "mpsc::recv_many cancelled"));
    asx_trace_set_observer(NULL, NULL);
    ASSERT_EQ(asx_channel_recv_many(ch, NULL, buf, 2u, &n), ASX_OK);
    ASSERT_EQ(n, 1u);
    ASSERT_EQ(buf[0], 6u);
}

TEST(recv_many_reports_zero_once_no_message_can_arrive) {
    /* Closed and drained is Ok(0), not Disconnected (mpsc.rs:1909-1917);
     * an outstanding permit can still commit, so it waits for it. */
    asx_channel_id ch;
    asx_channel_id sealed;
    asx_send_permit p;
    uint64_t buf[2] = {0u, 0u};
    uint32_t n = 99u;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_recv_many(ch, NULL, buf, 2u, &n), ASX_E_PENDING);
    asx_send_permit_abort(&p);
    ASSERT_EQ(asx_channel_recv_many(ch, NULL, buf, 2u, &n), ASX_OK);
    ASSERT_EQ(n, 0u);

    ASSERT_EQ(asx_channel_create(g_rid, 2, &sealed), ASX_OK);
    ASSERT_EQ(asx_channel_send(sealed, NULL, 7u), ASX_OK);
    ASSERT_EQ(asx_channel_seal(sealed), ASX_OK);
    ASSERT_EQ(asx_channel_recv_many(sealed, NULL, buf, 2u, &n), ASX_OK);
    ASSERT_EQ(n, 1u);
    ASSERT_EQ(buf[0], 7u);
    ASSERT_EQ(asx_channel_recv_many(sealed, NULL, buf, 2u, &n), ASX_OK);
    ASSERT_EQ(n, 0u);
}

TEST(recv_many_argument_errors) {
    asx_channel_id ch;
    uint64_t buf[1] = {0u};
    uint32_t n = 0u;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_recv_many(ch, NULL, buf, 1u, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_channel_recv_many(ch, NULL, NULL, 1u, &n), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_channel_recv_many(ASX_INVALID_ID, NULL, buf, 1u, &n), ASX_E_INVALID_ARGUMENT);
}

static uint64_t g_batch[4];
static uint32_t g_batch_len;
static uint32_t g_batch_polls;

static asx_status poll_recv_many(void *ud, asx_task_id self) {
    asx_status st;
    (void)ud;
    (void)self;
    g_batch_polls++;
    st = asx_channel_recv_many(g_recv_ch, NULL, g_batch, 4u, &g_batch_len);
    return st == ASX_E_PENDING ? ASX_E_PENDING : ASX_OK;
}

TEST(recv_many_parks_and_takes_the_whole_batch_when_woken) {
    asx_task_id t;
    setup();
    g_batch_polls = 0;
    g_batch_len = 0;
    ASSERT_EQ(asx_channel_create(g_rid, 4, &g_recv_ch), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_recv_many, NULL, &t), ASX_OK);
    run_region();
    ASSERT_EQ(g_batch_polls, 1u);
    ASSERT_EQ(asx_channel_send(g_recv_ch, NULL, 1u), ASX_OK);
    ASSERT_EQ(asx_channel_send(g_recv_ch, NULL, 2u), ASX_OK);
    run_region();
    ASSERT_EQ(g_batch_polls, 2u);
    ASSERT_EQ(g_batch_len, 2u);
    ASSERT_EQ(g_batch[0], 1u);
    ASSERT_EQ(g_batch[1], 2u);

    /* The last sender's drop wakes a parked batch with 0 messages. */
    g_batch_polls = 0;
    g_batch_len = 99u;
    ASSERT_EQ(asx_task_spawn(g_rid, poll_recv_many, NULL, &t), ASX_OK);
    run_region();
    ASSERT_EQ(g_batch_polls, 1u);
    ASSERT_EQ(asx_channel_close_sender(g_recv_ch), ASX_OK);
    run_region();
    ASSERT_EQ(g_batch_polls, 2u);
    ASSERT_EQ(g_batch_len, 0u);
}

TEST(recv_many_wakes_one_parked_producer_per_freed_slot) {
    /* Rust wakes min(freed, free) queued producers
     * (sender_wakers_for_freed_slots, mpsc.rs:402-423); only the head of
     * the line may claim (:1081-1086), and its claim wakes the next. P2
     * sits before P1 in the task table but parks after it: woken together,
     * P2 polls first, is refused behind P1, and claims after P1's cascade:
     * two polls. A batch that woke only the head would give it one. */
    mpsc_producer p2 = {1u, 20u, 0u};
    mpsc_producer p1 = {0u, 10u, 0u};
    asx_task_id t1;
    asx_task_id t2;
    uint64_t buf[2] = {0u, 0u};
    uint64_t v = 0;
    uint32_t n = 0;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &g_prod_ch), ASX_OK);
    ASSERT_EQ(asx_channel_send(g_prod_ch, NULL, 1u), ASX_OK);
    ASSERT_EQ(asx_channel_send(g_prod_ch, NULL, 2u), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_producer, &p2, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_producer, &p1, &t1), ASX_OK);
    run_region();
    ASSERT_EQ(p1.polls, 1u); /* parked first */
    ASSERT_EQ(p2.polls, 2u); /* yielded once, then parked behind P1 */
    p1.polls = 0;
    p2.polls = 0;

    ASSERT_EQ(asx_channel_recv_many(g_prod_ch, NULL, buf, 2u, &n), ASX_OK);
    ASSERT_EQ(n, 2u);
    run_region();
    ASSERT_EQ(p1.polls, 1u);
    ASSERT_EQ(p2.polls, 2u);
    ASSERT_EQ(asx_channel_try_recv(g_prod_ch, &v), ASX_OK);
    ASSERT_EQ(v, 10u);
    ASSERT_EQ(asx_channel_try_recv(g_prod_ch, &v), ASX_OK);
    ASSERT_EQ(v, 20u);
}

/* -------------------------------------------------------------------
 * closed (mpsc_closed.rs:39)
 * ------------------------------------------------------------------- */

static uint32_t g_closed_polls;
static asx_status g_closed_status;

static asx_status poll_closed(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    g_closed_polls++;
    g_closed_status = asx_channel_closed(g_recv_ch);
    return g_closed_status == ASX_E_PENDING ? ASX_E_PENDING : ASX_OK;
}

TEST(closed_waits_for_the_receiver_and_ignores_messages) {
    /* A send or a receive does not wake it; the receiver's close does, and
     * the queued message stays receivable. */
    asx_task_id t;
    uint64_t v = 0;
    setup();
    g_closed_polls = 0;
    ASSERT_EQ(asx_channel_create(g_rid, 2, &g_recv_ch), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_closed, NULL, &t), ASX_OK);
    run_region();
    ASSERT_EQ(g_closed_polls, 1u);
    ASSERT_EQ(asx_channel_send(g_recv_ch, NULL, 3u), ASX_OK);
    ASSERT_EQ(asx_channel_send(g_recv_ch, NULL, 4u), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(g_recv_ch, &v), ASX_OK);
    run_region();
    ASSERT_EQ(g_closed_polls, 1u);
    ASSERT_EQ(asx_channel_seal(g_recv_ch), ASX_OK);
    run_region();
    ASSERT_EQ(g_closed_polls, 2u);
    ASSERT_EQ(g_closed_status, ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(g_recv_ch, &v), ASX_OK);
    ASSERT_EQ(v, 4u);
    /* Already closed: ready at once. */
    ASSERT_EQ(asx_channel_closed(g_recv_ch), ASX_OK);
}

TEST(closed_is_woken_by_the_receiver_drop_and_withdrawable) {
    asx_task_id t;
    asx_task_id w;
    setup();
    g_closed_polls = 0;
    ASSERT_EQ(asx_channel_create(g_rid, 2, &g_recv_ch), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_closed, NULL, &w), ASX_OK);
    run_region();
    ASSERT_EQ(g_closed_polls, 1u);
    /* Withdrawn (its future dropped): the close no longer polls it. */
    ASSERT_EQ(asx_channel_wait_cancel(g_recv_ch, w), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_rid, poll_closed, NULL, &t), ASX_OK);
    run_region();
    ASSERT_EQ(g_closed_polls, 2u);
    ASSERT_EQ(asx_channel_close_receiver(g_recv_ch), ASX_OK);
    run_region();
    ASSERT_EQ(g_closed_polls, 3u);
    ASSERT_EQ(g_closed_status, ASX_OK);
}

TEST(closed_refuses_without_a_live_sender) {
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_closed(ch), ASX_E_PENDING); /* no task: nothing parks */
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_closed(ch), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_channel_closed(ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);
}

TEST(weak_sender_upgrade_is_a_counted_clone) {
    /* WeakSender::upgrade (mpsc.rs:1275) succeeds while a sender lives,
     * even after the receiver closed, and fails once the count hit 0. */
    asx_channel_id ch;
    asx_channel_state cs;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_sender_clone(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &cs), ASX_OK);
    ASSERT_EQ(cs, ASX_CHANNEL_RECEIVER_CLOSED);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_sender_clone(ch), ASX_E_INVALID_STATE);
}

/* -------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */

int main(void) {
    fprintf(stderr, "=== MPSC Channel Tests ===\n");

    RUN_TEST(create_basic);
    RUN_TEST(create_null_out);
    RUN_TEST(create_zero_capacity);
    RUN_TEST(create_over_max_capacity);
    RUN_TEST(create_max_capacity);
    RUN_TEST(create_exhaustion);
    RUN_TEST(create_rejects_non_region_handle);
    RUN_TEST(create_rejects_closed_region);
    RUN_TEST(initial_state_is_open);
    RUN_TEST(initial_queue_empty);
    RUN_TEST(initial_reserved_zero);

    RUN_TEST(close_sender_from_open);
    RUN_TEST(close_receiver_from_open);
    RUN_TEST(close_sender_then_receiver);
    RUN_TEST(close_receiver_then_sender);
    RUN_TEST(double_close_sender);
    RUN_TEST(double_close_receiver);
    RUN_TEST(sender_side_closes_when_the_last_counted_sender_drops);
    RUN_TEST(counted_senders_outlive_a_receiver_close);

    RUN_TEST(reserve_and_send_basic);
    RUN_TEST(fifo_ordering);
    RUN_TEST(trace_emits_send_and_recv);
    RUN_TEST(capacity_enforcement);
    RUN_TEST(capacity_mixed_reserved_and_queued);
    RUN_TEST(capacity_invariant_interleaves_send_abort);
    RUN_TEST(scripted_multi_producer_fifo_stress);
#if defined(ASX_MPSC_PTHREAD_STRESS)
    RUN_TEST(pthread_multi_producer_single_consumer_stress);
#endif

    RUN_TEST(abort_returns_capacity);
    RUN_TEST(abort_null_is_safe);
    RUN_TEST(abort_consumed_is_noop);

    RUN_TEST(recv_empty_returns_would_block);
    RUN_TEST(recv_null_out);
    RUN_TEST(recv_after_sender_closed_with_data);
    RUN_TEST(recv_after_sender_closed_empty);
    RUN_TEST(recv_sender_closed_with_pending_reserve);
    RUN_TEST(recv_sender_closed_pending_reserve_abort);
    RUN_TEST(recv_after_receiver_closed);
    RUN_TEST(recv_after_fully_closed);

    RUN_TEST(ring_buffer_wraparound);
    RUN_TEST(ring_buffer_batch_wraparound);

    RUN_TEST(reserve_after_sender_closed);
    RUN_TEST(reserve_after_receiver_closed);
    RUN_TEST(send_after_receiver_closed);
    RUN_TEST(close_receiver_discards_queued_messages);
    RUN_TEST(close_receiver_discards_queue_but_not_pending_reserve);

    RUN_TEST(get_state_null_out);
    RUN_TEST(queue_len_null_out);
    RUN_TEST(reserved_count_null_out);
    RUN_TEST(reserve_null_out);
    RUN_TEST(send_null_permit);
    RUN_TEST(double_send_same_permit);
    RUN_TEST(forged_permit_send_rejected);
    RUN_TEST(stale_permit_copy_cannot_send);

    RUN_TEST(reset_clears_all);
    RUN_TEST(reset_after_heavy_use_leaves_no_stale_state);

    RUN_TEST(reserve_with_cx_registers_send_permit_committed_by_send);
    RUN_TEST(reserved_permit_abort_and_disconnect_abort_the_obligation);
    RUN_TEST(tracked_permit_credit_rejects_wrong_task_before_holder_send);
    RUN_TEST(tracked_permit_credit_rejects_wrong_task_before_holder_abort);
    RUN_TEST(tracked_permit_moved_to_another_task_can_send_without_reassigning_holder);
    RUN_TEST(tracked_permit_moved_to_another_task_can_abort_without_reassigning_holder);
    RUN_TEST(untracked_reserves_register_no_obligation);
    RUN_TEST(cancelled_cx_wins_over_ready_channel_and_traces);
    RUN_TEST(seal_keeps_the_queue_receivable);
    RUN_TEST(sealed_channel_slot_is_not_reused_until_drained);
    RUN_TEST(receiver_drop_after_seal_discards_the_queue_and_frees_the_slot);
    RUN_TEST(wake_receiver_repolls_a_parked_receiver);
    RUN_TEST(create_accepts_a_closing_region);
    RUN_TEST(evict_oldest_sends_without_eviction_while_there_is_room);
    RUN_TEST(evict_oldest_replaces_the_oldest_message_when_full);
    RUN_TEST(evict_oldest_never_evicts_a_reserved_slot);
    RUN_TEST(evict_oldest_errors);
    RUN_TEST(evict_oldest_refuses_a_slot_a_parked_producer_owns);
    RUN_TEST(recv_many_takes_up_to_limit_oldest_first);
    RUN_TEST(recv_many_limit_zero_returns_before_the_cancel_check);
    RUN_TEST(recv_many_reports_zero_once_no_message_can_arrive);
    RUN_TEST(recv_many_argument_errors);
    RUN_TEST(recv_many_parks_and_takes_the_whole_batch_when_woken);
    RUN_TEST(recv_many_wakes_one_parked_producer_per_freed_slot);
    RUN_TEST(closed_waits_for_the_receiver_and_ignores_messages);
    RUN_TEST(closed_is_woken_by_the_receiver_drop_and_withdrawable);
    RUN_TEST(closed_refuses_without_a_live_sender);
    RUN_TEST(weak_sender_upgrade_is_a_counted_clone);

    TEST_REPORT();
    return test_failures;
}
