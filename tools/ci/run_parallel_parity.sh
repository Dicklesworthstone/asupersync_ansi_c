#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPORT_DIR="${REPORT_DIR:-$REPO_ROOT/tools/ci/artifacts/conformance}"
RUN_ID="${RUN_ID:-parallel-parity-$(date -u +%Y%m%dT%H%M%SZ)}"
BUILD_ROOT="${BUILD_DIR:-build}"
MODE="parallel-parity"

if [[ "$BUILD_ROOT" = /* ]]; then
  TEST_BIN="$BUILD_ROOT/tests/conformance/parallel_parity_conformance_test"
  MAKE_TARGET="$TEST_BIN"
  ARTIFACT_DIR="$BUILD_ROOT/conformance"
else
  TEST_BIN="$REPO_ROOT/$BUILD_ROOT/tests/conformance/parallel_parity_conformance_test"
  MAKE_TARGET="$BUILD_ROOT/tests/conformance/parallel_parity_conformance_test"
  ARTIFACT_DIR="$REPO_ROOT/$BUILD_ROOT/conformance"
fi

mkdir -p "$REPORT_DIR" "$ARTIFACT_DIR"

REPORT_FILE="$REPORT_DIR/${RUN_ID}-${MODE}.jsonl"
SUMMARY_FILE="$REPORT_DIR/${RUN_ID}-${MODE}.summary.json"
BUILD_LOG="$REPORT_DIR/${RUN_ID}-${MODE}.build.log"
RUN_LOG="$REPORT_DIR/${RUN_ID}-${MODE}.run.log"
ARTIFACT_FILE="$ARTIFACT_DIR/parallel_parity_${RUN_ID}.json"

: >"$REPORT_FILE"

if ! command -v jq >/dev/null 2>&1; then
  echo "[asx] parallel-parity: FAIL (jq is required)" >&2
  exit 1
fi

echo "[asx] parallel-parity: building conformance binary..." >&2
set +e
make -C "$REPO_ROOT" "$MAKE_TARGET" "BUILD_DIR=$BUILD_ROOT" >"$BUILD_LOG" 2>&1
build_rc=$?
set -e

run_rc=0
if [[ "$build_rc" -eq 0 ]]; then
  echo "[asx] parallel-parity: running single-vs-multi-worker scenarios..." >&2
  set +e
  "$TEST_BIN" >"$REPORT_FILE" 2>"$RUN_LOG"
  run_rc=$?
  set -e
else
  run_rc=1
  : >"$RUN_LOG"
fi

record_count="$(jq -s 'map(select(.kind == "parallel_parity_record")) | length' "$REPORT_FILE")"
fail_count="$(jq -s 'map(select(.kind == "parallel_parity_record" and .parity == "fail")) | length' "$REPORT_FILE")"
skip_count="$(jq -s 'map(select(.kind == "parallel_parity_record" and .parity == "skip")) | length' "$REPORT_FILE")"
scenario_count="$(jq -s 'map(select(.kind == "parallel_parity_record") | .scenario_id) | unique | length' "$REPORT_FILE")"
compared_records="$(jq -s '((map(select(.kind == "parallel_parity_summary")) | .[-1].compared_records) // 0)' "$REPORT_FILE")"
liveness_records="$(jq -s 'map(select(.kind == "parallel_parity_record" and .scenario_id == "parallel.liveness.mixed-quiescence")) | length' "$REPORT_FILE")"
liveness_fail_count="$(jq -s 'map(select(.kind == "parallel_parity_record" and .scenario_id == "parallel.liveness.mixed-quiescence" and ((.liveness.quiescent // 0) != 1 or (.liveness.bounded_liveness // 0) != 1 or .parity != "pass"))) | length' "$REPORT_FILE")"
commit_drift_count="$(jq -s 'map(select(.kind == "parallel_parity_record" and ((.commit_authority.drift_detected // 0) != 0))) | length' "$REPORT_FILE")"
native_live_records="$(jq -s 'map(select(.kind == "parallel_parity_record" and ((.commit_authority.native_live_enabled // 0) != 0))) | length' "$REPORT_FILE")"
native_fail_closed_records="$(jq -s 'map(select(.kind == "parallel_parity_record" and ((.commit_authority.native_live_enabled // 0) == 0) and ((.commit_authority.native_live_status_code // 0) != 0))) | length' "$REPORT_FILE")"
summary_status="$(jq -s -r '((map(select(.kind == "parallel_parity_summary")) | .[-1].status) // "fail")' "$REPORT_FILE")"
profiles_json="$(jq -s 'map(select(.kind == "parallel_parity_record") | .profile) | unique' "$REPORT_FILE")"
workers_json="$(jq -s 'map(select(.kind == "parallel_parity_record" and .parity != "skip") | .worker_count) | unique' "$REPORT_FILE")"

status="pass"
diagnostic="parallel worker digests, structured event summaries, and commit authority match"
if [[ "$build_rc" -ne 0 ]]; then
  status="fail"
  diagnostic="parallel parity conformance binary failed to build"
elif [[ "$run_rc" -ne 0 || "$summary_status" != "pass" || "$fail_count" -ne 0 || "$commit_drift_count" -ne 0 || "$liveness_fail_count" -ne 0 ]]; then
  status="fail"
  diagnostic="parallel parity mismatch, liveness/quiescence failure, commit-authority drift, or test failure; inspect run log and JSONL report"
fi

jq -n \
  --arg run_id "$RUN_ID" \
  --arg mode "$MODE" \
  --arg status "$status" \
  --arg diagnostic "$diagnostic" \
  --arg report_file "$REPORT_FILE" \
  --arg summary_file "$SUMMARY_FILE" \
  --arg build_log "$BUILD_LOG" \
  --arg run_log "$RUN_LOG" \
  --arg artifact_file "$ARTIFACT_FILE" \
  --arg rerun "make parallel-parity" \
  --argjson build_rc "$build_rc" \
  --argjson run_rc "$run_rc" \
  --argjson record_count "$record_count" \
  --argjson fail_count "$fail_count" \
  --argjson skip_count "$skip_count" \
  --argjson scenario_count "$scenario_count" \
  --argjson compared_records "$compared_records" \
  --argjson liveness_records "$liveness_records" \
  --argjson liveness_fail_count "$liveness_fail_count" \
  --argjson commit_drift_count "$commit_drift_count" \
  --argjson native_live_records "$native_live_records" \
  --argjson native_fail_closed_records "$native_fail_closed_records" \
  --argjson profiles "$profiles_json" \
  --argjson workers "$workers_json" \
  '{
    kind: "parallel_parity_summary",
    run_id: $run_id,
    mode: $mode,
    status: $status,
    pass: (if $status == "pass" then 1 else 0 end),
    fail: (if $status == "pass" then 0 else 1 end),
    skip: $skip_count,
    total: $record_count,
    scenario_count: $scenario_count,
    compared_records: $compared_records,
    liveness: {
      scenario_id: "parallel.liveness.mixed-quiescence",
      records: $liveness_records,
      fail_records: $liveness_fail_count
    },
    fail_records: $fail_count,
    native_adapter_commit_authority: {
      drift_records: $commit_drift_count,
      native_live_records: $native_live_records,
      fail_closed_records: $native_fail_closed_records
    },
    profiles: $profiles,
    worker_counts: $workers,
    report_file: $report_file,
    summary_file: $summary_file,
    artifact_file: $artifact_file,
    logs: {
      build: $build_log,
      run: $run_log
    },
    exit_codes: {
      build: $build_rc,
      run: $run_rc
    },
    semantic_delta_count: ($fail_count + $commit_drift_count + $liveness_fail_count),
    rerun: $rerun,
    diagnostic: $diagnostic
  }' >"$SUMMARY_FILE"

cp "$SUMMARY_FILE" "$ARTIFACT_FILE"

echo "[asx] parallel-parity: status=$status records=$record_count compared=$compared_records artifact=$ARTIFACT_FILE" >&2
if [[ "$status" != "pass" ]]; then
  exit 1
fi
