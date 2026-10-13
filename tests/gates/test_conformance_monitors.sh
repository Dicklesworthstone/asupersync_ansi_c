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
#   GHOST_UNEXPECTED_LEAK     logging a reservation asks the linearity monitor
#                             about the obligation's region at once, so every
#                             reserved obligation is reported as a leak
#                             -> the ghost lane accepts the leaks Rust's trace
#                             expects (obligation.leaked) and FAILs the rest,
#                             naming the obligation. Its clean build must
#                             also pass the fixtures that leak on purpose and
#                             the one whose obligation resolves after its
#                             sibling regions finalize (a scoped scan).
#   SANITIZER_OOB_READ        every logged event reads one byte past a
#                             static table
#                             -> `conformance-asan` reports a
#                             global-buffer-overflow and exits nonzero.
#   ORACLE_LIVE_COUNT_DRIFT   logging a spawn also counts one more live task
#                             than there is (a counter only the resource
#                             gauge reads)
#                             -> `make conformance`, whose compare pass runs
#                             the kernel-state oracles (CONFORMANCE_ORACLES),
#                             fails naming arena_counters; the same lane with
#                             CONFORMANCE_ORACLES empty passes it.
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

# --- GHOST_UNEXPECTED_LEAK ----------------------------------------------------
# The ghost lane accepts a linearity_leak only for an obligation whose Rust
# trace has obligation.leaked. The corpus leaks on purpose in these two; the
# mutant reports every reserved obligation as a leak, so the lane must accept
# those and FAIL every obligation Rust commits or aborts.
LEAK_FIXTURES=(
    fixtures/rust_reference_v2/lab-dispatch-waker-rearm-001.json
    fixtures/rust_reference_v2/obligation-drop-leaks-at-once-001.json
)
# A scoped scan must not report an obligation another region still holds: this
# fixture's child resolves its obligation after its siblings' regions finalize.
SCOPE_FIXTURE=fixtures/rust_reference_v2/supervision-child-holds-obligation-001.json

L_PLAIN="$MUTANT_BUILD_ROOT/GHOST_UNEXPECTED_LEAK-plain"
L_GHOST="$MUTANT_BUILD_ROOT/GHOST_UNEXPECTED_LEAK-ghost"
L_CLEAN="$MUTANT_BUILD_ROOT/GHOST_CLEAN-ghost"

# The clean ghost-checked runner: the leaks the corpus expects and the
# scoped scan must not be reported.
if ! build_runner "$L_CLEAN" "-DASX_DEBUG_GHOST=1 -DASX_CONFORMANCE_GHOST_CHECK=1"; then
    echo "  FAIL: GHOST_CLEAN ghost-checked runner does not build (log $L_CLEAN/build.log)"
    log_case "GHOST_CLEAN" "build" "build-failed" "fail" "$L_CLEAN/build.log"
    failures=$((failures + 1))
else
    c_out="$(cd "$REPO_ROOT" && "$L_CLEAN/bin/asx-conformance" compare "${LEAK_FIXTURES[@]}" "$SCOPE_FIXTURE" 2>&1)"
    c_rc=$?
    printf '%s\n' "$c_out" >"$L_CLEAN/compare.log"
    if [ "$c_rc" -eq 0 ] && ! grep -Eq '^(FAIL|ERROR) ' <<<"$c_out"; then
        echo "  PASS: the clean ghost-checked runner passes the fixtures that leak on purpose and the scoped-scan fixture"
        log_case "GHOST_CLEAN" "clean-ghost-runner-passes" "exit$c_rc" "pass" "$L_CLEAN/compare.log"
    else
        echo "  FAIL: the clean ghost-checked runner rejects a fixture it must accept (exit$c_rc; log $L_CLEAN/compare.log)"
        log_case "GHOST_CLEAN" "clean-ghost-runner-passes" "exit$c_rc" "fail" "$L_CLEAN/compare.log"
        failures=$((failures + 1))
    fi
fi

if ! build_runner "$L_PLAIN" "-DASX_MUTANT_GHOST_UNEXPECTED_LEAK"; then
    echo "  FAIL: GHOST_UNEXPECTED_LEAK plain runner does not build (log $L_PLAIN/build.log)"
    log_case "GHOST_UNEXPECTED_LEAK" "build" "build-failed" "fail" "$L_PLAIN/build.log"
    failures=$((failures + 1))
elif ! plain_passes "GHOST_UNEXPECTED_LEAK" "$L_PLAIN"; then
    failures=$((failures + 1))
fi

if ! build_runner "$L_GHOST" \
    "-DASX_DEBUG_GHOST=1 -DASX_CONFORMANCE_GHOST_CHECK=1 -DASX_MUTANT_GHOST_UNEXPECTED_LEAK"; then
    echo "  FAIL: GHOST_UNEXPECTED_LEAK ghost-checked runner does not build (log $L_GHOST/build.log)"
    log_case "GHOST_UNEXPECTED_LEAK" "build" "build-failed" "fail" "$L_GHOST/build.log"
    failures=$((failures + 1))
else
    l_out="$(cd "$REPO_ROOT" && "$L_GHOST/bin/asx-conformance" compare fixtures/rust_reference_v2/*.json 2>&1)"
    l_rc=$?
    printf '%s\n' "$l_out" >"$L_GHOST/compare.log"
    l_fails="$(grep -cE '^FAIL ' <<<"$l_out")"
    l_leaks="$(grep -cE '^FAIL [^ ]+: ghost monitor recorded [0-9]+ violation\(s\) in [^;]+; first: linearity_leak entity=0x[0-9a-f]+ obligation=[^ ]+ ' <<<"$l_out")"
    l_status="pass"
    # Nonzero; every FAIL line an unexpected leak and nothing else; the leaks
    # the corpus expects accepted (named in a NOTE), never failed.
    if [ "$l_rc" -eq 0 ] || [ "$l_leaks" -eq 0 ] || [ "$l_leaks" -ne "$l_fails" ]; then l_status="fail"; fi
    for f in "${LEAK_FIXTURES[@]}"; do
        id="$(basename "$f" .json)"
        if grep -Eq "^FAIL $id:" <<<"$l_out"; then l_status="fail"; fi
        if ! grep -Eq "^NOTE $id: ghost monitor accepted [0-9]+ linearity_leak" <<<"$l_out"; then l_status="fail"; fi
    done
    if [ "$l_status" = "pass" ]; then
        echo "  PASS: GHOST_UNEXPECTED_LEAK caught ($l_leaks scenario run(s) fail on a leak Rust does not report; the leaks it expects are accepted)"
    else
        echo "  FAIL: GHOST_UNEXPECTED_LEAK not caught as expected (exit$l_rc; fail=$l_fails leak=$l_leaks; log $L_GHOST/compare.log)"
        failures=$((failures + 1))
    fi
    log_case "GHOST_UNEXPECTED_LEAK" "ghost-runner-fails-on-unexpected-leak" "exit$l_rc fail=$l_fails leak=$l_leaks" "$l_status" "$L_GHOST/compare.log"
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

# --- ORACLE_LIVE_COUNT_DRIFT --------------------------------------------------
# The real lane twice, on one build directory (so the second run reuses the
# library): with the oracles it ships with, it must FAIL naming the oracle;
# with CONFORMANCE_ORACLES empty, the same runner must PASS the corpus, which
# is what makes the first failure the oracles' and not parity's.
O_DIR="$MUTANT_BUILD_ROOT/ORACLE_LIVE_COUNT_DRIFT-lane"
mkdir -p "$O_DIR"
o_out="$(cd "$REPO_ROOT" && make -j"$JOBS" --no-print-directory conformance \
    BUILD_DIR="$O_DIR" CFLAGS="${CFLAGS:-} -DASX_MUTANT_ORACLE_LIVE_COUNT_DRIFT" 2>&1)"
o_rc=$?
printf '%s\n' "$o_out" >"$O_DIR/conformance-oracles.log"
o_named="$(grep -cE 'oracle failed: arena_counters, [0-9]+ violation' <<<"$o_out")"
o_status="pass"
if [ "$o_rc" -eq 0 ] || [ "$o_named" -eq 0 ]; then o_status="fail"; fi
if [ "$o_status" = "pass" ]; then
    echo "  PASS: ORACLE_LIVE_COUNT_DRIFT caught by make conformance ($o_named fixture(s) fail, each naming arena_counters)"
else
    echo "  FAIL: ORACLE_LIVE_COUNT_DRIFT not caught as expected (exit$o_rc; named=$o_named; log $O_DIR/conformance-oracles.log)"
    failures=$((failures + 1))
fi
log_case "ORACLE_LIVE_COUNT_DRIFT" "lane-fails-naming-the-oracle" "exit$o_rc named=$o_named" "$o_status" "$O_DIR/conformance-oracles.log"

o2_out="$(cd "$REPO_ROOT" && make -j"$JOBS" --no-print-directory conformance \
    BUILD_DIR="$O_DIR" CFLAGS="${CFLAGS:-} -DASX_MUTANT_ORACLE_LIVE_COUNT_DRIFT" \
    CONFORMANCE_ORACLES= 2>&1)"
o2_rc=$?
printf '%s\n' "$o2_out" >"$O_DIR/conformance-no-oracles.log"
if [ "$o2_rc" -eq 0 ] && ! grep -Eq '^(FAIL|ERROR) ' <<<"$o2_out"; then
    echo "  PASS: ORACLE_LIVE_COUNT_DRIFT is invisible to the lane without its oracles (parity passes)"
    log_case "ORACLE_LIVE_COUNT_DRIFT" "lane-without-oracles-passes" "exit$o2_rc" "pass" "$O_DIR/conformance-no-oracles.log"
else
    echo "  FAIL: ORACLE_LIVE_COUNT_DRIFT changes what parity sees; it must not (exit$o2_rc; log $O_DIR/conformance-no-oracles.log)"
    log_case "ORACLE_LIVE_COUNT_DRIFT" "lane-without-oracles-passes" "exit$o2_rc" "fail" "$O_DIR/conformance-no-oracles.log"
    failures=$((failures + 1))
fi

if [ "$failures" -eq 0 ]; then
    echo "test_conformance_monitors: every planted bug caught by its lane and invisible to parity"
    exit 0
fi
echo "test_conformance_monitors: $failures check(s) failed"
exit 1
