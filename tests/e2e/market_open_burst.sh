#!/usr/bin/env bash
# market_open_burst.sh — e2e family for HFT market-open burst deployment hardening (bd-j4m.6)
#
# Exercises: extreme admission spike, burst with interleaved obligations,
# partial budget drain, burst recovery, mass cancel during burst,
# multi-region isolation, and deterministic trace digest stability.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ASX_E2E_PROFILE="${ASX_E2E_PROFILE:-HFT}"
export ASX_E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-DEPLOY-MARKET-BURST}"
source "$SCRIPT_DIR/harness.sh"

e2e_init "market-open-burst" "E2E-DEPLOY-HFT"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_market_open_burst"

if ! e2e_build "${SCRIPT_DIR}/e2e_market_open_burst.c" "$E2E_BIN"; then
    e2e_scenario "market_open_burst.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "market_open_burst.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/market_open_burst.stderr" "market_open_burst"
OUTPUT="$E2E_LAST_OUTPUT"

DIGEST=""
if echo "$OUTPUT" | grep -q "^DIGEST "; then
    DIGEST="$(echo "$OUTPUT" | grep "^DIGEST " | tail -1 | cut -d' ' -f2)"
fi
if [ -n "$DIGEST" ]; then
    e2e_scenario "market_open_burst.trace_digest" "" "pass" "sha256:${DIGEST}"
fi

set +e
e2e_finish
FINISH_RC=$?
set -e
exit $FINISH_RC
