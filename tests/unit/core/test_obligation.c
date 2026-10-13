/*
 * test_obligation.c — unit tests for obligation arena and lifecycle
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include "test_runtime.h"
#include "test_task.h"
#include <asx/asx.h>
#include <asx/runtime/runtime.h>

/* ---- Reserve / commit / abort ----
 *
 * An obligation has a holder task, and only the holder's own poll reserves
 * or settles it, so the bodies below run as the poll of a task of the region
 * (test_task.h). A task resolves everything it reserves before it returns:
 * an obligation left reserved leaks when its holder completes. */

/* A holder that reserves one obligation, parks with it, and aborts it when
 * released: the host sees it reserved, and tries to settle it, from outside
 * the holder's poll. */
typedef struct {
    asx_region_id region;
    asx_obligation_id ob;
    asx_status reserve_status;
    int release;
} parked_holder;

static asx_status poll_parked_holder(void *user_data, asx_task_id self) {
    parked_holder *h = (parked_holder *)user_data;

    if (h->ob == ASX_INVALID_ID) {
        h->reserve_status = asx_obligation_reserve(h->region, &h->ob);
        if (h->reserve_status != ASX_OK) return ASX_OK;
    }
    if (!h->release) return asx_task_park(self) == ASX_OK ? ASX_E_PENDING : ASX_E_INVALID_STATE;
    return asx_obligation_abort(h->ob) == ASX_OK ? ASX_OK : ASX_E_INVALID_STATE;
}

TEST(obligation_reserve_and_get_state) {
    asx_region_id rid;
    asx_task_id tid;
    parked_holder h;
    asx_obligation_state state;
    asx_budget run;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    memset(&h, 0, sizeof(h));
    h.region = rid;
    h.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(rid, poll_parked_holder, &h, &tid), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(h.reserve_status, ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(h.ob, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_RESERVED);

    /* The holder resolves it in its own poll before it completes. */
    h.release = 1;
    ASSERT_EQ(asx_task_wake(tid), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(h.ob, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_ABORTED);
}

TEST(obligation_host_can_neither_reserve_nor_settle) {
    asx_region_id rid;
    asx_task_id tid;
    parked_holder h;
    asx_obligation_id other = ASX_INVALID_ID;
    asx_obligation_state state;
    asx_budget run;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    memset(&h, 0, sizeof(h));
    h.region = rid;
    h.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(rid, poll_parked_holder, &h, &tid), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(h.reserve_status, ASX_OK);

    /* Outside a poll the host holds no authority: it cannot reserve, and it
     * cannot settle the holder's obligation. Nothing changes. */
    ASSERT_EQ(asx_obligation_reserve(rid, &other), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(other, ASX_INVALID_ID);
    ASSERT_EQ(asx_obligation_commit(h.ob), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_obligation_abort(h.ob), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_obligation_get_state(h.ob, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_RESERVED);

    h.release = 1;
    ASSERT_EQ(asx_task_wake(tid), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(h.ob, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_ABORTED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)0u);
}

static void commit_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oid;
    asx_obligation_state state;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(oid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_COMMITTED);
}

TEST(obligation_commit) {
    asx_region_id rid;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, commit_body, &rid), ASX_OK);
}

static void abort_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oid;
    asx_obligation_state state;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(oid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_ABORTED);
}

TEST(obligation_abort) {
    asx_region_id rid;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, abort_body, &rid), ASX_OK);
}

/* ---- Terminal state absorbing ---- */

static void commit_is_terminal_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oid;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    /* Cannot commit again */
    ASSERT_NE(asx_obligation_commit(oid), ASX_OK);
    /* Cannot abort after commit */
    ASSERT_NE(asx_obligation_abort(oid), ASX_OK);
}

TEST(obligation_commit_is_terminal) {
    asx_region_id rid;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, commit_is_terminal_body, &rid), ASX_OK);
}

static void abort_is_terminal_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oid;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);
    /* Cannot abort again */
    ASSERT_NE(asx_obligation_abort(oid), ASX_OK);
    /* Cannot commit after abort */
    ASSERT_NE(asx_obligation_commit(oid), ASX_OK);
}

TEST(obligation_abort_is_terminal) {
    asx_region_id rid;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, abort_is_terminal_body, &rid), ASX_OK);
}

/* ---- Error cases ---- */

TEST(obligation_reserve_null_out) {
    asx_region_id rid;
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(obligation_reserve_invalid_region) {
    asx_obligation_id oid;
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_obligation_reserve(ASX_INVALID_ID, &oid), ASX_E_NOT_FOUND);
}

static void get_state_null_out_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oid;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(oid, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);
}

TEST(obligation_get_state_null_out) {
    asx_region_id rid;
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, get_state_null_out_body, &rid), ASX_OK);
}

TEST(obligation_invalid_handle) {
    asx_obligation_state state;
    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_obligation_get_state(ASX_INVALID_ID, &state), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_commit(ASX_INVALID_ID), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_abort(ASX_INVALID_ID), ASX_E_NOT_FOUND);
}

/* ---- Region admission ---- */

TEST(obligation_reserve_rejected_after_close) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_budget budget;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_E_REGION_CLOSED);

    /* Also rejected after drain */
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_NE(asx_obligation_reserve(rid, &oid), ASX_OK);
}

/* ---- Handle correctness ---- */

static void handle_type_tag_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oid;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_handle_type_tag(oid), ASX_TYPE_OBLIGATION);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);
}

TEST(obligation_handle_type_tag) {
    asx_region_id rid;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, handle_type_tag_body, &rid), ASX_OK);
}

static void multiple_in_region_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id o1, o2, o3;
    asx_obligation_state state;

    ASSERT_EQ(asx_obligation_reserve(rid, &o1), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &o2), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &o3), ASX_OK);

    /* Each has independent state */
    ASSERT_EQ(asx_obligation_commit(o1), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(o3), ASX_OK);

    ASSERT_EQ(asx_obligation_get_state(o1, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_COMMITTED);

    ASSERT_EQ(asx_obligation_get_state(o2, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_RESERVED);

    ASSERT_EQ(asx_obligation_get_state(o3, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_ABORTED);

    /* The one left reserved is resolved before the task completes. */
    ASSERT_EQ(asx_obligation_abort(o2), ASX_OK);
}

TEST(obligation_multiple_in_region) {
    asx_region_id rid;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, multiple_in_region_body, &rid), ASX_OK);
}

/* ---- Held time (Rust's duration_ns on commit/abort/leak, bd-yqe4) ----
 *
 * With no hooks installed the runtime clock is the virtual one, which these
 * tests move with asx_runtime_virtual_advance. */

/* The virtual clock is moved from inside the holder's poll: it is absolute
 * and nothing in a run of one runnable task moves it. */
static void commit_reports_held_time_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oid;
    asx_obligation_info info;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);

    /* Still reserved: nothing held yet, however much time has passed. */
    asx_runtime_virtual_advance(3000u);
    ASSERT_EQ(asx_obligation_get_info(oid, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_RESERVED);
    ASSERT_EQ(info.held_ns, (asx_time)0u);

    asx_runtime_virtual_advance(6000u);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(oid, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_COMMITTED);
    /* Reserved at 1000, committed at 6000. */
    ASSERT_EQ(info.held_ns, (asx_time)5000u);
}

TEST(obligation_commit_reports_held_time) {
    asx_region_id rid;

    asx_runtime_reset(test_runtime());
    asx_runtime_virtual_advance(1000u);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, commit_reports_held_time_body, &rid), ASX_OK);
}

static void abort_reports_held_time_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oid;
    asx_obligation_info info;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    asx_runtime_virtual_advance(2100u);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(oid, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
    ASSERT_EQ(info.held_ns, (asx_time)2000u);
}

TEST(obligation_abort_reports_held_time) {
    asx_region_id rid;

    asx_runtime_reset(test_runtime());
    asx_runtime_virtual_advance(100u);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, abort_reports_held_time_body, &rid), ASX_OK);
}

static void resolved_at_the_reserve_instant_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id oid;
    asx_obligation_info info;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(oid, &info), ASX_OK);
    /* Reserved late in virtual time is not held for that time. */
    ASSERT_EQ(info.held_ns, (asx_time)0u);
}

TEST(obligation_resolved_at_the_reserve_instant_was_held_for_zero) {
    asx_region_id rid;

    asx_runtime_reset(test_runtime());
    asx_runtime_virtual_advance(4242u);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, resolved_at_the_reserve_instant_body, &rid), ASX_OK);
}

static void held_time_is_per_obligation_body(void *ctx) {
    asx_region_id rid = *(asx_region_id *)ctx;
    asx_obligation_id first;
    asx_obligation_id second;
    asx_obligation_info info;

    ASSERT_EQ(asx_obligation_reserve(rid, &first), ASX_OK);
    asx_runtime_virtual_advance(500u);
    ASSERT_EQ(asx_obligation_reserve(rid, &second), ASX_OK);
    asx_runtime_virtual_advance(800u);
    ASSERT_EQ(asx_obligation_commit(first), ASX_OK);
    asx_runtime_virtual_advance(1500u);
    ASSERT_EQ(asx_obligation_abort(second), ASX_OK);

    ASSERT_EQ(asx_obligation_get_info(first, &info), ASX_OK);
    ASSERT_EQ(info.held_ns, (asx_time)800u); /* 0 -> 800 */
    ASSERT_EQ(asx_obligation_get_info(second, &info), ASX_OK);
    ASSERT_EQ(info.held_ns, (asx_time)1000u); /* 500 -> 1500 */
}

TEST(obligation_held_time_is_per_obligation) {
    asx_region_id rid;

    asx_runtime_reset(test_runtime());
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(test_run_in_task(rid, held_time_is_per_obligation_body, &rid), ASX_OK);
}

/* A task that reserves, lets virtual time pass, and completes with the
 * obligation unresolved. */
typedef struct {
    asx_region_id region;
    asx_obligation_id ob;
    asx_time advance_to;
} holding_task;

static asx_status poll_hold_then_complete(void *user_data, asx_task_id self) {
    holding_task *h = (holding_task *)user_data;
    (void)self;
    if (asx_obligation_reserve(h->region, &h->ob) != ASX_OK) return ASX_E_INVALID_STATE;
    asx_runtime_virtual_advance(h->advance_to);
    return ASX_OK;
}

TEST(obligation_leak_at_holder_completion_reports_held_time) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_region_id rid;
    asx_task_id tid;
    holding_task h;
    asx_obligation_info info;
    asx_budget run;

    /* A runtime with hooks installed, so the clock is read through them
     * (the default table's logical clock is the virtual one), and with the
     * LOG policy named rather than assumed: this test is about the held
     * time, not about what a leak does. */
    asx_runtime_reset(test_runtime());
    asx_runtime_config_init(&cfg);
    cfg.leak_response = ASX_LEAK_LOG;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_OK);
    ASSERT_EQ(asx_region_open(&rt, &rid), ASX_OK);
    memset(&h, 0, sizeof(h));
    h.region = rid;
    h.ob = ASX_INVALID_ID;
    h.advance_to = 7000u;
    ASSERT_EQ(asx_task_spawn(rid, poll_hold_then_complete, &h, &tid), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_OK);

    /* The holder completed with the obligation reserved: it leaked, and the
     * leak is stamped with how long it had been held (0 -> 7000). */
    ASSERT_EQ(asx_obligation_get_info(h.ob, &info), ASX_OK);
    ASSERT_EQ(info.holder, tid);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(info.held_ns, (asx_time)7000u);
    asx_runtime_shutdown(&rt);
}

int main(void) {
    fprintf(stderr, "=== test_obligation ===\n");
    RUN_TEST(obligation_reserve_and_get_state);
    RUN_TEST(obligation_host_can_neither_reserve_nor_settle);
    RUN_TEST(obligation_commit);
    RUN_TEST(obligation_abort);
    RUN_TEST(obligation_commit_is_terminal);
    RUN_TEST(obligation_abort_is_terminal);
    RUN_TEST(obligation_reserve_null_out);
    RUN_TEST(obligation_reserve_invalid_region);
    RUN_TEST(obligation_get_state_null_out);
    RUN_TEST(obligation_invalid_handle);
    RUN_TEST(obligation_reserve_rejected_after_close);
    RUN_TEST(obligation_handle_type_tag);
    RUN_TEST(obligation_multiple_in_region);
    RUN_TEST(obligation_commit_reports_held_time);
    RUN_TEST(obligation_abort_reports_held_time);
    RUN_TEST(obligation_resolved_at_the_reserve_instant_was_held_for_zero);
    RUN_TEST(obligation_held_time_is_per_obligation);
    RUN_TEST(obligation_leak_at_holder_completion_reports_held_time);
    TEST_REPORT();
    return test_failures;
}
