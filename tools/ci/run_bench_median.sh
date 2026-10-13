#!/usr/bin/env bash
# =============================================================================
# run_bench_median.sh — run a benchmark N times and keep the median
#                       (DEC-016, bd-9kll.14.2)
#
# The performance gates judge the median of five runs of each statistic, not
# one run: a tail percentile of one run is one or two samples, and one
# preemption moved the verdict of a commit whose code had not changed.
#
# Usage:
#   tools/ci/run_bench_median.sh [--runs N] --output <file.json> -- <command...>
#
# <command...> prints one bench_runtime JSON document on stdout (for example
# `make bench-json PROFILE=CORE BUILD_DIR=build/perf-core`, or
# `build/bench/bench_runtime --json`). It runs N times (default 5, odd, at
# least 3). Every run's document is kept beside the output as
# <output stem>-run-<i>.json, and tools/ci/aggregate_bench_runs.sh writes the
# median document to <file.json>, with every run's value of every metric in
# its `repeats` object.
#
# Exit codes: 0 done; 2 usage; the command's own status when a run fails (the
# runs already made are kept).
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNS=5
OUTPUT=""

usage() {
    sed -n '/^# Usage:/,/^# Exit codes:/p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --runs)
            [ $# -ge 2 ] || usage
            RUNS="$2"
            shift 2
            ;;
        --output)
            [ $# -ge 2 ] || usage
            OUTPUT="$2"
            shift 2
            ;;
        --)
            shift
            break
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo "[asx] bench-median: ERROR - unknown option: $1" >&2
            usage
            ;;
    esac
done

if [ -z "$OUTPUT" ] || [ $# -eq 0 ]; then
    echo "[asx] bench-median: ERROR - --output and a command after -- are required" >&2
    usage
fi
case "$RUNS" in
    ''|*[!0-9]*) echo "[asx] bench-median: ERROR - --runs must be a number" >&2; exit 2 ;;
esac
if [ "$RUNS" -lt 3 ] || [ $(( RUNS % 2 )) -eq 0 ]; then
    echo "[asx] bench-median: ERROR - --runs must be odd and at least 3 (got $RUNS)" >&2
    exit 2
fi

STEM="${OUTPUT%.json}"
mkdir -p "$(dirname "$OUTPUT")"

run_files=()
i=1
while [ "$i" -le "$RUNS" ]; do
    run_file="${STEM}-run-${i}.json"
    echo "[asx] bench-median: run $i of $RUNS -> $run_file" >&2
    "$@" > "$run_file"
    run_files+=("$run_file")
    i=$((i + 1))
done

exec "$SCRIPT_DIR/aggregate_bench_runs.sh" --output "$OUTPUT" "${run_files[@]}"
