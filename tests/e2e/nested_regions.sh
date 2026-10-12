#!/usr/bin/env bash
# nested_regions.sh — e2e family for nested parent/child region lifecycle
#
# Exercises nested region shutdown ordering, unlinking, and Q2 quiescence
# evidence reporting, body-driven normal/exceptional close and scope retry.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

source "$SCRIPT_DIR/harness.sh"

e2e_init "nested-regions" "E2E-NESTED-REGIONS"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_nested_regions"
NESTED_FLAGS=""
if [ -n "${ASX_E2E_RESOURCE_CLASS:-${RESOURCE_CLASS:-}}" ]; then
    case "${ASX_E2E_RESOURCE_CLASS:-${RESOURCE_CLASS}}" in
        R1|1) NESTED_FLAGS="-DASX_RESOURCE_CLASS=1" ;;
        R2|2) NESTED_FLAGS="-DASX_RESOURCE_CLASS=2" ;;
        R3|3) NESTED_FLAGS="-DASX_RESOURCE_CLASS=3" ;;
        *) echo "[e2e] invalid nested-region resource class" >&2; exit 2 ;;
    esac
fi

if ! e2e_build "${SCRIPT_DIR}/e2e_nested_regions.c" "$E2E_BIN" "$NESTED_FLAGS"; then
    e2e_scenario "nested_regions.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
cat "${E2E_BIN}.build.log" >&2
e2e_scenario "nested_regions.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/nested_regions.stderr" "nested_regions"
OUTPUT="$E2E_LAST_OUTPUT"
cat "${E2E_ARTIFACT_DIR}/nested_regions.stderr" >&2

DIGEST="$(printf '%s\n' "$OUTPUT" | awk '$1 == "DIGEST" { digest=$2 } END { print digest }')"

if [ -n "$DIGEST" ]; then
    e2e_scenario "nested_regions.trace_digest" "" "pass" "sha256:${DIGEST}"
fi

set +e
e2e_finish
FINISH_RC=$?
set -e

exit $FINISH_RC
