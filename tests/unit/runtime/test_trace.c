/*
 * test_trace.c — unit tests for deterministic event trace, replay, and snapshot
 *
 * Tests: trace emission, digest computation, replay verification,
 * snapshot export, and deterministic identity across runs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/runtime/hindsight.h>
#include <asx/runtime/parallel.h>
#include <asx/runtime/snapshot.h>
#include <asx/runtime/telemetry.h>
#include <asx/runtime/trace.h>
#include <asx/time/timer_wheel.h>
#include <string.h>

static asx_status poll_complete(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

static asx_status poll_pending(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

static uint64_t task_transition_aux(asx_task_state from, asx_task_state to) {
    return ((uint64_t)(uint32_t)from << 32) | (uint64_t)(uint32_t)to;
}

static uint64_t timer_trace_entity_id(const asx_timer_handle *handle) {
    return ((uint64_t)handle->slot << 32) | (uint64_t)handle->generation;
}

/* ---- Trace emission ---- */

TEST(trace_emit_records_events) {
    asx_trace_event ev;

    asx_trace_reset();

    asx_trace_emit(ASX_TRACE_REGION_OPEN, 0x1000, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 0x2000, 0x1000);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 0x2000, 0);

    ASSERT_EQ(asx_trace_event_count(), (uint32_t)3);

    ASSERT_TRUE(asx_trace_event_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_REGION_OPEN);
    ASSERT_EQ(ev.entity_id, (uint64_t)0x1000);
    ASSERT_EQ(ev.sequence, (uint32_t)0);

    ASSERT_TRUE(asx_trace_event_get(1, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_TASK_SPAWN);
    ASSERT_EQ(ev.aux, (uint64_t)0x1000);
    ASSERT_EQ(ev.sequence, (uint32_t)1);

    ASSERT_TRUE(asx_trace_event_get(2, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_SCHED_POLL);
    ASSERT_EQ(ev.sequence, (uint32_t)2);
}

TEST(trace_reset_clears) {
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 0, 0);
    ASSERT_TRUE(asx_trace_event_count() > (uint32_t)0);

    asx_trace_reset();
    ASSERT_EQ(asx_trace_event_count(), (uint32_t)0);
}

TEST(trace_get_out_of_bounds) {
    asx_trace_event ev;
    asx_trace_reset();

    ASSERT_FALSE(asx_trace_event_get(0, &ev));
    ASSERT_FALSE(asx_trace_event_get(0, NULL));
}

TEST(trace_monotonic_sequence) {
    asx_trace_event e0, e1, e2;
    asx_trace_reset();

    asx_trace_emit(ASX_TRACE_REGION_OPEN, 1, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 2, 1);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 2, 0);

    ASSERT_TRUE(asx_trace_event_get(0, &e0));
    ASSERT_TRUE(asx_trace_event_get(1, &e1));
    ASSERT_TRUE(asx_trace_event_get(2, &e2));

    ASSERT_TRUE(e0.sequence < e1.sequence);
    ASSERT_TRUE(e1.sequence < e2.sequence);
}

/* ---- Digest computation ---- */

TEST(trace_digest_deterministic) {
    uint64_t d1, d2;

    /* Run 1 */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 100, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 100, 0);
    d1 = asx_trace_digest();

    /* Run 2 (identical) */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 100, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 100, 0);
    d2 = asx_trace_digest();

    ASSERT_EQ(d1, d2);
}

TEST(trace_digest_differs_on_different_events) {
    uint64_t d1, d2;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 100, 0);
    d1 = asx_trace_digest();

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 100, 0);
    d2 = asx_trace_digest();

    ASSERT_TRUE(d1 != d2);
}

TEST(trace_digest_empty_is_stable) {
    uint64_t d1, d2;

    asx_trace_reset();
    d1 = asx_trace_digest();

    asx_trace_reset();
    d2 = asx_trace_digest();

    ASSERT_EQ(d1, d2);
}

/* ---- Replay verification ---- */

TEST(replay_match_identical_sequence) {
    asx_trace_event ref[3];
    asx_replay_result result;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 42, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 42, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);

    /* Copy trace as reference */
    asx_trace_event_get(0, &ref[0]);
    asx_trace_event_get(1, &ref[1]);
    asx_trace_event_get(2, &ref[2]);

    ASSERT_EQ(asx_replay_load_reference(ref, 3), ASX_OK);

    /* Replay with same events */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 42, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 42, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_MATCH);

    asx_replay_clear_reference();
}

TEST(replay_detects_length_mismatch) {
    asx_trace_event ref[2];
    asx_replay_result result;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 1, 0);

    asx_trace_event_get(0, &ref[0]);
    asx_trace_event_get(1, &ref[1]);

    ASSERT_EQ(asx_replay_load_reference(ref, 2), ASX_OK);

    /* Replay with extra event */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_LENGTH_MISMATCH);

    asx_replay_clear_reference();
}

TEST(replay_detects_kind_mismatch) {
    asx_trace_event ref[2];
    asx_replay_result result;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 1, 0);

    asx_trace_event_get(0, &ref[0]);
    asx_trace_event_get(1, &ref[1]);

    ASSERT_EQ(asx_replay_load_reference(ref, 2), ASX_OK);

    /* Replay with different event kind */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_BUDGET, 1, 0); /* wrong kind */

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_KIND_MISMATCH);
    ASSERT_EQ(result.divergence_index, (uint32_t)1);

    asx_replay_clear_reference();
}

TEST(replay_detects_entity_mismatch) {
    asx_trace_event ref[1];
    asx_replay_result result;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 42, 0);
    asx_trace_event_get(0, &ref[0]);

    ASSERT_EQ(asx_replay_load_reference(ref, 1), ASX_OK);

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 99, 0); /* wrong entity */

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_ENTITY_MISMATCH);
    ASSERT_EQ(result.divergence_index, (uint32_t)0);

    asx_replay_clear_reference();
}

TEST(replay_no_reference_is_match) {
    asx_replay_result result;

    asx_replay_clear_reference();
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_MATCH);
}

TEST(replay_reference_rejects_over_capacity) {
    asx_trace_event ref[1];

    ref[0].sequence = 0;
    ref[0].kind = ASX_TRACE_SCHED_POLL;
    ref[0].entity_id = 1;
    ref[0].aux = 0;

    ASSERT_EQ(asx_replay_load_reference(ref, ASX_TRACE_CAPACITY + 1u), ASX_E_INVALID_ARGUMENT);
}

/* ---- Snapshot export ---- */

TEST(snapshot_capture_empty) {
    asx_snapshot_buffer snap;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_snapshot_capture(&snap), ASX_OK);
    ASSERT_TRUE(snap.len > (uint32_t)0);
    /* Should contain JSON structure markers */
    ASSERT_TRUE(snap.data[0] == '{');
}

TEST(snapshot_capture_with_region) {
    asx_snapshot_buffer snap;
    asx_region_id rid;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_snapshot_capture(&snap), ASX_OK);

    /* Should mention regions */
    ASSERT_TRUE(snap.len > (uint32_t)20);
}

TEST(snapshot_capture_uses_runtime_snapshot_entities) {
    asx_snapshot_buffer legacy;
    asx_runtime_snapshot typed;
    asx_region_id rid;
    asx_task_id tid;
    asx_obligation_id oid;
    char needle[64];

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);

    ASSERT_EQ(asx_runtime_snapshot_capture(&typed), ASX_OK);
    ASSERT_EQ(typed.region_count, (uint32_t)1);
    ASSERT_EQ(typed.task_count, (uint32_t)1);
    ASSERT_EQ(typed.obligation_count, (uint32_t)1);

    ASSERT_EQ(asx_snapshot_capture(&legacy), ASX_OK);

    snprintf(needle, sizeof(needle), "\"slot\":%u", (unsigned)asx_handle_slot(typed.regions[0].id));
    ASSERT_TRUE(strstr(legacy.data, needle) != NULL);
    snprintf(needle, sizeof(needle), "\"tasks\":%u", (unsigned)typed.regions[0].task_count);
    ASSERT_TRUE(strstr(legacy.data, needle) != NULL);
    snprintf(needle, sizeof(needle), "\"gen\":%u",
             (unsigned)asx_handle_generation(typed.tasks[0].id));
    ASSERT_TRUE(strstr(legacy.data, needle) != NULL);
    snprintf(needle, sizeof(needle), "\"state\":%u", (unsigned)typed.obligations[0].state);
    ASSERT_TRUE(strstr(legacy.data, needle) != NULL);
}

TEST(snapshot_digest_deterministic) {
    asx_snapshot_buffer s1, s2;
    uint64_t d1, d2;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_snapshot_capture(&s1), ASX_OK);
    d1 = asx_snapshot_digest(&s1);

    /* Same state again */
    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_snapshot_capture(&s2), ASX_OK);
    d2 = asx_snapshot_digest(&s2);

    ASSERT_EQ(d1, d2);
}

TEST(snapshot_null_returns_error) { ASSERT_EQ(asx_snapshot_capture(NULL), ASX_E_INVALID_ARGUMENT); }

/* ---- Binary export/import ---- */

TEST(trace_binary_export_basic) {
    uint8_t buf[8192];
    uint32_t written = 0;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 0x1000, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 0x2000, 0x1000);

    ASSERT_EQ(asx_trace_export_binary(buf, sizeof(buf), &written), ASX_OK);
    /* Header(24) + 2 events * 24 = 72 */
    ASSERT_EQ(written, (uint32_t)72);
}

TEST(trace_binary_export_null_rejects) {
    uint32_t written = 0;
    uint8_t buf[128];

    ASSERT_EQ(asx_trace_export_binary(NULL, 128, &written), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_trace_export_binary(buf, 128, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(trace_binary_export_too_small) {
    uint8_t buf[10];
    uint32_t written = 0;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);

    ASSERT_EQ(asx_trace_export_binary(buf, sizeof(buf), &written), ASX_E_BUFFER_TOO_SMALL);
}

TEST(trace_binary_roundtrip) {
    uint8_t buf[8192];
    uint32_t written = 0;
    asx_replay_result result;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 42, 7);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 42, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);

    ASSERT_EQ(asx_trace_export_binary(buf, sizeof(buf), &written), ASX_OK);

    /* Import as reference */
    ASSERT_EQ(asx_trace_import_binary(buf, written), ASX_OK);

    /* Re-emit same events and verify */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 42, 7);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 42, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_MATCH);

    asx_replay_clear_reference();
}

TEST(trace_binary_import_null_rejects) {
    ASSERT_EQ(asx_trace_import_binary(NULL, 100), ASX_E_INVALID_ARGUMENT);
}

TEST(trace_binary_import_truncated) {
    uint8_t buf[10] = {0};
    ASSERT_EQ(asx_trace_import_binary(buf, 10), ASX_E_INVALID_ARGUMENT);
}

TEST(trace_continuity_check_match) {
    uint8_t buf[8192];
    uint32_t written = 0;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 1, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 2, 1);

    ASSERT_EQ(asx_trace_export_binary(buf, sizeof(buf), &written), ASX_OK);

    /* Same events still in trace ring → continuity check should pass */
    ASSERT_EQ(asx_trace_continuity_check(buf, written), ASX_OK);

    asx_replay_clear_reference();
}

TEST(trace_obligation_abort_emitted_by_runtime) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_trace_event ev;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);

    ASSERT_TRUE(asx_trace_event_get(asx_trace_event_count() - 1u, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_OBLIGATION_ABORT);
    ASSERT_EQ(ev.entity_id, (uint64_t)oid);
}

TEST(trace_region_closed_emitted_by_drain) {
    asx_region_id rid;
    asx_budget budget;
    asx_trace_event ev;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);

    ASSERT_TRUE(asx_trace_event_get(asx_trace_event_count() - 1u, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_REGION_CLOSED);
    ASSERT_EQ(ev.entity_id, (uint64_t)rid);
}

TEST(trace_task_transitions_emitted_by_scheduler) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_trace_event ev;
    uint32_t i;
    int saw_created_running = 0;
    int saw_running_completed = 0;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    for (i = 0; i < asx_trace_event_count(); i++) {
        ASSERT_TRUE(asx_trace_event_get(i, &ev));
        if (ev.kind != ASX_TRACE_TASK_TRANSITION) continue;
        if (ev.entity_id != (uint64_t)tid) continue;

        if (ev.aux == task_transition_aux(ASX_TASK_CREATED, ASX_TASK_RUNNING)) {
            saw_created_running = 1;
        }
        if (ev.aux == task_transition_aux(ASX_TASK_RUNNING, ASX_TASK_COMPLETED)) {
            saw_running_completed = 1;
        }
    }

    ASSERT_TRUE(saw_created_running);
    ASSERT_TRUE(saw_running_completed);
}

TEST(trace_task_transitions_emitted_by_cancel_api) {
    asx_region_id rid;
    asx_task_id tid;
    asx_trace_event ev;
    uint32_t i;
    int saw_created_running = 0;
    int saw_running_cancel_requested = 0;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    for (i = 0; i < asx_trace_event_count(); i++) {
        ASSERT_TRUE(asx_trace_event_get(i, &ev));
        if (ev.kind != ASX_TRACE_TASK_TRANSITION) continue;
        if (ev.entity_id != (uint64_t)tid) continue;

        if (ev.aux == task_transition_aux(ASX_TASK_CREATED, ASX_TASK_RUNNING)) {
            saw_created_running = 1;
        }
        if (ev.aux == task_transition_aux(ASX_TASK_RUNNING, ASX_TASK_CANCEL_REQUESTED)) {
            saw_running_cancel_requested = 1;
        }
    }

    ASSERT_TRUE(saw_created_running);
    ASSERT_TRUE(saw_running_cancel_requested);
}

TEST(trace_channel_events_emitted_by_runtime) {
    asx_region_id rid;
    asx_channel_id ch;
    asx_send_permit permit;
    asx_trace_event ev;
    uint64_t value;

    asx_runtime_reset();
    asx_channel_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    asx_trace_reset();

    ASSERT_EQ(asx_channel_create(rid, 4, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&permit, 77u), ASX_OK);
    ASSERT_EQ(asx_channel_try_recv(ch, &value), ASX_OK);
    ASSERT_EQ(value, 77u);

    ASSERT_EQ(asx_trace_event_count(), (uint32_t)2);

    ASSERT_TRUE(asx_trace_event_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_CHANNEL_SEND);
    ASSERT_EQ(ev.entity_id, (uint64_t)ch);
    ASSERT_EQ(ev.aux, (uint64_t)77);

    ASSERT_TRUE(asx_trace_event_get(1, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_CHANNEL_RECV);
    ASSERT_EQ(ev.entity_id, (uint64_t)ch);
    ASSERT_EQ(ev.aux, (uint64_t)77);
}

TEST(trace_timer_events_emitted_by_runtime) {
    asx_timer_wheel *wheel;
    asx_timer_handle fire_h, cancel_h;
    asx_trace_event ev;
    void *wakers[2];
    uint32_t count;

    wheel = asx_timer_wheel_global();
    asx_timer_wheel_reset(wheel);
    asx_trace_reset();

    ASSERT_EQ(asx_timer_register(wheel, 100, (void *)0x11, &fire_h), ASX_OK);
    ASSERT_EQ(asx_timer_register(wheel, 200, (void *)0x22, &cancel_h), ASX_OK);
    ASSERT_TRUE(asx_timer_cancel(wheel, &cancel_h));

    count = asx_timer_collect_expired(wheel, 100, wakers, 2);
    ASSERT_EQ(count, (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)0x11);

    ASSERT_EQ(asx_trace_event_count(), (uint32_t)4);

    ASSERT_TRUE(asx_trace_event_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_TIMER_SET);
    ASSERT_EQ(ev.entity_id, timer_trace_entity_id(&fire_h));
    ASSERT_EQ(ev.aux, (uint64_t)100);

    ASSERT_TRUE(asx_trace_event_get(1, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_TIMER_SET);
    ASSERT_EQ(ev.entity_id, timer_trace_entity_id(&cancel_h));
    ASSERT_EQ(ev.aux, (uint64_t)200);

    ASSERT_TRUE(asx_trace_event_get(2, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_TIMER_CANCEL);
    ASSERT_EQ(ev.entity_id, timer_trace_entity_id(&cancel_h));
    ASSERT_EQ(ev.aux, (uint64_t)200);

    ASSERT_TRUE(asx_trace_event_get(3, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_TIMER_FIRE);
    ASSERT_EQ(ev.entity_id, timer_trace_entity_id(&fire_h));
    ASSERT_EQ(ev.aux, (uint64_t)100);
}

TEST(runtime_reset_clears_global_support_state) {
    asx_parallel_config cfg = {0};
    asx_budget budget = asx_budget_infinite();
    asx_region_id rid;
    asx_channel_id ch;
    asx_send_permit permit;
    asx_timer_handle timer_handle;
    uint64_t value;

    cfg.worker_count = 1u;
    cfg.fairness = ASX_FAIRNESS_ROUND_ROBIN;
    cfg.lane_weights[0] = 1u;
    cfg.lane_weights[1] = 1u;
    cfg.lane_weights[2] = 1u;
    cfg.starvation_limit = 8u;

    asx_runtime_reset();

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 2, &ch), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(ch, &permit), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&permit, 55u), ASX_OK);
    ASSERT_EQ(asx_timer_register(asx_timer_wheel_global(), 100, (void *)0x1, &timer_handle),
              ASX_OK);
    ASSERT_EQ(asx_telemetry_set_tier(ASX_TELEMETRY_OPS_LIGHT), ASX_OK);
    asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, 1u, 0u);
    asx_hindsight_log(ASX_ND_CLOCK_READ, 1u, 99u);

    ASSERT_TRUE(asx_trace_event_count() > (uint32_t)0);
    ASSERT_TRUE(asx_timer_active_count(asx_timer_wheel_global()) > (uint32_t)0);
    ASSERT_EQ(asx_telemetry_emitted_count(), (uint32_t)1);
    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)1);

    asx_runtime_reset();

    ASSERT_EQ(asx_trace_event_count(), (uint32_t)0);
    ASSERT_EQ(asx_timer_active_count(asx_timer_wheel_global()), (uint32_t)0);
    ASSERT_EQ(asx_telemetry_get_tier(), ASX_TELEMETRY_FORENSIC);
    ASSERT_EQ(asx_telemetry_emitted_count(), (uint32_t)0);
    ASSERT_EQ(asx_telemetry_filtered_count(), (uint32_t)0);
    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)0);
    ASSERT_EQ(asx_hindsight_readable_count(), (uint32_t)0);
    ASSERT_EQ(asx_parallel_run(ASX_INVALID_ID, &budget), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_channel_try_recv(ch, &value), ASX_E_NOT_FOUND);
}

/* ---- Aux mismatch detection ---- */

TEST(replay_detects_aux_mismatch) {
    asx_trace_event ref[1];
    asx_replay_result result;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 42, 100);
    asx_trace_event_get(0, &ref[0]);

    ASSERT_EQ(asx_replay_load_reference(ref, 1), ASX_OK);

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 42, 999); /* wrong aux */

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_AUX_MISMATCH);
    ASSERT_EQ(result.divergence_index, (uint32_t)0);

    asx_replay_clear_reference();
}

/* ---- Ring buffer wrap ---- */

TEST(trace_ring_drops_beyond_capacity) {
    uint32_t i;
    asx_trace_event ev;

    asx_trace_reset();

    /* Fill beyond capacity — events past cap are silently dropped */
    for (i = 0; i < ASX_TRACE_CAPACITY + 10u; i++) {
        asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)i, 0);
    }

    /* Readable count is capped at capacity */
    ASSERT_EQ(asx_trace_event_count(), ASX_TRACE_CAPACITY);

    /* First event is still index 0 (no wrap — fill-once ring) */
    ASSERT_TRUE(asx_trace_event_get(0, &ev));
    ASSERT_EQ(ev.entity_id, (uint64_t)0);

    /* Last readable is capacity - 1 */
    ASSERT_TRUE(asx_trace_event_get(ASX_TRACE_CAPACITY - 1, &ev));
    ASSERT_EQ(ev.entity_id, (uint64_t)(ASX_TRACE_CAPACITY - 1));

    /* Index beyond capacity returns false */
    ASSERT_TRUE(!asx_trace_event_get(ASX_TRACE_CAPACITY, &ev));
}

/* ---- Digest sensitivity to aux ---- */

TEST(trace_digest_sensitive_to_aux) {
    uint64_t d1, d2;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 100);
    d1 = asx_trace_digest();

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 200);
    d2 = asx_trace_digest();

    ASSERT_TRUE(d1 != d2);
}

TEST(trace_digest_sensitive_to_entity_id) {
    uint64_t d1, d2;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    d1 = asx_trace_digest();

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 2, 0);
    d2 = asx_trace_digest();

    ASSERT_TRUE(d1 != d2);
}

TEST(trace_digest_sensitive_to_order) {
    uint64_t d1, d2;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 2, 0);
    d1 = asx_trace_digest();

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 2, 0);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    d2 = asx_trace_digest();

    ASSERT_TRUE(d1 != d2);
}

/* ---- All event kinds emit ---- */

TEST(trace_all_event_kinds) {
    asx_trace_event ev;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 0, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 0, 0);
    asx_trace_emit(ASX_TRACE_SCHED_BUDGET, 0, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);
    asx_trace_emit(ASX_TRACE_SCHED_ROUND, 0, 0);
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 0, 0);
    asx_trace_emit(ASX_TRACE_REGION_CLOSE, 0, 0);
    asx_trace_emit(ASX_TRACE_REGION_CLOSED, 0, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 0, 0);
    asx_trace_emit(ASX_TRACE_TASK_TRANSITION, 0, 0);
    asx_trace_emit(ASX_TRACE_OBLIGATION_RESERVE, 0, 0);
    asx_trace_emit(ASX_TRACE_OBLIGATION_COMMIT, 0, 0);
    asx_trace_emit(ASX_TRACE_OBLIGATION_ABORT, 0, 0);
    asx_trace_emit(ASX_TRACE_CHANNEL_SEND, 0, 0);
    asx_trace_emit(ASX_TRACE_CHANNEL_RECV, 0, 0);
    asx_trace_emit(ASX_TRACE_TIMER_SET, 0, 0);
    asx_trace_emit(ASX_TRACE_TIMER_FIRE, 0, 0);
    asx_trace_emit(ASX_TRACE_TIMER_CANCEL, 0, 0);

    ASSERT_EQ(asx_trace_event_count(), (uint32_t)18);

    /* Spot check a few kinds */
    ASSERT_TRUE(asx_trace_event_get(5, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_REGION_OPEN);

    ASSERT_TRUE(asx_trace_event_get(15, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_TIMER_SET);
}

/* ---- String helpers ---- */

TEST(trace_event_kind_str_all_kinds) {
    ASSERT_TRUE(asx_trace_event_kind_str(ASX_TRACE_SCHED_POLL) != NULL);
    ASSERT_TRUE(asx_trace_event_kind_str(ASX_TRACE_REGION_OPEN) != NULL);
    ASSERT_TRUE(asx_trace_event_kind_str(ASX_TRACE_OBLIGATION_COMMIT) != NULL);
    ASSERT_TRUE(asx_trace_event_kind_str(ASX_TRACE_TIMER_FIRE) != NULL);
}

TEST(replay_result_kind_str_all_kinds) {
    ASSERT_TRUE(asx_replay_result_kind_str(ASX_REPLAY_MATCH) != NULL);
    ASSERT_TRUE(asx_replay_result_kind_str(ASX_REPLAY_LENGTH_MISMATCH) != NULL);
    ASSERT_TRUE(asx_replay_result_kind_str(ASX_REPLAY_DIGEST_MISMATCH) != NULL);
}

int main(void) {
    fprintf(stderr, "=== test_trace ===\n");

    RUN_TEST(trace_emit_records_events);
    RUN_TEST(trace_reset_clears);
    RUN_TEST(trace_get_out_of_bounds);
    RUN_TEST(trace_monotonic_sequence);
    RUN_TEST(trace_digest_deterministic);
    RUN_TEST(trace_digest_differs_on_different_events);
    RUN_TEST(trace_digest_empty_is_stable);
    RUN_TEST(replay_match_identical_sequence);
    RUN_TEST(replay_detects_length_mismatch);
    RUN_TEST(replay_detects_kind_mismatch);
    RUN_TEST(replay_detects_entity_mismatch);
    RUN_TEST(replay_no_reference_is_match);
    RUN_TEST(replay_reference_rejects_over_capacity);
    RUN_TEST(snapshot_capture_empty);
    RUN_TEST(snapshot_capture_with_region);
    RUN_TEST(snapshot_capture_uses_runtime_snapshot_entities);
    RUN_TEST(snapshot_digest_deterministic);
    RUN_TEST(snapshot_null_returns_error);
    RUN_TEST(trace_binary_export_basic);
    RUN_TEST(trace_binary_export_null_rejects);
    RUN_TEST(trace_binary_export_too_small);
    RUN_TEST(trace_binary_roundtrip);
    RUN_TEST(trace_binary_import_null_rejects);
    RUN_TEST(trace_binary_import_truncated);
    RUN_TEST(trace_continuity_check_match);
    RUN_TEST(trace_obligation_abort_emitted_by_runtime);
    RUN_TEST(trace_region_closed_emitted_by_drain);
    RUN_TEST(trace_task_transitions_emitted_by_scheduler);
    RUN_TEST(trace_task_transitions_emitted_by_cancel_api);
    RUN_TEST(trace_channel_events_emitted_by_runtime);
    RUN_TEST(trace_timer_events_emitted_by_runtime);
    RUN_TEST(runtime_reset_clears_global_support_state);
    RUN_TEST(replay_detects_aux_mismatch);
    RUN_TEST(trace_ring_drops_beyond_capacity);
    RUN_TEST(trace_digest_sensitive_to_aux);
    RUN_TEST(trace_digest_sensitive_to_entity_id);
    RUN_TEST(trace_digest_sensitive_to_order);
    RUN_TEST(trace_all_event_kinds);
    RUN_TEST(trace_event_kind_str_all_kinds);
    RUN_TEST(replay_result_kind_str_all_kinds);

    TEST_REPORT();
    return test_failures;
}
