#!/usr/bin/env bash
# test_doc_citations.sh — negative controls for the doc citation gate
# (tools/ci/check_doc_citations.py, run by make lint-docs; bd-r3o5).
#
# The gate checks that every cited repository path and line range exists
# and that a citation tied to a C symbol ("`sym` (`path:N`)" or
# "`path:N` `sym`") cites lines that mention the symbol. If its patterns
# stopped matching, drifted citations would pass. These controls run it
# over the parity docs and over scratch docs built around one function's
# real definition line:
#   - the parity docs as they are                  -> PASSES
#   - `sym` (`path:DEF`), and `path:DEF` `sym`     -> PASSES
#   - `sym` (`path:DEF+1`)                         -> FAILS naming the symbol and DEF
#   - `path:DEF+1` `sym`                           -> FAILS likewise
#   - a word the file never mentions next to a citation -> PASSES (not a symbol)
#   - a line past the end of the file              -> FAILS
#
# Scratch files live under build/test-gates/<run_id>/; tracked files are never
# touched. Logs JSONL to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="doc-citations-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK_REL="build/test-gates/$RUN_ID"
WORK="$REPO_ROOT/$WORK_REL"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
CHECK="$REPO_ROOT/tools/ci/check_doc_citations.py"
mkdir -p "$WORK" "$LOG_DIR"

failures=0

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"doc_citations","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# run_case <name> <expected exit: 0|nonzero> <expected output regex> [doc...]
run_case() {
    local name="$1" expect="$2" pattern="$3"
    shift 3
    local out rc observed status
    out="$(python3 "$CHECK" "$@" 2>&1)"
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

# scratch_doc <name> <line>: a one-line doc under the scratch directory.
scratch_doc() {
    printf '%s\n' "$2" >"$WORK/$1.md"
    printf '%s' "$WORK_REL/$1.md"
}

echo "=== test_doc_citations ($RUN_ID) ==="

run_case docs_as_they_are_pass 0 'every cited path, line range, symbol and fixture resolves'

SRC="src/runtime/scheduler.c"
SYM="asx_task_cancel_dominates_internal"
DEF="$(grep -n "^int $SYM(" "$REPO_ROOT/$SRC" | head -1 | cut -d: -f1)"
if [ -z "$DEF" ]; then
    echo "  FAIL: cannot find the definition of $SYM in $SRC"
    exit 1
fi
OFF=$((DEF + 1))

doc="$(scratch_doc right_before "The rule (\`$SYM\` (\`$SRC:$DEF\`)) and \`$SRC:$DEF\` \`$SYM\`.")"
run_case symbol_on_its_line_passes 0 'resolves' "$doc"

doc="$(scratch_doc off_before "The rule (\`$SYM\` (\`$SRC:$OFF\`)).")"
run_case symbol_before_off_by_one_fails nonzero "for \`$SYM\`, which those lines do not mention; it is at $SRC:$DEF" "$doc"

doc="$(scratch_doc off_after "The rule: \`$SRC:$OFF\` \`$SYM\`.")"
run_case symbol_after_off_by_one_fails nonzero "for \`$SYM\`, which those lines do not mention; it is at $SRC:$DEF" "$doc"

doc="$(scratch_doc vocabulary "The outcome \`Cancelled\` (\`$SRC:$OFF\`).")"
run_case word_not_in_the_file_passes 0 'resolves' "$doc"

doc="$(scratch_doc past_end "See \`$SRC:999999\`.")"
run_case line_past_the_end_fails nonzero "cites $SRC:999999, but the file has [0-9]+ lines" "$doc"

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_doc_citations: PASS"
    exit 0
fi
echo "test_doc_citations: FAIL ($failures case(s))"
exit 1
