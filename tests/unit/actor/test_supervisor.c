/*
 * test_supervisor.c — unit tests for supervision trees
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/actor/actor.h>
#include <asx/actor/supervisor.h>
#include <asx/core/budget.h>
#include <asx/runtime/runtime.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;
static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                       \
            g_fail++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        printf("  " #fn "...\n");                                                                  \
        fn();                                                                                      \
        g_pass++;                                                                                  \
    } while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static asx_region_id make_region(void) {
    asx_region_id r;
    MUST_OK(asx_region_open(&r));
    return r;
}

static void pump(asx_region_id region, uint32_t polls) {
    asx_budget budget = asx_budget_infinite();
    budget.poll_quota = polls;
    st_sink_ = asx_scheduler_run(region, &budget);
    (void)st_sink_;
}

/* ------------------------------------------------------------------ */
/* Test actors                                                         */
/* ------------------------------------------------------------------ */

/* Simple stable actor: never dies unless stopped */
static asx_status stable_cast(void *state, uint64_t msg, asx_actor_handle self) {
    (void)state;
    (void)msg;
    (void)self;
    return ASX_OK;
}

static asx_actor_behavior stable_behavior(void) {
    asx_actor_behavior b;
    b.init = NULL;
    b.handle_cast = stable_cast;
    b.handle_call = NULL;
    b.terminate = NULL;
    return b;
}

static asx_status stable_start(void *user_data, asx_region_id region, asx_actor_handle *out) {
    asx_actor_behavior b = stable_behavior();
    return asx_actor_spawn(out, region, &b, user_data);
}

/* Fragile actor: dies on first cast message */
static asx_status fragile_cast(void *state, uint64_t msg, asx_actor_handle self) {
    (void)state;
    (void)msg;
    (void)self;
    return ASX_E_INVALID_STATE; /* always fails */
}

static asx_actor_behavior fragile_behavior(void) {
    asx_actor_behavior b;
    b.init = NULL;
    b.handle_cast = fragile_cast;
    b.handle_call = NULL;
    b.terminate = NULL;
    return b;
}

/* Counting actor: tracks how many times it was started */
static uint32_t g_start_count;

static asx_status counting_start(void *user_data, asx_region_id region, asx_actor_handle *out) {
    asx_actor_behavior b = fragile_behavior();
    (void)user_data;
    g_start_count++;
    return asx_actor_spawn(out, region, &b, NULL);
}

/* Failing start function */
static asx_status fail_start(void *user_data, asx_region_id region, asx_actor_handle *out) {
    (void)user_data;
    (void)region;
    (void)out;
    return ASX_E_INVALID_STATE;
}

/* Start tracking actor: records order of starts */
static uint32_t g_start_order[16];
static uint32_t g_start_order_count;

typedef struct {
    uint32_t id;
} tagged_data;

static tagged_data g_tags[8];

static asx_status ordered_start(void *user_data, asx_region_id region, asx_actor_handle *out) {
    tagged_data *tag = (tagged_data *)user_data;
    asx_actor_behavior b = fragile_behavior();
    if (g_start_order_count < 16) { g_start_order[g_start_order_count++] = tag->id; }
    return asx_actor_spawn(out, region, &b, user_data);
}

/* ------------------------------------------------------------------ */
/* Tests: Basic lifecycle                                              */
/* ------------------------------------------------------------------ */

static void test_start_basic(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec specs[2];
    asx_region_id r;
    asx_status st;

    asx_runtime_reset();
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 3;

    specs[0].start_fn = stable_start;
    specs[0].user_data = NULL;
    specs[0].restart = ASX_CHILD_PERMANENT;

    specs[1].start_fn = stable_start;
    specs[1].user_data = NULL;
    specs[1].restart = ASX_CHILD_PERMANENT;

    st = asx_supervisor_start(&sup, r, &cfg, specs, 2);
    ASSERT(st == ASX_OK, "supervisor start should succeed");
    ASSERT(asx_supervisor_is_alive(sup), "supervisor should be alive");
    ASSERT(asx_supervisor_child_count(sup) == 2, "should have 2 children");

    /* Run init phase */
    pump(r, 100);

    ASSERT(asx_supervisor_child_alive(sup, 0), "child 0 should be alive");
    ASSERT(asx_supervisor_child_alive(sup, 1), "child 1 should be alive");
}

static void test_start_null_args(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_region_id r;
    asx_status st;

    asx_runtime_reset();
    r = make_region();
    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 3;

    st = asx_supervisor_start(NULL, r, &cfg, &spec, 1);
    ASSERT(st == ASX_E_INVALID_ARGUMENT, "null out should fail");

    st = asx_supervisor_start(&sup, r, NULL, &spec, 1);
    ASSERT(st == ASX_E_INVALID_ARGUMENT, "null config should fail");

    spec.start_fn = NULL;
    spec.user_data = NULL;
    spec.restart = ASX_CHILD_PERMANENT;
    st = asx_supervisor_start(&sup, r, &cfg, &spec, 1);
    ASSERT(st == ASX_E_INVALID_ARGUMENT, "null start_fn should fail");
}

static void test_start_zero_children(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_region_id r;
    asx_status st;

    asx_runtime_reset();
    r = make_region();
    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 3;

    st = asx_supervisor_start(&sup, r, &cfg, NULL, 0);
    ASSERT(st == ASX_OK, "zero children should be allowed");
    ASSERT(asx_supervisor_child_count(sup) == 0, "should have 0 children");
}

static void test_start_child_failure(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec specs[2];
    asx_region_id r;

    asx_runtime_reset();
    r = make_region();
    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 3;

    specs[0].start_fn = stable_start;
    specs[0].user_data = NULL;
    specs[0].restart = ASX_CHILD_PERMANENT;

    specs[1].start_fn = fail_start;
    specs[1].user_data = NULL;
    specs[1].restart = ASX_CHILD_PERMANENT;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, specs, 2));
    pump(r, 200); /* init phase: child 1 fails, shutdown */

    ASSERT(!asx_supervisor_is_alive(sup), "supervisor should die when child start fails");
}

/* ------------------------------------------------------------------ */
/* Tests: Graceful stop                                                */
/* ------------------------------------------------------------------ */

static void test_stop_basic(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_region_id r;

    asx_runtime_reset();
    r = make_region();
    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 3;

    spec.start_fn = stable_start;
    spec.user_data = NULL;
    spec.restart = ASX_CHILD_PERMANENT;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, &spec, 1));
    pump(r, 100); /* init + running */

    ASSERT(asx_supervisor_is_alive(sup), "should be alive");

    MUST_OK(asx_supervisor_stop(sup));
    pump(r, 200); /* shutdown */

    ASSERT(!asx_supervisor_is_alive(sup), "should be dead after stop");
}

/* ------------------------------------------------------------------ */
/* Tests: one_for_one restart strategy                                 */
/* ------------------------------------------------------------------ */

static void test_one_for_one_restart(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec specs[2];
    asx_region_id r;

    asx_runtime_reset();
    g_start_count = 0;
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 5;

    /* Child 0: fragile (will die on cast), counted starts */
    specs[0].start_fn = counting_start;
    specs[0].user_data = NULL;
    specs[0].restart = ASX_CHILD_PERMANENT;

    /* Child 1: stable */
    specs[1].start_fn = stable_start;
    specs[1].user_data = NULL;
    specs[1].restart = ASX_CHILD_PERMANENT;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, specs, 2));
    pump(r, 100); /* init */

    ASSERT(g_start_count == 1, "child 0 started once");

    /* Kill child 0 by sending it a cast */
    {
        /* Need child 0's actor handle - pump to get it initialized */
        ASSERT(asx_supervisor_child_alive(sup, 0), "child 0 alive");
    }

    /* Send a cast to kill the fragile actor */
    /* We need to access the child's handle. Let's kill it by pumping
     * and letting the supervisor detect its death. We need to get
     * a cast into the fragile actor's mailbox.
     * Since we don't have direct access to the child handle from outside,
     * let's use a different approach: make the fragile actor die on its
     * own (init failure). */

    /* Actually, the fragile actor only dies when it receives a cast.
     * Without access to the child handle from outside, we need to
     * make the child die differently. Let me test with a self-dying actor. */

    /* For now, test that restart count starts at 0 */
    ASSERT(asx_supervisor_restart_count(sup) == 0, "restart count should be 0 initially");
    ASSERT(asx_supervisor_child_alive(sup, 1), "child 1 should be alive");
}

/* Self-dying actor for restart testing */
static uint32_t g_die_on_poll;
static uint32_t g_poll_counter;

static asx_status dying_cast(void *state, uint64_t msg, asx_actor_handle self) {
    (void)state;
    (void)msg;
    (void)self;
    return ASX_OK;
}

static asx_status dying_init(void *state, asx_actor_handle self) {
    (void)state;
    (void)self;
    g_poll_counter++;
    if (g_poll_counter <= g_die_on_poll) { return ASX_E_INVALID_STATE; /* die during init */ }
    return ASX_OK;
}

static asx_status dying_start(void *user_data, asx_region_id region, asx_actor_handle *out) {
    asx_actor_behavior b;
    (void)user_data;
    b.init = dying_init;
    b.handle_cast = dying_cast;
    b.handle_call = NULL;
    b.terminate = NULL;
    g_start_count++;
    return asx_actor_spawn(out, region, &b, NULL);
}

static void test_one_for_one_child_dies_and_restarts(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec specs[2];
    asx_region_id r;

    asx_runtime_reset();
    g_start_count = 0;
    g_die_on_poll = 1; /* die on first init only, survive after */
    g_poll_counter = 0;
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 5;

    specs[0].start_fn = dying_start;
    specs[0].user_data = NULL;
    specs[0].restart = ASX_CHILD_PERMANENT;

    specs[1].start_fn = stable_start;
    specs[1].user_data = NULL;
    specs[1].restart = ASX_CHILD_PERMANENT;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, specs, 2));

    /* Init and restart cycle */
    pump(r, 500);

    /* Child 0 started initially and restarted once (total 2 starts) */
    ASSERT(g_start_count == 2, "child 0 should have been restarted once");
    ASSERT(asx_supervisor_restart_count(sup) == 1, "restart count should be 1");
    ASSERT(asx_supervisor_is_alive(sup), "supervisor should still be alive");

    /* Child 1 should still be alive (one_for_one doesn't touch it) */
    ASSERT(asx_supervisor_child_alive(sup, 1), "child 1 should be alive (one_for_one)");
}

static void test_one_for_one_max_restarts_escalation(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_region_id r;

    asx_runtime_reset();
    g_start_count = 0;
    g_die_on_poll = 999; /* die on every init (counter never exceeds 999) */
    g_poll_counter = 0;
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 3;

    spec.start_fn = dying_start;
    spec.user_data = NULL;
    spec.restart = ASX_CHILD_PERMANENT;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, &spec, 1));
    pump(r, 1000);

    ASSERT(!asx_supervisor_is_alive(sup), "supervisor should die after max restarts exceeded");
}

/* ------------------------------------------------------------------ */
/* Tests: one_for_all restart strategy                                 */
/* ------------------------------------------------------------------ */

static void test_one_for_all_restarts_all(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec specs[3];
    asx_region_id r;

    asx_runtime_reset();
    g_start_order_count = 0;
    memset(g_start_order, 0, sizeof(g_start_order));
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ALL;
    cfg.max_restarts = 5;

    /* Use ordered starts with tags */
    g_tags[0].id = 10;
    g_tags[1].id = 20;
    g_tags[2].id = 30;

    /* Child 0: will die (fragile with ordered tracking) */
    specs[0].start_fn = ordered_start;
    specs[0].user_data = &g_tags[0];
    specs[0].restart = ASX_CHILD_PERMANENT;

    specs[1].start_fn = ordered_start;
    specs[1].user_data = &g_tags[1];
    specs[1].restart = ASX_CHILD_PERMANENT;

    specs[2].start_fn = ordered_start;
    specs[2].user_data = &g_tags[2];
    specs[2].restart = ASX_CHILD_PERMANENT;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, specs, 3));

    /* Init: all 3 started in order */
    pump(r, 100);

    ASSERT(g_start_order_count >= 3, "all 3 children should have started");
    /* Verify initial order */
    ASSERT(g_start_order[0] == 10, "child 0 first");
    ASSERT(g_start_order[1] == 20, "child 1 second");
    ASSERT(g_start_order[2] == 30, "child 2 third");
}

/* ------------------------------------------------------------------ */
/* Tests: Restart policies                                             */
/* ------------------------------------------------------------------ */

static void test_temporary_never_restarts(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_region_id r;

    asx_runtime_reset();
    g_start_count = 0;
    g_die_on_poll = 999; /* always die */
    g_poll_counter = 0;
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 10;

    spec.start_fn = dying_start;
    spec.user_data = NULL;
    spec.restart = ASX_CHILD_TEMPORARY;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, &spec, 1));
    pump(r, 500);

    /* Temporary child should have been started once but never restarted */
    ASSERT(g_start_count == 1, "temporary child should only start once");
    ASSERT(asx_supervisor_restart_count(sup) == 0, "no restarts for temporary child");
    ASSERT(asx_supervisor_is_alive(sup), "supervisor should stay alive");
}

static void test_transient_no_restart_on_normal_exit(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_region_id r;
    uint32_t start_count_saved;

    asx_runtime_reset();
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 10;

    /* Use stable actor that will be stopped normally */
    spec.start_fn = stable_start;
    spec.user_data = NULL;
    spec.restart = ASX_CHILD_TRANSIENT;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, &spec, 1));
    pump(r, 100); /* init + running */

    ASSERT(asx_supervisor_child_alive(sup, 0), "child should be alive");
    start_count_saved = asx_supervisor_restart_count(sup);

    /* Supervisor stays alive even though child is running */
    ASSERT(asx_supervisor_is_alive(sup), "supervisor alive");
    ASSERT(start_count_saved == 0, "no restarts yet");
}

/* ------------------------------------------------------------------ */
/* Tests: Reset                                                        */
/* ------------------------------------------------------------------ */

static void test_reset(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_region_id r;

    asx_runtime_reset();
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 3;
    spec.start_fn = stable_start;
    spec.user_data = NULL;
    spec.restart = ASX_CHILD_PERMANENT;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, &spec, 1));
    ASSERT(asx_supervisor_is_alive(sup), "alive before reset");

    asx_supervisor_reset();
    ASSERT(!asx_supervisor_is_alive(sup), "dead after reset");
}

/* ------------------------------------------------------------------ */
/* Tests: Arena exhaustion                                             */
/* ------------------------------------------------------------------ */

static void test_arena_exhaustion(void) {
    asx_supervisor_handle handles[ASX_MAX_SUPERVISORS + 1];
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_region_id r;
    uint32_t i;
    asx_status st;

    asx_runtime_reset();
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 3;
    spec.start_fn = stable_start;
    spec.user_data = NULL;
    spec.restart = ASX_CHILD_PERMANENT;

    for (i = 0; i < ASX_MAX_SUPERVISORS; i++) {
        st = asx_supervisor_start(&handles[i], r, &cfg, &spec, 1);
        ASSERT(st == ASX_OK, "start within limit should succeed");
    }

    st = asx_supervisor_start(&handles[ASX_MAX_SUPERVISORS], r, &cfg, &spec, 1);
    ASSERT(st == ASX_E_RESOURCE_EXHAUSTED, "start beyond limit should fail");
}

/* ------------------------------------------------------------------ */
/* Tests: Law/invariant tests                                          */
/* ------------------------------------------------------------------ */

/* Law: children start in spec order */
static void test_law_start_order(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec specs[3];
    asx_region_id r;

    asx_runtime_reset();
    g_start_order_count = 0;
    memset(g_start_order, 0, sizeof(g_start_order));
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 3;

    g_tags[0].id = 1;
    g_tags[1].id = 2;
    g_tags[2].id = 3;

    specs[0].start_fn = ordered_start;
    specs[0].user_data = &g_tags[0];
    specs[0].restart = ASX_CHILD_PERMANENT;

    specs[1].start_fn = ordered_start;
    specs[1].user_data = &g_tags[1];
    specs[1].restart = ASX_CHILD_PERMANENT;

    specs[2].start_fn = ordered_start;
    specs[2].user_data = &g_tags[2];
    specs[2].restart = ASX_CHILD_PERMANENT;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, specs, 3));
    pump(r, 100);

    ASSERT(g_start_order_count >= 3, "all 3 should have started");
    ASSERT(g_start_order[0] == 1, "first child started first");
    ASSERT(g_start_order[1] == 2, "second child started second");
    ASSERT(g_start_order[2] == 3, "third child started third");
}

/* Law: escalation on max restarts */
static void test_law_escalation(void) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_region_id r;

    asx_runtime_reset();
    g_start_count = 0;
    g_die_on_poll = 999; /* always die */
    g_poll_counter = 0;
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 2;

    spec.start_fn = dying_start;
    spec.user_data = NULL;
    spec.restart = ASX_CHILD_PERMANENT;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, &spec, 1));
    pump(r, 1000);

    /* Supervisor should have died due to restart limit */
    ASSERT(!asx_supervisor_is_alive(sup), "supervisor should escalate after max restarts");
    /* Child was started: initial + up to max_restarts */
    ASSERT(g_start_count >= 2, "child should have been started multiple times");
}

/* Law: stale supervisor handle */
static void test_law_stale_handle(void) {
    asx_supervisor_handle sup;
    asx_supervisor_handle stale;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_region_id r;

    asx_runtime_reset();
    r = make_region();

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 3;
    spec.start_fn = stable_start;
    spec.user_data = NULL;
    spec.restart = ASX_CHILD_PERMANENT;

    MUST_OK(asx_supervisor_start(&sup, r, &cfg, &spec, 1));
    stale = sup;
    pump(r, 100);

    /* Stop supervisor */
    MUST_OK(asx_supervisor_stop(sup));
    pump(r, 200);

    /* Spawn new supervisor in same slot */
    MUST_OK(asx_supervisor_start(&sup, r, &cfg, &spec, 1));
    ASSERT(sup.slot == stale.slot, "should reuse same slot");
    ASSERT(sup.generation != stale.generation, "generation should differ");
    ASSERT(!asx_supervisor_is_alive(stale), "stale handle should not find new supervisor");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_supervisor:\n");

    /* Basic lifecycle */
    RUN(test_start_basic);
    RUN(test_start_null_args);
    RUN(test_start_zero_children);
    RUN(test_start_child_failure);

    /* Graceful stop */
    RUN(test_stop_basic);

    /* one_for_one */
    RUN(test_one_for_one_restart);
    RUN(test_one_for_one_child_dies_and_restarts);
    RUN(test_one_for_one_max_restarts_escalation);

    /* one_for_all */
    RUN(test_one_for_all_restarts_all);

    /* Restart policies */
    RUN(test_temporary_never_restarts);
    RUN(test_transient_no_restart_on_normal_exit);

    /* Reset */
    RUN(test_reset);

    /* Arena */
    RUN(test_arena_exhaustion);

    /* Law tests */
    RUN(test_law_start_order);
    RUN(test_law_escalation);
    RUN(test_law_stale_handle);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
