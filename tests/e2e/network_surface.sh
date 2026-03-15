#!/usr/bin/env bash
# network_surface.sh — e2e lane for deterministic network/public runtime surfaces
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ASX_E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-NETWORK-SURFACE-SMOKE}"
source "$SCRIPT_DIR/harness.sh"

e2e_init "network-surface" "E2E-NETWORK-SURFACE"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_network_surface"

if ! ~/.local/bin/rch exec -- make -B build; then
    e2e_scenario "network_surface.lib_build" "make build failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "network_surface.lib_build" "" "pass"

if ! ~/.local/bin/rch exec -- cc \
    -std=c99 -Wall -Wextra -Wpedantic -Werror \
    -Wconversion -Wsign-conversion -Wshadow \
    -Wstrict-prototypes -Wmissing-prototypes \
    -Wswitch-enum -Wformat=2 -Wno-unused-parameter \
    -I"${E2E_PROJECT_ROOT}/include" \
    -I"${E2E_PROJECT_ROOT}/tests" \
    -I"${E2E_PROJECT_ROOT}/src" \
    -DASX_PROFILE_CORE \
    -DASX_CODEC_JSON \
    -DASX_DETERMINISTIC=1 \
    "${SCRIPT_DIR}/e2e_network_surface.c" \
    "${E2E_BUILD_DIR}/lib/libasx.a" \
    -o "$E2E_BIN"; then
    e2e_scenario "network_surface.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "network_surface.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/network_surface.stderr" "network_surface"
OUTPUT="$E2E_LAST_OUTPUT"

DIGEST=""
if echo "$OUTPUT" | grep -q "^DIGEST "; then
    DIGEST="$(echo "$OUTPUT" | grep "^DIGEST " | tail -1 | cut -d' ' -f2)"
fi
if [ -n "$DIGEST" ]; then
    e2e_scenario "network_surface.digest" "" "pass" "sha256:${DIGEST}"
fi

set +e
e2e_finish
FINISH_RC=$?
set -e
exit $FINISH_RC
