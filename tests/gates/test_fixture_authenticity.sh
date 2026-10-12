#!/usr/bin/env bash
# test_fixture_authenticity.sh — the Rust-parity runner must refuse a
# fixture that is not an untouched Rust capture (oracle self-test,
# bd-9kll.2.7: engine identity, no edited goldens).
#
# Cases, each on a copy of one captured fixture under build/test-gates/:
#   - untouched copy                         -> compare PASS
#   - one expected observation edited        -> ERROR (digest does not recompute)
#   - provenance.producer not tools/twin_run -> ERROR (not a Rust capture)
# Requires jq and rebuilds build/bin/asx-conformance from source. Logs JSONL
# to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="authenticity-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
RUNNER="$REPO_ROOT/build/bin/asx-conformance"
SOURCE="$REPO_ROOT/fixtures/rust_reference_v2/oneshot-send-recv-001.json"
mkdir -p "$WORK" "$LOG_DIR"

failures=0

log_case() {
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"fixture_authenticity","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# run_case <name> <expected exit> <expected line regex> <fixture file>
run_case() {
    local name="$1" expect="$2" pattern="$3" fixture="$4"
    local out rc status
    out="$("$RUNNER" compare "$fixture" 2>&1)"
    rc=$?
    printf '%s\n' "$out" >"$WORK/$name.log"
    status="pass"
    if [ "$rc" -ne "$expect" ]; then status="fail"; fi
    if ! grep -Eq -- "$pattern" <<<"$out"; then status="fail"; fi
    if [ "$status" = "pass" ]; then
        echo "  PASS: $name"
    else
        echo "  FAIL: $name (expected /$pattern/; log $WORK/$name.log)"
        failures=$((failures + 1))
    fi
    log_case "$name" "$expect:$pattern" "exit$rc:$(head -1 <<<"$out")" "$status" "$WORK/$name.log"
}

echo "=== test_fixture_authenticity ($RUN_ID) ==="
# A runnable cached binary can belong to an older source revision. Make's
# conformance-lib dependency rebuilds and links the current runtime first.
make -C "$REPO_ROOT" --no-print-directory build/bin/asx-conformance >"$WORK/build.log" 2>&1 || {
    echo "test_fixture_authenticity: cannot build the runner (log $WORK/build.log)"
    exit 1
}

cp "$SOURCE" "$WORK/untouched.json"
jq -c '.observations[0].status = "ASX_E_EDITED"' "$SOURCE" >"$WORK/edited.json"
jq -c '.provenance.producer = "asx-conformance"' "$SOURCE" >"$WORK/other_producer.json"

run_case untouched_capture_passes 0 '^PASS ' "$WORK/untouched.json"
run_case edited_fixture_rejected 1 '^ERROR .*does not recompute' "$WORK/edited.json"
run_case other_producer_rejected 1 '^ERROR .*not a Rust capture' "$WORK/other_producer.json"

if [ "$failures" -eq 0 ]; then
    echo "test_fixture_authenticity: all cases passed"
    exit 0
fi
echo "test_fixture_authenticity: $failures case(s) failed"
exit 1
