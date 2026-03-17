/*
 * ex_quickstart.c — Quick-start: kernel lifecycle plus modern runtime surfaces
 *
 * This is the canonical "hello world" walkthrough for asupersync. It demonstrates:
 *   1. Opening a region (the ownership scope for tasks)
 *   2. Spawning tasks with captured state (region-owned memory)
 *   3. Running the scheduler with a bounded poll budget
 *   4. Draining the region through its full close protocol
 *   5. Verifying quiescence (all tasks done, all obligations resolved)
 *   6. Using the current-thread runtime builder, local scope wrappers,
 *      blocking offload surface, and deadline arming in one flow
 *
 * Output: SCENARIO lines for smoke-test validation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

/* --- Scenario macros (shared pattern) --- */

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
 * Task: countdown
 *
 * A coroutine-style task that yields `remaining` times then completes.
 * State is captured in region-owned memory via asx_task_spawn_captured.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_co_state co;
    int remaining;
} countdown_state;

static asx_status poll_countdown(void *ud, asx_task_id self) {
    countdown_state *s = (countdown_state *)ud;
    (void)self;
    ASX_CO_BEGIN(&s->co);
    while (s->remaining > 0) {
        s->remaining--;
        ASX_CO_YIELD(&s->co);
    }
    ASX_CO_END(&s->co);
}

/* -------------------------------------------------------------------
 * Scenario: quickstart.region_task_drain
 *
 * Opens a region, spawns two countdown tasks, runs the scheduler to
 * completion, drains the region, and checks quiescence.
 * ------------------------------------------------------------------- */

static void scenario_region_task_drain(void) {
    SCENARIO_BEGIN("quickstart.region_task_drain");

    asx_runtime_reset();

    /* Step 1: Open a region */
    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Step 2: Spawn two tasks with captured state */
    asx_task_id t1, t2;
    void *s1_raw = NULL;
    void *s2_raw = NULL;
    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_countdown, (uint32_t)sizeof(countdown_state),
                                           NULL, &t1, &s1_raw) == ASX_OK,
                   "spawn_t1");
    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_countdown, (uint32_t)sizeof(countdown_state),
                                           NULL, &t2, &s2_raw) == ASX_OK,
                   "spawn_t2");

    /* Initialize countdown values */
    ((countdown_state *)s1_raw)->remaining = 3;
    ((countdown_state *)s2_raw)->remaining = 5;

    /* Step 3: Run scheduler with bounded budget */
    asx_budget budget = asx_budget_from_polls(64);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    /* Step 4: Both tasks should have completed */
    asx_task_state state;
    SCENARIO_CHECK(asx_task_get_state(t1, &state) == ASX_OK && state == ASX_TASK_COMPLETED,
                   "t1_completed");
    SCENARIO_CHECK(asx_task_get_state(t2, &state) == ASX_OK && state == ASX_TASK_COMPLETED,
                   "t2_completed");

    /* Step 5: Drain the region (close protocol) */
    budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "drain_ok");

    /* Step 6: Verify quiescence */
    SCENARIO_CHECK(asx_quiescence_check(rid) == ASX_OK, "quiescent");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: quickstart.obligation_protocol
 *
 * Demonstrates the obligation reserve/commit protocol that ensures
 * all outstanding work is accounted for before region close.
 * ------------------------------------------------------------------- */

static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

typedef struct {
    int remaining;
} local_countdown_state;

static asx_status poll_local_countdown(void *ud, asx_task_id self) {
    local_countdown_state *state = (local_countdown_state *)ud;
    (void)self;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->remaining > 0) {
        state->remaining--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

static uint64_t blocking_add_one(void *user_data) {
    uint64_t value = *(uint64_t *)user_data;
    return value + 1u;
}

static void scenario_obligation_protocol(void) {
    SCENARIO_BEGIN("quickstart.obligation_protocol");

    asx_runtime_reset();

    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Reserve an obligation — must be resolved before quiescence */
    asx_obligation_id oid;
    SCENARIO_CHECK(asx_obligation_reserve(rid, &oid) == ASX_OK, "reserve");

    /* Spawn a task that completes immediately */
    asx_task_id tid;
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "spawn");

    /* Commit the obligation (resolve it) */
    SCENARIO_CHECK(asx_obligation_commit(oid) == ASX_OK, "commit");

    /* Drain and verify quiescence */
    asx_budget budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "drain");
    SCENARIO_CHECK(asx_quiescence_check(rid) == ASX_OK, "quiescent");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: quickstart.runtime_builder_local_blocking_deadline
 *
 * Demonstrates the higher-level runtime surfaces layered on top of the
 * kernel: current-thread runtime bootstrap, local scope wrappers,
 * inline blocking completion, and deadline/timer-wheel integration.
 * ------------------------------------------------------------------- */

static void scenario_runtime_builder_local_blocking_deadline(void) {
    asx_runtime_builder builder;
    asx_runtime runtime;
    asx_region_id rid;
    asx_cx root_cx;
    asx_local_scope scope;
    asx_local_task_handle handle;
    asx_outcome outcome;
    local_countdown_state *state = NULL;
    asx_blocking_handle blocking;
    uint64_t blocking_input = 41u;
    uint64_t blocking_result = 0u;
    asx_deadline deadline;
    int deadline_marker = 7;
    void *wakers[1];
    uint32_t fired = 0u;

    memset(&runtime, 0, sizeof(runtime));
    memset(&builder, 0, sizeof(builder));
    memset(&scope, 0, sizeof(scope));
    memset(&handle, 0, sizeof(handle));
    memset(&outcome, 0, sizeof(outcome));
    memset(&blocking, 0, sizeof(blocking));
    memset(&deadline, 0, sizeof(deadline));

    SCENARIO_BEGIN("quickstart.runtime_builder_local_blocking_deadline");

    SCENARIO_CHECK(asx_runtime_builder_init_current_thread(&builder) == ASX_OK, "builder_init");
    SCENARIO_CHECK(asx_runtime_builder_set_finalizer_poll_budget(&builder, 48u) == ASX_OK,
                   "builder_budget");
    SCENARIO_CHECK(asx_runtime_builder_build(&builder, &runtime) == ASX_OK, "runtime_build");
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_cx_init(&root_cx, rid, ASX_INVALID_ID, ASX_CAP_ALL) == ASX_OK,
                   "root_cx");
    SCENARIO_CHECK(asx_local_scope_init(&scope, rid, &root_cx, asx_budget_from_polls(8u)) ==
                       ASX_OK,
                   "local_scope_init");
    SCENARIO_CHECK(asx_local_scope_spawn_captured(&scope, poll_local_countdown,
                                                  (uint32_t)sizeof(*state), NULL, &handle,
                                                  (void **)&state) == ASX_OK,
                   "local_spawn");
    SCENARIO_CHECK(state != NULL, "local_state");
    state->remaining = 2;

    SCENARIO_CHECK(asx_local_scope_run(&scope) == ASX_OK, "local_run");
    SCENARIO_CHECK(asx_local_task_handle_try_join(&handle, &outcome) == ASX_OK, "local_join");
    SCENARIO_CHECK(asx_local_task_handle_join_error(&outcome) == ASX_JOIN_OK, "local_join_ok");

    SCENARIO_CHECK(asx_spawn_blocking(blocking_add_one, &blocking_input, NULL, &blocking) == ASX_OK,
                   "spawn_blocking");
    SCENARIO_CHECK(asx_blocking_get_result(&blocking, &blocking_result) == ASX_OK,
                   "blocking_result_status");
    SCENARIO_CHECK(blocking_result == 42u, "blocking_result_value");

    asx_timer_wheel_reset(asx_timer_wheel_global());
    SCENARIO_CHECK(asx_deadline_init(&deadline, 5u) == ASX_OK, "deadline_init");
    SCENARIO_CHECK(asx_deadline_arm(&deadline, &deadline_marker) == ASX_OK, "deadline_arm");
    fired = asx_timer_collect_expired(asx_timer_wheel_global(), 6u, wakers, 1u);
    SCENARIO_CHECK(fired == 1u, "deadline_fired");
    SCENARIO_CHECK(wakers[0] == &deadline_marker, "deadline_marker");
    SCENARIO_CHECK(asx_deadline_is_expired_at(&deadline, 6u), "deadline_expired");

    asx_runtime_shutdown(&runtime);
    SCENARIO_END();
}

int main(void) {
    scenario_region_task_drain();
    scenario_obligation_protocol();
    scenario_runtime_builder_local_blocking_deadline();

    printf("SCENARIO _summary %s total=%d pass=%d fail=%d\n", g_fail > 0 ? "fail" : "pass",
           g_pass + g_fail, g_pass, g_fail);

    return g_fail > 0 ? 1 : 0;
}
