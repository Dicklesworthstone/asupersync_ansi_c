#!/usr/bin/env bash
# =============================================================================
# check_codegen_stability.sh — Cross-optimization codegen stability (bd-3vt.4)
#
# Verifies that critical kernel functions produce identical observable
# behavior across optimization levels (-O0, -O1, -O2, -O3, -Os).
# Detects when compiler optimizations change semantic behavior.
#
# Usage:
#   tools/ci/check_codegen_stability.sh [--strict] [--output <file>]
#
# Exit codes:
#   0: all checks pass
#   1: codegen stability violation (strict mode)
#   2: usage/build error
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

STRICT=0
OUTPUT=""
CC="${CC:-gcc}"
OPT_LEVELS="-O0 -O1 -O2 -O3 -Os"

usage() {
    cat <<'USAGE'
Usage: tools/ci/check_codegen_stability.sh [OPTIONS]

Options:
  --strict    Exit 1 on instability (default: warn only)
  --output    Write report JSON to file (default: stdout)
  --help      Show this help
USAGE
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --strict) STRICT=1; shift ;;
        --output) OUTPUT="$2"; shift 2 ;;
        --help|-h) usage ;;
        *) echo "[asx] codegen-check: ERROR — unknown option: $1" >&2; usage ;;
    esac
done

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/asx-codegen-check.XXXXXX")"
cleanup() { rm -rf "$tmp_dir"; }
trap cleanup EXIT

# --- Generate codegen probe ---

probe_src="$tmp_dir/codegen_probe.c"

cat > "$probe_src" <<'PROBE'
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <asx/asx.h>

/*
 * Codegen stability probe: exercises critical kernel functions
 * and prints observable outputs. Any difference across optimization
 * levels indicates a codegen instability.
 */

static void probe_transition_tables(void)
{
    unsigned f, t;
    printf("=== transition_tables ===\n");

    /* Region: all 25 pairs */
    for (f = 0; f <= 4; f++) {
        for (t = 0; t <= 4; t++) {
            asx_status st = asx_region_transition_check(
                (asx_region_state)f, (asx_region_state)t);
            printf("region[%u][%u]=%d\n", f, t, (int)st);
        }
    }

    /* Task: all 36 pairs */
    for (f = 0; f <= 5; f++) {
        for (t = 0; t <= 5; t++) {
            asx_status st = asx_task_transition_check(
                (asx_task_state)f, (asx_task_state)t);
            printf("task[%u][%u]=%d\n", f, t, (int)st);
        }
    }

    /* Obligation: all 16 pairs */
    for (f = 0; f <= 3; f++) {
        for (t = 0; t <= 3; t++) {
            asx_status st = asx_obligation_transition_check(
                (asx_obligation_state)f, (asx_obligation_state)t);
            printf("obligation[%u][%u]=%d\n", f, t, (int)st);
        }
    }
}

static void probe_cancel_severity(void)
{
    int k;
    printf("=== cancel_severity ===\n");
    for (k = -1; k <= 12; k++) {
        printf("severity[%d]=%d\n", k, asx_cancel_severity((asx_cancel_kind)k));
    }
}

static void probe_outcome_join(void)
{
    int a, b;
    printf("=== outcome_join ===\n");
    for (a = 0; a <= 3; a++) {
        for (b = 0; b <= 3; b++) {
            asx_outcome oa, ob, result;
            oa.severity = (asx_outcome_severity)a;
            ob.severity = (asx_outcome_severity)b;
            result = asx_outcome_join(&oa, &ob);
            printf("join[%d][%d]=%d\n", a, b, (int)result.severity);
        }
    }
    /* NULL handling */
    {
        asx_outcome o;
        o.severity = ASX_OUTCOME_ERR;
        asx_outcome r1 = asx_outcome_join(NULL, &o);
        asx_outcome r2 = asx_outcome_join(&o, NULL);
        asx_outcome r3 = asx_outcome_join(NULL, NULL);
        printf("join[NULL][ERR]=%d\n", (int)r1.severity);
        printf("join[ERR][NULL]=%d\n", (int)r2.severity);
        printf("join[NULL][NULL]=%d\n", (int)r3.severity);
    }
}

static void probe_handle_ops(void)
{
    printf("=== handle_ops ===\n");
    printf("valid(0)=%d\n", asx_handle_is_valid(0));
    printf("valid(1)=%d\n", asx_handle_is_valid(1));
    printf("valid(UINT64_MAX)=%d\n", asx_handle_is_valid(UINT64_MAX));
}

static void probe_budget_ops(void)
{
    printf("=== budget_ops ===\n");

    asx_budget inf = asx_budget_infinite();
    asx_budget zero = asx_budget_zero();
    asx_budget from100 = asx_budget_from_polls(100);

    printf("inf.poll=%u\n", inf.poll_quota);
    printf("inf.exhausted=%d\n", asx_budget_is_exhausted(&inf));
    printf("zero.poll=%u\n", zero.poll_quota);
    printf("zero.exhausted=%d\n", asx_budget_is_exhausted(&zero));
    printf("from100.poll=%u\n", from100.poll_quota);

    /* Meet operations */
    asx_budget m1 = asx_budget_meet(&inf, &from100);
    asx_budget m2 = asx_budget_meet(&zero, &from100);
    printf("meet(inf,100).poll=%u\n", m1.poll_quota);
    printf("meet(zero,100).poll=%u\n", m2.poll_quota);
    printf("meet(zero,100).exhausted=%d\n", asx_budget_is_exhausted(&m2));
}

static void probe_type_sizes(void)
{
    printf("=== type_sizes ===\n");
    printf("region_id=%lu\n", (unsigned long)sizeof(asx_region_id));
    printf("task_id=%lu\n", (unsigned long)sizeof(asx_task_id));
    printf("status=%lu\n", (unsigned long)sizeof(asx_status));
    printf("time=%lu\n", (unsigned long)sizeof(asx_time));
    printf("budget=%lu\n", (unsigned long)sizeof(asx_budget));
    printf("outcome=%lu\n", (unsigned long)sizeof(asx_outcome));
    printf("cancel_reason=%lu\n", (unsigned long)sizeof(asx_cancel_reason));
    printf("runtime_config=%lu\n", (unsigned long)sizeof(asx_runtime_config));
}

int main(void)
{
    probe_transition_tables();
    probe_cancel_severity();
    probe_outcome_join();
    probe_handle_ops();
    probe_budget_ops();
    probe_type_sizes();
    return 0;
}
PROBE

# --- Compile and run at each optimization level ---

violations="$tmp_dir/violations.txt"
passes="$tmp_dir/passes.txt"
: > "$violations"
: > "$passes"

reference_output=""
reference_opt=""

for opt in $OPT_LEVELS; do
    probe_bin="$tmp_dir/codegen_probe${opt}"

    # Build libasx.a if not present
    if [ ! -f "$REPO_ROOT/build/lib/libasx.a" ]; then
        make -C "$REPO_ROOT" build >/dev/null 2>&1 || true
    fi

    if ! $CC -std=c99 "$opt" -I"$REPO_ROOT/include" \
        -DASX_PROFILE_CORE -DASX_CODEC_JSON -DASX_DETERMINISTIC=1 \
        -o "$probe_bin" "$probe_src" \
        "$REPO_ROOT/build/lib/libasx.a" \
        2>"$tmp_dir/compile_${opt}.log"; then
        echo "[asx] codegen-check: ERROR — compile failed at $opt" >&2
        cat "$tmp_dir/compile_${opt}.log" >&2
        exit 2
    fi

    output="$tmp_dir/output${opt}.txt"
    "$probe_bin" > "$output"

    if [ -z "$reference_output" ]; then
        reference_output="$output"
        reference_opt="$opt"
        echo "codegen:reference=$opt" >> "$passes"
    else
        if diff -q "$reference_output" "$output" >/dev/null 2>&1; then
            echo "codegen:${opt}_vs_${reference_opt}:match" >> "$passes"
        else
            # Find specific differences
            diff_count=$(diff "$reference_output" "$output" | grep "^[<>]" | wc -l | tr -d ' ')
            echo "codegen:${opt}_vs_${reference_opt}:mismatch:identical:${diff_count}_lines_differ" >> "$violations"
        fi
    fi
done

violation_count="$(wc -l < "$violations" | tr -d ' ')"
pass_count="$(wc -l < "$passes" | tr -d ' ')"
total=$((violation_count + pass_count))

# --- Build report ---

status="pass"
if [ "$violation_count" -gt 0 ]; then
    status="fail"
fi

report_json="$tmp_dir/report.json"
{
    printf '{\n'
    printf '  "schema": "asx.codegen_stability_report.v1",\n'
    printf '  "generated_at": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '  "compiler": "%s",\n' "$CC"
    printf '  "optimization_levels": "%s",\n' "$OPT_LEVELS"
    printf '  "reference_level": "%s",\n' "$reference_opt"
    printf '  "status": "%s",\n' "$status"
    printf '  "summary": {\n'
    printf '    "total": %d,\n' "$total"
    printf '    "passed": %d,\n' "$pass_count"
    printf '    "failed": %d,\n' "$violation_count"
    printf '    "probes": ["transition_tables", "cancel_severity", "outcome_join", "handle_ops", "budget_ops", "type_sizes"]\n'
    printf '  },\n'

    printf '  "violations": [\n'
    first=1
    while IFS=':' read -r metric kind expected actual; do
        [ -z "$metric" ] && continue
        if [ "$first" -eq 0 ]; then printf ',\n'; fi
        printf '    {"metric": "%s", "kind": "%s", "expected": "%s", "actual": "%s"}' \
            "$metric" "$kind" "$expected" "$actual"
        first=0
    done < "$violations"
    printf '\n  ],\n'

    printf '  "rerun_command": "tools/ci/check_codegen_stability.sh --strict"\n'
    printf '}\n'
} > "$report_json"

# --- Output ---

if [ -n "$OUTPUT" ]; then
    mkdir -p "$(dirname "$OUTPUT")"
    cp "$report_json" "$OUTPUT"
    echo "[asx] codegen-check: report written to $OUTPUT" >&2
else
    cat "$report_json"
fi

# --- Summary ---

echo "[asx] codegen-check: status=$status total=$total passed=$pass_count failed=$violation_count" >&2
echo "[asx] codegen-check: tested $CC at $OPT_LEVELS" >&2

if [ "$violation_count" -gt 0 ]; then
    echo "[asx] codegen-check: CODEGEN STABILITY VIOLATIONS:" >&2
    while IFS=':' read -r metric kind expected actual; do
        [ -z "$metric" ] && continue
        echo "  - $metric: $kind (expected=$expected actual=$actual)" >&2
    done < "$violations"
fi

# --- Exit code ---

if [ "$STRICT" = "1" ] && [ "$violation_count" -gt 0 ]; then
    echo "[asx] codegen-check: FAIL (strict mode, $violation_count violations)" >&2
    exit 1
fi

exit 0
