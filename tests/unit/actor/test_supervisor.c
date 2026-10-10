/*
 * test_supervisor.c — unit tests for the managed supervisor
 *
 * An owner task spawns a supervisor in its region and joins it; the
 * children are generations of test bodies whose behaviour is set per
 * child: fail, succeed, yield a while, wait until cancelled, panic, or a
 * start function that fails.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/actor/supervisor.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
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
        int before_ = g_fail;                                                                      \
        printf("  " #fn "...\n");                                                                  \
        fn();                                                                                      \
        if (g_fail == before_) g_pass++;                                                           \
    } while (0)

/* ------------------------------------------------------------------ */
/* Children                                                            */
/* ------------------------------------------------------------------ */

/* What every generation of a child does. */
typedef struct {
    uint32_t yields;        /* polls that yield before the end */
    asx_status result;      /* the body's end status (ASX_OK: ok) */
    uint64_t fail_below;    /* generations numbered below this end with result */
    asx_status later;       /* ...and later ones with this one */
    int block;              /* wait until cancelled, then end ASX_E_CANCELLED */
    int blind;              /* wait until woken, then end with result, never checking */
    int panic;              /* panic instead of ending */
    int fail_start;         /* the start function fails */
    uint64_t sleep_ns;      /* virtual time each generation waits before its end */
    asx_task_id task;       /* the latest generation's task */
    uint32_t started;       /* start function calls */
    uint64_t last_number;   /* the latest generation's number */
    uint32_t order;         /* g_order when it last started */
    asx_time started_at[8]; /* when each of the first eight started */
} behaviour;

typedef struct {
    behaviour *b;
    uint64_t number;
    uint32_t polls;
    asx_time wake_at; /* the end of its sleep, 0 before its first poll */
} body;

static body g_bodies[128];
static uint32_t g_n_bodies;
static uint32_t g_order;

static asx_time now_ns(void) {
    asx_time t = 0;
    if (asx_runtime_now_ns(&t) != ASX_OK) t = asx_runtime_virtual_now();
    return t;
}

static asx_status body_poll(void *user_data, asx_task_id self) {
    body *bd = (body *)user_data;
    const behaviour *b = bd->b;
    if (b->block) {
        asx_checkpoint_result cr;
        if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_E_CANCELLED;
        MUST_OK(asx_task_park(self));
        return ASX_E_PENDING;
    }
    if (b->blind && bd->polls++ == 0u) {
        MUST_OK(asx_task_park(self));
        return ASX_E_PENDING;
    }
    if (b->sleep_ns != 0u) {
        if (bd->wake_at == 0u) bd->wake_at = now_ns() + b->sleep_ns;
        if (asx_task_wait_until(self, bd->wake_at) == ASX_E_PENDING) return ASX_E_PENDING;
        MUST_OK(asx_task_complete_timer(self));
    }
    if (bd->polls++ < b->yields) return ASX_E_PENDING;
    if (b->panic) {
        MUST_OK(asx_task_panic(self, "boom"));
        return ASX_OK;
    }
    if (b->fail_below != 0u) return bd->number < b->fail_below ? b->result : b->later;
    return b->result;
}

static asx_status start_child(void *user_data, const asx_supervisor_generation *gen,
                              asx_task_poll_fn *out_poll, void **out_data) {
    behaviour *b = (behaviour *)user_data;
    if (b->started < 8u) b->started_at[b->started] = now_ns();
    b->started++;
    b->last_number = gen->number;
    b->task = gen->task;
    b->order = ++g_order;
    if (b->fail_start || g_n_bodies >= 128u) return ASX_E_INVALID_STATE;
    g_bodies[g_n_bodies].b = b;
    g_bodies[g_n_bodies].number = gen->number;
    g_bodies[g_n_bodies].polls = 0;
    g_bodies[g_n_bodies].wake_at = 0;
    *out_poll = body_poll;
    *out_data = &g_bodies[g_n_bodies++];
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* The owner                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_region_id region; /* the owner's, where the supervisor is spawned */
    asx_supervisor_config cfg;
    asx_child_spec specs[4];
    uint32_t n;
    uint32_t abort_after;  /* polls before an abort, 0 for none */
    behaviour *victim;     /* a child whose generation it cancels directly... */
    uint32_t cancel_after; /* ...after this many polls, 0 for none */
    asx_status cancel_st;
    uint32_t polls;
    int spawned;
    asx_status spawn_st;
    asx_supervisor_handle h;
    asx_status join_st;
    int joined;
    asx_supervisor_report report;
} owner;

static asx_status owner_poll(void *user_data, asx_task_id self) {
    owner *o = (owner *)user_data;
    if (!o->spawned) {
        o->spawned = 1;
        o->spawn_st = asx_supervisor_spawn(&o->h, o->region, &o->cfg, o->specs, o->n);
        if (o->spawn_st != ASX_OK) return ASX_OK;
    }
    o->polls++;
    if (o->cancel_after != 0u) {
        if (o->polls < o->cancel_after) return ASX_E_PENDING; /* yield */
        if (o->polls == o->cancel_after) {
            o->cancel_st = asx_task_cancel(o->victim->task, ASX_CANCEL_USER);
        }
    }
    if (o->abort_after != 0u) {
        if (o->polls < o->abort_after) return ASX_E_PENDING; /* yield */
        if (o->polls == o->abort_after) MUST_OK(asx_supervisor_abort(o->h));
    }
    o->join_st = asx_supervisor_join(o->h, self, &o->report);
    if (o->join_st == ASX_E_PENDING) return ASX_E_PENDING;
    o->joined = 1;
    return ASX_OK;
}

static asx_region_id g_root;
static asx_region_id g_owner_region;

static void setup(void) {
    asx_runtime_reset();
    g_n_bodies = 0;
    g_order = 0;
    MUST_OK(asx_region_open(&g_root));
    MUST_OK(asx_region_open_child(g_root, &g_owner_region));
}

static void owner_init(owner *o, uint32_t max_restarts) {
    memset(o, 0, sizeof(*o));
    asx_supervisor_config_init(&o->cfg, "sup", max_restarts, 60000000000u);
    o->cfg.backoff.kind = ASX_RESTART_BACKOFF_NONE;
}

static void add_child(owner *o, const char *name, asx_child_restart restart, behaviour *b) {
    asx_child_spec_init(&o->specs[o->n++], name, restart, start_child, b);
}

/* Spawn the owner and run until nothing is runnable. */
static void run_owner(owner *o) {
    asx_task_id id = ASX_INVALID_ID;
    asx_budget b = asx_budget_from_polls(5000);
    o->region = g_owner_region;
    MUST_OK(asx_task_spawn(g_owner_region, owner_poll, o, &id));
    st_sink_ = asx_scheduler_run(g_root, &b);
    (void)st_sink_;
}

static const asx_supervisor_completion *completion_of(const owner *o, uint32_t child) {
    uint32_t i;
    for (i = 0; i < o->report.completion_count; i++) {
        if (o->report.completions[i].child == child) return &o->report.completions[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Configuration and compilation                                       */
/* ------------------------------------------------------------------ */

static void test_config_defaults(void) {
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_supervisor_config_init(&cfg, "s", 3, 1000u);
    ASSERT(cfg.strategy == ASX_SUPERVISOR_ONE_FOR_ONE, "one_for_one by default");
    ASSERT(cfg.escalation == ASX_ESCALATION_STOP, "stop by default");
    ASSERT(cfg.backoff.kind == ASX_RESTART_BACKOFF_EXPONENTIAL, "exponential backoff");
    ASSERT(cfg.backoff.initial_ns == 100000000u && cfg.backoff.max_ns == 10000000000u &&
               cfg.backoff.multiplier == 2u,
           "Rust's default backoff: 100 ms doubling up to 10 s");
    ASSERT(cfg.budget == NULL && cfg.max_restarts == 3u && cfg.window_ns == 1000u, "limits");
    asx_child_spec_init(&spec, "c", ASX_CHILD_TRANSIENT, start_child, NULL);
    ASSERT(spec.required == 1u && spec.start_immediately == 1u && spec.dep_count == 0u,
           "ChildSpec::new's defaults");
    ASSERT(asx_child_spec_depends_on(&spec, 1) == ASX_OK, "a dependency");
    ASSERT(asx_child_spec_depends_on(&spec, 2) == ASX_OK, "a second dependency");
    ASSERT(asx_child_spec_depends_on(&spec, 3) == ASX_OK, "a third dependency");
    ASSERT(asx_child_spec_depends_on(&spec, 4) == ASX_OK, "a fourth dependency");
    ASSERT(asx_child_spec_depends_on(&spec, 5) == ASX_E_RESOURCE_EXHAUSTED, "too many");
}

static void test_spawn_rejects_bad_arguments(void) {
    asx_supervisor_config cfg;
    asx_child_spec specs[ASX_SUPERVISOR_MAX_CHILDREN + 1u];
    asx_supervisor_handle h;
    behaviour b;
    uint32_t i;
    setup();
    memset(&b, 0, sizeof(b));
    asx_supervisor_config_init(&cfg, "s", 1, 1000u);
    for (i = 0; i < ASX_SUPERVISOR_MAX_CHILDREN + 1u; i++) {
        asx_child_spec_init(&specs[i], "c", ASX_CHILD_PERMANENT, start_child, &b);
    }
    ASSERT(asx_supervisor_spawn(NULL, g_root, &cfg, specs, 1) == ASX_E_INVALID_ARGUMENT, "out");
    ASSERT(asx_supervisor_spawn(&h, g_root, NULL, specs, 1) == ASX_E_INVALID_ARGUMENT, "config");
    ASSERT(asx_supervisor_spawn(&h, g_root, &cfg, NULL, 1) == ASX_E_INVALID_ARGUMENT, "children");
    ASSERT(asx_supervisor_spawn(&h, g_root, &cfg, specs, 0) == ASX_E_INVALID_ARGUMENT, "none");
    ASSERT(asx_supervisor_spawn(&h, g_root, &cfg, specs, ASX_SUPERVISOR_MAX_CHILDREN + 1u) ==
               ASX_E_INVALID_ARGUMENT,
           "too many children");
    cfg.max_restarts = ASX_SUPERVISOR_MAX_RESTARTS + 1u;
    ASSERT(asx_supervisor_spawn(&h, g_root, &cfg, specs, 1) == ASX_E_INVALID_ARGUMENT,
           "max_restarts above the history bound");
    cfg.max_restarts = 1;
    specs[0].start = NULL;
    ASSERT(asx_supervisor_spawn(&h, g_root, &cfg, specs, 1) == ASX_E_INVALID_ARGUMENT,
           "no start function");
    specs[0].start = start_child;
    specs[0].name = "a-name-that-is-far-too-long-for-a-child";
    ASSERT(asx_supervisor_spawn(&h, g_root, &cfg, specs, 1) == ASX_E_INVALID_ARGUMENT,
           "name too long");
}

static void test_spawn_compiles_the_topology(void) {
    asx_supervisor_config cfg;
    asx_child_spec specs[3];
    asx_supervisor_handle h;
    behaviour b;
    setup();
    memset(&b, 0, sizeof(b));
    asx_supervisor_config_init(&cfg, "s", 1, 1000u);
    asx_child_spec_init(&specs[0], "a", ASX_CHILD_PERMANENT, start_child, &b);
    asx_child_spec_init(&specs[1], "a", ASX_CHILD_PERMANENT, start_child, &b);
    ASSERT(asx_supervisor_spawn(&h, g_root, &cfg, specs, 2) == ASX_E_NAME_CONFLICT,
           "duplicate names");
    specs[1].name = "b";
    MUST_OK(asx_child_spec_depends_on(&specs[1], 1));
    ASSERT(asx_supervisor_spawn(&h, g_root, &cfg, specs, 2) == ASX_E_INVALID_ARGUMENT,
           "a child depending on itself");
    specs[1].dep_count = 0;
    MUST_OK(asx_child_spec_depends_on(&specs[1], 7));
    ASSERT(asx_supervisor_spawn(&h, g_root, &cfg, specs, 2) == ASX_E_INVALID_ARGUMENT,
           "an unknown dependency");
    specs[1].dep_count = 0;
    MUST_OK(asx_child_spec_depends_on(&specs[0], 1));
    MUST_OK(asx_child_spec_depends_on(&specs[1], 0));
    ASSERT(asx_supervisor_spawn(&h, g_root, &cfg, specs, 2) == ASX_E_INVALID_ARGUMENT, "a cycle");
    specs[0].dep_count = 0;
    specs[0].start_immediately = 0;
    ASSERT(asx_supervisor_spawn(&h, g_root, &cfg, specs, 2) == ASX_E_INVALID_ARGUMENT,
           "a child started at boot depending on a deferred one");
}

static void test_dependencies_order_the_start(void) {
    owner o;
    behaviour first;
    behaviour second;
    behaviour third;
    setup();
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(&third, 0, sizeof(third));
    owner_init(&o, 0);
    /* "second" depends on "first" though listed before it. */
    add_child(&o, "second", ASX_CHILD_TEMPORARY, &second);
    add_child(&o, "first", ASX_CHILD_TEMPORARY, &first);
    add_child(&o, "third", ASX_CHILD_TEMPORARY, &third);
    MUST_OK(asx_child_spec_depends_on(&o.specs[0], 1));
    run_owner(&o);
    ASSERT(o.joined && o.join_st == ASX_OK, "joined");
    ASSERT(first.order < second.order, "a dependency starts first");
    ASSERT(third.order == 3u, "the others keep spec order");
    ASSERT(o.report.completion_count == 3u && o.report.completions[0].child == 1u &&
               o.report.completions[1].child == 0u,
           "completions in compiled order, with spec indices");
}

/* ------------------------------------------------------------------ */
/* Restart modes and intensity                                         */
/* ------------------------------------------------------------------ */

static void test_transient_child_restarts_until_the_limit(void) {
    owner o;
    behaviour b;
    const asx_supervisor_completion *c;
    setup();
    memset(&b, 0, sizeof(b));
    b.result = ASX_E_INVALID_STATE;
    owner_init(&o, 2);
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.spawn_st == ASX_OK && o.joined && o.join_st == ASX_OK, "spawned and joined");
    ASSERT(b.started == 3u, "the first generation and two restarts");
    ASSERT(o.report.outcome == ASX_OUTCOME_OK, "a refused restart under STOP is not an error");
    ASSERT(o.report.restart_batches == 2u && o.report.started == 3u && o.report.joined == 3u,
           "counters");
    c = completion_of(&o, 0);
    ASSERT(c != NULL && c->generation.number == 3u, "the latest generation is kept");
    ASSERT(c->outcome == ASX_OUTCOME_ERR && c->status == ASX_E_INVALID_STATE, "its error");
    ASSERT(c->task_outcome == ASX_OUTCOME_OK, "its task itself completed ok");
    ASSERT(!asx_supervisor_is_alive(o.h) && asx_supervisor_finished(o.h), "finished");
}

static void test_transient_child_that_succeeds_stays_stopped(void) {
    owner o;
    behaviour b;
    setup();
    memset(&b, 0, sizeof(b));
    b.result = ASX_OK;
    owner_init(&o, 3);
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.joined && b.started == 1u && o.report.restart_batches == 0u, "not restarted");
    ASSERT(completion_of(&o, 0)->outcome == ASX_OUTCOME_OK, "ok");
}

static void test_permanent_child_restarts_after_success(void) {
    owner o;
    behaviour b;
    setup();
    memset(&b, 0, sizeof(b));
    b.result = ASX_OK;
    owner_init(&o, 1);
    add_child(&o, "worker", ASX_CHILD_PERMANENT, &b);
    run_owner(&o);
    ASSERT(o.joined && b.started == 2u && o.report.restart_batches == 1u,
           "restarted once, then refused");
}

static void test_temporary_child_never_restarts(void) {
    owner o;
    behaviour b;
    setup();
    memset(&b, 0, sizeof(b));
    b.result = ASX_E_INVALID_STATE;
    owner_init(&o, 5);
    add_child(&o, "worker", ASX_CHILD_TEMPORARY, &b);
    run_owner(&o);
    ASSERT(o.joined && b.started == 1u && o.report.restart_batches == 0u, "never restarted");
}

static void test_max_restarts_zero_refuses_at_once(void) {
    owner o;
    behaviour b;
    setup();
    memset(&b, 0, sizeof(b));
    b.result = ASX_E_INVALID_STATE;
    owner_init(&o, 0);
    add_child(&o, "worker", ASX_CHILD_PERMANENT, &b);
    run_owner(&o);
    ASSERT(o.joined && b.started == 1u && o.report.outcome == ASX_OUTCOME_OK, "stopped");
}

static void test_reset_counter_keeps_restarting(void) {
    owner o;
    behaviour b;
    setup();
    memset(&b, 0, sizeof(b));
    b.result = ASX_E_INVALID_STATE;
    b.fail_below = 5;
    b.later = ASX_OK;
    owner_init(&o, 1);
    o.cfg.escalation = ASX_ESCALATION_RESET_COUNTER;
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.joined && b.started == 5u, "restarted past the limit until it succeeded");
    ASSERT(o.report.restart_batches == 4u, "four batches");
}

/* Each generation lives 2 us, then fails; the fourth succeeds. One restart
 * a minute stops the child after its first replacement, one restart per
 * microsecond lets every restart through: the earlier one has aged out of
 * the window by the next failure (RestartTracker's sliding window). */
static void test_restarts_age_out_of_the_window(void) {
    owner o;
    behaviour b;
    setup();
    memset(&b, 0, sizeof(b));
    b.sleep_ns = 2000u;
    b.result = ASX_E_INVALID_STATE;
    b.fail_below = 4;
    b.later = ASX_OK;
    owner_init(&o, 1);
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.joined && b.started == 2u && o.report.restart_batches == 1u,
           "a 60 s window refuses the second restart");

    setup();
    memset(&b, 0, sizeof(b));
    b.sleep_ns = 2000u;
    b.result = ASX_E_INVALID_STATE;
    b.fail_below = 4;
    b.later = ASX_OK;
    owner_init(&o, 1);
    o.cfg.window_ns = 1000u;
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.joined && b.started == 4u && o.report.restart_batches == 3u,
           "a 1 us window lets each restart through");
    ASSERT(b.started_at[3] - b.started_at[0] == 6000u, "three generations of 2 us each before");
    ASSERT(completion_of(&o, 0)->outcome == ASX_OUTCOME_OK, "the fourth ended ok");
}

/* A transient generation whose task ends cancelled stopped normally for
 * Rust (ManagedRestartMode::eligible): the task's join is Cancelled when
 * the cancel dominates a return the body never acknowledged, and such a
 * child is not replaced, even when that return was an error. A body that
 * observed the cancel and returned
 * ASX_E_CANCELLED returned an error, which its acknowledged cancel does not
 * dominate (Rust's join gives the user's return), so it is replaced; and a
 * permanent child is replaced either way. */
static void test_cancelled_transient_child_is_not_restarted(void) {
    owner o;
    behaviour b;
    const asx_supervisor_completion *c;
    setup();
    memset(&b, 0, sizeof(b));
    b.blind = 1;
    b.result = ASX_E_INVALID_STATE;
    owner_init(&o, 3);
    o.victim = &b;
    o.cancel_after = 6;
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.cancel_st == ASX_OK, "the generation's task was cancelled");
    ASSERT(o.joined && b.started == 1u && o.report.restart_batches == 0u, "not replaced");
    c = completion_of(&o, 0);
    ASSERT(c != NULL && c->task_outcome == ASX_OUTCOME_CANCELLED && c->outcome == ASX_OUTCOME_ERR &&
               !c->shutdown_requested_before_completion,
           "its task ended cancelled over its body's error, not stopped by the supervisor");
    ASSERT(o.report.outcome == ASX_OUTCOME_OK, "the supervisor stopped normally");

    setup();
    memset(&b, 0, sizeof(b));
    b.block = 1;
    owner_init(&o, 1);
    o.victim = &b;
    o.cancel_after = 6;
    o.abort_after = 12;
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.cancel_st == ASX_OK && o.joined, "cancelled, then the supervisor aborted");
    ASSERT(b.started == 2u && o.report.restart_batches == 1u,
           "an acknowledged cancel's ASX_E_CANCELLED is an error: replaced");

    setup();
    memset(&b, 0, sizeof(b));
    b.blind = 1;
    b.result = ASX_OK;
    owner_init(&o, 1);
    o.victim = &b;
    o.cancel_after = 6;
    o.abort_after = 12;
    add_child(&o, "worker", ASX_CHILD_PERMANENT, &b);
    run_owner(&o);
    ASSERT(o.cancel_st == ASX_OK && o.joined, "cancelled, then the supervisor aborted");
    ASSERT(b.started == 2u && o.report.restart_batches == 1u, "a permanent one is replaced");
}

static void test_escalate_cancels_the_owner_region(void) {
    owner o;
    behaviour b;
    asx_cancel_reason r;
    setup();
    memset(&b, 0, sizeof(b));
    b.result = ASX_E_INVALID_STATE;
    owner_init(&o, 0);
    o.cfg.escalation = ASX_ESCALATION_ESCALATE;
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.joined && o.join_st == ASX_OK, "the join is not interrupted");
    ASSERT(o.report.outcome == ASX_OUTCOME_ERR &&
               o.report.error == ASX_SUPERVISOR_ERR_RESTART_LIMIT && o.report.error_child == 0u,
           "RestartLimit for the child");
    ASSERT(o.report.escalations == 1u, "one escalation");
    ASSERT(asx_region_get_cancel_reason(g_owner_region, &r) == ASX_OK &&
               r.kind == ASX_CANCEL_FAIL_FAST,
           "the owner's region was cancelled FailFast");
    ASSERT(r.message != NULL &&
               strcmp(r.message, "managed supervisor restart intensity exhausted") == 0,
           "with Rust's message");
}

/* ------------------------------------------------------------------ */
/* Strategies                                                          */
/* ------------------------------------------------------------------ */

static void test_one_for_all_restarts_the_siblings(void) {
    owner o;
    behaviour failing;
    behaviour sibling;
    const asx_supervisor_completion *c;
    setup();
    memset(&failing, 0, sizeof(failing));
    memset(&sibling, 0, sizeof(sibling));
    failing.result = ASX_E_INVALID_STATE;
    sibling.yields = 3;
    sibling.result = ASX_OK;
    owner_init(&o, 1);
    o.cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ALL;
    add_child(&o, "failing", ASX_CHILD_TRANSIENT, &failing);
    add_child(&o, "sibling", ASX_CHILD_TRANSIENT, &sibling);
    run_owner(&o);
    ASSERT(o.joined && o.report.restart_batches == 1u, "one batch");
    ASSERT(failing.started == 2u && sibling.started == 2u, "both restarted");
    c = completion_of(&o, 1);
    ASSERT(c != NULL && c->generation.number == 2u && c->outcome == ASX_OUTCOME_OK,
           "the sibling's second generation ended ok");
}

static void test_rest_for_one_restarts_the_later_ones(void) {
    owner o;
    behaviour earlier;
    behaviour failing;
    behaviour later;
    setup();
    memset(&earlier, 0, sizeof(earlier));
    memset(&failing, 0, sizeof(failing));
    memset(&later, 0, sizeof(later));
    earlier.yields = 6;
    later.yields = 6;
    failing.result = ASX_E_INVALID_STATE;
    failing.fail_below = 2;
    failing.later = ASX_OK;
    owner_init(&o, 2);
    o.cfg.strategy = ASX_SUPERVISOR_REST_FOR_ONE;
    add_child(&o, "earlier", ASX_CHILD_TRANSIENT, &earlier);
    add_child(&o, "failing", ASX_CHILD_TRANSIENT, &failing);
    add_child(&o, "later", ASX_CHILD_TRANSIENT, &later);
    run_owner(&o);
    ASSERT(o.joined && o.report.restart_batches == 1u, "one batch");
    ASSERT(earlier.started == 1u, "an earlier child is left running");
    ASSERT(failing.started == 2u && later.started == 2u, "the failed and later ones restart");
}

static void test_one_for_one_leaves_siblings_alone(void) {
    owner o;
    behaviour failing;
    behaviour sibling;
    setup();
    memset(&failing, 0, sizeof(failing));
    memset(&sibling, 0, sizeof(sibling));
    failing.result = ASX_E_INVALID_STATE;
    sibling.yields = 4;
    owner_init(&o, 1);
    add_child(&o, "failing", ASX_CHILD_TRANSIENT, &failing);
    add_child(&o, "sibling", ASX_CHILD_TRANSIENT, &sibling);
    run_owner(&o);
    ASSERT(o.joined && failing.started == 2u && sibling.started == 1u, "only the failed one");
}

/* ------------------------------------------------------------------ */
/* Cancellation, panics and dependencies                               */
/* ------------------------------------------------------------------ */

static void test_abort_drains_without_restarting(void) {
    owner o;
    behaviour b;
    const asx_supervisor_completion *c;
    setup();
    memset(&b, 0, sizeof(b));
    b.block = 1;
    owner_init(&o, 3);
    o.abort_after = 3;
    add_child(&o, "worker", ASX_CHILD_PERMANENT, &b);
    run_owner(&o);
    ASSERT(o.joined && o.join_st == ASX_OK, "the join waits for the drain");
    ASSERT(b.started == 1u && o.report.restart_batches == 0u, "a drained child is not replaced");
    ASSERT(o.report.outcome == ASX_OUTCOME_CANCELLED && o.report.cancel_reason.message != NULL &&
               strcmp(o.report.cancel_reason.message, "abort") == 0,
           "cancelled with the abort's reason");
    c = completion_of(&o, 0);
    ASSERT(c != NULL && c->shutdown_requested_before_completion, "it was asked to stop");
}

static void test_a_panic_in_the_body_is_caught(void) {
    owner o;
    behaviour b;
    const asx_supervisor_completion *c;
    setup();
    memset(&b, 0, sizeof(b));
    b.panic = 1;
    b.yields = 1;
    owner_init(&o, 1);
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.joined && b.started == 2u, "a panic is a transient failure");
    c = completion_of(&o, 0);
    ASSERT(c != NULL && c->outcome == ASX_OUTCOME_PANICKED && c->panic_message != NULL &&
               strcmp(c->panic_message, "boom") == 0,
           "the panic and its message");
    ASSERT(c->task_outcome == ASX_OUTCOME_OK, "the generation's task completed normally");
}

static void test_a_failing_start_function_is_a_panic(void) {
    owner o;
    behaviour b;
    const asx_supervisor_completion *c;
    setup();
    memset(&b, 0, sizeof(b));
    b.fail_start = 1;
    owner_init(&o, 0);
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.joined && o.report.started == 1u, "it counts as started");
    c = completion_of(&o, 0);
    ASSERT(c != NULL && c->outcome == ASX_OUTCOME_PANICKED, "panicked");
}

static void test_restart_without_its_dependency_fails(void) {
    owner o;
    behaviour dependency;
    behaviour dependent;
    setup();
    memset(&dependency, 0, sizeof(dependency));
    memset(&dependent, 0, sizeof(dependent));
    dependency.result = ASX_OK; /* temporary: stays stopped */
    dependent.yields = 4;
    dependent.result = ASX_E_INVALID_STATE;
    owner_init(&o, 3);
    add_child(&o, "dependency", ASX_CHILD_TEMPORARY, &dependency);
    add_child(&o, "dependent", ASX_CHILD_TRANSIENT, &dependent);
    MUST_OK(asx_child_spec_depends_on(&o.specs[1], 0));
    run_owner(&o);
    ASSERT(o.joined && o.report.outcome == ASX_OUTCOME_ERR &&
               o.report.error == ASX_SUPERVISOR_ERR_DEPENDENCY_UNAVAILABLE &&
               o.report.error_child == 1u,
           "DependencyUnavailable for the required dependent");
    ASSERT(dependent.started == 1u, "not restarted");
}

static void test_backoff_delays_the_restart(void) {
    owner o;
    behaviour b;
    setup();
    memset(&b, 0, sizeof(b));
    b.result = ASX_E_INVALID_STATE;
    owner_init(&o, 2);
    o.cfg.backoff.kind = ASX_RESTART_BACKOFF_EXPONENTIAL;
    o.cfg.backoff.initial_ns = 1000u;
    o.cfg.backoff.max_ns = 1500u;
    o.cfg.backoff.multiplier = 2u;
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.joined && b.started == 3u, "two restarts");
    ASSERT(b.started_at[1] - b.started_at[0] >= 1000u, "the first waits the initial delay");
    ASSERT(b.started_at[2] - b.started_at[1] >= 1500u && b.started_at[2] - b.started_at[1] < 2000u,
           "the second waits the doubled delay, capped");
}

/* ------------------------------------------------------------------ */
/* Handles                                                             */
/* ------------------------------------------------------------------ */

static void test_handles(void) {
    owner o;
    behaviour b;
    asx_supervisor_handle forged;
    asx_supervisor_report r;
    setup();
    memset(&b, 0, sizeof(b));
    b.result = ASX_OK;
    owner_init(&o, 0);
    add_child(&o, "worker", ASX_CHILD_TRANSIENT, &b);
    run_owner(&o);
    ASSERT(o.joined, "joined");
    ASSERT(asx_supervisor_region(o.h) != ASX_INVALID_ID, "its region");
    ASSERT(asx_supervisor_task(o.h) != ASX_INVALID_ID, "its controller");
    ASSERT(asx_supervisor_join(o.h, asx_supervisor_task(o.h), &r) == ASX_E_INVALID_STATE,
           "a second join");
    forged = o.h;
    forged.generation++;
    ASSERT(asx_supervisor_abort(forged) == ASX_E_INVALID_ARGUMENT, "a forged handle");
    ASSERT(!asx_supervisor_is_alive(forged) && !asx_supervisor_finished(forged), "unknown");
    asx_supervisor_reset();
    ASSERT(asx_supervisor_region(o.h) == ASX_INVALID_ID, "reset forgets every supervisor");
}

int main(void) {
    printf("test_supervisor:\n");

    RUN(test_config_defaults);
    RUN(test_spawn_rejects_bad_arguments);
    RUN(test_spawn_compiles_the_topology);
    RUN(test_dependencies_order_the_start);

    RUN(test_transient_child_restarts_until_the_limit);
    RUN(test_transient_child_that_succeeds_stays_stopped);
    RUN(test_permanent_child_restarts_after_success);
    RUN(test_temporary_child_never_restarts);
    RUN(test_max_restarts_zero_refuses_at_once);
    RUN(test_reset_counter_keeps_restarting);
    RUN(test_restarts_age_out_of_the_window);
    RUN(test_cancelled_transient_child_is_not_restarted);
    RUN(test_escalate_cancels_the_owner_region);

    RUN(test_one_for_all_restarts_the_siblings);
    RUN(test_rest_for_one_restarts_the_later_ones);
    RUN(test_one_for_one_leaves_siblings_alone);

    RUN(test_abort_drains_without_restarting);
    RUN(test_a_panic_in_the_body_is_caught);
    RUN(test_a_failing_start_function_is_a_panic);
    RUN(test_restart_without_its_dependency_fails);
    RUN(test_backoff_delays_the_restart);

    RUN(test_handles);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
