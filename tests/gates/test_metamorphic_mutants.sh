#!/usr/bin/env bash
# test_metamorphic_mutants.sh — the metamorphic relations must catch a
# planted order dependence (bd-9kll.12.1).
#
# tests/unit/conformance/test_metamorphic.c checks four relations on the C
# interpreter: M1 (permuting independent tasks keeps the projection), M2
# (adding a no-op task changes no other entity), M3 (nested region budgets a
# then b equal one region with meet(a, b)) and M4 (a weaker cancel request
# after a stronger one changes nothing). A relation test that never fails
# proves nothing, so this gate builds the same test against a runtime with
# one deliberate bug per relation, each compiled only here:
#
#   SPAWN_ORDINAL_CANCEL  the third task a run spawns cancels the first, so a
#                         task's outcome depends on how many tasks were
#                         spawned before it and in what order  (M1 and M2;
#                         src/runtime/event_log.c)
#   BUDGET_CHILD_WINS     a later deadline replaces the earlier in a budget
#                         meet instead of meeting it: a child region ignores
#                         its ancestors' deadlines  (M3; src/core/budget.c)
#   CANCEL_LAST_WINS      the later cancel request always replaces the
#                         current reason, however weak  (M4;
#                         src/core/cancel.c)
#
# and expects, for each mutant, a nonzero exit with at least one VIOLATION
# line of the relation it breaks, and, for the clean build of the same test,
# a zero exit with none, so the bug and not the corpus makes the relations
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

# --- each mutant is caught by the relation it breaks --------------------------
# run_mutant <macro> <relation>...: build the test with ASX_MUTANT_<macro>;
# it must exit nonzero with at least one <relation> VIOLATION line for each
# relation named, and the others are free (a mutant may break several).
run_mutant() {
    local macro="$1"
    shift
    local dir="$MUTANT_BUILD_ROOT/METAMORPHIC_$macro"
    local out rc status="pass" counts="" rel n
    if ! build_test "$dir" "-DASX_MUTANT_$macro"; then
        echo "  FAIL: $macro (the mutant does not build; log $dir/build.log)"
        log_case "$macro" "build" "build-failed" "fail" "$dir/build.log"
        failures=$((failures + 1))
        return
    fi
    out="$(cd "$REPO_ROOT" && "$dir/$TEST_REL" 2>&1)"
    rc=$?
    printf '%s\n' "$out" >"$dir/run.log"
    if [ "$rc" -eq 0 ]; then status="fail"; fi
    for rel in "$@"; do
        n="$(grep -cE " $rel VIOLATION " <<<"$out")"
        counts="$counts $rel=$n"
        if [ "$n" -eq 0 ]; then status="fail"; fi
    done
    if [ "$status" = "pass" ]; then
        echo "  PASS: $macro caught (${counts# } violation line(s))"
    else
        echo "  FAIL: $macro not caught by $* (exit$rc;$counts; log $dir/run.log)"
        failures=$((failures + 1))
    fi
    log_case "$macro" "nonzero $*" "exit$rc$counts" "$status" "$dir/run.log"
}

# The third spawned task cancels the first: a task's outcome depends on how
# many tasks came before it and in what order (M1 and M2).
run_mutant SPAWN_ORDINAL_CANCEL M1 M2
# A later deadline replaces the earlier in a budget meet instead of meeting
# it: a child region's budget ignores its ancestors' (M3).
run_mutant BUDGET_CHILD_WINS M3
# The later cancel request always replaces the current reason: a weaker
# request after a stronger one weakens it (M4).
run_mutant CANCEL_LAST_WINS M4

if [ "$failures" -eq 0 ]; then
    echo "test_metamorphic_mutants: all cases passed"
    exit 0
fi
echo "test_metamorphic_mutants: $failures case(s) failed"
exit 1
