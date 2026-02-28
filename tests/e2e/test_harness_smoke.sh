#!/usr/bin/env bash
# test_harness_smoke.sh — smoke tests for the shared e2e harness (bd-1md.20)
#
# Validates that the harness contract produces deterministic outputs,
# correct manifest/log structure, and proper error handling.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Use a temp directory for artifacts
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

PASS=0
FAIL=0

assert_eq() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected='$expected' actual='$actual')"
        FAIL=$((FAIL + 1))
    fi
}

assert_file_exists() {
    local desc="$1"
    local path="$2"
    if [ -f "$path" ]; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (file not found: $path)"
        FAIL=$((FAIL + 1))
    fi
}

assert_dir_exists() {
    local desc="$1"
    local path="$2"
    if [ -d "$path" ]; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (directory not found: $path)"
        FAIL=$((FAIL + 1))
    fi
}

assert_contains() {
    local desc="$1"
    local file="$2"
    local pattern="$3"
    if grep -q "$pattern" "$file" 2>/dev/null; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (pattern '$pattern' not found in $file)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== test_harness_smoke (e2e harness) ==="

# -------------------------------------------------------------------
# Test 1: harness sourcing and init
# -------------------------------------------------------------------

export ASX_E2E_SEED=99
export ASX_E2E_PROFILE=CORE
export ASX_E2E_CODEC=json
export ASX_E2E_RESOURCE_CLASS=R2
export ASX_E2E_RUN_ID="smoke-test-001"
export ASX_E2E_ARTIFACT_DIR="${WORK_DIR}/artifacts"
export ASX_E2E_LOG_DIR="${WORK_DIR}/logs"
export ASX_E2E_VERBOSE=0

source "$SCRIPT_DIR/harness.sh"

e2e_init "SMOKE-HARNESS" "E2E-SMOKE"

assert_eq "family_id set" "SMOKE-HARNESS" "$_E2E_FAMILY_ID"
assert_eq "lane_id set" "E2E-SMOKE" "$_E2E_LANE_ID"
assert_eq "seed from env" "99" "$E2E_SEED"
assert_eq "profile from env" "CORE" "$E2E_PROFILE"
assert_eq "codec from env" "json" "$E2E_CODEC"
assert_eq "resource class from env" "R2" "$E2E_RESOURCE_CLASS"
assert_dir_exists "artifact dir created" "${WORK_DIR}/artifacts"
assert_dir_exists "log dir created" "${WORK_DIR}/logs"

# -------------------------------------------------------------------
# Test 2: scenario recording
# -------------------------------------------------------------------

e2e_scenario "smoke.pass.basic" "" "pass"
e2e_scenario "smoke.fail.example" "intentional failure" "fail"
e2e_scenario "smoke.skip.example" "not applicable" "skip"
e2e_scenario "smoke.pass.with_digest" "" "pass" "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

assert_eq "total count" "4" "$_E2E_TOTAL"
assert_eq "pass count" "2" "$_E2E_PASS"
assert_eq "fail count" "1" "$_E2E_FAIL"
assert_eq "skip count" "1" "$_E2E_SKIP"

# -------------------------------------------------------------------
# Test 3: first failure tracking
# -------------------------------------------------------------------

assert_eq "first failure scenario" "smoke.fail.example" "$_E2E_FIRST_FAILURE_SCENARIO"
assert_eq "first failure message" "intentional failure" "$_E2E_FIRST_FAILURE"

# -------------------------------------------------------------------
# Test 4: finish and manifest emission
# -------------------------------------------------------------------

# e2e_finish returns non-zero on failures, so we capture it
set +e
e2e_finish > /dev/null 2>&1
FINISH_RC=$?
set -e

assert_eq "finish returns non-zero on failures" "1" "$FINISH_RC"

# Check report JSONL
REPORT_FILE="${WORK_DIR}/logs/e2e-SMOKE-HARNESS.jsonl"
assert_file_exists "report JSONL exists" "$REPORT_FILE"
assert_contains "report has pass record" "$REPORT_FILE" '"status":"pass"'
assert_contains "report has fail record" "$REPORT_FILE" '"status":"fail"'
assert_contains "report has skip record" "$REPORT_FILE" '"status":"skip"'
assert_contains "report has layer e2e" "$REPORT_FILE" '"layer":"e2e"'
assert_contains "report has run_id" "$REPORT_FILE" '"run_id":"smoke-test-001"'
assert_contains "report has seed" "$REPORT_FILE" '"seed":99'
assert_contains "report has digest" "$REPORT_FILE" '"digest":"sha256:'
assert_contains "report has summary" "$REPORT_FILE" '"test":"_summary"'

# Check summary JSON
SUMMARY_FILE="${WORK_DIR}/artifacts/SMOKE-HARNESS.summary.json"
assert_file_exists "summary JSON exists" "$SUMMARY_FILE"
assert_contains "summary has family_id" "$SUMMARY_FILE" '"family_id"'
assert_contains "summary has total" "$SUMMARY_FILE" '"total": 4'
assert_contains "summary has pass count" "$SUMMARY_FILE" '"pass": 2'
assert_contains "summary has fail count" "$SUMMARY_FILE" '"fail": 1'

# -------------------------------------------------------------------
# Test 5: rerun command generation
# -------------------------------------------------------------------

RERUN="$(e2e_rerun_command "smoke.fail.example" "tests/e2e/my_script.sh")"
assert_contains "rerun has seed" <(echo "$RERUN") "ASX_E2E_SEED=99"
assert_contains "rerun has profile" <(echo "$RERUN") "ASX_E2E_PROFILE=CORE"
assert_contains "rerun has class" <(echo "$RERUN") "ASX_E2E_RESOURCE_CLASS=R2"
assert_contains "rerun has scenario" <(echo "$RERUN") "ASX_E2E_SCENARIO_PACK=smoke.fail.example"

# -------------------------------------------------------------------
# Test 6: scenario filter
# -------------------------------------------------------------------

export ASX_E2E_SCENARIO_PACK="smoke.pass"
# Re-source to pick up new pack
source "$SCRIPT_DIR/harness.sh"

if e2e_should_run "smoke.pass.basic"; then
    assert_eq "filter matches prefix" "0" "0"
else
    assert_eq "filter matches prefix" "0" "1"
fi

if e2e_should_run "smoke.fail.example"; then
    assert_eq "filter rejects non-match" "1" "0"
else
    assert_eq "filter rejects non-match" "0" "0"
fi

export ASX_E2E_SCENARIO_PACK="all"
source "$SCRIPT_DIR/harness.sh"

if e2e_should_run "anything.at.all"; then
    assert_eq "filter passes all" "0" "0"
else
    assert_eq "filter passes all" "0" "1"
fi

# -------------------------------------------------------------------
# Test 7: deterministic output (same seed → same run_id)
# -------------------------------------------------------------------

assert_eq "run_id deterministic" "smoke-test-001" "$E2E_RUN_ID"

# -------------------------------------------------------------------
# Test 8: validate JSONL with test-log validator
# -------------------------------------------------------------------

if [ -x "${PROJECT_ROOT}/tools/ci/validate_test_logs.sh" ]; then
    if "${PROJECT_ROOT}/tools/ci/validate_test_logs.sh" --log-dir "${WORK_DIR}/logs" > /dev/null 2>&1; then
        echo "  PASS: JSONL validates against schema"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: JSONL schema validation failed"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  SKIP: validate_test_logs.sh not found"
fi

# -------------------------------------------------------------------
# Report
# -------------------------------------------------------------------

echo ""
echo "$((PASS + FAIL))/$((PASS + FAIL)) tests: ${PASS} passed, ${FAIL} failed"

if [ "$FAIL" -gt 0 ]; then
    echo "FAILURES: $FAIL"
    exit 1
fi

exit 0
