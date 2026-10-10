#!/usr/bin/env bash
# =============================================================================
# run_evidence_dashboard.sh — evidence-ledger regression dashboard (bd-66l.4)
#
# Aggregates quality-gate artifacts into a historical evidence ledger and emits
# low-noise trend alerts for release-readiness decisions.
#
# Inputs (auto-discovered unless overridden):
#   - conformance summaries: tools/ci/artifacts/conformance/*.summary.json
#   - fuzz summaries:        tools/ci/artifacts/fuzz/*.summary.json
#   - bench JSON:            bench-results.json | build/perf/bench-results.json
#                            | build/bench/bench-results.json
#   - adaptive ledger JSON:  build/perf/evidence_ledger_*.json (optional)
#
# Outputs:
#   - build/perf/evidence_ledger_<run_id>.json
#   - build/perf/latency_trend_<run_id>.json
#   - build/perf/regression_dashboard_<run_id>.json
#   - tools/ci/artifacts/evidence/evidence_ledger.jsonl (append-only history)
#
# Exit codes:
#   0: dashboard generated (or no strict regression failure)
#   1: strict mode + regression alerts detected
#   2: usage/config error
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

RUN_ID="${ASX_CI_RUN_TAG:-evidence-$(date -u +%Y%m%dT%H%M%SZ)}"
GENERATED_TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

CONFORMANCE_DIR="$REPO_ROOT/tools/ci/artifacts/conformance"
FUZZ_DIR="$REPO_ROOT/tools/ci/artifacts/fuzz"
PERF_DIR="$REPO_ROOT/build/perf"
HISTORY_FILE="$REPO_ROOT/tools/ci/artifacts/evidence/evidence_ledger.jsonl"
DASHBOARD_FILE="$PERF_DIR/regression_dashboard_${RUN_ID}.json"
LEDGER_FILE="$PERF_DIR/evidence_ledger_${RUN_ID}.json"
TREND_FILE="$PERF_DIR/latency_trend_${RUN_ID}.json"
BENCH_JSON=""
ADAPTIVE_LEDGER_JSON=""
STRICT=0

usage() {
    cat <<'USAGE'
Usage: tools/ci/run_evidence_dashboard.sh [OPTIONS]

Options:
  --run-id <id>                 Override run id (default: ASX_CI_RUN_TAG or timestamp)
  --conformance-dir <dir>       Conformance summary directory
  --fuzz-dir <dir>              Fuzz summary directory
  --bench-json <file>           Benchmark JSON input file
  --adaptive-ledger-json <file> Adaptive ledger input file (optional)
  --history-file <file>         Append-only history JSONL file
  --dashboard-out <file>        Dashboard JSON output path
  --ledger-out <file>           Snapshot JSON output path
  --trend-out <file>            Trend JSON output path
  --strict                      Exit 1 when high-severity alerts exist
  --help                        Show this help
USAGE
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --run-id)
            [ $# -ge 2 ] || usage
            RUN_ID="$2"
            shift 2
            ;;
        --conformance-dir)
            [ $# -ge 2 ] || usage
            CONFORMANCE_DIR="$2"
            shift 2
            ;;
        --fuzz-dir)
            [ $# -ge 2 ] || usage
            FUZZ_DIR="$2"
            shift 2
            ;;
        --bench-json)
            [ $# -ge 2 ] || usage
            BENCH_JSON="$2"
            shift 2
            ;;
        --adaptive-ledger-json)
            [ $# -ge 2 ] || usage
            ADAPTIVE_LEDGER_JSON="$2"
            shift 2
            ;;
        --history-file)
            [ $# -ge 2 ] || usage
            HISTORY_FILE="$2"
            shift 2
            ;;
        --dashboard-out)
            [ $# -ge 2 ] || usage
            DASHBOARD_FILE="$2"
            shift 2
            ;;
        --ledger-out)
            [ $# -ge 2 ] || usage
            LEDGER_FILE="$2"
            shift 2
            ;;
        --trend-out)
            [ $# -ge 2 ] || usage
            TREND_FILE="$2"
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
            echo "[asx] evidence-dashboard: unknown argument '$1'" >&2
            usage
            ;;
    esac
done

mkdir -p "$(dirname "$DASHBOARD_FILE")" "$(dirname "$LEDGER_FILE")" \
         "$(dirname "$TREND_FILE")" "$(dirname "$HISTORY_FILE")"
touch "$HISTORY_FILE"

if [ -z "$BENCH_JSON" ]; then
    for candidate in \
        "$REPO_ROOT/bench-results.json" \
        "$PERF_DIR/bench-results.json" \
        "$REPO_ROOT/build/bench/bench-results.json"; do
        if [ -f "$candidate" ]; then
            BENCH_JSON="$candidate"
            break
        fi
    done
fi

if [ -z "$ADAPTIVE_LEDGER_JSON" ]; then
    ADAPTIVE_LEDGER_JSON="$(ls -1t "$PERF_DIR"/evidence_ledger_*.json 2>/dev/null | head -n 1 || true)"
fi

latest_mode_summary() {
    local mode="$1"
    local file=""
    for file in $(ls -1t "$CONFORMANCE_DIR"/*.summary.json 2>/dev/null || true); do
        if [ "$(jq -r '.mode // empty' "$file" 2>/dev/null)" = "$mode" ]; then
            echo "$file"
            return 0
        fi
    done
    return 1
}

read_json_or_null() {
    local file="$1"
    if [ -n "$file" ] && [ -f "$file" ]; then
        jq -c '.' "$file" 2>/dev/null || echo 'null'
    else
        echo 'null'
    fi
}

conformance_summary_file="$(latest_mode_summary "conformance" || true)"
codec_summary_file="$(latest_mode_summary "codec-equivalence" || true)"
profile_summary_file="$(latest_mode_summary "profile-parity" || true)"
fuzz_summary_file="$(ls -1t "$FUZZ_DIR"/*.summary.json 2>/dev/null | head -n 1 || true)"

conformance_json='null'
codec_json='null'
profile_json='null'
fuzz_json='null'
bench_metrics_json='null'
adaptive_ledger_json='null'

if [ -n "$conformance_summary_file" ]; then
    conformance_json="$(jq -c '{
        mode: .mode,
        run_id: .run_id,
        pass: (.pass // 0),
        fail: (.fail // 0),
        skip: (.skip // 0),
        semantic_delta_count: (.semantic_delta_count // 0),
        semantic_delta_pass: (.semantic_delta_pass // true),
        report_file: (.report_file // null),
        summary_file: (.summary_file // null)
    }' "$conformance_summary_file")"
fi

if [ -n "$codec_summary_file" ]; then
    codec_json="$(jq -c '{
        mode: .mode,
        run_id: .run_id,
        pass: (.pass // 0),
        fail: (.fail // 0),
        skip: (.skip // 0),
        parity_records: (.parity_records // 0),
        diff_records: (.diff_records // 0),
        report_file: (.report_file // null),
        summary_file: (.summary_file // null)
    }' "$codec_summary_file")"
fi

if [ -n "$profile_summary_file" ]; then
    profile_json="$(jq -c '{
        mode: .mode,
        run_id: .run_id,
        pass: (.pass // 0),
        fail: (.fail // 0),
        skip: (.skip // 0),
        semantic_delta_count: (.semantic_delta_count // 0),
        semantic_delta_pass: (.semantic_delta_pass // true),
        adapter_isomorphism_pass: (.adapter_isomorphism_pass // null),
        report_file: (.report_file // null),
        summary_file: (.summary_file // null)
    }' "$profile_summary_file")"
fi

if [ -n "$fuzz_summary_file" ]; then
    fuzz_json="$(jq -c '{
        run_id: (.run_id // null),
        mode: (.mode // null),
        iterations: (.iterations // 0),
        determinism_failures: (.determinism_failures // 0),
        crashes: (.crashes // 0),
        mismatch_count: (.mismatch_count // 0),
        duration_sec: (.duration_sec // null),
        report_file: (.report_file // null)
    }' "$fuzz_summary_file")"
fi

if [ -n "$BENCH_JSON" ] && [ -f "$BENCH_JSON" ]; then
    bench_metrics_json="$(jq -c '
        {
          profile: (.profile // "unknown"),
          deterministic: (.deterministic // null),
          scheduler_tail: (
            .benchmarks.scheduler_multi_round
            // .benchmarks.scheduler_multi_task
            // .benchmarks.scheduler_single_task
            // null
          ),
          deadline: (.deadline_report // null),
          adaptive: (.adaptive_report // null)
        }
    ' "$BENCH_JSON" 2>/dev/null || echo 'null')"
fi

if [ -n "$ADAPTIVE_LEDGER_JSON" ] && [ -f "$ADAPTIVE_LEDGER_JSON" ]; then
    adaptive_ledger_json="$(jq -c '{
        fallback_rate: (.fallback_rate // .metrics.fallback_rate // null),
        fallback_exercise_count: (
            .fallback_exercise_count
            // .metrics.fallback_exercise_count
            // null
        ),
        mean_confidence_fp32: (
            .mean_confidence_fp32
            // .metrics.mean_confidence_fp32
            // null
        ),
        mean_expected_loss_fp16: (
            .mean_expected_loss_fp16
            // .metrics.mean_expected_loss_fp16
            // null
        ),
        source_kind: (.kind // "external")
    }' "$ADAPTIVE_LEDGER_JSON" 2>/dev/null || echo 'null')"
fi

adaptive_metrics_json="$(jq -cn \
    --argjson bench "$bench_metrics_json" \
    --argjson ledger "$adaptive_ledger_json" '
    if ($bench | type) == "object" and $bench.adaptive != null then
      $bench.adaptive
    else
      $ledger
    end
')"

git_sha="$(git -C "$REPO_ROOT" rev-parse --verify HEAD 2>/dev/null || echo "unknown")"

snapshot_json="$(jq -cn \
    --arg run_id "$RUN_ID" \
    --arg generated_ts "$GENERATED_TS" \
    --arg git_sha "$git_sha" \
    --arg conformance_file "$conformance_summary_file" \
    --arg codec_file "$codec_summary_file" \
    --arg profile_file "$profile_summary_file" \
    --arg fuzz_file "$fuzz_summary_file" \
    --arg bench_file "$BENCH_JSON" \
    --arg adaptive_file "$ADAPTIVE_LEDGER_JSON" \
    --argjson conformance "$conformance_json" \
    --argjson codec "$codec_json" \
    --argjson profile "$profile_json" \
    --argjson fuzz "$fuzz_json" \
    --argjson bench "$bench_metrics_json" \
    --argjson adaptive "$adaptive_metrics_json" '
    {
      kind: "evidence_snapshot",
      run_id: $run_id,
      generated_ts: $generated_ts,
      git_sha: $git_sha,
      traceability_refs: ["bd-66l.4", "TRC-GSM-003", "TG-CONF", "TG-PROFILE", "TG-FUZZ"],
      sources: {
        conformance_summary: (if $conformance_file == "" then null else $conformance_file end),
        codec_summary: (if $codec_file == "" then null else $codec_file end),
        profile_parity_summary: (if $profile_file == "" then null else $profile_file end),
        fuzz_summary: (if $fuzz_file == "" then null else $fuzz_file end),
        bench_json: (if $bench_file == "" then null else $bench_file end),
        adaptive_ledger_json: (if $adaptive_file == "" then null else $adaptive_file end)
      },
      gates: {
        conformance: $conformance,
        codec_equivalence: $codec,
        profile_parity: $profile,
        fuzz: $fuzz
      },
      perf: (
        if ($bench | type) == "object" then
          {
            profile: ($bench.profile // null),
            deterministic: ($bench.deterministic // null),
            tail: ($bench.scheduler_tail // null),
            deadline: ($bench.deadline // null)
          }
        else
          null
        end
      ),
      adaptive: $adaptive
    }')"

if ! grep -Eq "\"run_id\"[[:space:]]*:[[:space:]]*\"$RUN_ID\"" "$HISTORY_FILE"; then
    printf '%s\n' "$snapshot_json" >> "$HISTORY_FILE"
fi

history_json="$(jq -s 'map(select(type == "object"))' "$HISTORY_FILE" 2>/dev/null || echo '[]')"

printf '%s\n' "$history_json" | jq 'map({
    run_id,
    generated_ts,
    scheduler_p50_ns: (.perf.tail.p50_ns // null),
    scheduler_p95_ns: (.perf.tail.p95_ns // null),
    scheduler_p99_ns: (.perf.tail.p99_ns // null),
    scheduler_p99_9_ns: (.perf.tail.p99_9_ns // null),
    scheduler_p99_99_ns: (.perf.tail.p99_99_ns // null),
    scheduler_jitter_ns: (.perf.tail.jitter_ns // null),
    deadline_miss_rate: (.perf.deadline.miss_rate // null),
    bench_artifact: (.sources.bench_json // null)
})' > "$TREND_FILE"

dashboard_json="$(jq -cn \
    --arg run_id "$RUN_ID" \
    --arg generated_ts "$GENERATED_TS" \
    --arg history_file "$HISTORY_FILE" \
    --arg trend_file "$TREND_FILE" \
    --arg ledger_file "$LEDGER_FILE" \
    --arg dashboard_file "$DASHBOARD_FILE" \
    --argjson history "$history_json" '
    def median:
      if length == 0 then null
      else (sort | .[(length - 1) / 2 | floor])
      end;
    def percent_delta(curr; base):
      if base == null or base == 0 then null else ((curr - base) / base) end;
    def alert_up(metric; curr; base; min_abs; min_ratio; severity; artifact):
      if curr == null or base == null then empty
      elif (curr - base) >= min_abs and (
             (base == 0 and curr > 0) or
             (base > 0 and ((curr - base) / base) >= min_ratio)
           )
      then {
        metric: metric,
        severity: severity,
        direction: "up",
        current: curr,
        baseline: base,
        absolute_delta: (curr - base),
        relative_delta: percent_delta(curr; base),
        artifact: artifact
      }
      else empty
      end;
    def alert_down(metric; curr; base; min_abs; min_ratio; severity; artifact):
      if curr == null or base == null then empty
      elif (base - curr) >= min_abs and (
             base > 0 and ((base - curr) / base) >= min_ratio
           )
      then {
        metric: metric,
        severity: severity,
        direction: "down",
        current: curr,
        baseline: base,
        absolute_delta: (base - curr),
        relative_delta: (if base == 0 then null else ((base - curr) / base) end),
        artifact: artifact
      }
      else empty
      end;

    ($history | sort_by(.generated_ts)) as $h |
    ($h | length) as $count |
    ($h[-1] // null) as $latest |
    ($h[0:-1]) as $prev |
    ($prev | map(.perf.tail.p99_ns // null) | map(select(. != null)) | .[-20:]) as $prev_p99 |
    ($prev | map(.perf.tail.p99_9_ns // null) | map(select(. != null)) | .[-20:]) as $prev_p99_9 |
    ($prev | map(.perf.tail.p99_99_ns // null) | map(select(. != null)) | .[-20:]) as $prev_p99_99 |
    ($prev | map(.perf.tail.jitter_ns // null) | map(select(. != null)) | .[-20:]) as $prev_jitter |
    ($prev | map(.perf.deadline.miss_rate // null) | map(select(. != null)) | .[-20:]) as $prev_deadline |
    ($prev | map(.adaptive.fallback_rate // null) | map(select(. != null)) | .[-20:]) as $prev_fallback_rate |
    ($prev | map(.adaptive.mean_confidence_fp32 // null) | map(select(. != null)) | .[-20:]) as $prev_confidence |
    ($prev | map(.adaptive.mean_expected_loss_fp16 // null) | map(select(. != null)) | .[-20:]) as $prev_expected_loss |
    ($latest.perf.tail.p99_ns // null) as $curr_p99 |
    ($latest.perf.tail.p99_9_ns // null) as $curr_p99_9 |
    ($latest.perf.tail.p99_99_ns // null) as $curr_p99_99 |
    ($latest.perf.tail.jitter_ns // null) as $curr_jitter |
    ($latest.perf.deadline.miss_rate // null) as $curr_deadline |
    ($latest.adaptive.fallback_rate // null) as $curr_fallback_rate |
    ($latest.adaptive.mean_confidence_fp32 // null) as $curr_confidence |
    ($latest.adaptive.mean_expected_loss_fp16 // null) as $curr_expected_loss |
    ($latest.sources.bench_json // null) as $bench_artifact |
    {
      kind: "evidence_regression_dashboard",
      run_id: $run_id,
      generated_ts: $generated_ts,
      policy: {
        low_noise_min_samples: 5,
        baseline_window: 20
      },
      history: {
        file: $history_file,
        entries: $count
      },
      latest_snapshot: $latest,
      trend_artifact: $trend_file,
      alerts: (
        [
          (if ($latest.gates.conformance.fail // 0) > 0 then
             {metric: "conformance_failures", severity: "high", current: ($latest.gates.conformance.fail // 0), baseline: 0, artifact: ($latest.sources.conformance_summary // null)}
           else empty end),
          (if ($latest.gates.conformance.semantic_delta_count // 0) > 0 then
             {metric: "semantic_delta_count", severity: "high", current: ($latest.gates.conformance.semantic_delta_count // 0), baseline: 0, artifact: ($latest.gates.conformance.summary_file // null)}
           else empty end),
          (if ($latest.gates.profile_parity.fail // 0) > 0 then
             {metric: "profile_parity_failures", severity: "high", current: ($latest.gates.profile_parity.fail // 0), baseline: 0, artifact: ($latest.sources.profile_parity_summary // null)}
           else empty end),
          (if ($latest.gates.fuzz.crashes // 0) > 0 then
             {metric: "fuzz_crashes", severity: "high", current: ($latest.gates.fuzz.crashes // 0), baseline: 0, artifact: ($latest.sources.fuzz_summary // null)}
           else empty end),
          (if ($latest.gates.fuzz.determinism_failures // 0) > 0 then
             {metric: "fuzz_determinism_failures", severity: "high", current: ($latest.gates.fuzz.determinism_failures // 0), baseline: 0, artifact: ($latest.sources.fuzz_summary // null)}
           else empty end),
          (if ($latest.gates.fuzz.mismatch_count // 0) > 0 then
             {metric: "fuzz_mismatch_count", severity: "high", current: ($latest.gates.fuzz.mismatch_count // 0), baseline: 0, artifact: ($latest.sources.fuzz_summary // null)}
           else empty end)
        ]
        + (if ($prev_p99 | length) >= 5 then
             [alert_up("scheduler_p99_ns"; $curr_p99; ($prev_p99 | median); 500; 0.15; "warn"; $bench_artifact)]
           else [] end)
        + (if ($prev_p99_9 | length) >= 5 then
             [alert_up("scheduler_p99_9_ns"; $curr_p99_9; ($prev_p99_9 | median); 800; 0.18; "warn"; $bench_artifact)]
           else [] end)
        + (if ($prev_p99_99 | length) >= 5 then
             [alert_up("scheduler_p99_99_ns"; $curr_p99_99; ($prev_p99_99 | median); 1200; 0.20; "warn"; $bench_artifact)]
           else [] end)
        + (if ($prev_jitter | length) >= 5 then
             [alert_up("scheduler_jitter_ns"; $curr_jitter; ($prev_jitter | median); 400; 0.20; "warn"; $bench_artifact)]
           else [] end)
        + (if ($prev_deadline | length) >= 5 then
             [alert_up("deadline_miss_rate"; $curr_deadline; ($prev_deadline | median); 0.001; 0.25; "warn"; $bench_artifact)]
           else [] end)
        + (if ($prev_fallback_rate | length) >= 5 then
             [alert_up("adaptive_fallback_rate"; $curr_fallback_rate; ($prev_fallback_rate | median); 0.02; 0.30; "warn"; ($latest.sources.adaptive_ledger_json // $bench_artifact))]
           else [] end)
        + (if ($prev_confidence | length) >= 5 then
             [alert_down("adaptive_mean_confidence_fp32"; $curr_confidence; ($prev_confidence | median); 100000000; 0.10; "warn"; ($latest.sources.adaptive_ledger_json // $bench_artifact))]
           else [] end)
        + (if ($prev_expected_loss | length) >= 5 then
             [alert_up("adaptive_mean_expected_loss_fp16"; $curr_expected_loss; ($prev_expected_loss | median); 5000; 0.15; "warn"; ($latest.sources.adaptive_ledger_json // $bench_artifact))]
           else [] end)
      )
    }
    | .alert_counts = {
        high: (.alerts | map(select(.severity == "high")) | length),
        warn: (.alerts | map(select(.severity == "warn")) | length)
      }
    | .release_readiness = (
        if .alert_counts.high > 0 then "fail"
        elif .alert_counts.warn > 0 then "warn"
        else "pass"
        end
      )
    | .artifacts = {
        snapshot: $ledger_file,
        dashboard: $dashboard_file,
        trend: $trend_file
      }
')"

printf '%s\n' "$dashboard_json" > "$DASHBOARD_FILE"

jq -cn \
  --arg run_id "$RUN_ID" \
  --arg generated_ts "$GENERATED_TS" \
  --arg history_file "$HISTORY_FILE" \
  --arg dashboard_file "$DASHBOARD_FILE" \
  --arg trend_file "$TREND_FILE" \
  --argjson snapshot "$snapshot_json" \
  --argjson dashboard "$dashboard_json" '
  {
    kind: "evidence_ledger_entry",
    run_id: $run_id,
    generated_ts: $generated_ts,
    history_file: $history_file,
    dashboard_file: $dashboard_file,
    trend_file: $trend_file,
    snapshot: $snapshot,
    release_readiness: $dashboard.release_readiness,
    alert_counts: $dashboard.alert_counts
  }' > "$LEDGER_FILE"

alerts_total="$(jq -r '.alerts | length' "$DASHBOARD_FILE")"
alerts_high="$(jq -r '.alert_counts.high' "$DASHBOARD_FILE")"
status="$(jq -r '.release_readiness' "$DASHBOARD_FILE")"

echo "[asx] evidence-dashboard: status=$status alerts=$alerts_total high=$alerts_high dashboard=$DASHBOARD_FILE" >&2
echo "[asx] evidence-dashboard: ledger=$LEDGER_FILE trend=$TREND_FILE history=$HISTORY_FILE" >&2

if [ "$alerts_total" -gt 0 ]; then
    jq -r '.alerts[] | "  - [" + .severity + "] " + .metric + " current=" + (.current|tostring) + " baseline=" + (.baseline|tostring)' "$DASHBOARD_FILE" >&2
fi

if [ "$STRICT" = "1" ] && [ "$alerts_high" -gt 0 ]; then
    echo "[asx] evidence-dashboard: FAIL (strict mode with high-severity alerts)" >&2
    exit 1
fi

exit 0
