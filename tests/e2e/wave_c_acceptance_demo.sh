#!/usr/bin/env bash
# wave_c_acceptance_demo.sh -- user-facing Wave C acceptance evidence bundle
#
# This lane assembles existing Wave C evidence into one rerunnable artifact
# bundle. It intentionally records and checks existing gates rather than adding
# new runtime semantics.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
if [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="${PROJECT_ROOT}/${BUILD_DIR}"
fi

TIMESTAMP="$(date -u '+%Y%m%dT%H%M%SZ')"
RUN_ID="${ASX_E2E_RUN_ID:-wave-c-acceptance-${TIMESTAMP}}"
ARTIFACT_DIR="${ASX_E2E_ARTIFACT_DIR:-${BUILD_DIR}/e2e-artifacts/${RUN_ID}}"
REPORT_FILE="${BUILD_DIR}/test-logs/e2e-wave-c-acceptance.jsonl"
SUMMARY_FILE="${ARTIFACT_DIR}/wave_c_acceptance.summary.json"
CONFIG_FILE="${ARTIFACT_DIR}/wave_c_acceptance.config.json"
EXPECTED_FILE="${ARTIFACT_DIR}/wave_c_acceptance.expected_outputs.json"
RERUN_FILE="${ARTIFACT_DIR}/wave_c_acceptance.rerun.txt"
UNSUPPORTED_FILE="${ARTIFACT_DIR}/wave_c_acceptance.unsupported_platforms.json"

PASS_COUNT=0
FAIL_COUNT=0
TOTAL_COUNT=0

mkdir -p "$ARTIFACT_DIR" "${BUILD_DIR}/test-logs"
: > "$REPORT_FILE"

json_str() {
    printf '"%s"' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/\\t/g')"
}

record() {
    local step="$1"
    local status="$2"
    local detail="${3:-}"

    TOTAL_COUNT=$((TOTAL_COUNT + 1))
    case "$status" in
        pass) PASS_COUNT=$((PASS_COUNT + 1)) ;;
        fail) FAIL_COUNT=$((FAIL_COUNT + 1)) ;;
        *) status="fail"; FAIL_COUNT=$((FAIL_COUNT + 1)) ;;
    esac

    {
        printf '{'
        printf '"run_id":%s' "$(json_str "$RUN_ID")"
        printf ',"step":%s' "$(json_str "$step")"
        printf ',"status":%s' "$(json_str "$status")"
        if [ -n "$detail" ]; then
            printf ',"detail":%s' "$(json_str "$detail")"
        fi
        printf '}\n'
    } >> "$REPORT_FILE"
}

run_step() {
    local step="$1"
    local command="$2"
    local stdout_file="${ARTIFACT_DIR}/${step}.stdout"
    local stderr_file="${ARTIFACT_DIR}/${step}.stderr"
    local rc=0

    printf '%s\n' "$command" > "${ARTIFACT_DIR}/${step}.command.txt"
    set +e
    (cd "$PROJECT_ROOT" && bash -c "$command") >"$stdout_file" 2>"$stderr_file"
    rc=$?
    set -e

    if [ "$rc" -eq 0 ]; then
        record "$step" "pass" "stdout=${stdout_file} stderr=${stderr_file}"
    else
        record "$step" "fail" "exit=${rc} stdout=${stdout_file} stderr=${stderr_file}"
    fi
    return 0
}

require_nonempty_file() {
    local step="$1"
    local path="$2"

    if [ -n "$path" ] && [ -s "$path" ]; then
        record "$step" "pass" "$path"
    else
        record "$step" "fail" "missing or empty: ${path}"
    fi
}

cat > "$CONFIG_FILE" <<CONFIG
{
  "schema": "asx.wave_c_acceptance.config.v1",
  "run_id": "$(printf '%s' "$RUN_ID")",
  "profile": "PARALLEL",
  "codec": "json",
  "deterministic": 1,
  "parallel_workers": 64,
  "parallel_scale": "large",
  "resource_class": "R3",
  "semantic_policy": "acceptance evidence is resource-plane only; profile/parity gates remain authoritative"
}
CONFIG

cat > "$EXPECTED_FILE" <<EXPECTED
{
  "schema": "asx.wave_c_acceptance.expected_outputs.v1",
  "required_artifacts": [
    "wave_c_acceptance.config.json",
    "wave_c_acceptance.summary.json",
    "wave_c_acceptance.expected_outputs.json",
    "wave_c_acceptance.rerun.txt",
    "wave_c_acceptance.unsupported_platforms.json",
    "parallel_swarm.incident_bundle.jsonl",
    "parallel_swarm.details.jsonl",
    "parallel-bench-gate-summary.json",
    "parallel-parity summary",
    "profile-parity summary",
    "parallel-parity fail-closed diagnostic"
  ]
}
EXPECTED

printf 'rch exec -- make wave-c-acceptance-demo\n' > "$RERUN_FILE"

run_step "parallel_swarm_64" \
    "ASX_E2E_PROFILE=PARALLEL ASX_E2E_PARALLEL_SCALE=large ASX_E2E_PARALLEL_WORKERS=64 ASX_E2E_RUN_ID=${RUN_ID}-parallel ASX_E2E_ARTIFACT_DIR=${ARTIFACT_DIR}/parallel-swarm ${SCRIPT_DIR}/parallel_swarm.sh"
run_step "network_surface" \
    "ASX_E2E_RUN_ID=${RUN_ID}-network ASX_E2E_ARTIFACT_DIR=${ARTIFACT_DIR}/network-surface make test-e2e-network-surface"
run_step "actor_supervision" \
    "ASX_E2E_RUN_ID=${RUN_ID}-actor ASX_E2E_ARTIFACT_DIR=${ARTIFACT_DIR}/actor-supervision make test-e2e-actor-supervision"
run_step "parallel_parity" "make parallel-parity"
run_step "profile_parity" "make profile-parity"
run_step "parallel_bench_gate" "make parallel-bench-gate"

PARALLEL_INCIDENT="${ARTIFACT_DIR}/parallel-swarm/parallel_swarm.incident_bundle.jsonl"
PARALLEL_DETAIL="${ARTIFACT_DIR}/parallel-swarm/parallel_swarm.details.jsonl"
BENCH_SUMMARY="${PROJECT_ROOT}/build/perf/parallel-bench-gate-summary.json"
PARALLEL_PARITY_STDOUT="${ARTIFACT_DIR}/parallel_parity.stdout"
PARALLEL_PARITY_SUMMARY="$(ls -t "${PROJECT_ROOT}"/tools/ci/artifacts/conformance/parallel-parity-*-parallel-parity.summary.json 2>/dev/null | head -1 || true)"
PROFILE_SUMMARY="$(ls -t "${PROJECT_ROOT}"/tools/ci/artifacts/conformance/profile-parity-*-profile-parity.summary.json 2>/dev/null | head -1 || true)"

require_nonempty_file "artifact.config" "$CONFIG_FILE"
require_nonempty_file "artifact.expected_outputs" "$EXPECTED_FILE"
require_nonempty_file "artifact.rerun" "$RERUN_FILE"
require_nonempty_file "artifact.parallel_incident_bundle" "$PARALLEL_INCIDENT"
require_nonempty_file "artifact.parallel_detail" "$PARALLEL_DETAIL"
require_nonempty_file "artifact.benchmark_summary" "$BENCH_SUMMARY"
require_nonempty_file "artifact.parallel_parity_summary" "$PARALLEL_PARITY_SUMMARY"
require_nonempty_file "artifact.profile_parity_summary" "$PROFILE_SUMMARY"

NATIVE_FAIL_CLOSED_COUNT=0
if [ -s "$PARALLEL_PARITY_SUMMARY" ]; then
    NATIVE_FAIL_CLOSED_COUNT="$(jq -r '.native_adapter_commit_authority.fail_closed_records // 0' "$PARALLEL_PARITY_SUMMARY")"
fi
if [ "$NATIVE_FAIL_CLOSED_COUNT" -gt 0 ] || grep -q "capability not granted" "$PARALLEL_PARITY_STDOUT" || grep -q '"native_live_status":"capability not granted"' "$PARALLEL_INCIDENT"; then
    record "fail_closed.native_live_parallel" "pass" "native fail-closed records=${NATIVE_FAIL_CLOSED_COUNT}"
else
    record "fail_closed.native_live_parallel" "fail" "missing fail-closed native-live diagnostic"
fi

REPLAY_COMMAND="$(jq -r '(.replay_command // .artifacts.replay_command // empty)' "$PARALLEL_INCIDENT" 2>/dev/null | head -1 || true)"
if [ -n "$REPLAY_COMMAND" ]; then
    printf '%s\n' "$REPLAY_COMMAND" > "$RERUN_FILE"
    record "artifact.replay_command" "pass" "$REPLAY_COMMAND"
else
    record "artifact.replay_command" "fail" "no replay_command in ${PARALLEL_INCIDENT}"
fi

cat > "$UNSUPPORTED_FILE" <<UNSUPPORTED
{
  "schema": "asx.wave_c_acceptance.unsupported_platforms.v1",
  "entries": [
    {
      "surface": "native_live_parallel_workers",
      "status": "fail_closed",
      "diagnostic": "capability not granted",
      "source": "parallel_parity.stdout"
    },
    {
      "surface": "unsupported_platform_live_claims",
      "status": "forbidden",
      "diagnostic": "acceptance evidence must report unsupported live-mode behavior as fail-closed, not pass"
    }
  ]
}
UNSUPPORTED
require_nonempty_file "artifact.unsupported_platforms" "$UNSUPPORTED_FILE"

if [ -s "$BENCH_SUMMARY" ]; then
    BENCH_WARN_COUNT="$(jq -r '.overload_slo_summary.status_counts.warn // 0' "$BENCH_SUMMARY")"
    BENCH_BLOCK_COUNT="$(jq -r '.overload_slo_summary.status_counts.block // 0' "$BENCH_SUMMARY")"
else
    BENCH_WARN_COUNT=0
    BENCH_BLOCK_COUNT=0
fi
PROFILE_STATUS="$(jq -r '.status // .summary.status // "pass"' "$PROFILE_SUMMARY" 2>/dev/null || echo "pass")"
OVERALL_STATUS="pass"
if [ "$FAIL_COUNT" -gt 0 ]; then
    OVERALL_STATUS="fail"
fi

{
    printf '{\n'
    printf '  "schema": "asx.wave_c_acceptance.summary.v1",\n'
    printf '  "run_id": %s,\n' "$(json_str "$RUN_ID")"
    printf '  "status": %s,\n' "$(json_str "$OVERALL_STATUS")"
    printf '  "total": %d,\n' "$TOTAL_COUNT"
    printf '  "pass": %d,\n' "$PASS_COUNT"
    printf '  "fail": %d,\n' "$FAIL_COUNT"
    printf '  "config": %s,\n' "$(json_str "$CONFIG_FILE")"
    printf '  "expected_outputs": %s,\n' "$(json_str "$EXPECTED_FILE")"
    printf '  "rerun_command_file": %s,\n' "$(json_str "$RERUN_FILE")"
    printf '  "parallel_incident_bundle": %s,\n' "$(json_str "$PARALLEL_INCIDENT")"
    printf '  "parallel_detail_records": %s,\n' "$(json_str "$PARALLEL_DETAIL")"
    printf '  "parallel_parity_summary": %s,\n' "$(json_str "$PARALLEL_PARITY_SUMMARY")"
    printf '  "profile_parity_summary": %s,\n' "$(json_str "$PROFILE_SUMMARY")"
    printf '  "benchmark_summary": %s,\n' "$(json_str "$BENCH_SUMMARY")"
    printf '  "unsupported_platforms": %s,\n' "$(json_str "$UNSUPPORTED_FILE")"
    printf '  "benchmark_warn_count": %s,\n' "$BENCH_WARN_COUNT"
    printf '  "benchmark_block_count": %s,\n' "$BENCH_BLOCK_COUNT"
    printf '  "native_fail_closed_count": %s,\n' "$NATIVE_FAIL_CLOSED_COUNT"
    printf '  "profile_status": %s\n' "$(json_str "$PROFILE_STATUS")"
    printf '}\n'
} > "$SUMMARY_FILE"

record "_summary" "$OVERALL_STATUS" "summary=${SUMMARY_FILE}"

printf '[asx] wave-c-acceptance-demo: status=%s pass=%d fail=%d summary=%s\n' \
    "$OVERALL_STATUS" "$PASS_COUNT" "$FAIL_COUNT" "$SUMMARY_FILE"

if [ "$OVERALL_STATUS" != "pass" ]; then
    exit 1
fi
