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
    "${E2E_PROJECT_ROOT}/src/app/app.c" \
    "${E2E_PROJECT_ROOT}/src/app/doctor.c" \
    "${E2E_PROJECT_ROOT}/src/app/report.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/rt.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/lifecycle.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/scheduler.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/cancellation.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/quiescence.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/resource.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/trace.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/telemetry.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/hindsight.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/event_log.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/parallel.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/profile_compat.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/adapter.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/vertical_adapter.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/virtual_time.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/config_reload.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/regression_localize.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/snapshot.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/overload_catalog.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/hft_instrument.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/automotive_instrument.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/diagnostic.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/io_driver.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/waker.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/blocking.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/hooks.c" \
    "${E2E_PROJECT_ROOT}/src/runtime/equivalence.c" \
    "${E2E_PROJECT_ROOT}/src/time/timer_wheel.c" \
    "${E2E_PROJECT_ROOT}/src/channel/mpsc.c" \
    "${E2E_PROJECT_ROOT}/src/channel/oneshot.c" \
    "${E2E_PROJECT_ROOT}/src/channel/watch.c" \
    "${E2E_PROJECT_ROOT}/src/channel/broadcast.c" \
    "${E2E_PROJECT_ROOT}/src/channel/session.c" \
    "${E2E_PROJECT_ROOT}/src/sync/notify.c" \
    "${E2E_PROJECT_ROOT}/src/sync/semaphore.c" \
    "${E2E_PROJECT_ROOT}/src/sync/barrier.c" \
    "${E2E_PROJECT_ROOT}/src/sync/once.c" \
    "${E2E_PROJECT_ROOT}/src/actor/actor.c" \
    "${E2E_PROJECT_ROOT}/src/actor/supervisor.c" \
    "${E2E_PROJECT_ROOT}/src/cx/cx.c" \
    "${E2E_PROJECT_ROOT}/src/cx/scope.c" \
    "${E2E_PROJECT_ROOT}/src/net/net.c" \
    "${E2E_PROJECT_ROOT}/src/fs/fs.c" \
    "${E2E_PROJECT_ROOT}/src/process/process.c" \
    "${E2E_PROJECT_ROOT}/src/signal/signal.c" \
    "${E2E_PROJECT_ROOT}/src/bytes/buf.c" \
    "${E2E_PROJECT_ROOT}/src/core/status.c" \
    "${E2E_PROJECT_ROOT}/src/core/transition_tables.c" \
    "${E2E_PROJECT_ROOT}/src/core/outcome.c" \
    "${E2E_PROJECT_ROOT}/src/core/budget.c" \
    "${E2E_PROJECT_ROOT}/src/core/cancel.c" \
    "${E2E_PROJECT_ROOT}/src/core/cleanup.c" \
    "${E2E_PROJECT_ROOT}/src/core/ghost.c" \
    "${E2E_PROJECT_ROOT}/src/core/affinity.c" \
    "${E2E_PROJECT_ROOT}/src/core/adaptive.c" \
    "${E2E_PROJECT_ROOT}/src/core/abi.c" \
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
