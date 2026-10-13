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
if ! (cd "$E2E_PROJECT_ROOT" && make cli BUILD_DIR="$E2E_BUILD_DIR" 2>"${E2E_ARTIFACT_DIR}/cli_build.log"); then
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

# Scenario: doctor runs the platform probes (bd-9kll.10.8), each with a
# verdict line; a probe the build cannot run is a SKIP, never a PASS
missing=""
for probe in clock entropy socket reactor blocking_job; do
    grep -Eq "^  (PASS|SKIP): ${probe} — " "${E2E_ARTIFACT_DIR}/doctor.txt" || missing="$missing $probe"
done
if [ -z "$missing" ]; then
    e2e_scenario "cli_smoke.doctor_probes" "" "pass"
else
    e2e_scenario "cli_smoke.doctor_probes" "no verdict for:$missing" "fail"
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

# Scenario: an unknown output format is refused, not silently ignored
if "$CLI_BIN" doctor --format=yaml >/dev/null 2>&1; then
    e2e_scenario "cli_smoke.unknown_format" "should have exited non-zero" "fail"
else
    e2e_scenario "cli_smoke.unknown_format" "" "pass"
fi

# Scenario: an unknown option is refused
if "$CLI_BIN" doctor --verbose >/dev/null 2>&1; then
    e2e_scenario "cli_smoke.unknown_option" "should have exited non-zero" "fail"
else
    e2e_scenario "cli_smoke.unknown_option" "" "pass"
fi

# Scenario: no args exits 1
if "$CLI_BIN" >/dev/null 2>&1; then
    e2e_scenario "cli_smoke.no_args" "should have exited non-zero" "fail"
else
    e2e_scenario "cli_smoke.no_args" "" "pass"
fi

# Scenario: a doctor run is recorded to a journal and the journal is
# accepted by `asx replay` (bd-9kll.11.2): identity, integrity and every
# record the build serves
JOURNAL="${E2E_ARTIFACT_DIR}/doctor.journal"
if "$CLI_BIN" doctor --journal="$JOURNAL" >"${E2E_ARTIFACT_DIR}/doctor_journal.txt" 2>&1 &&
    [ -s "$JOURNAL" ]; then
    e2e_scenario "cli_smoke.journal_recorded" "" "pass"
else
    e2e_scenario "cli_smoke.journal_recorded" "doctor --journal failed or wrote nothing" "fail"
fi
if "$CLI_BIN" replay "$JOURNAL" --rust-rev=5e60b1c4c >"${E2E_ARTIFACT_DIR}/replay.txt" 2>&1 &&
    grep -q "verdict: ACCEPTED" "${E2E_ARTIFACT_DIR}/replay.txt"; then
    e2e_scenario "cli_smoke.replay_accepts" "" "pass"
else
    e2e_scenario "cli_smoke.replay_accepts" "journal not accepted" "fail"
fi
if "$CLI_BIN" replay "$JOURNAL" --format=json 2>/dev/null | grep -q '"accepted":true'; then
    e2e_scenario "cli_smoke.replay_json" "" "pass"
else
    e2e_scenario "cli_smoke.replay_json" "no accepted:true in JSON" "fail"
fi

# Scenario: a journal naming another revision is refused, exit 1, with a seq
if "$CLI_BIN" replay "$JOURNAL" --rust-rev=not-this-one >"${E2E_ARTIFACT_DIR}/replay_other.txt" 2>&1; then
    e2e_scenario "cli_smoke.replay_other_rev" "should have exited non-zero" "fail"
elif grep -q "REFUSED BUILD_MISMATCH at seq 0" "${E2E_ARTIFACT_DIR}/replay_other.txt"; then
    e2e_scenario "cli_smoke.replay_other_rev" "" "pass"
else
    e2e_scenario "cli_smoke.replay_other_rev" "no BUILD_MISMATCH verdict" "fail"
fi

# Scenario: a truncated journal is refused as INCOMPLETE (the END is cut off)
head -c "$(($(wc -c <"$JOURNAL") - 70))" "$JOURNAL" >"${E2E_ARTIFACT_DIR}/doctor_cut.journal"
if "$CLI_BIN" replay "${E2E_ARTIFACT_DIR}/doctor_cut.journal" >"${E2E_ARTIFACT_DIR}/replay_cut.txt" 2>&1; then
    e2e_scenario "cli_smoke.replay_truncated" "should have exited non-zero" "fail"
elif grep -q "REFUSED INCOMPLETE" "${E2E_ARTIFACT_DIR}/replay_cut.txt"; then
    e2e_scenario "cli_smoke.replay_truncated" "" "pass"
else
    e2e_scenario "cli_smoke.replay_truncated" "no INCOMPLETE verdict" "fail"
fi

# Scenario: a missing journal is an error (exit 2), not a verdict
"$CLI_BIN" replay "${E2E_ARTIFACT_DIR}/no-such.journal" >/dev/null 2>&1 && rc=0 || rc=$?
if [ "$rc" -eq 2 ]; then
    e2e_scenario "cli_smoke.replay_missing_file" "" "pass"
else
    e2e_scenario "cli_smoke.replay_missing_file" "exit $rc, wanted 2" "fail"
fi

e2e_finish
