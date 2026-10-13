#!/usr/bin/env bash
# test_conformance_monitors.sh — the monitors a Rust-parity compare cannot
# see must see planted bugs (bd-9kll.2.21, bd-9kll.2.22).
#
# A parity compare judges what a run produced (trace, snapshot, observations,
# dispatch order), so a runtime that breaks its own protocol or its own
# memory without changing any of that passes it. Two lanes look past it, and
# each case here plants a bug only its lane can see, behind an #ifdef
# compiled only here, and expects:
#   - the plain runner (`conformance`) to PASS the whole corpus: the mutant is
#     invisible to parity, so the lane is not vacuously catching something
#     parity already caught;
#   - the lane to FAIL, with its own evidence in the output.
#
#   GHOST_ILLEGAL_TRANSITION  logging a task spawn also tells the ghost
#                             protocol monitor the task went Completed ->
#                             Created
#                             -> `conformance-ghost` (ghost-checked runner)
#                             FAILs scenarios naming that violation first.
#   SANITIZER_OOB_READ        every logged event reads one byte past a
#                             static table
#                             -> `conformance-asan` reports a
#                             global-buffer-overflow and exits nonzero.
#
# Builds under build/mutants/<case>-<lane>/ (the normal build is untouched)
# and logs JSONL to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="monitors-$(date -u +%Y%m%dT%H%M%SZ)-$$"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
JOBS="${JOBS:-4}"
MUTANT_BUILD_ROOT="${ASX_MUTANT_BUILD_DIR:-build/mutants}"
if [[ "$MUTANT_BUILD_ROOT" != /* ]]; then
    MUTANT_BUILD_ROOT="$REPO_ROOT/$MUTANT_BUILD_ROOT"
fi
mkdir -p "$LOG_DIR"

failures=0

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"conformance_monitors","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# build_runner <dir> <extra cflags>: the conformance runner of a library built
# with <extra cflags>, in its own BUILD_DIR.
build_runner() {
    local dir="$1" cflags="$2"
    mkdir -p "$dir"
    make -C "$REPO_ROOT" -j"$JOBS" --no-print-directory BUILD_DIR="$dir" \
        CFLAGS="${CFLAGS:-} $cflags" "$dir/bin/asx-conformance" >"$dir/build.log" 2>&1
}

# plain_passes <case> <dir>: the plain runner with the mutant passes the whole
# corpus, so the planted bug is one parity does not see. Returns 0 when so.
plain_passes() {
    local name="$1" dir="$2" out rc
    out="$(cd "$REPO_ROOT" && "$dir/bin/asx-conformance" compare fixtures/rust_reference_v2/*.json 2>&1)"
    rc=$?
    printf '%s\n' "$out" >"$dir/compare.log"
    if [ "$rc" -eq 0 ] && ! grep -Eq '^(FAIL|ERROR) ' <<<"$out"; then
        echo "  PASS: $name is invisible to the parity compare (plain runner passes the corpus)"
        log_case "$name" "plain-runner-passes" "exit$rc" "pass" "$dir/compare.log"
        return 0
    fi
    echo "  FAIL: $name changes what parity sees; it must not (exit$rc; log $dir/compare.log)"
    log_case "$name" "plain-runner-passes" "exit$rc" "fail" "$dir/compare.log"
    return 1
}

echo "=== test_conformance_monitors ($RUN_ID) ==="

# --- GHOST_ILLEGAL_TRANSITION -------------------------------------------------
G_PLAIN="$MUTANT_BUILD_ROOT/GHOST_ILLEGAL_TRANSITION-plain"
G_GHOST="$MUTANT_BUILD_ROOT/GHOST_ILLEGAL_TRANSITION-ghost"
if ! build_runner "$G_PLAIN" "-DASX_MUTANT_GHOST_ILLEGAL_TRANSITION"; then
    echo "  FAIL: GHOST_ILLEGAL_TRANSITION plain runner does not build (log $G_PLAIN/build.log)"
    log_case "GHOST_ILLEGAL_TRANSITION" "build" "build-failed" "fail" "$G_PLAIN/build.log"
    failures=$((failures + 1))
elif ! plain_passes "GHOST_ILLEGAL_TRANSITION" "$G_PLAIN"; then
    failures=$((failures + 1))
fi

# The same command line `make conformance-ghost` uses (-DASX_DEBUG_GHOST=1
# -DASX_CONFORMANCE_GHOST_CHECK=1), plus the mutant.
if ! build_runner "$G_GHOST" \
    "-DASX_DEBUG_GHOST=1 -DASX_CONFORMANCE_GHOST_CHECK=1 -DASX_MUTANT_GHOST_ILLEGAL_TRANSITION"; then
    echo "  FAIL: GHOST_ILLEGAL_TRANSITION ghost-checked runner does not build (log $G_GHOST/build.log)"
    log_case "GHOST_ILLEGAL_TRANSITION" "build" "build-failed" "fail" "$G_GHOST/build.log"
    failures=$((failures + 1))
else
    g_out="$(cd "$REPO_ROOT" && "$G_GHOST/bin/asx-conformance" compare fixtures/rust_reference_v2/*.json 2>&1)"
    g_rc=$?
    printf '%s\n' "$g_out" >"$G_GHOST/compare.log"
    g_fails="$(grep -cE '^FAIL ' <<<"$g_out")"
    # A scenario's C run and its replay of Rust's schedule each report.
    g_ghost="$(grep -cE '^FAIL [^ ]+: ghost monitor recorded [0-9]+ violation\(s\) in [^;]+; first: protocol_task entity=0x[0-9a-f]+ Completed -> Created ' <<<"$g_out")"
    g_status="pass"
    # Nonzero, every FAIL line the planted violation and nothing else (the
    # mutant changes no result), and the first violation named in full.
    if [ "$g_rc" -eq 0 ] || [ "$g_ghost" -eq 0 ] || [ "$g_ghost" -ne "$g_fails" ]; then
        g_status="fail"
    fi
    if [ "$g_status" = "pass" ]; then
        echo "  PASS: GHOST_ILLEGAL_TRANSITION caught by the ghost-checked runner ($g_ghost scenario(s) fail, each naming the first violation)"
    else
        echo "  FAIL: GHOST_ILLEGAL_TRANSITION not caught as expected (exit$g_rc; fail=$g_fails ghost=$g_ghost; log $G_GHOST/compare.log)"
        failures=$((failures + 1))
    fi
    log_case "GHOST_ILLEGAL_TRANSITION" "ghost-runner-fails" "exit$g_rc fail=$g_fails ghost=$g_ghost" "$g_status" "$G_GHOST/compare.log"
fi

# --- SANITIZER_OOB_READ -------------------------------------------------------
S_PLAIN="$MUTANT_BUILD_ROOT/SANITIZER_OOB_READ-plain"
S_ASAN="$MUTANT_BUILD_ROOT/SANITIZER_OOB_READ-asan"
if ! build_runner "$S_PLAIN" "-DASX_MUTANT_SANITIZER_OOB_READ"; then
    echo "  FAIL: SANITIZER_OOB_READ plain runner does not build (log $S_PLAIN/build.log)"
    log_case "SANITIZER_OOB_READ" "build" "build-failed" "fail" "$S_PLAIN/build.log"
    failures=$((failures + 1))
elif ! plain_passes "SANITIZER_OOB_READ" "$S_PLAIN"; then
    failures=$((failures + 1))
fi

# The real lane, so its flags and halt-on-error settings are the ones under
# test: a build that fails here is a failure of the lane, not a skipped case.
s_out="$(cd "$REPO_ROOT" && make -j"$JOBS" --no-print-directory conformance-asan \
    CONFORMANCE_ASAN_DIR="$S_ASAN" CFLAGS="${CFLAGS:-} -DASX_MUTANT_SANITIZER_OOB_READ" 2>&1)"
s_rc=$?
mkdir -p "$S_ASAN"
printf '%s\n' "$s_out" >"$S_ASAN/conformance-asan.log"
s_status="pass"
if [ "$s_rc" -eq 0 ] || ! grep -q 'AddressSanitizer: global-buffer-overflow' <<<"$s_out"; then
    s_status="fail"
fi
if [ "$s_status" = "pass" ]; then
    echo "  PASS: SANITIZER_OOB_READ caught by conformance-asan (global-buffer-overflow reported)"
else
    echo "  FAIL: SANITIZER_OOB_READ not caught as expected (exit$s_rc; log $S_ASAN/conformance-asan.log)"
    failures=$((failures + 1))
fi
log_case "SANITIZER_OOB_READ" "asan-lane-fails" "exit$s_rc" "$s_status" "$S_ASAN/conformance-asan.log"

if [ "$failures" -eq 0 ]; then
    echo "test_conformance_monitors: every planted bug caught by its lane and invisible to parity"
    exit 0
fi
echo "test_conformance_monitors: $failures check(s) failed"
exit 1
