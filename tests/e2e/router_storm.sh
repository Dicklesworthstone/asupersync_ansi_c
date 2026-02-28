#!/usr/bin/env bash
# router_storm.sh — e2e family for embedded router storm deployment hardening (bd-j4m.6)
#
# Exercises: rapid region churn, multi-region task saturation, exhaustion
# recovery, obligation churn, poison isolation, cancel under load, and
# deterministic trace digest stability.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ASX_E2E_PROFILE="${ASX_E2E_PROFILE:-EMBEDDED_ROUTER}"
export ASX_E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-DEPLOY-ROUTER-STORM}"
source "$SCRIPT_DIR/harness.sh"

e2e_init "router-storm" "E2E-DEPLOY-ROUTER"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_router_storm"

if ! e2e_build "${SCRIPT_DIR}/e2e_router_storm.c" "$E2E_BIN"; then
    e2e_scenario "router_storm.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "router_storm.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/router_storm.stderr" "router_storm"
OUTPUT="$E2E_LAST_OUTPUT"

DIGEST=""
if echo "$OUTPUT" | grep -q "^DIGEST "; then
    DIGEST="$(echo "$OUTPUT" | grep "^DIGEST " | tail -1 | cut -d' ' -f2)"
fi
if [ -n "$DIGEST" ]; then
    e2e_scenario "router_storm.trace_digest" "" "pass" "sha256:${DIGEST}"
fi

set +e
e2e_finish
FINISH_RC=$?
set -e
exit $FINISH_RC
