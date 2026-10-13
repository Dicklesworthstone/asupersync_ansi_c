#!/usr/bin/env bash
# test_rust_constants.sh — negative controls for the kernel-constants gate
# (make check-rust-constants: asx-conformance constants against
# schemas/rust_kernel_constants.json; bd-9kll.2.13).
#
# The gate must not be able to pass vacuously or drift silently. Over
# scratch copies of the Rust document:
#   - the document as it is                        -> PASSES, >= 50 constants equal
#   - a constant with a different Rust value       -> FAILS naming it
#   - Rust's leak-response default changed (D2)    -> FAILS naming it
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
    # case expected observed status detail. json.dumps escapes what the
    # expected regexes carry (backslashes, quotes); printf '%s' wrote them raw
    # and left most records invalid JSON.
    python3 - "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG" <<'PY'
import datetime
import json
import sys

run_id, test, expected, observed, status, detail = sys.argv[1:]
row = {
    "ts": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "run_id": run_id,
    "layer": "gates",
    "suite": "rust_constants",
    "test": test,
    "expected": expected,
    "observed": observed,
    "status": status,
    "detail": detail,
}
print(json.dumps(row, separators=(",", ":")))
PY
}

if ! make --no-print-directory -C "$REPO_ROOT" conformance-runner >"$WORK/build.log" 2>&1; then
    echo "  FAIL: cannot build the conformance runner (log $WORK/build.log)"
    log_case build "exit0" "build-failed" "fail" "$WORK/build.log"
    exit 1
fi
RUNNER="${ASX_CONFORMANCE_RUNNER:-$REPO_ROOT/build/bin/asx-conformance}"

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
    if ! grep -Eq -- "$pattern" <<<"$out"; then status="fail"; fi

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

edit ordinary_finalizing.json '.constants["region_state.Finalizing.can_accept_work"].value = true'
run_case ordinary_finalizing_admission_drift_fails nonzero \
    'FAIL region_state\.Finalizing\.can_accept_work: rust=true c=false' \
    "$WORK/ordinary_finalizing.json"

edit cleanup_finalizing.json '.constants["region_state.Finalizing.can_accept_cleanup_work"].value = false'
run_case cleanup_finalizing_admission_drift_fails nonzero \
    'FAIL region_state\.Finalizing\.can_accept_cleanup_work: rust=false c=true' \
    "$WORK/cleanup_finalizing.json"

edit leak_default.json '.constants["runtime.default.obligation_leak_response"].value = "Log"'
run_case leak_response_default_drift_fails nonzero \
    'FAIL runtime\.default\.obligation_leak_response: rust="Log" c="Panic"' \
    "$WORK/leak_default.json"

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

# This run's records must be valid JSON (log_case once wrote the expected
# regexes raw, backslashes and all): one record per case, each parseable.
if python3 - "$LOG" "$RUN_ID" >"$WORK/jsonl.log" 2>&1 <<'PY'
import json
import sys

prefix = '"run_id":' + json.dumps(sys.argv[2])
with open(sys.argv[1]) as stream:
    rows = [json.loads(line) for line in stream if prefix in line]
if len(rows) != 8:
    raise ValueError(f"expected 8 records for this run, got {len(rows)}")
if any(row["suite"] != "rust_constants" or row["run_id"] != sys.argv[2] for row in rows):
    raise ValueError("a record does not belong to this run")
print("all 8 records of this run are valid JSON")
PY
then
    echo "  PASS: current_run_jsonl_is_valid (8 records)"
else
    cat "$WORK/jsonl.log" >&2
    echo "  FAIL: current_run_jsonl_is_valid (log $WORK/jsonl.log)"
    failures=$((failures + 1))
fi

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_rust_constants: PASS"
    exit 0
fi
echo "test_rust_constants: FAIL ($failures case(s))"
exit 1
