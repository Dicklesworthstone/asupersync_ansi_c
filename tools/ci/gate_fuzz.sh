#!/bin/bash
# gate_fuzz.sh — GATE-FUZZ CI gate for differential fuzzing
#
# Wraps the Makefile fuzz targets with tier-based iteration budgets,
# optional Rust binary comparison, and structured JSONL output.
#
# Usage:
#   tools/ci/gate_fuzz.sh --tier smoke              # CI: 100 iterations, <60s
#   tools/ci/gate_fuzz.sh --tier nightly             # Nightly: 100K iterations
#   tools/ci/gate_fuzz.sh --tier release             # Release: 500K iterations
#   tools/ci/gate_fuzz.sh --tier smoke --rust-binary path/to/binary
#   tools/ci/gate_fuzz.sh --tier smoke --report /tmp/fuzz.jsonl
#
# Bead: bd-rkql.7

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Defaults
TIER="smoke"
RUST_BINARY=""
REPORT_PATH=""
MINIMIZE_BINARY=""
VERBOSE=0

# Tier iteration budgets
TIER_SMOKE=100
TIER_NIGHTLY=100000
TIER_RELEASE=500000

# Default Rust binary location
DEFAULT_RUST_BIN="$PROJECT_ROOT/tools/rust_fuzz_target/target/release/rust_fuzz_target"

usage() {
    cat <<EOF
GATE-FUZZ — Differential fuzzing CI gate

Usage: $(basename "$0") [OPTIONS]

Options:
  --tier <smoke|nightly|release>   Iteration budget tier (default: smoke)
  --rust-binary <path>             Path to Rust fuzz target binary
  --auto-rust                      Auto-detect Rust binary at default location
  --minimize <path>                Path to fuzz_minimize binary
  --report <path>                  JSONL report output path
  --verbose                        Verbose output
  -h, --help                       Show this help

Tiers:
  smoke     ${TIER_SMOKE} iterations — CI gate, must complete in <60s
  nightly   ${TIER_NIGHTLY} iterations — nightly CI run
  release   ${TIER_RELEASE} iterations — pre-release validation
EOF
    exit 0
}

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --tier)
            TIER="$2"; shift 2 ;;
        --rust-binary)
            RUST_BINARY="$2"; shift 2 ;;
        --auto-rust)
            RUST_BINARY="$DEFAULT_RUST_BIN"; shift ;;
        --minimize)
            MINIMIZE_BINARY="$2"; shift 2 ;;
        --report)
            REPORT_PATH="$2"; shift 2 ;;
        --verbose)
            VERBOSE=1; shift ;;
        -h|--help)
            usage ;;
        *)
            echo "gate_fuzz: unknown option: $1" >&2; exit 1 ;;
    esac
done

# Validate tier
case "$TIER" in
    smoke)   ITERATIONS=$TIER_SMOKE ;;
    nightly) ITERATIONS=$TIER_NIGHTLY ;;
    release) ITERATIONS=$TIER_RELEASE ;;
    *)       echo "gate_fuzz: invalid tier: $TIER (use smoke|nightly|release)" >&2; exit 1 ;;
esac

# Build fuzz harness
echo "[gate-fuzz] Building fuzz harness..."
make -C "$PROJECT_ROOT" fuzz-build 2>&1 | tail -1

FUZZ_BIN="$PROJECT_ROOT/build/fuzz/fuzz_differential"
if [ ! -x "$FUZZ_BIN" ]; then
    echo "[gate-fuzz] ERROR: fuzz_differential not found at $FUZZ_BIN" >&2
    exit 1
fi

# Build minimizer if requested
MIN_BIN="$PROJECT_ROOT/build/fuzz/fuzz_minimize"
if [ -n "$MINIMIZE_BINARY" ]; then
    MIN_BIN="$MINIMIZE_BINARY"
elif [ -z "$MINIMIZE_BINARY" ]; then
    # Auto-build minimizer if source exists
    if [ -f "$PROJECT_ROOT/tests/fuzz/fuzz_minimize.c" ]; then
        make -C "$PROJECT_ROOT" minimize-build 2>&1 | tail -1
        if [ -x "$MIN_BIN" ]; then
            MINIMIZE_BINARY="$MIN_BIN"
        fi
    fi
fi

# Check Rust binary availability
RUST_MODE="skip"
if [ -n "$RUST_BINARY" ]; then
    if [ -x "$RUST_BINARY" ]; then
        RUST_MODE="active"
    else
        echo "[gate-fuzz] SKIP: Rust binary not found at $RUST_BINARY"
        RUST_MODE="skip"
        RUST_BINARY=""
    fi
fi

# Set up report path
TMPDIR_WORK="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_WORK"' EXIT

if [ -z "$REPORT_PATH" ]; then
    REPORT_PATH="$TMPDIR_WORK/gate_fuzz_report.jsonl"
fi

# Emit gate header as JSONL
START_TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "[gate-fuzz] tier=$TIER iterations=$ITERATIONS rust=$RUST_MODE"

# Construct fuzz command
FUZZ_CMD=("$FUZZ_BIN" --iterations "$ITERATIONS" --report "$REPORT_PATH")

if [ -n "$RUST_BINARY" ]; then
    FUZZ_CMD+=(--rust-binary "$RUST_BINARY")
fi

if [ -n "$MINIMIZE_BINARY" ]; then
    FUZZ_CMD+=(--minimize "$MINIMIZE_BINARY")
fi

if [ "$VERBOSE" -eq 1 ]; then
    FUZZ_CMD+=(--verbose)
fi

# Run the fuzzer
echo "[gate-fuzz] Running: ${FUZZ_CMD[*]}"
FUZZ_EXIT=0
"${FUZZ_CMD[@]}" 2>&1 || FUZZ_EXIT=$?

END_TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# Parse the summary from the report
if [ -f "$REPORT_PATH" ]; then
    SUMMARY_LINE=$(command grep '"kind":"summary"' "$REPORT_PATH" 2>/dev/null || true)
fi

# Emit gate JSONL verdict
GATE_PASS=true
GATE_REASON="all checks passed"

if [ "$FUZZ_EXIT" -ne 0 ]; then
    GATE_PASS=false
    GATE_REASON="fuzz_differential exited with code $FUZZ_EXIT"
fi

# Check for determinism failures or crashes in summary
if [ -n "${SUMMARY_LINE:-}" ]; then
    DET_FAILURES=$(echo "$SUMMARY_LINE" | python3 -c "import json,sys; d=json.loads(sys.stdin.readline()); print(d.get('determinism_failures',0))" 2>/dev/null || echo "0")
    CRASHES=$(echo "$SUMMARY_LINE" | python3 -c "import json,sys; d=json.loads(sys.stdin.readline()); print(d.get('crashes',0))" 2>/dev/null || echo "0")
    RUST_DIV=$(echo "$SUMMARY_LINE" | python3 -c "import json,sys; d=json.loads(sys.stdin.readline()); print(d.get('rust_divergences',0))" 2>/dev/null || echo "0")
    ITERS_SEC=$(echo "$SUMMARY_LINE" | python3 -c "import json,sys; d=json.loads(sys.stdin.readline()); print(d.get('iterations_per_sec',0))" 2>/dev/null || echo "0")

    if [ "$DET_FAILURES" -gt 0 ] 2>/dev/null; then
        GATE_PASS=false
        GATE_REASON="$DET_FAILURES determinism failure(s)"
    fi
    if [ "$CRASHES" -gt 0 ] 2>/dev/null; then
        GATE_PASS=false
        GATE_REASON="$CRASHES crash(es)"
    fi
fi

# Emit structured gate result
GATE_JSON=$(python3 -c "
import json, sys
d = {
    'kind': 'gate_fuzz_result',
    'tier': '$TIER',
    'iterations': $ITERATIONS,
    'rust_mode': '$RUST_MODE',
    'gate_pass': $( [ "$GATE_PASS" = "true" ] && echo "True" || echo "False" ),
    'reason': '$GATE_REASON',
    'start_ts': '$START_TS',
    'end_ts': '$END_TS',
    'determinism_failures': int('${DET_FAILURES:-0}'),
    'crashes': int('${CRASHES:-0}'),
    'rust_divergences': int('${RUST_DIV:-0}'),
    'iterations_per_sec': float('${ITERS_SEC:-0}'),
    'report_path': '$REPORT_PATH'
}
print(json.dumps(d))
")

echo "$GATE_JSON" >> "$REPORT_PATH"

# Print verdict
if [ "$GATE_PASS" = "true" ]; then
    echo ""
    echo "[gate-fuzz] PASS: $TIER tier ($ITERATIONS iterations, rust=$RUST_MODE)"
    if [ -n "${ITERS_SEC:-}" ]; then
        echo "[gate-fuzz] Performance: ${ITERS_SEC} iterations/sec"
    fi
    if [ "$RUST_MODE" = "active" ] && [ "${RUST_DIV:-0}" = "0" ]; then
        echo "[gate-fuzz] Rust comparison: 0 divergences"
    fi
    exit 0
else
    echo ""
    echo "[gate-fuzz] FAIL: $GATE_REASON"
    echo "[gate-fuzz] Report: $REPORT_PATH"
    exit 1
fi
