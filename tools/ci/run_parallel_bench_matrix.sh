#!/usr/bin/env bash
# =============================================================================
# run_parallel_bench_matrix.sh -- PARALLEL profile worker-count benchmark matrix
#
# Runs bench_runtime across requested worker counts and emits one structured
# artifact that keeps resource-plane throughput separate from semantic parity.
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BENCH_BIN="$REPO_ROOT/build/bench/bench_runtime"
OUTPUT="$REPO_ROOT/build/perf/parallel_bench_matrix.json"
WORKERS="${ASX_PARALLEL_BENCH_WORKERS:-1 2 8 32 64}"
RUN_ID="${ASX_CI_RUN_TAG:-parallel-bench-$(date -u +%Y%m%dT%H%M%SZ)}"
COMMAND="make parallel-bench-json PROFILE=PARALLEL"
SEED="${ASX_BENCH_SEED:-0}"
GROSS_FACTOR="${ASX_PARALLEL_BENCH_GROSS_REGRESSION_FACTOR:-10}"
STRICT=0

usage() {
    cat <<'USAGE'
Usage: tools/ci/run_parallel_bench_matrix.sh [OPTIONS]

Options:
  --bench-bin <path>       bench_runtime binary (default: build/bench/bench_runtime)
  --output <path>          Aggregate JSON output (default: build/perf/parallel_bench_matrix.json)
  --workers "<list>"       Worker counts to run (default: "1 2 8 32 64")
  --run-id <id>            Run identifier (default: ASX_CI_RUN_TAG or timestamp)
  --command <text>         Provenance command recorded in the report
  --seed <u32>             Deterministic benchmark seed metadata (default: ASX_BENCH_SEED or 0)
  --gross-factor <number>  Observe-only gross regression factor vs worker_count=1 (default: 10)
  --strict                 Exit 1 on threshold alerts, not just metadata failures
  --help                   Show this help
USAGE
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bench-bin)
            [ $# -ge 2 ] || usage
            BENCH_BIN="$2"
            shift 2
            ;;
        --output)
            [ $# -ge 2 ] || usage
            OUTPUT="$2"
            shift 2
            ;;
        --workers)
            [ $# -ge 2 ] || usage
            WORKERS="$2"
            shift 2
            ;;
        --run-id)
            [ $# -ge 2 ] || usage
            RUN_ID="$2"
            shift 2
            ;;
        --command)
            [ $# -ge 2 ] || usage
            COMMAND="$2"
            shift 2
            ;;
        --seed)
            [ $# -ge 2 ] || usage
            SEED="$2"
            shift 2
            ;;
        --gross-factor)
            [ $# -ge 2 ] || usage
            GROSS_FACTOR="$2"
            shift 2
            ;;
        --strict)
            STRICT=1
            shift
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo "[asx] parallel-bench: ERROR unknown option: $1" >&2
            usage
            ;;
    esac
done

if ! command -v jq >/dev/null 2>&1; then
    echo "[asx] parallel-bench: ERROR jq is required" >&2
    exit 2
fi

if [ ! -x "$BENCH_BIN" ]; then
    echo "[asx] parallel-bench: ERROR bench binary not executable: $BENCH_BIN" >&2
    exit 2
fi

case "$GROSS_FACTOR" in
    ''|*[!0-9.]*)
        echo "[asx] parallel-bench: ERROR invalid --gross-factor: $GROSS_FACTOR" >&2
        exit 2
        ;;
esac

case "$SEED" in
    ''|*[!0-9]*)
        echo "[asx] parallel-bench: ERROR invalid --seed: $SEED" >&2
        exit 2
        ;;
esac

output_dir="$(dirname "$OUTPUT")"
run_dir="$output_dir/parallel_bench_runs"
mkdir -p "$run_dir"

run_files=()
for worker in $WORKERS; do
    case "$worker" in
        ''|*[!0-9]*)
            echo "[asx] parallel-bench: ERROR invalid worker count: $worker" >&2
            exit 2
            ;;
    esac

    run_file="$run_dir/worker-${worker}.json"
    echo "[asx] parallel-bench: worker_count=$worker -> $run_file" >&2
    ASX_BENCH_PARALLEL_WORKERS="$worker" ASX_BENCH_SEED="$SEED" "$BENCH_BIN" --json >"$run_file"
    jq empty "$run_file" >/dev/null
    run_files+=("$run_file")
done

git_commit="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || printf 'unknown')"
hostname_value="$(hostname 2>/dev/null || printf 'unknown')"
uname_value="$(uname -a 2>/dev/null || printf 'unknown')"
generated_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

jq -s \
    --arg schema "asx.parallel_bench_matrix.v1" \
    --arg run_id "$RUN_ID" \
    --arg generated_at "$generated_at" \
    --arg command "$COMMAND" \
    --arg git_commit "$git_commit" \
    --arg hostname "$hostname_value" \
    --arg uname "$uname_value" \
    --arg workers "$WORKERS" \
    --arg strict "$STRICT" \
    --argjson gross_factor "$GROSS_FACTOR" \
    '
def required_missing($run):
    [
        (if (($run.run_metadata.schema // "") != "asx.bench_run_metadata.v1") then "run_metadata.schema" else empty end),
        (if (($run.run_metadata.compiler.name // "") == "") then "run_metadata.compiler.name" else empty end),
        (if (($run.run_metadata.compiler.version // "") == "") then "run_metadata.compiler.version" else empty end),
        (if (($run.run_metadata.target.arch // "") == "") then "run_metadata.target.arch" else empty end),
        (if (($run.run_metadata.target.pointer_width // 0) == 0) then "run_metadata.target.pointer_width" else empty end),
        (if (($run.run_metadata.codec // "") == "") then "run_metadata.codec" else empty end),
        (if (($run.run_metadata.scenario_set // "") == "") then "run_metadata.scenario_set" else empty end),
        (if (($run.run_metadata.seed // null) == null) then "run_metadata.seed" else empty end),
        (if (($run.run_metadata.runtime_config.asx_max_workers // 0) == 0) then "run_metadata.runtime_config.asx_max_workers" else empty end),
        (if (($run.parallel_report.requested_worker_count // 0) == 0) then "parallel_report.requested_worker_count" else empty end),
        (if (($run.parallel_report.worker_count // 0) == 0) then "parallel_report.worker_count" else empty end),
        (if (($run.benchmarks.parallel_large_swarm.p99_ns // null) == null) then "benchmarks.parallel_large_swarm.p99_ns" else empty end)
    ];

def run_record($run):
    {
        worker_count_requested: ($run.parallel_report.requested_worker_count // $run.run_metadata.runtime_config.parallel_worker_request // null),
        worker_count_active: ($run.parallel_report.worker_count // null),
        supported: ($run.parallel_report.worker_count_supported // (($run.parallel_report.worker_count // null) == ($run.parallel_report.requested_worker_count // null))),
        profile: ($run.profile // ""),
        codec: ($run.run_metadata.codec // ""),
        deterministic: ($run.deterministic // null),
        metadata: ($run.run_metadata // {}),
        resource_plane_metrics: {
            benchmarks: ($run.benchmarks // {}),
            deadline_report: ($run.deadline_report // {}),
            parallel_report: ($run.parallel_report // {})
        },
        semantic_results: {
            status: "not_evaluated",
            gate: "parallel-parity",
            reason: "parallel benchmark matrix records resource-plane throughput only"
        },
        missing_required_metadata: required_missing($run)
    };

[.[] | run_record(.)] as $runs |
($runs | map(select(.supported == true and (.resource_plane_metrics.benchmarks.parallel_large_swarm.p99_ns // null) != null))) as $supported |
(($supported | map(select(.worker_count_requested == 1))[0]) // ($supported[0] // null)) as $baseline |
(if $baseline == null then []
 else [
    $supported[] | . as $run |
    ($baseline.resource_plane_metrics.benchmarks.parallel_large_swarm.p99_ns // 0) as $base_p99 |
    ($run.resource_plane_metrics.benchmarks.parallel_large_swarm.p99_ns // 0) as $actual_p99 |
    if ($base_p99 > 0 and $actual_p99 > ($base_p99 * $gross_factor)) then
        {
            metric: "parallel_large_swarm.p99_ns",
            baseline_worker_count: $baseline.worker_count_requested,
            worker_count: $run.worker_count_requested,
            baseline: $base_p99,
            actual: $actual_p99,
            gross_regression_factor: $gross_factor,
            ratio: ($actual_p99 / $base_p99)
        }
    else empty end
 ] end) as $alerts |
($runs | map(. as $run | $run.missing_required_metadata[]? | {worker_count_requested: $run.worker_count_requested, field: .})) as $metadata_failures |
{
    schema: $schema,
    run_id: $run_id,
    generated_at: $generated_at,
    command: $command,
    git_commit: $git_commit,
    host: {
        hostname: $hostname,
        uname: $uname
    },
    benchmark_domain_policy: {
        resource_plane_throughput: "runs[].resource_plane_metrics",
        semantic_results: "excluded from throughput verdicts",
        semantic_gate: "parallel-parity"
    },
    threshold_policy: {
        mode: "observe_only",
        strict: ($strict == "1"),
        gross_regression_factor: $gross_factor,
        baseline_worker_count: ($baseline.worker_count_requested // null),
        metrics: ["parallel_large_swarm.p99_ns"]
    },
    worker_counts_requested: ($workers | split(" ") | map(select(length > 0) | tonumber)),
    summary: {
        runs: ($runs | length),
        supported: ($runs | map(select(.supported == true)) | length),
        unsupported: ($runs | map(select(.supported != true)) | length),
        metadata_failures: ($metadata_failures | length),
        threshold_alerts: ($alerts | length)
    },
    metadata_failures: $metadata_failures,
    threshold_alerts: $alerts,
    runs: $runs,
    status: (if ($metadata_failures | length) > 0 then "fail" elif ($alerts | length) > 0 then "warn" else "pass" end)
}
' "${run_files[@]}" >"$OUTPUT"

status="$(jq -r '.status' "$OUTPUT")"
metadata_failures="$(jq -r '.summary.metadata_failures' "$OUTPUT")"
threshold_alerts="$(jq -r '.summary.threshold_alerts' "$OUTPUT")"

echo "[asx] parallel-bench: report written to $OUTPUT" >&2
echo "[asx] parallel-bench: status=$status metadata_failures=$metadata_failures threshold_alerts=$threshold_alerts" >&2

if [ "$metadata_failures" -gt 0 ]; then
    jq -r '.metadata_failures[] | "  - worker_count=\(.worker_count_requested): missing \(.field)"' "$OUTPUT" >&2
    exit 1
fi

if [ "$STRICT" = "1" ] && [ "$threshold_alerts" -gt 0 ]; then
    jq -r '.threshold_alerts[] | "  - \(.metric) worker_count=\(.worker_count): actual=\(.actual) baseline=\(.baseline) ratio=\(.ratio)"' "$OUTPUT" >&2
    exit 1
fi

exit 0
