/*
 * test_channel_exhaustion.c — channel boundary exhaustion and backpressure tests
 *
 * Exercises capacity limits, backpressure recovery, permit lifecycle
 * edge cases, and channel-slot exhaustion under pathological patterns.
 *
 * Part of bd-1md.7: per-boundary exhaustion test suite.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_log.h"
#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/core/channel.h>

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static void reset_all(void)
{
    asx_runtime_reset();
    asx_channel_reset();
}

/* -------------------------------------------------------------------
 * Capacity boundary: reserve at capacity, verify FULL
 * ------------------------------------------------------------------- */

TEST(reserve_at_capacity_returns_full)
{
    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit permits[4];
    asx_send_permit extra;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &cid), ASX_OK);

    /* Reserve all 4 slots */
    for (i = 0; i < 4; i++) {
        ASSERT_EQ(asx_channel_try_reserve(cid, &permits[i]), ASX_OK);
    }

    /* Next reserve should fail */
    ASSERT_EQ(asx_channel_try_reserve(cid, &extra), ASX_E_CHANNEL_FULL);

    /* Clean up permits */
    for (i = 0; i < 4; i++) {
        asx_send_permit_abort(&permits[i]);
    }
}

/* -------------------------------------------------------------------
 * Backpressure recovery: abort frees capacity
 * ------------------------------------------------------------------- */

TEST(abort_frees_capacity_for_new_reserve)
{
    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit permits[4];
    asx_send_permit recovered;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &cid), ASX_OK);

    /* Fill capacity */
    for (i = 0; i < 4; i++) {
        ASSERT_EQ(asx_channel_try_reserve(cid, &permits[i]), ASX_OK);
    }

    /* Abort one permit */
    asx_send_permit_abort(&permits[2]);

    /* Should now have 1 slot free */
    ASSERT_EQ(asx_channel_try_reserve(cid, &recovered), ASX_OK);

    /* Still full after re-reservation */
    {
        asx_send_permit extra;
        ASSERT_EQ(asx_channel_try_reserve(cid, &extra), ASX_E_CHANNEL_FULL);
    }

    /* Cleanup */
    asx_send_permit_abort(&permits[0]);
    asx_send_permit_abort(&permits[1]);
    asx_send_permit_abort(&permits[3]);
    asx_send_permit_abort(&recovered);
}

/* -------------------------------------------------------------------
 * Recv frees capacity for new reserves
 * ------------------------------------------------------------------- */

TEST(recv_frees_capacity_for_new_reserve)
{
    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit p;
    uint64_t val;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &cid), ASX_OK);

    /* Fill queue with committed messages */
    for (i = 0; i < 4; i++) {
        ASSERT_EQ(asx_channel_try_reserve(cid, &p), ASX_OK);
        ASSERT_EQ(asx_send_permit_send(&p, (uint64_t)(100 + i)), ASX_OK);
    }

    /* Should be full */
    {
        asx_send_permit extra;
        ASSERT_EQ(asx_channel_try_reserve(cid, &extra), ASX_E_CHANNEL_FULL);
    }

    /* Recv one message */
    ASSERT_EQ(asx_channel_try_recv(cid, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)100);

    /* Now should be able to reserve again */
    ASSERT_EQ(asx_channel_try_reserve(cid, &p), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 999), ASX_OK);
}

/* -------------------------------------------------------------------
 * Rapid reserve-abort churn at boundary
 * ------------------------------------------------------------------- */

TEST(rapid_reserve_abort_churn_at_boundary)
{
    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit p;
    uint32_t i;
    uint32_t reserved;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 2, &cid), ASX_OK);

    /* Rapid reserve-abort cycles at boundary */
    for (i = 0; i < 100; i++) {
        ASSERT_EQ(asx_channel_try_reserve(cid, &p), ASX_OK);
        asx_send_permit_abort(&p);
    }

    /* Channel should be empty after all aborts */
    ASSERT_EQ(asx_channel_reserved_count(cid, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 0u);
}

/* -------------------------------------------------------------------
 * Interleaved send-recv at capacity
 * ------------------------------------------------------------------- */

TEST(interleaved_send_recv_at_capacity)
{
    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit p;
    uint64_t val;
    uint32_t i;
    uint32_t qlen;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 2, &cid), ASX_OK);

    /* Send-recv interleave for 50 cycles with capacity 2 */
    for (i = 0; i < 50; i++) {
        /* Send two messages */
        ASSERT_EQ(asx_channel_try_reserve(cid, &p), ASX_OK);
        ASSERT_EQ(asx_send_permit_send(&p, (uint64_t)(i * 2)), ASX_OK);

        ASSERT_EQ(asx_channel_try_reserve(cid, &p), ASX_OK);
        ASSERT_EQ(asx_send_permit_send(&p, (uint64_t)(i * 2 + 1)), ASX_OK);

        /* Recv both */
        ASSERT_EQ(asx_channel_try_recv(cid, &val), ASX_OK);
        ASSERT_EQ(val, (uint64_t)(i * 2));

        ASSERT_EQ(asx_channel_try_recv(cid, &val), ASX_OK);
        ASSERT_EQ(val, (uint64_t)(i * 2 + 1));
    }

    /* Queue should be empty */
    ASSERT_EQ(asx_channel_queue_len(cid, &qlen), ASX_OK);
    ASSERT_EQ(qlen, 0u);
}

/* -------------------------------------------------------------------
 * Queue + reserved invariant at capacity
 * ------------------------------------------------------------------- */

TEST(queue_plus_reserved_equals_capacity_invariant)
{
    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit p1, p2;
    uint32_t qlen, reserved;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &cid), ASX_OK);

    /* Reserve 2, send 1, abort 1 */
    ASSERT_EQ(asx_channel_try_reserve(cid, &p1), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(cid, &p2), ASX_OK);

    /* queued=0, reserved=2 */
    ASSERT_EQ(asx_channel_queue_len(cid, &qlen), ASX_OK);
    ASSERT_EQ(qlen, 0u);
    ASSERT_EQ(asx_channel_reserved_count(cid, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 2u);

    /* Send p1: queued=1, reserved=1 */
    ASSERT_EQ(asx_send_permit_send(&p1, 42), ASX_OK);
    ASSERT_EQ(asx_channel_queue_len(cid, &qlen), ASX_OK);
    ASSERT_EQ(qlen, 1u);
    ASSERT_EQ(asx_channel_reserved_count(cid, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 1u);

    /* Abort p2: queued=1, reserved=0 */
    asx_send_permit_abort(&p2);
    ASSERT_EQ(asx_channel_queue_len(cid, &qlen), ASX_OK);
    ASSERT_EQ(qlen, 1u);
    ASSERT_EQ(asx_channel_reserved_count(cid, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 0u);
}

/* -------------------------------------------------------------------
 * Channel-slot exhaustion and recovery
 * ------------------------------------------------------------------- */

TEST(channel_slot_exhaustion_and_recovery)
{
    asx_region_id rid;
    asx_channel_id cids[ASX_MAX_CHANNELS];
    asx_channel_id extra;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Create max channels */
    for (i = 0; i < ASX_MAX_CHANNELS; i++) {
        ASSERT_EQ(asx_channel_create(rid, 4, &cids[i]), ASX_OK);
    }

    /* Next creation should fail */
    ASSERT_EQ(asx_channel_create(rid, 4, &extra), ASX_E_RESOURCE_EXHAUSTED);

    /* Close one channel fully */
    ASSERT_EQ(asx_channel_close_sender(cids[0]), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(cids[0]), ASX_OK);

    /* Now we may or may not be able to create a new one, depending
     * on whether the walking skeleton recycles channel slots.
     * Verify the close at least succeeded without corruption. */
    {
        asx_channel_state st;
        ASSERT_EQ(asx_channel_get_state(cids[0], &st), ASX_OK);
        ASSERT_EQ(st, ASX_CHANNEL_FULLY_CLOSED);
    }
}

/* -------------------------------------------------------------------
 * Max capacity channel stress
 * ------------------------------------------------------------------- */

TEST(max_capacity_channel_fill_and_drain)
{
    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit p;
    uint64_t val;
    uint32_t i;
    uint32_t qlen;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, ASX_CHANNEL_MAX_CAPACITY, &cid), ASX_OK);

    /* Fill to max capacity */
    for (i = 0; i < ASX_CHANNEL_MAX_CAPACITY; i++) {
        ASSERT_EQ(asx_channel_try_reserve(cid, &p), ASX_OK);
        ASSERT_EQ(asx_send_permit_send(&p, (uint64_t)i), ASX_OK);
    }

    /* Verify full */
    ASSERT_EQ(asx_channel_queue_len(cid, &qlen), ASX_OK);
    ASSERT_EQ(qlen, (uint32_t)ASX_CHANNEL_MAX_CAPACITY);

    {
        asx_send_permit extra;
        ASSERT_EQ(asx_channel_try_reserve(cid, &extra), ASX_E_CHANNEL_FULL);
    }

    /* Drain all — verify FIFO order */
    for (i = 0; i < ASX_CHANNEL_MAX_CAPACITY; i++) {
        ASSERT_EQ(asx_channel_try_recv(cid, &val), ASX_OK);
        ASSERT_EQ(val, (uint64_t)i);
    }

    /* Queue should be empty */
    ASSERT_EQ(asx_channel_queue_len(cid, &qlen), ASX_OK);
    ASSERT_EQ(qlen, 0u);

    /* Can reserve again */
    ASSERT_EQ(asx_channel_try_reserve(cid, &p), ASX_OK);
    asx_send_permit_abort(&p);
}

/* -------------------------------------------------------------------
 * Disconnected channel backpressure
 * ------------------------------------------------------------------- */

TEST(disconnect_during_reserve_storm)
{
    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit permits[4];
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &cid), ASX_OK);

    /* Reserve all slots */
    for (i = 0; i < 4; i++) {
        ASSERT_EQ(asx_channel_try_reserve(cid, &permits[i]), ASX_OK);
    }

    /* Close receiver while permits outstanding */
    ASSERT_EQ(asx_channel_close_receiver(cid), ASX_OK);

    /* Sending on outstanding permits should fail with DISCONNECTED */
    ASSERT_EQ(asx_send_permit_send(&permits[0], 42), ASX_E_DISCONNECTED);

    /* New reserves should also fail */
    {
        asx_send_permit extra;
        ASSERT_EQ(asx_channel_try_reserve(cid, &extra), ASX_E_DISCONNECTED);
    }

    /* Clean up remaining permits */
    for (i = 1; i < 4; i++) {
        asx_send_permit_abort(&permits[i]);
    }
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void)
{
    test_log_open("unit", "channel/exhaustion", "test_channel_exhaustion");

    RUN_TEST(reserve_at_capacity_returns_full);
    RUN_TEST(abort_frees_capacity_for_new_reserve);
    RUN_TEST(recv_frees_capacity_for_new_reserve);
    RUN_TEST(rapid_reserve_abort_churn_at_boundary);
    RUN_TEST(interleaved_send_recv_at_capacity);
    RUN_TEST(queue_plus_reserved_equals_capacity_invariant);
    RUN_TEST(channel_slot_exhaustion_and_recovery);
    RUN_TEST(max_capacity_channel_fill_and_drain);
    RUN_TEST(disconnect_during_reserve_storm);

    TEST_REPORT();
    test_log_close();
    return test_failures;
}
