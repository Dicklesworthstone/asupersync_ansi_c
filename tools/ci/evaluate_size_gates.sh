#!/usr/bin/env bash
# =============================================================================
# evaluate_size_gates.sh — binary-size and cold-start SLO gate evaluator
#                          (bd-66l.8)
#
# Measures binary size metrics from a release build and cold-start metrics
# from benchmark JSON, compares against per-profile baselines, and emits
# structured pass/fail reports with budget deltas and suspect identification.
#
# Usage:
#   tools/ci/evaluate_size_gates.sh --lib <file> [--bench-json <file>]
#       [--baselines <file>] [--profile <name>] [--run-id <id>]
#       [--strict] [--output <file>]
#
# Exit codes:
#   0: all gates pass
#   1: one or more gates violated (strict mode)
#   2: usage/configuration error
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

LIB_PATH=""
BENCH_JSON=""
BASELINES="${SCRIPT_DIR}/size_cold_start_baselines.json"
PROFILE=""
RUN_ID="${ASX_CI_RUN_TAG:-size-$(date -u +%Y%m%dT%H%M%SZ)}"
STRICT=0
OUTPUT=""
INCLUDE_CONFORMANCE=0

usage() {
    cat <<'USAGE'
Usage: tools/ci/evaluate_size_gates.sh [OPTIONS]

Required:
  --lib <file>              Static library (.a) to measure

Options:
  --bench-json <file>       Benchmark JSON for cold-start metrics
  --baselines <file>        Baselines file (default: tools/ci/size_cold_start_baselines.json)
  --profile <name>          Override profile (default: CORE)
  --run-id <id>             Run identifier (default: ASX_CI_RUN_TAG or timestamp)
  --strict                  Exit 1 on any violation (default: always exit 0)
  --output <file>           Write gate report JSON to file (default: stdout)
  --include-conformance     Measure the host-only conformance interpreter too
                            (default: left out, DEC-015)
  --help                    Show this help
USAGE
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --lib)
            [ $# -ge 2 ] || usage
            LIB_PATH="$2"
            shift 2
            ;;
        --bench-json)
            [ $# -ge 2 ] || usage
            BENCH_JSON="$2"
            shift 2
            ;;
        --baselines)
            [ $# -ge 2 ] || usage
            BASELINES="$2"
            shift 2
            ;;
        --profile)
            [ $# -ge 2 ] || usage
            PROFILE="$2"
            shift 2
            ;;
        --run-id)
            [ $# -ge 2 ] || usage
            RUN_ID="$2"
            shift 2
            ;;
        --strict)
            STRICT=1
            shift
            ;;
        --output)
            [ $# -ge 2 ] || usage
            OUTPUT="$2"
            shift 2
            ;;
        --include-conformance)
            INCLUDE_CONFORMANCE=1
            shift
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo "[asx] size-gate: ERROR — unknown option: $1" >&2
            usage
            ;;
    esac
done

# --- Validate inputs ---

if [ -z "$LIB_PATH" ]; then
    echo "[asx] size-gate: ERROR — --lib is required" >&2
    usage
fi

if [ ! -f "$LIB_PATH" ]; then
    echo "[asx] size-gate: ERROR — library not found: $LIB_PATH" >&2
    exit 2
fi

if [ ! -f "$BASELINES" ]; then
    echo "[asx] size-gate: ERROR — baselines file not found: $BASELINES" >&2
    exit 2
fi

# --- Detect profile ---

if [ -z "$PROFILE" ]; then
    if [ -n "$BENCH_JSON" ] && [ -f "$BENCH_JSON" ]; then
        PROFILE="$(jq -r '.profile // "CORE"' "$BENCH_JSON")"
    else
        PROFILE="CORE"
    fi
fi

# --- Check profile exists in baselines ---

has_profile="$(jq -r --arg p "$PROFILE" '.profiles[$p] // empty' "$BASELINES")"
if [ -z "$has_profile" ]; then
    echo "[asx] size-gate: WARN — profile '$PROFILE' not found in baselines, skipping gate" >&2
    report=$(jq -n \
        --arg schema "asx.size_gate_report.v1" \
        --arg run_id "$RUN_ID" \
        --arg profile "$PROFILE" \
        --arg ts "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        '{
            schema: $schema,
            run_id: $run_id,
            profile: $profile,
            generated_at: $ts,
            status: "skip",
            reason: "no baseline for profile",
            violations: [],
            passes: [],
            summary: { total: 0, passed: 0, failed: 0, skipped: 0 }
        }')
    if [ -n "$OUTPUT" ]; then
        mkdir -p "$(dirname "$OUTPUT")"
        echo "$report" > "$OUTPUT"
    else
        echo "$report"
    fi
    exit 0
fi

# --- Measure binary size ---

# The whole archive first, for the record (full_archive in the report).
full_archive_bytes="$(wc -c < "$LIB_PATH" | tr -d ' ')"
full_size_output="$(size "$LIB_PATH" 2>/dev/null || true)"
if [ -n "$full_size_output" ]; then
    full_text_bytes="$(echo "$full_size_output" | awk 'NR>1{s+=$1} END{print s+0}')"
    full_data_bytes="$(echo "$full_size_output" | awk 'NR>1{s+=$2} END{print s+0}')"
    full_bss_bytes="$(echo "$full_size_output" | awk 'NR>1{s+=$3} END{print s+0}')"
else
    full_text_bytes=0
    full_data_bytes=0
    full_bss_bytes=0
fi

# DEC-015: the budgets bound the shipped library surface. The conformance
# interpreter (src/conformance: json, canon, interpreter) is host-only test
# machinery whose static tables are most of the archive's bss; its members
# are left out of the measurement, which is taken on a copy of the archive
# with them removed (the archive itself is not touched). --include-conformance
# measures everything, as the gate did before DEC-015.
EXCLUDED_MEMBERS=()
if [ "$INCLUDE_CONFORMANCE" != "1" ]; then
    for src in "$REPO_ROOT"/src/conformance/*.c; do
        [ -f "$src" ] || continue
        member="$(basename "$src" .c).o"
        if ar t "$LIB_PATH" 2>/dev/null | grep -qx -- "$member"; then
            EXCLUDED_MEMBERS+=("$member")
        fi
    done
fi

MEASURE_PATH="$LIB_PATH"
if [ "${#EXCLUDED_MEMBERS[@]}" -gt 0 ]; then
    MEASURE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/asx-size-gate-XXXXXX")"
    MEASURE_PATH="$MEASURE_DIR/shipped.a"
    cp "$LIB_PATH" "$MEASURE_PATH"
    ar d "$MEASURE_PATH" "${EXCLUDED_MEMBERS[@]}"
fi

archive_bytes="$(wc -c < "$MEASURE_PATH" | tr -d ' ')"

# Extract text/data/bss totals from size command
size_output="$(size "$MEASURE_PATH" 2>/dev/null || true)"
if [ -n "$size_output" ]; then
    text_bytes="$(echo "$size_output" | awk 'NR>1{s+=$1} END{print s+0}')"
    data_bytes="$(echo "$size_output" | awk 'NR>1{s+=$2} END{print s+0}')"
    bss_bytes="$(echo "$size_output" | awk 'NR>1{s+=$3} END{print s+0}')"
else
    text_bytes=0
    data_bytes=0
    bss_bytes=0
fi

# --- Build per-module size breakdown for suspect identification ---

suspects_json="[]"
if [ -n "$size_output" ]; then
    suspects_json="$(echo "$size_output" | awk 'NR>1{printf "{\"module\":\"%s\",\"text\":%d,\"data\":%d,\"bss\":%d},\n", $6, $1, $2, $3}' | sed '$ s/,$//' | { echo "["; cat; echo "]"; } | jq -c '.')"
fi

# --- Extract cold-start metrics from bench JSON ---

cold_start_json="null"
cold_samples_json="{}"
if [ -n "$BENCH_JSON" ] && [ -f "$BENCH_JSON" ]; then
    cold_start_json="$(jq '.cold_start_report // null' "$BENCH_JSON")"
    # DEC-016: a bench-results.json that is the median of N runs carries the
    # N values of each metric in `repeats`.
    cold_samples_json="$(jq '(.repeats.samples // {})
        | with_entries(select(.key | startswith("cold_start_report/")))
        | with_entries(.key |= sub("^cold_start_report/"; ""))' "$BENCH_JSON")"
fi
excluded_json="$(printf '%s\n' "${EXCLUDED_MEMBERS[@]+"${EXCLUDED_MEMBERS[@]}"}" | jq -R . | jq -sc 'map(select(. != ""))')"

# --- Evaluate gates ---

report=$(jq -n \
    --slurpfile base "$BASELINES" \
    --arg profile "$PROFILE" \
    --arg run_id "$RUN_ID" \
    --arg lib_path "$LIB_PATH" \
    --arg baselines_file "$BASELINES" \
    --arg bench_json "${BENCH_JSON:-none}" \
    --arg ts "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --argjson archive_bytes "$archive_bytes" \
    --argjson text_bytes "$text_bytes" \
    --argjson data_bytes "$data_bytes" \
    --argjson bss_bytes "$bss_bytes" \
    --argjson cold_start "$cold_start_json" \
    --argjson cold_samples "$cold_samples_json" \
    --argjson excluded "$excluded_json" \
    --argjson full_archive_bytes "$full_archive_bytes" \
    --argjson full_text_bytes "$full_text_bytes" \
    --argjson full_data_bytes "$full_data_bytes" \
    --argjson full_bss_bytes "$full_bss_bytes" \
    --argjson suspects "$suspects_json" \
'
def check_upper(category; metric_name; actual; budget_obj):
    {
        category: category,
        metric: metric_name,
        actual: actual,
        budget: budget_obj.budget,
        baseline: budget_obj.baseline,
        headroom: budget_obj.headroom,
        delta_vs_budget: (actual - budget_obj.budget),
        delta_vs_baseline: (actual - budget_obj.baseline),
        pct_of_budget: (if budget_obj.budget > 0 then ((actual / budget_obj.budget) * 100 | round) else 0 end),
        passed: (actual <= budget_obj.budget)
    };

($base[0].profiles[$profile]) as $p |

# Check size metrics
[
    (if ($p.size_metrics // null) != null then
        (check_upper("size"; "archive_bytes"; $archive_bytes; $p.size_metrics.archive_bytes)),
        (check_upper("size"; "text_bytes"; $text_bytes; $p.size_metrics.text_bytes)),
        (check_upper("size"; "data_bytes"; $data_bytes; $p.size_metrics.data_bytes)),
        (check_upper("size"; "bss_bytes"; $bss_bytes; $p.size_metrics.bss_bytes))
    else
        empty
    end),

    # Check cold-start metrics
    (if ($p.cold_start_metrics // null) != null and $cold_start != null then
        ($p.cold_start_metrics | to_entries[] |
            . as $entry |
            ($cold_start[$entry.key] // null) as $actual |
            if $actual != null then
                check_upper("cold_start"; $entry.key; $actual; $entry.value)
                + (($cold_samples[$entry.key] // null) as $s |
                   if $s == null then {}
                   else {runs: ($s | length), samples_all: $s,
                         runs_over_budget: ($s | map(select(. > $entry.value.budget)) | length)}
                   end)
            else
                empty
            end
        )
    else
        empty
    end)
] as $checks |

($checks | map(select(.passed)) | length) as $passed |
($checks | map(select(.passed | not)) | length) as $failed |

{
    schema: "asx.size_gate_report.v1",
    run_id: $run_id,
    profile: $profile,
    generated_at: $ts,
    sources: {
        lib: $lib_path,
        baselines: $baselines_file,
        bench_json: $bench_json
    },
    measured_surface: {
        kind: (if ($excluded | length) > 0 then "shipped" else "full_archive" end),
        excluded_members: $excluded,
        full_archive: {
            archive_bytes: $full_archive_bytes,
            text_bytes: $full_text_bytes,
            data_bytes: $full_data_bytes,
            bss_bytes: $full_bss_bytes
        }
    },
    measured: {
        archive_bytes: $archive_bytes,
        text_bytes: $text_bytes,
        data_bytes: $data_bytes,
        bss_bytes: $bss_bytes,
        cold_start: $cold_start
    },
    suspects: ($suspects | sort_by(-.text) | .[0:10]),
    status: (if $failed > 0 then "fail" else "pass" end),
    summary: {
        total: ($checks | length),
        passed: $passed,
        failed: $failed,
        skipped: 0
    },
    violations: ($checks | map(select(.passed | not)) | sort_by(.delta_vs_budget) | reverse),
    passes: ($checks | map(select(.passed)) | sort_by(.pct_of_budget) | reverse),
    worst_offenders: ($checks | map(select(.passed | not)) | sort_by(.delta_vs_budget) | reverse | .[0:5]),
    rerun_command: "make release && tools/ci/evaluate_size_gates.sh --lib build/lib/libasx.a --bench-json bench-results.json --strict"
}
')

# --- Output report ---

if [ -n "$OUTPUT" ]; then
    mkdir -p "$(dirname "$OUTPUT")"
    echo "$report" > "$OUTPUT"
    echo "[asx] size-gate: report written to $OUTPUT" >&2
else
    echo "$report"
fi

# --- Summary ---

status="$(echo "$report" | jq -r '.status')"
total="$(echo "$report" | jq -r '.summary.total')"
passed="$(echo "$report" | jq -r '.summary.passed')"
failed="$(echo "$report" | jq -r '.summary.failed')"

echo "[asx] size-gate: status=$status total=$total passed=$passed failed=$failed profile=$PROFILE" >&2
echo "[asx] size-gate: archive=${archive_bytes}B text=${text_bytes}B data=${data_bytes}B bss=${bss_bytes}B" >&2
if [ "${#EXCLUDED_MEMBERS[@]}" -gt 0 ]; then
    echo "[asx] size-gate: shipped surface, left out ${EXCLUDED_MEMBERS[*]} (full archive: ${full_archive_bytes}B text=${full_text_bytes}B bss=${full_bss_bytes}B)" >&2
fi

if [ "$failed" -gt 0 ]; then
    echo "[asx] size-gate: violations:" >&2
    echo "$report" | jq -r '.violations[] | "  - \(.category)/\(.metric): actual=\(.actual) budget=\(.budget) delta=\(.delta_vs_budget)"' >&2
fi

# --- Exit code ---

if [ "$STRICT" = "1" ] && [ "$failed" -gt 0 ]; then
    echo "[asx] size-gate: FAIL (strict mode, $failed violations)" >&2
    exit 1
fi

exit 0
