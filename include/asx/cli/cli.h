/*
 * asx/cli/cli.h — command-line interface utilities
 *
 * Provides argument parsing, output formatting, progress display,
 * signal handling for CLI applications, and error reporting.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CLI_CLI_H
#define ASX_CLI_CLI_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/console/console.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if ASX_HAS_NATIVE_RUNTIME_SURFACES
/* -------------------------------------------------------------------
 * Output format
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_OUTPUT_TEXT = 0,
    ASX_OUTPUT_JSON = 1,
    ASX_OUTPUT_JSON_PRETTY = 2
} asx_output_format;

/* Parse an output format string ("text", "json", "json-pretty") into an enum. */
ASX_API asx_output_format asx_cli_parse_output_format(const char *str);
/* Return the string representation of an output format. */
ASX_API const char *asx_output_format_str(asx_output_format fmt);

/* -------------------------------------------------------------------
 * CLI context
 * ------------------------------------------------------------------- */

typedef struct {
    asx_output_format output_format;
    asx_color_mode color_mode;
    uint8_t verbose;
    uint8_t quiet;
} asx_cli_config;

/* Initialize CLI config with defaults. */
ASX_API void asx_cli_config_init(asx_cli_config *cfg);

/* Parse CLI config from environment variables (ASX_FORMAT, ASX_COLOR, etc). */
ASX_API void asx_cli_config_from_env(asx_cli_config *cfg);

/* Check if output should use colors. */
ASX_API int asx_cli_should_colorize(const asx_cli_config *cfg);

/* Check verbosity. */
ASX_API int asx_cli_is_verbose(const asx_cli_config *cfg);
ASX_API int asx_cli_is_quiet(const asx_cli_config *cfg);

/* -------------------------------------------------------------------
 * Progress display
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t total;
    uint64_t completed;
    uint64_t started_ns;
    const char *label;
    uint8_t active;
} asx_cli_progress;

/* Start a progress tracker. */
ASX_API void asx_cli_progress_start(asx_cli_progress *p, const char *label, uint64_t total);

/* Update progress. */
ASX_API void asx_cli_progress_update(asx_cli_progress *p, uint64_t completed);

/* Complete the progress tracker. */
ASX_API void asx_cli_progress_complete(asx_cli_progress *p);

/* Get percentage (0-100). */
ASX_API uint32_t asx_cli_progress_percentage(const asx_cli_progress *p);

/* Check if complete. */
ASX_API int asx_cli_progress_is_done(const asx_cli_progress *p);

/* -------------------------------------------------------------------
 * CLI error reporting
 * ------------------------------------------------------------------- */

/* Report an error with context. */
ASX_API void asx_cli_report_error(const char *context, asx_status error);

/* Report a warning. */
ASX_API void asx_cli_report_warning(const char *message);

/* Report an informational message (suppressed in quiet mode). */
ASX_API void asx_cli_report_info(const asx_cli_config *cfg, const char *message);

/* -------------------------------------------------------------------
 * Signal tracking for CLI (ctrl-C, SIGTERM)
 * ------------------------------------------------------------------- */

/* Record that a signal was received. Thread-safe (uses volatile). */
ASX_API void asx_cli_record_signal(int signal_number);

/* Get the number of signals received. */
ASX_API uint32_t asx_cli_signal_count(void);

/* Check if cancellation was requested (signal count > 0). */
ASX_API int asx_cli_is_cancelled(void);

/* Check if force quit should happen (signal count >= 2). */
ASX_API int asx_cli_should_force_quit(void);

/* Reset signal state (for testing). */
ASX_API void asx_cli_reset_signal_state(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ASX_CLI_CLI_H */
