#!/usr/bin/env bash
# nested_regions.sh — e2e family for nested parent/child region lifecycle
#
# Exercises nested region shutdown ordering, unlinking, and Q2 quiescence
# evidence reporting.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

source "$SCRIPT_DIR/harness.sh"

e2e_init "nested-regions" "E2E-NESTED-REGIONS"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_nested_regions"

if ! e2e_build "${SCRIPT_DIR}/e2e_nested_regions.c" "$E2E_BIN"; then
    e2e_scenario "nested_regions.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "nested_regions.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/nested_regions.stderr" "nested_regions"
OUTPUT="$E2E_LAST_OUTPUT"

DIGEST=""
if echo "$OUTPUT" | grep -q "^DIGEST "; then
    DIGEST="$(echo "$OUTPUT" | grep "^DIGEST " | tail -1 | cut -d' ' -f2)"
fi

if [ -n "$DIGEST" ]; then
    e2e_scenario "nested_regions.trace_digest" "" "pass" "sha256:${DIGEST}"
fi

set +e
e2e_finish
FINISH_RC=$?
set -e

exit $FINISH_RC
