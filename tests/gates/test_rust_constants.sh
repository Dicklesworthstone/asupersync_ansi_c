#!/usr/bin/env bash
# test_rust_constants.sh — negative controls for the kernel-constants gate
# (make check-rust-constants: asx-conformance constants against
# schemas/rust_kernel_constants.json; bd-9kll.2.13).
#
# The gate must not be able to pass vacuously or drift silently. Over
# scratch copies of the Rust document:
#   - the document as it is                        -> PASSES, >= 50 constants equal
#   - a constant with a different Rust value       -> FAILS naming it
#   - a recorded difference that no longer differs -> FAILS (stale KNOWN entry)
#   - a Rust constant C does not declare           -> FAILS ("no C counterpart")
#   - a C constant the document lacks              -> FAILS ("the Rust document does not")
#   - a document of another schema                 -> FAILS
#
# Scratch files live under build/test-gates/<run_id>/; tracked files are never
# touched. Logs JSONL to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="rust-constants-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
DOC="$REPO_ROOT/schemas/rust_kernel_constants.json"
mkdir -p "$WORK" "$LOG_DIR"

failures=0

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"rust_constants","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

if ! make --no-print-directory -C "$REPO_ROOT" conformance-runner >"$WORK/build.log" 2>&1; then
    echo "  FAIL: cannot build the conformance runner (log $WORK/build.log)"
    log_case build "exit0" "build-failed" "fail" "$WORK/build.log"
    exit 1
fi
RUNNER="$REPO_ROOT/build/bin/asx-conformance"

# run_case <name> <expected exit: 0|nonzero> <expected output regex> <document>
run_case() {
    local name="$1" expect="$2" pattern="$3" doc="$4"
    local out rc observed status
    out="$("$RUNNER" constants "$doc" 2>&1)"
    rc=$?
    printf '%s\n' "$out" >"$WORK/$name.log"

    if [ "$rc" -eq 0 ]; then observed="exit0"; else observed="exit$rc"; fi
    status="pass"
    if [ "$expect" = "0" ] && [ "$rc" -ne 0 ]; then status="fail"; fi
    if [ "$expect" = "nonzero" ] && [ "$rc" -eq 0 ]; then status="fail"; fi
    if ! printf '%s\n' "$out" | grep -Eq "$pattern"; then status="fail"; fi

    if [ "$status" = "pass" ]; then
        echo "  PASS: $name ($observed)"
    else
        echo "  FAIL: $name ($observed; expected $expect matching /$pattern/; log $WORK/$name.log)"
        failures=$((failures + 1))
    fi
    log_case "$name" "$expect:$pattern" "$observed" "$status" "$WORK/$name.log"
}

# edit <out> <jq program>: a scratch copy of the document, edited by jq.
edit() {
    jq -c "$2" "$DOC" >"$WORK/$1"
}

echo "=== test_rust_constants ($RUN_ID) ==="

run_case document_as_is_passes 0 'ok=([5-9][0-9]|[1-9][0-9]{2,}) .* fail=0' "$DOC"

edit changed_value.json '.constants["timer.max_duration_ns"].value = 86400000000000'
run_case changed_value_fails nonzero 'FAIL timer\.max_duration_ns: rust=86400000000000' \
    "$WORK/changed_value.json"

edit stale_known.json '.constants["budget.zero.deadline_ns"].value = 1'
run_case stale_known_entry_fails nonzero 'FAIL budget\.zero\.deadline_ns: .*now equals rust' \
    "$WORK/stale_known.json"

edit unknown_rust_key.json '.constants["lab.new_constant"] = {"value": 1, "source": "test"}'
run_case undeclared_rust_constant_fails nonzero 'FAIL lab\.new_constant: .*no C counterpart' \
    "$WORK/unknown_rust_key.json"

edit missing_rust_key.json 'del(.constants["rwlock.max_consecutive_writers"])'
run_case constant_missing_from_document_fails nonzero \
    'FAIL rwlock\.max_consecutive_writers: C declares it, the Rust document does not' \
    "$WORK/missing_rust_key.json"

edit wrong_schema.json '.schema = "asx.something_else.v1"'
run_case wrong_schema_fails nonzero 'not an asx\.rust_kernel_constants\.v1 document' \
    "$WORK/wrong_schema.json"

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_rust_constants: PASS"
    exit 0
fi
echo "test_rust_constants: FAIL ($failures case(s))"
exit 1
