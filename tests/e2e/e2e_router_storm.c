/*
 * e2e_router_storm.c — deployment hardening: embedded router storm scenarios
 *
 * Exercises: rapid region churn under EMBEDDED_ROUTER profile, task
 * exhaustion and recovery at R1/R2 capacity, obligation lifecycle
 * under resource pressure, region poison isolation during storm,
 * and deterministic trace digest stability.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/telemetry.h>
#include <asx/runtime/trace.h>
#include <stdio.h>
#include <string.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define IGNORE_RC(expr) \
    do { asx_status ignore_rc_ = (expr); (void)ignore_rc_; } while (0)

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id) \
    do { const char *_scenario_id = (id); int _scenario_ok = 1; (void)0

#define SCENARIO_CHECK(cond, msg)                         \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("SCENARIO %s fail %s\n",               \
                   _scenario_id, (msg));                  \
            _scenario_ok = 0;                             \
            g_fail++;                                     \
            goto _scenario_end;                           \
        }                                                 \
    } while (0)

#define SCENARIO_END()                                    \
    _scenario_end:                                        \
    if (_scenario_ok) {                                   \
        printf("SCENARIO %s pass\n", _scenario_id);      \
        g_pass++;                                         \
    }                                                     \
    } while (0)

static asx_status poll_complete(void *ud, asx_task_id self)
{
    (void)ud; (void)self;
    return ASX_OK;
}

/* Checkpoint-cooperative poll */
static asx_status poll_checkpoint_cooperative(void *ud, asx_task_id self)
{
    asx_checkpoint_result cp;
    (void)ud;

    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
        IGNORE_RC(asx_task_finalize(self));
        return ASX_OK;
    }
    return ASX_E_PENDING;
}

/* Yield-once poll */
typedef struct {
    asx_co_state co;
} yield_state;

static asx_status poll_yield_once(void *ud, asx_task_id self)
{
    yield_state *s = (yield_state *)ud;
    (void)self;
    ASX_CO_BEGIN(&s->co);
    ASX_CO_YIELD(&s->co);
    ASX_CO_END(&s->co);
}

/* -------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------- */

/* router-storm-001: rapid region open/close churn */
static void scenario_region_churn(void)
{
    SCENARIO_BEGIN("router-storm-001.region_churn");
    asx_runtime_reset();

    uint32_t cycles = 0;
    uint32_t i;

    /* Open and close regions in rapid succession */
    for (i = 0; i < 50; i++) {
        asx_region_id rid;
        asx_status rc = asx_region_open(&rid);
        if (rc == ASX_E_RESOURCE_EXHAUSTED) break;
        SCENARIO_CHECK(rc == ASX_OK, "region_open");

        /* Spawn and drain a task each cycle */
        asx_task_id tid;
        SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK,
                       "task_spawn");

        asx_budget budget = asx_budget_from_polls(5);
        IGNORE_RC(asx_scheduler_run(rid, &budget));

        SCENARIO_CHECK(asx_region_close(rid) == ASX_OK, "region_close");
        cycles++;
    }

    SCENARIO_CHECK(cycles >= 8, "should complete at least 8 churn cycles");

    SCENARIO_END();
}

/* router-storm-002: multi-region concurrent task saturation */
static void scenario_multi_region_saturation(void)
{
    SCENARIO_BEGIN("router-storm-002.multi_region_saturation");
    asx_runtime_reset();

    asx_region_id regions[4];
    uint32_t i, j;
    uint32_t region_count = 0;

    /* Open multiple regions */
    for (i = 0; i < 4; i++) {
        asx_status rc = asx_region_open(&regions[i]);
        if (rc != ASX_OK) break;
        region_count++;
    }
    SCENARIO_CHECK(region_count >= 2, "need at least 2 regions");

    /* Saturate each region with tasks */
    for (i = 0; i < region_count; i++) {
        for (j = 0; j < 8; j++) {
            asx_task_id tid;
            asx_status rc = asx_task_spawn(regions[i], poll_complete, NULL, &tid);
            if (rc == ASX_E_RESOURCE_EXHAUSTED) break;
            SCENARIO_CHECK(rc == ASX_OK, "task_spawn");
        }
    }

    /* Drain all regions */
    for (i = 0; i < region_count; i++) {
        asx_budget budget = asx_budget_from_polls(100);
        IGNORE_RC(asx_scheduler_run(regions[i], &budget));
    }

    SCENARIO_END();
}

/* router-storm-003: exhaustion handling and state consistency */
static void scenario_exhaustion_handling(void)
{
    SCENARIO_BEGIN("router-storm-003.exhaustion_handling");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    int spawned = 0;
    asx_status rc;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Fill task arena to exhaustion */
    for (;;) {
        rc = asx_task_spawn(rid, poll_complete, NULL, &tid);
        if (rc != ASX_OK) break;
        spawned++;
    }
    SCENARIO_CHECK(spawned > 0, "should spawn at least one task");
    SCENARIO_CHECK(rc == ASX_E_RESOURCE_EXHAUSTED, "expected exhaustion");

    /* Region must remain healthy despite overload */
    asx_region_state rs;
    SCENARIO_CHECK(asx_region_get_state(rid, &rs) == ASX_OK &&
                   rs == ASX_REGION_OPEN,
                   "region still OPEN after exhaustion");

    /* Drain all tasks to quiescence */
    asx_budget budget = asx_budget_from_polls(500);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Region still healthy after drain */
    SCENARIO_CHECK(asx_region_get_state(rid, &rs) == ASX_OK &&
                   rs == ASX_REGION_OPEN,
                   "region healthy after drain");

    /* Clean close succeeds */
    SCENARIO_CHECK(asx_region_close(rid) == ASX_OK, "region_close");

    SCENARIO_END();
}

/* router-storm-004: obligation churn under resource pressure */
static void scenario_obligation_churn(void)
{
    SCENARIO_BEGIN("router-storm-004.obligation_churn");
    asx_runtime_reset();

    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Reserve and commit obligations in rapid cycles */
    uint32_t committed = 0;
    uint32_t i;
    for (i = 0; i < 20; i++) {
        asx_obligation_id oid;
        asx_status rc = asx_obligation_reserve(rid, &oid);
        if (rc == ASX_E_RESOURCE_EXHAUSTED) break;
        SCENARIO_CHECK(rc == ASX_OK, "obligation_reserve");

        if (i % 3 == 0) {
            SCENARIO_CHECK(asx_obligation_abort(oid) == ASX_OK,
                           "obligation_abort");
        } else {
            SCENARIO_CHECK(asx_obligation_commit(oid) == ASX_OK,
                           "obligation_commit");
            committed++;
        }
    }
    SCENARIO_CHECK(committed > 0, "should commit at least one obligation");

    SCENARIO_END();
}

/* router-storm-005: poison isolation during storm */
static void scenario_poison_during_storm(void)
{
    SCENARIO_BEGIN("router-storm-005.poison_isolation");
    asx_runtime_reset();

    asx_region_id r_healthy, r_faulted;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&r_healthy) == ASX_OK, "open_healthy");
    SCENARIO_CHECK(asx_region_open(&r_faulted) == ASX_OK, "open_faulted");

    /* Spawn tasks in both */
    SCENARIO_CHECK(asx_task_spawn(r_healthy, poll_complete, NULL, &tid) == ASX_OK,
                   "spawn_healthy");
    SCENARIO_CHECK(asx_task_spawn(r_faulted, poll_checkpoint_cooperative, NULL, &tid)
                   == ASX_OK, "spawn_faulted");

    /* Poison the faulted region */
    SCENARIO_CHECK(asx_region_poison(r_faulted) == ASX_OK, "poison");

    /* Faulted region rejects new work */
    asx_status rc = asx_task_spawn(r_faulted, poll_complete, NULL, &tid);
    SCENARIO_CHECK(rc == ASX_E_REGION_POISONED, "spawn_blocked_on_poison");

    /* Healthy region still operational */
    SCENARIO_CHECK(asx_task_spawn(r_healthy, poll_complete, NULL, &tid) == ASX_OK,
                   "spawn_on_healthy_still_works");

    asx_budget budget = asx_budget_from_polls(50);
    SCENARIO_CHECK(asx_scheduler_run(r_healthy, &budget) == ASX_OK,
                   "drain_healthy");

    SCENARIO_END();
}

/* router-storm-006: cancel propagation under load */
static void scenario_cancel_under_load(void)
{
    SCENARIO_BEGIN("router-storm-006.cancel_under_load");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tids[8];
    void *states[8];
    yield_state *ys;
    uint32_t i;
    uint32_t spawned = 0;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn cancel-aware tasks */
    for (i = 0; i < 8; i++) {
        asx_status rc = asx_task_spawn_captured(rid, poll_yield_once,
                       (uint32_t)sizeof(yield_state), NULL,
                       &tids[i], &states[i]);
        if (rc != ASX_OK) break;
        ys = (yield_state *)states[i];
        ys->co.line = 0;
        spawned++;
    }
    SCENARIO_CHECK(spawned >= 4, "need at least 4 tasks");

    /* Run once to transition tasks */
    asx_budget budget = asx_budget_from_polls(spawned);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Cancel all via region-level propagation */
    uint32_t cancelled = asx_cancel_propagate(rid, ASX_CANCEL_SHUTDOWN);
    SCENARIO_CHECK(cancelled > 0, "should cancel tasks");

    /* Drain to quiescence */
    budget = asx_budget_from_polls(200);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    SCENARIO_END();
}

/* router-storm-007: deterministic trace digest */
static void scenario_trace_digest(void)
{
    SCENARIO_BEGIN("router-storm-007.trace_deterministic");

    /* Run 1 */
    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_1");
    for (i = 0; i < 4; i++) {
        SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK,
                       "spawn_1");
    }
    asx_budget budget = asx_budget_from_polls(20);
    IGNORE_RC(asx_scheduler_run(rid, &budget));
    uint64_t digest1 = asx_trace_digest();

    /* Run 2: identical scenario */
    asx_runtime_reset();
    asx_trace_reset();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_2");
    for (i = 0; i < 4; i++) {
        SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK,
                       "spawn_2");
    }
    budget = asx_budget_from_polls(20);
    IGNORE_RC(asx_scheduler_run(rid, &budget));
    uint64_t digest2 = asx_trace_digest();

    SCENARIO_CHECK(digest1 == digest2, "router storm digest must be deterministic");
    SCENARIO_CHECK(digest1 != 0, "digest should not be zero");
    printf("DIGEST %016llx\n", (unsigned long long)digest1);

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void)
{
    scenario_region_churn();
    scenario_multi_region_saturation();
    scenario_exhaustion_handling();
    scenario_obligation_churn();
    scenario_poison_during_storm();
    scenario_cancel_under_load();
    scenario_trace_digest();

    fprintf(stderr, "[e2e] router_storm: %d passed, %d failed\n",
            g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
