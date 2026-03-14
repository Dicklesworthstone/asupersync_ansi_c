/*
 * test_record.c — unit tests for public record surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/obligation/obligation.h>
#include <asx/record/record.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <string.h>

static asx_runtime g_rt;

static void setup(void) {
    ASSERT_EQ(asx_runtime_init_default(&g_rt), ASX_OK);
    asx_event_log_reset();
}

static void teardown(void) { asx_runtime_shutdown(&g_rt); }

TEST(event_view_tracks_runtime_emissions) {
    asx_region_id region;
    asx_record_event event;

    setup();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_TRUE(asx_record_event_count() > 0u);
    ASSERT_TRUE(asx_record_event_get(0u, &event));
    ASSERT_EQ(event.kind, ASX_EVENT_REGION_OPEN);
    teardown();
}

TEST(summary_capture_reports_live_obligation_counts) {
    asx_region_id region;
    asx_obligation_id obligation;
    asx_record_summary summary;

    setup();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_obligation_open(region, &obligation), ASX_OK);
    ASSERT_EQ(asx_record_summary_capture(&summary), ASX_OK);
    ASSERT_EQ(summary.region_count, 1u);
    ASSERT_EQ(summary.obligation_count, 1u);
    ASSERT_TRUE(summary.event_count >= 2u);
    teardown();
}

TEST(snapshot_json_is_nonempty) {
    asx_region_id region;
    asx_record_snapshot snapshot;
    asx_codec_buffer buf;

    setup();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    asx_record_snapshot_init(&snapshot);
    ASSERT_EQ(asx_record_snapshot_capture(&snapshot), ASX_OK);
    asx_codec_buffer_init(&buf);
    ASSERT_EQ(asx_record_snapshot_json(&snapshot, &buf), ASX_OK);
    ASSERT_TRUE(buf.len > 0u);
    ASSERT_TRUE(strstr(buf.data, "\"regions\"") != NULL);
    asx_codec_buffer_reset(&buf);
    teardown();
}

int main(void) {
    RUN_TEST(event_view_tracks_runtime_emissions);
    RUN_TEST(summary_capture_reports_live_obligation_counts);
    RUN_TEST(snapshot_json_is_nonempty);
    TEST_REPORT();
    return test_failures;
}
