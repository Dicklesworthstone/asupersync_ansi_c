#!/usr/bin/env bash
# =============================================================================
# capture_baselines.sh — re-capture the SLO baselines of one profile from a
#                        Perf Gates run, budgets untouched (DEC-014,
#                        bd-9kll.14.2)
#
# tools/ci/slo_baselines.json is under approval_required governance: its
# baselines go stale (max_age_days, 120) and are re-captured on the CI runner
# class from the Perf Gates workflow's own artifact, with the host and the
# commit recorded. This script is that re-capture. It never changes a budget:
# a budget that has to move is a separate owner decision (DEC-016 made the
# statistic the median of five runs; DEC-015 the size gate's subject), not
# something a capture does.
#
# Usage:
#   tools/ci/capture_baselines.sh --bench-json <bench-results.json>
#       --profile CORE|HFT --run-id <id> --run-url <url> --decision DEC-014
#       --author <name> --output <file> [--baselines <file>] [--commit <sha>]
#       [--captured-at <iso8601>]
#
# --bench-json is the artifact's bench-results.json (the median of N runs from
# tools/ci/run_bench_median.sh, or a single run; the document says which in
# its `repeats`). --commit defaults to bench_metadata.host.ci_sha, which the
# runner records (GITHUB_SHA). --output may be the baselines file itself.
#
# Updated for the profile: every benchmarks.<name>.<metric>, deadline_report
# and adaptive_report entry that the bench document has gets `baseline` = the
# measured value and `headroom` = budget/baseline (baseline/budget for a
# `lower` direction), rounded to one decimal; the profile's captured_at and
# captured_on; baseline_metadata (git_commit, compiler, target, command
# unchanged, captured_at, host, run_id, run_url, statistic, runs, decision);
# and one governance.audit_trail entry. Nothing else changes, and the script
# checks that every budget is byte-for-byte what it was.
#
# Exit codes: 0 written; 1 a budget would have changed (nothing written);
# 2 usage or an input that cannot be read.
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASELINES="${SCRIPT_DIR}/slo_baselines.json"
BENCH_JSON=""
PROFILE=""
RUN_ID=""
RUN_URL=""
DECISION=""
AUTHOR=""
OUTPUT=""
COMMIT=""
CAPTURED_AT=""

usage() {
    sed -n '/^# Usage:/,/^# Exit codes:/p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bench-json) [ $# -ge 2 ] || usage; BENCH_JSON="$2"; shift 2 ;;
        --profile)    [ $# -ge 2 ] || usage; PROFILE="$2"; shift 2 ;;
        --run-id)     [ $# -ge 2 ] || usage; RUN_ID="$2"; shift 2 ;;
        --run-url)    [ $# -ge 2 ] || usage; RUN_URL="$2"; shift 2 ;;
        --decision)   [ $# -ge 2 ] || usage; DECISION="$2"; shift 2 ;;
        --author)     [ $# -ge 2 ] || usage; AUTHOR="$2"; shift 2 ;;
        --output)     [ $# -ge 2 ] || usage; OUTPUT="$2"; shift 2 ;;
        --baselines)  [ $# -ge 2 ] || usage; BASELINES="$2"; shift 2 ;;
        --commit)     [ $# -ge 2 ] || usage; COMMIT="$2"; shift 2 ;;
        --captured-at) [ $# -ge 2 ] || usage; CAPTURED_AT="$2"; shift 2 ;;
        --help|-h) usage ;;
        *) echo "[asx] capture-baselines: ERROR - unknown option: $1" >&2; usage ;;
    esac
done

for need in BENCH_JSON PROFILE RUN_ID RUN_URL DECISION AUTHOR OUTPUT; do
    if [ -z "${!need}" ]; then
        echo "[asx] capture-baselines: ERROR - a value for $need is required" >&2
        usage
    fi
done
for f in "$BENCH_JSON" "$BASELINES"; do
    if [ ! -s "$f" ] || ! jq -e type "$f" >/dev/null 2>&1; then
        echo "[asx] capture-baselines: ERROR - not readable JSON: $f" >&2
        exit 2
    fi
done
if ! jq -e --arg p "$PROFILE" '.profiles[$p] != null' "$BASELINES" >/dev/null; then
    echo "[asx] capture-baselines: ERROR - no profile $PROFILE in $BASELINES" >&2
    exit 2
fi
bench_profile="$(jq -r '.profile // ""' "$BENCH_JSON")"
if [ "$bench_profile" != "$PROFILE" ]; then
    echo "[asx] capture-baselines: ERROR - the bench document is profile '$bench_profile', not $PROFILE" >&2
    exit 2
fi
[ -n "$COMMIT" ] || COMMIT="$(jq -r '.bench_metadata.host.ci_sha // ""' "$BENCH_JSON")"
if [ -z "$COMMIT" ]; then
    echo "[asx] capture-baselines: ERROR - no commit: the runner did not record GITHUB_SHA; pass --commit" >&2
    exit 2
fi
[ -n "$CAPTURED_AT" ] || CAPTURED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

new="$(jq --slurpfile b "$BENCH_JSON" \
    --arg p "$PROFILE" --arg run_id "$RUN_ID" --arg run_url "$RUN_URL" \
    --arg decision "$DECISION" --arg author "$AUTHOR" --arg commit "$COMMIT" \
    --arg at "$CAPTURED_AT" '
    def r1: (. * 10 | round) / 10;
    def put(actual; direction):
        if actual == null then .
        else .baseline = actual
             | .headroom = (if (.budget // 0) == 0 or actual == 0 then .headroom
                            elif direction == "lower" then (actual / .budget | r1)
                            else (.budget / actual | r1) end)
        end;
    ($b[0]) as $bench
    | ($bench.repeats.runs // 1) as $runs
    | ($bench.bench_metadata // {}) as $meta
    | ($meta.host // {}) as $host
    | .profiles[$p] as $old
    | .profiles[$p].benchmarks |= with_entries(
          .key as $name
          | .value |= with_entries(
              .key as $metric
              | .value |= put($bench.benchmarks[$name][$metric]; "upper")))
    | .profiles[$p].deadline_report |= with_entries(
          .key as $k | .value |= put($bench.deadline_report[$k]; "upper"))
    | .profiles[$p].adaptive_report |= with_entries(
          .key as $k
          | .value |= put(
              (if $k == "mean_confidence_fp32_floor" then $bench.adaptive_report.mean_confidence_fp32
               elif $k == "fallback_rate" then $bench.adaptive_report.fallback_rate
               else null end);
              (.direction // "upper")))
    | .profiles[$p].captured_at = $at
    | .profiles[$p].captured_on = ("github-hosted runner: " + ($host.cpu_model // "unrecorded"))
    | .profiles[$p].baseline_metadata += {
          git_commit: $commit,
          compiler: ($meta.compiler // $old.baseline_metadata.compiler),
          compiler_version: ($meta.compiler_version // null),
          target: ("linux-" + (if ($host.pointer_bits // 64) == 64 then "x86_64" else "x86" end)
                   + " " + ($host.os_release // "unrecorded")),
          captured_at: $at,
          host: $host,
          run_id: $run_id,
          run_url: $run_url,
          statistic: (if $runs > 1 then "median" else "single run" end),
          runs: $runs,
          decision: $decision
      }
    | .governance.audit_trail += [{
          date: ($at | .[0:10]),
          author: $author,
          action: "recapture_baselines",
          rationale: ("Re-captured the " + $p + " baselines on the CI runner class from the Perf Gates run "
                      + $run_id + " (" + $run_url + "), commit " + $commit + ", host "
                      + ($host.cpu_model // "unrecorded") + ", " + ($runs | tostring) + " run(s), "
                      + (if $runs > 1 then "median" else "single run" end)
                      + ". Budgets unchanged (" + $decision + ")."),
          commit: $commit,
          run_id: $run_id,
          decision: $decision
      }]
' "$BASELINES")"

# The only thing this script may not do: change a budget or a direction.
if ! jq -ne --argjson a "$(jq -c --arg p "$PROFILE" '[.profiles[$p] | .. | objects | select(has("budget")) | {budget, direction}]' "$BASELINES")" \
           --argjson b "$(printf '%s' "$new" | jq -c --arg p "$PROFILE" '[.profiles[$p] | .. | objects | select(has("budget")) | {budget, direction}]')" \
           '$a == $b' >/dev/null; then
    echo "[asx] capture-baselines: ERROR - a budget would have changed; nothing written" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT")"
printf '%s\n' "$new" > "$OUTPUT"
echo "[asx] capture-baselines: $PROFILE re-captured from run $RUN_ID at $COMMIT into $OUTPUT" >&2
