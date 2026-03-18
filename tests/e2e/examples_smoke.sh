#!/usr/bin/env bash
# examples_smoke.sh — e2e family for canonical example smoke tests (bd-1eqo.9.3)
#
# Builds and runs all canonical examples from examples/ as deterministic
# smoke tests. Each example emits SCENARIO lines parsed by the harness.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Source shared harness
source "$SCRIPT_DIR/harness.sh"

# -------------------------------------------------------------------
# Init
# -------------------------------------------------------------------

e2e_init "examples-smoke" "E2E-EXAMPLES-SMOKE"

# -------------------------------------------------------------------
# Build and run each example
# -------------------------------------------------------------------

EXAMPLES_DIR="${E2E_PROJECT_ROOT}/examples"
EXAMPLE_SOURCES=(
    ex_quickstart.c
    ex_actor_supervision.c
    ex_cancel_drain.c
    ex_channel_flow.c
    ex_network_surface.c
    ex_timeout_deadline.c
    ex_lab_replay.c
)

for src in "${EXAMPLE_SOURCES[@]}"; do
    name="${src%.c}"

    EX_BIN="${E2E_ARTIFACT_DIR}/${name}"

    if ! e2e_build "${EXAMPLES_DIR}/${src}" "$EX_BIN"; then
        e2e_scenario "${name}.build" "compilation failed" "fail"
        continue
    fi
    e2e_scenario "${name}.build" "" "pass"

    e2e_run_binary "$EX_BIN" "${E2E_ARTIFACT_DIR}/${name}.stderr" "$name"
done

# -------------------------------------------------------------------
# Finish
# -------------------------------------------------------------------

set +e
e2e_finish
FINISH_RC=$?
set -e

exit $FINISH_RC
