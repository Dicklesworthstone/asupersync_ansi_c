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
#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/core/resource.h>
#include <asx/core/channel.h>
#include <asx/time/timer_wheel.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* ---- Helpers ---- */

static void reset_all(void)
{
    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();
    asx_channel_reset();
    asx_replay_clear_reference();
}

static asx_status poll_ok(void *ud, asx_task_id self)
{
    (void)ud; (void)self;
    return ASX_OK;
}

static asx_status poll_pending(void *ud, asx_task_id self)
{
    (void)ud; (void)self;
    return ASX_E_PENDING;
}

/* ====================================================================
 * Cross-arena simultaneous exhaustion
 * ==================================================================== */

TEST(cross_arena_task_and_obligation_both_exhausted)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_obligation_id oid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Fill all task slots */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid), ASX_OK);
    }
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_TASK), (uint32_t)0);

    /* Fill all obligation slots */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    }
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_OBLIGATION), (uint32_t)0);

    /* Both should fail independently with correct errors */
    ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid),
              ASX_E_RESOURCE_EXHAUSTED);

    /* State should be consistent */
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_TASK), (uint32_t)ASX_MAX_TASKS);
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_OBLIGATION),
              (uint32_t)ASX_MAX_OBLIGATIONS);
}

TEST(cross_arena_region_full_but_tasks_available)
{
    asx_region_id rids[ASX_MAX_REGIONS];
    asx_region_id overflow;
    asx_task_id tid;
    uint32_t i;

    reset_all();

    /* Fill all region slots */
    for (i = 0; i < ASX_MAX_REGIONS; i++) {
        ASSERT_EQ(asx_region_open(&rids[i]), ASX_OK);
    }

    /* Region arena is full */
    ASSERT_EQ(asx_region_open(&overflow), ASX_E_RESOURCE_EXHAUSTED);

    /* But tasks are still available within existing regions */
    ASSERT_EQ(asx_resource_remaining(ASX_RESOURCE_TASK),
              (uint32_t)ASX_MAX_TASKS);
    ASSERT_EQ(asx_task_spawn(rids[0], poll_ok, NULL, &tid), ASX_OK);
}

TEST(cross_arena_tasks_exhausted_regions_ok)
{
    asx_region_id r1, r2;
    asx_task_id tid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&r1), ASX_OK);

    /* Fill all task slots via region 1 */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(r1, poll_ok, NULL, &tid), ASX_OK);
    }

    /* Task arena exhausted — but regions are fine */
    ASSERT_EQ(asx_region_open(&r2), ASX_OK);

    /* Cannot spawn in new region either (task arena shared) */
    ASSERT_EQ(asx_task_spawn(r2, poll_ok, NULL, &tid),
              ASX_E_RESOURCE_EXHAUSTED);
}

/* ====================================================================
 * Multi-region obligation arena competition
 * ==================================================================== */

TEST(multi_region_obligation_competition)
{
    asx_region_id r1, r2;
    asx_obligation_id oid;
    uint32_t i;
    uint32_t half = ASX_MAX_OBLIGATIONS / 2u;

    reset_all();
    ASSERT_EQ(asx_region_open(&r1), ASX_OK);
    ASSERT_EQ(asx_region_open(&r2), ASX_OK);

    /* Region 1 takes half the arena */
    for (i = 0; i < half; i++) {
        ASSERT_EQ(asx_obligation_reserve(r1, &oid), ASX_OK);
    }

    /* Region 2 takes the other half */
    for (i = 0; i < ASX_MAX_OBLIGATIONS - half; i++) {
        ASSERT_EQ(asx_obligation_reserve(r2, &oid), ASX_OK);
    }

    /* Both regions now fail on new reservations */
    ASSERT_EQ(asx_obligation_reserve(r1, &oid), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_obligation_reserve(r2, &oid), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_OBLIGATION),
              (uint32_t)ASX_MAX_OBLIGATIONS);
}

TEST(multi_region_task_competition)
{
    asx_region_id r1, r2;
    asx_task_id tid;
    uint32_t i;
    uint32_t half = ASX_MAX_TASKS / 2u;

    reset_all();
    ASSERT_EQ(asx_region_open(&r1), ASX_OK);
    ASSERT_EQ(asx_region_open(&r2), ASX_OK);

    /* Region 1 takes half */
    for (i = 0; i < half; i++) {
        ASSERT_EQ(asx_task_spawn(r1, poll_ok, NULL, &tid), ASX_OK);
    }

    /* Region 2 takes the rest */
    for (i = 0; i < ASX_MAX_TASKS - half; i++) {
        ASSERT_EQ(asx_task_spawn(r2, poll_ok, NULL, &tid), ASX_OK);
    }

    /* Both regions fail on spawn */
    ASSERT_EQ(asx_task_spawn(r1, poll_ok, NULL, &tid),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_task_spawn(r2, poll_ok, NULL, &tid),
              ASX_E_RESOURCE_EXHAUSTED);
}

/* ====================================================================
 * Channel backpressure edge cases
 * ==================================================================== */

TEST(channel_reserve_at_capacity_boundary)
{
    asx_region_id rid;
    asx_channel_id ch;
    asx_send_permit permits[4];
    asx_send_permit extra;
    uint32_t qlen, rcount;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_OK);

    /* Reserve all 4 slots */
    for (i = 0; i < 4; i++) {
        ASSERT_EQ(asx_channel_try_reserve(ch, &permits[i]), ASX_OK);
    }

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

TEST(channel_sender_close_mid_reservation)
{
    asx_region_id rid;
    asx_channel_id ch;
    asx_send_permit permit;
    asx_send_permit post_close;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_OK);

    /* Reserve a slot */
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);

    /* Close sender while permit is outstanding */
    ASSERT_EQ(asx_channel_close_sender(ch), ASX_OK);

    /* Existing permit can still send */
    ASSERT_EQ(asx_send_permit_send(&permit, 42), ASX_OK);

    /* But new reservations fail */
    ASSERT_EQ(asx_channel_try_reserve(ch, &post_close),
              ASX_E_INVALID_STATE);
}

TEST(channel_receiver_close_with_pending_permits)
{
    asx_region_id rid;
    asx_channel_id ch;
    asx_send_permit permit;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_OK);

    /* Reserve a slot */
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);

    /* Close receiver while permit is outstanding */
    ASSERT_EQ(asx_channel_close_receiver(ch), ASX_OK);

    /* Send via existing permit fails with DISCONNECTED */
    ASSERT_EQ(asx_send_permit_send(&permit, 42), ASX_E_DISCONNECTED);
}

TEST(channel_full_send_then_recv_frees_capacity)
{
    asx_region_id rid;
    asx_channel_id ch;
    asx_send_permit p;
    uint64_t val;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
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

TEST(channel_arena_exhaustion)
{
    asx_region_id rid;
    asx_channel_id ch;
    asx_channel_id overflow;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Create all 16 channels */
    for (i = 0; i < ASX_MAX_CHANNELS; i++) {
        ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_OK);
    }

    /* 17th should fail */
    ASSERT_EQ(asx_channel_create(rid, 4, &overflow),
              ASX_E_RESOURCE_EXHAUSTED);
}

/* ====================================================================
 * Timer wheel exhaustion with cancel+re-register
 * ==================================================================== */

TEST(timer_exhaust_cancel_all_reregister)
{
    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_handle handles[ASX_MAX_TIMERS];
    asx_timer_handle fresh;
    uint32_t i;

    asx_timer_wheel_reset(wheel);

    /* Fill to capacity */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(1000 + i),
                                      NULL, &handles[i]),
                  ASX_OK);
    }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)ASX_MAX_TIMERS);

    /* Overflow fails */
    ASSERT_EQ(asx_timer_register(wheel, 9999, NULL, &fresh),
              ASX_E_RESOURCE_EXHAUSTED);

    /* Cancel ALL timers */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        ASSERT_TRUE(asx_timer_cancel(wheel, &handles[i]));
    }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)0);

    /* Re-register all slots — should succeed (recycled) */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(2000 + i),
                                      NULL, &handles[i]),
                  ASX_OK);
    }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)ASX_MAX_TIMERS);
}

TEST(timer_cancel_reregister_interleaved)
{
    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_handle handles[ASX_MAX_TIMERS];
    asx_timer_handle fresh;
    uint32_t i;

    asx_timer_wheel_reset(wheel);

    /* Fill to capacity */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(1000 + i),
                                      NULL, &handles[i]),
                  ASX_OK);
    }

    /* Cancel even-indexed, then re-register into freed slots */
    for (i = 0; i < ASX_MAX_TIMERS; i += 2) {
        ASSERT_TRUE(asx_timer_cancel(wheel, &handles[i]));
    }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)(ASX_MAX_TIMERS / 2));

    /* Fill freed slots */
    for (i = 0; i < ASX_MAX_TIMERS / 2; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(5000 + i),
                                      NULL, &fresh),
                  ASX_OK);
    }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)ASX_MAX_TIMERS);

    /* Should be full again */
    ASSERT_EQ(asx_timer_register(wheel, 9999, NULL, &fresh),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(timer_fire_frees_slots_for_reregister)
{
    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_handle handles[ASX_MAX_TIMERS];
    asx_timer_handle fresh;
    void *wakers[ASX_MAX_TIMERS];
    uint32_t fired;
    uint32_t i;

    asx_timer_wheel_reset(wheel);

    /* Fill to capacity with deadlines at t=100..227 */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(100 + i),
                                      NULL, &handles[i]),
                  ASX_OK);
    }

    /* Fire half (deadlines 100..163) */
    fired = asx_timer_collect_expired(wheel, 163, wakers, ASX_MAX_TIMERS);
    ASSERT_EQ(fired, (uint32_t)64);
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)64);

    /* Re-register into freed slots */
    for (i = 0; i < 64; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(10000 + i),
                                      NULL, &fresh),
                  ASX_OK);
    }
    ASSERT_EQ(asx_timer_active_count(wheel), (uint32_t)ASX_MAX_TIMERS);
}

/* ====================================================================
 * Capture arena multi-task consumption
 * ==================================================================== */

TEST(capture_arena_multi_task_boundary)
{
    asx_region_id rid;
    asx_task_id tid;
    void *state;
    uint32_t remaining;
    uint32_t chunk_size;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Allocate 4 tasks each consuming 1/4 of the capture arena */
    chunk_size = ASX_REGION_CAPTURE_ARENA_BYTES / 4u;
    for (i = 0; i < 4; i++) {
        ASSERT_EQ(asx_task_spawn_captured(rid, poll_ok, chunk_size,
                                           NULL, &tid, &state),
                  ASX_OK);
    }

    /* Check remaining — should be near zero (alignment overhead) */
    ASSERT_EQ(asx_resource_region_capture_remaining(rid, &remaining), ASX_OK);
    ASSERT_TRUE(remaining < chunk_size);

    /* One more should fail */
    ASSERT_EQ(asx_task_spawn_captured(rid, poll_ok, chunk_size,
                                       NULL, &tid, &state),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(capture_arena_exact_boundary_allocation)
{
    asx_region_id rid;
    asx_task_id tid;
    void *state;
    uint32_t remaining;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Allocate most of the arena */
    ASSERT_EQ(asx_task_spawn_captured(rid, poll_ok,
              ASX_REGION_CAPTURE_ARENA_BYTES - 64u,
              NULL, &tid, &state),
              ASX_OK);

    ASSERT_EQ(asx_resource_region_capture_remaining(rid, &remaining), ASX_OK);
    ASSERT_TRUE(remaining <= 64u);

    /* Allocation of remaining + 1 should fail */
    if (remaining > 0) {
        ASSERT_EQ(asx_task_spawn_captured(rid, poll_ok,
                  remaining + 1u, NULL, &tid, &state),
                  ASX_E_RESOURCE_EXHAUSTED);
    }
}

TEST(capture_arena_independent_per_region)
{
    asx_region_id r1, r2;
    asx_task_id tid;
    void *state;
    uint32_t r1_remaining, r2_remaining;

    reset_all();
    ASSERT_EQ(asx_region_open(&r1), ASX_OK);
    ASSERT_EQ(asx_region_open(&r2), ASX_OK);

    /* Consume half of region 1's capture arena */
    ASSERT_EQ(asx_task_spawn_captured(r1, poll_ok,
              ASX_REGION_CAPTURE_ARENA_BYTES / 2u,
              NULL, &tid, &state), ASX_OK);

    /* Region 2's capture arena should be untouched */
    ASSERT_EQ(asx_resource_region_capture_remaining(r1, &r1_remaining),
              ASX_OK);
    ASSERT_EQ(asx_resource_region_capture_remaining(r2, &r2_remaining),
              ASX_OK);

    ASSERT_TRUE(r1_remaining < ASX_REGION_CAPTURE_ARENA_BYTES);
    ASSERT_EQ(r2_remaining, (uint32_t)ASX_REGION_CAPTURE_ARENA_BYTES);
}

/* ====================================================================
 * Obligation edge cases at exhaustion
 * ==================================================================== */

TEST(obligation_exhaust_then_commit_no_free_slots)
{
    asx_region_id rid;
    asx_obligation_id oids[ASX_MAX_OBLIGATIONS];
    asx_obligation_id extra;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Fill all slots */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        ASSERT_EQ(asx_obligation_reserve(rid, &oids[i]), ASX_OK);
    }

    /* Exhaust */
    ASSERT_EQ(asx_obligation_reserve(rid, &extra), ASX_E_RESOURCE_EXHAUSTED);

    /* Commit all — arena stays full (no recycling in walking skeleton) */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        ASSERT_EQ(asx_obligation_commit(oids[i]), ASX_OK);
    }

    /* Still exhausted */
    ASSERT_EQ(asx_obligation_reserve(rid, &extra), ASX_E_RESOURCE_EXHAUSTED);

    /* All obligations in committed state */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        asx_obligation_state s;
        ASSERT_EQ(asx_obligation_get_state(oids[i], &s), ASX_OK);
        ASSERT_EQ(s, ASX_OBLIGATION_COMMITTED);
    }
}

TEST(obligation_exhaust_mixed_commit_abort)
{
    asx_region_id rid;
    asx_obligation_id oids[ASX_MAX_OBLIGATIONS];
    asx_obligation_id extra;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

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

    /* Still exhausted (no slot recycling) */
    ASSERT_EQ(asx_obligation_reserve(rid, &extra), ASX_E_RESOURCE_EXHAUSTED);

    /* Verify final states */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        asx_obligation_state s;
        ASSERT_EQ(asx_obligation_get_state(oids[i], &s), ASX_OK);
        if (i % 2 == 0) {
            ASSERT_EQ(s, ASX_OBLIGATION_COMMITTED);
        } else {
            ASSERT_EQ(s, ASX_OBLIGATION_ABORTED);
        }
    }
}

/* ====================================================================
 * Determinism of exhaustion failures
 * ==================================================================== */

TEST(determinism_task_exhaustion_twice)
{
    uint32_t run;
    asx_status results[2];

    for (run = 0; run < 2; run++) {
        asx_region_id rid;
        asx_task_id tid;
        uint32_t i;

        reset_all();
        ASSERT_EQ(asx_region_open(&rid), ASX_OK);

        for (i = 0; i < ASX_MAX_TASKS; i++) {
            ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid), ASX_OK);
        }

        results[run] = asx_task_spawn(rid, poll_ok, NULL, &tid);
    }

    /* Same error both runs */
    ASSERT_EQ(results[0], results[1]);
    ASSERT_EQ(results[0], ASX_E_RESOURCE_EXHAUSTED);
}

TEST(determinism_channel_full_twice)
{
    uint32_t run;
    asx_status results[2];

    for (run = 0; run < 2; run++) {
        asx_region_id rid;
        asx_channel_id ch;
        asx_send_permit p, overflow_p;
        uint32_t i;

        reset_all();
        ASSERT_EQ(asx_region_open(&rid), ASX_OK);
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

TEST(determinism_timer_exhaustion_twice)
{
    uint32_t run;
    asx_status results[2];

    for (run = 0; run < 2; run++) {
        asx_timer_wheel *wheel = asx_timer_wheel_global();
        asx_timer_handle h;
        uint32_t i;

        asx_timer_wheel_reset(wheel);

        for (i = 0; i < ASX_MAX_TIMERS; i++) {
            ASSERT_EQ(asx_timer_register(wheel, (asx_time)(100 + i),
                                          NULL, &h), ASX_OK);
        }

        results[run] = asx_timer_register(wheel, 9999, NULL, &h);
    }

    ASSERT_EQ(results[0], results[1]);
    ASSERT_EQ(results[0], ASX_E_RESOURCE_EXHAUSTED);
}

TEST(determinism_obligation_exhaustion_twice)
{
    uint32_t run;
    asx_status results[2];

    for (run = 0; run < 2; run++) {
        asx_region_id rid;
        asx_obligation_id oid;
        uint32_t i;

        reset_all();
        ASSERT_EQ(asx_region_open(&rid), ASX_OK);

        for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
            ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
        }

        results[run] = asx_obligation_reserve(rid, &oid);
    }

    ASSERT_EQ(results[0], results[1]);
    ASSERT_EQ(results[0], ASX_E_RESOURCE_EXHAUSTED);
}

/* ====================================================================
 * Scheduler with exhaustion boundary conditions
 * ==================================================================== */

TEST(scheduler_drains_full_task_arena)
{
    asx_region_id rid;
    asx_task_id tids[ASX_MAX_TASKS];
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

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

TEST(scheduler_partial_drain_budget_boundary)
{
    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* 16 always-pending tasks */
    for (i = 0; i < 16; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    }

    /* Budget of exactly 16 = one round */
    {
        asx_budget budget = asx_budget_from_polls(16);
        ASSERT_EQ(asx_scheduler_run(rid, &budget),
                  ASX_E_POLL_BUDGET_EXHAUSTED);
    }

    /* Budget of exactly 1 = partial round */
    {
        asx_budget budget = asx_budget_from_polls(1);
        ASSERT_EQ(asx_scheduler_run(rid, &budget),
                  ASX_E_POLL_BUDGET_EXHAUSTED);
    }
}

/* ====================================================================
 * No partial state corruption invariants
 * ==================================================================== */

TEST(no_corruption_after_task_spawn_failure)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_task_id last_valid;
    asx_outcome out;
    asx_outcome_severity sev;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Fill arena, remembering the last valid task */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid), ASX_OK);
        last_valid = tid;
    }

    /* Failed spawn should not corrupt existing tasks */
    ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid),
              ASX_E_RESOURCE_EXHAUSTED);

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

TEST(no_corruption_after_obligation_reserve_failure)
{
    asx_region_id rid;
    asx_obligation_id oids[ASX_MAX_OBLIGATIONS];
    asx_obligation_id extra;
    asx_obligation_state s;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

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
}

/* ====================================================================
 * Main
 * ==================================================================== */

int main(void)
{
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

    TEST_REPORT();
    return test_failures;
}
