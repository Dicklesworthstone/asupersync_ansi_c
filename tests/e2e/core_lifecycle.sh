#!/usr/bin/env bash
# core_lifecycle.sh — e2e family for core runtime lifecycle scenarios (bd-1md.16)
#
# Exercises: region/task/obligation lifecycle, cancellation propagation,
# quiescence close paths, timer wheel, bounded MPSC channel, and
# deterministic trace digest.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Source shared harness
source "$SCRIPT_DIR/harness.sh"

# -------------------------------------------------------------------
# Init
# -------------------------------------------------------------------

e2e_init "core-lifecycle" "E2E-CORE-LIFECYCLE"
export ASX_E2E_RUN_ID="$E2E_RUN_ID"
export ASX_E2E_SEED="$E2E_SEED"

# Embed the checkout used to compile this binary. A dirty draft cannot
# stand in for an exact committed revision in the ownership evidence.
LIFECYCLE_BUILD_SHA="$(git -C "$E2E_PROJECT_ROOT" rev-parse HEAD)"
# Untracked tracker recovery and scratch logs are not compiler inputs.
# Untracked source/header inputs still prevent an exact-revision claim.
if [ -n "$(git -C "$E2E_PROJECT_ROOT" status --porcelain --untracked-files=no)" ] ||
    [ -n "$(git -C "$E2E_PROJECT_ROOT" ls-files --others --exclude-standard -- src include tests/e2e)" ]; then
    LIFECYCLE_BUILD_SHA="${LIFECYCLE_BUILD_SHA}_dirty_draft"
fi
LIFECYCLE_RUST_REV="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["provenance"]["rust_baseline_commit"])' \
    "$E2E_PROJECT_ROOT/fixtures/rust_reference_v2/obligation-limit-token-drop-releases-count-001.json")"
LIFECYCLE_FLAGS="-DASX_E2E_BUILD_SHA=${LIFECYCLE_BUILD_SHA} -DASX_E2E_RUST_REV=${LIFECYCLE_RUST_REV}"
# The shared harness defaults its label to R3 but unclassed libraries are
# also valid. Only an explicitly selected class changes compilation.
if [ -n "${ASX_E2E_RESOURCE_CLASS:-${RESOURCE_CLASS:-}}" ]; then
    case "${ASX_E2E_RESOURCE_CLASS:-${RESOURCE_CLASS}}" in
        R1|1) LIFECYCLE_FLAGS="$LIFECYCLE_FLAGS -DASX_RESOURCE_CLASS=1" ;;
        R2|2) LIFECYCLE_FLAGS="$LIFECYCLE_FLAGS -DASX_RESOURCE_CLASS=2" ;;
        R3|3) LIFECYCLE_FLAGS="$LIFECYCLE_FLAGS -DASX_RESOURCE_CLASS=3" ;;
        *) echo "[e2e] invalid lifecycle resource class" >&2; exit 2 ;;
    esac
fi

# -------------------------------------------------------------------
# Build C test binary
# -------------------------------------------------------------------

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_core_lifecycle"
DETAIL_FILE="${E2E_ARTIFACT_DIR}/core_lifecycle.details.jsonl"

if ! e2e_build "${SCRIPT_DIR}/e2e_core_lifecycle.c" "$E2E_BIN" "$LIFECYCLE_FLAGS"; then
    e2e_scenario "core_lifecycle.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
cat "${E2E_BIN}.build.log" >&2
e2e_scenario "core_lifecycle.build" "" "pass"

# -------------------------------------------------------------------
# Run binary and parse scenario results
# -------------------------------------------------------------------

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/core_lifecycle.stderr" "core_lifecycle"
OUTPUT="$E2E_LAST_OUTPUT"
cat "${E2E_ARTIFACT_DIR}/core_lifecycle.stderr" >&2

# Keep the artifact pure JSONL: human SCENARIO and DIGEST lines belong in
# the runner output, while these records diagnose actual actor operations.
: > "$DETAIL_FILE"
while IFS= read -r line; do
    case "$line" in
        DETAIL\ *) printf '%s\n' "${line#DETAIL }" >> "$DETAIL_FILE" ;;
    esac
done <<< "$OUTPUT"
if DETAIL_COUNT="$(python3 -c 'import json, sys; rows = [json.loads(line) for line in open(sys.argv[1])]; assert rows, "no lifecycle evidence"; assert any(row.get("lab_seed_applied") for row in rows), "no seeded Lab evidence"; print(len(rows))' "$DETAIL_FILE")"; then
    e2e_scenario "core_lifecycle.details_jsonl" "records=${DETAIL_COUNT} file=${DETAIL_FILE}" "pass"
else
    e2e_scenario "core_lifecycle.details_jsonl" "missing or malformed lifecycle evidence" "fail"
fi

# -------------------------------------------------------------------
# Compute trace digest from the final run for manifest
# -------------------------------------------------------------------

# If the binary produced a digest line, record it
DIGEST=""
if echo "$OUTPUT" | grep -q "^DIGEST "; then
    DIGEST="$(echo "$OUTPUT" | grep "^DIGEST " | tail -1 | cut -d' ' -f2)"
fi

if [ -n "$DIGEST" ]; then
    e2e_scenario "core_lifecycle.trace_digest" "" "pass" "sha256:${DIGEST}"
fi

# -------------------------------------------------------------------
# Finish
# -------------------------------------------------------------------

set +e
e2e_finish
FINISH_RC=$?
set -e

exit $FINISH_RC
