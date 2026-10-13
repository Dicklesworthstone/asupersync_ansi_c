/*
 * ex_cancel_drain.c — Cancel-aware task drain with quiescence evidence
 *
 * Demonstrates the full cancellation lifecycle:
 *   1. Spawning a long-running task that cooperates with cancellation
 *   2. Region drain propagates cancel to all tasks
 *   3. Task observes cancel via asx_checkpoint() and finalizes
 *   4. Quiescence evidence decomposes the final state into Q1-Q4
 *   5. Drain progress snapshot shows task/obligation counters mid-drain
 *
 * This is the canonical example for understanding structured cancellation
 * and the bounded-cleanup guarantee.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>

#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

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

/* -------------------------------------------------------------------
 * Task: cancel-aware worker
 *
 * Polls indefinitely until it observes cancellation via checkpoint,
 * then finalizes and completes. This models a long-running operation
 * (e.g., network listener, data processor) that cooperates with
 * structured shutdown.
 * ------------------------------------------------------------------- */

static asx_status poll_cancel_aware(void *ud, asx_task_id self) {
    asx_checkpoint_result cp;
    (void)ud;

    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
        IGNORE_RC(asx_task_finalize(self));
        return ASX_OK;
    }
    return ASX_E_PENDING;
}

/* -------------------------------------------------------------------
 * Task: cancel-aware worker holding an obligation
 *
 * Reserves an obligation on its first poll, so the task is the
 * obligation's holder, then polls until it observes cancellation. Only
 * the holder's own poll can settle the obligation: the worker commits it
 * once its work is released, and a cancellation that arrives first
 * aborts it in cleanup. A task never completes with an obligation
 * still reserved.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_region_id region;
    asx_obligation_id oid;
    int reserved;
    int release; /* set by the host: the work is done, commit the obligation */
    int settled;
    asx_status reserve_status;
    asx_status settle_status;
} guarded_worker_state;

static asx_status poll_guarded_worker(void *ud, asx_task_id self) {
    guarded_worker_state *w = (guarded_worker_state *)ud;
    asx_checkpoint_result cp;

    if (!w->reserved) {
        w->reserve_status = asx_obligation_reserve(w->region, &w->oid);
        if (w->reserve_status != ASX_OK) return w->reserve_status;
        w->reserved = 1;
    }
    if (w->release && !w->settled) {
        w->settle_status = asx_obligation_commit(w->oid);
        w->settled = 1;
    }
    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
        if (!w->settled) {
            /* Cleanup: abort what is still held */
            w->settle_status = asx_obligation_abort(w->oid);
            w->settled = 1;
        }
        IGNORE_RC(asx_task_finalize(self));
        return ASX_OK;
    }
    return ASX_E_PENDING;
}

/* -------------------------------------------------------------------
 * Scenario: cancel_drain.cooperative_shutdown
 *
 * A cancel-aware task is drained via region close. The task observes
 * the cancel signal, finalizes, and the region reaches quiescence.
 * ------------------------------------------------------------------- */

static void scenario_cooperative_shutdown(void) {
    SCENARIO_BEGIN("cancel_drain.cooperative_shutdown");

    asx_runtime_reset();

    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn a long-running cancel-aware task */
    asx_task_id tid;
    SCENARIO_CHECK(asx_task_spawn(rid, poll_cancel_aware, NULL, &tid) == ASX_OK, "spawn");

    /* Prime the task to RUNNING */
    asx_budget budget = asx_budget_from_polls(1);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    asx_task_state state;
    SCENARIO_CHECK(asx_task_get_state(tid, &state) == ASX_OK && state == ASX_TASK_RUNNING,
                   "task_running");

    /* Drain the region — this propagates cancel and runs scheduler */
    budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "drain_ok");

    /* Task should be completed via cancel cooperation */
    SCENARIO_CHECK(asx_task_get_state(tid, &state) == ASX_OK && state == ASX_TASK_COMPLETED,
                   "task_completed");

    /* Full quiescence */
    SCENARIO_CHECK(asx_quiescence_check(rid) == ASX_OK, "quiescent");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: cancel_drain.quiescence_evidence
 *
 * Uses asx_quiescence_check_detailed() to decompose the final
 * quiescence state into Q1-Q4 conjuncts, proving each condition
 * individually.
 * ------------------------------------------------------------------- */

static void scenario_quiescence_evidence(void) {
    SCENARIO_BEGIN("cancel_drain.quiescence_evidence");

    asx_runtime_reset();

    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn the worker task; its first poll reserves the obligation */
    asx_task_id tid;
    guarded_worker_state worker;
    worker.region = rid;
    worker.oid = ASX_INVALID_ID;
    worker.reserved = 0;
    worker.release = 0;
    worker.settled = 0;
    worker.reserve_status = ASX_E_INVALID_STATE;
    worker.settle_status = ASX_E_INVALID_STATE;
    SCENARIO_CHECK(asx_task_spawn(rid, poll_guarded_worker, &worker, &tid) == ASX_OK, "spawn");

    asx_budget budget = asx_budget_from_polls(1);
    IGNORE_RC(asx_scheduler_run(rid, &budget));
    SCENARIO_CHECK(worker.reserve_status == ASX_OK, "reserve_obligation");

    /* Before drain: check detailed report shows not quiescent */
    asx_quiescence_report report;
    SCENARIO_CHECK(asx_quiescence_check_detailed(rid, &report) == ASX_OK, "report_before");
    SCENARIO_CHECK(report.quiescent == 0, "not_quiescent_before");

    /* Resolve obligation: the holder commits it in its own poll */
    worker.release = 1;
    budget = asx_budget_from_polls(1);
    IGNORE_RC(asx_scheduler_run(rid, &budget));
    SCENARIO_CHECK(worker.settle_status == ASX_OK, "commit");

    /* Drain */
    budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "drain_ok");

    /* After drain: all Q1-Q4 conditions should hold */
    SCENARIO_CHECK(asx_quiescence_check_detailed(rid, &report) == ASX_OK, "report_after");
    SCENARIO_CHECK(report.quiescent == 1, "quiescent");
    SCENARIO_CHECK(report.q1_tasks_complete == 1, "q1_tasks_done");
    SCENARIO_CHECK(report.q2_children_closed == 1, "q2_children_done");
    SCENARIO_CHECK(report.q3_obligations_resolved == 1, "q3_obligations_done");
    SCENARIO_CHECK(report.q4_cleanup_drained == 1, "q4_cleanup_done");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: cancel_drain.drain_progress_snapshot
 *
 * Uses asx_region_drain_progress() to observe task/obligation
 * counters at different points in the drain lifecycle.
 * ------------------------------------------------------------------- */

static void scenario_drain_progress(void) {
    SCENARIO_BEGIN("cancel_drain.drain_progress_snapshot");

    asx_runtime_reset();

    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn two tasks */
    asx_task_id t1, t2;
    SCENARIO_CHECK(asx_task_spawn(rid, poll_cancel_aware, NULL, &t1) == ASX_OK, "spawn_t1");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_cancel_aware, NULL, &t2) == ASX_OK, "spawn_t2");

    /* Snapshot before drain */
    asx_drain_progress prog;
    SCENARIO_CHECK(asx_region_drain_progress(rid, &prog) == ASX_OK, "progress_before");
    SCENARIO_CHECK(prog.tasks_live == 2, "two_tasks_live");
    SCENARIO_CHECK(prog.tasks_completed == 0, "zero_completed");
    SCENARIO_CHECK(prog.region_state == ASX_REGION_OPEN, "state_open");

    /* Drain */
    asx_budget budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "drain_ok");

    /* Snapshot after drain */
    SCENARIO_CHECK(asx_region_drain_progress(rid, &prog) == ASX_OK, "progress_after");
    SCENARIO_CHECK(prog.tasks_live == 0, "zero_tasks_live");
    SCENARIO_CHECK(prog.region_state == ASX_REGION_CLOSED, "state_closed");
    SCENARIO_CHECK(prog.cleanup_drained == 1, "cleanup_done");

    SCENARIO_END();
}

int main(void) {
    scenario_cooperative_shutdown();
    scenario_quiescence_evidence();
    scenario_drain_progress();

    printf("SCENARIO _summary %s total=%d pass=%d fail=%d\n", g_fail > 0 ? "fail" : "pass",
           g_pass + g_fail, g_pass, g_fail);

    return g_fail > 0 ? 1 : 0;
}
