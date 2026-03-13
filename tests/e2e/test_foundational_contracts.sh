#!/bin/bash
# test_foundational_contracts.sh — e2e evidence script for foundational type
# contracts (bd-1eqo.15.1)
#
# Builds and runs all foundational type parity tests:
#   1. Existing algebraic tests (outcome lattice, budget lattice, cancel monotone)
#   2. New foundational parity test (45 tests)
#   3. Existing unit tests (outcome, budget, cancel, ids, transition, status)
#
# Emits JSONL evidence to stderr, human summary to stdout.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SRCROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILDDIR="$SRCROOT/build"
GCC=/usr/bin/gcc
PASS=0
FAIL=0
TOTAL=0

COMMON_FLAGS="-std=c99 -Wall -Wextra -Wpedantic -Wno-unused-parameter -O0 -g -DASX_DEBUG=1"
INCLUDES="-I$SRCROOT/include -I$SRCROOT/tests -I$SRCROOT/tests/unit/core -DASX_PROFILE_CORE -DASX_CODEC_JSON"

emit_jsonl() {
    echo "$1" >&2
}

log() {
    echo "[e2e] $1"
}

# Build libasx.a if not present
if [ ! -f "$BUILDDIR/lib/libasx.a" ]; then
    log "Building libasx.a..."
    make -C "$SRCROOT" CC=$GCC lib >/dev/null 2>&1
fi

emit_jsonl "{\"kind\":\"session_start\",\"suite\":\"foundational_contracts\",\"bead\":\"bd-1eqo.15.1\"}"
log "=== Foundational Type Contract Evidence (bd-1eqo.15.1) ==="

# ── Test suites to run ─────────────────────────────────────────────

declare -a SUITES=(
    # Formal algebraic tests
    "tests/formal/algebraic/test_outcome_lattice.c:outcome_lattice"
    "tests/formal/algebraic/test_budget_lattice.c:budget_lattice"
    "tests/formal/algebraic/test_cancel_monotone.c:cancel_monotone"
    "tests/formal/algebraic/test_foundational_parity.c:foundational_parity"
    "tests/formal/algebraic/test_resource_ownership_parity.c:resource_ownership_parity"
    # Core unit tests
    "tests/unit/core/test_outcome.c:outcome_unit"
    "tests/unit/core/test_budget.c:budget_unit"
    "tests/unit/core/test_cancel.c:cancel_unit"
    "tests/unit/core/test_ids.c:ids_unit"
    "tests/unit/core/test_transition.c:transition_unit"
    "tests/unit/core/test_status.c:status_unit"
)

for entry in "${SUITES[@]}"; do
    SRC="${entry%%:*}"
    NAME="${entry##*:}"
    BIN="$BUILDDIR/e2e_$NAME"
    TOTAL=$((TOTAL + 1))

    log "Building $NAME..."
    if ! $GCC $COMMON_FLAGS $INCLUDES -o "$BIN" "$SRCROOT/$SRC" "$BUILDDIR/lib/libasx.a" 2>/dev/null; then
        FAIL=$((FAIL + 1))
        emit_jsonl "{\"test\":\"$NAME\",\"result\":\"FAIL\",\"reason\":\"compile error\"}"
        log "FAIL: $NAME (compile error)"
        continue
    fi

    OUTPUT=$("$BIN" 2>&1)
    EXIT=$?

    # Extract pass/total counts from output
    COUNTS=$(echo "$OUTPUT" | grep -oE '[0-9]+/[0-9]+ tests passed' | head -1 || echo "")

    if [ $EXIT -eq 0 ]; then
        PASS=$((PASS + 1))
        emit_jsonl "{\"test\":\"$NAME\",\"result\":\"PASS\",\"detail\":\"$COUNTS\"}"
        log "PASS: $NAME ($COUNTS)"
    else
        FAIL=$((FAIL + 1))
        FAILURES=$(echo "$OUTPUT" | grep "FAIL:" | head -3 || echo "exit=$EXIT")
        emit_jsonl "{\"test\":\"$NAME\",\"result\":\"FAIL\",\"detail\":\"$FAILURES\"}"
        log "FAIL: $NAME"
        echo "$OUTPUT" | grep "FAIL:" | head -5
    fi
done

# ── Summary ──────────────────────────────────────────────────────────

emit_jsonl "{\"kind\":\"session_end\",\"total\":$TOTAL,\"pass\":$PASS,\"fail\":$FAIL}"

echo ""
log "=== Results: $PASS/$TOTAL suites passed, $FAIL failed ==="

if [ $FAIL -gt 0 ]; then
    log "FAIL"
    exit 1
else
    log "ALL PASS — foundational type contracts verified"
    exit 0
fi
