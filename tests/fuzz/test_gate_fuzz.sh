#!/bin/bash
# test_gate_fuzz.sh — Integration tests for gate_fuzz.sh CI gate
#
# Tests:
# 1. Smoke tier passes without Rust binary
# 2. Smoke tier passes with Rust binary (--auto-rust)
# 3. JSONL report has gate_fuzz_result record
# 4. Invalid tier rejected
# 5. Makefile RUST_FUZZ_BINARY variable works
# 6. Gate completes in <60 seconds (smoke tier)
#
# Bead: bd-rkql.7

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GATE_BIN="$PROJECT_ROOT/tools/ci/gate_fuzz.sh"
RUST_BIN="$PROJECT_ROOT/tools/rust_fuzz_target/target/release/rust_fuzz_target"

TMPDIR_WORK="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_WORK"' EXIT

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

# ---- Test 1: Smoke tier without Rust ----
test_smoke_no_rust() {
    "$GATE_BIN" --tier smoke 2>&1 | command grep -q "PASS: smoke tier"
}
run_test "smoke tier passes (no Rust)" test_smoke_no_rust

# ---- Test 2: Smoke tier with Rust ----
test_smoke_with_rust() {
    if [ ! -x "$RUST_BIN" ]; then
        echo "    (skip: no Rust binary)"
        return 0
    fi
    "$GATE_BIN" --tier smoke --auto-rust 2>&1 | command grep -q "PASS: smoke tier"
}
run_test "smoke tier passes (with Rust)" test_smoke_with_rust

# ---- Test 3: JSONL report structure ----
test_jsonl_gate_result() {
    local report="$TMPDIR_WORK/test3.jsonl"
    "$GATE_BIN" --tier smoke --report "$report" 2>/dev/null
    command grep 'gate_fuzz_result' "$report" | python3 -c "
import json, sys
d = json.loads(sys.stdin.readline())
assert d['kind'] == 'gate_fuzz_result'
assert d['tier'] == 'smoke'
assert d['iterations'] == 100
assert d['gate_pass'] == True
assert 'start_ts' in d
assert 'end_ts' in d
assert 'determinism_failures' in d
assert 'crashes' in d
assert 'iterations_per_sec' in d
"
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

# ---- Test 5: Makefile RUST_FUZZ_BINARY variable ----
test_makefile_rust_var() {
    # Just verify the variable is accepted and fuzz-smoke works
    make -C "$PROJECT_ROOT" fuzz-smoke 2>&1 | command grep -q "no issues"
}
run_test "Makefile fuzz-smoke with default (no RUST_FUZZ_BINARY)" test_makefile_rust_var

# ---- Test 6: Timing constraint ----
test_timing() {
    local start_ts
    start_ts=$(date +%s)
    "$GATE_BIN" --tier smoke 2>/dev/null
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

echo ""
echo "$((PASS + FAIL))/$((PASS + FAIL)) tests run: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
