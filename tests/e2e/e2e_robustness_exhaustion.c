/*
 * e2e_robustness_exhaustion.c — robustness e2e for resource exhaustion boundaries
 *
 * Exercises: region/task arena exhaustion, channel capacity limits,
 * timer wheel saturation, obligation arena limits, budget exhaustion,
 * and failure-atomic rollback after exhaustion.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/runtime.h>
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
 * Helpers
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

static asx_status poll_pending(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_PENDING;
}

static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------- */

/* robust-exhaustion-001: task arena exhaustion returns deterministic error */
static void scenario_task_arena_exhaustion(void) {
    SCENARIO_BEGIN("robust-exhaustion-001.task_arena_full");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    int spawned = 0;
    asx_status rc;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Fill task arena to capacity */
    for (;;) {
        rc = asx_task_spawn(rid, poll_pending, NULL, &tid);
        if (rc != ASX_OK) break;
        spawned++;
    }

    SCENARIO_CHECK(spawned > 0, "should spawn at least one task");
    SCENARIO_CHECK(rc == ASX_E_RESOURCE_EXHAUSTED,
                   "expected RESOURCE_EXHAUSTED on task arena full");

    /* Verify region state is still queryable (no corruption) */
    asx_region_state rs;
    SCENARIO_CHECK(asx_region_get_state(rid, &rs) == ASX_OK, "region state query after exhaustion");
    SCENARIO_CHECK(rs == ASX_REGION_OPEN, "region should still be OPEN");

    SCENARIO_END();
}

/* robust-exhaustion-002: region arena exhaustion returns deterministic error */
static void scenario_region_arena_exhaustion(void) {
    SCENARIO_BEGIN("robust-exhaustion-002.region_arena_full");
    asx_runtime_reset();

    asx_region_id rid;
    int opened = 0;
    asx_status rc;

    /* Fill region arena to capacity */
    for (;;) {
        rc = asx_region_open(&rid);
        if (rc != ASX_OK) break;
        opened++;
    }

    SCENARIO_CHECK(opened > 0, "should open at least one region");
    SCENARIO_CHECK(rc == ASX_E_RESOURCE_EXHAUSTED,
                   "expected RESOURCE_EXHAUSTED on region arena full");

    SCENARIO_END();
}

/* robust-exhaustion-003: channel capacity exhaustion */
static void scenario_channel_capacity_exhaustion(void) {
    SCENARIO_BEGIN("robust-exhaustion-003.channel_full");
    asx_runtime_reset();
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit permit;
    int sent = 0;
    asx_status rc;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 4, &cid) == ASX_OK, "channel_create");

    /* Fill channel to capacity */
    for (;;) {
        rc = asx_channel_try_reserve(cid, &permit);
        if (rc != ASX_OK) break;
        SCENARIO_CHECK(asx_send_permit_send(&permit, (uint64_t)(sent + 1)) == ASX_OK,
                       "permit_send");
        sent++;
    }

    SCENARIO_CHECK(sent == 4, "should send exactly capacity(4) messages");
    SCENARIO_CHECK(rc == ASX_E_WOULD_BLOCK || rc == ASX_E_CHANNEL_FULL,
                   "expected WOULD_BLOCK or CHANNEL_FULL");

    /* Verify channel still functional after exhaustion */
    uint64_t value;
    SCENARIO_CHECK(asx_channel_try_recv(cid, &value) == ASX_OK, "recv after channel full");
    SCENARIO_CHECK(value == 1, "first recv should be 1 (FIFO)");

    SCENARIO_END();
}

/* robust-exhaustion-004: timer wheel exhaustion */
static void scenario_timer_wheel_exhaustion(void) {
    SCENARIO_BEGIN("robust-exhaustion-004.timer_wheel_full");

    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_wheel_reset(wheel);

    asx_timer_handle h;
    int registered = 0;
    asx_status rc;

    /* Fill timer wheel to capacity */
    for (;;) {
        rc = asx_timer_register(wheel, (asx_time)(100 + registered), NULL, &h);
        if (rc != ASX_OK) break;
        registered++;
    }

    SCENARIO_CHECK(registered > 0, "should register at least one timer");
    SCENARIO_CHECK(rc == ASX_E_RESOURCE_EXHAUSTED,
                   "expected RESOURCE_EXHAUSTED on timer wheel full");

    SCENARIO_END();
}

/* robust-exhaustion-005: obligation arena exhaustion */
static void scenario_obligation_arena_exhaustion(void) {
    SCENARIO_BEGIN("robust-exhaustion-005.obligation_arena_full");
    asx_runtime_reset();

    asx_region_id rid;
    asx_obligation_id oid;
    int reserved = 0;
    asx_status rc;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Fill obligation arena to capacity */
    for (;;) {
        rc = asx_obligation_reserve(rid, &oid);
        if (rc != ASX_OK) break;
        reserved++;
    }

    SCENARIO_CHECK(reserved > 0, "should reserve at least one obligation");
    SCENARIO_CHECK(rc == ASX_E_RESOURCE_EXHAUSTED,
                   "expected RESOURCE_EXHAUSTED on obligation arena full");

    SCENARIO_END();
}

/* robust-exhaustion-006: poll budget exhaustion is deterministic */
static void scenario_budget_exhaustion_deterministic(void) {
    SCENARIO_BEGIN("robust-exhaustion-006.budget_deterministic");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_pending, NULL, &tid) == ASX_OK, "task_spawn");

    /* Run with tight budget — should exhaust deterministically */
    asx_budget b1 = asx_budget_from_polls(5);
    asx_status rc1 = asx_scheduler_run(rid, &b1);
    SCENARIO_CHECK(rc1 == ASX_E_POLL_BUDGET_EXHAUSTED, "first run: expected BUDGET_EXHAUSTED");

    uint32_t events1 = asx_scheduler_event_count();

    /* Replay same scenario — event count must match */
    asx_runtime_reset();
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_2");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_pending, NULL, &tid) == ASX_OK, "task_spawn_2");

    asx_budget b2 = asx_budget_from_polls(5);
    asx_status rc2 = asx_scheduler_run(rid, &b2);
    SCENARIO_CHECK(rc2 == ASX_E_POLL_BUDGET_EXHAUSTED, "replay: expected BUDGET_EXHAUSTED");

    uint32_t events2 = asx_scheduler_event_count();
    SCENARIO_CHECK(events1 == events2, "event counts must match across runs");

    SCENARIO_END();
}

/* robust-exhaustion-007: exhaustion does not corrupt partial state */
static void scenario_exhaustion_no_partial_mutation(void) {
    SCENARIO_BEGIN("robust-exhaustion-007.no_partial_mutation");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id t1;
    asx_budget budget;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &t1) == ASX_OK, "task_spawn");

    /* Complete t1 first */
    budget = asx_budget_from_polls(10);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(t1, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "t1 should complete");

    /* Now exhaust task arena — t1's outcome must remain stable */
    asx_task_id dummy;
    asx_status rc;
    for (;;) {
        rc = asx_task_spawn(rid, poll_pending, NULL, &dummy);
        if (rc != ASX_OK) break;
    }

    /* Verify t1 outcome wasn't corrupted */
    asx_outcome out;
    SCENARIO_CHECK(asx_task_get_outcome(t1, &out) == ASX_OK,
                   "outcome query after arena exhaustion");
    SCENARIO_CHECK(out.severity == ASX_OUTCOME_OK,
                   "outcome should still be OK after arena exhaust");

    SCENARIO_END();
}

/* robust-exhaustion-008: quiescence check on region with exhausted budget */
static void scenario_quiescence_budget_exhausted(void) {
    SCENARIO_BEGIN("robust-exhaustion-008.quiescence_budget");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_pending, NULL, &tid) == ASX_OK, "task_spawn");

    /* Quiescence should fail with active tasks */
    asx_status rc = asx_quiescence_check(rid);
    SCENARIO_CHECK(rc != ASX_OK, "quiescence should fail with active task");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    scenario_task_arena_exhaustion();
    scenario_region_arena_exhaustion();
    scenario_channel_capacity_exhaustion();
    scenario_timer_wheel_exhaustion();
    scenario_obligation_arena_exhaustion();
    scenario_budget_exhaustion_deterministic();
    scenario_exhaustion_no_partial_mutation();
    scenario_quiescence_budget_exhausted();

    fprintf(stderr, "[e2e] robustness_exhaustion: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
