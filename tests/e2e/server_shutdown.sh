#!/usr/bin/env bash
# server_shutdown.sh — native bootstrap/shutdown acceptance lane
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ASX_E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-SERVER-SHUTDOWN}"
source "$SCRIPT_DIR/harness.sh"

e2e_init "server-shutdown" "E2E-SERVER-SHUTDOWN"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_server_shutdown"

# Build libasx.a if not present
LIB_A="${E2E_PROJECT_ROOT}/build/lib/libasx.a"
if [ ! -f "$LIB_A" ]; then
    if ! ~/.local/bin/rch exec -- make -C "${E2E_PROJECT_ROOT}" build 2>/dev/null; then
        e2e_scenario "server_shutdown.build" "library build failed" "fail"
        e2e_finish
        exit $?
    fi
fi

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
    "${SCRIPT_DIR}/e2e_server_shutdown.c" \
    "$LIB_A" \
    -o "$E2E_BIN"; then
    e2e_scenario "server_shutdown.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "server_shutdown.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/server_shutdown.stderr" "server_shutdown"
OUTPUT="$E2E_LAST_OUTPUT"

DIGEST=""
if echo "$OUTPUT" | grep -q "^DIGEST "; then
    DIGEST="$(echo "$OUTPUT" | grep "^DIGEST " | tail -1 | cut -d' ' -f2)"
fi
if [ -n "$DIGEST" ]; then
    e2e_scenario "server_shutdown.digest" "" "pass" "sha256:${DIGEST}"
fi

set +e
e2e_finish
FINISH_RC=$?
set -e
exit $FINISH_RC
