#!/usr/bin/env bash
# test_perf_median.sh — controls for the performance gates' median-of-five
# statistic, the shipped-surface size measurement and the baseline re-capture
# (DEC-014, DEC-015, DEC-016; bd-9kll.14.2).
#
# Synthetic bench documents only: nothing here is a measurement. Each case
# runs the real script. The aggregation and the capture also run as a mutant
# (a copy of the script with one line changed by sed) which the same case
# must reject, so a control that cannot tell the difference is noticed.
#
#   aggregate_bench_runs.sh
#     - five runs, one outlier           -> the median, all five kept in order
#     - the same with `max` for the median -> a different value (control)
#     - an even count, a short list, mixed profiles -> refused (exit 2)
#   evaluate_slo_gates.sh on the aggregate
#     - one run of five over the budget  -> PASS, the report counts 1 over
#     - three of five over the budget    -> FAIL, the line names the median
#                                           and the five samples
#     - the max-aggregating mutant        -> the one-outlier case FAILS (so the
#                                           PASS above is the median's)
#   evaluate_size_gates.sh
#     - an archive with json.o, canon.o and interpreter.o of a large bss
#                                         -> measured without them, the full
#                                            archive recorded
#     - --include-conformance             -> measured with them
#   capture_baselines.sh
#     - every budget and direction unchanged, baselines/captured_at/host/commit
#       written, one audit entry
#     - a wrong profile, a missing commit -> refused (exit 2)
#     - the budget-changing mutant        -> exit 1, nothing written
#
# Scratch files live under build/test-gates/<run_id>/; tracked files are never
# touched and nothing is deleted. Logs JSONL to build/test-logs/gates.jsonl.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOLS="$REPO_ROOT/tools/ci"
RUN_ID="perf-median-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
mkdir -p "$WORK" "$LOG_DIR"

failures=0

log_case() {
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"perf_median","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# check <name> <expected> <observed> <detail>: pass when equal.
check() {
    local name="$1" expected="$2" observed="$3" detail="${4:-}" status="pass"
    if [ "$expected" != "$observed" ]; then status="fail"; failures=$((failures + 1)); fi
    if [ "$status" = "pass" ]; then
        echo "  PASS: $name ($observed)"
    else
        echo "  FAIL: $name (expected $expected, observed $observed; $detail)"
    fi
    log_case "$name" "$expected" "$observed" "$status" "$detail"
}

# A synthetic bench document: scheduler_single_task p99_99 = $1, cold start
# init_p99 = $2, the rest fixed.
bench_doc() {
    jq -n --argjson tail "$1" --argjson cold "$2" --arg profile "${3:-CORE}" '{
        version: "0.1.0", profile: $profile, deterministic: 1,
        bench_metadata: {schema: "asx.bench_runtime.v2", compiler: "gcc", compiler_version: "13.3.0",
                         host: {cpu_model: "SYNTHETIC cpu", os_release: "0.0.0-synthetic",
                                pointer_bits: 64, ci_sha: "0123456789abcdef0123456789abcdef01234567"}},
        benchmarks: {scheduler_single_task: {count: 10000, p50_ns: 200, p99_ns: 300,
                                             p99_9_ns: 400, p99_99_ns: $tail, jitter_ns: 20}},
        cold_start_report: {samples: 5000, init_p99_ns: $cold},
        deadline_report: {max_overshoot_ns: 0, miss_rate: 0},
        adaptive_report: {mean_confidence_fp32: 2641404886, fallback_rate: 0.5}
    }'
}

# Synthetic baselines: one benchmark metric with budget 1000, fresh capture.
cat >"$WORK/baselines.json" <<'EOF'
{
  "governance": {"approval_required": true,
                 "baseline_validation": {"max_age_days": 120},
                 "audit_trail": []},
  "profiles": {
    "CORE": {
      "captured_at": "2099-01-01T00:00:00Z",
      "baseline_metadata": {"git_commit": "x", "profile": "CORE", "compiler": "gcc",
        "target": "t", "command": "make bench-json", "accepted_commands": ["make bench-json"],
        "scenario_set": "s", "captured_at": "2099-01-01T00:00:00Z",
        "threshold_policy": {"name": "p"}, "max_age_days": 120},
      "benchmarks": {"scheduler_single_task": {"p99_99_ns": {"budget": 1000, "headroom": 5.0, "baseline": 200}}},
      "deadline_report": {"miss_rate": {"budget": 0.01, "headroom": 20.0, "baseline": 0.0005}},
      "adaptive_report": {"fallback_rate": {"budget": 0.8, "headroom": 1.6, "baseline": 0.5, "direction": "upper"}}
    },
    "HFT": {
      "captured_at": "2099-01-01T00:00:00Z",
      "baseline_metadata": {"git_commit": "y", "profile": "HFT"},
      "benchmarks": {"scheduler_single_task": {"p99_99_ns": {"budget": 777, "headroom": 5.0, "baseline": 150}}}
    }
  }
}
EOF

# Five runs whose p99_99 values are given; write them as <prefix>-run-N.json.
five_runs() {
    local prefix="$1" i=1
    shift
    for v in "$@"; do
        bench_doc "$v" 3000 >"${prefix}-run-${i}.json"
        i=$((i + 1))
    done
}

echo "=== test_perf_median ($RUN_ID) ==="

# --- aggregate_bench_runs.sh ---------------------------------------------
five_runs "$WORK/one_outlier" 900 950 960 20000 980
"$TOOLS/aggregate_bench_runs.sh" --output "$WORK/one_outlier.json" "$WORK"/one_outlier-run-{1,2,3,4,5}.json 2>/dev/null
check agg_median_value 960 "$(jq '.benchmarks.scheduler_single_task.p99_99_ns' "$WORK/one_outlier.json")"
check agg_keeps_all_five_in_run_order '[900,950,960,20000,980]' \
    "$(jq -c '.repeats.samples["benchmarks/scheduler_single_task/p99_99_ns"]' "$WORK/one_outlier.json")"
check agg_cold_start_is_judged_too 5 "$(jq '.repeats.samples["cold_start_report/init_p99_ns"] | length' "$WORK/one_outlier.json")"
check agg_records_runs_and_statistic '5 median' \
    "$(jq -r '"\(.repeats.runs) \(.repeats.statistic)"' "$WORK/one_outlier.json")"

# The control: the same script taking the largest value instead of the median.
sed 's/def med: sort | .\[(length \/ 2 | floor)\];/def med: sort | .[-1];/' \
    "$TOOLS/aggregate_bench_runs.sh" >"$WORK/aggregate_max.sh"
if cmp -s "$TOOLS/aggregate_bench_runs.sh" "$WORK/aggregate_max.sh"; then
    check agg_max_mutant_is_a_mutant changed unchanged "the sed did not change the script"
else
    bash "$WORK/aggregate_max.sh" --output "$WORK/one_outlier_max.json" "$WORK"/one_outlier-run-{1,2,3,4,5}.json 2>/dev/null
    check agg_max_mutant_differs 20000 "$(jq '.benchmarks.scheduler_single_task.p99_99_ns' "$WORK/one_outlier_max.json")"
fi

"$TOOLS/aggregate_bench_runs.sh" --output "$WORK/even.json" "$WORK"/one_outlier-run-{1,2,3,4}.json >/dev/null 2>&1
check agg_refuses_an_even_count 2 "$?"
"$TOOLS/aggregate_bench_runs.sh" --output "$WORK/short.json" "$WORK"/one_outlier-run-{1,2}.json >/dev/null 2>&1
check agg_refuses_fewer_than_three 2 "$?"
bench_doc 900 3000 HFT >"$WORK/hft-run.json"
"$TOOLS/aggregate_bench_runs.sh" --output "$WORK/mixed.json" "$WORK"/one_outlier-run-{1,2,3,4}.json "$WORK/hft-run.json" >/dev/null 2>&1
check agg_refuses_mixed_profiles 2 "$?"

# --- evaluate_slo_gates.sh on the aggregate ---------------------------------
slo() {
    "$TOOLS/evaluate_slo_gates.sh" --baselines "$WORK/baselines.json" --profile CORE \
        --bench-json "$1" --output "$2" --run-id perf-median --strict 2>"$2.stderr" >/dev/null
}
slo "$WORK/one_outlier.json" "$WORK/slo_one.json"
check slo_one_outlier_run_passes 0 "$?" "$WORK/slo_one.json.stderr"
check slo_report_counts_the_outlier 1 \
    "$(jq '[.passes[] | select(.metric == "p99_99_ns")][0].runs_over_budget' "$WORK/slo_one.json")"

five_runs "$WORK/three_over" 900 1100 1200 20000 980
"$TOOLS/aggregate_bench_runs.sh" --output "$WORK/three_over.json" "$WORK"/three_over-run-{1,2,3,4,5}.json 2>/dev/null
slo "$WORK/three_over.json" "$WORK/slo_three.json"
check slo_three_runs_over_fails 1 "$?" "$WORK/slo_three.json.stderr"
check slo_failure_names_the_median_and_samples 1 \
    "$(grep -c 'median of 5 runs: \[900,1100,1200,20000,980\]; 3 over budget' "$WORK/slo_three.json.stderr")"

slo "$WORK/one_outlier_max.json" "$WORK/slo_max.json"
check slo_max_aggregate_fails_the_one_outlier_case 1 "$?" "the control: a max aggregate must not pass"

# --- evaluate_size_gates.sh -------------------------------------------------
if command -v "${CC:-cc}" >/dev/null 2>&1 && command -v ar >/dev/null 2>&1; then
    SZ="$WORK/size"
    mkdir -p "$SZ"
    printf 'int core_fn(void){return 1;}\nchar core_table[1000];\n' >"$SZ/core.c"
    for m in json canon interpreter; do
        printf 'int %s_fn(void){return 2;}\nchar %s_table[100000];\n' "$m" "$m" >"$SZ/$m.c"
    done
    ok=1
    for m in core json canon interpreter; do
        "${CC:-cc}" -c -o "$SZ/$m.o" "$SZ/$m.c" || ok=0
    done
    ar rcs "$SZ/lib.a" "$SZ"/core.o "$SZ"/json.o "$SZ"/canon.o "$SZ"/interpreter.o || ok=0
    if [ "$ok" = "1" ]; then
        jq -n '{profiles: {CORE: {size_metrics: {
                  archive_bytes: {budget: 99999999, headroom: 1, baseline: 1},
                  text_bytes: {budget: 99999999, headroom: 1, baseline: 1},
                  data_bytes: {budget: 99999999, headroom: 1, baseline: 1},
                  bss_bytes: {budget: 99999999, headroom: 1, baseline: 1}}}}}' >"$SZ/size_baselines.json"
        "$TOOLS/evaluate_size_gates.sh" --lib "$SZ/lib.a" --baselines "$SZ/size_baselines.json" \
            --profile CORE --output "$SZ/shipped.json" --run-id perf-median >/dev/null 2>&1
        "$TOOLS/evaluate_size_gates.sh" --lib "$SZ/lib.a" --baselines "$SZ/size_baselines.json" \
            --profile CORE --include-conformance --output "$SZ/all.json" --run-id perf-median >/dev/null 2>&1
        check size_shipped_surface_leaves_out_the_interpreter 'shipped ["canon.o","interpreter.o","json.o"]' \
            "$(jq -c '"\(.measured_surface.kind) \(.measured_surface.excluded_members | sort | tojson)"' "$SZ/shipped.json" | tr -d '\\' | sed 's/^"//; s/"$//')"
        shipped_bss="$(jq '.measured.bss_bytes' "$SZ/shipped.json")"
        all_bss="$(jq '.measured.bss_bytes' "$SZ/all.json")"
        check size_shipped_bss_is_the_core_table_only "$([ "$shipped_bss" -ge 1000 ] && [ "$shipped_bss" -lt 100000 ] && echo yes || echo "no:$shipped_bss")" yes
        check size_full_archive_is_recorded "$all_bss" "$(jq '.measured_surface.full_archive.bss_bytes' "$SZ/shipped.json")"
        check size_include_conformance_measures_all full_archive "$(jq -r '.measured_surface.kind' "$SZ/all.json")"
        check size_include_conformance_bss_has_the_tables yes \
            "$([ "$all_bss" -ge 300000 ] && echo yes || echo "no:$all_bss")"
    else
        check size_fixture_builds yes no "the tiny archive did not build"
    fi
else
    echo "  SKIP: size cases (no C compiler or ar)"
fi

# --- capture_baselines.sh ---------------------------------------------------
bench_doc 900 3000 CORE >"$WORK/capture_bench.json"
capture() {
    "$TOOLS/capture_baselines.sh" --baselines "$WORK/baselines.json" --bench-json "$WORK/capture_bench.json" \
        --profile "${PROFILE:-CORE}" --run-id 123-1 --run-url https://example.invalid/run/123 \
        --decision DEC-014 --author control --captured-at 2026-10-13T12:00:00Z \
        --output "$1" "${@:2}" >/dev/null 2>&1
}
capture "$WORK/captured.json"
check capture_succeeds 0 "$?"
check capture_leaves_every_budget_and_direction \
    "$(jq -cS '[.. | objects | select(has("budget")) | {budget, direction}]' "$WORK/baselines.json")" \
    "$(jq -cS '[.. | objects | select(has("budget")) | {budget, direction}]' "$WORK/captured.json")"
check capture_writes_the_baseline_and_headroom '900 1.1' \
    "$(jq -r '.profiles.CORE.benchmarks.scheduler_single_task.p99_99_ns | "\(.baseline) \(.headroom)"' "$WORK/captured.json")"
check capture_records_host_commit_and_time \
    'SYNTHETIC cpu 0123456789abcdef0123456789abcdef01234567 2026-10-13T12:00:00Z' \
    "$(jq -r '.profiles.CORE.baseline_metadata | "\(.host.cpu_model) \(.git_commit) \(.captured_at)"' "$WORK/captured.json")"
check capture_appends_one_audit_entry 'recapture_baselines DEC-014 1' \
    "$(jq -r '.governance.audit_trail | "\(.[-1].action) \(.[-1].decision) \(length)"' "$WORK/captured.json")"
check capture_leaves_the_other_profile 'null' \
    "$(jq -c '.profiles.HFT.baseline_metadata.host' "$WORK/captured.json")"

PROFILE=HFT capture "$WORK/wrong_profile.json"
check capture_refuses_a_profile_the_bench_is_not 2 "$?"
jq 'del(.bench_metadata.host.ci_sha)' "$WORK/capture_bench.json" >"$WORK/capture_bench_nosha.json"
"$TOOLS/capture_baselines.sh" --baselines "$WORK/baselines.json" --bench-json "$WORK/capture_bench_nosha.json" \
    --profile CORE --run-id 1 --run-url u --decision DEC-014 --author control \
    --output "$WORK/nosha.json" >/dev/null 2>&1
check capture_refuses_a_run_with_no_commit 2 "$?"

# The control: a capture that also doubles each budget must be refused.
sed 's/else \.baseline = actual$/else .baseline = actual | .budget = (.budget * 2)/' \
    "$TOOLS/capture_baselines.sh" >"$WORK/capture_budget.sh"
if cmp -s "$TOOLS/capture_baselines.sh" "$WORK/capture_budget.sh"; then
    check capture_budget_mutant_is_a_mutant changed unchanged "the sed did not change the script"
else
    SCRIPT_DIR_OVERRIDE=1 bash "$WORK/capture_budget.sh" --baselines "$WORK/baselines.json" \
        --bench-json "$WORK/capture_bench.json" --profile CORE --run-id 1 --run-url u \
        --decision DEC-014 --author control --output "$WORK/budget_mutant.json" >/dev/null 2>&1
    rc=$?
    exists=no
    [ -f "$WORK/budget_mutant.json" ] && exists=yes
    check capture_budget_mutant_refused "1 no" "$rc $exists"
fi

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_perf_median: PASS"
    exit 0
fi
echo "test_perf_median: FAIL ($failures case(s))"
exit 1
