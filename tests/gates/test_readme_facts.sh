#!/usr/bin/env bash
# test_readme_facts.sh — negative controls for the README facts gate
# (tools/ci/check_readme_facts.sh, run by make lint-docs; bd-9kll.1.5).
#
# The gate compares every <!-- fact:KEY -->N<!-- /fact --> marker and the API
# and test badges with tools/count_inventory.sh. If its marker pattern stopped
# matching, it would report "matches (0 numbers)" and pass whatever the README
# says. These controls run it over scratch copies of README.md:
#   - the README as it is                         -> PASSES, checks >= 10 numbers
#   - one fact marker off by one                  -> FAILS naming the fact
#   - a marker with an unknown key                -> FAILS naming the key
#   - the test-programs badge off by one          -> FAILS naming the badge
#
# Scratch files live under build/test-gates/<run_id>/; tracked files are never
# touched. Logs JSONL to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="readme-facts-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
CHECK="$REPO_ROOT/tools/ci/check_readme_facts.sh"
mkdir -p "$WORK" "$LOG_DIR"

failures=0

# replace_first <old> <new> <in> <out>: the first occurrence, as literal text.
replace_first() {
    awk -v old="$1" -v new="$2" '
        !done && (i = index($0, old)) {
            $0 = substr($0, 1, i - 1) new substr($0, i + length(old))
            done = 1
        }
        { print }' "$3" >"$4"
}

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"readme_facts","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# run_case <name> <expected exit: 0|nonzero> <expected output regex> <readme copy>
run_case() {
    local name="$1" expect="$2" pattern="$3" readme="$4"
    local out rc observed status
    out="$(bash "$CHECK" "$readme" 2>&1)"
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

echo "=== test_readme_facts ($RUN_ID) ==="

cp "$REPO_ROOT/README.md" "$WORK/README.md"
run_case readme_as_is_passes 0 'matches \(([1-9][0-9]+) numbers\)' "$WORK/README.md"

# The first fact marker, its number raised by one.
marker="$(grep -oE '<!-- fact:[a-z0-9_]+ -->[0-9,]+<!-- /fact -->' "$WORK/README.md" | head -1)"
key="$(printf '%s' "$marker" | sed -E 's/^<!-- fact:([a-z0-9_]+) -->.*/\1/')"
value="$(printf '%s' "$marker" | sed -E 's/^.*-->([0-9,]+)<!--.*/\1/' | tr -d ,)"
wrong="<!-- fact:$key -->$((value + 1))<!-- /fact -->"
replace_first "$marker" "$wrong" "$WORK/README.md" "$WORK/README.wrong-fact.md"
run_case fact_off_by_one_fails nonzero "FAIL: $key is $value, .* says $((value + 1))" \
    "$WORK/README.wrong-fact.md"

# A marker whose key count_inventory.sh does not know.
cp "$WORK/README.md" "$WORK/README.unknown-key.md"
printf '\n<!-- fact:no_such_fact -->1<!-- /fact -->\n' >>"$WORK/README.unknown-key.md"
run_case unknown_fact_key_fails nonzero "FAIL: unknown fact 'no_such_fact'" \
    "$WORK/README.unknown-key.md"

# The test-programs badge, its number raised by one.
badge="$(grep -oE 'badge/tests-[0-9%C]+%20programs' "$WORK/README.md" | head -1)"
count="$(printf '%s' "$badge" | sed -E 's/^badge\/tests-//; s/%20programs$//; s/%2C//g')"
replace_first "$badge" "badge/tests-$((count + 1))%20programs" "$WORK/README.md" \
    "$WORK/README.wrong-badge.md"
run_case test_badge_off_by_one_fails nonzero "FAIL: badge test_programs says '$((count + 1))'" \
    "$WORK/README.wrong-badge.md"

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_readme_facts: PASS"
    exit 0
fi
echo "test_readme_facts: FAIL ($failures case(s))"
exit 1
