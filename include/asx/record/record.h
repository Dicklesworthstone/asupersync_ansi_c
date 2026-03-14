/*
 * asx/record/record.h — public execution record helpers
 *
 * Groups runtime event-log and snapshot surfaces into a dedicated
 * record family for diagnostics, proofs, and smoke reporting.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RECORD_RECORD_H
#define ASX_RECORD_RECORD_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/codec/codec.h>
#include <asx/runtime/event.h>
#include <asx/runtime/snapshot.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef asx_event_record asx_record_event;
typedef asx_runtime_snapshot asx_record_snapshot;

typedef struct {
    uint32_t event_count;
    uint64_t event_hash;
    uint32_t region_count;
    uint32_t task_count;
    uint32_t obligation_count;
} asx_record_summary;

static inline void asx_record_snapshot_init(asx_record_snapshot *snapshot) {
    asx_runtime_snapshot_init(snapshot);
}

static inline ASX_MUST_USE asx_status asx_record_snapshot_capture(asx_record_snapshot *snapshot) {
    return asx_runtime_snapshot_capture(snapshot);
}

static inline ASX_MUST_USE asx_status asx_record_snapshot_json(
    const asx_record_snapshot *snapshot, asx_codec_buffer *out_json) {
    return asx_runtime_snapshot_to_json(snapshot, out_json);
}

static inline uint32_t asx_record_event_count(void) { return asx_event_log_count(); }

static inline int asx_record_event_get(uint32_t index, asx_record_event *out_event) {
    return asx_event_log_get(index, out_event);
}

static inline ASX_MUST_USE asx_status asx_record_event_log_json(asx_codec_buffer *out_json) {
    return asx_event_log_to_json(out_json);
}

static inline ASX_MUST_USE asx_status asx_record_summary_capture(asx_record_summary *out_summary) {
    asx_record_snapshot snapshot;
    asx_status st;

    if (out_summary == NULL) return ASX_E_INVALID_ARGUMENT;
    asx_record_snapshot_init(&snapshot);
    st = asx_record_snapshot_capture(&snapshot);
    if (st != ASX_OK) return st;

    out_summary->event_count = asx_record_event_count();
    out_summary->event_hash = snapshot.event_hash;
    out_summary->region_count = snapshot.region_count;
    out_summary->task_count = snapshot.task_count;
    out_summary->obligation_count = snapshot.obligation_count;
    return ASX_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* ASX_RECORD_RECORD_H */
