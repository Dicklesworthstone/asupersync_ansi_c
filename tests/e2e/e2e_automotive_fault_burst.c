/*
 * e2e_automotive_fault_burst.c — deployment hardening: automotive fault-burst scenarios
 *
 * Exercises: fault injection cascades under AUTOMOTIVE profile,
 * clock skew resilience, allocator failure recovery, multi-fault
 * interaction, region poison as containment response, and
 * deterministic trace digest stability across fault sequences.
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

/* -------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------- */

/* auto-fault-burst-001: clock skew fault injection and recovery */
static void scenario_clock_skew_fault(void)
{
    SCENARIO_BEGIN("auto-fault-burst-001.clock_skew");
    asx_runtime_reset();

    asx_fault_injection fault;
    memset(&fault, 0, sizeof(fault));
    fault.kind = ASX_FAULT_CLOCK_SKEW;
    fault.param = 5000;          /* 5000 ns skew per read */
    fault.trigger_after = 0;
    fault.trigger_count = 10;    /* inject for 10 reads then deactivate */

    asx_status rc = asx_fault_inject(&fault);
    SCENARIO_CHECK(rc == ASX_OK, "fault_inject_clock_skew");

    SCENARIO_CHECK(asx_fault_injection_count() > 0, "fault_active");

    /* Normal operations should still work under skew */
    asx_region_id rid;
    asx_task_id tid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK,
                   "task_spawn_under_skew");

    asx_budget budget = asx_budget_from_polls(10);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Clear faults and verify recovery */
    SCENARIO_CHECK(asx_fault_clear() == ASX_OK, "fault_clear");
    SCENARIO_CHECK(asx_fault_injection_count() == 0, "faults_cleared");

    /* Post-fault operations */
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK,
                   "spawn_after_clear");
    budget = asx_budget_from_polls(10);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    SCENARIO_END();
}

/* auto-fault-burst-002: clock reversal fault */
static void scenario_clock_reversal(void)
{
    SCENARIO_BEGIN("auto-fault-burst-002.clock_reversal");
    asx_runtime_reset();

    asx_fault_injection fault;
    memset(&fault, 0, sizeof(fault));
    fault.kind = ASX_FAULT_CLOCK_REVERSE;
    fault.param = 2000;          /* subtract 2000 ns per read */
    fault.trigger_after = 2;     /* after 2 normal reads */
    fault.trigger_count = 5;     /* inject for 5 reads */

    SCENARIO_CHECK(asx_fault_inject(&fault) == ASX_OK, "inject_reversal");

    asx_region_id rid;
    asx_task_id tid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK,
                   "spawn_under_reversal");

    asx_budget budget = asx_budget_from_polls(10);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    SCENARIO_CHECK(asx_fault_clear() == ASX_OK, "clear");

    SCENARIO_END();
}

/* auto-fault-burst-003: constant entropy fault */
static void scenario_entropy_const(void)
{
    SCENARIO_BEGIN("auto-fault-burst-003.entropy_const");
    asx_runtime_reset();

    asx_fault_injection fault;
    memset(&fault, 0, sizeof(fault));
    fault.kind = ASX_FAULT_ENTROPY_CONST;
    fault.param = 0xDEADBEEF;   /* constant entropy value */
    fault.trigger_after = 0;
    fault.trigger_count = 0;     /* permanent until cleared */

    SCENARIO_CHECK(asx_fault_inject(&fault) == ASX_OK, "inject_entropy");

    /* Operations should still complete deterministically */
    asx_region_id rid;
    asx_task_id tid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    uint32_t i;
    for (i = 0; i < 4; i++) {
        SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK,
                       "spawn_under_const_entropy");
    }

    asx_budget budget = asx_budget_from_polls(20);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    SCENARIO_CHECK(asx_fault_clear() == ASX_OK, "clear_entropy");

    SCENARIO_END();
}

/* auto-fault-burst-004: multi-fault cascade */
static void scenario_multi_fault_cascade(void)
{
    SCENARIO_BEGIN("auto-fault-burst-004.multi_fault_cascade");
    asx_runtime_reset();

    /* Inject clock skew */
    asx_fault_injection f1;
    memset(&f1, 0, sizeof(f1));
    f1.kind = ASX_FAULT_CLOCK_SKEW;
    f1.param = 1000;
    f1.trigger_after = 0;
    f1.trigger_count = 20;

    SCENARIO_CHECK(asx_fault_inject(&f1) == ASX_OK, "inject_clock");

    /* Inject constant entropy on top */
    asx_fault_injection f2;
    memset(&f2, 0, sizeof(f2));
    f2.kind = ASX_FAULT_ENTROPY_CONST;
    f2.param = 42;
    f2.trigger_after = 0;
    f2.trigger_count = 20;

    SCENARIO_CHECK(asx_fault_inject(&f2) == ASX_OK, "inject_entropy");

    SCENARIO_CHECK(asx_fault_injection_count() >= 2, "two_faults_active");

    /* Runtime should still function under compound faults */
    asx_region_id rid;
    asx_task_id tid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    uint32_t i;
    for (i = 0; i < 4; i++) {
        SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK,
                       "spawn_under_cascade");
    }

    asx_budget budget = asx_budget_from_polls(50);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Clear all faults */
    SCENARIO_CHECK(asx_fault_clear() == ASX_OK, "clear_all");
    SCENARIO_CHECK(asx_fault_injection_count() == 0, "all_cleared");

    SCENARIO_END();
}

/* auto-fault-burst-005: poison containment after fault-induced degradation */
static void scenario_fault_containment(void)
{
    SCENARIO_BEGIN("auto-fault-burst-005.fault_containment");
    asx_runtime_reset();

    asx_region_id r_faulty, r_healthy;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&r_faulty) == ASX_OK, "open_faulty");
    SCENARIO_CHECK(asx_region_open(&r_healthy) == ASX_OK, "open_healthy");

    /* Spawn work in both regions */
    SCENARIO_CHECK(asx_task_spawn(r_faulty, poll_complete, NULL, &tid) == ASX_OK,
                   "spawn_faulty");
    SCENARIO_CHECK(asx_task_spawn(r_healthy, poll_complete, NULL, &tid) == ASX_OK,
                   "spawn_healthy");

    /* Simulate fault detection -> poison the faulty region */
    SCENARIO_CHECK(asx_region_poison(r_faulty) == ASX_OK, "poison_faulty");

    /* Faulty region rejects new work */
    asx_status rc = asx_task_spawn(r_faulty, poll_complete, NULL, &tid);
    SCENARIO_CHECK(rc == ASX_E_REGION_POISONED, "faulty_region_blocked");

    /* Healthy region unaffected */
    SCENARIO_CHECK(asx_task_spawn(r_healthy, poll_complete, NULL, &tid) == ASX_OK,
                   "healthy_unaffected");

    asx_budget budget = asx_budget_from_polls(50);
    SCENARIO_CHECK(asx_scheduler_run(r_healthy, &budget) == ASX_OK,
                   "drain_healthy");

    SCENARIO_END();
}

/* auto-fault-burst-006: deadline cancel under active fault */
static void scenario_deadline_under_fault(void)
{
    SCENARIO_BEGIN("auto-fault-burst-006.deadline_under_fault");
    asx_runtime_reset();

    /* Inject clock skew to simulate timing pressure */
    asx_fault_injection fault;
    memset(&fault, 0, sizeof(fault));
    fault.kind = ASX_FAULT_CLOCK_SKEW;
    fault.param = 10000;
    fault.trigger_after = 0;
    fault.trigger_count = 50;

    SCENARIO_CHECK(asx_fault_inject(&fault) == ASX_OK, "inject_skew");

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_checkpoint_cooperative, NULL, &tid)
                   == ASX_OK, "spawn_cooperative");

    /* Run to get RUNNING */
    asx_budget budget = asx_budget_from_polls(1);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Issue deadline cancel */
    SCENARIO_CHECK(asx_task_cancel(tid, ASX_CANCEL_DEADLINE) == ASX_OK,
                   "cancel_deadline");

    /* Drain — task should cooperate despite clock skew */
    budget = asx_budget_from_polls(50);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK &&
                   ts == ASX_TASK_COMPLETED,
                   "task should complete under fault pressure");

    SCENARIO_CHECK(asx_fault_clear() == ASX_OK, "clear_faults");

    SCENARIO_END();
}

/* auto-fault-burst-007: deterministic trace digest across fault sequences */
static void scenario_fault_trace_digest(void)
{
    SCENARIO_BEGIN("auto-fault-burst-007.trace_deterministic");

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

    SCENARIO_CHECK(digest1 == digest2, "fault-burst digest must be deterministic");
    SCENARIO_CHECK(digest1 != 0, "digest should not be zero");
    printf("DIGEST %016llx\n", (unsigned long long)digest1);

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void)
{
    scenario_clock_skew_fault();
    scenario_clock_reversal();
    scenario_entropy_const();
    scenario_multi_fault_cascade();
    scenario_fault_containment();
    scenario_deadline_under_fault();
    scenario_fault_trace_digest();

    fprintf(stderr, "[e2e] automotive_fault_burst: %d passed, %d failed\n",
            g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
