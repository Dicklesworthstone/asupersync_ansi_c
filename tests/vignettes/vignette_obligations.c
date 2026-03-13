/*
 * vignette_obligations.c — API ergonomics vignette: obligation protocol
 *
 * Exercises: obligation reserve/commit/abort, linearity enforcement,
 * quiescence blocking on unresolved obligations, and obligation state
 * queries. Obligations are the "must-complete" contract primitive.
 *
 * bd-56t.5 — API ergonomics validation gate
 * SPDX-License-Identifier: MIT
 */
/* ASX_CHECKPOINT_WAIVER_FILE("vignette: no kernel loops") */

#include <asx/asx.h>
#include <stdio.h>

/*
 * ERGO: The obligation model is the most distinctive part of the API.
 * It requires explanation (reserve->commit or abort, exactly once).
 * The analogy to RAII or two-phase commit helps C developers reason
 * about it, but it has no direct C stdlib equivalent.
 */

/* A task that reserves an obligation, does work, then commits it. */
typedef struct {
    asx_co_state co;
    asx_region_id region;
    asx_obligation_id obligation;
    int committed;
} obligated_state;

static asx_status poll_with_obligation(void *ud, asx_task_id self) {
    obligated_state *s = (obligated_state *)ud;
    asx_status st;
    (void)self;

    ASX_CO_BEGIN(&s->co);

    /*
     * ERGO: Reserving an obligation within a poll function is natural.
     * The (region, &out_id) signature matches task_spawn. Consistent.
     */
    st = asx_obligation_reserve(s->region, &s->obligation);
    if (st != ASX_OK) return st;
    printf("  obligation reserved\n");

    ASX_CO_YIELD(&s->co);

    /* Simulate work, then commit. */
    st = asx_obligation_commit(s->obligation);
    if (st != ASX_OK) return st;
    s->committed = 1;
    printf("  obligation committed\n");

    ASX_CO_END(&s->co);
}

/* A task that reserves then aborts its obligation. */
static asx_status poll_abort_obligation(void *ud, asx_task_id self) {
    obligated_state *s = (obligated_state *)ud;
    asx_status st;
    (void)self;

    ASX_CO_BEGIN(&s->co);

    st = asx_obligation_reserve(s->region, &s->obligation);
    if (st != ASX_OK) return st;
    printf("  obligation reserved (will abort)\n");

    ASX_CO_YIELD(&s->co);

    /*
     * ERGO: abort() is the explicit cancellation path for obligations.
     * The naming is clear — "abort" means "I won't fulfill this."
     * Both commit and abort transition from RESERVED to terminal state.
     */
    st = asx_obligation_abort(s->obligation);
    if (st != ASX_OK) return st;
    printf("  obligation aborted\n");

    ASX_CO_END(&s->co);
}

/* -------------------------------------------------------------------
 * Scenario 1: Happy path — reserve, commit, quiesce
 * ------------------------------------------------------------------- */
static int scenario_commit(void) {
    asx_status st;
    asx_region_id region;
    asx_task_id task;
    asx_budget budget;
    void *state_ptr = NULL;
    obligated_state *os;

    printf("--- scenario: obligation commit ---\n");

    asx_runtime_reset();

    st = asx_region_open(&region);
    if (st != ASX_OK) return 1;

    st = asx_task_spawn_captured(region, poll_with_obligation, (uint32_t)sizeof(obligated_state),
                                 NULL, &task, &state_ptr);
    if (st != ASX_OK) return 1;

    os = (obligated_state *)state_ptr;
    os->co = (asx_co_state)ASX_CO_STATE_INIT;
    os->region = region;
    os->committed = 0;

    /*
     * Run scheduler to completion before initiating close/drain so this
     * scenario exercises the explicit commit path instead of close-time
     * cancellation behavior.
     */
    budget = asx_budget_from_polls(100);
    st = asx_scheduler_run(region, &budget);
    if (st != ASX_OK) {
        printf("  FAIL: scheduler_run returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_region_close(region);
    if (st != ASX_OK) {
        printf("  FAIL: region_close returned %s\n", asx_status_str(st));
        return 1;
    }

    budget = asx_budget_from_polls(100);
    st = asx_region_drain(region, &budget);
    if (st != ASX_OK) {
        printf("  FAIL: drain returned %s\n", asx_status_str(st));
        return 1;
    }

    if (!os->committed) {
        printf("  FAIL: obligation was not committed\n");
        return 1;
    }

    printf("  PASS: obligation commit\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 2: Abort path
 * ------------------------------------------------------------------- */
static int scenario_abort(void) {
    asx_status st;
    asx_region_id region;
    asx_task_id task;
    asx_obligation_state ostate;
    asx_budget budget;
    void *state_ptr = NULL;
    obligated_state *os;

    printf("--- scenario: obligation abort ---\n");

    asx_runtime_reset();

    st = asx_region_open(&region);
    if (st != ASX_OK) return 1;

    st = asx_task_spawn_captured(region, poll_abort_obligation, (uint32_t)sizeof(obligated_state),
                                 NULL, &task, &state_ptr);
    if (st != ASX_OK) return 1;

    os = (obligated_state *)state_ptr;
    os->co = (asx_co_state)ASX_CO_STATE_INIT;
    os->region = region;
    os->committed = 0;

    /*
     * Reserve/abort must happen while region is OPEN because obligation
     * reserve is illegal once close starts (OPEN -> CLOSING).
     */
    budget = asx_budget_from_polls(100);
    st = asx_scheduler_run(region, &budget);
    if (st != ASX_OK) {
        printf("  FAIL: scheduler_run returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_obligation_get_state(os->obligation, &ostate);
    if (st != ASX_OK || ostate != ASX_OBLIGATION_ABORTED) {
        printf("  FAIL: obligation should be aborted before close/drain\n");
        return 1;
    }

    st = asx_region_close(region);
    if (st != ASX_OK) {
        printf("  FAIL: region_close returned %s\n", asx_status_str(st));
        return 1;
    }

    budget = asx_budget_from_polls(100);
    st = asx_region_drain(region, &budget);
    if (st != ASX_OK) {
        printf("  FAIL: drain returned %s\n", asx_status_str(st));
        return 1;
    }

    /*
     * ERGO: After abort, quiescence succeeds because the obligation
     * is resolved (aborted counts as resolved). The user does NOT need
     * to distinguish commit vs abort for quiescence purposes — good.
     */
    printf("  PASS: obligation abort\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 3: State query
 * ------------------------------------------------------------------- */
static int scenario_state_query(void) {
    asx_status st;
    asx_region_id region;
    asx_obligation_id obl;
    asx_obligation_state ostate;

    printf("--- scenario: obligation state query ---\n");

    asx_runtime_reset();

    st = asx_region_open(&region);
    if (st != ASX_OK) return 1;

    st = asx_obligation_reserve(region, &obl);
    if (st != ASX_OK) return 1;

    /*
     * ERGO: asx_obligation_get_state is consistent with region/task
     * state queries. The pattern of (id, &out) is uniform across all
     * entity types — good API consistency.
     */
    st = asx_obligation_get_state(obl, &ostate);
    if (st != ASX_OK) return 1;

    if (ostate != ASX_OBLIGATION_RESERVED) {
        printf("  FAIL: expected RESERVED, got %d\n", (int)ostate);
        return 1;
    }
    printf("  obligation state: %s\n", asx_obligation_state_str(ostate));

    /* Commit and verify terminal state. */
    st = asx_obligation_commit(obl);
    if (st != ASX_OK) return 1;

    st = asx_obligation_get_state(obl, &ostate);
    if (st != ASX_OK) return 1;

    if (ostate != ASX_OBLIGATION_COMMITTED) {
        printf("  FAIL: expected COMMITTED, got %d\n", (int)ostate);
        return 1;
    }
    printf("  obligation state: %s\n", asx_obligation_state_str(ostate));

    /*
     * ERGO: Double-commit returns ASX_E_INVALID_TRANSITION — clear
     * error for linearity violation. The error name is descriptive.
     */
    st = asx_obligation_commit(obl);
    if (st != ASX_E_INVALID_TRANSITION) {
        printf("  FAIL: double commit should return INVALID_TRANSITION, "
               "got %s\n",
               asx_status_str(st));
        return 1;
    }
    printf("  double-commit correctly returns: %s\n", asx_status_str(st));

    /* Clean up the region. */
    asx_budget budget = asx_budget_from_polls(10);
    st = asx_region_drain(region, &budget);
    if (st != ASX_OK) {
        printf("  FAIL: drain returned %s\n", asx_status_str(st));
        return 1;
    }

    printf("  PASS: obligation state query\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */
int main(void) {
    int failures = 0;

    printf("=== vignette: obligations ===\n\n");

    failures += scenario_commit();
    failures += scenario_abort();
    failures += scenario_state_query();

    printf("\n=== obligations: %d failures ===\n", failures);
    return failures;
}
