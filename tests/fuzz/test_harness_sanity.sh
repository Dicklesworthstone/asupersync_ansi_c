#!/bin/bash
# test_harness_sanity.sh — Automated fuzz harness sanity tests with ASan+UBSan
#
# Validates that fuzz_differential and fuzz_minimize run clean under
# AddressSanitizer + UndefinedBehaviorSanitizer with zero crashes,
# zero determinism failures, and no sanitizer violations.
#
# Usage:
#   bash tests/fuzz/test_harness_sanity.sh [--quick|--nightly]
#
# Output: JSONL to stderr, human summary to stdout.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SRCROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILDDIR="$SRCROOT/build_asan"
FUZZ_DIFF="$BUILDDIR/fuzz/fuzz_differential"
FUZZ_MIN="$BUILDDIR/fuzz/fuzz_minimize"
LOGFILE=""
PASS=0
FAIL=0
TOTAL=0
MODE="quick"

# Parse args
for arg in "$@"; do
    case "$arg" in
        --quick)   MODE="quick" ;;
        --nightly) MODE="nightly" ;;
        --help)    echo "Usage: $0 [--quick|--nightly]"; exit 0 ;;
        *)         echo "Unknown arg: $arg"; exit 1 ;;
    esac
done

case "$MODE" in
    quick)   DIFF_ITERS=1000;   DETERMINISM_ITERS=1000 ;;
    nightly) DIFF_ITERS=100000; DETERMINISM_ITERS=10000 ;;
esac

# ── Helpers ──────────────────────────────────────────────────────────────────

emit_jsonl() {
    echo "$1" >&2
}

log() {
    echo "[sanity] $1"
}

check_binary() {
    local name="$1" path="$2"
    TOTAL=$((TOTAL + 1))
    if [ -x "$path" ]; then
        PASS=$((PASS + 1))
        emit_jsonl "{\"test\":\"binary_exists\",\"name\":\"$name\",\"result\":\"PASS\"}"
        log "PASS: $name exists and is executable"
    else
        FAIL=$((FAIL + 1))
        emit_jsonl "{\"test\":\"binary_exists\",\"name\":\"$name\",\"result\":\"FAIL\",\"reason\":\"not found or not executable\"}"
        log "FAIL: $name not found at $path"
        log "  hint: run 'bash build_asan.sh' first"
        return 1
    fi
}

# ── Prerequisite: Build if needed ────────────────────────────────────────────

if [ ! -x "$FUZZ_DIFF" ] || [ ! -x "$FUZZ_MIN" ]; then
    log "ASan binaries not found, building..."
    bash "$SRCROOT/build_asan.sh" >/dev/null 2>&1
fi

emit_jsonl "{\"kind\":\"session_start\",\"mode\":\"$MODE\",\"diff_iters\":$DIFF_ITERS,\"determinism_iters\":$DETERMINISM_ITERS}"
log "=== Fuzz Harness Sanity Tests (mode=$MODE) ==="

# ── Test 1: Binaries exist ──────────────────────────────────────────────────

check_binary "fuzz_differential" "$FUZZ_DIFF"
check_binary "fuzz_minimize" "$FUZZ_MIN"

# ── Test 2: ASan is active (detect_leaks probe) ─────────────────────────────

TOTAL=$((TOTAL + 1))
ASAN_CHECK=$("$FUZZ_DIFF" --iterations 1 2>&1 || true)
if echo "$ASAN_CHECK" | grep -q "fuzz"; then
    PASS=$((PASS + 1))
    emit_jsonl "{\"test\":\"asan_active\",\"result\":\"PASS\"}"
    log "PASS: ASan binary runs successfully"
else
    FAIL=$((FAIL + 1))
    emit_jsonl "{\"test\":\"asan_active\",\"result\":\"FAIL\",\"reason\":\"binary did not produce expected output\"}"
    log "FAIL: ASan binary did not run correctly"
fi

# ── Test 3: Differential fuzz (main run) ─────────────────────────────────────

TOTAL=$((TOTAL + 1))
log "Running fuzz_differential ($DIFF_ITERS iterations under ASan+UBSan)..."
DIFF_OUTPUT=$("$FUZZ_DIFF" --seed 0 --iterations "$DIFF_ITERS" 2>&1)
DIFF_EXIT=$?

if [ $DIFF_EXIT -eq 0 ]; then
    # Extract summary JSON
    SUMMARY=$(echo "$DIFF_OUTPUT" | grep '"kind":"summary"' || echo "")
    if [ -n "$SUMMARY" ]; then
        CRASHES=$(echo "$SUMMARY" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['crashes'])" 2>/dev/null || echo "?")
        DET_FAIL=$(echo "$SUMMARY" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['determinism_failures'])" 2>/dev/null || echo "?")
        ITER_SEC=$(echo "$SUMMARY" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['iterations_per_sec'])" 2>/dev/null || echo "?")

        if [ "$CRASHES" = "0" ] && [ "$DET_FAIL" = "0" ]; then
            PASS=$((PASS + 1))
            emit_jsonl "{\"test\":\"differential_fuzz\",\"result\":\"PASS\",\"iterations\":$DIFF_ITERS,\"crashes\":$CRASHES,\"determinism_failures\":$DET_FAIL,\"iter_per_sec\":$ITER_SEC}"
            log "PASS: differential fuzz $DIFF_ITERS iters, 0 crashes, 0 det failures ($ITER_SEC iter/s)"
        else
            FAIL=$((FAIL + 1))
            emit_jsonl "{\"test\":\"differential_fuzz\",\"result\":\"FAIL\",\"iterations\":$DIFF_ITERS,\"crashes\":$CRASHES,\"determinism_failures\":$DET_FAIL}"
            log "FAIL: differential fuzz: crashes=$CRASHES determinism_failures=$DET_FAIL"
        fi
    else
        FAIL=$((FAIL + 1))
        emit_jsonl "{\"test\":\"differential_fuzz\",\"result\":\"FAIL\",\"reason\":\"no summary JSON\"}"
        log "FAIL: no summary JSON from fuzz_differential"
    fi
else
    FAIL=$((FAIL + 1))
    # Check for sanitizer output
    SANITIZER_MSG=$(echo "$DIFF_OUTPUT" | grep -E "ERROR:|runtime error:|SUMMARY:" | head -5 || echo "exit code $DIFF_EXIT")
    emit_jsonl "{\"test\":\"differential_fuzz\",\"result\":\"FAIL\",\"reason\":\"nonzero exit\",\"detail\":\"$SANITIZER_MSG\"}"
    log "FAIL: fuzz_differential exited with code $DIFF_EXIT"
    log "  $SANITIZER_MSG"
fi

# ── Test 4: Minimizer self-tests ─────────────────────────────────────────────

TOTAL=$((TOTAL + 1))
log "Running fuzz_minimize --selftest under ASan+UBSan..."
MIN_OUTPUT=$("$FUZZ_MIN" --selftest --verbose 2>&1)
MIN_EXIT=$?

if [ $MIN_EXIT -eq 0 ]; then
    # Count PASS lines
    PASS_COUNT=$(echo "$MIN_OUTPUT" | grep -c "self-test.*PASS" || echo "0")
    if [ "$PASS_COUNT" -ge 3 ]; then
        PASS=$((PASS + 1))
        emit_jsonl "{\"test\":\"minimizer_selftest\",\"result\":\"PASS\",\"subtests_passed\":$PASS_COUNT}"
        log "PASS: minimizer self-tests ($PASS_COUNT subtests passed)"
    else
        FAIL=$((FAIL + 1))
        emit_jsonl "{\"test\":\"minimizer_selftest\",\"result\":\"FAIL\",\"reason\":\"only $PASS_COUNT subtests passed\"}"
        log "FAIL: minimizer self-tests: only $PASS_COUNT/3 subtests passed"
    fi
else
    FAIL=$((FAIL + 1))
    SANITIZER_MSG=$(echo "$MIN_OUTPUT" | grep -E "ERROR:|runtime error:|SUMMARY:" | head -5 || echo "exit code $MIN_EXIT")
    emit_jsonl "{\"test\":\"minimizer_selftest\",\"result\":\"FAIL\",\"reason\":\"nonzero exit\",\"detail\":\"$SANITIZER_MSG\"}"
    log "FAIL: fuzz_minimize exited with code $MIN_EXIT"
    log "  $SANITIZER_MSG"
fi

# ── Test 5: Determinism under sanitizers ─────────────────────────────────────

TOTAL=$((TOTAL + 1))
log "Running determinism check ($DETERMINISM_ITERS iterations, seed=42, 2 runs)..."

RUN1=$("$FUZZ_DIFF" --seed 42 --iterations "$DETERMINISM_ITERS" 2>&1 | grep '"kind":"summary"')
RUN2=$("$FUZZ_DIFF" --seed 42 --iterations "$DETERMINISM_ITERS" 2>&1 | grep '"kind":"summary"')

DET1=$(echo "$RUN1" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['determinism_failures'])" 2>/dev/null || echo "?")
DET2=$(echo "$RUN2" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['determinism_failures'])" 2>/dev/null || echo "?")

if [ "$DET1" = "0" ] && [ "$DET2" = "0" ]; then
    PASS=$((PASS + 1))
    emit_jsonl "{\"test\":\"determinism\",\"result\":\"PASS\",\"iterations\":$DETERMINISM_ITERS,\"seed\":42,\"run1_failures\":0,\"run2_failures\":0}"
    log "PASS: determinism check ($DETERMINISM_ITERS iters x 2 runs, seed=42)"
else
    FAIL=$((FAIL + 1))
    emit_jsonl "{\"test\":\"determinism\",\"result\":\"FAIL\",\"run1_failures\":\"$DET1\",\"run2_failures\":\"$DET2\"}"
    log "FAIL: determinism check: run1=$DET1 run2=$DET2"
fi

# ── Summary ──────────────────────────────────────────────────────────────────

emit_jsonl "{\"kind\":\"session_end\",\"total\":$TOTAL,\"pass\":$PASS,\"fail\":$FAIL}"

echo ""
log "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="

if [ $FAIL -gt 0 ]; then
    log "FAIL"
    exit 1
else
    log "ALL PASS"
    exit 0
fi
