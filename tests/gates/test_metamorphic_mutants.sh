#!/usr/bin/env bash
# test_metamorphic_mutants.sh — the metamorphic relations must catch a
# planted order dependence (bd-9kll.12.1).
#
# tests/unit/conformance/test_metamorphic.c checks M1 (permuting independent
# tasks keeps the projection) and M2 (adding a no-op task changes no other
# entity) on the C interpreter. A relation test that never fails proves
# nothing, so this gate builds the same test against a runtime with one
# deliberate bug, compiled only here (src/runtime/event_log.c):
#
#   SPAWN_ORDINAL_CANCEL   the third task a run spawns cancels the first, so
#                          a task's outcome depends on how many tasks were
#                          spawned before it and in what order
#
# and expects, for the mutant, a nonzero exit with at least one M1 VIOLATION
# and one M2 VIOLATION line, and, for the clean build of the same test, a
# zero exit with none, so the bug and not the corpus makes the relations
# fail.
#
# Builds under build/mutants/METAMORPHIC_*/ (the normal build is untouched)
# and logs JSONL to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="metamorphic-$(date -u +%Y%m%dT%H%M%SZ)-$$"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
JOBS="${JOBS:-4}"
MUTANT_BUILD_ROOT="${ASX_MUTANT_BUILD_DIR:-build/mutants}"
if [[ "$MUTANT_BUILD_ROOT" != /* ]]; then
    MUTANT_BUILD_ROOT="$REPO_ROOT/$MUTANT_BUILD_ROOT"
fi
TEST_REL="tests/unit/conformance/test_metamorphic"
mkdir -p "$LOG_DIR"

failures=0

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"metamorphic_mutants","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# build_test <dir> <extra cflags>: the metamorphic test of a library built
# with <extra cflags>, in its own BUILD_DIR. Returns the make status.
build_test() {
    local dir="$1" cflags="$2"
    mkdir -p "$dir"
    make -C "$REPO_ROOT" -j"$JOBS" --no-print-directory BUILD_DIR="$dir" \
        CFLAGS="${CFLAGS:-} $cflags" "$dir/$TEST_REL" >"$dir/build.log" 2>&1
}

echo "=== test_metamorphic_mutants ($RUN_ID) ==="

# --- the clean build passes ---------------------------------------------------
CLEAN_DIR="$MUTANT_BUILD_ROOT/METAMORPHIC_clean"
if ! build_test "$CLEAN_DIR" ""; then
    echo "  FAIL: the clean metamorphic test does not build (log $CLEAN_DIR/build.log)"
    log_case "clean" "build" "build-failed" "fail" "$CLEAN_DIR/build.log"
    failures=$((failures + 1))
else
    c_out="$(cd "$REPO_ROOT" && "$CLEAN_DIR/$TEST_REL" 2>&1)"
    c_rc=$?
    printf '%s\n' "$c_out" >"$CLEAN_DIR/run.log"
    c_violations="$(grep -cE ' VIOLATION ' <<<"$c_out")"
    if [ "$c_rc" -eq 0 ] && [ "$c_violations" -eq 0 ]; then
        echo "  PASS: the clean runtime satisfies M1 and M2 over the corpus"
        log_case "clean" "exit0 no-violations" "exit$c_rc" "pass" "$CLEAN_DIR/run.log"
    else
        echo "  FAIL: the clean runtime does not satisfy the relations (exit$c_rc; $c_violations violation(s); log $CLEAN_DIR/run.log)"
        log_case "clean" "exit0 no-violations" "exit$c_rc violations=$c_violations" "fail" "$CLEAN_DIR/run.log"
        failures=$((failures + 1))
    fi
fi

# --- the mutant is caught by both relations ------------------------------------
MACRO=SPAWN_ORDINAL_CANCEL
M_DIR="$MUTANT_BUILD_ROOT/METAMORPHIC_$MACRO"
if ! build_test "$M_DIR" "-DASX_MUTANT_$MACRO"; then
    echo "  FAIL: $MACRO (the mutant does not build; log $M_DIR/build.log)"
    log_case "$MACRO" "build" "build-failed" "fail" "$M_DIR/build.log"
    failures=$((failures + 1))
else
    m_out="$(cd "$REPO_ROOT" && "$M_DIR/$TEST_REL" 2>&1)"
    m_rc=$?
    printf '%s\n' "$m_out" >"$M_DIR/run.log"
    m1="$(grep -cE ' M1 VIOLATION ' <<<"$m_out")"
    m2="$(grep -cE ' M2 VIOLATION ' <<<"$m_out")"
    status="pass"
    if [ "$m_rc" -eq 0 ] || [ "$m1" -eq 0 ] || [ "$m2" -eq 0 ]; then status="fail"; fi
    if [ "$status" = "pass" ]; then
        echo "  PASS: $MACRO caught (M1: $m1 violation(s), M2: $m2 violation(s))"
    else
        echo "  FAIL: $MACRO not caught by both relations (exit$m_rc; M1 $m1, M2 $m2; log $M_DIR/run.log)"
        failures=$((failures + 1))
    fi
    log_case "$MACRO" "nonzero M1>0 M2>0" "exit$m_rc M1=$m1 M2=$m2" "$status" "$M_DIR/run.log"
fi

if [ "$failures" -eq 0 ]; then
    echo "test_metamorphic_mutants: all cases passed"
    exit 0
fi
echo "test_metamorphic_mutants: $failures case(s) failed"
exit 1
