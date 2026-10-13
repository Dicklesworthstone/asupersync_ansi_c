/*
 * test_monitor.c — unit tests for task monitors and DOWN notifications
 * (Rust src/monitor.rs).
 *
 * The runtime fires monitors when a task completes; these tests fire them
 * directly with asx_monitor_task_finished, which is that call. Rust parity
 * of the observable schedule is covered by the conformance fixtures.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/actor/actor.h>
#include <asx/core/budget.h>
#include <asx/monitor/monitor.h>
#include <asx/runtime/runtime.h>
#include <stdio.h>
#include <string.h>

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

/* g_root is run; g_idle never is, so a task spawned in it stays live until
 * a test finishes it by hand with asx_monitor_task_finished. */
static asx_region_id g_root;
static asx_region_id g_idle;

static void setup(void) {
    asx_runtime_reset();
    asx_monitor_reset();
    MUST_OK(asx_region_open(&g_root));
    MUST_OK(asx_region_open(&g_idle));
}

static void run(asx_region_id region) {
    asx_budget b = asx_budget_from_polls(1000);
    st_sink_ = asx_scheduler_run(region, &b);
    (void)st_sink_;
}

static asx_status finish_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static asx_status fail_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_E_INVALID_STATE;
}

/* A task that is never polled: created, so live. */
static asx_task_id spawn_task(void) {
    asx_task_id id = ASX_INVALID_ID;
    MUST_OK(asx_task_spawn(g_idle, finish_poll, NULL, &id));
    return id;
}

/* A task in the run region: it completes when the test runs it. */
static asx_task_id spawn_runnable(asx_task_poll_fn poll) {
    asx_task_id id = ASX_INVALID_ID;
    MUST_OK(asx_task_spawn(g_root, poll, NULL, &id));
    return id;
}

/* A task that ran to completion: not live. */
static asx_task_id spawn_finished_task(void) {
    asx_task_id id = spawn_runnable(finish_poll);
    run(g_root);
    return id;
}

/* ------------------------------------------------------------------ */
/* A server recording what handle_info is given                        */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_actor_info_msg got[8];
    uint32_t n;
} rec_state;

static asx_status rec_info(void *state, const asx_actor_info_msg *m, asx_actor_handle self) {
    rec_state *s = (rec_state *)state;
    (void)self;
    if (s->n < 8u) s->got[s->n] = *m;
    s->n++;
    return ASX_OK;
}

static const asx_actor_behavior g_rec = {
    NULL, NULL, NULL, NULL, NULL, NULL, ASX_ACTOR_OVERFLOW_REJECT, rec_info};

static asx_actor_handle spawn_server(rec_state *state) {
    asx_actor_handle h;
    memset(state, 0, sizeof(*state));
    h.slot = UINT32_MAX;
    h.generation = 0;
    MUST_OK(asx_actor_spawn(&h, g_root, &g_rec, state, 4));
    return h;
}

static asx_outcome ok_outcome(void) { return asx_outcome_make(ASX_OUTCOME_OK); }

/* ------------------------------------------------------------------ */
/* Establishing                                                        */
/* ------------------------------------------------------------------ */

static void test_references_start_at_one_and_increase(void) {
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref a = 0;
    asx_monitor_ref b = 0;
    setup();
    watcher = spawn_task();
    target = spawn_task();
    ASSERT(asx_monitor_establish(watcher, target, &a) == ASX_OK, "first monitor");
    ASSERT(asx_monitor_establish(watcher, target, &b) == ASX_OK, "second monitor");
    ASSERT(a == 1u && b == 2u, "references are allocated from 1 in order");
    ASSERT(asx_monitor_count() == 2u, "both exist");
}

static void test_a_target_must_be_live(void) {
    asx_task_id watcher;
    asx_task_id finished;
    asx_monitor_ref ref = 0;
    setup();
    watcher = spawn_task();
    finished = spawn_finished_task();
    ASSERT(asx_monitor_establish(watcher, finished, &ref) == ASX_E_NOT_FOUND,
           "a task that already finished is not live (Rust WatchError::NotFound)");
    ASSERT(asx_monitor_establish(watcher, ASX_INVALID_ID, &ref) == ASX_E_NOT_FOUND,
           "nor is one that never existed");
    ASSERT(asx_monitor_count() == 0u, "nothing was registered");
}

static void test_a_watcher_must_be_live(void) {
    asx_task_id finished;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    setup();
    finished = spawn_finished_task();
    target = spawn_task();
    ASSERT(asx_monitor_establish(finished, target, &ref) == ASX_E_STALE_HANDLE,
           "a finished watcher cannot hold a monitor (Rust WatcherNotFound)");
}

static void test_establish_checks_its_arguments(void) {
    asx_task_id watcher;
    asx_task_id target;
    setup();
    watcher = spawn_task();
    target = spawn_task();
    ASSERT(asx_monitor_establish(watcher, target, NULL) == ASX_E_INVALID_ARGUMENT, "NULL out_ref");
    ASSERT(asx_monitor_count() == 0u, "nothing was registered");
}

static void test_capacity_is_bounded_and_freed_by_demonitor(void) {
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_monitor_ref first = 0;
    uint32_t i;
    setup();
    watcher = spawn_task();
    target = spawn_task();
    for (i = 0; i < ASX_MONITOR_CAPACITY; i++) {
        ASSERT(asx_monitor_establish(watcher, target, &ref) == ASX_OK, "up to the capacity");
        if (i == 0u) first = ref;
    }
    ASSERT(asx_monitor_establish(watcher, target, &ref) == ASX_E_RESOURCE_EXHAUSTED,
           "the next is refused");
    asx_demonitor(first);
    ASSERT(asx_monitor_establish(watcher, target, &ref) == ASX_OK, "room again");
}

/* ------------------------------------------------------------------ */
/* DOWN delivery to a server                                           */
/* ------------------------------------------------------------------ */

static void test_a_server_receives_the_down(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_outcome out = ok_outcome();
    setup();
    h = spawn_server(&s);
    target = spawn_task();
    ASSERT(asx_monitor_establish_server(h, target, &ref) == ASX_OK, "the server monitors it");
    asx_monitor_task_finished(target, &out, 42u);
    run(g_root);
    ASSERT(s.n == 1u, "one DOWN");
    ASSERT(s.got[0].kind == ASX_ACTOR_INFO_DOWN, "a DOWN message");
    ASSERT(s.got[0].task == target && s.got[0].ref == ref && s.got[0].vt == 42u,
           "with the monitored task, the reference and the completion time");
    ASSERT(s.got[0].reason.severity == ASX_OUTCOME_OK, "and the outcome (Normal)");
    ASSERT(asx_monitor_count() == 0u, "a fired server monitor is gone");
}

static void test_downs_are_delivered_by_ascending_reference(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id target;
    asx_monitor_ref r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;
    asx_outcome out = ok_outcome();
    setup();
    h = spawn_server(&s);
    target = spawn_task();
    MUST_OK(asx_monitor_establish_server(h, target, &r1));
    MUST_OK(asx_monitor_establish_server(h, target, &r2));
    MUST_OK(asx_monitor_establish_server(h, target, &r3));
    MUST_OK(asx_monitor_establish_server(h, target, &r4));
    asx_demonitor(r2);
    /* The freed table slot is reused by the newest reference: slot order is
     * no longer reference order. */
    MUST_OK(asx_monitor_establish_server(h, target, &r5));
    ASSERT(r5 == 5u, "references are never reused");
    asx_monitor_task_finished(target, &out, 7u);
    run(g_root);
    ASSERT(s.n == 4u, "a DOWN per live monitor");
    ASSERT(s.got[0].ref == r1 && s.got[1].ref == r3 && s.got[2].ref == r4 && s.got[3].ref == r5,
           "by ascending monitor reference (DOWN-ORDER)");
}

static void test_one_watcher_may_monitor_a_task_twice(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id target;
    asx_monitor_ref r1 = 0;
    asx_monitor_ref r2 = 0;
    asx_outcome out = ok_outcome();
    setup();
    h = spawn_server(&s);
    target = spawn_task();
    MUST_OK(asx_monitor_establish_server(h, target, &r1));
    MUST_OK(asx_monitor_establish_server(h, target, &r2));
    ASSERT(r1 != r2, "distinct references");
    asx_monitor_task_finished(target, &out, 1u);
    run(g_root);
    ASSERT(s.n == 2u && s.got[0].ref == r1 && s.got[1].ref == r2, "each produces its own DOWN");
}

static void test_a_task_fires_its_monitors_once(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_outcome out = ok_outcome();
    setup();
    h = spawn_server(&s);
    target = spawn_task();
    MUST_OK(asx_monitor_establish_server(h, target, &ref));
    asx_monitor_task_finished(target, &out, 1u);
    asx_monitor_task_finished(target, &out, 2u);
    run(g_root);
    ASSERT(s.n == 1u, "the monitors left the set when they fired (remove_monitored)");
}

static void test_a_demonitored_monitor_never_fires(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_outcome out = ok_outcome();
    setup();
    h = spawn_server(&s);
    target = spawn_task();
    MUST_OK(asx_monitor_establish_server(h, target, &ref));
    asx_demonitor(ref);
    asx_demonitor(ref); /* idempotent */
    asx_demonitor(9999u);
    ASSERT(asx_monitor_count() == 0u, "removed");
    asx_monitor_task_finished(target, &out, 1u);
    run(g_root);
    ASSERT(s.n == 0u, "no DOWN afterwards");
}

static void test_a_stale_server_handle_cannot_monitor(void) {
    rec_state s;
    asx_actor_handle h;
    asx_actor_handle stale;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    setup();
    h = spawn_server(&s);
    target = spawn_task();
    stale = h;
    stale.generation++;
    ASSERT(asx_monitor_establish_server(stale, target, &ref) == ASX_E_STALE_HANDLE,
           "a forged handle");
    ASSERT(asx_monitor_count() == 0u, "nothing was registered");
}

static void test_a_stopped_server_drops_the_down(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_outcome out = ok_outcome();
    setup();
    h = spawn_server(&s);
    target = spawn_task();
    MUST_OK(asx_monitor_establish_server(h, target, &ref));
    run(g_root);
    MUST_OK(asx_actor_stop(h));
    run(g_root);
    ASSERT(!asx_actor_is_alive(h), "the server stopped");
    asx_monitor_task_finished(target, &out, 3u);
    ASSERT(s.n == 0u, "a closed lane drops the message (Rust SystemLane::push)");
    ASSERT(asx_monitor_count() == 0u, "and nothing is left behind");
}

/* ------------------------------------------------------------------ */
/* Polled monitors                                                     */
/* ------------------------------------------------------------------ */

static void test_a_polled_monitor_is_pending_then_stable(void) {
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_down d;
    asx_down again;
    asx_outcome out = ok_outcome();
    setup();
    watcher = spawn_task();
    target = spawn_task();
    MUST_OK(asx_monitor_establish(watcher, target, &ref));
    ASSERT(asx_monitor_down(ref, &d) == ASX_E_PENDING, "not finished yet");
    asx_monitor_task_finished(target, &out, 11u);
    ASSERT(asx_monitor_down(ref, &d) == ASX_OK, "delivered");
    ASSERT(d.monitored == target && d.ref == ref && d.vt == 11u, "its content");
    ASSERT(asx_monitor_down(ref, &again) == ASX_OK && again.vt == 11u && again.ref == ref,
           "the same notification on every later call");
    ASSERT(asx_monitor_count() == 1u, "held until released");
    asx_monitor_release(ref);
    ASSERT(asx_monitor_count() == 0u, "released");
    ASSERT(asx_monitor_down(ref, &d) == ASX_E_NOT_FOUND, "and gone");
}

static void test_releasing_a_waiting_monitor_removes_it(void) {
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_down d;
    asx_outcome out = ok_outcome();
    setup();
    watcher = spawn_task();
    target = spawn_task();
    MUST_OK(asx_monitor_establish(watcher, target, &ref));
    asx_monitor_release(ref);
    asx_monitor_task_finished(target, &out, 1u);
    ASSERT(asx_monitor_down(ref, &d) == ASX_E_NOT_FOUND, "it never fired");
}

static void test_a_server_monitor_is_not_readable_as_a_polled_one(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_down d;
    setup();
    h = spawn_server(&s);
    target = spawn_task();
    MUST_OK(asx_monitor_establish_server(h, target, &ref));
    ASSERT(asx_monitor_down(ref, &d) == ASX_E_NOT_FOUND, "its notice goes to the server");
    ASSERT(asx_monitor_down(ref, NULL) == ASX_E_INVALID_ARGUMENT, "NULL out_down");
}

static void test_a_watchers_monitors_end_with_it(void) {
    asx_task_id watcher;
    asx_task_id other;
    asx_task_id target;
    asx_monitor_ref mine = 0;
    asx_monitor_ref theirs = 0;
    asx_down d;
    asx_outcome out = ok_outcome();
    setup();
    watcher = spawn_task();
    other = spawn_task();
    target = spawn_task();
    MUST_OK(asx_monitor_establish(watcher, target, &mine));
    MUST_OK(asx_monitor_establish(other, target, &theirs));
    asx_monitor_task_finished(watcher, &out, 1u);
    ASSERT(asx_monitor_count() == 1u, "the finished watcher's monitor ended silently");
    asx_monitor_task_finished(target, &out, 2u);
    ASSERT(asx_monitor_down(mine, &d) == ASX_E_NOT_FOUND, "it never fired");
    ASSERT(asx_monitor_down(theirs, &d) == ASX_OK, "the other watcher's did");
}

/* ------------------------------------------------------------------ */
/* What the notification says                                          */
/* ------------------------------------------------------------------ */

static void test_the_outcome_is_carried(void) {
    asx_task_id watcher;
    asx_task_id t_err;
    asx_task_id t_cancel;
    asx_task_id t_panic;
    asx_monitor_ref r_err = 0, r_cancel = 0, r_panic = 0;
    asx_cancel_reason reason = asx_cancel_reason_default(ASX_CANCEL_TIMEOUT, NULL);
    asx_outcome o_err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome o_cancel = asx_outcome_cancelled(&reason);
    asx_outcome o_panic = asx_outcome_panicked("boom");
    asx_down d;
    setup();
    watcher = spawn_task();
    t_err = spawn_task();
    t_cancel = spawn_task();
    t_panic = spawn_task();
    MUST_OK(asx_monitor_establish(watcher, t_err, &r_err));
    MUST_OK(asx_monitor_establish(watcher, t_cancel, &r_cancel));
    MUST_OK(asx_monitor_establish(watcher, t_panic, &r_panic));
    asx_monitor_task_finished(t_err, &o_err, 1u);
    asx_monitor_task_finished(t_cancel, &o_cancel, 2u);
    asx_monitor_task_finished(t_panic, &o_panic, 3u);
    MUST_OK(asx_monitor_down(r_err, &d));
    ASSERT(d.reason.severity == ASX_OUTCOME_ERR, "Error");
    MUST_OK(asx_monitor_down(r_cancel, &d));
    ASSERT(d.reason.severity == ASX_OUTCOME_CANCELLED &&
               d.reason.cancel_reason.kind == ASX_CANCEL_TIMEOUT,
           "Cancelled carries its reason");
    MUST_OK(asx_monitor_down(r_panic, &d));
    ASSERT(d.reason.severity == ASX_OUTCOME_PANICKED && strcmp(d.reason.panic_message, "boom") == 0,
           "Panicked carries its message");
}

static void test_no_outcome_is_an_error_exit(void) {
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_down d;
    setup();
    watcher = spawn_task();
    target = spawn_task();
    MUST_OK(asx_monitor_establish(watcher, target, &ref));
    asx_monitor_task_finished(target, NULL, 5u);
    MUST_OK(asx_monitor_down(ref, &d));
    ASSERT(d.reason.severity == ASX_OUTCOME_ERR,
           "a task that finished without a recorded outcome is an Error (:1389)");
}

static void test_reset_forgets_monitors_and_restarts_references(void) {
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    setup();
    watcher = spawn_task();
    target = spawn_task();
    MUST_OK(asx_monitor_establish(watcher, target, &ref));
    asx_monitor_reset();
    ASSERT(asx_monitor_count() == 0u, "forgotten");
    MUST_OK(asx_monitor_establish(watcher, target, &ref));
    ASSERT(ref == 1u, "references restart at 1");
}

/* ------------------------------------------------------------------ */
/* The runtime fires monitors at task completion                       */
/* ------------------------------------------------------------------ */

static void test_the_runtime_fires_a_servers_monitor_at_completion(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    setup();
    h = spawn_server(&s);
    target = spawn_runnable(finish_poll);
    MUST_OK(asx_monitor_establish_server(h, target, &ref));
    run(g_root);
    ASSERT(s.n == 1u, "the completion fired the monitor");
    ASSERT(s.got[0].kind == ASX_ACTOR_INFO_DOWN && s.got[0].ref == ref,
           "a DOWN with its reference");
    ASSERT(asx_handle_index(s.got[0].task) == asx_handle_index(target),
           "for the monitored task (its handle's state tag has changed since)");
    ASSERT(s.got[0].reason.severity == ASX_OUTCOME_OK, "Normal");
    ASSERT(s.got[0].vt == asx_runtime_virtual_now(), "at the runtime's virtual time");
    ASSERT(asx_monitor_count() == 0u, "and it is gone");
}

static void test_the_runtime_reports_an_error_exit(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    setup();
    h = spawn_server(&s);
    target = spawn_runnable(fail_poll);
    MUST_OK(asx_monitor_establish_server(h, target, &ref));
    run(g_root);
    ASSERT(s.n == 1u && s.got[0].reason.severity == ASX_OUTCOME_ERR, "an Error exit");
}

static void test_the_runtime_fires_a_polled_monitor(void) {
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_down d;
    setup();
    watcher = spawn_task();
    target = spawn_runnable(finish_poll);
    MUST_OK(asx_monitor_establish(watcher, target, &ref));
    ASSERT(asx_monitor_down(ref, &d) == ASX_E_PENDING, "waiting");
    run(g_root);
    ASSERT(asx_monitor_down(ref, &d) == ASX_OK, "delivered by the completion");
    ASSERT(d.ref == ref && d.reason.severity == ASX_OUTCOME_OK, "its content");
}

static void test_a_watchers_completion_ends_its_monitors_silently(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref r_task = 0;
    asx_monitor_ref r_server = 0;
    asx_down d;
    asx_outcome out = ok_outcome();
    setup();
    h = spawn_server(&s);
    watcher = spawn_runnable(finish_poll);
    target = spawn_task();
    MUST_OK(asx_monitor_establish(watcher, target, &r_task));
    MUST_OK(asx_monitor_establish_server(h, target, &r_server));
    MUST_OK(asx_actor_stop(h));
    run(g_root);
    ASSERT(asx_monitor_count() == 0u, "both watchers finished: their monitors ended with them");
    ASSERT(asx_monitor_down(r_task, &d) == ASX_E_NOT_FOUND, "the task's never fired");
    asx_monitor_task_finished(target, &out, 1u);
    ASSERT(s.n == 0u, "and the stopped server was told nothing");
}

static void test_a_runtime_reset_forgets_monitors(void) {
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    setup();
    watcher = spawn_task();
    target = spawn_task();
    MUST_OK(asx_monitor_establish(watcher, target, &ref));
    asx_runtime_reset();
    ASSERT(asx_monitor_count() == 0u, "the runtime reset reaches the monitors");
}

int main(void) {
    printf("test_monitor:\n");

    RUN(test_references_start_at_one_and_increase);
    RUN(test_a_target_must_be_live);
    RUN(test_a_watcher_must_be_live);
    RUN(test_establish_checks_its_arguments);
    RUN(test_capacity_is_bounded_and_freed_by_demonitor);
    RUN(test_a_server_receives_the_down);
    RUN(test_downs_are_delivered_by_ascending_reference);
    RUN(test_one_watcher_may_monitor_a_task_twice);
    RUN(test_a_task_fires_its_monitors_once);
    RUN(test_a_demonitored_monitor_never_fires);
    RUN(test_a_stale_server_handle_cannot_monitor);
    RUN(test_a_stopped_server_drops_the_down);
    RUN(test_a_polled_monitor_is_pending_then_stable);
    RUN(test_releasing_a_waiting_monitor_removes_it);
    RUN(test_a_server_monitor_is_not_readable_as_a_polled_one);
    RUN(test_a_watchers_monitors_end_with_it);
    RUN(test_the_outcome_is_carried);
    RUN(test_no_outcome_is_an_error_exit);
    RUN(test_reset_forgets_monitors_and_restarts_references);
    RUN(test_the_runtime_fires_a_servers_monitor_at_completion);
    RUN(test_the_runtime_reports_an_error_exit);
    RUN(test_the_runtime_fires_a_polled_monitor);
    RUN(test_a_watchers_completion_ends_its_monitors_silently);
    RUN(test_a_runtime_reset_forgets_monitors);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
