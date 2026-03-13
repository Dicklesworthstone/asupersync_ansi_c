#!/bin/bash
# test_differential_rust_mode.sh — Integration test for --rust-binary mode
#
# Tests:
# 1. Mock binary test: verify that the C harness correctly detects divergences
#    and non-divergences from a controlled mock binary
# 2. Real Rust binary: verify 1000 iterations with no crashes or grammar mismatches
#
# Bead: bd-rkql.4

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FUZZ_BIN="$SCRIPT_DIR/fuzz_differential"
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

# ---- Test 1: Mock binary that echoes fixed digest ----
# Create a mock binary that reads stdin scenarios and returns a fixed digest
cat > "$TMPDIR_WORK/mock_rust.sh" << 'MOCK_EOF'
#!/bin/bash
while IFS= read -r line; do
    seed=$(echo "$line" | awk '{print $1}')
    echo -e "${seed}\t0000000000000000"
done
MOCK_EOF
chmod +x "$TMPDIR_WORK/mock_rust.sh"

# Test 1a: Mock should produce divergences (fixed digest vs C digest)
test_mock_divergences() {
    local report="$TMPDIR_WORK/mock_report.jsonl"
    "$FUZZ_BIN" --rust-binary "$TMPDIR_WORK/mock_rust.sh" --iterations 10 \
        --report "$report" 2>/dev/null
    # Should have summary with rust_divergences > 0
    command grep -q '"rust_divergences":10' "$report"
}
run_test "mock produces divergences" test_mock_divergences

# Test 1b: Verify JSONL report has correct structure
test_mock_jsonl_structure() {
    local report="$TMPDIR_WORK/mock_report.jsonl"
    # Check divergence records have required fields
    command grep '"kind":"rust_divergence"' "$report" | head -1 | \
        python3 -c "
import json, sys
d = json.loads(sys.stdin.readline())
assert d['kind'] == 'rust_divergence'
assert 'iteration' in d
assert 'seed' in d
assert 'c_digest' in d
assert 'rust_digest' in d
assert 'ops' in d
"
}
run_test "JSONL structure valid" test_mock_jsonl_structure

# Test 1c: Summary has all required fields
test_summary_fields() {
    local report="$TMPDIR_WORK/mock_report.jsonl"
    command grep '"kind":"summary"' "$report" | \
        python3 -c "
import json, sys
d = json.loads(sys.stdin.readline())
assert d['kind'] == 'summary'
assert 'initial_seed' in d
assert 'iterations' in d
assert 'determinism_failures' in d
assert 'crashes' in d
assert 'rust_divergences' in d
assert 'duration_sec' in d
assert 'iterations_per_sec' in d
"
}
run_test "summary has all fields" test_summary_fields

# ---- Test 2: Real Rust binary ----
if [ ! -x "$RUST_BIN" ]; then
    echo "  SKIP: Rust binary not found at $RUST_BIN (build with: cd tools/rust_fuzz_target && cargo build --release)"
else
    # Test 2a: 1000 iterations, no crashes or determinism failures
    test_real_rust_1000() {
        local report="$TMPDIR_WORK/real_report.jsonl"
        "$FUZZ_BIN" --rust-binary "$RUST_BIN" --iterations 1000 \
            --report "$report" 2>/dev/null
        # Must have 0 determinism failures and 0 crashes
        command grep '"kind":"summary"' "$report" | \
            python3 -c "
import json, sys
d = json.loads(sys.stdin.readline())
assert d['determinism_failures'] == 0, f'determinism failures: {d[\"determinism_failures\"]}'
assert d['crashes'] == 0, f'crashes: {d[\"crashes\"]}'
assert d['iterations'] == 1000
"
    }
    run_test "1000 iters: 0 det failures, 0 crashes" test_real_rust_1000

    # Test 2b: No grammar mismatches (scenario seeds must match)
    test_no_grammar_mismatches() {
        local report="$TMPDIR_WORK/real_report.jsonl"
        # If there were grammar mismatches, they'd appear in stderr as GRAMMAR MISMATCH
        # Re-run with stderr captured
        local stderr_log="$TMPDIR_WORK/stderr.log"
        "$FUZZ_BIN" --rust-binary "$RUST_BIN" --iterations 100 --verbose \
            2>"$stderr_log" >/dev/null
        if command grep -q "GRAMMAR MISMATCH" "$stderr_log"; then
            return 1
        fi
        return 0
    }
    run_test "no grammar mismatches" test_no_grammar_mismatches

    # Test 2c: Performance > 100 comparisons/sec
    test_performance() {
        local report="$TMPDIR_WORK/perf_report.jsonl"
        "$FUZZ_BIN" --rust-binary "$RUST_BIN" --iterations 1000 \
            --report "$report" 2>/dev/null
        command grep '"kind":"summary"' "$report" | \
            python3 -c "
import json, sys
d = json.loads(sys.stdin.readline())
ips = d['iterations_per_sec']
assert ips > 100, f'too slow: {ips} iterations/sec (need >100)'
"
    }
    run_test "performance > 100 iters/sec" test_performance
fi

echo ""
echo "$((PASS + FAIL))/$((PASS + FAIL)) tests run: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
