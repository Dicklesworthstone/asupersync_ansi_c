/*
 * vignette_session_link.c — API ergonomics vignette: session link/record flow
 *
 * Exercises: public session link coordination, obligation tracking, record
 * summary capture, and JSON snapshot export in one realistic smoke lane.
 *
 * bd-yx9r.5.3 — obligation / record / session / link public parity
 * SPDX-License-Identifier: MIT
 */
/* ASX_CHECKPOINT_WAIVER_FILE("vignette: no kernel loops") */

#include <asx/asx.h>
#include <stdio.h>

static int scenario_session_link_roundtrip(void) {
    asx_region_id region;
    asx_session_link link;
    asx_session_link_summary summary;
    asx_obligation_id obligation;
    asx_obligation_record record;
    asx_record_summary rec_summary;
    asx_record_snapshot snapshot;
    asx_codec_buffer json;
    uint64_t request;
    uint64_t response;
    asx_status st;

    printf("--- scenario: session link roundtrip with record capture ---\n");

    asx_runtime_reset();
    asx_event_log_reset();

    st = asx_region_open(&region);
    if (st != ASX_OK) {
        printf("  FAIL: region_open returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_session_link_open(region, 4u, &link);
    if (st != ASX_OK) {
        printf("  FAIL: session_link_open returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_obligation_open(region, &obligation);
    if (st != ASX_OK) {
        printf("  FAIL: obligation_open returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_session_link_send_request(&link, 111u);
    if (st != ASX_OK) {
        printf("  FAIL: send_request returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_session_link_recv_request(&link, &request);
    if (st != ASX_OK || request != 111u) {
        printf("  FAIL: recv_request returned %s value=%llu\n", asx_status_str(st),
               (unsigned long long)request);
        return 1;
    }

    st = asx_session_link_send_response(&link, 222u);
    if (st != ASX_OK) {
        printf("  FAIL: send_response returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_session_link_recv_response(&link, &response);
    if (st != ASX_OK || response != 222u) {
        printf("  FAIL: recv_response returned %s value=%llu\n", asx_status_str(st),
               (unsigned long long)response);
        return 1;
    }

    st = asx_obligation_resolve_commit(obligation);
    if (st != ASX_OK) {
        printf("  FAIL: obligation_commit returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_obligation_capture(obligation, &record);
    if (st != ASX_OK) {
        printf("  FAIL: obligation_capture returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_session_link_capture_summary(&link, &summary);
    if (st != ASX_OK) {
        printf("  FAIL: session_link_summary returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_record_summary_capture(&rec_summary);
    if (st != ASX_OK) {
        printf("  FAIL: record_summary returned %s\n", asx_status_str(st));
        return 1;
    }

    asx_record_snapshot_init(&snapshot);
    st = asx_record_snapshot_capture(&snapshot);
    if (st != ASX_OK) {
        printf("  FAIL: snapshot_capture returned %s\n", asx_status_str(st));
        return 1;
    }

    asx_codec_buffer_init(&json);
    st = asx_record_snapshot_json(&snapshot, &json);
    if (st != ASX_OK) {
        printf("  FAIL: snapshot_json returned %s\n", asx_status_str(st));
        asx_codec_buffer_reset(&json);
        return 1;
    }

    printf("  session link: state=%s req_sent=%u req_recv=%u resp_sent=%u resp_recv=%u "
           "outstanding=%u\n",
           asx_session_link_state_name(summary.state), summary.requests_sent,
           summary.requests_received, summary.responses_sent, summary.responses_received,
           summary.outstanding_obligations);
    printf("  obligation: id=%llu state=%s\n", (unsigned long long)record.id,
           asx_obligation_state_name(record.state));
    printf("  record: events=%u hash=%llu regions=%u tasks=%u obligations=%u\n",
           rec_summary.event_count, (unsigned long long)rec_summary.event_hash,
           rec_summary.region_count, rec_summary.task_count, rec_summary.obligation_count);
    printf("  snapshot-json: %s\n", json.data);

    asx_codec_buffer_reset(&json);
    asx_session_link_close(&link);
    printf("  PASS: session link roundtrip\n");
    return 0;
}

int main(void) {
    int failures = 0;

    printf("=== vignette: session link ===\n\n");
    failures += scenario_session_link_roundtrip();
    printf("\n=== session link: %d failures ===\n", failures);
    return failures;
}
