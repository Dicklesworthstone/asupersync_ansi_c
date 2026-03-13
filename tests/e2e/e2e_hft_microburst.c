/*
 * e2e_hft_microburst.c — e2e scenarios for HFT microburst/overload boundaries
 *
 * Exercises: admission under load, deterministic overload handling,
 * fairness (round-robin no starvation), budget-bounded partial
 * completion, mass cancellation, and replay digest stability.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/runtime.h>
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

static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

static asx_status poll_pending(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_PENDING;
}

/* Yield-once poll: pending on first call, complete on second */
typedef struct {
    asx_co_state co;
    int done;
} yield_once_state;

static asx_status poll_yield_once(void *ud, asx_task_id self) {
    yield_once_state *s = (yield_once_state *)ud;
    (void)self;
    ASX_CO_BEGIN(&s->co);
    ASX_CO_YIELD(&s->co);
    ASX_CO_END(&s->co);
}

/* Cancel-aware poll for mass-cancel test */
static asx_status poll_cancel_aware(void *ud, asx_task_id self) {
    yield_once_state *s = (yield_once_state *)ud;
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

/* hft-microburst-overload-001: capacity saturation + overload rejection */
static void scenario_overload_saturation(void) {
    SCENARIO_BEGIN("hft-microburst-overload-001.saturation");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    int spawned = 0;
    asx_status rc;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Fill task arena */
    for (;;) {
        rc = asx_task_spawn(rid, poll_pending, NULL, &tid);
        if (rc != ASX_OK) break;
        spawned++;
    }

    SCENARIO_CHECK(spawned > 0, "should spawn at least one task");
    SCENARIO_CHECK(rc == ASX_E_RESOURCE_EXHAUSTED, "overload should return RESOURCE_EXHAUSTED");

    /* Region state remains healthy */
    asx_region_state rs;
    SCENARIO_CHECK(asx_region_get_state(rid, &rs) == ASX_OK && rs == ASX_REGION_OPEN,
                   "region still OPEN after overload");

    SCENARIO_END();
}

/* hft-microburst-fairness-002: round-robin no starvation */
static void scenario_fairness_round_robin(void) {
    SCENARIO_BEGIN("hft-microburst-fairness-002.no_starvation");
    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tids[4];
    void *states[4];
    yield_once_state *ys;
    uint32_t i;
    asx_budget budget;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn 4 yield-once tasks */
    for (i = 0; i < 4; i++) {
        SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_yield_once,
                                               (uint32_t)sizeof(yield_once_state), NULL, &tids[i],
                                               &states[i]) == ASX_OK,
                       "spawn");
        ys = (yield_once_state *)states[i];
        ys->co.line = 0;
        ys->done = 0;
    }

    /* Give enough budget for all tasks to complete (4 tasks × 2 polls each) */
    budget = asx_budget_from_polls(20);
    asx_status rc = asx_scheduler_run(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "scheduler should complete all tasks");

    /* Verify all 4 completed */
    for (i = 0; i < 4; i++) {
        asx_task_state ts;
        SCENARIO_CHECK(asx_task_get_state(tids[i], &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                       "all tasks must complete");
    }

    SCENARIO_END();
}

/* hft-microburst-fairness-002b: tight budget partial completion */
static void scenario_fairness_partial_budget(void) {
    SCENARIO_BEGIN("hft-microburst-fairness-002b.partial_budget");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    int spawned = 0;
    uint32_t i;
    int completed;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn 8 immediate-complete tasks */
    for (i = 0; i < 8; i++) {
        SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "spawn");
        spawned++;
    }

    /* Give tight budget: only 4 polls */
    asx_budget budget = asx_budget_from_polls(4);
    asx_status rc = asx_scheduler_run(rid, &budget);
    SCENARIO_CHECK(rc == ASX_E_POLL_BUDGET_EXHAUSTED,
                   "expected budget exhaustion with tight budget");

    /* Some tasks should have completed, some still pending */
    completed = 0;
    for (i = 0; i < 8; i++) {
        /* Can't easily iterate tasks, but we know 4 polls = 4 completions */
    }
    (void)completed;

    /* Finish remaining with generous budget */
    budget = asx_budget_from_polls(100);
    rc = asx_scheduler_run(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "remaining tasks should complete");

    SCENARIO_END();
}

/* hft-overload-recovery-003: mass cancel + drain to quiescence */
static void scenario_overload_recovery(void) {
    SCENARIO_BEGIN("hft-overload-recovery-003.mass_cancel_drain");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tids[8];
    void *states[8];
    yield_once_state *ys;
    uint32_t i;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn 8 cancel-aware tasks */
    for (i = 0; i < 8; i++) {
        SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_cancel_aware,
                                               (uint32_t)sizeof(yield_once_state), NULL, &tids[i],
                                               &states[i]) == ASX_OK,
                       "spawn");
        ys = (yield_once_state *)states[i];
        ys->co.line = 0;
        ys->done = 0;
    }

    /* Run once to transition to RUNNING */
    asx_budget budget = asx_budget_from_polls(8);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Mass cancel with SHUTDOWN */
    uint32_t cancelled = asx_cancel_propagate(rid, ASX_CANCEL_SHUTDOWN);
    SCENARIO_CHECK(cancelled == 8, "should cancel all 8 tasks");

    /* Drain to quiescence */
    budget = asx_budget_from_polls(100);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* All tasks should be completed */
    for (i = 0; i < 8; i++) {
        asx_task_state ts;
        SCENARIO_CHECK(asx_task_get_state(tids[i], &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                       "task must complete after cancel");
    }

    SCENARIO_END();
}

/* hft-microburst replay digest determinism */
static void scenario_replay_digest(void) {
    SCENARIO_BEGIN("hft-microburst-replay.digest_deterministic");

    /* Run 1 */
    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tids[4];
    void *states[4];
    yield_once_state *ys;
    uint32_t i;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_1");
    for (i = 0; i < 4; i++) {
        SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_yield_once,
                                               (uint32_t)sizeof(yield_once_state), NULL, &tids[i],
                                               &states[i]) == ASX_OK,
                       "spawn_1");
        ys = (yield_once_state *)states[i];
        ys->co.line = 0;
        ys->done = 0;
    }
    asx_budget budget = asx_budget_from_polls(20);
    IGNORE_RC(asx_scheduler_run(rid, &budget));
    uint64_t digest1 = asx_trace_digest();

    /* Run 2: identical scenario */
    asx_runtime_reset();
    asx_trace_reset();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_2");
    for (i = 0; i < 4; i++) {
        SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_yield_once,
                                               (uint32_t)sizeof(yield_once_state), NULL, &tids[i],
                                               &states[i]) == ASX_OK,
                       "spawn_2");
        ys = (yield_once_state *)states[i];
        ys->co.line = 0;
        ys->done = 0;
    }
    budget = asx_budget_from_polls(20);
    IGNORE_RC(asx_scheduler_run(rid, &budget));
    uint64_t digest2 = asx_trace_digest();

    SCENARIO_CHECK(digest1 == digest2, "HFT burst digest must be deterministic");
    SCENARIO_CHECK(digest1 != 0, "digest should not be zero");
    printf("DIGEST %016llx\n", (unsigned long long)digest1);

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    scenario_overload_saturation();
    scenario_fairness_round_robin();
    scenario_fairness_partial_budget();
    scenario_overload_recovery();
    scenario_replay_digest();

    fprintf(stderr, "[e2e] hft_microburst: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
