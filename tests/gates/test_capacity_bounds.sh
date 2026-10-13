#!/usr/bin/env bash
# test_capacity_bounds.sh — the lower-bound #errors of the size limits, and
# the lists that name them, must really bind (bd-t8if).
#
# Every capacity (make print-capacity-macros) that has a single-macro check
# `#if (ASX_X) < K` / `#error` is compiled twice, in the translation unit of
# its check (the header, through a probe that includes every public header, or
# the src/platform/posix file that defines it):
#   - at K - 1   -> must FAIL, with the macro's own #error message;
#   - at K       -> the probe and every source that names the macro must
#                   compile at -O2 under the strict warning set (no zero-size
#                   array, no overflow the compiler can see at the smallest
#                   size the build admits).
# Then tools/ci/capacity_table.sh runs on a scratch copy of the tree with
#   - a new guarded ASX_*_MAX macro in neither list      -> must FAIL;
#   - the lower-bound check of one capacity removed      -> must FAIL;
#   - a macro in both lists                              -> must FAIL.
#
# Everything lives under build/test-gates/<run_id>/; the real tree is never
# touched. Logs JSONL to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT" || exit 1
RUN_ID="capacity-bounds-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
mkdir -p "$WORK" "$LOG_DIR"
CC="${CC:-cc}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

STRICT="-std=c99 -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wswitch-enum -Wformat=2 -Wno-unused-parameter -Iinclude -Isrc"
CORE_DEFS="-DASX_PROFILE_CORE -DASX_CODEC_JSON -DASX_DETERMINISTIC=1"
LIVE_DEFS="-DASX_PROFILE_POSIX -DASX_CODEC_JSON -DASX_DETERMINISTIC=0"

failures=0
log_case() {
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"capacity_bounds","test":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" >>"$LOG"
}

capacities="$(make --no-print-directory -s print-capacity-macros | tr ' ' '\n' | sed '/^$/d' | sort -u)"
[ -n "$capacities" ] || { echo "test_capacity_bounds: FAIL (no capacity macros listed)"; exit 1; }

# The probe: every public header and the two src headers that define limits.
PROBE="$WORK/probe.c"
{
    for h in $(cd include && find asx -name '*.h' | sort); do echo "#include <$h>"; done
    echo '#include "sync/wait_queue.h"'
    echo '#include "conformance/canon.h"'
    echo '#include "conformance/json.h"'
    echo 'int main(void) { return 0; }'
} >"$PROBE"

# Bounds: "MACRO K FILE" for every capacity with a single-macro check.
BOUNDS="$WORK/bounds.txt"
: >"$BOUNDS"
for m in $capacities; do
    grep -rnE "^#(if|elif) +\\($m\\) *< *[0-9]+ *\$" include src 2>/dev/null |
        sed -E "s/^([^:]+):[0-9]+:#(if|elif) +\\(([A-Z0-9_]+)\\) *< *([0-9]+) *\$/\\3 \\4 \\1/" >>"$BOUNDS"
done
sort -u "$BOUNDS" -o "$BOUNDS"
n_bounds="$(wc -l <"$BOUNDS" | tr -d ' ')"
n_caps="$(printf '%s\n' "$capacities" | wc -l | tr -d ' ')"
# The capacities with a compound or derived check (e.g. ASX_MAX_ACTORS) are
# bound-checked by capacity_table.sh's grep but not probed here.
if [ "$n_bounds" -lt 100 ]; then
    echo "test_capacity_bounds: FAIL (only $n_bounds single-macro lower bounds found of $n_caps capacities)"
    exit 1
fi

# check_one MACRO K FILE: prints "ok" or a failure line.
check_one() {
    local m="$1" k="$2" f="$3" below tu defs out rc
    below=$((k - 1))
    case "$f" in
    *.c) tu="$f"; defs="$LIVE_DEFS" ;;
    *) tu="$PROBE"; defs="$CORE_DEFS" ;;
    esac
    # shellcheck disable=SC2086
    out="$($CC $STRICT $defs -fsyntax-only "-D$m=${below}u" "$tu" 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ]; then echo "FAIL $m: -D$m=${below}u compiled (the bound $k does not bind)"; return; fi
    if ! grep -q "$m must be" <<<"$out"; then
        echo "FAIL $m: -D$m=${below}u failed, but not with the macro's #error"; return
    fi
    # At the minimum: the probe and each source naming the macro, at -O2, in
    # the profile that compiles them. A bound relative to another capacity
    # (ASX_MONITOR_CAPACITY and ASX_LINK_CAPACITY must not exceed the actor
    # system lane) is taken with those at their own minimum.
    local srcs src sdefs extra=""
    case "$m" in
    ASX_ACTOR_SYSTEM_LANE_CAPACITY) extra="-DASX_MONITOR_CAPACITY=1u -DASX_LINK_CAPACITY=1u" ;;
    esac
    srcs="$PROBE $(grep -rwl "$m" src --include='*.c' 2>/dev/null | tr '\n' ' ')"
    for src in $srcs; do
        case "$src" in
        */platform/posix/*) sdefs="$LIVE_DEFS" ;;
        *) sdefs="$CORE_DEFS" ;;
        esac
        # shellcheck disable=SC2086
        if ! out="$($CC $STRICT $sdefs $extra -O2 -c -o /dev/null "-D$m=${k}u" "$src" 2>&1)"; then
            echo "FAIL $m: -D$m=${k}u does not compile $src: $(head -n 3 <<<"$out" | tr '\n' ' ' | cut -c1-200)"
            return
        fi
    done
    echo "ok $m"
}
export -f check_one
export CC STRICT CORE_DEFS LIVE_DEFS PROBE

RESULTS="$WORK/results.txt"
# shellcheck disable=SC2016
xargs -P "$JOBS" -L 1 bash -c 'check_one "$0" "$1" "$2"' <"$BOUNDS" >"$RESULTS"
bad="$(grep -c '^FAIL' "$RESULTS" || true)"
good="$(grep -c '^ok' "$RESULTS" || true)"
if [ "$bad" -eq 0 ] && [ "$good" -eq "$n_bounds" ]; then
    echo "  PASS: $good lower bounds: one below fails with its #error, the minimum compiles"
    log_case "lower_bounds" "pass" "$good"
else
    echo "  FAIL: lower bounds ($bad failing, $good ok of $n_bounds)"
    grep '^FAIL' "$RESULTS" | head -n 20
    failures=$((failures + 1))
    log_case "lower_bounds" "fail" "$bad failing"
fi

# The lists: run capacity_table.sh on a scratch copy of the tree with a plant.
copy_tree() {
    local d="$1"
    mkdir -p "$d/tools/ci"
    cp -R include src "$d/"
    cp Makefile "$d/"
    cp tools/ci/capacity_table.sh tools/ci/capacity_fixed.txt "$d/tools/ci/"
}
run_table() {
    # run_table DIR OUT: the script's output in that tree, the generated
    # files in OUT
    # shellcheck disable=SC2086
    ( cd "$1" && bash tools/ci/capacity_table.sh --out-dir "$2" -- "$CC" $CORE_DEFS -std=c99 ) 2>&1
}
control() {
    # control NAME DIR PATTERN: run_table must fail and print PATTERN
    local name="$1" dir="$2" pattern="$3" out rc
    out="$(run_table "$dir" "$dir/out")"
    rc=$?
    if [ "$rc" -ne 0 ] && grep -q -- "$pattern" <<<"$out"; then
        echo "  PASS: $name"
        log_case "$name" "pass" "exit$rc"
    else
        echo "  FAIL: $name (exit$rc): $(head -n 3 <<<"$out" | tr '\n' ' ' | cut -c1-200)"
        failures=$((failures + 1))
        log_case "$name" "fail" "exit$rc"
    fi
}

# The scratch copies need make to find a Makefile whose print-capacity-macros
# works without the rest of the tree: it only expands variables.
D1="$WORK/plant-new-limit"
copy_tree "$D1"
printf '%s\n' '#ifndef ASX_PLANTED_MAX_THING' '#define ASX_PLANTED_MAX_THING 4u' '#endif' >>"$D1/include/asx/asx_config.h"
control "new_guarded_limit_in_neither_list_fails" "$D1" "ASX_PLANTED_MAX_THING"

D2="$WORK/plant-no-bound"
copy_tree "$D2"
sed 's/^#if (ASX_SEMAPHORE_MAX) < 1$/#if 0/' "$D2/include/asx/asx_config.h" >"$D2/asx_config.h.new" &&
    cat "$D2/asx_config.h.new" >"$D2/include/asx/asx_config.h"
control "capacity_without_lower_bound_fails" "$D2" "ASX_SEMAPHORE_MAX has no lower-bound"

D3="$WORK/plant-both"
copy_tree "$D3"
printf '%s\n' 'ASX_SEMAPHORE_MAX planted into the fixed list' >>"$D3/tools/ci/capacity_fixed.txt"
control "macro_in_both_lists_fails" "$D3" "ASX_SEMAPHORE_MAX"

# And the real tree passes its own check.
out="$(run_table "$REPO_ROOT" "$WORK/out-real" 2>&1)"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "  PASS: real_tree_lists_are_complete"
    log_case "real_tree_lists_are_complete" "pass" "exit0"
else
    echo "  FAIL: real_tree_lists_are_complete: $(head -n 3 <<<"$out" | tr '\n' ' ' | cut -c1-200)"
    failures=$((failures + 1))
    log_case "real_tree_lists_are_complete" "fail" "exit$rc"
fi

if [ "$failures" -eq 0 ]; then
    echo "test_capacity_bounds: every control behaved"
    exit 0
fi
echo "test_capacity_bounds: $failures control(s) failed"
exit 1
