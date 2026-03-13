/*
 * e2e_robustness.c — end-to-end robustness boundary and fault-injection scenarios
 *
 * Exercises: resource exhaustion, stale handle rejection, region poisoning,
 * fault containment isolation, allocator seal, fault injection hooks,
 * and failure-atomic rollback.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/time/timer_wheel.h>
#include <stdio.h>
#include <string.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

/* -------------------------------------------------------------------
 * Scenario macros
 * ------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

/* Simple poll: completes immediately */
static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Scenario: Region arena exhaustion
 * ------------------------------------------------------------------- */

static void scenario_region_exhaustion(void) {
    SCENARIO_BEGIN("exhaustion.region_arena");
    asx_runtime_reset();

    asx_region_id regions[ASX_MAX_REGIONS + 1];
    uint32_t i;
    asx_status rc;

    /* Fill all region slots */
    for (i = 0; i < ASX_MAX_REGIONS; i++) {
        rc = asx_region_open(&regions[i]);
        SCENARIO_CHECK(rc == ASX_OK, "region_open during fill");
    }

    /* Next open should fail with resource exhaustion */
    rc = asx_region_open(&regions[ASX_MAX_REGIONS]);
    SCENARIO_CHECK(rc == ASX_E_RESOURCE_EXHAUSTED,
                   "expected RESOURCE_EXHAUSTED on region overflow");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Task arena exhaustion
 * ------------------------------------------------------------------- */

static void scenario_task_exhaustion(void) {
    SCENARIO_BEGIN("exhaustion.task_arena");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;
    asx_status rc;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Fill task slots */
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        rc = asx_task_spawn(rid, poll_complete, NULL, &tid);
        SCENARIO_CHECK(rc == ASX_OK, "task_spawn during fill");
    }

    /* Next spawn should fail */
    rc = asx_task_spawn(rid, poll_complete, NULL, &tid);
    SCENARIO_CHECK(rc == ASX_E_RESOURCE_EXHAUSTED, "expected RESOURCE_EXHAUSTED on task overflow");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Obligation arena exhaustion
 * ------------------------------------------------------------------- */

static void scenario_obligation_exhaustion(void) {
    SCENARIO_BEGIN("exhaustion.obligation_arena");
    asx_runtime_reset();

    asx_region_id rid;
    asx_obligation_id oid;
    uint32_t i;
    asx_status rc;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Fill obligation slots */
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        rc = asx_obligation_reserve(rid, &oid);
        SCENARIO_CHECK(rc == ASX_OK, "obligation_reserve during fill");
    }

    /* Next reserve should fail */
    rc = asx_obligation_reserve(rid, &oid);
    SCENARIO_CHECK(rc == ASX_E_RESOURCE_EXHAUSTED,
                   "expected RESOURCE_EXHAUSTED on obligation overflow");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Timer arena exhaustion
 * ------------------------------------------------------------------- */

static void scenario_timer_exhaustion(void) {
    SCENARIO_BEGIN("exhaustion.timer_arena");

    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_wheel_reset(wheel);

    asx_timer_handle h;
    uint32_t i;
    int marker = 0;
    asx_status rc;

    /* Fill timer slots */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        rc = asx_timer_register(wheel, (asx_time)(1000 + i), &marker, &h);
        SCENARIO_CHECK(rc == ASX_OK, "timer_register during fill");
    }

    /* Next register should fail */
    rc = asx_timer_register(wheel, 9999, &marker, &h);
    SCENARIO_CHECK(rc == ASX_E_RESOURCE_EXHAUSTED, "expected RESOURCE_EXHAUSTED on timer overflow");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Channel capacity exhaustion
 * ------------------------------------------------------------------- */

static void scenario_channel_full(void) {
    SCENARIO_BEGIN("exhaustion.channel_full");
    asx_runtime_reset();
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit p;
    uint32_t i;
    asx_status rc;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 4, &cid) == ASX_OK, "channel_create");

    /* Fill channel to capacity */
    for (i = 0; i < 4; i++) {
        rc = asx_channel_try_reserve(cid, &p);
        SCENARIO_CHECK(rc == ASX_OK, "reserve during fill");
        rc = asx_send_permit_send(&p, (uint64_t)(i + 1));
        SCENARIO_CHECK(rc == ASX_OK, "send during fill");
    }

    /* Next reserve should fail with channel full */
    rc = asx_channel_try_reserve(cid, &p);
    SCENARIO_CHECK(rc == ASX_E_CHANNEL_FULL, "expected CHANNEL_FULL on capacity overflow");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Stale region handle rejection
 * ------------------------------------------------------------------- */

static void scenario_stale_region_handle(void) {
    SCENARIO_BEGIN("stale.region_handle");
    asx_runtime_reset();

    asx_region_id rid1;
    asx_region_state st;
    asx_budget budget = asx_budget_from_polls(10);

    /* Open and close a region (slot becomes reusable) */
    SCENARIO_CHECK(asx_region_open(&rid1) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_region_drain(rid1, &budget) == ASX_OK, "region_drain");

    /* Open a new region in the same slot (generation incremented) */
    asx_region_id rid2;
    SCENARIO_CHECK(asx_region_open(&rid2) == ASX_OK, "region_open_2");

    /* Old handle should be stale */
    asx_status rc = asx_region_get_state(rid1, &st);
    SCENARIO_CHECK(rc == ASX_E_STALE_HANDLE || rc == ASX_E_NOT_FOUND,
                   "expected STALE_HANDLE on old region handle");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Region poison blocks mutations
 * ------------------------------------------------------------------- */

static void scenario_poison_blocks_mutations(void) {
    SCENARIO_BEGIN("poison.blocks_mutations");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    asx_obligation_id oid;
    int poisoned;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_region_poison(rid) == ASX_OK, "region_poison");

    SCENARIO_CHECK(asx_region_is_poisoned(rid, &poisoned) == ASX_OK && poisoned == 1,
                   "region should be poisoned");

    /* All mutating operations should fail */
    asx_status rc = asx_task_spawn(rid, poll_complete, NULL, &tid);
    SCENARIO_CHECK(rc == ASX_E_REGION_POISONED, "spawn should fail");

    rc = asx_obligation_reserve(rid, &oid);
    SCENARIO_CHECK(rc == ASX_E_REGION_POISONED, "reserve should fail");

    rc = asx_region_close(rid);
    SCENARIO_CHECK(rc == ASX_E_REGION_POISONED, "close should fail");

    /* Poison is idempotent */
    SCENARIO_CHECK(asx_region_poison(rid) == ASX_OK, "poison_idempotent");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Fault containment isolates regions
 * ------------------------------------------------------------------- */

static void scenario_fault_containment_isolation(void) {
    SCENARIO_BEGIN("fault.containment_isolation");
    asx_runtime_reset();

    asx_region_id r1, r2;
    asx_task_id tid;
    int poisoned;

    SCENARIO_CHECK(asx_region_open(&r1) == ASX_OK, "region_open_r1");
    SCENARIO_CHECK(asx_region_open(&r2) == ASX_OK, "region_open_r2");

    /* Poison r1 */
    SCENARIO_CHECK(asx_region_poison(r1) == ASX_OK, "poison_r1");

    /* r1 mutations fail */
    asx_status rc = asx_task_spawn(r1, poll_complete, NULL, &tid);
    SCENARIO_CHECK(rc == ASX_E_REGION_POISONED, "r1 spawn should fail");

    /* r2 is unaffected */
    rc = asx_task_spawn(r2, poll_complete, NULL, &tid);
    SCENARIO_CHECK(rc == ASX_OK, "r2 spawn should succeed");

    SCENARIO_CHECK(asx_region_is_poisoned(r2, &poisoned) == ASX_OK && poisoned == 0,
                   "r2 should not be poisoned");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Double obligation commit rejected
 * ------------------------------------------------------------------- */

static void scenario_double_commit_rejected(void) {
    SCENARIO_BEGIN("misuse.double_commit");
    asx_runtime_reset();

    asx_region_id rid;
    asx_obligation_id oid;
    asx_obligation_state os;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_obligation_reserve(rid, &oid) == ASX_OK, "reserve");
    SCENARIO_CHECK(asx_obligation_commit(oid) == ASX_OK, "first_commit");

    /* Second commit should fail */
    asx_status rc = asx_obligation_commit(oid);
    SCENARIO_CHECK(rc != ASX_OK, "double commit should fail");

    /* State should remain COMMITTED */
    SCENARIO_CHECK(asx_obligation_get_state(oid, &os) == ASX_OK && os == ASX_OBLIGATION_COMMITTED,
                   "should stay COMMITTED");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Double obligation abort rejected
 * ------------------------------------------------------------------- */

static void scenario_double_abort_rejected(void) {
    SCENARIO_BEGIN("misuse.double_abort");
    asx_runtime_reset();

    asx_region_id rid;
    asx_obligation_id oid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_obligation_reserve(rid, &oid) == ASX_OK, "reserve");
    SCENARIO_CHECK(asx_obligation_abort(oid) == ASX_OK, "first_abort");

    /* Second abort should fail */
    asx_status rc = asx_obligation_abort(oid);
    SCENARIO_CHECK(rc != ASX_OK, "double abort should fail");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Resource query accuracy
 * ------------------------------------------------------------------- */

static void scenario_resource_query_accuracy(void) {
    SCENARIO_BEGIN("resource.query_accuracy");
    asx_runtime_reset();

    uint32_t cap = asx_resource_capacity(ASX_RESOURCE_REGION);
    SCENARIO_CHECK(cap == ASX_MAX_REGIONS, "region capacity mismatch");

    uint32_t used_before = asx_resource_used(ASX_RESOURCE_REGION);

    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    uint32_t used_after = asx_resource_used(ASX_RESOURCE_REGION);
    SCENARIO_CHECK(used_after == used_before + 1, "used count should increment");

    uint32_t remaining = asx_resource_remaining(ASX_RESOURCE_REGION);
    SCENARIO_CHECK(remaining == cap - used_after, "remaining mismatch");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Recv on empty channel returns WOULD_BLOCK
 * ------------------------------------------------------------------- */

static void scenario_recv_empty_would_block(void) {
    SCENARIO_BEGIN("misuse.recv_empty");
    asx_runtime_reset();
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    uint64_t val;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 4, &cid) == ASX_OK, "channel_create");

    asx_status rc = asx_channel_try_recv(cid, &val);
    SCENARIO_CHECK(rc == ASX_E_WOULD_BLOCK, "expected WOULD_BLOCK on empty");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Close sender then try reserve
 * ------------------------------------------------------------------- */

static void scenario_reserve_after_sender_closed(void) {
    SCENARIO_BEGIN("misuse.reserve_after_close");
    asx_runtime_reset();
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit p;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 4, &cid) == ASX_OK, "channel_create");
    SCENARIO_CHECK(asx_channel_close_sender(cid) == ASX_OK, "close_sender");

    asx_status rc = asx_channel_try_reserve(cid, &p);
    SCENARIO_CHECK(rc == ASX_E_INVALID_STATE, "reserve should fail after sender closed");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Fault injection — controlled clock behavior
 * ------------------------------------------------------------------- */

static void scenario_fault_injection_clock(void) {
    SCENARIO_BEGIN("fault.injection_clock");
    asx_runtime_reset();

    asx_fault_injection fault;
    memset(&fault, 0, sizeof(fault));
    fault.kind = ASX_FAULT_CLOCK_SKEW;
    fault.param = 1000; /* 1000 ns skew */
    fault.trigger_after = 0;
    fault.trigger_count = 0;

    asx_status rc = asx_fault_inject(&fault);
    SCENARIO_CHECK(rc == ASX_OK, "fault_inject");

    uint32_t count = asx_fault_injection_count();
    SCENARIO_CHECK(count > 0, "fault_injection_count should be > 0");

    rc = asx_fault_clear();
    SCENARIO_CHECK(rc == ASX_OK, "fault_clear");

    count = asx_fault_injection_count();
    SCENARIO_CHECK(count == 0, "fault_injection_count should be 0 after clear");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Ghost violation recording
 * ------------------------------------------------------------------- */

static void scenario_ghost_violation_recording(void) {
    SCENARIO_BEGIN("ghost.violation_recording");
    asx_runtime_reset();
    asx_ghost_reset();

    /*
     * Trigger a protocol violation by attempting an illegal region
     * transition — e.g., close a region that was never opened.
     * The ghost monitor records this as ASX_GHOST_PROTOCOL_REGION.
     */
    (void)asx_ghost_check_region_transition(0, ASX_REGION_OPEN, ASX_REGION_CLOSED);

    uint32_t count = asx_ghost_violation_count();
    SCENARIO_CHECK(count >= 1, "violation count should be >= 1");

    asx_ghost_violation v;
    int ok = asx_ghost_violation_get(0, &v);
    SCENARIO_CHECK(ok == 1, "violation_get should return 1");
    SCENARIO_CHECK(v.kind == ASX_GHOST_PROTOCOL_REGION, "violation kind mismatch");

    asx_ghost_reset();
    count = asx_ghost_violation_count();
    SCENARIO_CHECK(count == 0, "violation count should be 0 after reset");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Capture arena exhaustion with failure-atomic rollback
 * ------------------------------------------------------------------- */

static void scenario_capture_arena_rollback(void) {
    SCENARIO_BEGIN("exhaustion.capture_arena_rollback");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    void *state;
    asx_status rc;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn tasks with large captured state to exhaust capture arena */
    uint32_t remaining = 0;
    SCENARIO_CHECK(asx_resource_region_capture_remaining(rid, &remaining) == ASX_OK,
                   "capture_remaining");

    /* Try to allocate more than remaining */
    if (remaining < ASX_REGION_CAPTURE_ARENA_BYTES) {
        rc = asx_task_spawn_captured(rid, poll_complete, remaining + 1, NULL, &tid, &state);
        SCENARIO_CHECK(rc == ASX_E_RESOURCE_EXHAUSTED,
                       "expected RESOURCE_EXHAUSTED on capture overflow");
    }

    /* Verify task count is unchanged (rollback) */
    uint32_t tasks_used = asx_resource_used(ASX_RESOURCE_TASK);
    SCENARIO_CHECK(tasks_used == 0, "task count should be 0 after rollback");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: Deterministic exhaustion outcome
 * ------------------------------------------------------------------- */

static void scenario_deterministic_exhaustion(void) {
    SCENARIO_BEGIN("determinism.exhaustion_outcome");

    /* Run exhaustion sequence twice — outcomes must match */
    uint32_t pass;
    asx_status results[2][ASX_MAX_REGIONS + 1];

    for (pass = 0; pass < 2; pass++) {
        asx_runtime_reset();
        uint32_t i;
        for (i = 0; i <= ASX_MAX_REGIONS; i++) {
            asx_region_id rid;
            results[pass][i] = asx_region_open(&rid);
        }
    }

    /* All results must match between passes */
    uint32_t i;
    for (i = 0; i <= ASX_MAX_REGIONS; i++) {
        SCENARIO_CHECK(results[0][i] == results[1][i], "exhaustion outcome not deterministic");
    }

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    scenario_region_exhaustion();
    scenario_task_exhaustion();
    scenario_obligation_exhaustion();
    scenario_timer_exhaustion();
    scenario_channel_full();
    scenario_stale_region_handle();
    scenario_poison_blocks_mutations();
    scenario_fault_containment_isolation();
    scenario_double_commit_rejected();
    scenario_double_abort_rejected();
    scenario_resource_query_accuracy();
    scenario_recv_empty_would_block();
    scenario_reserve_after_sender_closed();
    scenario_fault_injection_clock();
    scenario_ghost_violation_recording();
    scenario_capture_arena_rollback();
    scenario_deterministic_exhaustion();

    fprintf(stderr, "[e2e] robustness: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
