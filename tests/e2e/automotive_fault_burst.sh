#!/usr/bin/env bash
# automotive_fault_burst.sh — e2e family for automotive fault-burst deployment hardening (bd-j4m.6)
#
# Exercises: clock skew/reversal injection, constant entropy fault,
# multi-fault cascade, poison containment, deadline cancel under
# active fault, and deterministic trace digest stability.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ASX_E2E_PROFILE="${ASX_E2E_PROFILE:-AUTOMOTIVE}"
export ASX_E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-DEPLOY-AUTO-FAULT}"
source "$SCRIPT_DIR/harness.sh"

e2e_init "automotive-fault-burst" "E2E-DEPLOY-AUTO"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_automotive_fault_burst"

if ! e2e_build "${SCRIPT_DIR}/e2e_automotive_fault_burst.c" "$E2E_BIN"; then
    e2e_scenario "automotive_fault_burst.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "automotive_fault_burst.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/automotive_fault_burst.stderr" "automotive_fault_burst"
OUTPUT="$E2E_LAST_OUTPUT"

DIGEST=""
if echo "$OUTPUT" | grep -q "^DIGEST "; then
    DIGEST="$(echo "$OUTPUT" | grep "^DIGEST " | tail -1 | cut -d' ' -f2)"
fi
if [ -n "$DIGEST" ]; then
    e2e_scenario "automotive_fault_burst.trace_digest" "" "pass" "sha256:${DIGEST}"
fi

set +e
e2e_finish
FINISH_RC=$?
set -e
exit $FINISH_RC
