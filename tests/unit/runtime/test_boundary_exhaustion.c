/*
 * test_boundary_exhaustion.c — per-boundary exhaustion tests with detailed logs
 *
 * Exercises cross-arena interaction under exhaustion, multi-region
 * competition for shared arenas, channel backpressure edge cases,
 * timer cancel+re-register under pressure, and failure-atomic
 * invariants across all resource boundaries.
 *
 * Covers gaps in existing exhaustion testing:
 *   - Simultaneous multi-arena exhaustion
 *   - Multi-region obligation competition
 *   - Channel mid-transmission sender close
 *   - Timer cancel+re-register churn at capacity
 *   - Capture arena multi-task consumption
 *   - Determinism of all partial failure states
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include "test_runtime.h"
#include "test_task.h"
#include <asx/asx.h>
#include <asx/core/channel.h>
#include <asx/core/resource.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <asx/time/timer_wheel.h>
#include <string.h>

/* ---- Helpers ---- */

static void reset_all(void) {
    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    asx_trace_reset(test_runtime());
    asx_channel_reset();
    asx_replay_clear_reference();
}

static asx_status poll_ok(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

static asx_status poll_pending(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_PENDING;
}

/* Reserves a generic obligation held by a live task from the host, outside
 * any poll. Only the holder's own poll could settle it, so tests that never
 * run the holder leave it reserved without leaking. */
static asx_status reserve_for(asx_region_id rid, asx_task_id holder, asx_obligation_id *oid) {
    return asx_obligation_reserve_ex(rid, ASX_OBLIGATION_KIND_GENERIC, holder, oid);
}

/* ====================================================================
 * Cross-arena simultaneous exhaustion
 * ==================================================================== */

TEST(cross_arena_task_and_obligation_both_exhausted) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_id holder = ASX_INVALID_ID;
    asx_obligation_id oid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);

    /* Fill all task slots */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid), ASX_OK);
        if (i == 0u) holder = tid;
    }
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_TASK), (uint32_t)0);

    /* Fill all obligation slots; the task arena is full, so the first
     * (never polled, hence live) task holds them all. */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) { ASSERT_EQ(reserve_for(rid, holder, &oid), ASX_OK); }
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_OBLIGATION), (uint32_t)0);

    /* Both should fail independently with correct errors */
    ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(reserve_for(rid, holder, &oid), ASX_E_RESOURCE_EXHAUSTED);

    /* State should be consistent */
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_TASK), (uint32_t)ASX_MAX_TASKS);
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_OBLIGATION), (uint32_t)ASX_MAX_OBLIGATIONS);
}

TEST(cross_arena_region_full_but_tasks_available) {
    asx_region_id rids[ASX_MAX_REGIONS];
    asx_region_id overflow;
    asx_task_id tid;
    uint32_t i;

    reset_all();

    /* Fill all region slots */
    for (i = 0; i < ASX_MAX_REGIONS; i++) {
        ASSERT_EQ(asx_region_open(test_runtime(), &rids[i]), ASX_OK);
    }

    /* Region arena is full */
    ASSERT_EQ(asx_region_open(test_runtime(), &overflow), ASX_E_RESOURCE_EXHAUSTED);

    /* But tasks are still available within existing regions */
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_TASK), (uint32_t)ASX_MAX_TASKS);
    ASSERT_EQ(asx_task_spawn(rids[0], poll_ok, NULL, &tid), ASX_OK);
}

TEST(cross_arena_tasks_exhausted_regions_ok) {
    asx_region_id r1, r2;
    asx_task_id tid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &r1), ASX_OK);

    /* Fill all task slots via region 1 */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(r1, poll_ok, NULL, &tid), ASX_OK);
    }

    /* Task arena exhausted — but regions are fine */
    ASSERT_EQ(asx_region_open(test_runtime(), &r2), ASX_OK);

    /* Cannot spawn in new region either (task arena shared) */
    ASSERT_EQ(asx_task_spawn(r2, poll_ok, NULL, &tid), ASX_E_RESOURCE_EXHAUSTED);
}

/* ====================================================================
 * Multi-region obligation arena competition
 * ==================================================================== */

TEST(multi_region_obligation_competition) {
    asx_region_id r1, r2;
    asx_task_id h1, h2;
    asx_obligation_id oid;
    uint32_t i;
    uint32_t half = ASX_MAX_OBLIGATIONS / 2u;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &r1), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &r2), ASX_OK);
    /* Each region's obligations are held by a (never polled) task of it. */
    ASSERT_EQ(asx_task_spawn(r1, poll_pending, NULL, &h1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r2, poll_pending, NULL, &h2), ASX_OK);

    /* Region 1 takes half the arena */
    for (i = 0; i < half; i++) { ASSERT_EQ(reserve_for(r1, h1, &oid), ASX_OK); }

    /* Region 2 takes the other half */
    for (i = 0; i < ASX_MAX_OBLIGATIONS - half; i++) {
        ASSERT_EQ(reserve_for(r2, h2, &oid), ASX_OK);
    }

    /* Both regions now fail on new reservations */
    ASSERT_EQ(reserve_for(r1, h1, &oid), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(reserve_for(r2, h2, &oid), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_OBLIGATION), (uint32_t)ASX_MAX_OBLIGATIONS);
}

TEST(multi_region_task_competition) {
    asx_region_id r1, r2;
    asx_task_id tid;
    uint32_t i;
    uint32_t half = ASX_MAX_TASKS / 2u;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &r1), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &r2), ASX_OK);

    /* Region 1 takes half */
    for (i = 0; i < half; i++) { ASSERT_EQ(asx_task_spawn(r1, poll_ok, NULL, &tid), ASX_OK); }

    /* Region 2 takes the rest */
    for (i = 0; i < ASX_MAX_TASKS - half; i++) {
        ASSERT_EQ(asx_task_spawn(r2, poll_ok, NULL, &tid), ASX_OK);
    }

    /* Both regions fail on spawn */
    ASSERT_EQ(asx_task_spawn(r1, poll_ok, NULL, &tid), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_task_spawn(r2, poll_ok, NULL, &tid), ASX_E_RESOURCE_EXHAUSTED);
}

/* ====================================================================
 * Channel backpressure edge cases
 * ==================================================================== */

TEST(channel_reserve_at_capacity_boundary) {
    asx_region_id rid;
    asx_channel_id ch;
    asx_send_permit permits[4];
    asx_send_permit extra;
    uint32_t qlen, rcount;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_OK);

    /* Reserve all 4 slots */
    for (i = 0; i < 4; i++) { ASSERT_EQ(asx_channel_try_reserve(ch, &permits[i]), ASX_OK); }

    /* 5th reserve should fail */
    ASSERT_EQ(asx_channel_try_reserve(ch, &extra), ASX_E_CHANNEL_FULL);

    /* Abort one — capacity returns */
    asx_send_permit_abort(&permits[0]);

    /* Now reserve succeeds again */
    ASSERT_EQ(asx_channel_try_reserve(ch, &extra), ASX_OK);

    /* Check counts */
    ASSERT_EQ(asx_channel_queue_len(ch, &qlen), ASX_OK);
    ASSERT_EQ(qlen, (uint32_t)0);
    ASSERT_EQ(asx_channel_reserved_count(ch, &rcount), ASX_OK);
    ASSERT_EQ(rcount, (uint32_t)4);
}

TEST(channel_sender_close_mid_reservation) {
    asx_region_id rid;
    asx_channel_id ch;
    asx_send_permit permit;
    asx_send_permit post_close;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_OK);

    /* Reserve a slot */
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);

    /* Close sender while permit is outstanding */
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);

    /* Existing permit can still send */
    ASSERT_EQ(asx_send_permit_send(&permit, 42), ASX_OK);

    /* But new reservations fail */
    ASSERT_EQ(asx_channel_try_reserve(ch, &post_close), ASX_E_INVALID_STATE);
}

TEST(channel_receiver_close_with_pending_permits) {
    asx_region_id rid;
    asx_channel_id ch;
    asx_send_permit permit;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_OK);

    /* Reserve a slot */
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);

    /* Close receiver while permit is outstanding */
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);

    /* Send via existing permit fails with DISCONNECTED */
    ASSERT_EQ(asx_send_permit_send(&permit, 42), ASX_E_DISCONNECTED);
}

TEST(channel_full_send_then_recv_frees_capacity) {
    asx_region_id rid;
    asx_channel_id ch;
    asx_send_permit p;
    uint64_t val;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_OK);

    /* Fill channel to capacity */
    for (i = 0; i < 4; i++) {
        ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
        ASSERT_EQ(asx_send_permit_send(&p, (uint64_t)(100 + i)), ASX_OK);
    }
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_E_CHANNEL_FULL);

    /* Recv one — frees capacity for one more send */
    ASSERT_EQ(asx_channel_try_recv(ch, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)100);

    /* Can now reserve+send again */
    ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&p, 999), ASX_OK);
}

TEST(channel_arena_exhaustion) {
    asx_region_id rid;
    asx_channel_id ch;
    asx_channel_id overflow;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);

    /* Create all 16 channels */
    for (i = 0; i < ASX_MAX_CHANNELS; i++) { ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_OK); }

    /* 17th should fail */
    ASSERT_EQ(asx_channel_create(rid, 4, &overflow), ASX_E_RESOURCE_EXHAUSTED);
}

/* ====================================================================
 * Timer wheel exhaustion with cancel+re-register
 * ==================================================================== */

TEST(timer_exhaust_cancel_all_reregister) {
    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_handle handles[ASX_MAX_TIMERS];
    asx_timer_handle fresh;
    uint32_t i;

    asx_timer_wheel_reset(wheel);

    /* Fill to capacity */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(1000 + i), NULL, &handles[i]), ASX_OK);
    }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)ASX_MAX_TIMERS);

    /* Overflow fails */
    ASSERT_EQ(asx_timer_register(wheel, 9999, NULL, &fresh), ASX_E_RESOURCE_EXHAUSTED);

    /* Cancel ALL timers */
    for (i = 0; i < ASX_MAX_TIMERS; i++) { ASSERT_TRUE(asx_timer_cancel(wheel, &handles[i])); }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)0);

    /* Re-register all slots — should succeed (recycled) */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(2000 + i), NULL, &handles[i]), ASX_OK);
    }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)ASX_MAX_TIMERS);
}

TEST(timer_cancel_reregister_interleaved) {
    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_handle handles[ASX_MAX_TIMERS];
    asx_timer_handle fresh;
    uint32_t i;

    asx_timer_wheel_reset(wheel);

    /* Fill to capacity */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(1000 + i), NULL, &handles[i]), ASX_OK);
    }

    /* Cancel even-indexed, then re-register into freed slots */
    for (i = 0; i < ASX_MAX_TIMERS; i += 2) { ASSERT_TRUE(asx_timer_cancel(wheel, &handles[i])); }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)(ASX_MAX_TIMERS / 2));

    /* Fill freed slots */
    for (i = 0; i < ASX_MAX_TIMERS / 2; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(5000 + i), NULL, &fresh), ASX_OK);
    }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)ASX_MAX_TIMERS);

    /* Should be full again */
    ASSERT_EQ(asx_timer_register(wheel, 9999, NULL, &fresh), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(timer_fire_frees_slots_for_reregister) {
    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_handle handles[ASX_MAX_TIMERS];
    asx_timer_handle fresh;
    void *wakers[ASX_MAX_TIMERS];
    uint32_t fired;
    uint32_t i;

    asx_timer_wheel_reset(wheel);

    /* Fill to capacity with deadlines at t=100..100+ASX_MAX_TIMERS-1 */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(100 + i), NULL, &handles[i]), ASX_OK);
    }

    /* Fire the first half (deadlines 100..100+half-1) */
    fired = asx_timer_collect_expired(wheel, (asx_time)(100u + ASX_MAX_TIMERS / 2u - 1u), wakers,
                                      ASX_MAX_TIMERS);
    ASSERT_EQ(fired, (uint32_t)(ASX_MAX_TIMERS / 2u));
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)(ASX_MAX_TIMERS - ASX_MAX_TIMERS / 2u));

    /* Re-register into freed slots */
    for (i = 0; i < ASX_MAX_TIMERS / 2u; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(10000 + i), NULL, &fresh), ASX_OK);
    }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)ASX_MAX_TIMERS);
}

/* ====================================================================
 * Capture arena multi-task consumption
 * ==================================================================== */

TEST(capture_arena_multi_task_boundary) {
    asx_region_id rid;
    asx_task_id tid;
    void *state;
    uint32_t remaining;
    uint32_t chunk_size;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);

    /* Allocate 4 tasks each consuming 1/4 of the capture arena */
    chunk_size = ASX_REGION_CAPTURE_ARENA_BYTES / 4u;
    for (i = 0; i < 4; i++) {
        ASSERT_EQ(asx_task_spawn_captured(rid, poll_ok, chunk_size, NULL, &tid, &state), ASX_OK);
    }

    /* Check remaining — should be near zero (alignment overhead) */
    ASSERT_EQ(asx_resource_region_capture_remaining(rid, &remaining), ASX_OK);
    ASSERT_TRUE(remaining < chunk_size);

    /* One more should fail */
    ASSERT_EQ(asx_task_spawn_captured(rid, poll_ok, chunk_size, NULL, &tid, &state),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(capture_arena_exact_boundary_allocation) {
    asx_region_id rid;
    asx_task_id tid;
    void *state;
    uint32_t remaining;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);

    /* Allocate most of the arena */
    ASSERT_EQ(asx_task_spawn_captured(rid, poll_ok, ASX_REGION_CAPTURE_ARENA_BYTES - 64u, NULL,
                                      &tid, &state),
              ASX_OK);

    ASSERT_EQ(asx_resource_region_capture_remaining(rid, &remaining), ASX_OK);
    ASSERT_TRUE(remaining <= 64u);

    /* Allocation of remaining + 1 should fail */
    if (remaining > 0) {
        ASSERT_EQ(asx_task_spawn_captured(rid, poll_ok, remaining + 1u, NULL, &tid, &state),
                  ASX_E_RESOURCE_EXHAUSTED);
    }
}

TEST(capture_arena_independent_per_region) {
    asx_region_id r1, r2;
    asx_task_id tid;
    void *state;
    uint32_t r1_remaining, r2_remaining;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &r1), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &r2), ASX_OK);

    /* Consume half of region 1's capture arena */
    ASSERT_EQ(asx_task_spawn_captured(r1, poll_ok, ASX_REGION_CAPTURE_ARENA_BYTES / 2u, NULL, &tid,
                                      &state),
              ASX_OK);

    /* Region 2's capture arena should be untouched */
    ASSERT_EQ(asx_resource_region_capture_remaining(r1, &r1_remaining), ASX_OK);
    ASSERT_EQ(asx_resource_region_capture_remaining(r2, &r2_remaining), ASX_OK);

    ASSERT_TRUE(r1_remaining < ASX_REGION_CAPTURE_ARENA_BYTES);
    ASSERT_EQ(r2_remaining, (uint32_t)ASX_REGION_CAPTURE_ARENA_BYTES);
}

/* ====================================================================
 * Obligation edge cases at exhaustion
 * ==================================================================== */

/* The obligation tests below hold and settle their obligations as the single
 * poll of a task of the region (only a holder's own poll can settle one) and
 * resolve everything they still hold before it completes: a holder that
 * finishes with an obligation reserved leaks it. */
static void exhaust_then_commit_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oids[ASX_MAX_OBLIGATIONS];
    asx_obligation_id extra;
    uint32_t i;

    /* Fill all slots */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        ASSERT_EQ(asx_obligation_reserve(rid, &oids[i]), ASX_OK);
    }

    /* Exhaust */
    ASSERT_EQ(asx_obligation_reserve(rid, &extra), ASX_E_RESOURCE_EXHAUSTED);

    /* Commit all — resolved records stay inspectable until pressure */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) { ASSERT_EQ(asx_obligation_commit(oids[i]), ASX_OK); }
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        asx_obligation_state s;
        ASSERT_EQ(asx_obligation_get_state(oids[i], &s), ASX_OK);
        ASSERT_EQ(s, ASX_OBLIGATION_COMMITTED);
    }

    /* Allocation pressure reclaims resolved slots: the reserve succeeds
     * and reuses the lowest slot index first. */
    ASSERT_EQ(asx_obligation_reserve(rid, &extra), ASX_OK);
    ASSERT_EQ(asx_handle_slot(extra), asx_handle_slot(oids[0]));

    /* Every reclaimed handle now fails closed instead of aliasing. */
    {
        asx_obligation_state s;
        ASSERT_EQ(asx_obligation_get_state(oids[0], &s), ASX_E_STALE_HANDLE);
        ASSERT_EQ(asx_obligation_get_state(oids[1], &s), ASX_E_NOT_FOUND);
        ASSERT_EQ(asx_obligation_commit(oids[0]), ASX_E_STALE_HANDLE);
        ASSERT_EQ(asx_obligation_get_state(extra, &s), ASX_OK);
        ASSERT_EQ(s, ASX_OBLIGATION_RESERVED);
    }

    ASSERT_EQ(asx_obligation_commit(extra), ASX_OK);
}

TEST(obligation_exhaust_then_commit_no_free_slots) {
    asx_region_id rid;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, exhaust_then_commit_body, &rid), ASX_OK);
}

static void exhaust_mixed_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oids[ASX_MAX_OBLIGATIONS];
    asx_obligation_id extra;
    uint32_t i;

    /* Fill all slots */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        ASSERT_EQ(asx_obligation_reserve(rid, &oids[i]), ASX_OK);
    }

    /* Commit even, abort odd */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        if (i % 2 == 0) {
            ASSERT_EQ(asx_obligation_commit(oids[i]), ASX_OK);
        } else {
            ASSERT_EQ(asx_obligation_abort(oids[i]), ASX_OK);
        }
    }

    /* Verify final states (resolved records remain inspectable) */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        asx_obligation_state s;
        ASSERT_EQ(asx_obligation_get_state(oids[i], &s), ASX_OK);
        if (i % 2 == 0) {
            ASSERT_EQ(s, ASX_OBLIGATION_COMMITTED);
        } else {
            ASSERT_EQ(s, ASX_OBLIGATION_ABORTED);
        }
    }

    /* Both committed and aborted slots are reclaimable under pressure:
     * the arena can be refilled completely. */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        ASSERT_EQ(asx_obligation_reserve(rid, &oids[i]), ASX_OK);
    }
    ASSERT_EQ(asx_obligation_reserve(rid, &extra), ASX_E_RESOURCE_EXHAUSTED);

    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) { ASSERT_EQ(asx_obligation_abort(oids[i]), ASX_OK); }
}

TEST(obligation_exhaust_mixed_commit_abort) {
    asx_region_id rid;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, exhaust_mixed_body, &rid), ASX_OK);
}

/* ====================================================================
 * Determinism of exhaustion failures
 * ==================================================================== */

TEST(determinism_task_exhaustion_twice) {
    uint32_t run;
    asx_status results[2];

    for (run = 0; run < 2; run++) {
        asx_region_id rid;
        asx_task_id tid;
        uint32_t i;

        reset_all();
        ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);

        for (i = 0; i < ASX_MAX_TASKS; i++) {
            ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid), ASX_OK);
        }

        results[run] = asx_task_spawn(rid, poll_ok, NULL, &tid);
    }

    /* Same error both runs */
    ASSERT_EQ(results[0], results[1]);
    ASSERT_EQ(results[0], ASX_E_RESOURCE_EXHAUSTED);
}

TEST(determinism_channel_full_twice) {
    uint32_t run;
    asx_status results[2];

    for (run = 0; run < 2; run++) {
        asx_region_id rid;
        asx_channel_id ch;
        asx_send_permit p, overflow_p;
        uint32_t i;

        reset_all();
        ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
        ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_OK);

        for (i = 0; i < 4; i++) {
            ASSERT_EQ(asx_channel_try_reserve(ch, &p), ASX_OK);
            ASSERT_EQ(asx_send_permit_send(&p, (uint64_t)i), ASX_OK);
        }

        results[run] = asx_channel_try_reserve(ch, &overflow_p);
    }

    ASSERT_EQ(results[0], results[1]);
    ASSERT_EQ(results[0], ASX_E_CHANNEL_FULL);
}

TEST(determinism_timer_exhaustion_twice) {
    uint32_t run;
    asx_status results[2];

    for (run = 0; run < 2; run++) {
        asx_timer_wheel *wheel = asx_timer_wheel_global();
        asx_timer_handle h;
        uint32_t i;

        asx_timer_wheel_reset(wheel);

        for (i = 0; i < ASX_MAX_TIMERS; i++) {
            ASSERT_EQ(asx_timer_register(wheel, (asx_time)(100 + i), NULL, &h), ASX_OK);
        }

        results[run] = asx_timer_register(wheel, 9999, NULL, &h);
    }

    ASSERT_EQ(results[0], results[1]);
    ASSERT_EQ(results[0], ASX_E_RESOURCE_EXHAUSTED);
}

TEST(determinism_obligation_exhaustion_twice) {
    uint32_t run;
    asx_status results[2];

    for (run = 0; run < 2; run++) {
        asx_region_id rid;
        asx_task_id holder;
        asx_obligation_id oid;
        uint32_t i;

        reset_all();
        ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
        ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &holder), ASX_OK);

        for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
            ASSERT_EQ(reserve_for(rid, holder, &oid), ASX_OK);
        }

        results[run] = reserve_for(rid, holder, &oid);
    }

    ASSERT_EQ(results[0], results[1]);
    ASSERT_EQ(results[0], ASX_E_RESOURCE_EXHAUSTED);
}

/* ====================================================================
 * Scheduler with exhaustion boundary conditions
 * ==================================================================== */

TEST(scheduler_drains_full_task_arena) {
    asx_region_id rid;
    asx_task_id tids[ASX_MAX_TASKS];
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);

    /* Fill arena with immediate-complete tasks */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tids[i]), ASX_OK);
    }

    /* Scheduler should complete all */
    {
        asx_budget budget = asx_budget_from_polls(ASX_MAX_TASKS * 2);
        ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    }

    /* All tasks should have OK outcome */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        asx_outcome out;
        asx_outcome_severity sev;
        ASSERT_EQ(asx_task_get_outcome(tids[i], &out), ASX_OK);
        sev = asx_outcome_severity_of(&out);
        ASSERT_EQ(sev, ASX_OUTCOME_OK);
    }
}

TEST(scheduler_partial_drain_budget_boundary) {
    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);

    /* 16 always-pending tasks */
    for (i = 0; i < 16; i++) { ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK); }

    /* Budget of exactly 16 = one round */
    {
        asx_budget budget = asx_budget_from_polls(16);
        ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    }

    /* Budget of exactly 1 = partial round */
    {
        asx_budget budget = asx_budget_from_polls(1);
        ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    }
}

/* ====================================================================
 * No partial state corruption invariants
 * ==================================================================== */

TEST(no_corruption_after_task_spawn_failure) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_id last_valid = ASX_INVALID_ID;
    asx_outcome out;
    asx_outcome_severity sev;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);

    /* Fill arena, remembering the last valid task */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid), ASX_OK);
        last_valid = tid;
    }

    /* Failed spawn should not corrupt existing tasks */
    ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid), ASX_E_RESOURCE_EXHAUSTED);

    /* Run scheduler — existing tasks should still complete fine */
    {
        asx_budget budget = asx_budget_from_polls(ASX_MAX_TASKS * 2);
        ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    }

    /* Last valid task should have clean outcome */
    ASSERT_EQ(asx_task_get_outcome(last_valid, &out), ASX_OK);
    sev = asx_outcome_severity_of(&out);
    ASSERT_EQ(sev, ASX_OUTCOME_OK);
}

static void reserve_failure_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oids[ASX_MAX_OBLIGATIONS];
    asx_obligation_id extra;
    asx_obligation_state s;
    uint32_t i;

    /* Fill obligation arena */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        ASSERT_EQ(asx_obligation_reserve(rid, &oids[i]), ASX_OK);
    }

    /* Failed reserve */
    ASSERT_EQ(asx_obligation_reserve(rid, &extra), ASX_E_RESOURCE_EXHAUSTED);

    /* Existing obligations should still be operable */
    ASSERT_EQ(asx_obligation_commit(oids[0]), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(oids[0], &s), ASX_OK);
    ASSERT_EQ(s, ASX_OBLIGATION_COMMITTED);

    ASSERT_EQ(asx_obligation_abort(oids[1]), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(oids[1], &s), ASX_OK);
    ASSERT_EQ(s, ASX_OBLIGATION_ABORTED);

    for (i = 2; i < ASX_MAX_OBLIGATIONS; i++) { ASSERT_EQ(asx_obligation_commit(oids[i]), ASX_OK); }
}

TEST(no_corruption_after_obligation_reserve_failure) {
    asx_region_id rid;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, reserve_failure_body, &rid), ASX_OK);
}

/* ====================================================================
 * Region admission limits (Rust RegionLimits)
 * ==================================================================== */

/* An obligation exists only through a task, so the obligation limit is
 * probed from the poll of the region's single task: the second reservation
 * is refused while the first is pending, and committing it frees the room. */
typedef struct {
    asx_region_id region;
    asx_status first;
    asx_status over_limit;
    asx_status commit_first;
    asx_status again;
    asx_status commit_again;
} obligation_limit_probe;

static asx_status poll_obligation_limit(void *ud, asx_task_id self) {
    obligation_limit_probe *p = (obligation_limit_probe *)ud;
    asx_obligation_id o1 = ASX_INVALID_ID;
    asx_obligation_id o2 = ASX_INVALID_ID;
    (void)self;
    p->first = asx_obligation_reserve(p->region, &o1);
    p->over_limit = asx_obligation_reserve(p->region, &o2);
    p->commit_first = p->first == ASX_OK ? asx_obligation_commit(o1) : p->first;
    p->again = asx_obligation_reserve(p->region, &o2);
    p->commit_again = p->again == ASX_OK ? asx_obligation_commit(o2) : p->again;
    return ASX_OK;
}

/* Each limit caps live work: the admission past it is refused with
 * ASX_E_ADMISSION_LIMIT and changes nothing; finishing work frees room. */
TEST(region_limits_cap_live_tasks_children_and_obligations) {
    asx_region_id rid;
    asx_region_id child;
    asx_region_id extra_child;
    asx_region_limits limits;
    asx_task_id t1;
    asx_task_id t2;
    obligation_limit_probe probe;
    asx_budget budget;

    reset_all();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_region_get_limits(rid, &limits), ASX_OK);
    ASSERT_EQ(limits.max_tasks, ASX_REGION_UNLIMITED);
    ASSERT_EQ(limits.max_children, ASX_REGION_UNLIMITED);
    ASSERT_EQ(limits.max_obligations, ASX_REGION_UNLIMITED);

    limits.max_tasks = 1u;
    limits.max_children = 1u;
    limits.max_obligations = 1u;
    ASSERT_EQ(asx_region_set_limits(rid, &limits), ASX_OK);

    probe.region = rid;
    probe.first = ASX_E_PENDING;
    probe.over_limit = ASX_E_PENDING;
    probe.commit_first = ASX_E_PENDING;
    probe.again = ASX_E_PENDING;
    probe.commit_again = ASX_E_PENDING;
    ASSERT_EQ(asx_task_spawn(rid, poll_obligation_limit, &probe, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &t2), ASX_E_ADMISSION_LIMIT);
    ASSERT_EQ(asx_region_open_child(rid, &child), ASX_OK);
    ASSERT_EQ(asx_region_open_child(rid, &extra_child), ASX_E_ADMISSION_LIMIT);

    /* Completing the task and resolving the obligation free their room. */
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(probe.first, ASX_OK);
    ASSERT_EQ(probe.over_limit, ASX_E_ADMISSION_LIMIT);
    ASSERT_EQ(probe.commit_first, ASX_OK);
    ASSERT_EQ(probe.again, ASX_OK);
    ASSERT_EQ(probe.commit_again, ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &t2), ASX_OK);

    /* Unlimited again. */
    limits.max_tasks = ASX_REGION_UNLIMITED;
    ASSERT_EQ(asx_region_set_limits(rid, &limits), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &t1), ASX_OK);

    ASSERT_EQ(asx_region_set_limits(rid, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_get_limits(rid, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_set_limits(ASX_INVALID_ID, &limits), ASX_E_NOT_FOUND);
}

/* ====================================================================
 * A reset after use clears the used prefix of each arena and nothing
 * else (bd-9kll.14.2): the arenas' counts are the high-water marks, and a
 * slot at or above one has not been allocated since the last full clear.
 * What would show a slot left behind is a handle: it carries the slot's
 * generation, which a clear returns to 0.
 * ==================================================================== */

#define RESET_LOG_CAP ((uint32_t)ASX_MAX_TASKS + (uint32_t)ASX_MAX_TASKS / 2u + 4u)

typedef struct {
    uint64_t ids[RESET_LOG_CAP];
    uint32_t n;
} reset_log;

static void reset_log_add(reset_log *l, uint64_t id) {
    if (l->n < RESET_LOG_CAP) l->ids[l->n++] = id;
}

/* Fills the task arena with tasks that complete and closes their region,
 * then spawns half as many again in another: the arena is full of closed
 * tasks, so their slots are reclaimed and the new handles carry generation
 * 1. Returns 0 on success. */
static int reset_heavy_epoch(reset_log *l) {
    asx_region_id first;
    asx_region_id second;
    asx_task_id tid;
    asx_budget budget = asx_budget_from_polls(10000);
    uint32_t i;

    memset(l, 0, sizeof(*l));
    if (asx_region_open(test_runtime(), &first) != ASX_OK) return 1;
    reset_log_add(l, first);
    for (i = 0; i < (uint32_t)ASX_MAX_TASKS; i++) {
        if (asx_task_spawn(first, poll_ok, NULL, &tid) != ASX_OK) return 2;
        reset_log_add(l, tid);
    }
    if (asx_scheduler_run(first, &budget) != ASX_OK) return 3;
    if (asx_region_close(first) != ASX_OK) return 4;
    budget = asx_budget_from_polls(10000);
    if (asx_scheduler_run(first, &budget) != ASX_OK) return 4;
    budget = asx_budget_from_polls(10000);
    if (asx_region_drain(first, &budget) != ASX_OK) return 4;
    if (asx_region_open(test_runtime(), &second) != ASX_OK) return 5;
    reset_log_add(l, second);
    for (i = 0; i < (uint32_t)ASX_MAX_TASKS / 2u; i++) {
        if (asx_task_spawn(second, poll_ok, NULL, &tid) != ASX_OK) return 6;
        reset_log_add(l, tid);
    }
    budget = asx_budget_from_polls(10000);
    if (asx_scheduler_run(second, &budget) != ASX_OK) return 7;
    return 0;
}

static int reset_light_epoch(reset_log *l) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget = asx_budget_from_polls(100);
    uint32_t i;

    memset(l, 0, sizeof(*l));
    if (asx_region_open(test_runtime(), &rid) != ASX_OK) return 1;
    reset_log_add(l, rid);
    for (i = 0; i < 2u; i++) {
        if (asx_task_spawn(rid, poll_ok, NULL, &tid) != ASX_OK) return 2;
        reset_log_add(l, tid);
    }
    if (asx_scheduler_run(rid, &budget) != ASX_OK) return 3;
    return 0;
}

TEST(reset_after_heavy_then_light_use_leaves_no_slot_behind) {
    reset_log first;
    reset_log light;
    reset_log third;
    uint32_t i;

    reset_all();
    ASSERT_EQ(reset_heavy_epoch(&first), 0);
    ASSERT_TRUE(first.n > (uint32_t)ASX_MAX_TASKS);

    /* Heavy, reset, light, reset: the second reset visits only the two
     * slots the light epoch used; every other slot must still be as the
     * first reset left it. */
    reset_all();
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_TASK), (uint32_t)ASX_MAX_TASKS);
    ASSERT_EQ(reset_light_epoch(&light), 0);
    reset_all();
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_TASK), (uint32_t)ASX_MAX_TASKS);

    /* The same heavy epoch again issues the same handles: the same indexes
     * and generations, as from a process that never ran anything. */
    ASSERT_EQ(reset_heavy_epoch(&third), 0);
    ASSERT_EQ(third.n, first.n);
    for (i = 0; i < first.n; i++) { ASSERT_EQ(third.ids[i], first.ids[i]); }

    reset_all();
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_TASK), (uint32_t)ASX_MAX_TASKS);
}

/* ====================================================================
 * Main
 * ==================================================================== */

int main(void) {
    fprintf(stderr, "=== test_boundary_exhaustion ===\n");

    /* Cross-arena simultaneous exhaustion */
    RUN_TEST(cross_arena_task_and_obligation_both_exhausted);
    RUN_TEST(cross_arena_region_full_but_tasks_available);
    RUN_TEST(cross_arena_tasks_exhausted_regions_ok);

    /* Multi-region competition */
    RUN_TEST(multi_region_obligation_competition);
    RUN_TEST(multi_region_task_competition);

    /* Channel backpressure */
    RUN_TEST(channel_reserve_at_capacity_boundary);
    RUN_TEST(channel_sender_close_mid_reservation);
    RUN_TEST(channel_receiver_close_with_pending_permits);
    RUN_TEST(channel_full_send_then_recv_frees_capacity);
    RUN_TEST(channel_arena_exhaustion);

    /* Timer cancel+re-register under pressure */
    RUN_TEST(timer_exhaust_cancel_all_reregister);
    RUN_TEST(timer_cancel_reregister_interleaved);
    RUN_TEST(timer_fire_frees_slots_for_reregister);

    /* Capture arena */
    RUN_TEST(capture_arena_multi_task_boundary);
    RUN_TEST(capture_arena_exact_boundary_allocation);
    RUN_TEST(capture_arena_independent_per_region);

    /* Obligation edge cases */
    RUN_TEST(obligation_exhaust_then_commit_no_free_slots);
    RUN_TEST(obligation_exhaust_mixed_commit_abort);

    /* Determinism */
    RUN_TEST(determinism_task_exhaustion_twice);
    RUN_TEST(determinism_channel_full_twice);
    RUN_TEST(determinism_timer_exhaustion_twice);
    RUN_TEST(determinism_obligation_exhaustion_twice);

    /* Scheduler with exhaustion */
    RUN_TEST(scheduler_drains_full_task_arena);
    RUN_TEST(scheduler_partial_drain_budget_boundary);

    /* No corruption after failures */
    RUN_TEST(no_corruption_after_task_spawn_failure);
    RUN_TEST(no_corruption_after_obligation_reserve_failure);

    /* Region admission limits */
    RUN_TEST(region_limits_cap_live_tasks_children_and_obligations);

    /* Reset visits the used prefix of each arena */
    RUN_TEST(reset_after_heavy_then_light_use_leaves_no_slot_behind);

    TEST_REPORT();
    return test_failures;
}
