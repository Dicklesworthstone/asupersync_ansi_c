/*
 * test_interpreter_ops.c — the interpreter dispatches every step family
 * (bd-9kll.7.7, bd-9kll.5.11).
 *
 * A step the dispatcher never reaches fails its scenario closed ("op ... is
 * not interpreted"), which only a Rust-vs-C compare would show; twice a
 * stale blob moved the interval / DelayQueue hook into the body of another
 * branch and the sources still compiled. This runs one small scenario that
 * uses each new family through the interpreter and counts its observations:
 * a step that is not dispatched ends its task with an error instead.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include "conformance/interpreter.h"

#include <string.h>

static asx_json_doc g_in;
static asx_json_doc g_out;
static char g_error[512];

/* Run the scenario in `text`; returns the interpreter's status and leaves
 * the projection in g_out under *root. */
static asx_status run_text(const char *text, uint32_t *root) {
    uint32_t scenario = 0;
    asx_status st;
    asx_json_doc_init(&g_in);
    asx_json_doc_init(&g_out);
    g_error[0] = '\0';
    st = asx_json_parse(&g_in, text, strlen(text), &scenario);
    if (st != ASX_OK) return st;
    return asx_conformance_run(&g_in, scenario, &g_out, root, g_error, sizeof(g_error));
}

/* Parse `text` into g_in, the scenario object's node in *root. */
static asx_status parse_only(const char *text, uint32_t *root) {
    asx_json_doc_init(&g_in);
    return asx_json_parse(&g_in, text, strlen(text), root);
}

#define SCENARIO_HEAD                                                                              \
    "{\"schema\":\"asx.scenario.v2\",\"id\":\"unit-ops-001\",\"description\":\"d\","               \
    "\"unit\":\"timers\",\"rules\":[\"inv.determinism.replayable\"],\"seed\":42,"                  \
    "\"lab\":{\"max_steps\":600},"                                                                 \
    "\"regions\":[{\"name\":\"r.main\",\"parent\":\"root\",\"budget\":null}],"

/* The interval and DelayQueue steps (DSL 3.9) and the watch steps (3.6): 13
 * steps in t.a and 6 in t.b, each observed once. */
TEST(the_interval_delay_queue_and_watch_steps_are_dispatched) {
    static const char text[] = SCENARIO_HEAD
        "\"channels\":[{\"name\":\"ch.w\",\"type\":\"watch\",\"initial\":1,\"sender\":\"t.a\","
        "\"subscribers\":[\"t.b\"]}],\"sync\":[],"
        "\"tasks\":[{\"name\":\"t.a\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"interval_new\",\"as\":\"iv\",\"period_ns\":100},"
        "{\"op\":\"interval_tick\",\"interval\":\"iv\"},"
        "{\"op\":\"delay_queue_new\",\"as\":\"q\"},"
        "{\"op\":\"delay_queue_insert\",\"queue\":\"q\",\"as\":\"k\",\"value\":7,\"timeout_ns\":10}"
        ","
        "{\"op\":\"delay_queue_len\",\"queue\":\"q\"},"
        "{\"op\":\"delay_queue_contains\",\"queue\":\"q\",\"key\":\"k\"},"
        "{\"op\":\"delay_queue_deadline\",\"queue\":\"q\",\"key\":\"k\"},"
        "{\"op\":\"delay_queue_reset\",\"queue\":\"q\",\"key\":\"k\",\"timeout_ns\":20},"
        "{\"op\":\"sleep\",\"ns\":30},"
        "{\"op\":\"delay_queue_poll\",\"queue\":\"q\"},"
        "{\"op\":\"delay_queue_clear\",\"queue\":\"q\"},"
        "{\"op\":\"watch_send_modify\",\"channel\":\"ch.w\",\"add\":2},"
        "{\"op\":\"watch_receiver_count\",\"channel\":\"ch.w\"}]},"
        "{\"name\":\"t.b\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"watch_borrow\",\"channel\":\"ch.w\"},"
        "{\"op\":\"watch_has_changed\",\"channel\":\"ch.w\"},"
        "{\"op\":\"watch_clone\",\"channel\":\"ch.w\",\"as\":\"c\"},"
        "{\"op\":\"watch_mark_seen\",\"channel\":\"ch.w\",\"receiver\":\"c\"},"
        "{\"op\":\"watch_is_closed\",\"channel\":\"ch.w\",\"end\":\"receiver\"},"
        "{\"op\":\"watch_wait_for\",\"channel\":\"ch.w\",\"cmp\":\"ge\",\"value\":0}]}],"
        "\"script\":[],\"expect\":{\"kind\":\"ok\"}}";
    uint32_t root = 0;
    uint32_t obs;
    ASSERT_EQ(run_text(text, &root), ASX_OK);
    obs = asx_json_get(&g_out, root, "observations");
    ASSERT_EQ(asx_json_count(&g_out, obs), 19u);
}

/* An unbounded mpsc channel (DSL 3.6, DEC-008): seven sends and no consumer
 * running, none waiting for capacity. A profile that forbids unbounded
 * queues refuses the channel instead, which the run reports closed; the
 * scenario declares what it needs, so the runner skips it there. */
TEST(an_unbounded_channel_runs_where_the_profile_allows_it) {
    static const char text[] = SCENARIO_HEAD
        "\"requires\":[\"unbounded_queue\"],"
        "\"channels\":[{\"name\":\"ch.q\",\"type\":\"mpsc\",\"unbounded\":true,"
        "\"sender\":\"t.p\",\"receiver\":\"t.c\"}],\"sync\":[],"
        "\"tasks\":[{\"name\":\"t.p\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"send\",\"channel\":\"ch.q\",\"value\":1},"
        "{\"op\":\"send\",\"channel\":\"ch.q\",\"value\":2},"
        "{\"op\":\"send\",\"channel\":\"ch.q\",\"value\":3},"
        "{\"op\":\"send\",\"channel\":\"ch.q\",\"value\":4},"
        "{\"op\":\"send\",\"channel\":\"ch.q\",\"value\":5},"
        "{\"op\":\"send\",\"channel\":\"ch.q\",\"value\":6},"
        "{\"op\":\"close_sender\",\"channel\":\"ch.q\"}]},"
        "{\"name\":\"t.c\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"sleep\",\"ns\":100},"
        "{\"op\":\"recv\",\"channel\":\"ch.q\"},{\"op\":\"recv\",\"channel\":\"ch.q\"},"
        "{\"op\":\"recv\",\"channel\":\"ch.q\"},{\"op\":\"recv\",\"channel\":\"ch.q\"},"
        "{\"op\":\"recv\",\"channel\":\"ch.q\"},{\"op\":\"recv\",\"channel\":\"ch.q\"},"
        "{\"op\":\"recv\",\"channel\":\"ch.q\"}]}],"
        "\"script\":[],\"expect\":{\"kind\":\"ok\"}}";
    uint32_t scenario = 0;
    uint32_t root = 0;
    const char *name = NULL;
    int forbidden;
    asx_status st;

    /* The runner's own question, asked first: does this build's profile
     * forbid what the scenario declares? */
    ASSERT_EQ(parse_only(text, &scenario), ASX_OK);
    forbidden = asx_conformance_requirement_forbidden(&g_in, scenario, asx_profile_active(), &name);
    ASSERT_TRUE(forbidden == 0 || forbidden == 1);
    st = run_text(text, &root);
    if (forbidden == 1) {
        ASSERT_TRUE(st != ASX_OK);
        ASSERT_TRUE(strstr(g_error, "forbids this channel") != NULL);
        return;
    }
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_json_count(&g_out, asx_json_get(&g_out, root, "observations")), 15u);
}

/* What the runner skips: a requirement the scenario declares and the
 * profile's overload catalog forbids (ASX_FORBID_UNBOUNDED_QUEUE), nothing
 * else. */
TEST(a_skip_needs_a_declared_requirement_the_profile_forbids) {
    static const char declared[] = "{\"requires\":[\"unbounded_queue\"]}";
    static const char undeclared[] = "{\"lab\":{\"max_steps\":1}}";
    static const char empty[] = "{\"requires\":[]}";
    static const char unknown[] = "{\"requires\":[\"warp_drive\"]}";
    static const char not_a_list[] = "{\"requires\":\"unbounded_queue\"}";
    static const asx_profile_id forbids[] = {ASX_PROFILE_ID_FREESTANDING,
                                             ASX_PROFILE_ID_EMBEDDED_ROUTER, ASX_PROFILE_ID_HFT,
                                             ASX_PROFILE_ID_BROWSER};
    static const asx_profile_id allows[] = {ASX_PROFILE_ID_CORE, ASX_PROFILE_ID_POSIX,
                                            ASX_PROFILE_ID_WIN32, ASX_PROFILE_ID_AUTOMOTIVE,
                                            ASX_PROFILE_ID_PARALLEL};
    uint32_t root = 0;
    const char *name = NULL;
    size_t i;

    for (i = 0; i < sizeof(forbids) / sizeof(forbids[0]); i++) {
        ASSERT_EQ(parse_only(declared, &root), ASX_OK);
        ASSERT_EQ(asx_conformance_requirement_forbidden(&g_in, root, forbids[i], &name), 1);
        ASSERT_TRUE(name != NULL);
        ASSERT_STR_EQ(name, "unbounded_queue");
        /* A profile that forbids it, a scenario that does not declare it:
         * not skipped, so the refused channel fails the run. */
        ASSERT_EQ(parse_only(undeclared, &root), ASX_OK);
        ASSERT_EQ(asx_conformance_requirement_forbidden(&g_in, root, forbids[i], &name), 0);
        ASSERT_TRUE(name == NULL);
        ASSERT_EQ(parse_only(empty, &root), ASX_OK);
        ASSERT_EQ(asx_conformance_requirement_forbidden(&g_in, root, forbids[i], &name), 0);
    }
    for (i = 0; i < sizeof(allows) / sizeof(allows[0]); i++) {
        /* A declared requirement the profile allows must run. */
        ASSERT_EQ(parse_only(declared, &root), ASX_OK);
        ASSERT_EQ(asx_conformance_requirement_forbidden(&g_in, root, allows[i], &name), 0);
        ASSERT_TRUE(name == NULL);
    }
    /* An unknown or malformed declaration is an error in every profile. */
    for (i = 0; i < (size_t)ASX_PROFILE_ID_COUNT; i++) {
        ASSERT_EQ(parse_only(unknown, &root), ASX_OK);
        ASSERT_EQ(asx_conformance_requirement_forbidden(&g_in, root, (asx_profile_id)i, &name), -1);
        ASSERT_TRUE(name == NULL);
        ASSERT_EQ(parse_only(not_a_list, &root), ASX_OK);
        ASSERT_EQ(asx_conformance_requirement_forbidden(&g_in, root, (asx_profile_id)i, &name), -1);
    }
}

/* The monitor and link steps (DSL 3.10, 3.11), and what an `err` return is:
 * the body's value, so the task exits normally. Its monitor and its
 * trapping link hear `normal`, a propagating link does not cancel the
 * holder, and the task's outcome still reads err. Nine observations: 2 in
 * t.peer, 2 in t.mon, 3 in t.trap and 2 in t.prop. */
TEST(an_err_return_is_a_normal_exit_for_monitors_and_links) {
    static const char text[] = SCENARIO_HEAD
        "\"channels\":[],\"sync\":[],"
        "\"tasks\":[{\"name\":\"t.peer\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"sleep\",\"ns\":50},"
        "{\"op\":\"return\",\"outcome\":{\"tag\":\"err\",\"status\":\"ASX_E_INVALID_STATE\"}}]},"
        "{\"name\":\"t.mon\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"monitor\",\"task\":\"t.peer\",\"as\":\"m\"},"
        "{\"op\":\"await_down\",\"monitor\":\"m\"}]},"
        "{\"name\":\"t.trap\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"link_trapping\",\"task\":\"t.peer\",\"as\":\"l\"},"
        "{\"op\":\"await_exit\",\"link\":\"l\"},"
        "{\"op\":\"unlink\",\"link\":\"l\"}]},"
        "{\"name\":\"t.prop\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"link\",\"task\":\"t.peer\",\"as\":\"l\"},"
        "{\"op\":\"sleep\",\"ns\":200}]}],"
        "\"script\":[],\"expect\":{\"kind\":\"ok\"}}";
    uint32_t root = 0;
    uint32_t obs;
    uint32_t i;
    uint32_t heard = 0;
    ASSERT_EQ(run_text(text, &root), ASX_OK);
    obs = asx_json_get(&g_out, root, "observations");
    ASSERT_EQ(asx_json_count(&g_out, obs), 9u);
    for (i = 0; i < 9u; i++) {
        uint32_t o = asx_json_item(&g_out, obs, i);
        const char *op = asx_json_get_string(&g_out, o, "op");
        const char *task = asx_json_get_string(&g_out, o, "task");
        ASSERT_TRUE(op != NULL && task != NULL);
        if (strcmp(op, "await_down") == 0 || strcmp(op, "await_exit") == 0) {
            uint32_t value = asx_json_get(&g_out, o, "value");
            ASSERT_STR_EQ(asx_json_get_string(&g_out, value, "reason"), "normal");
            heard++;
        }
        if (strcmp(task, "t.prop") == 0 && strcmp(op, "sleep") == 0) {
            ASSERT_STR_EQ(asx_json_get_string(&g_out, o, "status"), "ASX_OK");
        }
    }
    ASSERT_EQ(heard, 2u);
}

/* ---- Futurelock scenarios (DSL 1 and 5, bd-9kll.9.6) ---- */

#define FUTURELOCK_HEAD(threshold, expect)                                                         \
    "{\"schema\":\"asx.scenario.v2\",\"id\":\"unit-futurelock-001\","                              \
    "\"description\":\"threshold " #threshold ", expecting " expect "\","                          \
    "\"unit\":\"obligation\",\"rules\":[\"inv.determinism.replayable\"],\"seed\":42,"              \
    "\"lab\":{\"max_steps\":200,\"futurelock_max_idle_steps\":" #threshold "},"                    \
    "\"regions\":[{\"name\":\"r.main\",\"parent\":\"root\",\"budget\":null}],"                     \
    "\"channels\":[],\"sync\":[{\"name\":\"n\",\"type\":\"notify\"}],"

#define FUTURELOCK_TAIL(expect) "\"script\":[],\"expect\":{\"kind\":\"" expect "\"}}"

/* The run's futurelock.detected events: how many (all of them), and the
 * first `cap` idle_steps, sorted ascending. */
static uint32_t futurelock_events(uint32_t root, uint64_t *idle, uint32_t cap) {
    uint32_t trace = asx_json_get(&g_out, root, "trace");
    uint32_t n = 0;
    uint32_t l;
    uint32_t e;
    uint32_t i;
    uint32_t j;
    for (l = 0; l < asx_json_count(&g_out, trace); l++) {
        uint32_t layer = asx_json_item(&g_out, trace, l);
        for (e = 0; e < asx_json_count(&g_out, layer); e++) {
            uint32_t ev = asx_json_item(&g_out, layer, e);
            const char *k = asx_json_get_string(&g_out, ev, "k");
            if (k != NULL && strcmp(k, "futurelock.detected") == 0) {
                uint64_t v = 0;
                if (n < cap) {
                    (void)asx_json_u64(&g_out, asx_json_get(&g_out, ev, "idle_steps"), &v);
                    idle[n] = v;
                }
                n++;
            }
        }
    }
    for (i = 1; i < n && i < cap; i++) {
        for (j = i; j > 0 && idle[j - 1u] > idle[j]; j--) {
            uint64_t t = idle[j];
            idle[j] = idle[j - 1u];
            idle[j - 1u] = t;
        }
    }
    return n;
}

/* The deliberate futurelock: t.holder reserves a Lease and waits on a notify
 * nobody signals until t.driver has yielded eight times; it is not polled for
 * that long, so Rust's check (threshold 3) reports it at every step from its
 * fourth idle step, and it commits and completes once woken. The report
 * names the task, its region and the Lease, and the counts run on by one. */
TEST(a_holder_idle_past_the_threshold_is_reported_at_every_step) {
    static const char text[] = FUTURELOCK_HEAD(
        3,
        "futurelock") "\"tasks\":[{\"name\":\"t.holder\",\"region\":\"r.main\",\"budget\":null,"
                      "\"program\":["
                      "{\"op\":\"reserve\",\"kind\":\"Lease\",\"as\":\"o.1\"},"
                      "{\"op\":\"notify_wait\",\"notify\":\"n\"},"
                      "{\"op\":\"commit\",\"obligation\":\"o.1\"}]},"
                      "{\"name\":\"t.driver\",\"region\":\"r.main\",\"budget\":null,\"program\":["
                      "{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},"
                      "{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},"
                      "{\"op\":\"notify_one\",\"notify\":\"n\"}]}]," FUTURELOCK_TAIL("futurelock");
    uint32_t root = 0;
    uint64_t idle[32];
    uint32_t n;
    uint32_t i;
    uint32_t trace;
    uint32_t first = ASX_JSON_NONE;
    uint32_t l;
    uint32_t e;
    ASSERT_EQ(run_text(text, &root), ASX_OK);
    memset(idle, 0, sizeof(idle));
    n = futurelock_events(root, idle, 32u);
    ASSERT_TRUE(n >= 3u && n <= 32u);
    ASSERT_EQ(idle[0], (uint64_t)4u); /* threshold 3: the first count over it */
    for (i = 1; i < n; i++) ASSERT_EQ(idle[i], idle[i - 1u] + 1u);
    /* One report, in full. */
    trace = asx_json_get(&g_out, root, "trace");
    for (l = 0; l < asx_json_count(&g_out, trace) && first == ASX_JSON_NONE; l++) {
        uint32_t layer = asx_json_item(&g_out, trace, l);
        for (e = 0; e < asx_json_count(&g_out, layer) && first == ASX_JSON_NONE; e++) {
            uint32_t ev = asx_json_item(&g_out, layer, e);
            const char *k = asx_json_get_string(&g_out, ev, "k");
            if (k != NULL && strcmp(k, "futurelock.detected") == 0) first = ev;
        }
    }
    ASSERT_TRUE(first != ASX_JSON_NONE);
    ASSERT_STR_EQ(asx_json_get_string(&g_out, first, "task"), "t.holder");
    ASSERT_STR_EQ(asx_json_get_string(&g_out, first, "region"), "r.main");
    {
        uint32_t held = asx_json_get(&g_out, first, "held");
        uint32_t item;
        ASSERT_EQ(asx_json_count(&g_out, held), 1u);
        item = asx_json_item(&g_out, held, 0);
        ASSERT_STR_EQ(asx_json_get_string(&g_out, item, "obligation"), "t.holder/o1");
        ASSERT_STR_EQ(asx_json_get_string(&g_out, item, "kind"), "Lease");
    }
}

/* Negative: a lone holder is polled at every step (it yields), so it is never
 * idle past a threshold of 1; and a task that is idle but holds nothing is
 * not a futurelock. Both runs report nothing and finish ok. */
TEST(a_polled_holder_and_an_idle_task_without_an_obligation_are_not_reported) {
    static const char polled[] = FUTURELOCK_HEAD(
        1, "ok") "\"tasks\":[{\"name\":\"t.holder\",\"region\":\"r.main\",\"budget\":null,"
                 "\"program\":["
                 "{\"op\":\"reserve\",\"kind\":\"Lease\",\"as\":\"o.1\"},"
                 "{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},"
                 "{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"commit\",\"obligation\":\"o.1\"}]"
                 "}]," FUTURELOCK_TAIL("ok");
    static const char idle_only[] = FUTURELOCK_HEAD(
        3,
        "ok") "\"tasks\":[{\"name\":\"t.idle\",\"region\":\"r.main\",\"budget\":null,\"program\":["
              "{\"op\":\"notify_wait\",\"notify\":\"n\"}]},"
              "{\"name\":\"t.driver\",\"region\":\"r.main\",\"budget\":null,\"program\":["
              "{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},"
              "{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},"
              "{\"op\":\"notify_one\",\"notify\":\"n\"}]}]," FUTURELOCK_TAIL("ok");
    uint32_t root = 0;
    uint64_t idle[4];
    ASSERT_EQ(run_text(polled, &root), ASX_OK);
    ASSERT_EQ(futurelock_events(root, idle, 4u), 0u);
    ASSERT_EQ(run_text(idle_only, &root), ASX_OK);
    ASSERT_EQ(futurelock_events(root, idle, 4u), 0u);
}

/* The expectation is checked in both directions: `futurelock` needs the
 * knob and a report, and `ok` with the knob needs none. */
TEST(a_futurelock_expectation_is_checked_both_ways) {
    static const char no_knob[] = SCENARIO_HEAD
        "\"channels\":[],\"sync\":[],"
        "\"tasks\":[{\"name\":\"t.a\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"yield\"}]}],"
        "\"script\":[],\"expect\":{\"kind\":\"futurelock\"}}";
    static const char none_reported[] =
        FUTURELOCK_HEAD(3, "futurelock") "\"tasks\":[{\"name\":\"t.a\",\"region\":\"r.main\","
                                         "\"budget\":null,\"program\":["
                                         "{\"op\":\"yield\"}]}]," FUTURELOCK_TAIL("futurelock");
    static const char unexpected[] = FUTURELOCK_HEAD(
        3, "ok") "\"tasks\":[{\"name\":\"t.holder\",\"region\":\"r.main\",\"budget\":null,"
                 "\"program\":["
                 "{\"op\":\"reserve\",\"kind\":\"Lease\",\"as\":\"o.1\"},"
                 "{\"op\":\"notify_wait\",\"notify\":\"n\"},"
                 "{\"op\":\"commit\",\"obligation\":\"o.1\"}]},"
                 "{\"name\":\"t.driver\",\"region\":\"r.main\",\"budget\":null,\"program\":["
                 "{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"},"
                 "{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"notify_one\",\"notify\":\"n\"}]}]"
                 "," FUTURELOCK_TAIL("ok");
    uint32_t root = 0;
    ASSERT_TRUE(run_text(no_knob, &root) != ASX_OK);
    ASSERT_TRUE(strstr(g_error, "lab.futurelock_max_idle_steps") != NULL);
    ASSERT_TRUE(run_text(none_reported, &root) != ASX_OK);
    ASSERT_TRUE(strstr(g_error, "none reported") != NULL);
    ASSERT_TRUE(run_text(unexpected, &root) != ASX_OK);
    ASSERT_TRUE(strstr(g_error, "expects ok") != NULL);
}

/* Kernel-state oracles (DSL 1 lab.oracles, bd-9kll.9.2): the scenario's list
 * and the process-wide list both run once when the run has ended. A healthy
 * scenario passes them all, and a name that is not an oracle fails the run
 * (the schema's enum refuses it; the interpreter does not rely on that). */
#define ORACLE_SCENARIO(oracles)                                                                   \
    "{\"schema\":\"asx.scenario.v2\",\"id\":\"unit-oracles-001\",\"description\":\"d\","           \
    "\"unit\":\"quiescence\",\"rules\":[\"inv.determinism.replayable\"],\"seed\":42,"              \
    "\"lab\":{\"max_steps\":200" oracles "},"                                                      \
    "\"regions\":[{\"name\":\"r.main\",\"parent\":\"root\",\"budget\":null}],"                     \
    "\"channels\":[],\"sync\":[],"                                                                 \
    "\"tasks\":[{\"name\":\"t.a\",\"region\":\"r.main\",\"budget\":null,\"program\":["             \
    "{\"op\":\"yield\"},{\"op\":\"yield\"}]}],"                                                    \
    "\"script\":[],\"expect\":{\"kind\":\"ok\"}}"

TEST(declared_kernel_oracles_run_at_the_end_and_pass_on_a_healthy_scenario) {
    static const char all[] = ORACLE_SCENARIO(",\"oracles\":[\"all\"]");
    static const char some[] =
        ORACLE_SCENARIO(",\"oracles\":[\"region_tree\",\"quiescence\",\"deadline_monotone\"]");
    static const char bogus[] = ORACLE_SCENARIO(",\"oracles\":[\"no_such_oracle\"]");
    uint32_t root = 0;
    ASSERT_EQ(run_text(all, &root), ASX_OK);
    ASSERT_EQ(run_text(some, &root), ASX_OK);
    ASSERT_TRUE(run_text(bogus, &root) != ASX_OK);
    ASSERT_TRUE(strstr(g_error, "no_such_oracle") != NULL);
    ASSERT_TRUE(strstr(g_error, "unknown oracle") != NULL);
}

TEST(the_process_wide_oracle_list_runs_for_a_scenario_that_declares_none) {
    static const char plain[] = ORACLE_SCENARIO("");
    uint32_t root = 0;
    asx_conformance_set_oracles("all");
    ASSERT_EQ(run_text(plain, &root), ASX_OK);
    asx_conformance_set_oracles("task_leak,no_such_oracle");
    ASSERT_TRUE(run_text(plain, &root) != ASX_OK);
    ASSERT_TRUE(strstr(g_error, "no_such_oracle") != NULL);
    asx_conformance_set_oracles(NULL);
    ASSERT_EQ(run_text(plain, &root), ASX_OK);
}

/* A step the interpreter has no handler for still fails closed, so the
 * count above means something. */
TEST(an_unknown_step_fails_the_run_closed) {
    static const char text[] = SCENARIO_HEAD
        "\"channels\":[],\"sync\":[],"
        "\"tasks\":[{\"name\":\"t.a\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"delay_queue_frobnicate\",\"queue\":\"q\"}]}],"
        "\"script\":[],\"expect\":{\"kind\":\"ok\"}}";
    uint32_t root = 0;
    ASSERT_TRUE(run_text(text, &root) != ASX_OK);
    ASSERT_TRUE(g_error[0] != '\0');
}

/* The observation of task `task` at `step` with op `op` in g_out's
 * observations, or ASX_JSON_NONE. */
static uint32_t find_observation(uint32_t root, const char *task, uint32_t step, const char *op) {
    uint32_t obs = asx_json_get(&g_out, root, "observations");
    uint32_t i;
    for (i = 0; i < asx_json_count(&g_out, obs); i++) {
        uint32_t o = asx_json_item(&g_out, obs, i);
        const char *t = asx_json_get_string(&g_out, o, "task");
        const char *p = asx_json_get_string(&g_out, o, "op");
        uint64_t s = 0;
        (void)asx_json_u64(&g_out, asx_json_get(&g_out, o, "step"), &s);
        if (t != NULL && p != NULL && strcmp(t, task) == 0 && strcmp(p, op) == 0 && s == step) {
            return o;
        }
    }
    return ASX_JSON_NONE;
}

/* A sender closed while a permit of its channel is held (DSL 3.6): Rust's
 * permit borrows the sender, so the drop is projected after the permit. The
 * receiver's first recv gets the value the permit sends and only its second
 * sees the channel closed; the close step itself is observed ok. */
TEST(a_sender_closed_with_a_permit_held_is_dropped_after_the_permit) {
    static const char text[] = SCENARIO_HEAD
        "\"channels\":[{\"name\":\"ch.q\",\"type\":\"mpsc\",\"capacity\":2,\"sender\":\"t.p\","
        "\"receiver\":\"t.r\"}],\"sync\":[],"
        "\"tasks\":[{\"name\":\"t.p\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"reserve_send\",\"channel\":\"ch.q\",\"as\":\"p\"},"
        "{\"op\":\"close_sender\",\"channel\":\"ch.q\"},"
        "{\"op\":\"sleep\",\"ns\":100},"
        "{\"op\":\"permit_send\",\"permit\":\"p\",\"value\":5}]},"
        "{\"name\":\"t.r\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"recv\",\"channel\":\"ch.q\"},{\"op\":\"recv\",\"channel\":\"ch.q\"}]}],"
        "\"script\":[],\"expect\":{\"kind\":\"ok\"}}";
    uint32_t root = 0;
    uint32_t o;
    uint64_t v = 0;
    ASSERT_EQ(run_text(text, &root), ASX_OK);
    o = find_observation(root, "t.p", 2, "close_sender");
    ASSERT_TRUE(o != ASX_JSON_NONE);
    ASSERT_STR_EQ(asx_json_get_string(&g_out, o, "status"), "ASX_OK");
    o = find_observation(root, "t.r", 1, "recv");
    ASSERT_TRUE(o != ASX_JSON_NONE);
    ASSERT_STR_EQ(asx_json_get_string(&g_out, o, "status"), "ASX_OK");
    ASSERT_TRUE(asx_json_u64(&g_out, asx_json_get(&g_out, o, "value"), &v));
    ASSERT_EQ(v, (uint64_t)5u);
    o = find_observation(root, "t.r", 2, "recv");
    ASSERT_TRUE(o != ASX_JSON_NONE);
    ASSERT_STR_EQ(asx_json_get_string(&g_out, o, "status"), "ASX_E_DISCONNECTED");
}

/* The mask bound (DSL 3.2): depth 64 nests 64 masks and runs its steps, one
 * level more panics the task with Rust's assertion message before any step
 * of the section runs, so it has no observation. */
TEST(a_mask_beyond_the_bound_panics_the_task_and_depth_64_runs) {
    static const char text[] = SCENARIO_HEAD
        "\"channels\":[],\"sync\":[],"
        "\"tasks\":[{\"name\":\"t.ok\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"masked\",\"depth\":64,\"steps\":[{\"op\":\"trace\",\"message\":\"in\"}]}]},"
        "{\"name\":\"t.over\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"masked\",\"depth\":65,\"steps\":[{\"op\":\"trace\",\"message\":\"in\"}]}]}],"
        "\"script\":[],\"expect\":{\"kind\":\"ok\"}}";
    uint32_t root = 0;
    uint32_t tasks;
    uint32_t outcome;
    const char *message;
    ASSERT_EQ(run_text(text, &root), ASX_OK);
    ASSERT_TRUE(find_observation(root, "t.ok", 1, "masked/1/trace") != ASX_JSON_NONE);
    ASSERT_TRUE(find_observation(root, "t.over", 1, "masked/1/trace") == ASX_JSON_NONE);
    tasks = asx_json_get(&g_out, asx_json_get(&g_out, root, "snapshot"), "tasks");
    outcome = asx_json_get(&g_out, asx_json_get(&g_out, tasks, "t.ok"), "outcome");
    ASSERT_STR_EQ(asx_json_get_string(&g_out, outcome, "tag"), "ok");
    outcome = asx_json_get(&g_out, asx_json_get(&g_out, tasks, "t.over"), "outcome");
    ASSERT_STR_EQ(asx_json_get_string(&g_out, outcome, "tag"), "panicked");
    message = asx_json_get_string(&g_out, outcome, "message");
    ASSERT_TRUE(message != NULL);
    ASSERT_TRUE(strncmp(message, "mask depth exceeded MAX_MASK_DEPTH (64)", 39u) == 0);
}

/* The driver's region_close_outcome (DSL 4): none yet is ASX_E_NOT_FOUND,
 * and after the close of a cancelled region it is Cancelled; observed under
 * the task name `driver` at the script entry's 1-based index. */
TEST(the_close_outcome_of_a_region_is_observed_under_the_driver) {
    static const char text[] = SCENARIO_HEAD
        "\"channels\":[],\"sync\":[],"
        "\"tasks\":[{\"name\":\"t.a\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"sleep\",\"ns\":1000},{\"op\":\"checkpoint\"}]}],"
        "\"script\":[{\"at_ns\":5,\"op\":{\"op\":\"region_close_outcome\",\"region\":\"r.main\"}},"
        "{\"at_ns\":50,\"op\":{\"op\":\"close_region\",\"region\":\"r.main\",\"kind\":\"User\"}},"
        "{\"at_ns\":60,\"op\":{\"op\":\"region_close_outcome\",\"region\":\"r.main\"}}],"
        "\"expect\":{\"kind\":\"ok\"}}";
    uint32_t root = 0;
    uint32_t o;
    ASSERT_EQ(run_text(text, &root), ASX_OK);
    o = find_observation(root, "driver", 1, "region_close_outcome");
    ASSERT_TRUE(o != ASX_JSON_NONE);
    ASSERT_STR_EQ(asx_json_get_string(&g_out, o, "status"), "ASX_E_NOT_FOUND");
    o = find_observation(root, "driver", 3, "region_close_outcome");
    ASSERT_TRUE(o != ASX_JSON_NONE);
    ASSERT_STR_EQ(asx_json_get_string(&g_out, o, "status"), "ASX_OK");
    ASSERT_STR_EQ(asx_json_get_string(&g_out, asx_json_get(&g_out, o, "value"), "tag"),
                  "cancelled");
}

int main(void) {
    fprintf(stderr, "=== test_interpreter_ops ===\n");
    RUN_TEST(a_sender_closed_with_a_permit_held_is_dropped_after_the_permit);
    RUN_TEST(a_mask_beyond_the_bound_panics_the_task_and_depth_64_runs);
    RUN_TEST(the_close_outcome_of_a_region_is_observed_under_the_driver);
    RUN_TEST(the_interval_delay_queue_and_watch_steps_are_dispatched);
    RUN_TEST(an_unbounded_channel_runs_where_the_profile_allows_it);
    RUN_TEST(a_skip_needs_a_declared_requirement_the_profile_forbids);
    RUN_TEST(an_err_return_is_a_normal_exit_for_monitors_and_links);
    RUN_TEST(an_unknown_step_fails_the_run_closed);
    RUN_TEST(a_holder_idle_past_the_threshold_is_reported_at_every_step);
    RUN_TEST(a_polled_holder_and_an_idle_task_without_an_obligation_are_not_reported);
    RUN_TEST(a_futurelock_expectation_is_checked_both_ways);
    RUN_TEST(declared_kernel_oracles_run_at_the_end_and_pass_on_a_healthy_scenario);
    RUN_TEST(the_process_wide_oracle_list_runs_for_a_scenario_that_declares_none);
    TEST_REPORT();
    return test_failures;
}
