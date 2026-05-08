#!/usr/bin/env bash
# parallel_swarm.sh -- e2e family for large-swarm parallel runtime pressure
#
# Default mode is a CI-smoke profile. Set ASX_E2E_PARALLEL_SCALE=large
# or nightly/profile for the larger run while preserving the same JSONL
# schema and rerun contract.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ASX_E2E_PROFILE="${ASX_E2E_PROFILE:-PARALLEL}"
export ASX_E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-PARALLEL-SWARM}"
export ASX_E2E_PARALLEL_SCALE="${ASX_E2E_PARALLEL_SCALE:-smoke}"
export ASX_E2E_PARALLEL_WORKERS="${ASX_E2E_PARALLEL_WORKERS:-8}"
source "$SCRIPT_DIR/harness.sh"

e2e_init "parallel-swarm" "E2E-PARALLEL-SWARM"
export ASX_E2E_RUN_ID="$E2E_RUN_ID"
export ASX_E2E_SEED="$E2E_SEED"
export ASX_E2E_SCENARIO_PACK="$E2E_SCENARIO_PACK"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_parallel_swarm"
DETAIL_FILE="${E2E_ARTIFACT_DIR}/parallel_swarm.details.jsonl"

if ! e2e_build "${SCRIPT_DIR}/e2e_parallel_swarm.c" "$E2E_BIN"; then
    e2e_scenario "parallel_swarm.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "parallel_swarm.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/parallel_swarm.stderr" "parallel_swarm"
OUTPUT="$E2E_LAST_OUTPUT"

: > "$DETAIL_FILE"
while IFS= read -r line; do
    case "$line" in
        DETAIL\ *) printf '%s\n' "${line#DETAIL }" >> "$DETAIL_FILE" ;;
    esac
done <<< "$OUTPUT"

DETAIL_COUNT="$(wc -l < "$DETAIL_FILE" | tr -d ' ')"
if [ "$DETAIL_COUNT" -gt 0 ]; then
    e2e_scenario "parallel_swarm.details_jsonl" "records=${DETAIL_COUNT} file=${DETAIL_FILE}" "pass"
else
    e2e_scenario "parallel_swarm.details_jsonl" "no DETAIL JSONL records emitted" "fail"
fi

DIGEST=""
while IFS= read -r line; do
    case "$line" in
        DIGEST\ *) DIGEST="${line#DIGEST }" ;;
    esac
done <<< "$OUTPUT"
if [ -n "$DIGEST" ]; then
    e2e_scenario "parallel_swarm.aggregate_digest" "" "pass" "sha256:${DIGEST}"
else
    e2e_scenario "parallel_swarm.aggregate_digest" "no aggregate digest emitted" "fail"
fi

e2e_scenario "parallel_swarm.scale.${ASX_E2E_PARALLEL_SCALE}" \
    "workers=${ASX_E2E_PARALLEL_WORKERS} detail=${DETAIL_FILE}" "pass"

set +e
e2e_finish
FINISH_RC=$?
set -e
exit $FINISH_RC
