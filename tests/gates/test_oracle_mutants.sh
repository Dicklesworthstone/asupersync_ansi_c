#!/usr/bin/env bash
# test_oracle_mutants.sh — the Rust-parity oracle must catch planted
# semantic bugs (oracle self-test, bd-9kll.2.7).
#
# Each case builds the conformance runner with one ASX_MUTANT_* macro, a
# deliberate semantic bug kept in the source behind #ifdef and compiled only
# here, runs every Rust-captured fixture through it, and expects the run to
# FAIL, with at least one failing fixture in the family the bug belongs to:
#   - BUDGET_PRIORITY_MIN       budget meet takes the lower priority   -> budget-*
#   - CANCEL_NO_STRENGTHEN      a cancel reason is never strengthened  -> cancel-*
#   - SCHED_REVERSE_ORDER       the lab's tie-break pick is mirrored   -> lab-*
#   - OBLIGATION_DROP_COMMITS   a dropped token commits, not leaks     -> obligation-*
# A mutant the corpus does not catch means the fixtures stopped exercising
# that rule: the gate fails.
#
# Builds under build/mutants/<macro>/ (the normal build is untouched) and
# logs JSONL to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="mutants-$(date -u +%Y%m%dT%H%M%SZ)-$$"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
JOBS="${JOBS:-4}"
mkdir -p "$LOG_DIR"

failures=0

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"oracle_mutants","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# run_mutant <macro> <failing fixture family regex>
run_mutant() {
    local macro="$1" family="$2"
    local dir="build/mutants/$macro"
    local out rc status observed fails
    mkdir -p "$REPO_ROOT/$dir"
    if ! make -C "$REPO_ROOT" -j"$JOBS" --no-print-directory BUILD_DIR="$dir" \
        CFLAGS="${CFLAGS:-} -DASX_MUTANT_$macro" "$dir/bin/asx-conformance" \
        >"$REPO_ROOT/$dir/build.log" 2>&1; then
        echo "  FAIL: $macro (the mutant does not build; log $dir/build.log)"
        log_case "$macro" "build" "build-failed" "fail" "$dir/build.log"
        failures=$((failures + 1))
        return
    fi
    out="$(cd "$REPO_ROOT" && "$dir/bin/asx-conformance" compare fixtures/rust_reference_v2/*.json 2>&1)"
    rc=$?
    printf '%s\n' "$out" >"$REPO_ROOT/$dir/compare.log"
    fails="$(grep -cE '^FAIL ' <<<"$out")"
    observed="exit$rc fail=$fails"
    status="pass"
    if [ "$rc" -eq 0 ]; then status="fail"; fi
    if ! grep -Eq -- "^FAIL ($family)" <<<"$out"; then status="fail"; fi
    if [ "$status" = "pass" ]; then
        echo "  PASS: $macro caught ($fails fixture(s) fail, family /$family/ among them)"
    else
        echo "  FAIL: $macro not caught as expected ($observed; family /$family/; log $dir/compare.log)"
        failures=$((failures + 1))
    fi
    log_case "$macro" "nonzero:$family" "$observed" "$status" "$dir/compare.log"
}

echo "=== test_oracle_mutants ($RUN_ID) ==="

run_mutant BUDGET_PRIORITY_MIN 'budget-'
run_mutant CANCEL_NO_STRENGTHEN 'cancel-'
run_mutant SCHED_REVERSE_ORDER 'lab-'
run_mutant OBLIGATION_DROP_COMMITS 'obligation-'

if [ "$failures" -eq 0 ]; then
    echo "test_oracle_mutants: every mutant caught"
    exit 0
fi
echo "test_oracle_mutants: $failures mutant(s) not caught"
exit 1
