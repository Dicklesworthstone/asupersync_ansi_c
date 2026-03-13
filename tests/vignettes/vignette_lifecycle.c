/*
 * vignette_lifecycle.c — API ergonomics vignette: region/task lifecycle
 *
 * Exercises: region open/close, task spawn, scheduler run, quiescence,
 * and region drain. This is the "hello world" of asx — the minimum
 * viable program a new user would write.
 *
 * Ergonomics observations are marked with ERGO: comments.
 *
 * bd-56t.5 — API ergonomics validation gate
 * SPDX-License-Identifier: MIT
 */
/* ASX_CHECKPOINT_WAIVER_FILE("vignette: no kernel loops") */

#include <asx/asx.h>
#include <stdio.h>

/* -------------------------------------------------------------------
 * ERGO: Poll functions are straightforward — the (void*, task_id)
 * signature is familiar to C developers used to callback patterns.
 * The ASX_E_PENDING return to signal "not done yet" is clear.
 * ------------------------------------------------------------------- */

/* A simple task that completes immediately. */
static asx_status poll_hello(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    printf("  task polled: completing immediately\n");
    return ASX_OK;
}

/* A task that takes 3 polls to complete using the coroutine macros. */
typedef struct {
    asx_co_state co;
    int step;
} multi_step_state;

static asx_status poll_multi_step(void *ud, asx_task_id self) {
    multi_step_state *s = (multi_step_state *)ud;
    (void)self;

    /*
     * ERGO: The ASX_CO_BEGIN/YIELD/END macros are clean and low-boilerplate.
     * The pattern of embedding asx_co_state in user structs works well.
     * Minor footgun: forgetting ASX_CO_STATE_INIT on the struct initializer
     * would cause undefined behavior, but the zero-init convention helps.
     */
    ASX_CO_BEGIN(&s->co);
    printf("  multi-step: step 1\n");
    s->step = 1;
    ASX_CO_YIELD(&s->co);

    printf("  multi-step: step 2\n");
    s->step = 2;
    ASX_CO_YIELD(&s->co);

    printf("  multi-step: step 3 (final)\n");
    s->step = 3;
    ASX_CO_END(&s->co);
}

/* -------------------------------------------------------------------
 * Scenario 1: Minimal lifecycle — open region, spawn task, drain
 * ------------------------------------------------------------------- */
static int scenario_minimal(void) {
    asx_status st;
    asx_region_id region;
    asx_task_id task;
    asx_budget budget;

    printf("--- scenario: minimal lifecycle ---\n");

    asx_runtime_reset();

    /*
     * ERGO: asx_region_open(&region) is clean — single out-param, returns
     * status. The must-use attribute catches ignored errors at compile time.
     */
    st = asx_region_open(&region);
    if (st != ASX_OK) {
        printf("  FAIL: region_open returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_task_spawn(region, poll_hello, NULL, &task);
    if (st != ASX_OK) {
        printf("  FAIL: task_spawn returned %s\n", asx_status_str(st));
        return 1;
    }

    /*
     * ERGO: Budget construction via asx_budget_from_polls(N) is ergonomic
     * for the common case. The user doesn't need to manually fill all
     * fields of the budget struct.
     */
    budget = asx_budget_from_polls(100);

    /*
     * ERGO: asx_region_drain(region, &budget) combines "run scheduler
     * then close" into one call — good for the simple case. The budget
     * parameter is a pointer, which is slightly surprising (could be
     * by-value for small structs), but consistent with the rest of the API.
     */
    st = asx_region_drain(region, &budget);
    if (st != ASX_OK) {
        printf("  FAIL: region_drain returned %s\n", asx_status_str(st));
        return 1;
    }

    printf("  PASS: minimal lifecycle\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 2: Multi-task with captured state
 * ------------------------------------------------------------------- */
static int scenario_captured_state(void) {
    asx_status st;
    asx_region_id region;
    asx_task_id t1, t2;
    asx_budget budget;
    void *state_ptr = NULL;
    multi_step_state *ms;

    printf("--- scenario: captured state ---\n");

    asx_runtime_reset();

    st = asx_region_open(&region);
    if (st != ASX_OK) return 1;

    /* Spawn a task with region-owned captured state. */
    /*
     * ERGO: asx_task_spawn_captured has 6 parameters — that's a lot.
     * The NULL dtor parameter is commonly unused. Consider whether a
     * simpler spawn_captured variant without dtor would reduce friction.
     * Also, the out_state is void** which requires a cast — unavoidable
     * in C but still a minor paper cut.
     */
    st = asx_task_spawn_captured(region, poll_multi_step, (uint32_t)sizeof(multi_step_state),
                                 NULL, /* no destructor */
                                 &t1, &state_ptr);
    if (st != ASX_OK) {
        printf("  FAIL: spawn_captured returned %s\n", asx_status_str(st));
        return 1;
    }

    /* Initialize the captured state. */
    /*
     * ERGO: The user must manually initialize captured state after spawn.
     * This is standard C, but it means the state pointer is returned
     * uninitialized. A "spawn_captured_zeroed" variant could help.
     */
    ms = (multi_step_state *)state_ptr;
    ms->co = (asx_co_state)ASX_CO_STATE_INIT;
    ms->step = 0;

    /* Spawn a second simple task. */
    st = asx_task_spawn(region, poll_hello, NULL, &t2);
    if (st != ASX_OK) return 1;

    /* Run scheduler with generous budget. */
    budget = asx_budget_from_polls(200);
    st = asx_region_drain(region, &budget);
    if (st != ASX_OK) {
        printf("  FAIL: drain returned %s\n", asx_status_str(st));
        return 1;
    }

    /* Verify task outcomes. */
    /*
     * ERGO: Querying task outcome requires a separate call and only works
     * after completion. The error message for querying non-completed tasks
     * (ASX_E_TASK_NOT_COMPLETED) is descriptive and helpful.
     */
    asx_outcome outcome;
    st = asx_task_get_outcome(t1, &outcome);
    if (st != ASX_OK) {
        printf("  FAIL: get_outcome returned %s\n", asx_status_str(st));
        return 1;
    }

    printf("  task outcome severity: %d (0=OK)\n", (int)outcome.severity);
    printf("  PASS: captured state\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 3: Quiescence check (manual path)
 * ------------------------------------------------------------------- */
static int scenario_quiescence_manual(void) {
    asx_status st;
    asx_region_id region;
    asx_task_id task;
    asx_budget budget;

    printf("--- scenario: quiescence (manual) ---\n");

    asx_runtime_reset();

    st = asx_region_open(&region);
    if (st != ASX_OK) return 1;

    st = asx_task_spawn(region, poll_hello, NULL, &task);
    if (st != ASX_OK) return 1;

    /*
     * ERGO: The manual path (scheduler_run + close + drain + quiescence_check)
     * is more verbose but makes the lifecycle phases explicit. This mirrors
     * what operators do in staged shutdowns.
     *
     * ERGO: asx_scheduler_run takes (region, &budget) — consistent with
     * drain(). The scheduler runs until all tasks complete or budget is
     * exhausted — good clean semantics.
     */
    budget = asx_budget_from_polls(100);
    st = asx_scheduler_run(region, &budget);
    if (st != ASX_OK) {
        printf("  FAIL: scheduler_run returned %s\n", asx_status_str(st));
        return 1;
    }

    /* Close the region. */
    st = asx_region_close(region);
    if (st != ASX_OK) {
        printf("  FAIL: region_close returned %s\n", asx_status_str(st));
        return 1;
    }

    /* Finalize close/drain work before quiescence check. */
    budget = asx_budget_from_polls(100);
    st = asx_region_drain(region, &budget);
    if (st != ASX_OK) {
        printf("  FAIL: region_drain returned %s\n", asx_status_str(st));
        return 1;
    }

    /* Check quiescence. */
    st = asx_quiescence_check(region);
    if (st != ASX_OK) {
        printf("  FAIL: quiescence_check returned %s\n", asx_status_str(st));
        return 1;
    }

    printf("  PASS: quiescence (manual)\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */
int main(void) {
    int failures = 0;

    printf("=== vignette: lifecycle ===\n\n");

    failures += scenario_minimal();
    failures += scenario_captured_state();
    failures += scenario_quiescence_manual();

    printf("\n=== lifecycle: %d failures ===\n", failures);
    return failures;
}
