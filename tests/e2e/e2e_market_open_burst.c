/*
 * e2e_market_open_burst.c — deployment hardening: HFT market-open burst scenarios
 *
 * Exercises: extreme admission spike under HFT profile, overload
 * rejection under capacity pressure, priority-fair draining,
 * burst recovery to stable state, obligation linearity under load,
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

/* Yield-once poll */
typedef struct {
    asx_co_state co;
} yield_state;

/* Cancel-aware poll for drain tests */
static asx_status poll_cancel_aware(void *ud, asx_task_id self)
{
    yield_state *s = (yield_state *)ud;
    asx_checkpoint_result cp;
    ASX_CO_BEGIN(&s->co);
    for (;;) {
        if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
            IGNORE_RC(asx_task_finalize(self));
            return ASX_OK;
        }
        ASX_CO_YIELD(&s->co);
    }
    ASX_CO_END(&s->co);
}

/* -------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------- */

/* market-open-burst-001: extreme admission spike */
static void scenario_admission_spike(void)
{
    SCENARIO_BEGIN("market-open-burst-001.admission_spike");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    int admitted = 0;
    int rejected = 0;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Fire rapid task spawns until capacity */
    for (;;) {
        asx_status rc = asx_task_spawn(rid, poll_complete, NULL, &tid);
        if (rc == ASX_E_RESOURCE_EXHAUSTED) {
            rejected++;
            break;
        }
        SCENARIO_CHECK(rc == ASX_OK, "task_spawn");
        admitted++;
    }

    SCENARIO_CHECK(admitted > 0, "must admit at least one task");
    SCENARIO_CHECK(rejected > 0, "must hit capacity limit");

    /* All admitted tasks should drain cleanly */
    asx_budget budget = asx_budget_from_polls(500);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    SCENARIO_END();
}

/* market-open-burst-002: burst with interleaved obligations */
static void scenario_burst_obligations(void)
{
    SCENARIO_BEGIN("market-open-burst-002.burst_obligations");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn tasks and reserve obligations in a tight burst */
    for (i = 0; i < 8; i++) {
        asx_status rc = asx_task_spawn(rid, poll_complete, NULL, &tid);
        if (rc != ASX_OK) break;

        asx_obligation_id oid;
        rc = asx_obligation_reserve(rid, &oid);
        if (rc == ASX_OK) {
            /* Commit every other, abort the rest — tests linearity */
            if (i % 2 == 0) {
                SCENARIO_CHECK(asx_obligation_commit(oid) == ASX_OK,
                               "obligation_commit");
            } else {
                SCENARIO_CHECK(asx_obligation_abort(oid) == ASX_OK,
                               "obligation_abort");
            }
        }
    }

    /* Drain all tasks */
    asx_budget budget = asx_budget_from_polls(200);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    SCENARIO_END();
}

/* market-open-burst-003: partial budget drain under spike */
static void scenario_partial_drain(void)
{
    SCENARIO_BEGIN("market-open-burst-003.partial_drain");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    uint32_t spawned = 0;
    uint32_t i;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn many tasks */
    for (i = 0; i < 16; i++) {
        asx_status rc = asx_task_spawn(rid, poll_complete, NULL, &tid);
        if (rc != ASX_OK) break;
        spawned++;
    }
    SCENARIO_CHECK(spawned >= 4, "need at least 4 tasks for burst");

    /* Tight budget: fewer polls than tasks */
    asx_budget budget = asx_budget_from_polls(2);
    asx_status rc = asx_scheduler_run(rid, &budget);
    SCENARIO_CHECK(rc == ASX_E_POLL_BUDGET_EXHAUSTED,
                   "expected budget exhaustion");

    /* Second pass drains remaining */
    budget = asx_budget_from_polls(500);
    rc = asx_scheduler_run(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "full drain should succeed");

    SCENARIO_END();
}

/* market-open-burst-004: burst recovery and graceful degradation */
static void scenario_burst_recovery(void)
{
    SCENARIO_BEGIN("market-open-burst-004.burst_recovery");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Phase 1: saturate */
    int saturated = 0;
    for (;;) {
        asx_status rc = asx_task_spawn(rid, poll_complete, NULL, &tid);
        if (rc != ASX_OK) break;
        saturated++;
    }
    SCENARIO_CHECK(saturated > 0, "must saturate");

    /* Region stays healthy despite overload */
    asx_region_state rs;
    SCENARIO_CHECK(asx_region_get_state(rid, &rs) == ASX_OK &&
                   rs == ASX_REGION_OPEN, "region healthy during overload");

    /* Phase 2: drain to quiescence */
    asx_budget budget = asx_budget_from_polls(500);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Phase 3: region still clean after burst */
    SCENARIO_CHECK(asx_region_get_state(rid, &rs) == ASX_OK &&
                   rs == ASX_REGION_OPEN, "region healthy after burst drain");

    /* Clean close verifies no dangling state */
    SCENARIO_CHECK(asx_region_close(rid) == ASX_OK, "clean_close");

    SCENARIO_END();
}

/* market-open-burst-005: mass cancel during burst */
static void scenario_mass_cancel_burst(void)
{
    SCENARIO_BEGIN("market-open-burst-005.mass_cancel_burst");
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
        asx_status rc = asx_task_spawn_captured(rid, poll_cancel_aware,
                       (uint32_t)sizeof(yield_state), NULL,
                       &tids[i], &states[i]);
        if (rc != ASX_OK) break;
        ys = (yield_state *)states[i];
        ys->co.line = 0;
        spawned++;
    }
    SCENARIO_CHECK(spawned >= 4, "need at least 4 cancel-aware tasks");

    /* Run once to get tasks RUNNING */
    asx_budget budget = asx_budget_from_polls(spawned);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Mass cancel */
    uint32_t cancelled = asx_cancel_propagate(rid, ASX_CANCEL_SHUTDOWN);
    SCENARIO_CHECK(cancelled == spawned,
                   "all tasks should receive cancel");

    /* Drain */
    budget = asx_budget_from_polls(200);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* All should be completed */
    for (i = 0; i < spawned; i++) {
        asx_task_state ts;
        SCENARIO_CHECK(asx_task_get_state(tids[i], &ts) == ASX_OK &&
                       ts == ASX_TASK_COMPLETED, "task must complete post-cancel");
    }

    SCENARIO_END();
}

/* market-open-burst-006: multi-region burst isolation */
static void scenario_multi_region_burst(void)
{
    SCENARIO_BEGIN("market-open-burst-006.multi_region_isolation");
    asx_runtime_reset();

    asx_region_id r1, r2;
    asx_task_id tid;
    uint32_t i;

    SCENARIO_CHECK(asx_region_open(&r1) == ASX_OK, "open_r1");
    SCENARIO_CHECK(asx_region_open(&r2) == ASX_OK, "open_r2");

    /* Saturate r1 */
    for (i = 0; i < 16; i++) {
        asx_status rc = asx_task_spawn(r1, poll_complete, NULL, &tid);
        if (rc != ASX_OK) break;
    }

    /* r2 should still accept work independently */
    SCENARIO_CHECK(asx_task_spawn(r2, poll_complete, NULL, &tid) == ASX_OK,
                   "r2 accepts work despite r1 pressure");

    /* Drain both */
    asx_budget budget = asx_budget_from_polls(200);
    IGNORE_RC(asx_scheduler_run(r1, &budget));
    IGNORE_RC(asx_scheduler_run(r2, &budget));

    SCENARIO_END();
}

/* market-open-burst-007: deterministic trace digest */
static void scenario_trace_digest(void)
{
    SCENARIO_BEGIN("market-open-burst-007.trace_deterministic");

    /* Run 1 */
    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_1");
    for (i = 0; i < 6; i++) {
        SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK,
                       "spawn_1");
    }
    asx_budget budget = asx_budget_from_polls(30);
    IGNORE_RC(asx_scheduler_run(rid, &budget));
    uint64_t digest1 = asx_trace_digest();

    /* Run 2: identical scenario */
    asx_runtime_reset();
    asx_trace_reset();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_2");
    for (i = 0; i < 6; i++) {
        SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK,
                       "spawn_2");
    }
    budget = asx_budget_from_polls(30);
    IGNORE_RC(asx_scheduler_run(rid, &budget));
    uint64_t digest2 = asx_trace_digest();

    SCENARIO_CHECK(digest1 == digest2, "market burst digest must be deterministic");
    SCENARIO_CHECK(digest1 != 0, "digest should not be zero");
    printf("DIGEST %016llx\n", (unsigned long long)digest1);

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void)
{
    scenario_admission_spike();
    scenario_burst_obligations();
    scenario_partial_drain();
    scenario_burst_recovery();
    scenario_mass_cancel_burst();
    scenario_multi_region_burst();
    scenario_trace_digest();

    fprintf(stderr, "[e2e] market_open_burst: %d passed, %d failed\n",
            g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
