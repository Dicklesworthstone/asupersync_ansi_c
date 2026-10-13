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
    RUN_TEST(an_unknown_step_fails_the_run_closed);
    TEST_REPORT();
    return test_failures;
}
