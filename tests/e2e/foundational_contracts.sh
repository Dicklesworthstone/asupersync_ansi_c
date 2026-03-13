#!/usr/bin/env bash
# foundational_contracts.sh — e2e family for foundational type contracts (bd-1eqo.15.1)
#
# Exercises: budget-bounded lifecycle, cancel strengthening, cleanup
# budget enforcement, outcome aggregation, handle generation safety,
# policy containment, cancel chain + budget interaction, and full
# lifecycle composition of budget + cancel + outcome.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Source shared harness
source "$SCRIPT_DIR/harness.sh"

# -------------------------------------------------------------------
# Init
# -------------------------------------------------------------------

e2e_init "foundational-contracts" "E2E-FOUNDATIONAL-CONTRACTS"

# -------------------------------------------------------------------
# Build C test binary
# -------------------------------------------------------------------

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_foundational_contracts"

if ! e2e_build "${SCRIPT_DIR}/e2e_foundational_contracts.c" "$E2E_BIN"; then
    e2e_scenario "foundational_contracts.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "foundational_contracts.build" "" "pass"

# -------------------------------------------------------------------
# Run binary and parse scenario results
# -------------------------------------------------------------------

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/foundational_contracts.stderr" "foundational_contracts"

# -------------------------------------------------------------------
# Finish
# -------------------------------------------------------------------

set +e
e2e_finish
FINISH_RC=$?
set -e

exit $FINISH_RC
