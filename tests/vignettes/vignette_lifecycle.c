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

static asx_status poll_count_pending(void *ud, asx_task_id self) {
    uint32_t *count = (uint32_t *)ud;
    (void)self;
    (*count)++;
    return ASX_E_PENDING;
}

static asx_status poll_fail_twice_then_ok(void *ud, asx_task_id self) {
    int *remaining_failures = (int *)ud;
    (void)self;
    if (*remaining_failures > 0) {
        (*remaining_failures)--;
        return ASX_E_INVALID_STATE;
    }
    return ASX_OK;
}

static asx_status poll_pending_then_ok_vignette(void *ud, asx_task_id self) {
    int *remaining_pending = (int *)ud;
    (void)self;
    if (*remaining_pending > 0) {
        (*remaining_pending)--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
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
 * Scenario 4: capability-wrapped scope authority
 * ------------------------------------------------------------------- */
static int scenario_capability_wrapped_scope(void) {
    asx_status st;
    asx_region_id region;
    asx_budget scope_budget;
    asx_cx root_cx;
    asx_cx scope_cx;
    asx_cx child_cx;
    asx_cx_wrapper wrapper;
    asx_scope scope;
    asx_task_handle handle;

    printf("--- scenario: capability-wrapped scope ---\n");

    asx_runtime_reset();

    st = asx_region_open(&region);
    if (st != ASX_OK) {
        printf("  FAIL: region_open returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_cx_init(&root_cx, region, ASX_INVALID_ID,
                     ASX_CAP_SPAWN | ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY);
    if (st != ASX_OK) {
        printf("  FAIL: asx_cx_init returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_cx_wrap(&root_cx, ASX_CAP_SPAWN | ASX_CAP_CLOCK_READ, &wrapper);
    if (st != ASX_OK) {
        printf("  FAIL: asx_cx_wrap returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_cx_unwrap(&root_cx, &wrapper, &scope_cx);
    if (st != ASX_OK) {
        printf("  FAIL: asx_cx_unwrap returned %s\n", asx_status_str(st));
        return 1;
    }

    scope_budget = asx_budget_from_polls(64);
    st = asx_scope_init(&scope, region, &scope_cx, scope_budget);
    if (st != ASX_OK) {
        printf("  FAIL: asx_scope_init returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_scope_spawn_with_cx(&scope, ASX_CAP_CLOCK_READ, poll_hello, NULL, &handle, &child_cx);
    if (st != ASX_OK) {
        printf("  FAIL: asx_scope_spawn_with_cx returned %s\n", asx_status_str(st));
        return 1;
    }

    printf("  child caps: clock=%d entropy=%d spawn=%d\n",
           asx_cx_has_cap(&child_cx, ASX_CAP_CLOCK_READ),
           asx_cx_has_cap(&child_cx, ASX_CAP_ENTROPY),
           asx_cx_has_cap(&child_cx, ASX_CAP_SPAWN));

    if (!asx_cx_has_cap(&child_cx, ASX_CAP_CLOCK_READ) ||
        asx_cx_has_cap(&child_cx, ASX_CAP_ENTROPY) ||
        asx_cx_has_cap(&child_cx, ASX_CAP_SPAWN)) {
        printf("  FAIL: child authority was not narrowed correctly\n");
        return 1;
    }

    st = asx_scope_spawn_with_cx(&scope, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY, poll_hello, NULL,
                                 &handle, NULL);
    if (st != ASX_E_INVALID_ARGUMENT) {
        printf("  FAIL: widened child caps should fail closed, got %s\n", asx_status_str(st));
        return 1;
    }
    printf("  widened child caps correctly rejected: %s\n", asx_status_str(st));

    st = asx_scope_run(&scope);
    if (st != ASX_OK) {
        printf("  FAIL: asx_scope_run returned %s\n", asx_status_str(st));
        return 1;
    }

    printf("  PASS: capability-wrapped scope\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 5: combinator orchestration
 * ------------------------------------------------------------------- */
static int scenario_combinator_orchestration(void) {
    asx_status st;
    asx_retry_state retry;
    asx_pipeline_state pipeline;
    asx_join_state join;
    asx_select_state select;
    int retry_failures = 2;
    int pipeline_pending = 1;
    uint32_t loser_polls = 0;

    printf("--- scenario: combinator orchestration ---\n");

    asx_runtime_reset();

    st = asx_retry_init(&retry, poll_fail_twice_then_ok, &retry_failures, 5);
    if (st != ASX_OK) {
        printf("  FAIL: asx_retry_init returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_pipeline_init(&pipeline);
    if (st != ASX_OK) {
        printf("  FAIL: asx_pipeline_init returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_pipeline_add_stage(&pipeline, poll_pending_then_ok_vignette, &pipeline_pending);
    if (st != ASX_OK) {
        printf("  FAIL: pipeline_add_stage(stage1) returned %s\n", asx_status_str(st));
        return 1;
    }
    st = asx_pipeline_add_stage(&pipeline, poll_hello, NULL);
    if (st != ASX_OK) {
        printf("  FAIL: pipeline_add_stage(stage2) returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_join_init(&join);
    if (st != ASX_OK) {
        printf("  FAIL: asx_join_init returned %s\n", asx_status_str(st));
        return 1;
    }
    st = asx_join_add(&join, asx_retry_poll, &retry);
    if (st != ASX_OK) {
        printf("  FAIL: asx_join_add(retry) returned %s\n", asx_status_str(st));
        return 1;
    }
    st = asx_join_add(&join, asx_pipeline_poll, &pipeline);
    if (st != ASX_OK) {
        printf("  FAIL: asx_join_add(pipeline) returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_join_poll(&join, 0);
    if (st != ASX_E_PENDING) {
        printf("  FAIL: first join_poll returned %s\n", asx_status_str(st));
        return 1;
    }
    st = asx_join_poll(&join, 0);
    if (st != ASX_E_PENDING) {
        printf("  FAIL: second join_poll returned %s\n", asx_status_str(st));
        return 1;
    }
    st = asx_join_poll(&join, 0);
    if (st != ASX_OK) {
        printf("  FAIL: final join_poll returned %s\n", asx_status_str(st));
        return 1;
    }

    printf("  join: retry_attempts=%u pipeline_stages=%u outcome=%d\n",
           asx_retry_attempts(&retry),
           asx_pipeline_completed_stages(&pipeline),
           (int)asx_join_outcome(&join).severity);

    st = asx_select_init(&select);
    if (st != ASX_OK) {
        printf("  FAIL: asx_select_init returned %s\n", asx_status_str(st));
        return 1;
    }
    st = asx_select_add(&select, poll_hello, NULL);
    if (st != ASX_OK) {
        printf("  FAIL: asx_select_add(winner) returned %s\n", asx_status_str(st));
        return 1;
    }
    st = asx_select_add(&select, poll_count_pending, &loser_polls);
    if (st != ASX_OK) {
        printf("  FAIL: asx_select_add(loser) returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_select_poll(&select, 0);
    if (st != ASX_OK) {
        printf("  FAIL: asx_select_poll returned %s\n", asx_status_str(st));
        return 1;
    }

    printf("  select: winner=%d loser_polls=%u drained=%u\n", asx_select_winner(&select),
           loser_polls, select.drained);

    if (asx_retry_attempts(&retry) != 3u || asx_pipeline_completed_stages(&pipeline) != 2u ||
        asx_join_outcome(&join).severity != ASX_OUTCOME_OK || asx_select_winner(&select) != 0 ||
        loser_polls != 1u || select.drained != 1u) {
        printf("  FAIL: combinator orchestration produced unexpected state\n");
        return 1;
    }

    printf("  PASS: combinator orchestration\n");
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
    failures += scenario_capability_wrapped_scope();
    failures += scenario_combinator_orchestration();

    printf("\n=== lifecycle: %d failures ===\n", failures);
    return failures;
}
