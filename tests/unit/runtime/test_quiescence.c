/*
 * test_quiescence.c — focused unit tests for quiescence.c
 *
 * Exercises branch-level quiescence/finalization behavior that broader
 * lifecycle suites only cover indirectly.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/cleanup.h>
#include <asx/core/ghost.h>
#include <asx/runtime/trace.h>
#include "../../../src/runtime/runtime_internal.h"

/* Suppress warn_unused_result for intentionally-ignored scheduler calls. */
#define SCHED_RUN_IGNORE(rid, bud) \
    do { asx_status s_ = asx_scheduler_run((rid), (bud)); (void)s_; } while (0)

static asx_status poll_pending(void *data, asx_task_id self)
{
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

static asx_status poll_checkpoint_then_complete(void *data, asx_task_id self)
{
    asx_checkpoint_result cr;
    (void)data;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) {
        return ASX_OK;
    }
    return ASX_E_PENDING;
}

static void cleanup_mark(void *ctx)
{
    int *flag = (int *)ctx;
    *flag += 1;
}

TEST(quiescence_closed_region_with_live_tasks_reports_tasks_live)
{
    asx_region_id rid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 1;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_E_QUIESCENCE_TASKS_LIVE);
}

TEST(quiescence_closed_region_with_unresolved_obligation_reports_error)
{
    asx_region_id rid;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_E_OBLIGATIONS_UNRESOLVED);
}

TEST(region_drain_advances_draining_region_to_closed_and_runs_cleanup)
{
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;
    asx_budget budget;
    asx_trace_event ev;
    uint32_t event_count;
    int cleaned = 0;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_DRAINING;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &handle), ASX_OK);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
    ASSERT_EQ(cleaned, 1);
    event_count = asx_trace_event_count();
    ASSERT_TRUE(event_count > 0u);
    ASSERT_TRUE(asx_trace_event_get(event_count - 1u, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_REGION_CLOSED);
    ASSERT_EQ(ev.entity_id, rid);
}

TEST(region_drain_recancels_uncancelled_tasks_before_scheduler_run)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_RUNNING);

    budget = asx_budget_from_polls(16);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(region_drain_budget_exhaustion_leaves_region_closing)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_region_slot *region = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    budget = asx_budget_from_polls(0);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(region->state, ASX_REGION_CLOSING);
    ASSERT_TRUE(region->task_count > 0);
}

/* --- New tests covering additional branches --- */

TEST(quiescence_invalid_handle_returns_not_found)
{
    asx_region_id bad = 0xDEADBEEFu;

    asx_runtime_reset();

    ASSERT_EQ(asx_quiescence_check(bad), ASX_E_NOT_FOUND);
}

TEST(quiescence_open_region_returns_not_reached)
{
    asx_region_id rid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_E_QUIESCENCE_NOT_REACHED);
}

TEST(quiescence_success_after_drain_no_tasks)
{
    asx_region_id rid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(quiescence_success_with_committed_obligation)
{
    asx_region_id rid;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(quiescence_success_with_aborted_obligation)
{
    asx_region_id rid;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(drain_null_budget_returns_invalid_argument)
{
    asx_region_id rid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_drain(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(drain_invalid_handle_returns_not_found)
{
    asx_region_id bad = 0xDEADBEEFu;
    asx_budget budget;

    asx_runtime_reset();

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(bad, &budget), ASX_E_NOT_FOUND);
}

TEST(drain_multiple_tasks_all_complete)
{
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_task_state state;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t3), ASX_OK);

    /* Prime all tasks to RUNNING state */
    budget = asx_budget_from_polls(3);
    SCHED_RUN_IGNORE(rid, &budget);

    budget = asx_budget_from_polls(64);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_task_get_state(t1, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(t2, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(t3, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(drain_cleanup_lifo_multiple)
{
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle h1 = ASX_CLEANUP_INVALID;
    asx_cleanup_handle h2 = ASX_CLEANUP_INVALID;
    asx_cleanup_handle h3 = ASX_CLEANUP_INVALID;
    asx_budget budget;
    int c1 = 0, c2 = 0, c3 = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_DRAINING;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &c1, &h1), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &c2, &h2), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &c3, &h3), ASX_OK);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
    ASSERT_EQ(c1, 1);
    ASSERT_EQ(c2, 1);
    ASSERT_EQ(c3, 1);
}

TEST(drain_already_closed_is_idempotent)
{
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
}

TEST(drain_full_lifecycle_tasks_obligations_cleanup)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;
    asx_budget budget;
    int cleaned = 0;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &handle), ASX_OK);

    /* Prime task to RUNNING */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Full drain */
    budget = asx_budget_from_polls(64);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
    ASSERT_EQ(cleaned, 1);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

int main(void)
{
    fprintf(stderr, "=== test_quiescence ===\n");

    asx_runtime_reset(); RUN_TEST(quiescence_closed_region_with_live_tasks_reports_tasks_live);
    asx_runtime_reset(); RUN_TEST(quiescence_closed_region_with_unresolved_obligation_reports_error);
    asx_runtime_reset(); RUN_TEST(region_drain_advances_draining_region_to_closed_and_runs_cleanup);
    asx_runtime_reset(); RUN_TEST(region_drain_recancels_uncancelled_tasks_before_scheduler_run);
    asx_runtime_reset(); RUN_TEST(region_drain_budget_exhaustion_leaves_region_closing);
    asx_runtime_reset(); RUN_TEST(quiescence_invalid_handle_returns_not_found);
    asx_runtime_reset(); RUN_TEST(quiescence_open_region_returns_not_reached);
    asx_runtime_reset(); RUN_TEST(quiescence_success_after_drain_no_tasks);
    asx_runtime_reset(); RUN_TEST(quiescence_success_with_committed_obligation);
    asx_runtime_reset(); RUN_TEST(quiescence_success_with_aborted_obligation);
    asx_runtime_reset(); RUN_TEST(drain_null_budget_returns_invalid_argument);
    asx_runtime_reset(); RUN_TEST(drain_invalid_handle_returns_not_found);
    asx_runtime_reset(); RUN_TEST(drain_multiple_tasks_all_complete);
    asx_runtime_reset(); RUN_TEST(drain_cleanup_lifo_multiple);
    asx_runtime_reset(); RUN_TEST(drain_already_closed_is_idempotent);
    asx_runtime_reset(); RUN_TEST(drain_full_lifecycle_tasks_obligations_cleanup);

    TEST_REPORT();
    return test_failures;
}
