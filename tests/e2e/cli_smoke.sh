#!/usr/bin/env bash
# cli_smoke.sh — E2E smoke for asx CLI tool (bd-x3fv)
#
# Verifies all CLI subcommands produce expected output.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/harness.sh"

e2e_init "cli-smoke" "E2E-CLI"

# Build the CLI binary
CLI_BIN="${E2E_BUILD_DIR}/bin/asx"
if ! (cd "$E2E_PROJECT_ROOT" && make cli 2>"${E2E_ARTIFACT_DIR}/cli_build.log"); then
    e2e_scenario "cli_smoke.build" "make cli failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "cli_smoke.build" "" "pass"

# Scenario: version prints version string
output="$("$CLI_BIN" version 2>&1)" || true
if echo "$output" | grep -q "^asx "; then
    e2e_scenario "cli_smoke.version" "" "pass"
else
    e2e_scenario "cli_smoke.version" "unexpected output: $output" "fail"
fi

# Scenario: info prints profile name
output="$("$CLI_BIN" info 2>&1)" || true
if echo "$output" | grep -q "Profile:"; then
    e2e_scenario "cli_smoke.info" "" "pass"
else
    e2e_scenario "cli_smoke.info" "no Profile: in output" "fail"
fi

# Scenario: doctor exits 0 (healthy)
if "$CLI_BIN" doctor >"${E2E_ARTIFACT_DIR}/doctor.txt" 2>&1; then
    e2e_scenario "cli_smoke.doctor_healthy" "" "pass"
else
    e2e_scenario "cli_smoke.doctor_healthy" "exit non-zero" "fail"
fi

# Scenario: doctor text output contains PASS lines
if grep -q "PASS:" "${E2E_ARTIFACT_DIR}/doctor.txt"; then
    e2e_scenario "cli_smoke.doctor_text_format" "" "pass"
else
    e2e_scenario "cli_smoke.doctor_text_format" "no PASS: lines" "fail"
fi

# Scenario: doctor --format=json produces valid JSON
output="$("$CLI_BIN" doctor --format=json 2>&1)" || true
if echo "$output" | grep -q '"healthy":true'; then
    e2e_scenario "cli_smoke.doctor_json" "" "pass"
else
    e2e_scenario "cli_smoke.doctor_json" "no healthy:true in JSON" "fail"
fi

# Scenario: help prints usage
output="$("$CLI_BIN" help 2>&1)" || true
if echo "$output" | grep -q "Usage:"; then
    e2e_scenario "cli_smoke.help" "" "pass"
else
    e2e_scenario "cli_smoke.help" "no Usage: in output" "fail"
fi

# Scenario: --help prints usage
output="$("$CLI_BIN" --help 2>&1)" || true
if echo "$output" | grep -q "Usage:"; then
    e2e_scenario "cli_smoke.dash_help" "" "pass"
else
    e2e_scenario "cli_smoke.dash_help" "no Usage: in output" "fail"
fi

# Scenario: unknown command exits 1
if "$CLI_BIN" nonexistent >/dev/null 2>&1; then
    e2e_scenario "cli_smoke.unknown_cmd" "should have exited non-zero" "fail"
else
    e2e_scenario "cli_smoke.unknown_cmd" "" "pass"
fi

# Scenario: no args exits 1
if "$CLI_BIN" >/dev/null 2>&1; then
    e2e_scenario "cli_smoke.no_args" "should have exited non-zero" "fail"
else
    e2e_scenario "cli_smoke.no_args" "" "pass"
fi

e2e_finish
