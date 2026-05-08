#!/usr/bin/env bash
# =============================================================================
# evaluate_slo_gates.sh — golden performance SLO gate evaluator (bd-66l.5)
#
# Compares benchmark results against per-profile SLO baselines and emits
# structured pass/fail reports with budget deltas and culprit identification.
#
# Usage:
#   tools/ci/evaluate_slo_gates.sh --bench-json <file> [--baselines <file>]
#       [--profile <name>] [--command <command>] [--run-id <id>]
#       [--strict] [--output <file>]
#
# Exit codes:
#   0: all SLO gates pass
#   1: one or more SLO gates violated
#   2: usage/configuration error
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BENCH_JSON=""
BASELINES="${SCRIPT_DIR}/slo_baselines.json"
PROFILE=""
RUN_ID="${ASX_CI_RUN_TAG:-slo-$(date -u +%Y%m%dT%H%M%SZ)}"
STRICT=0
OUTPUT=""
BENCH_COMMAND=""

usage() {
    cat <<'USAGE'
Usage: tools/ci/evaluate_slo_gates.sh [OPTIONS]

Required:
  --bench-json <file>     Benchmark JSON results file (from make bench-json)

Options:
  --baselines <file>      SLO baselines file (default: tools/ci/slo_baselines.json)
  --profile <name>        Override profile (default: read from bench JSON)
  --command <command>     Benchmark command to validate against baseline metadata
  --run-id <id>           Run identifier (default: ASX_CI_RUN_TAG or timestamp)
  --strict                Exit 1 on any SLO violation (default: always exit 0)
  --output <file>         Write gate report JSON to file (default: stdout)
  --help                  Show this help
USAGE
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
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
        --command)
            [ $# -ge 2 ] || usage
            BENCH_COMMAND="$2"
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
        --help|-h)
            usage
            ;;
        *)
            echo "[asx] slo-gate: ERROR — unknown option: $1" >&2
            usage
            ;;
    esac
done

# --- Validate inputs ---

if [ -z "$BENCH_JSON" ]; then
    echo "[asx] slo-gate: ERROR — --bench-json is required" >&2
    usage
fi

if [ ! -f "$BENCH_JSON" ]; then
    echo "[asx] slo-gate: ERROR — bench JSON not found: $BENCH_JSON" >&2
    exit 2
fi

if [ ! -f "$BASELINES" ]; then
    echo "[asx] slo-gate: ERROR — baselines file not found: $BASELINES" >&2
    exit 2
fi

# --- Detect profile ---

if [ -z "$PROFILE" ]; then
    PROFILE="$(jq -r '.profile // "CORE"' "$BENCH_JSON")"
fi

# --- Evaluate SLO gates ---

report=$(jq -n \
    --slurpfile bench "$BENCH_JSON" \
    --slurpfile base "$BASELINES" \
    --arg profile "$PROFILE" \
    --arg bench_command "$BENCH_COMMAND" \
    --arg run_id "$RUN_ID" \
    --arg bench_file "$BENCH_JSON" \
    --arg baselines_file "$BASELINES" \
    --arg ts "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --argjson now_epoch "$(date -u +%s)" \
'
def present:
    . != null and
    (if type == "string" then length > 0
     elif type == "object" or type == "array" then length > 0
     else true
     end);

def validation_check(name; passed; detail):
    {check: name, passed: passed} + detail;

def command_matches(meta; command):
    command == "" or
    ((meta.command // "") == command) or
    (((meta.accepted_commands // []) | index(command)) != null);

def check_upper(bench_name; metric_name; actual; budget_obj):
    {
        benchmark: bench_name,
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

def check_lower(bench_name; metric_name; actual; budget_obj):
    {
        benchmark: bench_name,
        metric: metric_name,
        actual: actual,
        budget: budget_obj.budget,
        baseline: budget_obj.baseline,
        headroom: budget_obj.headroom,
        delta_vs_budget: (budget_obj.budget - actual),
        delta_vs_baseline: (budget_obj.baseline - actual),
        pct_of_budget: (if budget_obj.budget > 0 then ((actual / budget_obj.budget) * 100 | round) else 0 end),
        passed: (actual >= budget_obj.budget)
    };

def latency_delta(bench_name; metric_name; actual; budget_obj):
    (budget_obj.baseline // null) as $baseline |
    {
        benchmark: bench_name,
        metric: metric_name,
        actual: actual,
        baseline: $baseline,
        budget: (budget_obj.budget // null),
        baseline_available: ($baseline != null),
        delta_vs_baseline: (if $baseline != null then actual - $baseline else null end),
        pct_delta_vs_baseline: (
            if $baseline != null and $baseline != 0 then
                ((((actual - $baseline) / $baseline) * 10000) | round) / 100
            else
                null
            end
        )
    };

($bench[0]) as $b |
($base[0].profiles[$profile] // null) as $p |
($p.baseline_metadata // {}) as $meta |
($base[0].governance.baseline_validation.required_metadata_fields //
    ["git_commit", "profile", "compiler", "target", "command", "scenario_set", "captured_at", "threshold_policy"]) as $required_fields |
($required_fields | map(select((($meta[.] // null) | present) | not))) as $missing_metadata |
($meta.max_age_days // $base[0].governance.baseline_validation.max_age_days // 120) as $max_age_days |
($meta.captured_at // $p.captured_at // null) as $captured_at |
($captured_at | if . == null then null else (try fromdateiso8601 catch null) end) as $captured_epoch |
(if $captured_epoch == null then null else (($now_epoch - $captured_epoch) / 86400 | floor) end) as $age_days |

[
    validation_check(
        "profile_baseline_exists";
        ($p != null);
        {expected_profile: $profile}
    ),
    validation_check(
        "benchmark_report_has_metrics";
        ((($b.benchmarks // null) | type) == "object" and (($b.benchmarks // {}) | length) > 0);
        {benchmarks_present: (($b.benchmarks // null) | type)}
    ),
    validation_check(
        "benchmark_profile_matches";
        (($b.profile // null) == $profile);
        {expected_profile: $profile, actual_profile: ($b.profile // null)}
    ),
    validation_check(
        "baseline_metadata_complete";
        (($missing_metadata | length) == 0);
        {missing_fields: $missing_metadata, required_fields: $required_fields}
    ),
    validation_check(
        "baseline_metadata_profile_matches";
        (($meta.profile // null) == $profile);
        {expected_profile: $profile, metadata_profile: ($meta.profile // null)}
    ),
    validation_check(
        "baseline_not_stale";
        ($captured_epoch != null and $age_days <= $max_age_days);
        {
            captured_at: $captured_at,
            age_days: $age_days,
            max_age_days: $max_age_days
        }
    ),
    validation_check(
        "benchmark_command_matches";
        command_matches($meta; $bench_command);
        {
            expected_command: ($meta.command // null),
            accepted_commands: ($meta.accepted_commands // []),
            actual_command: (if $bench_command == "" then null else $bench_command end),
            enforced: ($bench_command != "")
        }
    )
] as $validation_checks |

# Check benchmark SLOs
[
    # For each benchmark in the baselines
    ($p.benchmarks // {} | to_entries[] | . as $bench_entry |
        $bench_entry.value | to_entries[] | . as $metric_entry |
        ($b.benchmarks[$bench_entry.key][$metric_entry.key] // null) as $actual |
        if $actual != null then
            check_upper($bench_entry.key; $metric_entry.key; $actual; $metric_entry.value)
        else
            empty
        end
    ),

    # Deadline report checks (upper bound)
    (if ($p.deadline_report // null) != null and ($b.deadline_report // null) != null then
        ($p.deadline_report | to_entries[] |
            . as $entry |
            ($b.deadline_report[$entry.key] // null) as $actual |
            if $actual != null then
                check_upper("deadline_report"; $entry.key; $actual; $entry.value)
            else
                empty
            end
        )
    else
        empty
    end),

    # Adaptive report checks
    (if ($p.adaptive_report // null) != null and ($b.adaptive_report // null) != null then
        ($p.adaptive_report | to_entries[] |
            . as $entry |
            (if $entry.key == "mean_confidence_fp32_floor" then
                ($b.adaptive_report.mean_confidence_fp32 // null)
            elif $entry.key == "fallback_rate" then
                ($b.adaptive_report.fallback_rate // null)
            else
                null
            end) as $actual |
            if $actual != null then
                if ($entry.value.direction // "upper") == "lower" then
                    check_lower("adaptive_report"; $entry.key; $actual; $entry.value)
                else
                    check_upper("adaptive_report"; $entry.key; $actual; $entry.value)
                end
            else
                empty
            end
        )
    else
        empty
    end)
] as $checks |

[
    (($b.benchmarks // {}) | to_entries[] |
        . as $bench_entry |
        (["p50_ns", "p95_ns", "p99_ns", "p99_9_ns"][] |
            . as $metric_name |
            ($bench_entry.value[$metric_name] // null) as $actual |
            if $actual != null then
                latency_delta(
                    $bench_entry.key;
                    $metric_name;
                    $actual;
                    ($p.benchmarks[$bench_entry.key][$metric_name] // {})
                )
            else
                empty
            end
        )
    )
] as $latency_deltas |

($checks | map(select(.passed)) | length) as $passed |
($checks | map(select(.passed | not)) | length) as $failed |
($validation_checks | map(select(.passed)) | length) as $validation_passed |
($validation_checks | map(select(.passed | not)) | length) as $validation_failed |

{
    schema: "asx.slo_gate_report.v1",
    run_id: $run_id,
    profile: $profile,
    generated_at: $ts,
    sources: {
        bench_json: $bench_file,
        baselines: $baselines_file
    },
    status: (if ($failed + $validation_failed) > 0 then "fail" else "pass" end),
    summary: {
        total: (($checks | length) + ($validation_checks | length)),
        passed: ($passed + $validation_passed),
        failed: ($failed + $validation_failed),
        slo_total: ($checks | length),
        slo_failed: $failed,
        validation_total: ($validation_checks | length),
        validation_failed: $validation_failed,
        skipped: 0
    },
    baseline_validation: {
        status: (if $validation_failed > 0 then "fail" else "pass" end),
        checks: $validation_checks,
        metadata: $meta
    },
    evaluation_domains: {
        resource_plane_throughput: {
            evaluated: true,
            metric_count: ($checks | length),
            latency_delta_metrics: ["p50_ns", "p95_ns", "p99_ns", "p99_9_ns"]
        },
        semantic_results: {
            evaluated: false,
            reason: "SLO gates cover resource-plane throughput only; semantic identity is enforced by profile-parity and conformance gates."
        }
    },
    latency_deltas: $latency_deltas,
    violations: ($checks | map(select(.passed | not)) | sort_by(.delta_vs_budget) | reverse),
    validation_failures: ($validation_checks | map(select(.passed | not))),
    passes: ($checks | map(select(.passed)) | sort_by(.pct_of_budget) | reverse),
    worst_offenders: ($checks | map(select(.passed | not)) | sort_by(.delta_vs_budget) | reverse | .[0:5]),
    rerun_command: "make bench-json > bench-results.json && tools/ci/evaluate_slo_gates.sh --bench-json bench-results.json --strict"
}
')

# --- Output report ---

if [ -n "$OUTPUT" ]; then
    mkdir -p "$(dirname "$OUTPUT")"
    echo "$report" > "$OUTPUT"
    echo "[asx] slo-gate: report written to $OUTPUT" >&2
else
    echo "$report"
fi

# --- Summary ---

status="$(echo "$report" | jq -r '.status')"
total="$(echo "$report" | jq -r '.summary.total')"
passed="$(echo "$report" | jq -r '.summary.passed')"
failed="$(echo "$report" | jq -r '.summary.failed')"

echo "[asx] slo-gate: status=$status total=$total passed=$passed failed=$failed profile=$PROFILE" >&2

if [ "$failed" -gt 0 ]; then
    echo "[asx] slo-gate: violations:" >&2
    echo "$report" | jq -r '.violations[] | "  - \(.benchmark)/\(.metric): actual=\(.actual) budget=\(.budget) delta=\(.delta_vs_budget)"' >&2
    echo "$report" | jq -r '.validation_failures[] | "  - validation/\(.check): \(. | @json)"' >&2
fi

# --- Exit code ---

if [ "$STRICT" = "1" ] && [ "$failed" -gt 0 ]; then
    echo "[asx] slo-gate: FAIL (strict mode, $failed violations)" >&2
    exit 1
fi

exit 0
