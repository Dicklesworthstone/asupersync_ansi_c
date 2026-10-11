/*
 * test_trace.c — unit tests for deterministic event trace, replay, and snapshot
 *
 * Tests: trace emission, digest computation, replay verification,
 * snapshot export, and deterministic identity across runs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include "../../test_io_fd.h"
#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/runtime/hindsight.h>
#include <asx/runtime/parallel.h>
#include <asx/runtime/snapshot.h>
#include <asx/runtime/telemetry.h>
#include <asx/runtime/trace.h>
#include <asx/time/timer_wheel.h>
#include <string.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_TRACE

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

/* ---- Trace schema contract ---- */

TEST(trace_schema_current_descriptor_is_versioned) {
    asx_trace_schema_descriptor desc;

    memset(&desc, 0, sizeof(desc));
    ASSERT_EQ(asx_trace_schema_current(&desc), ASX_OK);

    ASSERT_TRUE(strcmp(desc.schema_name, ASX_TRACE_SCHEMA_NAME) == 0);
    ASSERT_TRUE(strcmp(desc.schema_version, ASX_TRACE_SCHEMA_VERSION) == 0);
    ASSERT_EQ(desc.version_major, ASX_TRACE_SCHEMA_VERSION_MAJOR);
    ASSERT_EQ(desc.version_minor, ASX_TRACE_SCHEMA_VERSION_MINOR);
    ASSERT_EQ(desc.version_patch, ASX_TRACE_SCHEMA_VERSION_PATCH);
    ASSERT_EQ(desc.required_event_fields, (uint32_t)ASX_TRACE_SCHEMA_REQUIRED_EVENT_FIELDS);
    ASSERT_EQ(desc.digest_event_fields, (uint32_t)ASX_TRACE_SCHEMA_DIGEST_EVENT_FIELDS);
    ASSERT_TRUE((desc.optional_event_fields & ASX_TRACE_SCHEMA_FIELD_RUST_CORRELATION) != 0u);
}

TEST(trace_schema_current_rejects_null_output) {
    ASSERT_EQ(asx_trace_schema_current(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(trace_schema_exact_compatibility) {
    asx_trace_schema_descriptor producer;
    asx_trace_schema_descriptor consumer;

    ASSERT_EQ(asx_trace_schema_current(&producer), ASX_OK);
    ASSERT_EQ(asx_trace_schema_current(&consumer), ASX_OK);

    ASSERT_EQ(asx_trace_schema_compatibility(&producer, &consumer), ASX_TRACE_SCHEMA_COMPAT_EXACT);
    ASSERT_TRUE(strcmp(asx_trace_schema_compat_str(ASX_TRACE_SCHEMA_COMPAT_EXACT), "exact") == 0);
}

TEST(trace_schema_allows_additive_optional_version) {
    asx_trace_schema_descriptor producer;
    asx_trace_schema_descriptor consumer;

    ASSERT_EQ(asx_trace_schema_current(&producer), ASX_OK);
    ASSERT_EQ(asx_trace_schema_current(&consumer), ASX_OK);
    producer.schema_version = "asx.trace_event.v1.1";
    producer.version_minor = consumer.version_minor + 1u;

    ASSERT_EQ(asx_trace_schema_compatibility(&producer, &consumer),
              ASX_TRACE_SCHEMA_COMPAT_ADDITIVE_OPTIONAL);
    ASSERT_TRUE(strcmp(asx_trace_schema_compat_str(ASX_TRACE_SCHEMA_COMPAT_ADDITIVE_OPTIONAL),
                       "additive_optional") == 0);
}

TEST(trace_schema_rejects_required_or_digest_drift) {
    asx_trace_schema_descriptor producer;
    asx_trace_schema_descriptor consumer;

    ASSERT_EQ(asx_trace_schema_current(&producer), ASX_OK);
    ASSERT_EQ(asx_trace_schema_current(&consumer), ASX_OK);

    producer.required_event_fields &= ~((uint32_t)ASX_TRACE_SCHEMA_FIELD_AUX);
    ASSERT_EQ(asx_trace_schema_compatibility(&producer, &consumer),
              ASX_TRACE_SCHEMA_COMPAT_INCOMPATIBLE);

    ASSERT_EQ(asx_trace_schema_current(&producer), ASX_OK);
    producer.digest_event_fields &= ~((uint32_t)ASX_TRACE_SCHEMA_FIELD_AUX);
    ASSERT_EQ(asx_trace_schema_compatibility(&producer, &consumer),
              ASX_TRACE_SCHEMA_COMPAT_INCOMPATIBLE);

    ASSERT_EQ(asx_trace_schema_current(&producer), ASX_OK);
    producer.version_major = consumer.version_major + 1u;
    ASSERT_EQ(asx_trace_schema_compatibility(&producer, &consumer),
              ASX_TRACE_SCHEMA_COMPAT_INCOMPATIBLE);
}

TEST(trace_schema_rejects_unversioned_descriptors) {
    asx_trace_schema_descriptor producer;
    asx_trace_schema_descriptor consumer;

    ASSERT_EQ(asx_trace_schema_current(&producer), ASX_OK);
    ASSERT_EQ(asx_trace_schema_current(&consumer), ASX_OK);

    producer.schema_version = NULL;
    ASSERT_EQ(asx_trace_schema_compatibility(&producer, &consumer),
              ASX_TRACE_SCHEMA_COMPAT_INCOMPATIBLE);

    ASSERT_EQ(asx_trace_schema_current(&producer), ASX_OK);
    producer.version_major = 0u;
    ASSERT_EQ(asx_trace_schema_compatibility(&producer, &consumer),
              ASX_TRACE_SCHEMA_COMPAT_INCOMPATIBLE);

    ASSERT_EQ(asx_trace_schema_compatibility(NULL, &consumer),
              ASX_TRACE_SCHEMA_COMPAT_INCOMPATIBLE);
    ASSERT_TRUE(strcmp(asx_trace_schema_compat_str(ASX_TRACE_SCHEMA_COMPAT_INCOMPATIBLE),
                       "incompatible") == 0);
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

TEST(replay_reference_event_accessors_round_trip) {
    asx_trace_event ref[2];
    asx_trace_event got;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 5u, 9u);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 5u, 0u);
    ASSERT_TRUE(asx_trace_event_get(0u, &ref[0]));
    ASSERT_TRUE(asx_trace_event_get(1u, &ref[1]));

    ASSERT_EQ(asx_replay_load_reference(ref, 2u), ASX_OK);
    ASSERT_EQ(asx_replay_reference_event_count(), 2u);
    ASSERT_TRUE(asx_replay_reference_event_get(1u, &got));
    ASSERT_EQ(got.kind, ASX_TRACE_SCHED_COMPLETE);
    ASSERT_EQ(got.entity_id, (uint64_t)5u);
    ASSERT_FALSE(asx_replay_reference_event_get(2u, &got));

    asx_replay_clear_reference();
    ASSERT_EQ(asx_replay_reference_event_count(), 0u);
    ASSERT_FALSE(asx_replay_reference_event_get(0u, &got));
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
    asx_runtime rt;
    asx_waker w;
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_io_token tok;
#endif
    asx_region_id rid;
    asx_task_id tid;
    asx_obligation_id oid;
    char needle[64];

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    asx_ghost_reset();
    asx_trace_reset();
    ASSERT_EQ(asx_waker_register(77, &w), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_io_driver_is_initialized()) {
        ASSERT_EQ(asx_io_register(test_io_fd(42), ASX_IO_READABLE, &w, &tok), ASX_OK);
    }
#endif

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
    snprintf(needle, sizeof(needle), "\"io_driver_initialized\":%u",
             (unsigned)typed.io_driver_initialized);
    ASSERT_TRUE(strstr(legacy.data, needle) != NULL);
    snprintf(needle, sizeof(needle), "\"io_registration_count\":%u",
             (unsigned)typed.io_registration_count);
    ASSERT_TRUE(strstr(legacy.data, needle) != NULL);
    snprintf(needle, sizeof(needle), "\"blocking_pool_initialized\":%u",
             (unsigned)typed.blocking_pool_initialized);
    ASSERT_TRUE(strstr(legacy.data, needle) != NULL);
    snprintf(needle, sizeof(needle), "\"blocking_active_count\":%u",
             (unsigned)typed.blocking_active_count);
    ASSERT_TRUE(strstr(legacy.data, needle) != NULL);

#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_io_driver_is_initialized()) { asx_io_deregister(&tok); }
#endif
    asx_runtime_shutdown(&rt);
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
    /* Header(32) + 2 events * 24 = 80 */
    ASSERT_EQ(written, (uint32_t)(ASX_TRACE_BINARY_HEADER + 2u * ASX_TRACE_BINARY_EVENT));
    ASSERT_EQ(written, (uint32_t)80);
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
    /* A cancel before the first poll takes the task from Created to
     * CancelRequested in one traced step (Rust request_cancel*,
     * record/task.rs:803-816); polling it never makes it Running. */
    asx_region_id rid;
    asx_task_id tid;
    asx_trace_event ev;
    uint32_t i;
    uint32_t transitions = 0;
    int saw_created_cancel_requested = 0;

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

        transitions++;
        if (ev.aux == task_transition_aux(ASX_TASK_CREATED, ASX_TASK_CANCEL_REQUESTED)) {
            saw_created_cancel_requested = 1;
        }
    }

    ASSERT_TRUE(saw_created_cancel_requested);
    ASSERT_EQ(transitions, (uint32_t)1u);
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

TEST(trace_ring_keeps_most_recent_beyond_capacity) {
    uint32_t i;
    asx_trace_event ev;

    asx_trace_reset();

    /* Fill beyond capacity: the ring keeps the most recent events */
    for (i = 0; i < ASX_TRACE_CAPACITY + 10u; i++) {
        asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)i, 0);
    }

    ASSERT_EQ(asx_trace_event_count(), ASX_TRACE_CAPACITY);
    ASSERT_EQ(asx_trace_emitted_total(), (uint64_t)(ASX_TRACE_CAPACITY + 10u));
    ASSERT_EQ(asx_trace_dropped(), (uint64_t)10);

    /* Index 0 is the oldest retained event: sequence 10 */
    ASSERT_TRUE(asx_trace_event_get(0, &ev));
    ASSERT_EQ(ev.entity_id, (uint64_t)10);
    ASSERT_EQ(ev.sequence, (uint32_t)10);

    /* Last retained is the last emitted */
    ASSERT_TRUE(asx_trace_event_get(ASX_TRACE_CAPACITY - 1, &ev));
    ASSERT_EQ(ev.entity_id, (uint64_t)(ASX_TRACE_CAPACITY + 9u));

    /* Index beyond the retained window returns false */
    ASSERT_TRUE(!asx_trace_event_get(ASX_TRACE_CAPACITY, &ev));

    /* Absolute-sequence access: dropped events are gone, retained ones resolve */
    ASSERT_TRUE(!asx_trace_event_get_seq(9, &ev));
    ASSERT_TRUE(asx_trace_event_get_seq(10, &ev));
    ASSERT_EQ(ev.entity_id, (uint64_t)10);
    ASSERT_TRUE(asx_trace_event_get_seq(ASX_TRACE_CAPACITY + 9u, &ev));
    ASSERT_TRUE(!asx_trace_event_get_seq(ASX_TRACE_CAPACITY + 10u, &ev));
}

/* RB1 regression: before the rolling digest, events past capacity were
 * not hashed, so two traces that diverged after event 1024 had the same
 * digest and replay verification could not see the divergence. */
static uint64_t trace_digest_of_run(uint32_t total, uint32_t perturb_at) {
    uint32_t i;
    asx_trace_reset();
    for (i = 0; i < total; i++) {
        uint64_t aux = (i == perturb_at) ? 0xBADu : 0u;
        asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)i, aux);
    }
    return asx_trace_digest();
}

TEST(trace_digest_covers_events_beyond_capacity) {
    uint32_t total = ASX_TRACE_CAPACITY * 5u;
    uint64_t base = trace_digest_of_run(total, UINT32_MAX);

    /* Identical long runs digest identically */
    ASSERT_EQ(trace_digest_of_run(total, UINT32_MAX), base);
    /* A change far beyond the ring changes the digest */
    ASSERT_TRUE(trace_digest_of_run(total, total - 1u) != base);
    ASSERT_TRUE(trace_digest_of_run(total, ASX_TRACE_CAPACITY + 1u) != base);
    /* So does a change in the dropped prefix */
    ASSERT_TRUE(trace_digest_of_run(total, 0u) != base);
    /* And a different length */
    ASSERT_TRUE(trace_digest_of_run(total + 1u, UINT32_MAX) != base);
}

TEST(trace_digest_within_capacity_matches_fold_of_events) {
    asx_trace_event events[3];
    uint32_t i;
    uint64_t live;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 0x1000, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 0x2000, 0x1000);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 0x2000, 7);
    live = asx_trace_digest();

    for (i = 0; i < 3u; i++) ASSERT_TRUE(asx_trace_event_get(i, &events[i]));
    ASSERT_EQ(asx_replay_load_reference(events, 3), ASX_OK);
    {
        asx_replay_result r = asx_replay_verify();
        ASSERT_EQ(r.result, ASX_REPLAY_MATCH);
        ASSERT_EQ(r.expected_digest, live);
        ASSERT_EQ(r.actual_digest, live);
    }
    asx_replay_clear_reference();
}

/* The digest contract, byte by byte: FNV-1a steps from the asx digest
 * seed over each event's sequence (u32), kind (u32), entity_id (u64) and
 * aux (u64), little-endian. The runtime folds runs of zero high bytes as
 * one multiply; these values put zeros high, low, in the middle and
 * nowhere, so a fold that differs from the byte loop changes the digest. */
static uint64_t fnv1a_le_reference(uint64_t hash, uint64_t v, uint32_t width) {
    uint32_t i;
    for (i = 0; i < width; i++) {
        hash ^= (v >> (8u * i)) & 0xFFu;
        hash *= 0x00000100000001B3ULL;
    }
    return hash;
}

TEST(trace_digest_is_fnv1a_over_le_event_fields) {
    static const uint64_t values[] = {0u,
                                      1u,
                                      0xFFu,
                                      0x100u,
                                      0x10000u,
                                      0xFF000000u,
                                      0x0001000000000000ULL,
                                      0x8000000000000000ULL,
                                      0x00FF00FF00FF00FFULL,
                                      0x1234000000005678ULL,
                                      0xFFFFFFFFFFFFFFFFULL};
    uint32_t n = (uint32_t)(sizeof(values) / sizeof(values[0]));
    uint64_t expected = 0x517cc1b727220a95ULL;
    uint32_t seq = 0, i, j;

    asx_trace_reset();
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            asx_trace_event_kind kind =
                (i % 2u == 0u) ? ASX_TRACE_SCHED_POLL : ASX_TRACE_TASK_SPAWN;
            asx_trace_emit(kind, values[i], values[j]);
            expected = fnv1a_le_reference(expected, seq, 4u);
            expected = fnv1a_le_reference(expected, (uint32_t)kind, 4u);
            expected = fnv1a_le_reference(expected, values[i], 8u);
            expected = fnv1a_le_reference(expected, values[j], 8u);
            seq++;
        }
    }
    ASSERT_EQ(asx_trace_digest(), expected);

    /* Sequences with zero and non-zero high bytes */
    for (i = seq; i < 300u; i++) asx_trace_emit(ASX_TRACE_SCHED_POLL, 0u, 0u);
    for (; seq < 300u; seq++) {
        expected = fnv1a_le_reference(expected, seq, 4u);
        expected = fnv1a_le_reference(expected, (uint32_t)ASX_TRACE_SCHED_POLL, 4u);
        expected = fnv1a_le_reference(expected, 0u, 8u);
        expected = fnv1a_le_reference(expected, 0u, 8u);
    }
    ASSERT_EQ(asx_trace_digest(), expected);
}

TEST(trace_kind_totals_exact_beyond_capacity) {
    uint32_t i;

    asx_trace_reset();
    for (i = 0; i < ASX_TRACE_CAPACITY * 3u; i++) {
        asx_trace_emit((i % 3u) == 0u ? ASX_TRACE_SCHED_POLL : ASX_TRACE_TASK_SPAWN, i, 0);
    }
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_SCHED_POLL), (uint64_t)ASX_TRACE_CAPACITY);
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_TASK_SPAWN), (uint64_t)(ASX_TRACE_CAPACITY * 2u));
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_TIMER_FIRE), (uint64_t)0);

    asx_trace_reset();
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_SCHED_POLL), (uint64_t)0);
    ASSERT_EQ(asx_trace_emitted_total(), (uint64_t)0);
}

static uint8_t
    g_long_trace_buf[ASX_TRACE_BINARY_HEADER + ASX_TRACE_CAPACITY * ASX_TRACE_BINARY_EVENT];

static void emit_long_trace(uint32_t total, uint32_t perturb_at) {
    uint32_t i;
    asx_trace_reset();
    for (i = 0; i < total; i++) {
        asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)i, i == perturb_at ? 1u : 0u);
    }
}

TEST(trace_truncated_export_roundtrip_and_continuity) {
    uint32_t written = 0;
    uint32_t total = ASX_TRACE_CAPACITY * 2u + 7u;

    emit_long_trace(total, UINT32_MAX);
    ASSERT_EQ(asx_trace_export_binary(g_long_trace_buf, sizeof(g_long_trace_buf), &written),
              ASX_OK);
    ASSERT_EQ(written, (uint32_t)sizeof(g_long_trace_buf));

    /* Same run again: continuity holds */
    emit_long_trace(total, UINT32_MAX);
    ASSERT_EQ(asx_trace_continuity_check(g_long_trace_buf, written), ASX_OK);

    /* A divergence inside the dropped prefix is invisible to the window
     * but caught by the rolling digest */
    emit_long_trace(total, 3u);
    ASSERT_EQ(asx_trace_import_binary(g_long_trace_buf, written), ASX_OK);
    {
        asx_replay_result r = asx_replay_verify();
        ASSERT_EQ(r.result, ASX_REPLAY_DIGEST_MISMATCH);
    }
    asx_replay_clear_reference();

    /* A divergence inside the window is reported at its sequence */
    emit_long_trace(total, total - 2u);
    ASSERT_EQ(asx_trace_import_binary(g_long_trace_buf, written), ASX_OK);
    {
        asx_replay_result r = asx_replay_verify();
        ASSERT_EQ(r.result, ASX_REPLAY_AUX_MISMATCH);
        ASSERT_EQ(r.divergence_index, total - 2u);
    }
    asx_replay_clear_reference();

    /* A longer run is a length mismatch */
    emit_long_trace(total + 1u, UINT32_MAX);
    ASSERT_EQ(asx_trace_continuity_check(g_long_trace_buf, written), ASX_E_REPLAY_MISMATCH);
}

TEST(trace_import_rejects_inconsistent_truncation) {
    uint32_t written = 0;

    emit_long_trace(ASX_TRACE_CAPACITY + 5u, UINT32_MAX);
    ASSERT_EQ(asx_trace_export_binary(g_long_trace_buf, sizeof(g_long_trace_buf), &written),
              ASX_OK);

    /* Clearing the TRUNCATED flag while count < total is rejected */
    g_long_trace_buf[12] = 0u;
    ASSERT_EQ(asx_trace_import_binary(g_long_trace_buf, written), ASX_E_INVALID_ARGUMENT);
    g_long_trace_buf[12] = (uint8_t)ASX_TRACE_BINARY_FLAG_TRUNCATED;
    ASSERT_EQ(asx_trace_import_binary(g_long_trace_buf, written), ASX_OK);
    asx_replay_clear_reference();

    /* A non-contiguous window (tampered first sequence) is rejected */
    g_long_trace_buf[ASX_TRACE_BINARY_HEADER] ^= 0x01u;
    ASSERT_EQ(asx_trace_import_binary(g_long_trace_buf, written), ASX_E_INVALID_ARGUMENT);
    g_long_trace_buf[ASX_TRACE_BINARY_HEADER] ^= 0x01u;

    /* Unknown flag bits are rejected */
    g_long_trace_buf[13] = 0x80u;
    ASSERT_EQ(asx_trace_import_binary(g_long_trace_buf, written), ASX_E_INVALID_ARGUMENT);
    g_long_trace_buf[13] = 0u;
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

    RUN_TEST(trace_schema_current_descriptor_is_versioned);
    RUN_TEST(trace_schema_current_rejects_null_output);
    RUN_TEST(trace_schema_exact_compatibility);
    RUN_TEST(trace_schema_allows_additive_optional_version);
    RUN_TEST(trace_schema_rejects_required_or_digest_drift);
    RUN_TEST(trace_schema_rejects_unversioned_descriptors);
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
    RUN_TEST(replay_reference_event_accessors_round_trip);
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
    RUN_TEST(trace_ring_keeps_most_recent_beyond_capacity);
    RUN_TEST(trace_digest_covers_events_beyond_capacity);
    RUN_TEST(trace_digest_within_capacity_matches_fold_of_events);
    RUN_TEST(trace_digest_is_fnv1a_over_le_event_fields);
    RUN_TEST(trace_kind_totals_exact_beyond_capacity);
    RUN_TEST(trace_truncated_export_roundtrip_and_continuity);
    RUN_TEST(trace_import_rejects_inconsistent_truncation);
    RUN_TEST(trace_digest_sensitive_to_aux);
    RUN_TEST(trace_digest_sensitive_to_entity_id);
    RUN_TEST(trace_digest_sensitive_to_order);
    RUN_TEST(trace_all_event_kinds);
    RUN_TEST(trace_event_kind_str_all_kinds);
    RUN_TEST(replay_result_kind_str_all_kinds);

    TEST_REPORT();
    return test_failures;
}

#else

TEST(trace_family_compile_time_hidden_in_minimal_browser) {
    ASSERT_EQ(ASX_HAS_BROWSER_TRACE, 0);
    ASSERT_EQ(ASX_HAS_BROWSER_TRACE_SUBPROFILE_SPLIT, 1);
}

int main(void) {
    fprintf(stderr, "=== test_trace (minimal browser hidden contract) ===\n");
    RUN_TEST(trace_family_compile_time_hidden_in_minimal_browser);
    TEST_REPORT();
    return test_failures;
}

#endif
