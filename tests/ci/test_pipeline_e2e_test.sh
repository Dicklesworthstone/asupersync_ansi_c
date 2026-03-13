#!/usr/bin/env bash
# test_pipeline_e2e_test.sh — Unit test for the fixture pipeline e2e script
#
# Validates the e2e script's behavior using mock artifacts.
# No Rust toolchain required.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
E2E_SCRIPT="$PROJECT_ROOT/tools/ci/test_fixture_pipeline_e2e.sh"

PASS=0
FAIL=0
TMPDIR_BASE=""

setup() {
    TMPDIR_BASE=$(mktemp -d)
    trap 'rm -rf "$TMPDIR_BASE"' EXIT
}

assert_eq() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $label (expected=$expected, actual=$actual)"
        FAIL=$((FAIL + 1))
    fi
}

assert_contains() {
    local label="$1" haystack="$2" needle="$3"
    if echo "$haystack" | command grep -qF "$needle"; then
        echo "  PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $label (output does not contain '$needle')"
        FAIL=$((FAIL + 1))
    fi
}

assert_jsonl_valid() {
    local label="$1" output="$2"
    local invalid=0
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        if ! echo "$line" | python3 -c "import json,sys; json.load(sys.stdin)" 2>/dev/null; then
            invalid=$((invalid + 1))
        fi
    done <<< "$output"
    if [ "$invalid" -eq 0 ]; then
        echo "  PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $label ($invalid invalid JSONL lines)"
        FAIL=$((FAIL + 1))
    fi
}

# ── Create mock fixture set ────────────────────────────────────────────

create_mock_fixtures() {
    local dir="$1"
    local count="${2:-66}"
    local placeholder_digest="${3:-false}"

    mkdir -p "$dir/core_lifecycle" "$dir/core_budget" "$dir/smoke"

    local digest="sha256:abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890"
    if [ "$placeholder_digest" = "true" ]; then
        digest="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    fi

    local i=0
    while [ "$i" -lt "$count" ]; do
        local family="core_lifecycle"
        if [ "$((i % 3))" -eq 1 ]; then family="core_budget"; fi
        if [ "$((i % 3))" -eq 2 ]; then family="smoke"; fi

        cat > "$dir/$family/fixture-$(printf '%03d' "$i").json" <<FIXTURE
{
  "scenario_id": "test-scenario-$i",
  "fixture_schema_version": "1.0.0",
  "scenario_dsl_version": "1.0.0",
  "profile": "core",
  "codec": "json",
  "seed": $i,
  "input": {"ops": [{"op": "noop"}]},
  "expected_events": [],
  "expected_final_snapshot": {},
  "expected_error_codes": [],
  "semantic_digest": "$digest",
  "provenance": {
    "rust_baseline_commit": "a9e737d869c04c2f6aa4222fe38be66b28628227",
    "rust_toolchain_commit_hash": "3b1b0ef4d",
    "cargo_lock_sha256": "ddf46a437b62698175bdc96183436813a4a81c220a55ed94c7eb766fe4f20ac9"
  }
}
FIXTURE
        i=$((i + 1))
    done
}

# ── Tests ──────────────────────────────────────────────────────────────

test_dry_run_produces_jsonl() {
    echo "Test: dry-run produces valid JSONL"
    local output
    output=$("$E2E_SCRIPT" --dry-run 2>&1)
    assert_jsonl_valid "output is valid JSONL" "$output"
    assert_contains "has pipeline_result" "$output" "pipeline_result"
    assert_contains "all steps skipped" "$output" "dry-run"
}

test_skip_build_and_capture_with_good_fixtures() {
    echo "Test: skip-build + skip-capture with valid fixtures"
    local mock_dir="$TMPDIR_BASE/good_fixtures"
    create_mock_fixtures "$mock_dir" 66

    local output
    output=$("$E2E_SCRIPT" --skip-build --skip-capture --skip-conformance --fixtures-dir "$mock_dir" 2>&1)
    local rc=$?

    assert_eq "exit code 0" "0" "$rc"
    assert_jsonl_valid "output is valid JSONL" "$output"
    assert_contains "schema_validation pass" "$output" "\"status\":\"pass\""
    assert_contains "provenance_check present" "$output" "provenance_check"
}

test_detects_missing_fixtures() {
    echo "Test: detects insufficient fixture count"
    local mock_dir="$TMPDIR_BASE/few_fixtures"
    create_mock_fixtures "$mock_dir" 10

    local output rc=0
    output=$("$E2E_SCRIPT" --skip-build --skip-capture --skip-conformance --fixtures-dir "$mock_dir" 2>&1) || rc=$?

    assert_eq "exit code 1" "1" "$rc"
    assert_contains "fixture_capture fails" "$output" "\"status\":\"fail\""
    assert_contains "mentions count" "$output" "need >="
}

test_detects_placeholder_digests() {
    echo "Test: detects placeholder digests"
    local mock_dir="$TMPDIR_BASE/placeholder_fixtures"
    create_mock_fixtures "$mock_dir" 66 true

    local output rc=0
    output=$("$E2E_SCRIPT" --skip-build --skip-capture --skip-conformance --fixtures-dir "$mock_dir" 2>&1) || rc=$?

    assert_eq "exit code 1" "1" "$rc"
    assert_contains "schema_validation fails" "$output" "schema_validation"
    assert_contains "reports fail" "$output" "\"status\":\"fail\""
}

test_detects_provenance_mismatch() {
    echo "Test: detects provenance mismatch"
    local mock_dir="$TMPDIR_BASE/prov_mismatch"
    create_mock_fixtures "$mock_dir" 66

    # Tamper with one fixture's provenance
    local tampered
    tampered=$(command find "$mock_dir" -name '*.json' -type f | head -1)
    python3 -c "
import json
d = json.load(open('$tampered'))
d['provenance']['rust_baseline_commit'] = 'deadbeefdeadbeefdeadbeefdeadbeefdeadbeef'
json.dump(d, open('$tampered', 'w'))
"

    local output rc=0
    output=$("$E2E_SCRIPT" --skip-build --skip-capture --skip-conformance --fixtures-dir "$mock_dir" 2>&1) || rc=$?

    assert_eq "exit code 1" "1" "$rc"
    assert_contains "provenance_check fails" "$output" "provenance_check"
}

test_output_parseable_by_jq() {
    echo "Test: output parseable by jq"
    if ! command -v jq >/dev/null 2>&1; then
        echo "  SKIP: jq not available"
        return 0
    fi

    local output
    output=$("$E2E_SCRIPT" --dry-run 2>&1)

    local step_count
    step_count=$(echo "$output" | jq -r '.step' 2>/dev/null | wc -l | tr -d ' ')
    assert_eq "jq can parse all lines" "7" "$step_count"  # 6 steps + pipeline_result

    local result_status
    result_status=$(echo "$output" | jq -r 'select(.step == "pipeline_result") | .status' 2>/dev/null)
    assert_eq "pipeline_result extractable" "pass" "$result_status"
}

# ── Main ───────────────────────────────────────────────────────────────

main() {
    echo "=========================================="
    echo "Fixture Pipeline E2E Script — Unit Tests"
    echo "=========================================="
    echo ""

    setup

    test_dry_run_produces_jsonl
    echo ""
    test_skip_build_and_capture_with_good_fixtures
    echo ""
    test_detects_missing_fixtures
    echo ""
    test_detects_placeholder_digests
    echo ""
    test_detects_provenance_mismatch
    echo ""
    test_output_parseable_by_jq
    echo ""

    echo "=========================================="
    echo "Summary: PASS=$PASS  FAIL=$FAIL"
    echo "=========================================="

    if [ "$FAIL" -gt 0 ]; then
        exit 1
    fi
    exit 0
}

main
