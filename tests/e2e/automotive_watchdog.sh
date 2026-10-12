#!/usr/bin/env bash
# automotive_watchdog.sh — e2e family for automotive watchdog/degraded-mode (bd-1md.18)
#
# Exercises: checkpoint cooperation, degraded-mode phase progression,
# deadline miss forced completion, watchdog containment, and trace
# digest determinism.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ASX_E2E_PROFILE="${ASX_E2E_PROFILE:-AUTOMOTIVE}"
export ASX_E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-VERT-AUTOMOTIVE-WATCHDOG}"
source "$SCRIPT_DIR/harness.sh"

e2e_init "automotive-watchdog" "E2E-VERT-AUTO"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_automotive_watchdog"
E2E_BUILD_DIR="${E2E_BUILD_DIR}/automotive-watchdog"

# Build the same profile and mode as this family's caller, independently of
# whichever library the preceding family left in the shared build directory.
if ! "${MAKE:-make}" -C "$E2E_PROJECT_ROOT" build PROFILE="$E2E_PROFILE" \
    CODEC="$(printf '%s' "$E2E_CODEC" | tr '[:lower:]' '[:upper:]')" \
    DETERMINISTIC="$E2E_DETERMINISTIC" BUILD_DIR="$E2E_BUILD_DIR"; then
    e2e_scenario "automotive_watchdog.lib_build" "make build failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "automotive_watchdog.lib_build" "" "pass"

if ! e2e_build "${SCRIPT_DIR}/e2e_automotive_watchdog.c" "$E2E_BIN"; then
    e2e_scenario "automotive_watchdog.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "automotive_watchdog.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/automotive_watchdog.stderr" "automotive_watchdog"
OUTPUT="$E2E_LAST_OUTPUT"

DIGEST=""
if echo "$OUTPUT" | grep -q "^DIGEST "; then
    DIGEST="$(echo "$OUTPUT" | grep "^DIGEST " | tail -1 | cut -d' ' -f2)"
fi
if [ -n "$DIGEST" ]; then
    e2e_scenario "automotive_watchdog.trace_digest" "" "pass" "sha256:${DIGEST}"
fi

set +e
e2e_finish
FINISH_RC=$?
set -e
exit $FINISH_RC
