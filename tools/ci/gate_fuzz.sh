#!/bin/bash
# gate_fuzz.sh — GATE-FUZZ CI gate for C determinism and crash checks
#
# Wraps the Makefile fuzz targets with tier-based iteration budgets,
# C self-consistency checks and structured JSONL output. Rust parity runs
# separately through make fuzz-differential and the live twin_run DSL oracle.
#
# Usage:
#   tools/ci/gate_fuzz.sh --tier smoke              # CI: 100 iterations, <60s
#   tools/ci/gate_fuzz.sh --tier nightly             # Nightly: 100K iterations
#   tools/ci/gate_fuzz.sh --tier release             # Release: 500K iterations
#   tools/ci/gate_fuzz.sh --tier smoke --report /tmp/fuzz.jsonl
#
# Bead: bd-rkql.7

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Defaults
TIER="smoke"
REPORT_PATH=""
MINIMIZE_BINARY=""
VERBOSE=0
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
if [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$PROJECT_ROOT/$BUILD_DIR"
fi

# Tier iteration budgets
TIER_SMOKE=100
TIER_NIGHTLY=100000
TIER_RELEASE=500000

usage() {
    cat <<EOF
GATE-FUZZ — C determinism and crash CI gate

Usage: $(basename "$0") [OPTIONS]

Options:
  --tier <smoke|nightly|release>   Iteration budget tier (default: smoke)
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
make -C "$PROJECT_ROOT" -j2 BUILD_DIR="$BUILD_DIR" fuzz-build

FUZZ_BIN="$BUILD_DIR/fuzz/fuzz_differential"
if [ ! -x "$FUZZ_BIN" ]; then
    echo "[gate-fuzz] ERROR: fuzz_differential not found at $FUZZ_BIN" >&2
    exit 1
fi

# Build minimizer if requested
MIN_BIN="$BUILD_DIR/fuzz/fuzz_minimize"
if [ -n "$MINIMIZE_BINARY" ]; then
    MIN_BIN="$MINIMIZE_BINARY"
elif [ -z "$MINIMIZE_BINARY" ]; then
    # Auto-build minimizer if source exists
    if [ -f "$PROJECT_ROOT/tests/fuzz/fuzz_minimize.c" ]; then
        make -C "$PROJECT_ROOT" -j2 BUILD_DIR="$BUILD_DIR" minimize-build
        if [ -x "$MIN_BIN" ]; then
            MINIMIZE_BINARY="$MIN_BIN"
        fi
    fi
fi

if [ -n "$MINIMIZE_BINARY" ] && [ ! -x "$MINIMIZE_BINARY" ]; then
    echo "[gate-fuzz] ERROR: minimizer is not executable: $MINIMIZE_BINARY" >&2
    exit 1
fi

# Set up report path
if [ -z "$REPORT_PATH" ]; then
    TMPDIR_WORK="$(mktemp -d)"
    REPORT_PATH="$TMPDIR_WORK/gate_fuzz_report.jsonl"
fi

# Emit gate header as JSONL
START_TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "[gate-fuzz] tier=$TIER iterations=$ITERATIONS checks=C-determinism,crashes"

# Construct fuzz command
FUZZ_CMD=("$FUZZ_BIN" --iterations "$ITERATIONS" --report "$REPORT_PATH")

if [ -n "$MINIMIZE_BINARY" ]; then
    FUZZ_CMD+=(--minimize "$MINIMIZE_BINARY")
fi

if [ "$VERBOSE" -eq 1 ]; then
    FUZZ_CMD+=(--verbose)
fi

# Run the fuzzer
echo "[gate-fuzz] Running: ${FUZZ_CMD[*]}"
FUZZ_EXIT=0
"${FUZZ_CMD[@]}" || FUZZ_EXIT=$?

END_TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# Require one valid summary and the entire requested budget before passing.
python3 -I - "$REPORT_PATH" "$TIER" "$ITERATIONS" "$FUZZ_EXIT" "$START_TS" "$END_TS" <<'PY'
import json, math, sys
report, tier, iterations, fuzz_exit, start_ts, end_ts = sys.argv[1:]
iterations, fuzz_exit = int(iterations), int(fuzz_exit)
summary = {}
reason = "all C checks passed"
passed = False
try:
    with open(report) as stream:
        records = [json.loads(line) for line in stream if line.strip()]
    summaries = [record for record in records if record.get('kind') == 'summary']
    if len(summaries) != 1:
        raise ValueError("expected exactly one fuzz summary")
    summary = summaries[0]
    for key in ('iterations', 'determinism_failures', 'crashes'):
        value = summary.get(key)
        if type(value) is not int or value < 0:
            raise ValueError(f"invalid summary {key}")
    rate = summary.get('iterations_per_sec')
    if type(rate) not in (int, float) or not math.isfinite(rate) or rate < 0:
        raise ValueError("invalid summary iterations_per_sec")
    if summary['iterations'] != iterations or iterations <= 0:
        reason = "fuzzer did not complete the requested iteration budget"
    elif fuzz_exit != 0:
        reason = f"fuzz_differential exited with code {fuzz_exit}"
    elif summary['determinism_failures'] != 0:
        reason = f"{summary['determinism_failures']} determinism failure(s)"
    elif summary['crashes'] != 0:
        reason = f"{summary['crashes']} crash(es)"
    else:
        passed = True
except (OSError, ValueError, AttributeError) as error:
    reason = f"invalid or missing fuzz report: {error}"
    print(f"[gate-fuzz] {reason}", file=sys.stderr)

d = {
    'kind': 'gate_fuzz_result',
    'tier': tier,
    'iterations': iterations,
    'completed_iterations': summary.get('iterations'),
    'gate_pass': passed,
    'reason': reason,
    'start_ts': start_ts,
    'end_ts': end_ts,
    'determinism_failures': summary.get('determinism_failures'),
    'crashes': summary.get('crashes'),
    'iterations_per_sec': summary.get('iterations_per_sec'),
    'report_path': report
}
try:
    with open(report, 'a') as stream:
        stream.write(json.dumps(d) + '\n')
except OSError as error:
    passed = False
    reason = f"cannot write gate verdict: {error}"
    print(f"[gate-fuzz] {reason}", file=sys.stderr)
if passed:
    print(f"[gate-fuzz] PASS: {tier} tier ({iterations} C iterations)")
    print(f"[gate-fuzz] Performance: {summary['iterations_per_sec']} iterations/sec")
else:
    print(f"[gate-fuzz] FAIL: {reason}")
print(f"[gate-fuzz] Report: {report}")
sys.exit(0 if passed else 1)
PY
