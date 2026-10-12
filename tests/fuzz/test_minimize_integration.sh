#!/bin/bash
# test_minimize_integration.sh — Integration tests for minimizer pipeline
#
# Tests:
# 1. Self-tests pass (all built-in modes)
# 2. stdin-scenario mode works with generated scenarios
# 3. Minimizer reduces ops via predicate (self-test 3 baseline)
# 4. JSONL output is parseable
# 5. seed+iteration replay path works
# 6. End-to-end: fuzz_differential --minimize pipeline wired correctly
# 7. C smoke summary contains only measured C checks
#
# Bead: bd-rkql.5

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
if [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$PROJECT_ROOT/$BUILD_DIR"
fi
MINIMIZE_BIN="$BUILD_DIR/fuzz/fuzz_minimize"
FUZZ_BIN="$BUILD_DIR/fuzz/fuzz_differential"
JSONL_GEN="$BUILD_DIR/fuzz/generate_grammar_vectors_jsonl"

make -C "$PROJECT_ROOT" -j2 BUILD_DIR="$BUILD_DIR" fuzz-build minimize-build
"${CC:-cc}" -std=c99 -Wall -Wextra -Wpedantic -Werror -O2 \
    -o "$JSONL_GEN" "$SCRIPT_DIR/generate_grammar_vectors_jsonl.c"

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

# ---- Test 1: Self-tests ----
test_selftests() {
    local log="$TMPDIR_WORK/selftests.log"
    "$MINIMIZE_BIN" --selftest 2>&1 | tee "$log" || return 1
    command grep -q "self-test 4 PASS" "$log"
}
run_test "all built-in self-tests pass" test_selftests

# ---- Test 2: stdin-scenario mode ----
test_stdin_scenario() {
    # Generate a scenario and feed it to minimizer
    "$JSONL_GEN" 1 | python3 -I -c "
import json, sys
sc = json.loads(sys.stdin.readline())
kind_map = {'SpawnRegion':0,'CloseRegion':1,'PoisonRegion':2,'SpawnTask':3,'CancelTask':4,
    'ReserveObligation':5,'CommitObligation':6,'AbortObligation':7,
    'ChannelCreate':8,'ChannelReserve':9,'ChannelSend':10,'ChannelAbort':11,
    'ChannelRecv':12,'ChannelCloseTx':13,'ChannelCloseRx':14,
    'TimerRegister':15,'TimerCancel':16,'AdvanceTime':17,
    'SchedulerRun':18,'RegionDrain':19,'QuiescenceCheck':20}
parts = [str(sc['seed']), str(sc['op_count'])]
for op in sc['ops']:
    parts.extend([str(kind_map[op['op']]), str(op['idx_a']), str(op['idx_b']),
                  str(op['arg_u32']), str(op['arg_u64'])])
print(' '.join(parts))
" | "$MINIMIZE_BIN" --stdin-scenario --output "$TMPDIR_WORK/test2.json" || return 1
    # Verify output file exists and is valid JSON
    python3 -I -c "
import json
with open('$TMPDIR_WORK/test2.json') as f:
    d = json.loads(f.read())
assert d['kind'] == 'minimized_scenario'
assert d['minimized_ops'] > 0
assert 'ops' in d
"
}
run_test "stdin-scenario produces valid output" test_stdin_scenario

# ---- Test 3: JSONL output structure ----
test_jsonl_fields() {
    # Self-test JSONL goes to stderr in verbose mode
    "$MINIMIZE_BIN" --selftest --verbose 2>&1 | command grep 'minimized_scenario' | python3 -I -c "
import json, sys
lines = []
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    try:
        d = json.loads(line)
        lines.append(d)
    except: pass
assert len(lines) == 3, f'expected 3 JSONL records, got {len(lines)}'
for d in lines:
    assert d['kind'] == 'minimized_scenario'
    assert d['schema_name'] == 'asx.replay_counterexample'
    assert d['schema_version'] == 'asx.replay_counterexample.v1'
    assert d['preserves_failure_class'] is True
    assert 'failure_class' in d
    assert 'seed' in d
    assert 'original_ops' in d
    assert 'minimized_ops' in d
    assert 'rounds' in d
    assert 'ops_removed' in d
    assert 'args_simplified' in d
    assert 'duration_sec' in d
    assert 'target_digest' in d
    assert 'original_digest' in d
    assert 'final_digest' in d
    assert 'ops' in d
"
}
run_test "JSONL has all required fields" test_jsonl_fields

# ---- Test 4: Self-test 3 reduction ratio ----
test_reduction_ratio() {
    "$MINIMIZE_BIN" --selftest --verbose 2>&1 | command grep 'minimized_scenario' | tail -1 | python3 -I -c "
import json, sys
d = json.loads(sys.stdin.readline())
ratio = 1.0 - (d['minimized_ops'] / d['original_ops'])
assert ratio >= 0.5, f'reduction ratio {ratio:.2f} < 0.5 (need >50%)'
"
}
run_test "self-test 3: >50% reduction" test_reduction_ratio

# ---- Test 5: Pipeline wiring ----
test_seed_iteration_replay() {
    "$MINIMIZE_BIN" --initial-seed 424242 --iteration 7 --output "$TMPDIR_WORK/test5.json" \
        || return 1
    python3 -I -c "
import json
with open('$TMPDIR_WORK/test5.json') as f:
    d = json.loads(f.read())
assert d['kind'] == 'minimized_scenario'
assert d['seed'] > 0
assert d['original_ops'] > 0
assert d['minimized_ops'] > 0
assert len(d['ops']) == d['minimized_ops']
"
}
run_test "seed+iteration replay produces valid output" test_seed_iteration_replay

# ---- Test 6: Pipeline wiring ----
test_pipeline_wiring() {
    local log="$TMPDIR_WORK/pipeline.log"
    # Run the real C harness with --minimize flag.
    # Since there are no determinism failures, this just verifies the flag is accepted
    "$FUZZ_BIN" --iterations 10 --minimize "$MINIMIZE_BIN" 2>&1 | tee "$log" || return 1
    command grep -q "PASS: no C determinism failures or crashes" "$log"
}
run_test "C smoke --minimize accepted" test_pipeline_wiring

# ---- Test 7: Full C pipeline (not Rust parity evidence) ----
test_full_pipeline() {
    local report="$TMPDIR_WORK/full_report.jsonl"
    "$FUZZ_BIN" --minimize "$MINIMIZE_BIN" --iterations 100 --report "$report" \
        || return 1
    python3 -I - "$report" <<'PY'
import json, sys
with open(sys.argv[1]) as stream:
    records = [json.loads(line) for line in stream]
summaries = [d for d in records if d['kind'] == 'summary']
assert len(summaries) == 1
d = summaries[0]
assert d['determinism_failures'] == 0
assert d['crashes'] == 0
assert d['iterations'] == 100
assert set(d) == {'kind', 'initial_seed', 'iterations', 'determinism_failures',
                  'crashes', 'duration_sec', 'iterations_per_sec'}
PY
}
run_test "full C determinism/crash pipeline with minimizer" test_full_pipeline

echo ""
echo "$((PASS + FAIL))/$((PASS + FAIL)) tests run: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
