/*
 * test_cli.c — unit tests for CLI helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/cli/cli.h>
#include <asx/runtime/browser_boundary.h>

#if ASX_HAS_NATIVE_RUNTIME_SURFACES

TEST(cli_parse_output_format) {
    ASSERT_EQ(asx_cli_parse_output_format(NULL), ASX_OUTPUT_TEXT);
    ASSERT_EQ(asx_cli_parse_output_format("json"), ASX_OUTPUT_JSON);
    ASSERT_EQ(asx_cli_parse_output_format("json-pretty"), ASX_OUTPUT_JSON_PRETTY);
    ASSERT_EQ(asx_cli_parse_output_format("unknown"), ASX_OUTPUT_TEXT);
}

TEST(cli_output_format_str) {
    ASSERT_STR_EQ(asx_output_format_str(ASX_OUTPUT_TEXT), "text");
    ASSERT_STR_EQ(asx_output_format_str(ASX_OUTPUT_JSON), "json");
    ASSERT_STR_EQ(asx_output_format_str(ASX_OUTPUT_JSON_PRETTY), "json-pretty");
}

TEST(cli_config_init_defaults) {
    asx_cli_config cfg;

    asx_cli_config_init(&cfg);
    ASSERT_EQ(cfg.output_format, ASX_OUTPUT_TEXT);
    ASSERT_EQ(cfg.color_mode, ASX_COLOR_MODE_NONE);
    ASSERT_EQ(cfg.verbose, 0u);
    ASSERT_EQ(cfg.quiet, 0u);
    ASSERT_FALSE(asx_cli_should_colorize(&cfg));
    ASSERT_FALSE(asx_cli_is_verbose(&cfg));
    ASSERT_FALSE(asx_cli_is_quiet(&cfg));
}

TEST(cli_progress_lifecycle) {
    asx_cli_progress progress;

    asx_cli_progress_start(&progress, "sync", 8u);
    ASSERT_FALSE(asx_cli_progress_is_done(&progress));
    ASSERT_EQ(asx_cli_progress_percentage(&progress), 0u);

    asx_cli_progress_update(&progress, 2u);
    ASSERT_EQ(asx_cli_progress_percentage(&progress), 25u);
    ASSERT_FALSE(asx_cli_progress_is_done(&progress));

    asx_cli_progress_complete(&progress);
    ASSERT_EQ(asx_cli_progress_percentage(&progress), 100u);
    ASSERT_TRUE(asx_cli_progress_is_done(&progress));
}

TEST(cli_progress_zero_total_is_safe) {
    asx_cli_progress progress;

    asx_cli_progress_start(&progress, "noop", 0u);
    ASSERT_EQ(asx_cli_progress_percentage(&progress), 0u);
    ASSERT_FALSE(asx_cli_progress_is_done(&progress));
    asx_cli_progress_complete(&progress);
    ASSERT_TRUE(asx_cli_progress_is_done(&progress));
}

TEST(cli_signal_tracking) {
    asx_cli_reset_signal_state();
    ASSERT_EQ(asx_cli_signal_count(), 0u);
    ASSERT_FALSE(asx_cli_is_cancelled());
    ASSERT_FALSE(asx_cli_should_force_quit());

    asx_cli_record_signal(2);
    ASSERT_EQ(asx_cli_signal_count(), 1u);
    ASSERT_TRUE(asx_cli_is_cancelled());
    ASSERT_FALSE(asx_cli_should_force_quit());

    asx_cli_record_signal(15);
    ASSERT_EQ(asx_cli_signal_count(), 2u);
    ASSERT_TRUE(asx_cli_should_force_quit());

    asx_cli_reset_signal_state();
    ASSERT_EQ(asx_cli_signal_count(), 0u);
}

#else

TEST(cli_surface_compile_time_hidden_in_browser) {
    ASSERT_EQ(ASX_HAS_NATIVE_RUNTIME_SURFACES, 0);
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_FILESYSTEM));
    ASSERT_EQ(asx_surface_gate(ASX_SURFACE_FILESYSTEM), ASX_E_PERMISSION_DENIED);
}

#endif

int main(void) {
#if ASX_HAS_NATIVE_RUNTIME_SURFACES
    RUN_TEST(cli_parse_output_format);
    RUN_TEST(cli_output_format_str);
    RUN_TEST(cli_config_init_defaults);
    RUN_TEST(cli_progress_lifecycle);
    RUN_TEST(cli_progress_zero_total_is_safe);
    RUN_TEST(cli_signal_tracking);
#else
    RUN_TEST(cli_surface_compile_time_hidden_in_browser);
#endif
    TEST_REPORT();
    return test_failures;
}
