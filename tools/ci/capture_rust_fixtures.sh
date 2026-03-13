#!/usr/bin/env bash
# capture_rust_fixtures.sh — Capture Rust reference fixtures for all fixture families
#
# Runs the fixture_capture binary in --fixture-dir mode against
# fixtures/rust_reference/, replacing all placeholder fixtures with real
# Rust-captured golden outputs.
#
# This script is idempotent — safe to re-run.
#
# Usage:
#   tools/ci/capture_rust_fixtures.sh [--verify-only]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FIXTURE_DIR="$PROJECT_ROOT/fixtures/rust_reference"
CAPTURE_BIN="$PROJECT_ROOT/tools/fixture_capture/target/release/fixture_capture"

# Build fixture_capture if needed
if [ ! -x "$CAPTURE_BIN" ]; then
    echo "Building fixture_capture binary..."
    (cd "$PROJECT_ROOT/tools/fixture_capture" && cargo build --release)
fi

# Mode: --verify-only just checks, doesn't capture
VERIFY_ONLY=false
if [ "${1:-}" = "--verify-only" ]; then
    VERIFY_ONLY=true
fi

if [ "$VERIFY_ONLY" = "false" ]; then
    echo "Capturing Rust reference fixtures..."
    "$CAPTURE_BIN" --fixture-dir "$FIXTURE_DIR"
    echo ""
fi

# Verification
echo "=== Verification ==="

# 1. Check no placeholder digests remain
PLACEHOLDER_COUNT=$(command grep -rl 'sha256:aaaa' "$FIXTURE_DIR" 2>/dev/null | wc -l | tr -d ' ' || true)
PLACEHOLDER_COUNT="${PLACEHOLDER_COUNT:-0}"
echo "Placeholder digests remaining: $PLACEHOLDER_COUNT"

# 2. Count total fixtures
TOTAL=$(command find "$FIXTURE_DIR" -name '*.json' -type f 2>/dev/null | wc -l | tr -d ' ')
echo "Total fixtures: $TOTAL"

# 3. Validate schema (if check-jsonschema available)
SCHEMA="$PROJECT_ROOT/schemas/canonical_fixture.schema.json"
if command -v check-jsonschema &>/dev/null && [ -f "$SCHEMA" ]; then
    SCHEMA_FAILURES=0
    for f in $(command find "$FIXTURE_DIR" -name '*.json' -type f); do
        if ! check-jsonschema --schemafile "$SCHEMA" "$f" 2>/dev/null; then
            SCHEMA_FAILURES=$((SCHEMA_FAILURES + 1))
            echo "  SCHEMA FAIL: $f"
        fi
    done
    echo "Schema validation failures: $SCHEMA_FAILURES"
else
    echo "Schema validation: SKIPPED (check-jsonschema not available)"
fi

# 4. Summary
if [ "$PLACEHOLDER_COUNT" -eq 0 ] && [ "$TOTAL" -ge 66 ]; then
    echo ""
    echo "PASS: All $TOTAL fixtures captured with real digests"
    exit 0
else
    echo ""
    echo "FAIL: $PLACEHOLDER_COUNT placeholders remaining, $TOTAL total (need >= 66)"
    exit 1
fi
