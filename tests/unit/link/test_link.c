/*
 * test_link.c — unit tests for task links and exit signals (Rust
 * src/link.rs).
 *
 * The runtime fires links when a task completes; most tests fire them
 * directly with asx_link_task_finished, which is that call, on tasks that
 * were never polled. Rust parity of the observable schedule is covered by
 * the conformance fixtures.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_runtime.h"
#include <asx/actor/actor.h>
#include <asx/core/budget.h>
#include <asx/link/link.h>
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
 * a test finishes it by hand. */
static asx_region_id g_root;
static asx_region_id g_idle;

static void setup(void) {
    asx_runtime_reset(test_runtime());
    asx_link_reset();
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

static asx_task_id spawn_in(asx_region_id region, asx_task_poll_fn poll) {
    asx_task_id id = ASX_INVALID_ID;
    MUST_OK(asx_task_spawn(region, poll, NULL, &id));
    return id;
}

/* A task that is never polled: created, so live. */
static asx_task_id spawn_task(void) { return spawn_in(g_idle, finish_poll); }

static asx_task_id spawn_finished_task(void) {
    asx_task_id id = spawn_in(g_root, finish_poll);
    run(g_root);
    return id;
}

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

static asx_outcome err_exit(void) { return asx_outcome_make(ASX_OUTCOME_ERR); }
static asx_outcome ok_exit(void) { return asx_outcome_make(ASX_OUTCOME_OK); }

/* Whether the task was cancelled, and with what. */
static int cancel_reason_of(asx_task_id task, asx_cancel_reason *out) {
    return asx_task_get_cancel_reason(task, out) == ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Establishing                                                        */
/* ------------------------------------------------------------------ */

static void test_references_start_at_one_and_increase(void) {
    asx_task_id a;
    asx_task_id b;
    asx_link_ref r1 = 0;
    asx_link_ref r2 = 0;
    setup();
    a = spawn_task();
    b = spawn_task();
    ASSERT(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, &r1) == ASX_OK, "first link");
    ASSERT(asx_link_establish(a, b, ASX_EXIT_TRAP, &r2) == ASX_OK, "a second one, same pair");
    ASSERT(r1 == 1u && r2 == 2u, "references from 1, in order");
    ASSERT(asx_link_count() == 2u, "both exist");
}

static void test_a_peer_must_be_live_and_so_must_the_holder(void) {
    asx_task_id live;
    asx_task_id finished;
    asx_link_ref ref = 0;
    setup();
    live = spawn_task();
    finished = spawn_finished_task();
    ASSERT(asx_link_establish(live, finished, ASX_EXIT_PROPAGATE, &ref) == ASX_E_NOT_FOUND,
           "a finished peer (Rust WatchError::NotFound)");
    ASSERT(asx_link_establish(finished, live, ASX_EXIT_PROPAGATE, &ref) == ASX_E_STALE_HANDLE,
           "a finished holder (WatcherNotFound)");
    ASSERT(asx_link_count() == 0u, "nothing was registered");
}

static void test_establish_checks_its_arguments_and_the_capacity(void) {
    asx_task_id a;
    asx_task_id b;
    asx_link_ref ref = 0;
    asx_link_ref first = 0;
    uint32_t i;
    setup();
    a = spawn_task();
    b = spawn_task();
    ASSERT(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, NULL) == ASX_E_INVALID_ARGUMENT,
           "NULL out_ref");
    ASSERT(asx_link_establish(a, b, (asx_exit_policy)9, &ref) == ASX_E_INVALID_ARGUMENT,
           "an unknown policy");
    for (i = 0; i < ASX_LINK_CAPACITY; i++) {
        ASSERT(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, &ref) == ASX_OK, "up to the capacity");
        if (i == 0u) first = ref;
    }
    ASSERT(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, &ref) == ASX_E_RESOURCE_EXHAUSTED,
           "the next is refused");
    asx_unlink(first);
    ASSERT(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, &ref) == ASX_OK, "room again");
}

/* ------------------------------------------------------------------ */
/* What an exit does                                                   */
/* ------------------------------------------------------------------ */

static void test_an_abnormal_exit_cancels_a_propagating_peer(void) {
    asx_task_id a;
    asx_task_id b;
    asx_link_ref ref = 0;
    asx_outcome out = err_exit();
    asx_cancel_reason r;
    setup();
    a = spawn_task();
    b = spawn_task();
    MUST_OK(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, &ref));
    ASSERT(!cancel_reason_of(a, &r), "not cancelled before");
    asx_link_task_finished(b, &out, 7u);
    ASSERT(cancel_reason_of(a, &r), "the holder was cancelled");
    ASSERT(r.kind == ASX_CANCEL_LINKED_EXIT, "with a LinkedExit reason");
    ASSERT(r.message != NULL && strcmp(r.message, "link exit") == 0, "and the message link exit");
    ASSERT(r.cause == NULL, "an error exit is no cause");
    ASSERT(asx_link_count() == 0u, "the link ended with the exit");
}

static void test_a_cancelled_exit_is_the_cause_of_the_cancel(void) {
    asx_task_id a;
    asx_task_id b;
    asx_link_ref ref = 0;
    asx_cancel_reason why = asx_cancel_reason_default(ASX_CANCEL_TIMEOUT, NULL);
    asx_outcome out = asx_outcome_cancelled(&why);
    asx_cancel_reason r;
    setup();
    a = spawn_task();
    b = spawn_task();
    MUST_OK(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, &ref));
    asx_link_task_finished(b, &out, 7u);
    ASSERT(cancel_reason_of(a, &r) && r.kind == ASX_CANCEL_LINKED_EXIT, "cancelled, LinkedExit");
    ASSERT(r.cause != NULL && r.cause->kind == ASX_CANCEL_TIMEOUT,
           "caused by the exiting task's own reason (link.rs:610)");
}

static void test_a_normal_exit_ends_the_link_silently(void) {
    asx_task_id a;
    asx_task_id b;
    asx_link_ref ref = 0;
    asx_outcome out = ok_exit();
    asx_cancel_reason r;
    setup();
    a = spawn_task();
    b = spawn_task();
    MUST_OK(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, &ref));
    asx_link_task_finished(b, &out, 7u);
    ASSERT(!cancel_reason_of(a, &r), "the peer is not cancelled (OTP: a normal exit is silent)");
    ASSERT(asx_link_count() == 0u, "and the link is gone");
}

static void test_the_peer_side_always_propagates(void) {
    asx_task_id a;
    asx_task_id b;
    asx_link_ref ref = 0;
    asx_outcome out = err_exit();
    asx_cancel_reason r;
    setup();
    a = spawn_task();
    b = spawn_task();
    /* The holder traps, but when the holder exits abnormally its peer is
     * still cancelled: the peer's policy is Propagate (:1366). */
    MUST_OK(asx_link_establish(a, b, ASX_EXIT_TRAP, &ref));
    asx_link_task_finished(a, &out, 7u);
    ASSERT(cancel_reason_of(b, &r) && r.kind == ASX_CANCEL_LINKED_EXIT, "the peer was cancelled");
}

static void test_a_trapping_server_is_told_and_keeps_running(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id peer;
    asx_task_id server_task = ASX_INVALID_ID;
    asx_link_ref ref = 0;
    asx_outcome out = err_exit();
    asx_cancel_reason r;
    setup();
    h = spawn_server(&s);
    peer = spawn_task();
    MUST_OK(asx_link_establish_server(h, peer, ASX_EXIT_TRAP, &ref));
    asx_link_task_finished(peer, &out, 42u);
    run(g_root);
    ASSERT(s.n == 1u && s.got[0].kind == ASX_ACTOR_INFO_EXIT, "one EXIT message");
    ASSERT(asx_handle_index(s.got[0].task) == asx_handle_index(peer) && s.got[0].ref == ref &&
               s.got[0].vt == 42u && s.got[0].reason.severity == ASX_OUTCOME_ERR,
           "from the peer, with its reference, the exit time and the reason");
    MUST_OK(asx_actor_task_id(h, &server_task));
    ASSERT(!cancel_reason_of(server_task, &r), "the server was not cancelled");
    ASSERT(asx_actor_is_alive(h), "and keeps running");
}

static void test_a_trapping_holder_hears_a_normal_exit_too(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id peer;
    asx_link_ref ref = 0;
    asx_outcome out = ok_exit();
    setup();
    h = spawn_server(&s);
    peer = spawn_task();
    MUST_OK(asx_link_establish_server(h, peer, ASX_EXIT_TRAP, &ref));
    asx_link_task_finished(peer, &out, 5u);
    run(g_root);
    ASSERT(s.n == 1u && s.got[0].kind == ASX_ACTOR_INFO_EXIT &&
               s.got[0].reason.severity == ASX_OUTCOME_OK,
           "as in OTP, a trapping task also hears of a normal exit");
}

static void test_a_propagating_server_is_cancelled(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id peer;
    asx_task_id server_task = ASX_INVALID_ID;
    asx_link_ref ref = 0;
    asx_outcome out = err_exit();
    asx_cancel_reason r;
    setup();
    h = spawn_server(&s);
    peer = spawn_task();
    MUST_OK(asx_link_establish_server(h, peer, ASX_EXIT_PROPAGATE, &ref));
    asx_link_task_finished(peer, &out, 3u);
    MUST_OK(asx_actor_task_id(h, &server_task));
    ASSERT(cancel_reason_of(server_task, &r) && r.kind == ASX_CANCEL_LINKED_EXIT,
           "a propagating link cancels its holder");
    run(g_root);
    ASSERT(s.n == 0u && !asx_actor_is_alive(h), "it stopped without handling a message");
}

static void test_exit_signals_of_one_exit_go_by_reference(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id peer;
    asx_link_ref r1 = 0;
    asx_link_ref r2 = 0;
    asx_outcome out = err_exit();
    setup();
    h = spawn_server(&s);
    peer = spawn_task();
    MUST_OK(asx_link_establish_server(h, peer, ASX_EXIT_TRAP, &r1));
    MUST_OK(asx_link_establish_server(h, peer, ASX_EXIT_TRAP, &r2));
    asx_link_task_finished(peer, &out, 1u);
    run(g_root);
    ASSERT(s.n == 2u && s.got[0].ref == r1 && s.got[1].ref == r2, "by ascending reference");
}

static void test_unlink_removes_a_link(void) {
    asx_task_id a;
    asx_task_id b;
    asx_link_ref ref = 0;
    asx_outcome out = err_exit();
    asx_cancel_reason r;
    setup();
    a = spawn_task();
    b = spawn_task();
    MUST_OK(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, &ref));
    asx_unlink(ref);
    asx_unlink(ref); /* idempotent */
    ASSERT(asx_link_count() == 0u, "removed");
    asx_link_task_finished(b, &out, 1u);
    ASSERT(!cancel_reason_of(a, &r), "nothing is cancelled afterwards");
}

static void test_a_tasks_links_end_with_it(void) {
    asx_task_id a;
    asx_task_id b;
    asx_task_id c;
    asx_link_ref r1 = 0;
    asx_link_ref r2 = 0;
    asx_outcome ok = ok_exit();
    asx_outcome bad = err_exit();
    asx_cancel_reason r;
    setup();
    a = spawn_task();
    b = spawn_task();
    c = spawn_task();
    MUST_OK(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, &r1));
    MUST_OK(asx_link_establish(c, a, ASX_EXIT_PROPAGATE, &r2));
    asx_link_task_finished(a, &ok, 1u);
    ASSERT(asx_link_count() == 0u, "every link of the task ended (remove_task)");
    asx_link_task_finished(b, &bad, 2u);
    ASSERT(!cancel_reason_of(c, &r), "so a later exit cancels no one");
}

static void test_a_self_link_is_one_edge(void) {
    asx_task_id a;
    asx_link_ref ref = 0;
    asx_outcome out = err_exit();
    setup();
    a = spawn_task();
    MUST_OK(asx_link_establish(a, a, ASX_EXIT_TRAP, &ref));
    asx_link_task_finished(a, &out, 1u);
    ASSERT(asx_link_count() == 0u, "ended once, without trouble");
}

/* ------------------------------------------------------------------ */
/* A polled trapping link                                              */
/* ------------------------------------------------------------------ */

static void test_a_polled_trapping_link_stores_its_signal(void) {
    asx_task_id a;
    asx_task_id b;
    asx_link_ref ref = 0;
    asx_exit_signal sig;
    asx_outcome out = err_exit();
    setup();
    a = spawn_task();
    b = spawn_task();
    MUST_OK(asx_link_establish(a, b, ASX_EXIT_TRAP, &ref));
    ASSERT(asx_link_exit_poll(a, NULL, ref, &sig) == ASX_E_PENDING, "nothing yet");
    asx_link_task_finished(b, &out, 9u);
    ASSERT(asx_link_exit_poll(a, NULL, ref, &sig) == ASX_OK, "delivered");
    ASSERT(sig.vt == 9u && sig.ref == ref && sig.reason.severity == ASX_OUTCOME_ERR, "its content");
    ASSERT(asx_link_exit_poll(a, NULL, ref, &sig) == ASX_OK, "and again, unchanged");
    ASSERT(asx_link_count() == 1u, "the stored signal holds capacity");
    asx_link_release(ref);
    ASSERT(asx_link_count() == 0u, "until released");
}

static void test_a_stored_signal_goes_with_its_holder(void) {
    asx_task_id a;
    asx_task_id b;
    asx_link_ref ref = 0;
    asx_exit_signal sig;
    asx_outcome out = err_exit();
    setup();
    a = spawn_task();
    b = spawn_task();
    MUST_OK(asx_link_establish(a, b, ASX_EXIT_TRAP, &ref));
    asx_link_task_finished(b, &out, 1u);
    ASSERT(asx_link_count() == 1u, "the signal is stored for the holder's handle");
    asx_link_task_finished(a, &out, 2u);
    ASSERT(asx_link_count() == 0u, "the handle is dropped with the holder, nothing is left");
    ASSERT(asx_link_exit_poll(a, NULL, ref, &sig) == ASX_E_NOT_FOUND, "and nothing can read it");
}

static void test_a_propagating_link_has_no_exit_to_poll(void) {
    asx_task_id a;
    asx_task_id b;
    asx_link_ref ref = 0;
    asx_exit_signal sig;
    setup();
    a = spawn_task();
    b = spawn_task();
    MUST_OK(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, &ref));
    ASSERT(asx_link_exit_poll(a, NULL, ref, &sig) == ASX_E_NOT_FOUND, "only a trapping link");
    ASSERT(asx_link_exit_poll(a, NULL, ref, NULL) == ASX_E_INVALID_ARGUMENT, "NULL out_signal");
}

/* ------------------------------------------------------------------ */
/* The runtime fires links at task completion                          */
/* ------------------------------------------------------------------ */

static void test_the_runtime_cancels_a_peer_when_a_task_fails(void) {
    asx_task_id holder;
    asx_task_id failing;
    asx_link_ref ref = 0;
    asx_cancel_reason r;
    setup();
    holder = spawn_task();
    failing = spawn_in(g_root, fail_poll);
    MUST_OK(asx_link_establish(holder, failing, ASX_EXIT_PROPAGATE, &ref));
    run(g_root); /* may return the fault; the completion already fired the link */
    ASSERT(cancel_reason_of(holder, &r), "the completion fired the link");
    ASSERT(r.kind == ASX_CANCEL_LINKED_EXIT, "LinkedExit");
}

static void test_a_runtime_reset_forgets_links(void) {
    asx_task_id a;
    asx_task_id b;
    asx_link_ref ref = 0;
    setup();
    a = spawn_task();
    b = spawn_task();
    MUST_OK(asx_link_establish(a, b, ASX_EXIT_PROPAGATE, &ref));
    asx_runtime_reset(test_runtime());
    ASSERT(asx_link_count() == 0u, "the runtime reset reaches the links");
}

/* ------------------------------------------------------------------ */
/* Link requests through the region-command lane (lab dispatch)        */
/* ------------------------------------------------------------------ */

static void run_lab(asx_region_id region) {
    asx_budget b = asx_budget_from_polls(1000);
    st_sink_ = asx_scheduler_run_until_idle(region, &b);
    (void)st_sink_;
}

static int g_lane_go;

static asx_status gated_target_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    if (g_lane_go) return ASX_E_INVALID_STATE; /* the failing exit */
    MUST_OK(asx_task_park(self));
    return ASX_E_PENDING;
}

typedef struct {
    asx_task_id target;
    asx_actor_handle server;
    asx_link_ref ref;
    asx_status open_st;
    uint32_t polls;
    int opened;
} lane_opener;

static asx_status lane_opener_poll(void *user_data, asx_task_id self) {
    lane_opener *o = (lane_opener *)user_data;
    asx_status st;
    o->polls++;
    st = asx_link_open_server_poll(self, o->server, o->target, ASX_EXIT_TRAP, &o->ref);
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    o->open_st = st;
    o->opened = 1;
    g_lane_go = 1;
    MUST_OK(asx_task_wake(o->target));
    return ASX_OK;
}

static void test_a_lane_link_request_takes_a_step_and_its_exit_follows(void) {
    rec_state s;
    asx_actor_handle h;
    lane_opener o;
    asx_task_id target = ASX_INVALID_ID;
    setup();
    g_lane_go = 0;
    memset(&o, 0, sizeof(o));
    MUST_OK(asx_scheduler_use_lab_dispatch(7u));
    h = spawn_server(&s);
    MUST_OK(asx_task_spawn(g_root, gated_target_poll, NULL, &target));
    o.target = target;
    o.server = h;
    MUST_OK(asx_task_spawn(g_root, lane_opener_poll, &o, &target));
    run_lab(g_root);
    /* The target's failing exit is a fault: under fail-fast containment the
     * run halted on it, after the link fired. Resume, as a driver does. */
    run_lab(g_root);
    ASSERT(o.opened && o.open_st == ASX_OK && o.ref == 1u, "applied by the drain, reference 1");
    ASSERT(o.polls == 2u, "poll 1 queued the request and parked, poll 2 had the result");
    ASSERT(s.n == 1u && s.got[0].kind == ASX_ACTOR_INFO_EXIT && s.got[0].ref == o.ref,
           "and the server's trapped EXIT reached handle_info");
}

int main(void) {
    printf("test_link:\n");

    RUN(test_references_start_at_one_and_increase);
    RUN(test_a_peer_must_be_live_and_so_must_the_holder);
    RUN(test_establish_checks_its_arguments_and_the_capacity);
    RUN(test_an_abnormal_exit_cancels_a_propagating_peer);
    RUN(test_a_cancelled_exit_is_the_cause_of_the_cancel);
    RUN(test_a_normal_exit_ends_the_link_silently);
    RUN(test_the_peer_side_always_propagates);
    RUN(test_a_trapping_server_is_told_and_keeps_running);
    RUN(test_a_trapping_holder_hears_a_normal_exit_too);
    RUN(test_a_propagating_server_is_cancelled);
    RUN(test_exit_signals_of_one_exit_go_by_reference);
    RUN(test_unlink_removes_a_link);
    RUN(test_a_tasks_links_end_with_it);
    RUN(test_a_self_link_is_one_edge);
    RUN(test_a_polled_trapping_link_stores_its_signal);
    RUN(test_a_stored_signal_goes_with_its_holder);
    RUN(test_a_propagating_link_has_no_exit_to_poll);
    RUN(test_the_runtime_cancels_a_peer_when_a_task_fails);
    RUN(test_a_runtime_reset_forgets_links);
    RUN(test_a_lane_link_request_takes_a_step_and_its_exit_follows);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
