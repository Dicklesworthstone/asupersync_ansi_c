#!/usr/bin/env bash
# run_resource_pressure_gate.sh — resource-pressure failure-atomic release gate
#
# Builds and runs the resource-pressure conformance scenario pack for CORE and
# a constrained EMBEDDED_ROUTER/R1 lane, then verifies status, manifest coverage,
# and profile-stable semantic digests.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

MANIFEST="${ASX_RESOURCE_PRESSURE_MANIFEST:-fixtures/resource_pressure/failure_atomic_scenarios.json}"
RUN_ID="${ASX_RESOURCE_PRESSURE_RUN_ID:-resource-pressure-$(date -u '+%Y%m%dT%H%M%SZ')-$$}"
OUT_DIR="${ASX_RESOURCE_PRESSURE_OUT_DIR:-build/resource-pressure/${RUN_ID}}"
REPORT_JSONL="${OUT_DIR}/resource_pressure_failure_atomic_cases.jsonl"
SUMMARY_JSON="${OUT_DIR}/resource_pressure_failure_atomic_summary.json"
GIT_COMMIT="${ASX_GIT_COMMIT:-}"

if [ -z "$GIT_COMMIT" ]; then
    if git rev-parse --git-dir >/dev/null 2>&1; then
        GIT_COMMIT="$(git rev-parse HEAD)"
    else
        GIT_COMMIT="unknown"
    fi
fi

if ! command -v jq >/dev/null 2>&1; then
    echo "[asx] resource-pressure-gate: FAIL (jq missing)" >&2
    exit 1
fi

if [ ! -f "$MANIFEST" ]; then
    echo "[asx] resource-pressure-gate: FAIL (manifest missing: $MANIFEST)" >&2
    exit 1
fi

jq -e '
  .schema == "asx.resource_pressure.failure_atomic_scenarios.v1"
  and (.scenarios | length) == 7
  and all(.scenarios[]; (.scenario_id | type) == "string"
      and (.surface | type) == "string"
      and (.expected_status | type) == "string"
      and (.failure_atomic | type) == "boolean")
' "$MANIFEST" >/dev/null

mkdir -p "$OUT_DIR"

run_lane() {
    lane="$1"
    profile="$2"
    resource_class="$3"
    extra_cflags="${4:-}"
    bin="build/tests/conformance/resource_pressure_failure_atomic_test"

    echo "[asx] resource-pressure-gate: lane=${lane} profile=${profile} class=${resource_class}"
    make --no-print-directory build PROFILE="$profile" CODEC=JSON DETERMINISTIC=1 CFLAGS="${extra_cflags}"
    make --no-print-directory "$bin" PROFILE="$profile" CODEC=JSON DETERMINISTIC=1 CFLAGS="${extra_cflags}"
    ASX_RESOURCE_PRESSURE_REPORT_JSONL="$REPORT_JSONL" \
        ASX_RESOURCE_PRESSURE_LANE="$lane" \
        ASX_RESOURCE_PRESSURE_PROFILE="ASX_PROFILE_${profile}" \
        ASX_RESOURCE_PRESSURE_RESOURCE_CLASS="$resource_class" \
        "$bin"
}

run_lane "core-r3" "CORE" "R3" ""
run_lane "embedded-router-r1" "EMBEDDED_ROUTER" "R1" ""

jq -e --slurpfile report "$REPORT_JSONL" '
  ([.scenarios[].scenario_id] | sort) as $expected
  | ($report | [.[].scenario_id] | unique | sort) as $actual
  | $expected == $actual
' "$MANIFEST" >/dev/null

jq -s -e 'all(.[]; .schema == "asx.resource_pressure.case.v1" and .status == "pass")' \
    "$REPORT_JSONL" >/dev/null

jq -s -e '
  sort_by(.scenario_id)
  | group_by(.scenario_id)
  | all(.[]; ([.[].semantic_digest] | unique | length) == 1)
' "$REPORT_JSONL" >/dev/null

jq -s --arg generated_at "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" \
      --arg run_id "$RUN_ID" \
      --arg git_commit "$GIT_COMMIT" \
      --arg manifest "$MANIFEST" \
      --arg cases "$REPORT_JSONL" '
  {
    schema: "asx.resource_pressure.failure_atomic_report.v1",
    generated_at: $generated_at,
    run_id: $run_id,
    git: { commit: $git_commit },
    manifest: $manifest,
    cases_path: $cases,
    lane_count: ([.[].lane] | unique | length),
    lanes: ([.[].lane] | unique | sort),
    scenario_count: ([.[].scenario_id] | unique | length),
    case_count: length,
    failure_count: ([.[] | select(.status != "pass")] | length),
    digest_parity_failures: (
      sort_by(.scenario_id)
      | group_by(.scenario_id)
      | map(select(([.[].semantic_digest] | unique | length) != 1)
            | {scenario_id: .[0].scenario_id,
               digests: ([.[].semantic_digest] | unique | sort)})
    ),
    expected_statuses: [
      .[]
      | {lane, scenario_id, surface,
         expected_status: .expected_status.name,
         actual_status: .actual_status.name,
         failure_atomic_expected,
         failure_atomic_observed,
         semantic_digest}
    ]
  }
' "$REPORT_JSONL" > "$SUMMARY_JSON"

jq -e '
  .schema == "asx.resource_pressure.failure_atomic_report.v1"
  and .lane_count == 2
  and .scenario_count == 7
  and .case_count == 14
  and .failure_count == 0
  and (.digest_parity_failures | length) == 0
' "$SUMMARY_JSON" >/dev/null

echo "[asx] resource-pressure-gate: PASS report=${SUMMARY_JSON}"
