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
#include <asx/runtime/trace.h>

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

    RUN_TEST(reserve_and_send_basic);
    RUN_TEST(fifo_ordering);
    RUN_TEST(trace_emits_send_and_recv);
    RUN_TEST(capacity_enforcement);
    RUN_TEST(capacity_mixed_reserved_and_queued);

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

    RUN_TEST(get_state_null_out);
    RUN_TEST(queue_len_null_out);
    RUN_TEST(reserved_count_null_out);
    RUN_TEST(reserve_null_out);
    RUN_TEST(send_null_permit);
    RUN_TEST(double_send_same_permit);
    RUN_TEST(forged_permit_send_rejected);
    RUN_TEST(stale_permit_copy_cannot_send);

    RUN_TEST(reset_clears_all);

    TEST_REPORT();
    return test_failures;
}
