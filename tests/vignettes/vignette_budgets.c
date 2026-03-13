/*
 * vignette_budgets.c — API ergonomics vignette: budget mechanics
 *
 * Exercises: budget construction, poll/cost consumption, exhaustion
 * detection, deadline checking, budget meet (componentwise tightening),
 * and scheduler interaction with budgets.
 *
 * bd-56t.5 — API ergonomics validation gate
 * SPDX-License-Identifier: MIT
 */
/* ASX_CHECKPOINT_WAIVER_FILE("vignette: no kernel loops") */

#include <asx/asx.h>
#include <stdio.h>

/* A long-running task that yields many times. */
typedef struct {
    asx_co_state co;
    int polls_seen;
} long_runner_state;

static asx_status poll_long_runner(void *ud, asx_task_id self) {
    long_runner_state *s = (long_runner_state *)ud;
    (void)self;

    ASX_CO_BEGIN(&s->co);
    while (s->polls_seen < 50) {
        s->polls_seen++;
        ASX_CO_YIELD(&s->co);
    }
    ASX_CO_END(&s->co);
}

/* -------------------------------------------------------------------
 * Scenario 1: Budget construction and query
 * ------------------------------------------------------------------- */
static int scenario_construction(void) {
    asx_budget b;

    printf("--- scenario: budget construction ---\n");

    /*
     * ERGO: asx_budget_from_polls(N) is the ideal quick constructor.
     * For simple use cases, users never need to touch the raw struct.
     * The naming convention "from_polls" is clear and self-documenting.
     */
    b = asx_budget_from_polls(10);
    if (asx_budget_polls(&b) != 10) {
        printf("  FAIL: expected 10 polls\n");
        return 1;
    }
    printf("  budget_from_polls(10): polls=%u\n", asx_budget_polls(&b));

    /*
     * ERGO: asx_budget_infinite() is the unconstrained identity —
     * useful as a "run forever" default. Naming is intuitive.
     */
    b = asx_budget_infinite();
    if (asx_budget_is_exhausted(&b)) {
        printf("  FAIL: infinite budget should not be exhausted\n");
        return 1;
    }
    printf("  budget_infinite: exhausted=%d\n", asx_budget_is_exhausted(&b));

    /*
     * ERGO: asx_budget_zero() is the absorbing element — maximally
     * constrained. Less commonly used but good for testing.
     */
    b = asx_budget_zero();
    if (!asx_budget_is_exhausted(&b)) {
        printf("  FAIL: zero budget should be exhausted\n");
        return 1;
    }
    printf("  budget_zero: exhausted=%d\n", asx_budget_is_exhausted(&b));

    printf("  PASS: budget construction\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 2: Poll consumption
 * ------------------------------------------------------------------- */
static int scenario_poll_consumption(void) {
    asx_budget b;
    uint32_t old_quota;

    printf("--- scenario: poll consumption ---\n");

    b = asx_budget_from_polls(3);

    /*
     * ERGO: asx_budget_consume_poll returns the old quota before
     * decrement. Returning 0 means "was already exhausted." This is
     * a C-idiomatic pattern (non-zero = success) but the semantics
     * need documentation — "returns old quota" is not obvious.
     *
     * SUGGESTION: A boolean return (1=success, 0=exhausted) would be
     * more ergonomic. The current return type serves power users who
     * want to know remaining quota, but most callers just check != 0.
     */
    old_quota = asx_budget_consume_poll(&b);
    printf("  consume_poll: old_quota=%u, remaining=%u\n", old_quota, asx_budget_polls(&b));

    old_quota = asx_budget_consume_poll(&b);
    old_quota = asx_budget_consume_poll(&b);
    (void)old_quota;

    /* Now exhausted. */
    if (!asx_budget_is_exhausted(&b)) {
        printf("  FAIL: should be exhausted after 3 consumes\n");
        return 1;
    }

    /* Consuming from exhausted budget returns 0. */
    old_quota = asx_budget_consume_poll(&b);
    if (old_quota != 0) {
        printf("  FAIL: consume on exhausted should return 0\n");
        return 1;
    }

    printf("  PASS: poll consumption\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 3: Cost consumption
 * ------------------------------------------------------------------- */
static int scenario_cost_consumption(void) {
    asx_budget b;
    int ok;

    printf("--- scenario: cost consumption ---\n");

    b = asx_budget_infinite();
    b.cost_quota = 100;

    /*
     * ERGO: asx_budget_consume_cost returns int (1=success, 0=fail).
     * Unlike consume_poll, this is a clean boolean. The inconsistency
     * between poll (returns old quota) and cost (returns bool) is a
     * minor ergonomic surprise.
     */
    ok = asx_budget_consume_cost(&b, 60);
    if (!ok) {
        printf("  FAIL: consume_cost(60) should succeed\n");
        return 1;
    }
    printf("  consume_cost(60): ok=%d, remaining=%lu\n", ok, (unsigned long)b.cost_quota);

    /* Over-consume should fail without mutation. */
    ok = asx_budget_consume_cost(&b, 100);
    if (ok) {
        printf("  FAIL: consume_cost(100) should fail (only 40 left)\n");
        return 1;
    }
    printf("  consume_cost(100): ok=%d (correctly rejected)\n", ok);

    printf("  PASS: cost consumption\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 4: Budget meet (componentwise tightening)
 * ------------------------------------------------------------------- */
static int scenario_meet(void) {
    asx_budget a, b, m;

    printf("--- scenario: budget meet ---\n");

    a = asx_budget_from_polls(100);
    a.cost_quota = 500;

    b = asx_budget_from_polls(50);
    b.cost_quota = 1000;

    /*
     * ERGO: asx_budget_meet takes const pointers — good, no mutation.
     * Returns a new budget by value. The "meet" name comes from lattice
     * theory; "tighten" or "min" might be more accessible to C users
     * unfamiliar with the term. But the docstring is clear.
     */
    m = asx_budget_meet(&a, &b);
    if (asx_budget_polls(&m) != 50) {
        printf("  FAIL: meet polls should be min(100,50)=50\n");
        return 1;
    }
    if (m.cost_quota != 500) {
        printf("  FAIL: meet cost should be min(500,1000)=500\n");
        return 1;
    }

    printf("  meet result: polls=%u cost=%lu\n", asx_budget_polls(&m), (unsigned long)m.cost_quota);
    printf("  PASS: budget meet\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 5: Scheduler budget exhaustion
 * ------------------------------------------------------------------- */
static int scenario_scheduler_exhaustion(void) {
    asx_status st;
    asx_region_id region;
    asx_task_id task;
    asx_budget budget;
    void *state_ptr = NULL;
    long_runner_state *ls;

    printf("--- scenario: scheduler budget exhaustion ---\n");

    asx_runtime_reset();

    st = asx_region_open(&region);
    if (st != ASX_OK) return 1;

    st = asx_task_spawn_captured(region, poll_long_runner, (uint32_t)sizeof(long_runner_state),
                                 NULL, &task, &state_ptr);
    if (st != ASX_OK) return 1;

    ls = (long_runner_state *)state_ptr;
    ls->co = (asx_co_state)ASX_CO_STATE_INIT;
    ls->polls_seen = 0;

    /* Give only 5 polls — task needs 50, so budget will exhaust. */
    budget = asx_budget_from_polls(5);
    st = asx_scheduler_run(region, &budget);

    /*
     * ERGO: ASX_E_POLL_BUDGET_EXHAUSTED is returned when the scheduler runs
     * out of polls before all tasks complete. The error is descriptive.
     * The user can check remaining task state and re-run with more budget.
     * This is a clean, explicit exhaustion contract — no silent partial
     * completion or undefined behavior.
     */
    if (st != ASX_E_POLL_BUDGET_EXHAUSTED) {
        printf("  FAIL: expected BUDGET_EXHAUSTED, got %s\n", asx_status_str(st));
        return 1;
    }
    printf("  scheduler correctly returned: %s\n", asx_status_str(st));
    printf("  polls consumed by task: %d\n", ls->polls_seen);

    /* Continue with enough budget to finish. */
    budget = asx_budget_from_polls(200);
    st = asx_region_drain(region, &budget);
    if (st != ASX_OK) {
        printf("  FAIL: drain returned %s\n", asx_status_str(st));
        return 1;
    }

    printf("  PASS: scheduler budget exhaustion\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */
int main(void) {
    int failures = 0;

    printf("=== vignette: budgets ===\n\n");

    failures += scenario_construction();
    failures += scenario_poll_consumption();
    failures += scenario_cost_consumption();
    failures += scenario_meet();
    failures += scenario_scheduler_exhaustion();

    printf("\n=== budgets: %d failures ===\n", failures);
    return failures;
}
