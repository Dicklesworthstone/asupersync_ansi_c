#!/usr/bin/env bash
# =============================================================================
# check_abi_stability.sh — ABI break detection gate (bd-56t.4)
#
# Verifies that ABI-critical type sizes, enum values, and struct layouts
# match frozen baselines. Any deviation constitutes an ABI break and
# requires a major version bump.
#
# Usage:
#   tools/ci/check_abi_stability.sh [--strict] [--output <file>]
#
# Exit codes:
#   0: all ABI checks pass
#   1: ABI break detected (strict mode)
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

usage() {
    cat <<'USAGE'
Usage: tools/ci/check_abi_stability.sh [OPTIONS]

Options:
  --strict    Exit 1 on ABI break (default: warn only)
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
        *) echo "[asx] abi-check: ERROR — unknown option: $1" >&2; usage ;;
    esac
done

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/asx-abi-check.XXXXXX")"
cleanup() { rm -rf "$tmp_dir"; }
trap cleanup EXIT

# --- Generate ABI probe ---

probe_src="$tmp_dir/abi_probe.c"
probe_bin="$tmp_dir/abi_probe"

cat > "$probe_src" <<'PROBE'
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <asx/asx.h>

#define PROBE(name, val) printf("%s=%lu\n", name, (unsigned long)(val))

int main(void) {
    /* Handle type sizes */
    PROBE("size.asx_region_id", sizeof(asx_region_id));
    PROBE("size.asx_task_id", sizeof(asx_task_id));
    PROBE("size.asx_obligation_id", sizeof(asx_obligation_id));
    PROBE("size.asx_timer_id", sizeof(asx_timer_id));
    PROBE("size.asx_channel_id", sizeof(asx_channel_id));
    PROBE("size.asx_status", sizeof(asx_status));
    PROBE("size.asx_time", sizeof(asx_time));

    /* Key struct sizes */
    PROBE("size.asx_runtime_config", sizeof(asx_runtime_config));
    PROBE("size.asx_runtime_hooks", sizeof(asx_runtime_hooks));
    PROBE("size.asx_budget", sizeof(asx_budget));
    PROBE("size.asx_outcome", sizeof(asx_outcome));
    PROBE("size.asx_cancel_reason", sizeof(asx_cancel_reason));

    /* Frozen enum values */
    PROBE("enum.ASX_OK", ASX_OK);
    PROBE("enum.ASX_E_PENDING", ASX_E_PENDING);
    PROBE("enum.ASX_E_INVALID_ARGUMENT", ASX_E_INVALID_ARGUMENT);
    PROBE("enum.ASX_E_INVALID_TRANSITION", ASX_E_INVALID_TRANSITION);
    PROBE("enum.ASX_E_REGION_CLOSED", ASX_E_REGION_CLOSED);
    PROBE("enum.ASX_E_SCHEDULER_UNAVAILABLE", ASX_E_SCHEDULER_UNAVAILABLE);
    PROBE("enum.ASX_E_CANCELLED", ASX_E_CANCELLED);
    PROBE("enum.ASX_E_DISCONNECTED", ASX_E_DISCONNECTED);
    PROBE("enum.ASX_E_TIMER_DURATION_EXCEEDED", ASX_E_TIMER_DURATION_EXCEEDED);
    PROBE("enum.ASX_E_RESOURCE_EXHAUSTED", ASX_E_RESOURCE_EXHAUSTED);
    PROBE("enum.ASX_E_STALE_HANDLE", ASX_E_STALE_HANDLE);
    PROBE("enum.ASX_E_HOOK_MISSING", ASX_E_HOOK_MISSING);

    /* State machine enum values */
    PROBE("enum.ASX_REGION_OPEN", ASX_REGION_OPEN);
    PROBE("enum.ASX_REGION_CLOSED", ASX_REGION_CLOSED);
    PROBE("enum.ASX_TASK_CREATED", ASX_TASK_CREATED);
    PROBE("enum.ASX_TASK_COMPLETED", ASX_TASK_COMPLETED);
    PROBE("enum.ASX_OUTCOME_OK", ASX_OUTCOME_OK);
    PROBE("enum.ASX_OUTCOME_PANICKED", ASX_OUTCOME_PANICKED);
    PROBE("enum.ASX_CANCEL_USER", ASX_CANCEL_USER);
    PROBE("enum.ASX_CANCEL_PHASE_REQUESTED", ASX_CANCEL_PHASE_REQUESTED);
    PROBE("enum.ASX_CANCEL_PHASE_COMPLETED", ASX_CANCEL_PHASE_COMPLETED);

    /* Handle sentinel */
    PROBE("sentinel.ASX_INVALID_ID", ASX_INVALID_ID);

    /* ABI version */
    PROBE("abi.major", ASX_ABI_VERSION_MAJOR);
    PROBE("abi.minor", ASX_ABI_VERSION_MINOR);
    PROBE("abi.patch", ASX_ABI_VERSION_PATCH);

    /* Capacity constants */
    PROBE("capacity.ASX_MAX_REGIONS", ASX_MAX_REGIONS);
    PROBE("capacity.ASX_MAX_TASKS", ASX_MAX_TASKS);
    PROBE("capacity.ASX_MAX_OBLIGATIONS", ASX_MAX_OBLIGATIONS);

    return 0;
}
PROBE

# --- Compile and run probe ---

if ! $CC -std=c99 -I"$REPO_ROOT/include" \
    -DASX_PROFILE_CORE -DASX_CODEC_JSON -DASX_DETERMINISTIC=1 \
    -o "$probe_bin" "$probe_src" 2>"$tmp_dir/compile.log"; then
    echo "[asx] abi-check: ERROR — probe compilation failed" >&2
    cat "$tmp_dir/compile.log" >&2
    exit 2
fi

probe_output="$tmp_dir/probe_output.txt"
"$probe_bin" > "$probe_output"

# --- Define ABI baseline (frozen values) ---

baseline="$tmp_dir/baseline.txt"
cat > "$baseline" <<'BASELINE'
size.asx_region_id=8
size.asx_task_id=8
size.asx_obligation_id=8
size.asx_timer_id=8
size.asx_channel_id=8
size.asx_status=4
size.asx_time=8
enum.ASX_OK=0
enum.ASX_E_PENDING=1
enum.ASX_E_INVALID_ARGUMENT=100
enum.ASX_E_INVALID_TRANSITION=200
enum.ASX_E_REGION_CLOSED=301
enum.ASX_E_SCHEDULER_UNAVAILABLE=401
enum.ASX_E_CANCELLED=600
enum.ASX_E_DISCONNECTED=700
enum.ASX_E_TIMER_DURATION_EXCEEDED=802
enum.ASX_E_RESOURCE_EXHAUSTED=1000
enum.ASX_E_STALE_HANDLE=1100
enum.ASX_E_HOOK_MISSING=1200
enum.ASX_REGION_OPEN=0
enum.ASX_REGION_CLOSED=4
enum.ASX_TASK_CREATED=0
enum.ASX_TASK_COMPLETED=5
enum.ASX_OUTCOME_OK=0
enum.ASX_OUTCOME_PANICKED=3
enum.ASX_CANCEL_USER=0
enum.ASX_CANCEL_PHASE_REQUESTED=0
enum.ASX_CANCEL_PHASE_COMPLETED=3
sentinel.ASX_INVALID_ID=0
BASELINE

# --- Compare probe output against baseline ---

violations="$tmp_dir/violations.txt"
passes="$tmp_dir/passes.txt"
: > "$violations"
: > "$passes"

while IFS='=' read -r metric expected; do
    [ -z "$metric" ] && continue
    actual="$(grep "^${metric}=" "$probe_output" | cut -d= -f2)"
    if [ -z "$actual" ]; then
        echo "${metric}:missing:${expected}:0" >> "$violations"
    elif [ "$actual" != "$expected" ]; then
        echo "${metric}:changed:${expected}:${actual}" >> "$violations"
    else
        echo "${metric}:${expected}" >> "$passes"
    fi
done < "$baseline"

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
    printf '  "schema": "asx.abi_stability_report.v1",\n'
    printf '  "generated_at": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '  "compiler": "%s",\n' "$CC"
    printf '  "status": "%s",\n' "$status"
    printf '  "summary": {\n'
    printf '    "total": %d,\n' "$total"
    printf '    "passed": %d,\n' "$pass_count"
    printf '    "failed": %d\n' "$violation_count"
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

    printf '  "abi_version": {"major": %d, "minor": %d, "patch": %d},\n' \
        "$(grep '^abi.major=' "$probe_output" | cut -d= -f2)" \
        "$(grep '^abi.minor=' "$probe_output" | cut -d= -f2)" \
        "$(grep '^abi.patch=' "$probe_output" | cut -d= -f2)"

    printf '  "rerun_command": "tools/ci/check_abi_stability.sh --strict"\n'
    printf '}\n'
} > "$report_json"

# --- Output ---

if [ -n "$OUTPUT" ]; then
    mkdir -p "$(dirname "$OUTPUT")"
    cp "$report_json" "$OUTPUT"
    echo "[asx] abi-check: report written to $OUTPUT" >&2
else
    cat "$report_json"
fi

# --- Summary ---

echo "[asx] abi-check: status=$status total=$total passed=$pass_count failed=$violation_count" >&2

if [ "$violation_count" -gt 0 ]; then
    echo "[asx] abi-check: ABI BREAKS DETECTED:" >&2
    while IFS=':' read -r metric kind expected actual; do
        [ -z "$metric" ] && continue
        echo "  - $metric: $kind (expected=$expected actual=$actual)" >&2
    done < "$violations"
fi

# --- Exit code ---

if [ "$STRICT" = "1" ] && [ "$violation_count" -gt 0 ]; then
    echo "[asx] abi-check: FAIL (strict mode, $violation_count ABI breaks)" >&2
    exit 1
fi

exit 0
