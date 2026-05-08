#!/usr/bin/env bash
# posix_adapter_smoke.sh — E2E smoke for POSIX platform adapter (bd-c7nt)
#
# Exercises clock + entropy + reactor + log hooks through an integrated
# POSIX-profile build.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Force POSIX profile for this smoke test
export ASX_E2E_PROFILE="POSIX"
export ASX_E2E_DETERMINISTIC="0"

# Source shared harness
source "$SCRIPT_DIR/harness.sh"

# -------------------------------------------------------------------
# Init
# -------------------------------------------------------------------

e2e_init "posix-adapter-smoke" "E2E-POSIX-ADAPTER"

# -------------------------------------------------------------------
# Build C test binary (links against libasx.a with POSIX profile)
# -------------------------------------------------------------------

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_posix_adapter_smoke"

if ! e2e_build "${SCRIPT_DIR}/e2e_posix_adapter_smoke.c" "$E2E_BIN" \
    "-DASX_LOCKFREE_SINGLE_THREAD=0 -lpthread -lrt"; then
    e2e_scenario "posix_adapter_smoke.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "posix_adapter_smoke.build" "" "pass"

# -------------------------------------------------------------------
# Run binary and parse scenario results
# -------------------------------------------------------------------

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/posix_adapter_smoke.stderr" "posix_adapter_smoke"

# -------------------------------------------------------------------
# Finish
# -------------------------------------------------------------------

e2e_finish
