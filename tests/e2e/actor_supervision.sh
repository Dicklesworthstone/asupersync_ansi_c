#!/usr/bin/env bash
# actor_supervision.sh -- deterministic actor/supervision semantic harness
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/harness.sh"

e2e_init "actor-supervision" "E2E-ACTOR-SUPERVISION"

E2E_BIN="${E2E_ARTIFACT_DIR}/e2e_actor_supervision"
COMMAND_TRANSCRIPT="${E2E_ARTIFACT_DIR}/actor_supervision.command.txt"
RAW_LOG="${E2E_ARTIFACT_DIR}/actor_supervision.stdout"
REPLAY_COMMAND="${E2E_ARTIFACT_DIR}/actor_supervision.replay.txt"

{
    printf 'lib_build=%s -C %s -B build\n' "${MAKE:-make}" "$E2E_PROJECT_ROOT"
    printf 'binary_build=%s -std=c99 -Wall -Wextra -Wpedantic -Werror -I%s/include -I%s/tests -I%s/src -DASX_PROFILE_%s -DASX_CODEC_%s -DASX_DETERMINISTIC=%s -o %s %s/e2e_actor_supervision.c %s/lib/libasx.a\n' \
        "${CC:-gcc}" "$E2E_PROJECT_ROOT" "$E2E_PROJECT_ROOT" "$E2E_PROJECT_ROOT" \
        "$E2E_PROFILE" "$(echo "$E2E_CODEC" | tr '[:lower:]' '[:upper:]')" \
        "$E2E_DETERMINISTIC" "$E2E_BIN" "$SCRIPT_DIR" "$E2E_BUILD_DIR"
    printf 'binary_run=%s\n' "$E2E_BIN"
} > "$COMMAND_TRANSCRIPT"

if ! "${MAKE:-make}" -C "$E2E_PROJECT_ROOT" -B build; then
    e2e_scenario "actor_supervision.lib_build" "make build failed" "fail"
    e2e_finish
    exit $?
fi

if ! e2e_build "${SCRIPT_DIR}/e2e_actor_supervision.c" "$E2E_BIN"; then
    e2e_scenario "actor_supervision.build" "compilation failed" "fail"
    e2e_finish
    exit $?
fi
e2e_scenario "actor_supervision.build" "" "pass"

e2e_run_binary "$E2E_BIN" "${E2E_ARTIFACT_DIR}/actor_supervision.stderr" "actor_supervision"
OUTPUT="$E2E_LAST_OUTPUT"
printf '%s\n' "$OUTPUT" > "$RAW_LOG"

DIGEST=""
if echo "$OUTPUT" | grep -q "^DIGEST "; then
    DIGEST="$(echo "$OUTPUT" | awk '/^DIGEST / {print $2}' | tail -1)"
fi
if [ -n "$DIGEST" ]; then
    e2e_scenario "actor_supervision.digest" "" "pass" "sha256:${DIGEST}"
else
    e2e_scenario "actor_supervision.digest" "no digest emitted" "fail"
fi

if echo "$OUTPUT" | grep -q "^REPLAY "; then
    echo "$OUTPUT" | grep "^REPLAY " | tail -1 | cut -d' ' -f2- > "$REPLAY_COMMAND"
else
    printf 'ASX_E2E_SEED=%s ASX_E2E_PROFILE=%s ASX_E2E_CODEC=%s ASX_E2E_DETERMINISTIC=%s make test-e2e-actor-supervision\n' \
        "$E2E_SEED" "$E2E_PROFILE" "$E2E_CODEC" "$E2E_DETERMINISTIC" > "$REPLAY_COMMAND"
fi

RAW_DIGEST=""
if command -v sha256sum >/dev/null 2>&1; then
    RAW_DIGEST="$(sha256sum "$RAW_LOG" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    RAW_DIGEST="$(shasum -a 256 "$RAW_LOG" | awk '{print $1}')"
fi

if [ -s "$COMMAND_TRANSCRIPT" ] && [ -s "$RAW_LOG" ] && [ -s "$REPLAY_COMMAND" ]; then
    if [ -n "$RAW_DIGEST" ]; then
        e2e_scenario "actor_supervision.artifacts" "" "pass" "sha256:${RAW_DIGEST}"
    else
        e2e_scenario "actor_supervision.artifacts" "" "pass"
    fi
else
    e2e_scenario "actor_supervision.artifacts" "missing command/raw/replay artifact" "fail"
fi

set +e
e2e_finish
FINISH_RC=$?
set -e
exit $FINISH_RC
