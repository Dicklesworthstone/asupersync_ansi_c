/*
 * cli.c — command-line interface utilities
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/cli/cli.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ASX_HAS_NATIVE_RUNTIME_SURFACES
/* ------------------------------------------------------------------ */
/* Output format                                                       */
/* ------------------------------------------------------------------ */

asx_output_format asx_cli_parse_output_format(const char *str) {
    if (str == NULL) return ASX_OUTPUT_TEXT;
    if (strcmp(str, "json") == 0) return ASX_OUTPUT_JSON;
    if (strcmp(str, "json-pretty") == 0) return ASX_OUTPUT_JSON_PRETTY;
    return ASX_OUTPUT_TEXT;
}

const char *asx_output_format_str(asx_output_format fmt) {
    switch (fmt) {
    case ASX_OUTPUT_TEXT: return "text";
    case ASX_OUTPUT_JSON: return "json";
    case ASX_OUTPUT_JSON_PRETTY: return "json-pretty";
    }
    return "text";
}

/* ------------------------------------------------------------------ */
/* CLI config                                                          */
/* ------------------------------------------------------------------ */

void asx_cli_config_init(asx_cli_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->output_format = ASX_OUTPUT_TEXT;
    cfg->color_mode = ASX_COLOR_MODE_NONE;
}

void asx_cli_config_from_env(asx_cli_config *cfg) {
    const char *val;

    if (cfg == NULL) return;
    asx_cli_config_init(cfg);

    val = getenv("ASX_FORMAT");
    if (val != NULL) cfg->output_format = asx_cli_parse_output_format(val);

    val = getenv("ASX_COLOR");
    if (val != NULL) {
        if (strcmp(val, "always") == 0 || strcmp(val, "1") == 0) {
            cfg->color_mode = ASX_COLOR_MODE_ANSI;
        } else if (strcmp(val, "never") == 0 || strcmp(val, "0") == 0) {
            cfg->color_mode = ASX_COLOR_MODE_NONE;
        }
    }

    val = getenv("ASX_VERBOSE");
    if (val != NULL && (strcmp(val, "1") == 0 || strcmp(val, "true") == 0)) { cfg->verbose = 1u; }

    val = getenv("ASX_QUIET");
    if (val != NULL && (strcmp(val, "1") == 0 || strcmp(val, "true") == 0)) { cfg->quiet = 1u; }
}

int asx_cli_should_colorize(const asx_cli_config *cfg) {
    if (cfg == NULL) return 0;
    return cfg->color_mode != ASX_COLOR_MODE_NONE;
}

int asx_cli_is_verbose(const asx_cli_config *cfg) {
    if (cfg == NULL) return 0;
    return cfg->verbose != 0u;
}

int asx_cli_is_quiet(const asx_cli_config *cfg) {
    if (cfg == NULL) return 0;
    return cfg->quiet != 0u;
}

/* ------------------------------------------------------------------ */
/* Progress                                                            */
/* ------------------------------------------------------------------ */

void asx_cli_progress_start(asx_cli_progress *p, const char *label, uint64_t total) {
    if (p == NULL) return;
    memset(p, 0, sizeof(*p));
    p->label = label;
    p->total = total;
    p->active = 1u;
}

void asx_cli_progress_update(asx_cli_progress *p, uint64_t completed) {
    if (p == NULL || !p->active) return;
    p->completed = completed;
    if (completed >= p->total) p->active = 0u;
}

void asx_cli_progress_complete(asx_cli_progress *p) {
    if (p == NULL) return;
    p->completed = p->total;
    p->active = 0u;
}

uint32_t asx_cli_progress_percentage(const asx_cli_progress *p) {
    if (p == NULL || p->total == 0u) return 0u;
    if (p->completed >= p->total) return 100u;
    return (uint32_t)((p->completed * 100u) / p->total);
}

int asx_cli_progress_is_done(const asx_cli_progress *p) {
    if (p == NULL) return 1;
    return !p->active;
}

/* ------------------------------------------------------------------ */
/* Error reporting                                                     */
/* ------------------------------------------------------------------ */

void asx_cli_report_error(const char *context, asx_status error) {
    if (context != NULL) {
        fprintf(stderr, "error: %s: %s\n", context, asx_status_str(error));
    } else {
        fprintf(stderr, "error: %s\n", asx_status_str(error));
    }
}

void asx_cli_report_warning(const char *message) {
    if (message != NULL) { fprintf(stderr, "warning: %s\n", message); }
}

void asx_cli_report_info(const asx_cli_config *cfg, const char *message) {
    if (cfg != NULL && cfg->quiet) return;
    if (message != NULL) { fprintf(stderr, "info: %s\n", message); }
}

/* ------------------------------------------------------------------ */
/* Signal tracking                                                     */
/* ------------------------------------------------------------------ */

static volatile uint32_t g_signal_count = 0u;

void asx_cli_record_signal(int signal_number) {
    (void)signal_number;
    g_signal_count++;
}

uint32_t asx_cli_signal_count(void) { return g_signal_count; }

int asx_cli_is_cancelled(void) { return g_signal_count > 0u; }

int asx_cli_should_force_quit(void) { return g_signal_count >= 2u; }

void asx_cli_reset_signal_state(void) { g_signal_count = 0u; }
#endif
