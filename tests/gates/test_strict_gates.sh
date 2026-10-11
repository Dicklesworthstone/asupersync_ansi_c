#!/usr/bin/env bash
# test_strict_gates.sh — negative controls for STRICT_GATES (bridge bead W0.2,
# bd-9kll.1.3).
#
# Every agent "passed" format-check while CI failed it on every push, because
# a missing clang-format printed SKIP and exited 0. These controls pin the
# strict behaviour down:
#   - format-check, formatter unresolved, strict     -> FAILS
#   - format-check, formatter unresolved, lenient    -> SKIP, exit 0
#   - lint, cppcheck unresolved, strict              -> FAILS
#   - lint, cppcheck unresolved, lenient             -> SKIP, exit 0
#   - format-check over a misformatted scratch file  -> FAILS
#   - format-check over a well-formatted scratch file -> PASSES
# The last two need the pinned clang-format (system 18.1.8, or uvx); without
# it they fail under STRICT_GATES=1 / CI and are reported as SKIP otherwise.
#
# Scratch files live under build/test-gates/<run_id>/; tracked sources are
# never touched. Logs JSONL to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="strict-gates-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
mkdir -p "$WORK/bad" "$WORK/good" "$LOG_DIR"

STRICT="${STRICT_GATES:-${CI:-0}}"
failures=0
skipped=0

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"strict_gates","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# run_case <name> <expected exit: 0|nonzero> <expected output regex> <make args...>
run_case() {
    local name="$1" expect="$2" pattern="$3"
    shift 3
    local out rc observed status
    out="$(make --no-print-directory -C "$REPO_ROOT" "$@" 2>&1)"
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

skip_case() {
    echo "  SKIP: $1 ($2)"
    skipped=$((skipped + 1))
    log_case "$1" "n/a" "skipped" "skip" "$2"
}

echo "=== test_strict_gates ($RUN_ID) ==="

run_case format_check_missing_formatter_fails_strict nonzero 'format-check: FAIL .*strict mode' \
    format-check CLANG_FORMAT= FAIL_ON_MISSING_FORMATTER=1
run_case format_check_missing_formatter_skips_lenient 0 'format-check: SKIP' \
    format-check CLANG_FORMAT= FAIL_ON_MISSING_FORMATTER=0
run_case lint_missing_cppcheck_fails_strict nonzero 'lint: FAIL .*strict mode' \
    lint CPPCHECK= FAIL_ON_MISSING_LINTER=1
run_case lint_missing_cppcheck_skips_lenient 0 'lint: SKIP' \
    lint CPPCHECK= FAIL_ON_MISSING_LINTER=0

# Misformatting is caught. The scratch files sit under the repo, so
# clang-format picks up the project's .clang-format.
printf 'int  asx_gate_probe( void ){return 0;}\n' >"$WORK/bad/bad.c"
printf 'int asx_gate_probe(void);\n' >"$WORK/good/good.c"
probe="$(make --no-print-directory -C "$REPO_ROOT" format-check FORMAT_PATHS="$WORK/good" \
    FAIL_ON_MISSING_FORMATTER=0 2>&1)"
if grep -q 'format-check: SKIP' <<<"$probe"; then
    if [ "$STRICT" = "1" ]; then
        echo "  FAIL: pinned clang-format unavailable under STRICT_GATES/CI"
        failures=$((failures + 1))
        log_case formatter_available "available" "missing" "fail" "strict mode requires the pinned clang-format"
    else
        skip_case format_check_flags_misformatted_file "pinned clang-format unavailable"
        skip_case format_check_passes_formatted_file "pinned clang-format unavailable"
    fi
else
    run_case format_check_flags_misformatted_file nonzero 'format-check: FAIL' \
        format-check FORMAT_PATHS="$WORK/bad" FAIL_ON_MISSING_FORMATTER=1
    run_case format_check_passes_formatted_file 0 'format-check: PASS' \
        format-check FORMAT_PATHS="$WORK/good" FAIL_ON_MISSING_FORMATTER=1
fi

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_strict_gates: PASS ($skipped skipped)"
    exit 0
fi
echo "test_strict_gates: FAIL ($failures case(s))"
exit 1
