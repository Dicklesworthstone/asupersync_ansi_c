#!/usr/bin/env bash
# test_fixture_pipeline_e2e.sh — End-to-end test for the Rust fixture pipeline
#
# Validates the complete pipeline from Rust toolchain through conformance,
# emitting structured JSONL to stdout for automated triage.
#
# Usage:
#   tools/ci/test_fixture_pipeline_e2e.sh [OPTIONS]
#
# Options:
#   --dry-run            Show steps without executing
#   --skip-build         Skip Rust build (use existing binary)
#   --skip-capture       Skip fixture capture (use existing fixtures)
#   --skip-conformance   Skip conformance run
#   --fixtures-dir D     Override fixture directory
#   --help               Show this help
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FIXTURE_DIR="${PROJECT_ROOT}/fixtures/rust_reference"
CAPTURE_BIN="${PROJECT_ROOT}/tools/fixture_capture/target/release/fixture_capture"
CAPTURE_DIR="${PROJECT_ROOT}/tools/fixture_capture"
SCHEMA_FILE="${PROJECT_ROOT}/schemas/canonical_fixture.schema.json"

DRY_RUN=0
SKIP_BUILD=0
SKIP_CAPTURE=0
SKIP_CONFORMANCE=0
PIPELINE_START_NS=0
STEP_FAIL=""

# Minimum expected fixtures (from bd-reg6.3 acceptance criteria)
MIN_FIXTURES=66

# Expected fixture families
EXPECTED_FAMILIES="core_budget core_cancel core_channel core_lifecycle core_timer smoke"

usage() {
    cat <<'EOF'
Usage: test_fixture_pipeline_e2e.sh [OPTIONS]

End-to-end test for the Rust fixture capture pipeline.

Options:
  --dry-run            Show steps without executing
  --skip-build         Skip Rust build (use existing binary)
  --skip-capture       Skip fixture capture (use existing fixtures)
  --skip-conformance   Skip conformance run
  --fixtures-dir D     Override fixture directory
  --help               Show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)           DRY_RUN=1; shift ;;
        --skip-build)        SKIP_BUILD=1; shift ;;
        --skip-capture)      SKIP_CAPTURE=1; shift ;;
        --skip-conformance)  SKIP_CONFORMANCE=1; shift ;;
        --fixtures-dir)      FIXTURE_DIR="$2"; shift 2 ;;
        --help)              usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── helpers ─────────────────────────────────────────────────────────────

ts() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }

now_ns() { date +%s%N; }

emit() {
    # Emit a JSONL record to stdout
    echo "$1"
}

emit_step() {
    local step="$1"
    local status="$2"
    local detail="$3"
    local start_ns="$4"
    local end_ns
    end_ns=$(now_ns)
    local duration_ms=$(( (end_ns - start_ns) / 1000000 ))
    emit "{\"ts\":\"$(ts)\",\"step\":\"$step\",\"status\":\"$status\",\"detail\":\"$detail\",\"duration_ms\":$duration_ms}"
}

fail_step() {
    local step="$1"
    local detail="$2"
    local start_ns="$3"
    emit_step "$step" "fail" "$detail" "$start_ns"
    STEP_FAIL="$step"
}

# ── Step 1: Rust toolchain check ────────────────────────────────────────

step_rust_setup() {
    local start_ns
    start_ns=$(now_ns)

    if [ "$DRY_RUN" -eq 1 ]; then
        emit_step "rust_setup" "skip" "dry-run" "$start_ns"
        return 0
    fi

    if ! command -v rustc >/dev/null 2>&1; then
        fail_step "rust_setup" "rustc not found" "$start_ns"
        return 1
    fi

    if ! command -v cargo >/dev/null 2>&1; then
        fail_step "rust_setup" "cargo not found" "$start_ns"
        return 1
    fi

    local rustc_version
    rustc_version=$(rustc --version 2>/dev/null)

    # Check for nightly (required for fixture capture)
    if ! echo "$rustc_version" | command grep -q 'nightly'; then
        fail_step "rust_setup" "nightly toolchain required, found: $rustc_version" "$start_ns"
        return 1
    fi

    emit_step "rust_setup" "pass" "$rustc_version" "$start_ns"
    return 0
}

# ── Step 2: Build fixture_capture binary ────────────────────────────────

step_rust_build() {
    local start_ns
    start_ns=$(now_ns)

    if [ "$DRY_RUN" -eq 1 ]; then
        emit_step "rust_build" "skip" "dry-run" "$start_ns"
        return 0
    fi

    if [ "$SKIP_BUILD" -eq 1 ]; then
        if [ -x "$CAPTURE_BIN" ]; then
            local size
            size=$(stat -c%s "$CAPTURE_BIN" 2>/dev/null || stat -f%z "$CAPTURE_BIN" 2>/dev/null || echo "unknown")
            emit_step "rust_build" "pass" "skipped, existing binary ${size} bytes" "$start_ns"
            return 0
        else
            fail_step "rust_build" "skip-build requested but $CAPTURE_BIN not found" "$start_ns"
            return 1
        fi
    fi

    if [ ! -d "$CAPTURE_DIR" ]; then
        fail_step "rust_build" "capture source dir not found: $CAPTURE_DIR" "$start_ns"
        return 1
    fi

    if ! (cd "$CAPTURE_DIR" && cargo build --release >/dev/null 2>&1); then
        fail_step "rust_build" "cargo build --release failed" "$start_ns"
        return 1
    fi

    if [ ! -x "$CAPTURE_BIN" ]; then
        fail_step "rust_build" "binary not produced after build" "$start_ns"
        return 1
    fi

    local size
    size=$(stat -c%s "$CAPTURE_BIN" 2>/dev/null || stat -f%z "$CAPTURE_BIN" 2>/dev/null || echo "unknown")
    emit_step "rust_build" "pass" "release binary ${size} bytes" "$start_ns"
    return 0
}

# ── Step 3: Capture fixtures ───────────────────────────────────────────

step_fixture_capture() {
    local start_ns
    start_ns=$(now_ns)

    if [ "$DRY_RUN" -eq 1 ]; then
        emit_step "fixture_capture" "skip" "dry-run" "$start_ns"
        return 0
    fi

    if [ "$SKIP_CAPTURE" -eq 1 ]; then
        local count
        count=$(command find "$FIXTURE_DIR" -name '*.json' -type f 2>/dev/null | wc -l | tr -d ' ')
        if [ "$count" -ge "$MIN_FIXTURES" ]; then
            local family_count
            family_count=$(command find "$FIXTURE_DIR" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l | tr -d ' ')
            emit_step "fixture_capture" "pass" "skipped, $count existing fixtures in $family_count families" "$start_ns"
            return 0
        else
            fail_step "fixture_capture" "skip-capture requested but only $count fixtures (need >= $MIN_FIXTURES)" "$start_ns"
            return 1
        fi
    fi

    if [ ! -x "$CAPTURE_BIN" ]; then
        fail_step "fixture_capture" "capture binary not found: $CAPTURE_BIN" "$start_ns"
        return 1
    fi

    if ! "$CAPTURE_BIN" --fixture-dir "$FIXTURE_DIR" >/dev/null 2>&1; then
        fail_step "fixture_capture" "fixture capture failed" "$start_ns"
        return 1
    fi

    local count
    count=$(command find "$FIXTURE_DIR" -name '*.json' -type f 2>/dev/null | wc -l | tr -d ' ')
    local family_count
    family_count=$(command find "$FIXTURE_DIR" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l | tr -d ' ')

    if [ "$count" -lt "$MIN_FIXTURES" ]; then
        fail_step "fixture_capture" "only $count fixtures captured (need >= $MIN_FIXTURES)" "$start_ns"
        return 1
    fi

    emit_step "fixture_capture" "pass" "$count fixtures in $family_count families" "$start_ns"
    return 0
}

# ── Step 4: Schema validation ──────────────────────────────────────────

step_schema_validation() {
    local start_ns
    start_ns=$(now_ns)

    if [ "$DRY_RUN" -eq 1 ]; then
        emit_step "schema_validation" "skip" "dry-run" "$start_ns"
        return 0
    fi

    if [ ! -f "$SCHEMA_FILE" ]; then
        fail_step "schema_validation" "schema file not found: $SCHEMA_FILE" "$start_ns"
        return 1
    fi

    local total=0
    local valid=0
    local invalid=0
    local invalid_files=""

    while IFS= read -r f; do
        total=$((total + 1))
        # Validate required fields using python3 (available on all CI hosts)
        if python3 -c "
import json, sys
d = json.load(open('$f'))
required = ['scenario_id', 'fixture_schema_version', 'profile', 'codec',
            'seed', 'input', 'expected_events', 'expected_final_snapshot',
            'expected_error_codes', 'semantic_digest', 'provenance']
missing = [r for r in required if r not in d]
if missing:
    print('missing: ' + ', '.join(missing), file=sys.stderr)
    sys.exit(1)
# Validate digest is not placeholder
digest = d.get('semantic_digest', '')
if 'aaaa' in digest:
    print('placeholder digest', file=sys.stderr)
    sys.exit(1)
" 2>/dev/null; then
            valid=$((valid + 1))
        else
            invalid=$((invalid + 1))
            invalid_files="${invalid_files} ${f#$FIXTURE_DIR/}"
        fi
    done < <(command find "$FIXTURE_DIR" -name '*.json' -type f 2>/dev/null | sort)

    if [ "$invalid" -gt 0 ]; then
        fail_step "schema_validation" "$invalid/$total invalid:$invalid_files" "$start_ns"
        return 1
    fi

    emit_step "schema_validation" "pass" "$valid/$total valid" "$start_ns"
    return 0
}

# ── Step 5: Provenance consistency ─────────────────────────────────────

step_provenance_check() {
    local start_ns
    start_ns=$(now_ns)

    if [ "$DRY_RUN" -eq 1 ]; then
        emit_step "provenance_check" "skip" "dry-run" "$start_ns"
        return 0
    fi

    local result
    result=$(python3 -c "
import json, os, sys, glob

fixture_dir = '$FIXTURE_DIR'
fixtures = sorted(glob.glob(os.path.join(fixture_dir, '**', '*.json'), recursive=True))

if not fixtures:
    print('no fixtures found')
    sys.exit(1)

ref = None
mismatches = []

for f in fixtures:
    d = json.load(open(f))
    prov = d.get('provenance', {})
    key = (
        prov.get('rust_baseline_commit', ''),
        prov.get('rust_toolchain_commit_hash', ''),
        prov.get('cargo_lock_sha256', '')
    )
    if ref is None:
        ref = key
    elif key != ref:
        rel = os.path.relpath(f, fixture_dir)
        mismatches.append(rel)

if mismatches:
    print('MISMATCH:' + ','.join(mismatches[:5]))
    sys.exit(1)
else:
    print('CONSISTENT:commit=' + ref[0][:12])
" 2>&1)

    local rc=$?
    if [ "$rc" -ne 0 ]; then
        fail_step "provenance_check" "$result" "$start_ns"
        return 1
    fi

    local detail="${result#CONSISTENT:}"
    emit_step "provenance_check" "pass" "all provenance fields match ($detail)" "$start_ns"
    return 0
}

# ── Step 6: Conformance run ────────────────────────────────────────────

step_conformance_run() {
    local start_ns
    start_ns=$(now_ns)

    if [ "$DRY_RUN" -eq 1 ]; then
        emit_step "conformance_run" "skip" "dry-run" "$start_ns"
        return 0
    fi

    if [ "$SKIP_CONFORMANCE" -eq 1 ]; then
        emit_step "conformance_run" "skip" "skipped by --skip-conformance" "$start_ns"
        return 0
    fi

    local conformance_script="$SCRIPT_DIR/run_conformance.sh"
    if [ ! -x "$conformance_script" ]; then
        fail_step "conformance_run" "run_conformance.sh not found or not executable" "$start_ns"
        return 1
    fi

    # Build the host library first if needed
    if [ ! -f "${PROJECT_ROOT}/build/lib/libasx.a" ]; then
        if ! make -C "$PROJECT_ROOT" build >/dev/null 2>&1; then
            fail_step "conformance_run" "failed to build libasx.a" "$start_ns"
            return 1
        fi
    fi

    local output
    if ! output=$("$conformance_script" --fixtures-root "$FIXTURE_DIR" 2>&1); then
        # Extract summary info from output
        local records
        records=$(echo "$output" | command grep -oP 'total_records.*?(\d+)' | head -1 || echo "unknown")
        local deltas
        deltas=$(echo "$output" | command grep -oP 'semantic_deltas.*?(\d+)' | head -1 || echo "unknown")
        fail_step "conformance_run" "conformance failed ($records records, $deltas deltas)" "$start_ns"
        return 1
    fi

    # Parse the summary from conformance output
    local records="0"
    local deltas="0"
    if echo "$output" | command grep -q '"total_records"'; then
        records=$(echo "$output" | command grep -oP '"total_records"\s*:\s*\K\d+' | tail -1 || echo "0")
        deltas=$(echo "$output" | command grep -oP '"semantic_deltas"\s*:\s*\K\d+' | tail -1 || echo "0")
    fi

    emit_step "conformance_run" "pass" "$records records, $deltas deltas" "$start_ns"
    return 0
}

# ── Pipeline orchestration ─────────────────────────────────────────────

main() {
    PIPELINE_START_NS=$(now_ns)

    local steps=("rust_setup" "rust_build" "fixture_capture" "schema_validation" "provenance_check" "conformance_run")
    local failed=0

    for step in "${steps[@]}"; do
        if ! "step_$step"; then
            failed=1
            break
        fi
    done

    # Pipeline summary
    local end_ns
    end_ns=$(now_ns)
    local total_ms=$(( (end_ns - PIPELINE_START_NS) / 1000000 ))

    if [ "$failed" -eq 0 ]; then
        emit "{\"ts\":\"$(ts)\",\"step\":\"pipeline_result\",\"status\":\"pass\",\"detail\":\"full pipeline green\",\"duration_ms\":$total_ms}"
        exit 0
    else
        emit "{\"ts\":\"$(ts)\",\"step\":\"pipeline_result\",\"status\":\"fail\",\"detail\":\"failed at step: $STEP_FAIL\",\"duration_ms\":$total_ms}"
        exit 1
    fi
}

main
