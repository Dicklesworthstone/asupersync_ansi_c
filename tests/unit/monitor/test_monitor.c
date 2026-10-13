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

#include "../../../src/runtime/runtime_internal.h"
#include "../../test_runtime.h"

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
    asx_runtime_reset(test_runtime());
    asx_monitor_reset();
    MUST_OK(asx_region_open(test_runtime(), &g_root));
    MUST_OK(asx_region_open(test_runtime(), &g_idle));
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

static void test_a_stored_notice_goes_with_its_watcher(void) {
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_down d;
    asx_outcome out = ok_outcome();
    setup();
    watcher = spawn_task();
    target = spawn_task();
    MUST_OK(asx_monitor_establish(watcher, target, &ref));
    asx_monitor_task_finished(target, &out, 1u);
    ASSERT(asx_monitor_down(ref, &d) == ASX_OK && asx_monitor_count() == 1u,
           "the DOWN is stored for the handle");
    asx_monitor_task_finished(watcher, &out, 2u);
    ASSERT(asx_monitor_count() == 0u,
           "the handle is dropped with its task: nothing is left holding capacity");
    ASSERT(asx_monitor_down(ref, &d) == ASX_E_NOT_FOUND, "and nothing can read it");
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
    ASSERT(s.got[0].vt == asx_runtime_virtual_now(test_runtime()), "at the runtime's virtual time");
    ASSERT(asx_monitor_count() == 0u, "and it is gone");
}

/* A poll that returns an error is an Error exit and a fault: under the
 * debug profile's fail-fast containment the scheduler halts on it right
 * after the completion, so a server would not run handle_info in that run.
 * The monitor fires at the completion all the same, which a polled monitor's
 * stored notice shows whatever the profile does next. */
static void test_the_runtime_reports_an_error_exit(void) {
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_down d;
    setup();
    watcher = spawn_task();
    target = spawn_runnable(fail_poll);
    MUST_OK(asx_monitor_establish(watcher, target, &ref));
    run(g_root); /* may return the fault; the completion already fired it */
    ASSERT(asx_monitor_down(ref, &d) == ASX_OK, "the completion fired the monitor");
    ASSERT(d.reason.severity == ASX_OUTCOME_ERR, "an Error exit");
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
    asx_runtime_reset(test_runtime());
    ASSERT(asx_monitor_count() == 0u, "the runtime reset reaches the monitors");
}

/* ------------------------------------------------------------------ */
/* Monitor requests through the region-command lane (lab dispatch)     */
/* ------------------------------------------------------------------ */

static void run_lab(asx_region_id region) {
    asx_budget b = asx_budget_from_polls(1000);
    st_sink_ = asx_scheduler_run_until_idle(region, &b);
    (void)st_sink_;
}

static int g_lane_go;

/* Finishes once told to; parks until woken before that. */
static asx_status gated_target_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    if (g_lane_go) return ASX_OK;
    MUST_OK(asx_task_park(self));
    return ASX_E_PENDING;
}

typedef struct {
    asx_task_id target;
    asx_actor_handle server; /* to_server: the server holds the monitor */
    int to_server;
    int release_after_open; /* drop the monitor handle at once */
    int await_down;         /* wait for the DOWN with asx_monitor_down_poll */
    asx_monitor_ref ref;
    asx_status open_st;
    asx_status down_st;
    asx_down down;
    uint32_t polls;
    int opened;
} lane_watcher;

static asx_status lane_watcher_poll(void *user_data, asx_task_id self) {
    lane_watcher *w = (lane_watcher *)user_data;
    asx_status st;
    w->polls++;
    if (!w->opened) {
        st = w->to_server ? asx_monitor_open_server_poll(self, w->server, w->target, &w->ref)
                          : asx_monitor_open_poll(self, w->target, &w->ref);
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        w->open_st = st;
        w->opened = 1;
        if (st != ASX_OK) return ASX_OK;
        g_lane_go = 1;
        MUST_OK(asx_task_wake(w->target));
        if (w->release_after_open) {
            asx_monitor_release(w->ref);
            return ASX_OK;
        }
    }
    if (!w->await_down) return ASX_OK;
    st = asx_monitor_down_poll(self, NULL, w->ref, &w->down);
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    w->down_st = st;
    return ASX_OK;
}

static asx_task_id spawn_with(asx_region_id region, asx_task_poll_fn poll, void *user_data) {
    asx_task_id id = ASX_INVALID_ID;
    MUST_OK(asx_task_spawn(region, poll, user_data, &id));
    return id;
}

static void test_a_lane_request_takes_a_step_and_its_down_follows(void) {
    lane_watcher w;
    asx_task_id target;
    setup();
    g_lane_go = 0;
    memset(&w, 0, sizeof(w));
    w.await_down = 1;
    MUST_OK(asx_scheduler_use_lab_dispatch(7u));
    target = spawn_with(g_root, gated_target_poll, NULL);
    w.target = target;
    (void)spawn_with(g_root, lane_watcher_poll, &w);
    run_lab(g_root);
    ASSERT(w.open_st == ASX_OK && w.ref == 1u,
           "the request was applied and returned its reference");
    ASSERT(w.down_st == ASX_OK && w.down.ref == w.ref, "the DOWN was awaited");
    ASSERT(asx_handle_index(w.down.monitored) == asx_handle_index(target), "for the target");
    ASSERT(w.down.reason.severity == ASX_OUTCOME_OK, "Normal");
    ASSERT(w.polls == 3u, "poll 1 queued the request and parked, poll 2 had the reference and "
                          "waited, poll 3 had the DOWN");
}

static void test_a_lane_request_for_a_finished_task_is_not_found(void) {
    lane_watcher w;
    asx_task_id finished;
    setup();
    memset(&w, 0, sizeof(w));
    MUST_OK(asx_scheduler_use_lab_dispatch(7u));
    finished = spawn_with(g_root, finish_poll, NULL);
    run_lab(g_root);
    w.target = finished;
    (void)spawn_with(g_idle, lane_watcher_poll, &w);
    run_lab(g_idle);
    ASSERT(w.opened && w.open_st == ASX_E_NOT_FOUND, "refused when the lane applies it");
    ASSERT(asx_monitor_count() == 0u, "nothing was registered");
}

static void test_a_lane_request_for_a_server_reaches_its_handle_info(void) {
    rec_state s;
    asx_actor_handle h;
    lane_watcher w;
    asx_task_id target;
    setup();
    g_lane_go = 0;
    memset(&w, 0, sizeof(w));
    MUST_OK(asx_scheduler_use_lab_dispatch(7u));
    h = spawn_server(&s);
    target = spawn_with(g_root, gated_target_poll, NULL);
    w.target = target;
    w.server = h;
    w.to_server = 1;
    (void)spawn_with(g_root, lane_watcher_poll, &w);
    run_lab(g_root);
    ASSERT(w.open_st == ASX_OK, "the server's request was applied");
    ASSERT(s.n == 1u && s.got[0].kind == ASX_ACTOR_INFO_DOWN && s.got[0].ref == w.ref,
           "and its DOWN reached handle_info");
}

static void test_a_released_lane_monitor_never_fires(void) {
    lane_watcher w;
    asx_task_id target;
    asx_down d;
    setup();
    g_lane_go = 0;
    memset(&w, 0, sizeof(w));
    w.release_after_open = 1;
    MUST_OK(asx_scheduler_use_lab_dispatch(7u));
    target = spawn_with(g_root, gated_target_poll, NULL);
    w.target = target;
    (void)spawn_with(g_root, lane_watcher_poll, &w);
    run_lab(g_root);
    ASSERT(w.open_st == ASX_OK, "it was established");
    ASSERT(asx_monitor_count() == 0u, "and it is gone, fired or not, nothing is held");
    ASSERT(asx_monitor_down(w.ref, &d) == ASX_E_NOT_FOUND, "released");
}

/* Monitor::drop queues a Demonitor command unless the DOWN was delivered
 * (monitor.rs:1133), so a watcher that finishes while it still waits leaves
 * one in the lane: a no-op when applied, but a step of the lab. */
static void test_a_watcher_finishing_with_a_waiting_monitor_queues_a_demonitor(void) {
    asx_task_id watcher;
    asx_task_id target;
    asx_monitor_ref ref = 0;
    asx_outcome out = ok_outcome();
    setup();
    MUST_OK(asx_scheduler_use_lab_dispatch(7u));
    watcher = spawn_task();
    target = spawn_task();
    MUST_OK(asx_monitor_establish(watcher, target, &ref));
    ASSERT(asx_lab_region_commands_pending() == 0, "establishing queued nothing");
    asx_monitor_task_finished(watcher, &out, 1u);
    ASSERT(asx_monitor_count() == 0u, "the monitor ended with its watcher");
    ASSERT(asx_lab_region_commands_pending() != 0, "and its dropped handle queued a Demonitor");
}

static void test_a_delivered_or_server_monitor_queues_no_demonitor(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id watcher;
    asx_task_id server_task = ASX_INVALID_ID;
    asx_task_id target;
    asx_monitor_ref r_task = 0;
    asx_monitor_ref r_server = 0;
    asx_outcome out = ok_outcome();
    setup();
    MUST_OK(asx_scheduler_use_lab_dispatch(7u));
    h = spawn_server(&s);
    watcher = spawn_task();
    target = spawn_task();
    MUST_OK(asx_monitor_establish(watcher, target, &r_task));
    MUST_OK(asx_monitor_establish_server(h, target, &r_server));
    asx_monitor_task_finished(target, &out, 1u);
    MUST_OK(asx_actor_task_id(h, &server_task));
    asx_monitor_task_finished(watcher, &out, 2u);
    asx_monitor_task_finished(server_task, &out, 3u);
    ASSERT(asx_lab_region_commands_pending() == 0,
           "a stored notice and a server's monitor have no handle to drop");
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
    RUN(test_a_stored_notice_goes_with_its_watcher);
    RUN(test_the_outcome_is_carried);
    RUN(test_no_outcome_is_an_error_exit);
    RUN(test_reset_forgets_monitors_and_restarts_references);
    RUN(test_the_runtime_fires_a_servers_monitor_at_completion);
    RUN(test_the_runtime_reports_an_error_exit);
    RUN(test_the_runtime_fires_a_polled_monitor);
    RUN(test_a_watchers_completion_ends_its_monitors_silently);
    RUN(test_a_runtime_reset_forgets_monitors);
    RUN(test_a_lane_request_takes_a_step_and_its_down_follows);
    RUN(test_a_lane_request_for_a_finished_task_is_not_found);
    RUN(test_a_lane_request_for_a_server_reaches_its_handle_info);
    RUN(test_a_released_lane_monitor_never_fires);
    RUN(test_a_watcher_finishing_with_a_waiting_monitor_queues_a_demonitor);
    RUN(test_a_delivered_or_server_monitor_queues_no_demonitor);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
