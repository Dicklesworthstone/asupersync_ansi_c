/*
 * e2e_automotive_watchdog.c — e2e scenarios for automotive watchdog/deadline/degraded-mode
 *
 * Exercises: checkpoint cooperation under deadline, degraded-mode
 * transition with phase progression, deadline miss forced completion,
 * watchdog containment via region poison, and trace digest stability.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/cancel.h>
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

/* Checkpoint-cooperative poll: observes cancel and finalizes */
static asx_status poll_checkpoint_cooperative(void *ud, asx_task_id self) {
    asx_checkpoint_result cp;
    (void)ud;

    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
        IGNORE_RC(asx_task_finalize(self));
        return ASX_OK;
    }
    return ASX_E_PENDING;
}

/* Stubborn poll: never completes, never checks cancel */
static asx_status poll_stubborn(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_PENDING;
}

/* -------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------- */

/* auto-watchdog-checkpoint-001: cooperative cancel under deadline */
static void scenario_watchdog_checkpoint(void) {
    SCENARIO_BEGIN("auto-watchdog-checkpoint-001.cooperative");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_checkpoint_cooperative, NULL, &tid) == ASX_OK,
                   "task_spawn");

    /* Run once to transition to RUNNING */
    asx_budget budget = asx_budget_from_polls(1);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Cancel with DEADLINE */
    SCENARIO_CHECK(asx_task_cancel(tid, ASX_CANCEL_DEADLINE) == ASX_OK, "cancel_deadline");

    /* Run scheduler — task should cooperatively checkpoint and finalize */
    budget = asx_budget_from_polls(20);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "task should complete after cooperative cancel");

    asx_outcome out;
    SCENARIO_CHECK(asx_task_get_outcome(tid, &out) == ASX_OK, "get_outcome");

    SCENARIO_END();
}

/* auto-degraded-transition-002: phase progression under DEADLINE */
static void scenario_degraded_transition(void) {
    SCENARIO_BEGIN("auto-degraded-transition-002.phase_progression");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_checkpoint_cooperative, NULL, &tid) == ASX_OK,
                   "task_spawn");

    /* Run once to get to RUNNING */
    asx_budget budget = asx_budget_from_polls(1);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Cancel with DEADLINE severity */
    SCENARIO_CHECK(asx_task_cancel(tid, ASX_CANCEL_DEADLINE) == ASX_OK, "cancel_deadline");

    /* Task should be in CANCEL_REQUESTED */
    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_CANCEL_REQUESTED,
                   "task should be CANCEL_REQUESTED");

    /* Run scheduler — task checkpoints and transitions through phases */
    budget = asx_budget_from_polls(20);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "task should complete after phase progression");

    SCENARIO_END();
}

/* auto-deadline-miss-003: stubborn task force-completed by scheduler */
static void scenario_deadline_miss(void) {
    SCENARIO_BEGIN("auto-deadline-miss-003.forced_completion");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_stubborn, NULL, &tid) == ASX_OK, "task_spawn");

    /* Run once to get to RUNNING */
    asx_budget budget = asx_budget_from_polls(1);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Cancel with SHUTDOWN (50-poll cleanup budget — small enough for event log) */
    SCENARIO_CHECK(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN) == ASX_OK, "cancel_shutdown");

    /* Run scheduler with enough budget to exhaust cleanup (50+) */
    budget = asx_budget_from_polls(200);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Task should be force-completed */
    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "stubborn task should be force-completed");

    asx_outcome out;
    SCENARIO_CHECK(asx_task_get_outcome(tid, &out) == ASX_OK &&
                       out.severity == ASX_OUTCOME_CANCELLED,
                   "outcome should be CANCELLED");

    SCENARIO_END();
}

/* auto-watchdog containment: poisoned region isolates fault */
static void scenario_watchdog_containment(void) {
    SCENARIO_BEGIN("auto-watchdog-containment.poison_isolates");
    asx_runtime_reset();

    asx_region_id r1, r2;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&r1) == ASX_OK, "region_open_r1");
    SCENARIO_CHECK(asx_region_open(&r2) == ASX_OK, "region_open_r2");

    /* Poison r1 (watchdog containment) */
    SCENARIO_CHECK(asx_region_poison(r1) == ASX_OK, "poison_r1");

    /* r1 blocked, r2 unaffected */
    asx_status rc = asx_task_spawn(r1, poll_complete, NULL, &tid);
    SCENARIO_CHECK(rc == ASX_E_REGION_POISONED, "spawn blocked on poisoned");

    SCENARIO_CHECK(asx_task_spawn(r2, poll_complete, NULL, &tid) == ASX_OK, "spawn on healthy r2");

    /* Complete r2 task */
    asx_budget budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(r2, &budget) == ASX_OK, "drain r2");

    SCENARIO_END();
}

/* Deterministic trace digest for automotive scenarios */
static void scenario_trace_digest(void) {
    SCENARIO_BEGIN("auto-watchdog-trace.digest_deterministic");

    /* Run 1 */
    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_1");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "spawn_1");
    asx_budget budget = asx_budget_from_polls(10);
    IGNORE_RC(asx_scheduler_run(rid, &budget));
    uint64_t digest1 = asx_trace_digest();

    /* Run 2: replay */
    asx_runtime_reset();
    asx_trace_reset();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_2");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "spawn_2");
    budget = asx_budget_from_polls(10);
    IGNORE_RC(asx_scheduler_run(rid, &budget));
    uint64_t digest2 = asx_trace_digest();

    SCENARIO_CHECK(digest1 == digest2, "trace digest must match across runs");
    SCENARIO_CHECK(digest1 != 0, "digest should not be zero");
    printf("DIGEST %016llx\n", (unsigned long long)digest1);

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    scenario_watchdog_checkpoint();
    scenario_degraded_transition();
    scenario_deadline_miss();
    scenario_watchdog_containment();
    scenario_trace_digest();

    fprintf(stderr, "[e2e] automotive_watchdog: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
