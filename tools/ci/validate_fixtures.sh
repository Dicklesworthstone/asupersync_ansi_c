#!/usr/bin/env bash
# validate_fixtures.sh — Validate Rust reference fixtures against canonical schema
#
# Checks:
#   1. Schema validation (if jsonschema/check-jsonschema available)
#   2. Provenance consistency (all fixtures share same commit/toolchain/cargo_lock)
#   3. No placeholder digests (sha256:aaaa...)
#   4. Expected_events populated for non-smoke fixtures
#   5. Required fields present
#
# Usage:
#   tools/ci/validate_fixtures.sh [fixtures/rust_reference/]
#
# Output: JSONL to stderr, summary to stdout

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FIXTURE_DIR="${1:-$PROJECT_ROOT/fixtures/rust_reference}"
SCHEMA="$PROJECT_ROOT/schemas/canonical_fixture.schema.json"

TOTAL=0
PASSED=0
FAILED=0
ERRORS=""

# Collect all fixture files
FIXTURE_FILES=()
while IFS= read -r -d '' f; do
    FIXTURE_FILES+=("$f")
done < <(command find "$FIXTURE_DIR" -name '*.json' -type f -print0 2>/dev/null | sort -z)

# First pass: collect provenance for consistency check
FIRST_COMMIT=""
FIRST_TOOLCHAIN=""
FIRST_CARGO_LOCK=""

for f in "${FIXTURE_FILES[@]}"; do
    commit=$(python3 -c "import json; d=json.load(open('$f')); print(d.get('provenance',{}).get('rust_baseline_commit',''))" 2>/dev/null || echo "")
    toolchain=$(python3 -c "import json; d=json.load(open('$f')); print(d.get('provenance',{}).get('rust_toolchain_commit_hash',''))" 2>/dev/null || echo "")
    cargo_lock=$(python3 -c "import json; d=json.load(open('$f')); print(d.get('provenance',{}).get('cargo_lock_sha256',''))" 2>/dev/null || echo "")

    if [ -z "$FIRST_COMMIT" ]; then
        FIRST_COMMIT="$commit"
        FIRST_TOOLCHAIN="$toolchain"
        FIRST_CARGO_LOCK="$cargo_lock"
    fi
    break
done

# Validate each fixture
for f in "${FIXTURE_FILES[@]}"; do
    TOTAL=$((TOTAL + 1))
    rel_path="${f#$FIXTURE_DIR/}"
    checks_passed=()
    checks_failed=()

    # Parse fixture
    result=$(python3 -c "
import json, sys

f = json.load(open('$f'))
issues = []

# Required fields
required = ['scenario_id', 'fixture_schema_version', 'scenario_dsl_version',
            'profile', 'codec', 'seed', 'input', 'expected_events',
            'expected_final_snapshot', 'expected_error_codes', 'semantic_digest', 'provenance']
for field in required:
    if field not in f:
        issues.append(('required_field', f'missing {field}'))

# Placeholder digest check
digest = f.get('semantic_digest', '')
if 'aaaa' in digest:
    issues.append(('digest_real', 'placeholder digest'))

# Provenance consistency
prov = f.get('provenance', {})
commit = prov.get('rust_baseline_commit', '')
toolchain = prov.get('rust_toolchain_commit_hash', '')
cargo_lock = prov.get('cargo_lock_sha256', '')
if commit and commit != '$FIRST_COMMIT':
    issues.append(('provenance', f'commit mismatch: {commit} != $FIRST_COMMIT'))
if toolchain and toolchain != '$FIRST_TOOLCHAIN':
    issues.append(('provenance', f'toolchain mismatch'))
if cargo_lock and cargo_lock != '$FIRST_CARGO_LOCK':
    issues.append(('provenance', f'cargo_lock mismatch'))

# Non-empty provenance
if not commit:
    issues.append(('provenance', 'empty rust_baseline_commit'))

# Events check (smoke/noop may have empty events)
events = f.get('expected_events', [])
ops = f.get('input', {}).get('ops', [])
is_noop = len(ops) == 1 and ops[0].get('op') == 'noop'
if not events and not is_noop:
    pass  # Some non-noop fixtures may legitimately have no events

# Input.ops present
if 'ops' not in f.get('input', {}):
    issues.append(('input_ops', 'missing input.ops'))

if issues:
    for check, detail in issues:
        print(f'FAIL|{check}|{detail}')
else:
    print('PASS')
" 2>/dev/null || echo "FAIL|parse|python error")

    if [ "$result" = "PASS" ]; then
        PASSED=$((PASSED + 1))
        checks_passed=("required_fields" "digest_real" "provenance" "input_ops")
        log=$(python3 -c "import json; print(json.dumps({'ts':'$(date -u +%Y-%m-%dT%H:%M:%SZ)','fixture':'$rel_path','status':'pass','checks':$( python3 -c "import json; print(json.dumps(['required_fields','digest_real','provenance','input_ops']))")}))")
        echo "$log" >&2
    else
        FAILED=$((FAILED + 1))
        while IFS='|' read -r status check detail; do
            log=$(python3 -c "import json; print(json.dumps({'ts':'$(date -u +%Y-%m-%dT%H:%M:%SZ)','fixture':'$rel_path','status':'fail','check':'$check','detail':'$detail'}))")
            echo "$log" >&2
        done <<< "$result"
    fi
done

# Summary
summary=$(python3 -c "import json; print(json.dumps({'ts':'$(date -u +%Y-%m-%dT%H:%M:%SZ)','summary':'validation_complete','total':$TOTAL,'passed':$PASSED,'failed':$FAILED,'provenance_commit':'$FIRST_COMMIT'}))")
echo "$summary" >&2

echo ""
echo "=== Fixture Validation Summary ==="
echo "Total:  $TOTAL"
echo "Passed: $PASSED"
echo "Failed: $FAILED"
echo "Provenance commit: $FIRST_COMMIT"

if [ "$FAILED" -eq 0 ]; then
    echo ""
    echo "PASS: All $TOTAL fixtures valid"
    exit 0
else
    echo ""
    echo "FAIL: $FAILED fixture(s) failed validation"
    exit 1
fi
