/*
 * test_mpsc.c — unit tests for bounded MPSC two-phase channel (bd-1md.5)
 *
 * Exercises: create/close lifecycle, two-phase reserve/send/abort,
 * FIFO ordering, capacity enforcement, disconnect detection, ring
 * buffer wraparound, stale handle rejection, and capacity exhaustion.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/channel.h>
#include "test_harness.h"

/* Suppress warn_unused_result for intentionally-ignored calls */
#define CH_IGNORE(expr) \
    do { volatile asx_status _ch_ign = (expr); (void)_ch_ign; } while (0)

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static asx_region_id g_rid;

static void setup(void)
{
    asx_runtime_reset();
    asx_channel_reset();
    CH_IGNORE(asx_region_open(&g_rid));
}

/* -------------------------------------------------------------------
 * Lifecycle tests
 * ------------------------------------------------------------------- */

TEST(create_basic)
{
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
}

TEST(create_null_out)
{
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(create_zero_capacity)
{
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 0, &ch), ASX_E_INVALID_ARGUMENT);
}

TEST(create_over_max_capacity)
{
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, ASX_CHANNEL_MAX_CAPACITY + 1, &ch),
              ASX_E_INVALID_ARGUMENT);
}

TEST(create_max_capacity)
{
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, ASX_CHANNEL_MAX_CAPACITY, &ch), ASX_OK);
}

TEST(create_exhaustion)
{
    asx_channel_id ch;
    uint32_t i;
    setup();
    for (i = 0; i < ASX_MAX_CHANNELS; i++) {
        ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    }
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(initial_state_is_open)
{
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_OPEN);
}

TEST(initial_queue_empty)
{
    asx_channel_id ch;
    uint32_t len;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_queue_len(ch, &len), ASX_OK);
    ASSERT_EQ(len, 0u);
}

TEST(initial_reserved_zero)
{
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

TEST(close_sender_from_open)
{
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_SENDER_CLOSED);
}

TEST(close_receiver_from_open)
{
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_RECEIVER_CLOSED);
}

TEST(close_sender_then_receiver)
{
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_FULLY_CLOSED);
}

TEST(close_receiver_then_sender)
{
    asx_channel_id ch;
    asx_channel_state st;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, &st), ASX_OK);
    ASSERT_EQ(st, ASX_CHANNEL_FULLY_CLOSED);
}

TEST(double_close_sender)
{
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_E_INVALID_STATE);
}

TEST(double_close_receiver)
{
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_E_INVALID_STATE);
}

/* -------------------------------------------------------------------
 * Two-phase send: reserve + send
 * ------------------------------------------------------------------- */

TEST(reserve_and_send_basic)
{
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

TEST(fifo_ordering)
{
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

TEST(capacity_enforcement)
{
    asx_channel_id ch;
    asx_send_permit permits[4];
    asx_send_permit overflow;
    uint32_t i;
    uint32_t res;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    for (i = 0; i < 4; i++) {
        ASSERT_EQ(asx_channel_try_reserve(ch, &permits[i]), ASX_OK);
    }

    ASSERT_EQ(asx_channel_try_reserve(ch, &overflow), ASX_E_CHANNEL_FULL);

    ASSERT_EQ(asx_channel_reserved_count(ch, &res), ASX_OK);
    ASSERT_EQ(res, 4u);

    for (i = 0; i < 4; i++) {
        asx_send_permit_abort(&permits[i]);
    }
}

TEST(capacity_mixed_reserved_and_queued)
{
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

TEST(abort_returns_capacity)
{
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

TEST(abort_null_is_safe)
{
    asx_send_permit_abort(NULL);
    ASSERT_TRUE(1);
}

TEST(abort_consumed_is_noop)
{
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

TEST(recv_empty_returns_would_block)
{
    asx_channel_id ch;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_WOULD_BLOCK);
}

TEST(recv_null_out)
{
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(recv_after_sender_closed_with_data)
{
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

TEST(recv_after_sender_closed_empty)
{
    asx_channel_id ch;
    uint64_t val;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_E_DISCONNECTED);
}

/* -------------------------------------------------------------------
 * Ring buffer wraparound
 * ------------------------------------------------------------------- */

TEST(ring_buffer_wraparound)
{
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

TEST(ring_buffer_batch_wraparound)
{
    asx_channel_id ch;
    asx_send_permit permits[4];
    uint64_t val;
    uint32_t i, round;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);

    for (round = 0; round < 5; round++) {
        for (i = 0; i < 4; i++) {
            ASSERT_EQ(asx_channel_try_reserve(ch, &permits[i]), ASX_OK);
            ASSERT_EQ(asx_send_permit_send(&permits[i],
                       (uint64_t)(round * 100 + i)), ASX_OK);
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

TEST(reserve_after_sender_closed)
{
    asx_channel_id ch;
    asx_send_permit p;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_E_INVALID_STATE);
}

TEST(reserve_after_receiver_closed)
{
    asx_channel_id ch;
    asx_send_permit p;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_E_DISCONNECTED);
}

TEST(send_after_receiver_closed)
{
    asx_channel_id ch;
    asx_send_permit p;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 123), ASX_E_DISCONNECTED);
}

TEST(close_receiver_discards_queued_messages)
{
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

TEST(get_state_null_out)
{
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_get_state(ch, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(queue_len_null_out)
{
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_queue_len(ch, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(reserved_count_null_out)
{
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_reserved_count(ch, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(reserve_null_out)
{
    asx_channel_id ch;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(send_null_permit)
{
    ASSERT_EQ(asx_send_permit_send(NULL, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(double_send_same_permit)
{
    asx_channel_id ch;
    asx_send_permit p;
    setup();
    ASSERT_EQ(asx_channel_create(g_rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 1), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 2), ASX_E_INVALID_STATE);
}

/* -------------------------------------------------------------------
 * Reset
 * ------------------------------------------------------------------- */

TEST(reset_clears_all)
{
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

int main(void)
{
    fprintf(stderr, "=== MPSC Channel Tests ===\n");

    RUN_TEST(create_basic);
    RUN_TEST(create_null_out);
    RUN_TEST(create_zero_capacity);
    RUN_TEST(create_over_max_capacity);
    RUN_TEST(create_max_capacity);
    RUN_TEST(create_exhaustion);
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
    RUN_TEST(capacity_enforcement);
    RUN_TEST(capacity_mixed_reserved_and_queued);

    RUN_TEST(abort_returns_capacity);
    RUN_TEST(abort_null_is_safe);
    RUN_TEST(abort_consumed_is_noop);

    RUN_TEST(recv_empty_returns_would_block);
    RUN_TEST(recv_null_out);
    RUN_TEST(recv_after_sender_closed_with_data);
    RUN_TEST(recv_after_sender_closed_empty);

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

    RUN_TEST(reset_clears_all);

    TEST_REPORT();
    return test_failures;
}
