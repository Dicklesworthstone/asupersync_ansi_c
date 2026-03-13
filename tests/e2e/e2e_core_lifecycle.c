/*
 * e2e_core_lifecycle.c — end-to-end scenarios for core runtime semantics
 *
 * Exercises: region lifecycle, task spawn/complete, obligation protocol,
 * cancellation propagation, quiescence close paths, timer wheel, and
 * bounded MPSC channel with two-phase send.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/time/timer_wheel.h>
#include <stdio.h>
#include <string.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

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

/* Simple poll function: completes immediately */
static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

/* Countdown poll: yields N times then completes */
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

/* Cancel-aware poll: checkpoints and finalizes on cancel */
static asx_status poll_cancel_aware(void *ud, asx_task_id self) {
    countdown_state *s = (countdown_state *)ud;
    asx_checkpoint_result cp;

    ASX_CO_BEGIN(&s->co);
    for (;;) {
        if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
            IGNORE_RC(asx_task_finalize(self));
            return ASX_OK;
        }
        s->remaining--;
        if (s->remaining <= 0) break;
        ASX_CO_YIELD(&s->co);
    }
    ASX_CO_END(&s->co);
}

/* -------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------- */

static void scenario_region_open_close(void) {
    SCENARIO_BEGIN("region.open_close");
    asx_runtime_reset();

    asx_region_id rid;
    asx_region_state st;
    asx_status rc;

    rc = asx_region_open(&rid);
    SCENARIO_CHECK(rc == ASX_OK, "region_open failed");

    rc = asx_region_get_state(rid, &st);
    SCENARIO_CHECK(rc == ASX_OK && st == ASX_REGION_OPEN, "region not OPEN");

    rc = asx_region_close(rid);
    SCENARIO_CHECK(rc == ASX_OK, "region_close failed");

    SCENARIO_END();
}

static void scenario_task_spawn_complete(void) {
    SCENARIO_BEGIN("task.spawn_complete");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    asx_task_state ts;
    asx_outcome out;
    asx_budget budget = asx_budget_infinite();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn");

    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_CREATED,
                   "task not CREATED");

    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");

    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "task not COMPLETED");

    SCENARIO_CHECK(asx_task_get_outcome(tid, &out) == ASX_OK && out.severity == ASX_OUTCOME_OK,
                   "outcome not OK");

    SCENARIO_END();
}

static void scenario_task_coroutine_yields(void) {
    SCENARIO_BEGIN("task.coroutine_yields");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    void *state;
    countdown_state *cs;
    asx_budget budget;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_countdown, (uint32_t)sizeof(countdown_state),
                                           NULL, &tid, &state) == ASX_OK,
                   "task_spawn_captured");

    cs = (countdown_state *)state;
    cs->co.line = 0;
    cs->remaining = 3;

    budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "task not COMPLETED after 3 yields");

    SCENARIO_END();
}

static void scenario_obligation_reserve_commit(void) {
    SCENARIO_BEGIN("obligation.reserve_commit");
    asx_runtime_reset();

    asx_region_id rid;
    asx_obligation_id oid;
    asx_obligation_state os;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_obligation_reserve(rid, &oid) == ASX_OK, "obligation_reserve");

    SCENARIO_CHECK(asx_obligation_get_state(oid, &os) == ASX_OK && os == ASX_OBLIGATION_RESERVED,
                   "not RESERVED");

    SCENARIO_CHECK(asx_obligation_commit(oid) == ASX_OK, "obligation_commit");

    SCENARIO_CHECK(asx_obligation_get_state(oid, &os) == ASX_OK && os == ASX_OBLIGATION_COMMITTED,
                   "not COMMITTED");

    SCENARIO_END();
}

static void scenario_obligation_reserve_abort(void) {
    SCENARIO_BEGIN("obligation.reserve_abort");
    asx_runtime_reset();

    asx_region_id rid;
    asx_obligation_id oid;
    asx_obligation_state os;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_obligation_reserve(rid, &oid) == ASX_OK, "obligation_reserve");

    SCENARIO_CHECK(asx_obligation_abort(oid) == ASX_OK, "obligation_abort");

    SCENARIO_CHECK(asx_obligation_get_state(oid, &os) == ASX_OK && os == ASX_OBLIGATION_ABORTED,
                   "not ABORTED");

    SCENARIO_END();
}

static void scenario_cancel_checkpoint_finalize(void) {
    SCENARIO_BEGIN("cancel.checkpoint_finalize");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    void *state;
    countdown_state *cs;
    asx_budget budget;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_cancel_aware,
                                           (uint32_t)sizeof(countdown_state), NULL, &tid,
                                           &state) == ASX_OK,
                   "task_spawn_captured");

    cs = (countdown_state *)state;
    cs->co.line = 0;
    cs->remaining = 100; /* would run forever without cancel */

    /* Cancel the task */
    SCENARIO_CHECK(asx_task_cancel(tid, ASX_CANCEL_USER) == ASX_OK, "task_cancel");

    /* Run scheduler — task should observe cancel and finalize */
    budget = asx_budget_from_polls(20);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "task not COMPLETED after cancel");

    asx_outcome out;
    SCENARIO_CHECK(asx_task_get_outcome(tid, &out) == ASX_OK, "get_outcome");

    SCENARIO_END();
}

static void scenario_cancel_propagate_region(void) {
    SCENARIO_BEGIN("cancel.propagate_region");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id t1, t2, t3;
    void *s1, *s2, *s3;
    countdown_state *cs;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn 3 long-running tasks */
    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_cancel_aware,
                                           (uint32_t)sizeof(countdown_state), NULL, &t1,
                                           &s1) == ASX_OK,
                   "spawn_t1");
    cs = (countdown_state *)s1;
    cs->co.line = 0;
    cs->remaining = 100;

    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_cancel_aware,
                                           (uint32_t)sizeof(countdown_state), NULL, &t2,
                                           &s2) == ASX_OK,
                   "spawn_t2");
    cs = (countdown_state *)s2;
    cs->co.line = 0;
    cs->remaining = 100;

    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_cancel_aware,
                                           (uint32_t)sizeof(countdown_state), NULL, &t3,
                                           &s3) == ASX_OK,
                   "spawn_t3");
    cs = (countdown_state *)s3;
    cs->co.line = 0;
    cs->remaining = 100;

    /* Propagate cancel to all tasks */
    uint32_t cancelled = asx_cancel_propagate(rid, ASX_CANCEL_SHUTDOWN);
    SCENARIO_CHECK(cancelled == 3, "propagate should cancel 3 tasks");

    /* Run scheduler to completion */
    asx_budget budget = asx_budget_from_polls(60);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(t1, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "t1 not completed");
    SCENARIO_CHECK(asx_task_get_state(t2, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "t2 not completed");
    SCENARIO_CHECK(asx_task_get_state(t3, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "t3 not completed");

    SCENARIO_END();
}

static void scenario_quiescence_empty_region(void) {
    SCENARIO_BEGIN("quiescence.empty_region");
    asx_runtime_reset();

    asx_region_id rid;
    asx_budget budget = asx_budget_from_polls(10);

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Drain drives region through Close → Finalize → Closed */
    asx_status rc = asx_region_drain(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "region_drain");

    /* Empty closed region should be quiescent */
    rc = asx_quiescence_check(rid);
    SCENARIO_CHECK(rc == ASX_OK, "quiescence_check failed on empty region");

    SCENARIO_END();
}

static void scenario_quiescence_after_drain(void) {
    SCENARIO_BEGIN("quiescence.after_drain");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;
    void *state;
    countdown_state *cs;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_countdown, (uint32_t)sizeof(countdown_state),
                                           NULL, &tid, &state) == ASX_OK,
                   "spawn");

    cs = (countdown_state *)state;
    cs->co.line = 0;
    cs->remaining = 2;

    asx_budget budget = asx_budget_from_polls(20);
    asx_status rc = asx_region_drain(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "region_drain");

    rc = asx_quiescence_check(rid);
    SCENARIO_CHECK(rc == ASX_OK, "quiescence_check after drain");

    SCENARIO_END();
}

static void scenario_timer_register_expire(void) {
    SCENARIO_BEGIN("timer.register_expire");

    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_wheel_reset(wheel);

    asx_timer_handle h;
    int marker = 42;
    void *wakers[4];

    SCENARIO_CHECK(asx_timer_register(wheel, 100, &marker, &h) == ASX_OK, "timer_register");
    SCENARIO_CHECK(asx_timer_active_count(wheel) == 1, "active_count != 1");

    /* Advance past deadline */
    uint32_t fired = asx_timer_collect_expired(wheel, 101, wakers, 4);
    SCENARIO_CHECK(fired == 1, "expected 1 timer to fire");
    SCENARIO_CHECK(wakers[0] == &marker, "waker_data mismatch");
    SCENARIO_CHECK(asx_timer_active_count(wheel) == 0, "active_count != 0 after fire");

    SCENARIO_END();
}

static void scenario_timer_cancel_before_expire(void) {
    SCENARIO_BEGIN("timer.cancel_before_expire");

    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_wheel_reset(wheel);

    asx_timer_handle h;
    int marker = 99;
    void *wakers[4];

    SCENARIO_CHECK(asx_timer_register(wheel, 200, &marker, &h) == ASX_OK, "timer_register");

    int cancelled = asx_timer_cancel(wheel, &h);
    SCENARIO_CHECK(cancelled == 1, "cancel should return 1");
    SCENARIO_CHECK(asx_timer_active_count(wheel) == 0, "active after cancel");

    uint32_t fired = asx_timer_collect_expired(wheel, 300, wakers, 4);
    SCENARIO_CHECK(fired == 0, "cancelled timer should not fire");

    SCENARIO_END();
}

static void scenario_timer_deterministic_order(void) {
    SCENARIO_BEGIN("timer.deterministic_order");

    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_wheel_reset(wheel);

    int a = 1, b = 2, c = 3;
    asx_timer_handle ha, hb, hc;
    void *wakers[8];

    /* Same deadline — should fire in insertion order */
    SCENARIO_CHECK(asx_timer_register(wheel, 50, &a, &ha) == ASX_OK, "reg_a");
    SCENARIO_CHECK(asx_timer_register(wheel, 50, &b, &hb) == ASX_OK, "reg_b");
    SCENARIO_CHECK(asx_timer_register(wheel, 50, &c, &hc) == ASX_OK, "reg_c");

    uint32_t fired = asx_timer_collect_expired(wheel, 51, wakers, 8);
    SCENARIO_CHECK(fired == 3, "expected 3 timers");
    SCENARIO_CHECK(wakers[0] == &a && wakers[1] == &b && wakers[2] == &c,
                   "insertion order violated");

    SCENARIO_END();
}

static void scenario_channel_reserve_send_recv(void) {
    SCENARIO_BEGIN("channel.reserve_send_recv");
    asx_runtime_reset();
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit permit;
    uint64_t value;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 4, &cid) == ASX_OK, "channel_create");

    /* Reserve and send */
    SCENARIO_CHECK(asx_channel_try_reserve(cid, &permit) == ASX_OK, "try_reserve");
    SCENARIO_CHECK(asx_send_permit_send(&permit, 0xDEADBEEF) == ASX_OK, "permit_send");

    /* Receive */
    SCENARIO_CHECK(asx_channel_try_recv(cid, &value) == ASX_OK, "try_recv");
    SCENARIO_CHECK(value == 0xDEADBEEF, "value mismatch");

    /* Queue should be empty now */
    uint32_t qlen;
    SCENARIO_CHECK(asx_channel_queue_len(cid, &qlen) == ASX_OK && qlen == 0,
                   "queue not empty after recv");

    SCENARIO_END();
}

static void scenario_channel_reserve_abort(void) {
    SCENARIO_BEGIN("channel.reserve_abort");
    asx_runtime_reset();
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit permit;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 2, &cid) == ASX_OK, "channel_create");

    SCENARIO_CHECK(asx_channel_try_reserve(cid, &permit) == ASX_OK, "try_reserve");

    /* Abort — capacity should return */
    asx_send_permit_abort(&permit);

    uint32_t reserved;
    SCENARIO_CHECK(asx_channel_reserved_count(cid, &reserved) == ASX_OK && reserved == 0,
                   "reserved count not 0 after abort");

    SCENARIO_END();
}

static void scenario_channel_fifo_order(void) {
    SCENARIO_BEGIN("channel.fifo_order");
    asx_runtime_reset();
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit p;
    uint64_t v;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 8, &cid) == ASX_OK, "channel_create");

    /* Send 3 values */
    SCENARIO_CHECK(asx_channel_try_reserve(cid, &p) == ASX_OK, "reserve_1");
    SCENARIO_CHECK(asx_send_permit_send(&p, 10) == ASX_OK, "send_1");
    SCENARIO_CHECK(asx_channel_try_reserve(cid, &p) == ASX_OK, "reserve_2");
    SCENARIO_CHECK(asx_send_permit_send(&p, 20) == ASX_OK, "send_2");
    SCENARIO_CHECK(asx_channel_try_reserve(cid, &p) == ASX_OK, "reserve_3");
    SCENARIO_CHECK(asx_send_permit_send(&p, 30) == ASX_OK, "send_3");

    /* Receive in FIFO order */
    SCENARIO_CHECK(asx_channel_try_recv(cid, &v) == ASX_OK && v == 10, "fifo_1");
    SCENARIO_CHECK(asx_channel_try_recv(cid, &v) == ASX_OK && v == 20, "fifo_2");
    SCENARIO_CHECK(asx_channel_try_recv(cid, &v) == ASX_OK && v == 30, "fifo_3");

    SCENARIO_END();
}

static void scenario_channel_close_sender(void) {
    SCENARIO_BEGIN("channel.close_sender");
    asx_runtime_reset();
    asx_channel_reset();

    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit p;
    uint64_t v;
    asx_channel_state cs;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_channel_create(rid, 4, &cid) == ASX_OK, "channel_create");

    /* Send one value, then close sender */
    SCENARIO_CHECK(asx_channel_try_reserve(cid, &p) == ASX_OK, "reserve");
    SCENARIO_CHECK(asx_send_permit_send(&p, 42) == ASX_OK, "send");
    SCENARIO_CHECK(asx_channel_close_sender(cid) == ASX_OK, "close_sender");

    SCENARIO_CHECK(asx_channel_get_state(cid, &cs) == ASX_OK && cs == ASX_CHANNEL_SENDER_CLOSED,
                   "not SENDER_CLOSED");

    /* Can still receive pending messages */
    SCENARIO_CHECK(asx_channel_try_recv(cid, &v) == ASX_OK && v == 42, "recv after sender close");

    /* Next recv should report disconnected */
    asx_status rc = asx_channel_try_recv(cid, &v);
    SCENARIO_CHECK(rc == ASX_E_DISCONNECTED, "expected DISCONNECTED");

    SCENARIO_END();
}

static void scenario_trace_digest_deterministic(void) {
    SCENARIO_BEGIN("trace.digest_deterministic");
    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget = asx_budget_infinite();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn");
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");

    uint64_t digest1 = asx_trace_digest();

    /* Reset and replay identical scenario */
    asx_runtime_reset();
    asx_trace_reset();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_2");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn_2");
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run_2");

    uint64_t digest2 = asx_trace_digest();

    SCENARIO_CHECK(digest1 == digest2, "trace digest not deterministic");
    SCENARIO_CHECK(digest1 != 0, "trace digest is zero");

    SCENARIO_END();
}

static void scenario_region_poison_containment(void) {
    SCENARIO_BEGIN("region.poison_containment");
    asx_runtime_reset();

    asx_region_id rid;
    int poisoned;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    SCENARIO_CHECK(asx_region_is_poisoned(rid, &poisoned) == ASX_OK && poisoned == 0,
                   "should not be poisoned initially");

    SCENARIO_CHECK(asx_region_poison(rid) == ASX_OK, "region_poison");

    SCENARIO_CHECK(asx_region_is_poisoned(rid, &poisoned) == ASX_OK && poisoned == 1,
                   "should be poisoned");

    /* Spawn should fail on poisoned region */
    asx_task_id tid;
    asx_status rc = asx_task_spawn(rid, poll_complete, NULL, &tid);
    SCENARIO_CHECK(rc == ASX_E_REGION_POISONED, "spawn should fail on poisoned");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: drain progress snapshot (bd-1eqo.5.2)
 *
 * Verifies that asx_region_drain_progress reports accurate task,
 * obligation, and cleanup counters during a region drain lifecycle.
 * ------------------------------------------------------------------- */

static void scenario_drain_progress_snapshot(void) {
    SCENARIO_BEGIN("drain_progress.snapshot");

    asx_runtime_reset();
    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn two tasks and create one obligation */
    asx_task_id t1, t2;
    asx_obligation_id oid;
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &t1) == ASX_OK, "spawn_t1");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &t2) == ASX_OK, "spawn_t2");
    SCENARIO_CHECK(asx_obligation_reserve(rid, &oid) == ASX_OK, "reserve_obligation");

    /* Snapshot before drain: 2 live tasks, 1 reserved obligation */
    asx_drain_progress prog;
    SCENARIO_CHECK(asx_region_drain_progress(rid, &prog) == ASX_OK, "drain_progress_before");
    SCENARIO_CHECK(prog.region_state == ASX_REGION_OPEN, "state_open");
    SCENARIO_CHECK(prog.tasks_live == 2, "tasks_live_2");
    SCENARIO_CHECK(prog.obligations_total == 1, "obligations_total_1");
    SCENARIO_CHECK(prog.obligations_reserved == 1, "obligations_reserved_1");

    /* Resolve obligation and drain */
    SCENARIO_CHECK(asx_obligation_commit(oid) == ASX_OK, "commit_obligation");

    asx_budget budget = asx_budget_from_polls(128);
    IGNORE_RC(asx_region_drain(rid, &budget));

    /* Snapshot after drain */
    SCENARIO_CHECK(asx_region_drain_progress(rid, &prog) == ASX_OK, "drain_progress_after");
    SCENARIO_CHECK(prog.tasks_live == 0, "tasks_drained");
    SCENARIO_CHECK(prog.obligations_reserved == 0, "obligations_resolved");
    SCENARIO_CHECK(prog.cleanup_drained == 1, "cleanup_drained");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: quiescence evidence decomposition (bd-1eqo.5.2)
 *
 * Verifies that asx_quiescence_check_detailed decomposes quiescence
 * into Q1-Q4 conjuncts and reports correct per-condition status.
 * ------------------------------------------------------------------- */

static void scenario_quiescence_evidence(void) {
    SCENARIO_BEGIN("quiescence.evidence");

    asx_runtime_reset();
    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    /* Spawn a task (still READY, not yet polled) */
    asx_task_id tid;
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "spawn");

    /* Before drain: not quiescent (task still live, region open) */
    asx_quiescence_report report;
    SCENARIO_CHECK(asx_quiescence_check_detailed(rid, &report) == ASX_OK, "detailed_before");
    SCENARIO_CHECK(report.quiescent == 0, "not_quiescent_before");
    SCENARIO_CHECK(report.q1_tasks_complete == 0, "q1_false_before");

    /* Full drain — will run scheduler, cancel, finalize, close */
    asx_budget budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "drain_ok");

    /* After drain: fully quiescent */
    SCENARIO_CHECK(asx_quiescence_check_detailed(rid, &report) == ASX_OK, "detailed_after");
    SCENARIO_CHECK(report.quiescent == 1, "quiescent_after");
    SCENARIO_CHECK(report.q1_tasks_complete == 1, "q1_true");
    SCENARIO_CHECK(report.q2_children_closed == 1, "q2_true");
    SCENARIO_CHECK(report.q3_obligations_resolved == 1, "q3_true");
    SCENARIO_CHECK(report.q4_cleanup_drained == 1, "q4_true");
    SCENARIO_CHECK(report.region_state == ASX_REGION_CLOSED, "state_closed");

    /* Cross-validate with basic quiescence check */
    SCENARIO_CHECK(asx_quiescence_check(rid) == ASX_OK, "basic_quiescence_agrees");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    scenario_region_open_close();
    scenario_task_spawn_complete();
    scenario_task_coroutine_yields();
    scenario_obligation_reserve_commit();
    scenario_obligation_reserve_abort();
    scenario_cancel_checkpoint_finalize();
    scenario_cancel_propagate_region();
    scenario_quiescence_empty_region();
    scenario_quiescence_after_drain();
    scenario_timer_register_expire();
    scenario_timer_cancel_before_expire();
    scenario_timer_deterministic_order();
    scenario_channel_reserve_send_recv();
    scenario_channel_reserve_abort();
    scenario_channel_fifo_order();
    scenario_channel_close_sender();
    scenario_trace_digest_deterministic();
    scenario_region_poison_containment();
    scenario_drain_progress_snapshot();
    scenario_quiescence_evidence();

    fprintf(stderr, "[e2e] core_lifecycle: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
