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

int main(void) {
    fprintf(stderr, "=== test_interpreter_ops ===\n");
    RUN_TEST(the_interval_delay_queue_and_watch_steps_are_dispatched);
    RUN_TEST(an_unbounded_channel_runs_where_the_profile_allows_it);
    RUN_TEST(a_skip_needs_a_declared_requirement_the_profile_forbids);
    RUN_TEST(an_err_return_is_a_normal_exit_for_monitors_and_links);
    RUN_TEST(an_unknown_step_fails_the_run_closed);
    TEST_REPORT();
    return test_failures;
}
