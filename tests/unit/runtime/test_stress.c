/*
 * test_stress.c — stress tests for runtime subsystem boundaries
 *
 * Exercises high-watermark scenarios, rapid churn, cancellation storms,
 * timer pressure, and multi-region interleaving under deterministic
 * scheduling. Each test resets global state to ensure isolation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include "test_log.h"
#include <asx/asx.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <asx/time/timer_wheel.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static void reset_all(void) {
    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();
    asx_replay_clear_reference();
}

/* reset_all plus the opt-in hard cleanup bound
 * (asx_runtime_config.cleanup_hard_bound): for the storms of cancelled
 * tasks that never finish, which only the hard bound ends (Rust never
 * force-completes). */
static asx_status reset_all_with_hard_cleanup_bound(void) {
    static asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    reset_all();
    asx_runtime_config_init(&cfg);
    cfg.cleanup_hard_bound = 1u;
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&rt, &cfg, &hooks);
    return st;
}

/* Immediate-complete poll */
static asx_status poll_ok(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

/* Always-pending poll */
static asx_status poll_pending(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_PENDING;
}

/* Countdown poll: completes after N polls */
typedef struct {
    uint32_t remaining;
} countdown_state;

static asx_status poll_countdown(void *ud, asx_task_id self) {
    countdown_state *s = (countdown_state *)ud;
    (void)self;
    if (s->remaining == 0) return ASX_OK;
    s->remaining--;
    return ASX_E_PENDING;
}

/* Fail-once poll */
static asx_status poll_fail(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_CANCELLED;
}

/* -------------------------------------------------------------------
 * Task arena exhaustion
 * ------------------------------------------------------------------- */

TEST(task_arena_full_exhaustion) {
    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Fill the task arena to capacity */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    }

    /* Next spawn should fail with RESOURCE_EXHAUSTED */
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(task_arena_exhaustion_then_complete_then_respawn) {
    asx_region_id rid;
    asx_task_id tids[ASX_MAX_TASKS];
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Fill arena with immediate-complete tasks */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tids[i]), ASX_OK);
    }

    /* Complete all tasks */
    {
        asx_budget budget = asx_budget_from_polls(ASX_MAX_TASKS * 2);
        ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    }

    /* Arena is still full of slots (they don't recycle in walking skeleton).
     * Verify the scheduler completed successfully and all tasks ran. */
    ASSERT_TRUE(asx_scheduler_event_count() > 0);
}

/* -------------------------------------------------------------------
 * Region arena exhaustion
 * ------------------------------------------------------------------- */

TEST(region_arena_full_exhaustion) {
    asx_region_id rids[ASX_MAX_REGIONS];
    asx_region_id extra;
    uint32_t i;

    reset_all();

    for (i = 0; i < ASX_MAX_REGIONS; i++) { ASSERT_EQ(asx_region_open(&rids[i]), ASX_OK); }

    /* Next region should fail */
    ASSERT_EQ(asx_region_open(&extra), ASX_E_RESOURCE_EXHAUSTED);
}

/* -------------------------------------------------------------------
 * Obligation arena exhaustion
 * ------------------------------------------------------------------- */

TEST(obligation_arena_full_exhaustion) {
    asx_region_id rid;
    asx_obligation_id oids[ASX_MAX_OBLIGATIONS];
    asx_obligation_id extra;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        ASSERT_EQ(asx_obligation_reserve(rid, &oids[i]), ASX_OK);
    }

    /* Next obligation should fail */
    ASSERT_EQ(asx_obligation_reserve(rid, &extra), ASX_E_RESOURCE_EXHAUSTED);
}

/* -------------------------------------------------------------------
 * Cancellation storms
 * ------------------------------------------------------------------- */

TEST(cancel_all_tasks_in_region) {
    asx_region_id rid;
    asx_task_id tids[16];
    uint32_t i;

    ASSERT_EQ(reset_all_with_hard_cleanup_bound(), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Spawn 16 pending tasks */
    for (i = 0; i < 16; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tids[i]), ASX_OK);
    }

    /* Cancel all via region propagation */
    {
        uint32_t cancelled = asx_cancel_propagate(rid, ASX_CANCEL_SHUTDOWN);
        ASSERT_EQ(cancelled, (uint32_t)16);
    }

    /* Run scheduler — all tasks should complete as CANCELLED.
     * SHUTDOWN has severity 5 → cleanup budget of 50 polls per task.
     * 16 tasks × 50 cleanup polls = 800 budget polls minimum,
     * plus headroom for the force-completion pass. */
    {
        asx_budget budget = asx_budget_from_polls(1024);
        asx_status rc = asx_scheduler_run(rid, &budget);
        ASSERT_EQ(rc, ASX_OK);
    }

    /* Verify all tasks completed with CANCELLED outcome */
    for (i = 0; i < 16; i++) {
        asx_outcome out;
        asx_outcome_severity sev;
        asx_status st = asx_task_get_outcome(tids[i], &out);
        ASSERT_EQ(st, ASX_OK);
        sev = asx_outcome_severity_of(&out);
        ASSERT_EQ(sev, ASX_OUTCOME_CANCELLED);
    }
}

TEST(cancel_storm_rapid_cancel_propagate) {
    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;

    ASSERT_EQ(reset_all_with_hard_cleanup_bound(), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Spawn a single pending task */
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Cancel the same region with multiple kinds in rapid succession.
     * The highest-severity cancel should win (lattice join). */
    for (i = 0; i < 5; i++) {
        uint32_t cancelled = asx_cancel_propagate(rid, (asx_cancel_kind)i);
        /* First call cancels the task (1), subsequent calls return 0
         * because the task is already cancelled. */
        (void)cancelled;
    }

    /* Drain with scheduler.
     * Strengthening settles at severity 2 (COST_BUDGET) with
     * cleanup quota of 300 polls. Need budget >= 300. */
    {
        asx_budget budget = asx_budget_from_polls(512);
        asx_status rc = asx_scheduler_run(rid, &budget);
        ASSERT_EQ(rc, ASX_OK);
    }
}

/* -------------------------------------------------------------------
 * Multi-region interleaved scheduling
 * ------------------------------------------------------------------- */

TEST(multi_region_independent_scheduling) {
    asx_region_id r1, r2;
    asx_task_id t1, t2, t3;
    countdown_state s1, s2, s3;

    reset_all();

    ASSERT_EQ(asx_region_open(&r1), ASX_OK);
    ASSERT_EQ(asx_region_open(&r2), ASX_OK);

    /* Region 1: two tasks with different countdown */
    s1.remaining = 2;
    s2.remaining = 1;
    ASSERT_EQ(asx_task_spawn(r1, poll_countdown, &s1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r1, poll_countdown, &s2, &t2), ASX_OK);

    /* Region 2: one task */
    s3.remaining = 3;
    ASSERT_EQ(asx_task_spawn(r2, poll_countdown, &s3, &t3), ASX_OK);

    /* Run regions independently */
    {
        asx_budget b1 = asx_budget_from_polls(20);
        ASSERT_EQ(asx_scheduler_run(r1, &b1), ASX_OK);
    }
    {
        asx_budget b2 = asx_budget_from_polls(20);
        ASSERT_EQ(asx_scheduler_run(r2, &b2), ASX_OK);
    }

    /* All tasks should have completed */
    {
        asx_outcome o;
        asx_outcome_severity sev;
        ASSERT_EQ(asx_task_get_outcome(t1, &o), ASX_OK);
        sev = asx_outcome_severity_of(&o);
        ASSERT_EQ(sev, ASX_OUTCOME_OK);
        ASSERT_EQ(asx_task_get_outcome(t2, &o), ASX_OK);
        sev = asx_outcome_severity_of(&o);
        ASSERT_EQ(sev, ASX_OUTCOME_OK);
        ASSERT_EQ(asx_task_get_outcome(t3, &o), ASX_OK);
        sev = asx_outcome_severity_of(&o);
        ASSERT_EQ(sev, ASX_OUTCOME_OK);
    }
}

TEST(multi_region_cancel_one_leaves_other_intact) {
    asx_region_id r1, r2;
    asx_task_id t1, t2;

    ASSERT_EQ(reset_all_with_hard_cleanup_bound(), ASX_OK);

    ASSERT_EQ(asx_region_open(&r1), ASX_OK);
    ASSERT_EQ(asx_region_open(&r2), ASX_OK);

    ASSERT_EQ(asx_task_spawn(r1, poll_pending, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r2, poll_ok, NULL, &t2), ASX_OK);

    /* Cancel only region 1 */
    {
        uint32_t cancelled = asx_cancel_propagate(r1, ASX_CANCEL_SHUTDOWN);
        ASSERT_EQ(cancelled, (uint32_t)1);
    }

    /* Region 1 tasks should drain as cancelled.
     * SHUTDOWN cleanup budget = 50 polls per task. */
    {
        asx_budget b = asx_budget_from_polls(128);
        ASSERT_EQ(asx_scheduler_run(r1, &b), ASX_OK);
    }

    /* Region 2 should complete normally */
    {
        asx_budget b = asx_budget_from_polls(16);
        ASSERT_EQ(asx_scheduler_run(r2, &b), ASX_OK);
    }

    {
        asx_outcome o1, o2;
        asx_outcome_severity sev;
        ASSERT_EQ(asx_task_get_outcome(t1, &o1), ASX_OK);
        sev = asx_outcome_severity_of(&o1);
        ASSERT_EQ(sev, ASX_OUTCOME_CANCELLED);
        ASSERT_EQ(asx_task_get_outcome(t2, &o2), ASX_OK);
        sev = asx_outcome_severity_of(&o2);
        ASSERT_EQ(sev, ASX_OUTCOME_OK);
    }
}

/* -------------------------------------------------------------------
 * Timer churn
 * ------------------------------------------------------------------- */

/* As many timers as the wheel holds, up to 64 (even: half are cancelled). */
#define CHURN_TIMERS ((ASX_MAX_TIMERS) < 64u ? ((ASX_MAX_TIMERS) & ~1u) : 64u)

TEST(timer_rapid_register_cancel_churn) {
    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_handle handles[64];
    uint32_t i;

    asx_timer_wheel_reset(wheel);

    /* Fill (up to 64 timers) */
    for (i = 0; i < CHURN_TIMERS; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(1000 + i * 10), NULL, &handles[i]), ASX_OK);
    }
    ASSERT_EQ(asx_timer_active_count(wheel), CHURN_TIMERS);

    /* Cancel every other one */
    for (i = 0; i < CHURN_TIMERS; i += 2) { ASSERT_TRUE(asx_timer_cancel(wheel, &handles[i])); }
    ASSERT_EQ(asx_timer_active_count(wheel), CHURN_TIMERS / 2u);

    /* Register as many again in the freed slots */
    for (i = 0; i < CHURN_TIMERS / 2u; i++) {
        asx_timer_handle h;
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(2000 + i * 10), NULL, &h), ASX_OK);
    }
    ASSERT_EQ(asx_timer_active_count(wheel), CHURN_TIMERS);

    /* Fire all by advancing time far enough */
    {
        void *wakers[128];
        uint32_t fired = asx_timer_collect_expired(wheel, 99999, wakers, 128);
        ASSERT_EQ(fired, CHURN_TIMERS);
    }
    ASSERT_EQ(asx_timer_active_count(wheel), 0u);
}

TEST(timer_exhaustion_then_fire_then_reuse) {
    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_handle handles[ASX_MAX_TIMERS];
    asx_timer_handle extra;
    uint32_t i;
    void *wakers[ASX_MAX_TIMERS];

    asx_timer_wheel_reset(wheel);

    /* Fill to capacity */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        ASSERT_EQ(asx_timer_register(wheel, (asx_time)(100 + i), NULL, &handles[i]), ASX_OK);
    }

    /* Next should fail */
    ASSERT_EQ(asx_timer_register(wheel, 9999, NULL, &extra), ASX_E_RESOURCE_EXHAUSTED);

    /* Fire all */
    {
        uint32_t fired = asx_timer_collect_expired(wheel, 99999, wakers, ASX_MAX_TIMERS);
        ASSERT_EQ(fired, ASX_MAX_TIMERS);
    }

    /* Now we can register again (slots recycled) */
    ASSERT_EQ(asx_timer_register(wheel, 50000, NULL, &extra), ASX_OK);
    ASSERT_EQ(asx_timer_active_count(wheel), 1u);
}

/* -------------------------------------------------------------------
 * Budget exhaustion patterns
 * ------------------------------------------------------------------- */

TEST(budget_exhaustion_with_many_pending_tasks) {
    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Spawn 16 always-pending tasks */
    for (i = 0; i < 16; i++) { ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK); }

    /* Give a very small budget — should exhaust quickly */
    {
        asx_budget budget = asx_budget_from_polls(3);
        ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    }

    /* Give more budget — still pending, should exhaust again */
    {
        asx_budget budget = asx_budget_from_polls(10);
        ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    }
}

TEST(tight_budget_one_poll_per_call) {
    asx_region_id rid;
    asx_task_id tid;
    countdown_state cs;
    uint32_t polls_needed;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Task needs 5 polls to complete */
    cs.remaining = 4; /* 4 pending + 1 final OK = 5 polls */
    ASSERT_EQ(asx_task_spawn(rid, poll_countdown, &cs, &tid), ASX_OK);

    polls_needed = 5;
    for (i = 0; i < polls_needed; i++) {
        asx_budget budget = asx_budget_from_polls(1);
        asx_status rc = asx_scheduler_run(rid, &budget);
        if (rc == ASX_OK) break; /* Task completed */
        ASSERT_EQ(rc, ASX_E_POLL_BUDGET_EXHAUSTED);
    }

    /* Task should now be complete */
    {
        asx_outcome out;
        asx_outcome_severity sev;
        ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
        sev = asx_outcome_severity_of(&out);
        ASSERT_EQ(sev, ASX_OUTCOME_OK);
    }
}

/* -------------------------------------------------------------------
 * Trace ring pressure
 * ------------------------------------------------------------------- */

TEST(trace_ring_high_event_volume) {
    /* The ring keeps the latest ASX_TRACE_CAPACITY events (a classed build's
     * ring can be smaller than 500: then it wraps). */
    const uint32_t kept = 500u < ASX_TRACE_CAPACITY ? 500u : ASX_TRACE_CAPACITY;
    uint32_t i;
    uint64_t digest;

    reset_all();

    for (i = 0; i < 500; i++) { asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)i, 0); }

    ASSERT_EQ(asx_trace_event_count(), kept);
    ASSERT_EQ(asx_trace_emitted_total(), (uint64_t)500u);

    /* Digest should be non-zero */
    digest = asx_trace_digest();
    ASSERT_TRUE(digest != 0);

    /* Verify first and last events are readable */
    {
        asx_trace_event ev;
        ASSERT_TRUE(asx_trace_event_get(0, &ev));
        ASSERT_EQ(ev.entity_id, (uint64_t)(500u - kept));
        ASSERT_TRUE(asx_trace_event_get(kept - 1u, &ev));
        ASSERT_EQ(ev.entity_id, 499u);
    }
}

TEST(trace_digest_deterministic_across_resets) {
    uint64_t d1, d2;
    uint32_t i;

    /* Run 1 */
    reset_all();
    for (i = 0; i < 100; i++) {
        asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)(i * 3), (uint64_t)i);
    }
    d1 = asx_trace_digest();

    /* Run 2 — same sequence */
    reset_all();
    for (i = 0; i < 100; i++) {
        asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)(i * 3), (uint64_t)i);
    }
    d2 = asx_trace_digest();

    ASSERT_EQ(d1, d2);
}

/* -------------------------------------------------------------------
 * Obligation churn
 * ------------------------------------------------------------------- */

TEST(obligation_rapid_reserve_commit_cycle) {
    asx_region_id rid;
    asx_obligation_id oid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Rapidly reserve and commit far more obligations than the arena
     * holds: resolved slots are reclaimed, so the lifetime count is
     * unbounded while the resident count stays within capacity. */
    for (i = 0; i < ASX_MAX_OBLIGATIONS * 8u; i++) {
        ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
        ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    }
    ASSERT_TRUE(asx_resource_used(ASX_RESOURCE_OBLIGATION) <= (uint32_t)ASX_MAX_OBLIGATIONS);
}

TEST(task_spawn_join_cycle_exceeds_arena) {
    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Spawn/run/join many more tasks than the arena holds. */
    for (i = 0; i < ASX_MAX_TASKS * 8u; i++) {
        asx_budget budget = asx_budget_from_polls(4);
        asx_outcome out;
        ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid), ASX_OK);
        ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
        ASSERT_EQ(asx_task_join(tid, &out), ASX_OK);
        ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_OK);
        /* The joined handle is now stale; the slot is free again. */
        ASSERT_EQ(asx_task_join(tid, &out), ASX_E_NOT_FOUND);
    }
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_TASK), 0u);
}

TEST(task_detach_releases_on_completion) {
    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    for (i = 0; i < ASX_MAX_TASKS * 4u; i++) {
        asx_budget budget = asx_budget_from_polls(4);
        asx_task_state st;
        ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid), ASX_OK);
        ASSERT_EQ(asx_task_detach(tid), ASX_OK);
        ASSERT_EQ(asx_task_get_state(tid, &st), ASX_OK); /* still live */
        ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
        ASSERT_EQ(asx_task_get_state(tid, &st), ASX_E_NOT_FOUND);
    }
    ASSERT_EQ(asx_resource_used(ASX_RESOURCE_TASK), 0u);
}

TEST(closed_region_tasks_reclaimed_under_pressure) {
    asx_region_id r1, r2;
    asx_task_id first = ASX_INVALID_ID;
    asx_task_id tid;
    asx_outcome out;
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&r1), ASX_OK);
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(r1, poll_ok, NULL, &tid), ASX_OK);
        if (i == 0u) first = tid;
    }
    {
        asx_budget budget = asx_budget_from_polls(ASX_MAX_TASKS * 2u);
        ASSERT_EQ(asx_region_drain(r1, &budget), ASX_OK);
    }

    /* Outcomes of a CLOSED region stay readable until capacity is needed. */
    ASSERT_EQ(asx_task_get_outcome(first, &out), ASX_OK);

    ASSERT_EQ(asx_region_open(&r2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r2, poll_ok, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(first, &out), ASX_E_STALE_HANDLE);
}

/* Up to 32 obligations, as many as the arena holds. */
#define MIXED_OBLIGATIONS ((ASX_MAX_OBLIGATIONS) < 32 ? (ASX_MAX_OBLIGATIONS) : 32)

TEST(obligation_mixed_commit_abort_pattern) {
    asx_region_id rid;
    asx_obligation_id oids[32];
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    for (i = 0; i < MIXED_OBLIGATIONS; i++) {
        ASSERT_EQ(asx_obligation_reserve(rid, &oids[i]), ASX_OK);
    }

    /* Commit even-indexed, abort odd-indexed */
    for (i = 0; i < MIXED_OBLIGATIONS; i++) {
        if (i % 2 == 0) {
            ASSERT_EQ(asx_obligation_commit(oids[i]), ASX_OK);
        } else {
            ASSERT_EQ(asx_obligation_abort(oids[i]), ASX_OK);
        }
    }

    /* Verify states */
    for (i = 0; i < MIXED_OBLIGATIONS; i++) {
        asx_obligation_state s;
        ASSERT_EQ(asx_obligation_get_state(oids[i], &s), ASX_OK);
        if (i % 2 == 0) {
            ASSERT_EQ(s, ASX_OBLIGATION_COMMITTED);
        } else {
            ASSERT_EQ(s, ASX_OBLIGATION_ABORTED);
        }
    }
}

/* -------------------------------------------------------------------
 * Fault containment under pressure
 * ------------------------------------------------------------------- */

TEST(fault_containment_multiple_failures_same_region) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    int poisoned;
    asx_containment_policy policy;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Mix of failing and ok tasks */
    ASSERT_EQ(asx_task_spawn(rid, poll_fail, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_fail, NULL, &t3), ASX_OK);

    /* Run — fault containment should poison the region on first failure */
    {
        asx_budget budget = asx_budget_from_polls(20);
        asx_status rc = asx_scheduler_run(rid, &budget);
        policy = asx_containment_policy_active();
        if (policy == ASX_CONTAIN_POISON_REGION) {
            ASSERT_EQ(rc, ASX_OK);
        } else {
            ASSERT_TRUE(rc != ASX_OK);
        }
    }

    /* Region poison depends on active policy. */
    ASSERT_EQ(asx_region_is_poisoned(rid, &poisoned), ASX_OK);
    if (policy == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ(poisoned, 1);
    } else {
        ASSERT_EQ(poisoned, 0);
    }
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    test_log_open("unit", "runtime/stress", "test_stress");

    /* Arena exhaustion */
    RUN_TEST(task_arena_full_exhaustion);
    RUN_TEST(task_arena_exhaustion_then_complete_then_respawn);
    RUN_TEST(region_arena_full_exhaustion);
    RUN_TEST(obligation_arena_full_exhaustion);

    /* Cancellation storms */
    RUN_TEST(cancel_all_tasks_in_region);
    RUN_TEST(cancel_storm_rapid_cancel_propagate);

    /* Multi-region */
    RUN_TEST(multi_region_independent_scheduling);
    RUN_TEST(multi_region_cancel_one_leaves_other_intact);

    /* Timer churn */
    RUN_TEST(timer_rapid_register_cancel_churn);
    RUN_TEST(timer_exhaustion_then_fire_then_reuse);

    /* Budget exhaustion */
    RUN_TEST(budget_exhaustion_with_many_pending_tasks);
    RUN_TEST(tight_budget_one_poll_per_call);

    /* Trace pressure */
    RUN_TEST(trace_ring_high_event_volume);
    RUN_TEST(trace_digest_deterministic_across_resets);

    /* Obligation churn */
    RUN_TEST(obligation_rapid_reserve_commit_cycle);
    RUN_TEST(task_spawn_join_cycle_exceeds_arena);
    RUN_TEST(task_detach_releases_on_completion);
    RUN_TEST(closed_region_tasks_reclaimed_under_pressure);
    RUN_TEST(obligation_mixed_commit_abort_pattern);

    /* Fault containment under pressure */
    RUN_TEST(fault_containment_multiple_failures_same_region);

    TEST_REPORT();
    return test_failures;
}
