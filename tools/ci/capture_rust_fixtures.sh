#!/usr/bin/env bash
# capture_rust_fixtures.sh — capture Rust reference fixtures into STAGING only
# (bead W1.1, bd-9kll.2.1).
#
# The committed corpus in fixtures/rust_reference/ is never written here. A
# capture run copies the corpus into build/fixture_staging/<run_id>/, runs the
# capture binary there, and prints a diff summary against the committed
# corpus. Promotion is a separate, explicit step:
#   make fixtures-promote RUN_ID=<run_id>   (tools/ci/promote_fixtures.sh)
#
# Usage:
#   tools/ci/capture_rust_fixtures.sh [--run-id <id>] [--verify-only]
#
# Environment overrides (used by tests/gates/test_fixture_promotion.sh):
#   FIXTURE_DIR    committed corpus (default fixtures/rust_reference)
#   STAGING_ROOT   staging root (default build/fixture_staging)
#   CAPTURE_BIN    capture binary (default tools/fixture_capture release build)
#
# Exit 0 = staged and verified, 1 = capture or verification failed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FIXTURE_DIR="${FIXTURE_DIR:-$PROJECT_ROOT/fixtures/rust_reference}"
STAGING_ROOT="${STAGING_ROOT:-$PROJECT_ROOT/build/fixture_staging}"
CAPTURE_BIN="${CAPTURE_BIN:-$PROJECT_ROOT/tools/fixture_capture/target/release/fixture_capture}"
RUN_ID="capture-$(date -u +%Y%m%dT%H%M%SZ)"
VERIFY_ONLY=false

while [ $# -gt 0 ]; do
    case "$1" in
        --run-id) RUN_ID="$2"; shift 2 ;;
        --verify-only) VERIFY_ONLY=true; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

STAGING_DIR="$STAGING_ROOT/$RUN_ID"
if [ -e "$STAGING_DIR" ]; then
    echo "FAIL: staging dir already exists: $STAGING_DIR (use a new --run-id)" >&2
    exit 1
fi

if [ "$VERIFY_ONLY" = "false" ] && [ ! -x "$CAPTURE_BIN" ]; then
    echo "Building fixture_capture binary..."
    (cd "$PROJECT_ROOT/tools/fixture_capture" && cargo build --release)
fi

# Stage a copy of the committed corpus; the capture binary rewrites the copy.
mkdir -p "$STAGING_DIR"
cp -R "$FIXTURE_DIR/." "$STAGING_DIR/"

if [ "$VERIFY_ONLY" = "false" ]; then
    echo "Capturing Rust reference fixtures into $STAGING_DIR ..."
    "$CAPTURE_BIN" --fixture-dir "$STAGING_DIR"
    echo ""
fi

# Diff summary against the committed corpus (no file is removed anywhere).
echo "=== Staged vs committed ($RUN_ID) ==="
changed=0
added=0
while IFS= read -r -d '' staged; do
    rel="${staged#"$STAGING_DIR"/}"
    if [ ! -f "$FIXTURE_DIR/$rel" ]; then
        echo "  ADDED    $rel"
        added=$((added + 1))
    elif ! cmp -s "$staged" "$FIXTURE_DIR/$rel"; then
        echo "  CHANGED  $rel"
        changed=$((changed + 1))
    fi
done < <(find "$STAGING_DIR" -name '*.json' -type f -print0 | sort -z)
missing=0
while IFS= read -r -d '' committed; do
    rel="${committed#"$FIXTURE_DIR"/}"
    if [ ! -f "$STAGING_DIR/$rel" ]; then
        echo "  MISSING  $rel (in the committed corpus, not staged)"
        missing=$((missing + 1))
    fi
done < <(find "$FIXTURE_DIR" -name '*.json' -type f -print0 | sort -z)
echo "  summary: changed=$changed added=$added missing=$missing staging=$STAGING_DIR"

# Verification of the staged corpus.
echo "=== Verification ==="
PLACEHOLDER_COUNT=$(grep -rl 'sha256:aaaa' "$STAGING_DIR" 2>/dev/null | wc -l | tr -d ' ' || true)
PLACEHOLDER_COUNT="${PLACEHOLDER_COUNT:-0}"
TOTAL=$(find "$STAGING_DIR" -name '*.json' -type f 2>/dev/null | wc -l | tr -d ' ')
echo "Placeholder digests remaining: $PLACEHOLDER_COUNT"
echo "Total staged fixtures: $TOTAL"

if [ "$PLACEHOLDER_COUNT" -eq 0 ] && [ "$TOTAL" -ge 1 ]; then
    echo "PASS: staged $TOTAL fixtures in $STAGING_DIR"
    echo "Promote with: make fixtures-promote RUN_ID=$RUN_ID"
    exit 0
fi
echo "FAIL: $PLACEHOLDER_COUNT placeholders remaining, $TOTAL staged"
exit 1
