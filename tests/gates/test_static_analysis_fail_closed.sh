#!/usr/bin/env bash
# test_static_analysis_fail_closed.sh — negative controls for the section 10.7
# static-analysis gate (tools/ci/run_static_analysis.sh; bridge bead W0.1b,
# bd-9kll.1.2).
#
# The gate used to report "clang-tidy: 0 finding(s)" while clang-tidy printed
# 15 warnings (it only read stderr; diagnostics go to stdout), skipped silently
# when an analyzer was missing, and exited with the finding count (which wraps
# to 0 at 256). Stub analyzers generated per run pin each of those down:
#   - cppcheck finding on stderr                    -> FAILS
#   - clang-tidy finding on stdout                  -> FAILS
#   - finding with an ASX_ANALYZER_WAIVER           -> waived, PASSES
#   - same finding under --strict                   -> FAILS
#   - 256 findings                                  -> FAILS (exit is not the count)
#   - analyzers missing, FAIL_ON_MISSING_LINTER=1   -> FAILS
#   - analyzers missing, FAIL_ON_MISSING_LINTER=0   -> SKIP, PASSES
#   - analyzer exits abnormally                     -> FAILS
#
# No real analyzer runs, so this takes well under a second. Each run writes
# under build/test-gates/<run_id>/ and logs JSONL to build/test-logs/gates.jsonl.
# Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUNNER="$REPO_ROOT/tools/ci/run_static_analysis.sh"
RUN_ID="static-analysis-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
mkdir -p "$WORK" "$LOG_DIR"

failures=0

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"static_analysis_fail_closed","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# Sources the stub findings point at: plain.c has no waiver, waived.c has one
# on the line before the flagged line 2.
printf 'int a;\nint b;\n' >"$WORK/plain.c"
printf '/* ASX_ANALYZER_WAIVER("negative control") */\nint b;\n' >"$WORK/waived.c"

# make_stub <name> <stream: stdout|stderr> <count> <line>: an analyzer stand-in
# that ignores its arguments and prints <line> <count> times on <stream>.
make_stub() {
    local name="$1" stream="$2" count="$3" line="$4"
    local redirect=""
    if [ "$stream" = "stderr" ]; then redirect=" >&2"; fi
    cat >"$WORK/$name" <<EOF
#!/usr/bin/env bash
i=0
while [ \$i -lt $count ]; do
    printf '%s\\n' '$line'$redirect
    i=\$((i + 1))
done
EOF
}

make_stub cppcheck_plain.sh stderr 1 "$WORK/plain.c:2:style:knownConditionTrueFalse:stub finding"
make_stub cppcheck_waived.sh stderr 1 "$WORK/waived.c:2:style:knownConditionTrueFalse:stub finding"
make_stub cppcheck_many.sh stderr 256 "$WORK/plain.c:2:style:knownConditionTrueFalse:stub finding"
make_stub tidy_stdout.sh stdout 1 "$WORK/plain.c:2:5: warning: stub finding [misc-stub]"

# run_case <name> <expected exit: 0|nonzero> <expected output regex> <env...> -- [runner args...]
run_case() {
    local name="$1" expect="$2" pattern="$3"
    shift 3
    local env_args=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do
        env_args+=("$1")
        shift
    done
    [ $# -gt 0 ] && shift

    local out rc observed status
    out="$(env "${env_args[@]}" bash "$RUNNER" "$@" 2>&1)"
    rc=$?
    printf '%s\n' "$out" >"$WORK/$name.log"

    if [ "$rc" -eq 0 ]; then observed="exit0"; else observed="exit$rc"; fi
    status="pass"
    if [ "$expect" = "0" ] && [ "$rc" -ne 0 ]; then status="fail"; fi
    if [ "$expect" = "nonzero" ] && [ "$rc" -eq 0 ]; then status="fail"; fi
    if ! grep -Eq -- "$pattern" <<<"$out"; then status="fail"; fi

    if [ "$status" = "pass" ]; then
        echo "  PASS: $name ($observed)"
    else
        echo "  FAIL: $name ($observed; expected $expect matching /$pattern/; log $WORK/$name.log)"
        failures=$((failures + 1))
    fi
    log_case "$name" "$expect:$pattern" "$observed" "$status" "$WORK/$name.log"
}

echo "=== test_static_analysis_fail_closed ($RUN_ID) ==="

run_case cppcheck_finding_fails nonzero 'FAIL — 1 unwaived finding' \
    CPPCHECK="bash $WORK/cppcheck_plain.sh" CLANG_TIDY= FAIL_ON_MISSING_LINTER=0 --
run_case clang_tidy_stdout_finding_fails nonzero 'clang-tidy: 1 finding' \
    CPPCHECK= CLANG_TIDY="bash $WORK/tidy_stdout.sh" FAIL_ON_MISSING_LINTER=0 --
run_case waived_finding_passes 0 'cppcheck: 0 finding\(s\), 1 waived' \
    CPPCHECK="bash $WORK/cppcheck_waived.sh" CLANG_TIDY= FAIL_ON_MISSING_LINTER=0 --
run_case strict_ignores_waivers nonzero 'FAIL — 1 unwaived finding' \
    CPPCHECK="bash $WORK/cppcheck_waived.sh" CLANG_TIDY= FAIL_ON_MISSING_LINTER=0 -- --strict
run_case findings_256_do_not_wrap_exit nonzero 'FAIL — 256 unwaived finding' \
    CPPCHECK="bash $WORK/cppcheck_many.sh" CLANG_TIDY= FAIL_ON_MISSING_LINTER=0 --
run_case missing_analyzers_fail_when_strict nonzero '2 missing analyzer' \
    CPPCHECK= CLANG_TIDY= FAIL_ON_MISSING_LINTER=1 --
run_case missing_analyzers_skip_when_lenient 0 'static-analysis: PASS' \
    CPPCHECK= CLANG_TIDY= FAIL_ON_MISSING_LINTER=0 --
run_case analyzer_crash_fails nonzero '1 analyzer error' \
    CPPCHECK=false CLANG_TIDY= FAIL_ON_MISSING_LINTER=0 --

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_static_analysis_fail_closed: PASS (8 cases)"
    exit 0
fi
echo "test_static_analysis_fail_closed: FAIL ($failures case(s))"
exit 1
