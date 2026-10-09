#!/usr/bin/env bash
# test_conformance_fail_closed.sh — negative controls for the fixture and
# Rust-parity gates (bridge bead W0.3, bd-9kll.1.4).
#
# The conformance gate used to pass with zero comparisons and accepted a
# fixture whose op was `launch_missiles` and whose digest was made up. These
# controls make sure the gates fail on bad input:
#   - sabotaged.json      digest does not recompute         -> fixture-integrity FAILS
#   - unknown_op.json     capture traced an unknown op      -> fixture-integrity FAILS
#   - hand_authored.json  capture_run_id is not a UUID      -> excluded, counted, not a failure
#   - valid_control.json  well-formed capture               -> fixture-integrity PASSES
#   - valid_control.json  under --mode conformance          -> FAILS (no executed parity)
#   - empty fixture dir                                     -> fixture-integrity FAILS
#
# Requires build/lib/libasx.a (the runner compiles a codec round-trip helper).
# Each run writes under build/test-gates/<run_id>/ and logs JSONL to
# build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUNNER="$REPO_ROOT/tools/ci/run_conformance.sh"
CONTROLS="$REPO_ROOT/fixtures/negative_controls"
RUN_ID="gates-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
mkdir -p "$WORK" "$LOG_DIR"

failures=0

log_case() {
    # case expected observed pass detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"conformance_fail_closed","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# run_case <name> <mode> <expected exit: 0|nonzero> <expected output regex> [control files...]
run_case() {
    local name="$1" mode="$2" expect="$3" pattern="$4"
    shift 4
    local dir="$WORK/$name"
    mkdir -p "$dir/fixtures" "$dir/reports"
    local f
    for f in "$@"; do cp "$CONTROLS/$f" "$dir/fixtures/"; done

    local out rc observed status
    out="$(REPORT_DIR="$dir/reports" "$RUNNER" --mode "$mode" --fixtures-root "$dir/fixtures" \
        --run-id "$RUN_ID-$name" 2>&1)"
    rc=$?
    printf '%s\n' "$out" >"$dir/output.log"

    if [ "$rc" -eq 0 ]; then observed="exit0"; else observed="exit$rc"; fi
    status="pass"
    if [ "$expect" = "0" ] && [ "$rc" -ne 0 ]; then status="fail"; fi
    if [ "$expect" = "nonzero" ] && [ "$rc" -eq 0 ]; then status="fail"; fi
    if ! printf '%s\n' "$out" | grep -Eq "$pattern"; then status="fail"; fi

    if [ "$status" = "pass" ]; then
        echo "  PASS: $name ($mode -> $observed)"
    else
        echo "  FAIL: $name ($mode -> $observed; expected $expect matching /$pattern/; log $dir/output.log)"
        failures=$((failures + 1))
    fi
    log_case "$name" "$expect:$pattern" "$observed" "$status" "$dir/output.log"
}

echo "=== test_conformance_fail_closed ($RUN_ID) ==="

run_case sabotaged_fixture_rejected fixture-integrity nonzero \
    'semantic_digest does not recompute' sabotaged.json
run_case unknown_op_rejected fixture-integrity nonzero \
    'unknown ops' unknown_op.json
run_case hand_authored_excluded_not_counted fixture-integrity 0 \
    'hand_authored_records=1' hand_authored.json
run_case valid_capture_passes_integrity fixture-integrity 0 \
    'hand_authored_records=0' valid_control.json
run_case rust_parity_fails_without_execution conformance nonzero \
    'NO RUST PARITY EVIDENCE' valid_control.json
run_case empty_fixture_dir_rejected fixture-integrity nonzero \
    'no fixtures'

if [ "$failures" -eq 0 ]; then
    echo "test_conformance_fail_closed: all cases passed"
    exit 0
fi
echo "test_conformance_fail_closed: $failures case(s) failed"
exit 1
