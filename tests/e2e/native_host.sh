#!/usr/bin/env bash
# native_host.sh — e2e lane for deterministic native host surfaces
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ASX_E2E_POLICY_ID="${ASX_E2E_POLICY_ID:-NATIVE-HOST-SURFACES}"
source "$SCRIPT_DIR/harness.sh"

e2e_init "native-host" "E2E-NATIVE-HOST"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_native_host"

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
    "${SCRIPT_DIR}/e2e_native_host.c" \
    "${E2E_PROJECT_ROOT}/src/fs/fs.c" \
    "${E2E_PROJECT_ROOT}/src/process/process.c" \
    "${E2E_PROJECT_ROOT}/src/signal/signal.c" \
    "${E2E_PROJECT_ROOT}/src/bytes/buf.c" \
    -o "$E2E_BIN"; then
    e2e_scenario "native_host.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "native_host.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/native_host.stderr" "native_host"
OUTPUT="$E2E_LAST_OUTPUT"

DIGEST=""
if echo "$OUTPUT" | grep -q "^DIGEST "; then
    DIGEST="$(echo "$OUTPUT" | grep "^DIGEST " | tail -1 | cut -d' ' -f2)"
fi
if [ -n "$DIGEST" ]; then
    e2e_scenario "native_host.digest" "" "pass" "sha256:${DIGEST}"
fi

set +e
e2e_finish
FINISH_RC=$?
set -e
exit $FINISH_RC
