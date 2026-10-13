#!/usr/bin/env bash
# =============================================================================
# aggregate_bench_runs.sh — the median of N benchmark runs (DEC-016,
#                           bd-9kll.14.2)
#
# The SLO and size gates judge percentile statistics that are single samples
# of one run: p99_9 of 1000 samples is the 2nd largest, p99_99 of 10000 the
# 2nd. One preemption moves the verdict of a commit whose code did not
# change. DEC-016 makes the statistic the median of five runs, thresholds
# unchanged. This script is that aggregation: it takes the JSON of N runs of
# the same bench binary and writes one document in the same shape, so every
# consumer of bench-results.json keeps reading it, in which every numeric
# value under benchmarks, cold_start_report, deadline_report and
# adaptive_report is the median of the runs' values, and a top-level
# `repeats` object records all of them.
#
# Usage:
#   tools/ci/aggregate_bench_runs.sh --output <file> <run.json> <run.json>...
#
#   N must be odd and at least 3 (the median is then one of the runs' values,
#   not an average of two). Every run must be a bench_runtime JSON of the same
#   profile.
#
# The output is the first run's document with the medians put in, plus
#   repeats: {
#     statistic: "median", runs: N, files: [...],
#     samples: { "benchmarks/<name>/<metric>": [v1 .. vN], ... }
#   }
# where the samples are in run order. The runs themselves are not touched.
#
# Exit codes: 0 written; 2 usage or an input that cannot be aggregated.
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

OUTPUT=""
FILES=()

usage() {
    sed -n '2,/^# ====/p' "$0" | sed -n '/^# Usage:/,/^#   N must/p' | sed 's/^# \{0,1\}//' >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --output)
            [ $# -ge 2 ] || usage
            OUTPUT="$2"
            shift 2
            ;;
        --help|-h)
            usage
            ;;
        --*)
            echo "[asx] aggregate-bench: ERROR - unknown option: $1" >&2
            usage
            ;;
        *)
            FILES+=("$1")
            shift
            ;;
    esac
done

if [ -z "$OUTPUT" ] || [ "${#FILES[@]}" -lt 3 ]; then
    echo "[asx] aggregate-bench: ERROR - --output and at least 3 run files are required" >&2
    usage
fi
if [ $(( ${#FILES[@]} % 2 )) -eq 0 ]; then
    echo "[asx] aggregate-bench: ERROR - an odd number of runs is required (got ${#FILES[@]})" >&2
    exit 2
fi
for f in "${FILES[@]}"; do
    if [ ! -s "$f" ] || ! jq -e 'type == "object" and (.benchmarks | type == "object")' "$f" >/dev/null 2>&1; then
        echo "[asx] aggregate-bench: ERROR - not a bench_runtime JSON with benchmarks: $f" >&2
        exit 2
    fi
done

profiles="$(jq -r '.profile // ""' "${FILES[@]}" | sort -u | wc -l | tr -d ' ')"
if [ "$profiles" != "1" ]; then
    echo "[asx] aggregate-bench: ERROR - the runs are not all one profile" >&2
    exit 2
fi

files_json="$(printf '%s\n' "${FILES[@]}" | jq -R . | jq -s .)"

mkdir -p "$(dirname "$OUTPUT")"
jq -s --argjson files "$files_json" '
    . as $runs
    | ($runs | length) as $n
    | def med: sort | .[(length / 2 | floor)];
      def judged: .[0] == "benchmarks" or .[0] == "cold_start_report"
                  or .[0] == "deadline_report" or .[0] == "adaptive_report";
      # every judged number that is a number in every run
      [ $runs[0] | paths(type == "number") | select(judged)
        | select(. as $p | $runs | all(getpath($p) | type == "number")) ] as $paths
    | (reduce $paths[] as $p ($runs[0];
          setpath($p; ($runs | map(getpath($p)) | med)))) as $merged
    | $merged
    + { repeats: {
          statistic: "median",
          runs: $n,
          files: $files,
          samples: (reduce $paths[] as $p ({};
              . + { ($p | map(tostring) | join("/")): ($runs | map(getpath($p))) }))
        } }
' "${FILES[@]}" > "$OUTPUT"

echo "[asx] aggregate-bench: median of ${#FILES[@]} runs written to $OUTPUT" >&2
