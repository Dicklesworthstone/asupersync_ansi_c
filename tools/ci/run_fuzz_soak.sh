#!/usr/bin/env bash
# =============================================================================
# run_fuzz_soak.sh — soak the libFuzzer parser harnesses (bd-9kll.16.3)
#
# Runs each given harness (built by `make fuzz-parsers`: clang, libFuzzer,
# ASan + UBSan) for a fixed time from the committed seeds
# (tests/fuzz/parsers_seeds/<TARGET>, the hand-written boundary inputs) and
# the grown corpus (tests/fuzz/parsers_corpus/<TARGET>, what earlier soaks
# found, `-merge=1` minimized). A crash, sanitizer report or timeout
# ends one libFuzzer process; the harness is started again on the same grown
# corpus until its time is spent (at most MAX_STARTS starts), so one finding
# does not end its soak. Findings stay as crash-*/timeout-*/oom-* files in
# <out>/<TARGET>/crashes; the grown corpus is <out>/<TARGET>/corpus (what
# `-merge=1` folds into tests/fuzz/parsers_corpus, see docs/QUALITY_GATES.md).
#
# Usage: tools/ci/run_fuzz_soak.sh [--seconds N] [--out DIR] [--bin-dir DIR]
#                                  [--seeds DIR]... [--max-starts N] TARGET...
#   --seeds   a directory holding one <TARGET>/ per harness; repeatable, all
#             are copied into the starting corpus. The first must have the
#             harness's directory, a later one may lack it (default: the
#             seeds, then the grown corpus).
# Output: a table (starts, executions, coverage edges, features, corpus size,
#         crashes) per target, also in <out>/summary.md.
# Exit:   0 no finding, 1 a finding (or a harness that would not start after
#         it), 2 tool error.
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -u

SECONDS_PER_TARGET=600
OUT_DIR=build/fuzz-parsers/soak
BIN_DIR=build/fuzz-parsers/bin
SEEDS_DIRS=()
MAX_STARTS=10
TARGETS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --seconds)    SECONDS_PER_TARGET="$2"; shift 2 ;;
        --out)        OUT_DIR="$2"; shift 2 ;;
        --bin-dir)    BIN_DIR="$2"; shift 2 ;;
        --seeds)      SEEDS_DIRS+=("$2"); shift 2 ;;
        --max-starts) MAX_STARTS="$2"; shift 2 ;;
        -*)           echo "run_fuzz_soak: unknown option $1" >&2; exit 2 ;;
        *)            TARGETS+=("$1"); shift ;;
    esac
done
if [ "${#TARGETS[@]}" -eq 0 ]; then
    echo "run_fuzz_soak: no TARGET given" >&2
    exit 2
fi
if [ "${#SEEDS_DIRS[@]}" -eq 0 ]; then
    SEEDS_DIRS=(tests/fuzz/parsers_seeds tests/fuzz/parsers_corpus)
fi

mkdir -p "$OUT_DIR"
SUMMARY="$OUT_DIR/summary.md"
{
    echo "| harness | starts | executions | cov | ft | corpus | crashes |"
    echo "|---|---|---|---|---|---|---|"
} > "$SUMMARY"
findings=0

for t in "${TARGETS[@]}"; do
    bin="$BIN_DIR/$t"
    seeds="${SEEDS_DIRS[0]}/$t"
    run="$OUT_DIR/$t"
    if [ ! -x "$bin" ] || [ ! -d "$seeds" ]; then
        echo "run_fuzz_soak: $t: missing $bin or $seeds" >&2
        exit 2
    fi
    mkdir -p "$run/corpus" "$run/crashes"
    for d in "${SEEDS_DIRS[@]}"; do
        if [ -d "$d/$t" ]; then
            find "$d/$t" -type f -exec cp {} "$run/corpus/" \;
        fi
    done
    : > "$run/log.txt"
    end=$(( $(date +%s) + SECONDS_PER_TARGET ))
    starts=0
    while [ "$(date +%s)" -lt "$end" ] && [ "$starts" -lt "$MAX_STARTS" ]; do
        left=$(( end - $(date +%s) ))
        [ "$left" -lt 2 ] && break
        "$bin" -max_total_time="$left" -timeout=10 -rss_limit_mb=2048 -print_final_stats=1 \
            -artifact_prefix="$run/crashes/" "$run/corpus" >> "$run/log.txt" 2>&1
        rc=$?
        starts=$((starts + 1))
        [ "$rc" -eq 0 ] && break
    done
    crashes=$(find "$run/crashes" -type f | wc -l | tr -d ' ')
    execs=$(sed -n 's/^stat::number_of_executed_units: *//p' "$run/log.txt" |
        awk '{s += $1} END {print s + 0}')
    last=$(grep -E ' cov: [0-9]+ ft: [0-9]+ corp: [0-9]+/' "$run/log.txt" | tail -1)
    cov=$(echo "$last" | sed -n 's/.* cov: \([0-9]*\) .*/\1/p')
    ft=$(echo "$last" | sed -n 's/.* ft: \([0-9]*\) .*/\1/p')
    corp=$(echo "$last" | sed -n 's/.* corp: \([0-9]*\/[0-9]*b\).*/\1/p')
    echo "| $t | $starts | $execs | ${cov:-?} | ${ft:-?} | ${corp:-?} | $crashes |" >> "$SUMMARY"
    if [ "$crashes" -gt 0 ]; then
        findings=1
        echo "run_fuzz_soak: FINDING $t: $crashes input(s) in $run/crashes" >&2
        tail -30 "$run/log.txt" >&2
    fi
done

cat "$SUMMARY"
if [ "$findings" -ne 0 ]; then
    echo "run_fuzz_soak: FAIL (a harness crashed; the inputs are the artifacts)"
    exit 1
fi
echo "run_fuzz_soak: PASS (${#TARGETS[@]} harness(es), ${SECONDS_PER_TARGET} s each)"
exit 0
