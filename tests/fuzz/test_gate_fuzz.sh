#!/bin/bash
# test_gate_fuzz.sh — Integration tests for gate_fuzz.sh CI gate
#
# Tests:
# 1. C smoke tier passes
# 2. Removed Rust comparison options are rejected
# 3. JSONL report has gate_fuzz_result record
# 4. Invalid tier rejected
# 5. Makefile C fuzz-smoke works
# 6. Gate completes in <60 seconds (smoke tier)
# 7. An unwritable report path fails the gate
# 8. Real C replay reports measured C results
# 9. Empty, malformed, zero-run and counter-mismatch corpora fail replay
#
# Bead: bd-rkql.7

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GATE_BIN="$PROJECT_ROOT/tools/ci/gate_fuzz.sh"
REPLAY_BIN="$PROJECT_ROOT/tools/ci/replay_fuzz_counterexamples.sh"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
if [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$PROJECT_ROOT/$BUILD_DIR"
fi
export BUILD_DIR

TMPDIR_WORK="$(mktemp -d)"

PASS=0
FAIL=0

run_test() {
    local name="$1"
    shift
    if "$@"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

# ---- Test 1: Real C smoke ----
test_smoke() {
    local log="$TMPDIR_WORK/smoke.log"
    "$GATE_BIN" --tier smoke 2>&1 | tee "$log" || return 1
    command grep -q "PASS: smoke tier" "$log"
}
run_test "C smoke tier passes" test_smoke

# ---- Test 2: Obsolete options cannot claim Rust parity ----
test_removed_options() {
    if "$GATE_BIN" --auto-rust; then
        return 1
    fi
    if "$GATE_BIN" --rust-binary "$TMPDIR_WORK/obsolete-reference"; then
        return 1
    fi
    return 0
}
run_test "removed Rust comparison options rejected" test_removed_options

# ---- Test 3: JSONL report structure ----
test_jsonl_gate_result() {
    local report="$TMPDIR_WORK/test3.jsonl"
    "$GATE_BIN" --tier smoke --report "$report" || return 1
    python3 -I - "$report" <<'PY'
import json, sys
with open(sys.argv[1]) as stream:
    records = [json.loads(line) for line in stream]
summaries = [d for d in records if d['kind'] == 'summary']
assert len(summaries) == 1
assert set(summaries[0]) == {'kind', 'initial_seed', 'iterations',
                           'determinism_failures', 'crashes', 'duration_sec',
                           'iterations_per_sec'}
assert summaries[0]['iterations'] == 100
assert summaries[0]['determinism_failures'] == 0
assert summaries[0]['crashes'] == 0
verdicts = [d for d in records if d['kind'] == 'gate_fuzz_result']
assert len(verdicts) == 1
d = verdicts[0]
assert d['kind'] == 'gate_fuzz_result'
assert d['tier'] == 'smoke'
assert d['iterations'] == 100
assert d['completed_iterations'] == 100
assert d['gate_pass'] == True
assert 'start_ts' in d
assert 'end_ts' in d
assert 'determinism_failures' in d
assert 'crashes' in d
assert 'iterations_per_sec' in d
assert not any('rust' in key for key in d)
PY
}
run_test "JSONL has gate_fuzz_result record" test_jsonl_gate_result

# ---- Test 4: Invalid tier rejected ----
test_invalid_tier() {
    if "$GATE_BIN" --tier bogus 2>&1; then
        return 1  # Should have failed
    fi
    return 0
}
run_test "invalid tier rejected" test_invalid_tier

# ---- Test 5: Makefile C smoke ----
test_makefile_smoke() {
    local log="$TMPDIR_WORK/make-smoke.log"
    make -C "$PROJECT_ROOT" -j2 BUILD_DIR="$BUILD_DIR" fuzz-smoke 2>&1 | \
        tee "$log" || return 1
    command grep -q "PASS: no C determinism failures or crashes" "$log"
}
run_test "Makefile C fuzz-smoke passes" test_makefile_smoke

# ---- Test 6: Timing constraint ----
test_timing() {
    local start_ts
    start_ts=$(date +%s)
    "$GATE_BIN" --tier smoke || return 1
    local end_ts
    end_ts=$(date +%s)
    local elapsed=$((end_ts - start_ts))
    if [ "$elapsed" -ge 60 ]; then
        echo "    took ${elapsed}s (limit: 60s)"
        return 1
    fi
    return 0
}
run_test "smoke tier <60 seconds" test_timing

# ---- Test 7: Real harness report-open failure remains a gate failure ----
test_report_failure() {
    local report="$TMPDIR_WORK/missing-directory/report.jsonl"
    if "$GATE_BIN" --tier smoke --report "$report"; then
        return 1
    fi
    return 0
}
run_test "unwritable report path rejected" test_report_failure

# ---- Test 8: The existing corpus executes through the real C harness ----
test_replay_report() {
    local report_root="$TMPDIR_WORK/replay-positive"
    "$REPLAY_BIN" --report-root "$report_root" --run-id positive || return 1
    python3 -I - "$report_root/positive/fuzz_counterexample_replay_report.json" <<'PY'
import json, sys
with open(sys.argv[1]) as stream:
    report = json.load(stream)
assert report['status'] == 'pass'
assert report['corpus']['case_count'] > 0
assert report['corpus']['failure_count'] == 0
assert set(report['harness']) == {'fuzz_bin', 'checks'}
for case in report['cases']:
    assert case['status'] == 'pass'
    assert case['input']['iterations'] > 0
    assert case['actual'] == case['expected']
    assert set(case['actual']) == {'c_exit_code', 'determinism_failures', 'crashes'}
PY
}
run_test "real C replay reports only measured C counters" test_replay_report

# ---- Test 9: Corrupt test inputs must fail; these are gate controls ----
test_replay_failures() {
    local corpus="$TMPDIR_WORK/replay-negative-corpus"
    local report_root="$TMPDIR_WORK/replay-negative"
    local source="$PROJECT_ROOT/fixtures/fuzz_counterexamples/no-drift-smoke.json"
    local mode filter
    mkdir -p "$corpus" || return 1
    if "$REPLAY_BIN" --corpus-dir "$corpus" --report-root "$report_root" --run-id empty; then
        return 1
    fi
    python3 -I - "$report_root/empty/fuzz_counterexample_replay_report.json" <<'PY' || return 1
import json, sys
with open(sys.argv[1]) as stream:
    report = json.load(stream)
assert report['status'] == 'fail'
assert report['corpus']['case_count'] == 0
PY
    for mode in malformed missing zero fractional mismatch; do
        mkdir -p "$corpus/$mode" || return 1
        case "$mode" in
            malformed) printf '{\n' >"$corpus/$mode/case.json" ;;
            *)
                case "$mode" in
                    missing) filter='del(.expected.crashes)' ;;
                    zero) filter='.input.iterations = 0' ;;
                    fractional) filter='.expected.determinism_failures = 0.5' ;;
                    mismatch) filter='.expected.crashes = 1' ;;
                esac
                jq "$filter" "$source" >"$corpus/$mode/case.json" || return 1
                ;;
        esac
        if "$REPLAY_BIN" --corpus-dir "$corpus/$mode" --report-root "$report_root" \
            --run-id "$mode"; then
            return 1
        fi
        python3 -I - "$report_root/$mode/fuzz_counterexample_replay_report.json" "$mode" <<'PY' || return 1
import json, sys
with open(sys.argv[1]) as stream:
    report = json.load(stream)
assert report['status'] == 'fail'
assert report['corpus']['case_count'] == 1
assert report['corpus']['failure_count'] == 1
case = report['cases'][0]
assert case['status'] == 'fail'
assert case['diagnostic']
if sys.argv[2] == 'mismatch':
    assert case['actual']['c_exit_code'] == 0
    assert case['actual']['crashes'] == 0
    assert case['expected']['crashes'] == 1
PY
    done
}
run_test "replay rejects empty, malformed, zero-run and mismatched inputs" test_replay_failures

echo ""
echo "$((PASS + FAIL))/$((PASS + FAIL)) tests run: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
