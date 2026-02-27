#!/bin/sh
# =============================================================================
# run_qemu_smoke.sh — QEMU user-mode execution harness for router-class targets
#
# Runs cross-compiled asx test binaries under QEMU user-mode emulation.
# Validates runtime behavior on target-like environments (mipsel, armv7, aarch64).
#
# Prerequisites:
#   - Cross-compiled test binaries in build/tests/ (via make test-unit TARGET=...)
#   - QEMU user-mode emulators (qemu-mipsel-static, qemu-arm-static, qemu-aarch64-static)
#   - For static-linked binaries, QEMU static variants are required.
#
# Usage:
#   tools/ci/run_qemu_smoke.sh                    # run all available targets
#   tools/ci/run_qemu_smoke.sh --target mipsel    # run specific target
#   tools/ci/run_qemu_smoke.sh --dry-run          # print plan without executing
#   tools/ci/run_qemu_smoke.sh --build-first      # cross-compile then run
#
# Output format (JSONL to log file + parseable table to stdout):
#   PASS|FAIL|SKIP  target  test_name  duration_ms  [reason]
#
# Exit code: 0 if all available targets pass, nonzero otherwise.
# Missing QEMU or toolchains cause SKIP (not FAIL) unless --strict is set.
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ARTIFACT_DIR="$PROJECT_ROOT/tools/ci/artifacts/qemu"
RUN_ID="qemu-$(date -u +%Y%m%dT%H%M%SZ)"

# File-based counters (avoids subshell variable scoping issues in POSIX sh)
COUNTER_DIR=""

# Options
DRY_RUN=0
BUILD_FIRST=0
STRICT=0
TARGET_FILTER=""
TIMEOUT_SEC=30
LOG_FILE=""

# -----------------------------------------------------------------------
# Target definitions
# -----------------------------------------------------------------------
# Format: arch:qemu_binary:cross_prefix:qemu_flags
TARGET_MIPSEL="mipsel:qemu-mipsel-static:mipsel-openwrt-linux-musl:"
TARGET_ARMV7="armv7:qemu-arm-static:armv7-openwrt-linux-muslgnueabi:"
TARGET_AARCH64="aarch64:qemu-aarch64-static:aarch64-openwrt-linux-musl:"

# -----------------------------------------------------------------------
# Argument parsing
# -----------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)    DRY_RUN=1 ;;
        --build-first) BUILD_FIRST=1 ;;
        --strict)     STRICT=1 ;;
        --target)     TARGET_FILTER="$2"; shift ;;
        --timeout)    TIMEOUT_SEC="$2"; shift ;;
        --run-id)     RUN_ID="$2"; shift ;;
        --log-file)   LOG_FILE="$2"; shift ;;
        -h|--help)
            sed -n '2,/^$/s/^# //p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
    shift
done

mkdir -p "$ARTIFACT_DIR"
if [ -z "$LOG_FILE" ]; then
    LOG_FILE="$ARTIFACT_DIR/${RUN_ID}.jsonl"
fi
: >"$LOG_FILE"

# Set up file-based counters
COUNTER_DIR="$(mktemp -d)"
echo 0 >"$COUNTER_DIR/pass"
echo 0 >"$COUNTER_DIR/fail"
echo 0 >"$COUNTER_DIR/skip"
trap 'rm -rf "$COUNTER_DIR"' EXIT

# -----------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------

json_escape() {
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/	/\\t/g'
}

emit_jsonl() {
    target="$1"; test_name="$2"; status="$3"; exit_code="$4"
    duration_ms="$5"; diagnostic="$6"; output_file="$7"

    printf '{"kind":"qemu-smoke","run_id":"%s","target":"%s","test":"%s","status":"%s","exit_code":%s,"duration_ms":%s,"diagnostic":"%s","output":"%s"}\n' \
        "$(json_escape "$RUN_ID")" \
        "$(json_escape "$target")" \
        "$(json_escape "$test_name")" \
        "$(json_escape "$status")" \
        "$exit_code" \
        "$duration_ms" \
        "$(json_escape "$diagnostic")" \
        "$(json_escape "$output_file")" >>"$LOG_FILE"
}

inc_counter() {
    counter_file="$COUNTER_DIR/$1"
    val="$(cat "$counter_file")"
    echo $((val + 1)) >"$counter_file"
}

get_counter() {
    cat "$COUNTER_DIR/$1"
}

log_result() {
    status="$1"; target="$2"; test_name="$3"; duration="$4"; reason="${5:-}"
    printf "%-4s  %-12s  %-30s  %6s ms  %s\n" \
        "$status" "$target" "$test_name" "$duration" "$reason"
    case "$status" in
        PASS) inc_counter pass ;;
        FAIL) inc_counter fail ;;
        SKIP) inc_counter skip ;;
    esac
}

has_cmd() {
    command -v "$1" >/dev/null 2>&1
}

# Portable millisecond-resolution timing (falls back to second resolution)
time_ms() {
    if date +%s%N >/dev/null 2>&1; then
        date +%s%N | cut -c1-13
    else
        echo "$(($(date +%s) * 1000))"
    fi
}

# Run a command with timeout (portable: tries timeout, then plain exec)
run_with_timeout() {
    tsec="$1"
    shift
    if has_cmd timeout; then
        timeout "$tsec" "$@"
    else
        "$@"
    fi
}

# -----------------------------------------------------------------------
# Cross-build a target (if --build-first)
# -----------------------------------------------------------------------
cross_build_target() {
    arch="$1"
    cross_prefix="$2"
    compiler="${cross_prefix}-gcc"

    if ! has_cmd "$compiler"; then
        return 1
    fi

    build_log="$ARTIFACT_DIR/${RUN_ID}-build-${arch}.log"
    make -C "$PROJECT_ROOT" clean build test-unit \
        TARGET="$cross_prefix" \
        PROFILE=EMBEDDED_ROUTER \
        CODEC=JSON \
        DETERMINISTIC=1 \
        >"$build_log" 2>&1
}

# -----------------------------------------------------------------------
# Find test binaries for a target
# -----------------------------------------------------------------------
find_test_binaries() {
    build_test_dir="$PROJECT_ROOT/build/tests"
    if [ -d "$build_test_dir" ]; then
        find "$build_test_dir" -type f -executable 2>/dev/null | sort || true
    fi
}

# -----------------------------------------------------------------------
# Run a single test binary under QEMU
# -----------------------------------------------------------------------
run_qemu_test() {
    arch="$1"
    qemu_bin="$2"
    qemu_flags="$3"
    test_binary="$4"
    test_name="$(basename "$test_binary")"

    output_file="$ARTIFACT_DIR/${RUN_ID}-${arch}-${test_name}.out"
    stderr_file="$ARTIFACT_DIR/${RUN_ID}-${arch}-${test_name}.err"

    if [ "$DRY_RUN" -eq 1 ]; then
        log_result "SKIP" "$arch" "$test_name" "0" "dry-run"
        emit_jsonl "$arch" "$test_name" "planned" 0 0 "dry-run: execution skipped" "$output_file"
        return 0
    fi

    start_ms="$(time_ms)"

    # Run under QEMU with timeout
    set +e
    if [ -n "$qemu_flags" ]; then
        run_with_timeout "$TIMEOUT_SEC" "$qemu_bin" $qemu_flags "$test_binary" \
            >"$output_file" 2>"$stderr_file"
    else
        run_with_timeout "$TIMEOUT_SEC" "$qemu_bin" "$test_binary" \
            >"$output_file" 2>"$stderr_file"
    fi
    rc=$?
    set -e

    end_ms="$(time_ms)"
    duration=$((end_ms - start_ms))

    if [ "$rc" -eq 0 ]; then
        log_result "PASS" "$arch" "$test_name" "$duration"
        emit_jsonl "$arch" "$test_name" "pass" "$rc" "$duration" "" "$output_file"
    elif [ "$rc" -eq 124 ]; then
        log_result "FAIL" "$arch" "$test_name" "$duration" "timeout (${TIMEOUT_SEC}s)"
        emit_jsonl "$arch" "$test_name" "fail" "$rc" "$duration" "timeout after ${TIMEOUT_SEC}s" "$output_file"
        return 1
    else
        # Extract diagnostic from stderr
        diagnostic=""
        if [ -f "$stderr_file" ] && [ -s "$stderr_file" ]; then
            diagnostic="$(tail -5 "$stderr_file" | tr '\n' ' ' | cut -c1-200)"
        fi
        log_result "FAIL" "$arch" "$test_name" "$duration" "exit=$rc $diagnostic"
        emit_jsonl "$arch" "$test_name" "fail" "$rc" "$duration" "$diagnostic" "$output_file"
        return 1
    fi
}

# -----------------------------------------------------------------------
# Process a single target architecture
# -----------------------------------------------------------------------
process_target() {
    line="$1"
    arch="$(echo "$line" | cut -d: -f1)"
    qemu_bin="$(echo "$line" | cut -d: -f2)"
    cross_prefix="$(echo "$line" | cut -d: -f3)"
    qemu_flags="$(echo "$line" | cut -d: -f4)"

    # Filter check
    if [ -n "$TARGET_FILTER" ] && [ "$arch" != "$TARGET_FILTER" ]; then
        return 0
    fi

    # Check for QEMU binary
    if ! has_cmd "$qemu_bin"; then
        if [ "$STRICT" -eq 1 ]; then
            log_result "FAIL" "$arch" "(all)" "0" "$qemu_bin not found"
            emit_jsonl "$arch" "(all)" "fail" 127 0 "$qemu_bin not found (strict mode)" ""
        else
            log_result "SKIP" "$arch" "(all)" "0" "$qemu_bin not found"
            emit_jsonl "$arch" "(all)" "skip" 0 0 "$qemu_bin not available" ""
        fi
        return 0
    fi

    # Optionally cross-compile first
    if [ "$BUILD_FIRST" -eq 1 ]; then
        compiler="${cross_prefix}-gcc"
        if ! has_cmd "$compiler"; then
            log_result "SKIP" "$arch" "(all)" "0" "cross-compiler $compiler not found"
            emit_jsonl "$arch" "(all)" "skip" 0 0 "cross-compiler not available" ""
            return 0
        fi

        if ! cross_build_target "$arch" "$cross_prefix"; then
            log_result "FAIL" "$arch" "(build)" "0" "cross-build failed"
            emit_jsonl "$arch" "(build)" "fail" 1 0 "cross-build failed" \
                "$ARTIFACT_DIR/${RUN_ID}-build-${arch}.log"
            return 1
        fi
    fi

    # Find and run test binaries
    test_bins="$(find_test_binaries)"
    if [ -z "$test_bins" ]; then
        log_result "SKIP" "$arch" "(all)" "0" "no test binaries found"
        emit_jsonl "$arch" "(all)" "skip" 0 0 "no cross-compiled test binaries in build/tests/" ""
        return 0
    fi

    target_failed=0
    for bin in $test_bins; do
        if [ -n "$bin" ]; then
            run_qemu_test "$arch" "$qemu_bin" "$qemu_flags" "$bin" || target_failed=1
        fi
    done

    return $target_failed
}

# -----------------------------------------------------------------------
# Main execution
# -----------------------------------------------------------------------

echo "================================================================"
echo "asx QEMU smoke test harness"
echo "================================================================"
echo "run_id:    $RUN_ID"
echo "artifacts: $ARTIFACT_DIR/"
echo "timeout:   ${TIMEOUT_SEC}s per test"
echo "mode:      $([ "$DRY_RUN" -eq 1 ] && echo "dry-run" || echo "live")"
echo ""
printf "%-4s  %-12s  %-30s  %9s  %s\n" \
    "STAT" "TARGET" "TEST" "DURATION" "NOTE"
echo "----  ------------  ------------------------------  ---------  ----"

# Process each target directly (no subshell pipeline)
process_target "$TARGET_MIPSEL" || true
process_target "$TARGET_ARMV7" || true
process_target "$TARGET_AARCH64" || true

# Read final counters
PASS_COUNT="$(get_counter pass)"
FAIL_COUNT="$(get_counter fail)"
SKIP_COUNT="$(get_counter skip)"

# -----------------------------------------------------------------------
# Artifact bundle: config snapshot
# -----------------------------------------------------------------------
config_snapshot="$ARTIFACT_DIR/${RUN_ID}-config.json"
cat >"$config_snapshot" <<SNAPSHOT
{
  "run_id": "${RUN_ID}",
  "harness_version": "1.0.0",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "host_arch": "$(uname -m)",
  "host_os": "$(uname -s)",
  "host_kernel": "$(uname -r)",
  "qemu_mipsel": "$(qemu-mipsel-static --version 2>/dev/null | head -1 || echo 'not available')",
  "qemu_arm": "$(qemu-arm-static --version 2>/dev/null | head -1 || echo 'not available')",
  "qemu_aarch64": "$(qemu-aarch64-static --version 2>/dev/null | head -1 || echo 'not available')",
  "dry_run": ${DRY_RUN},
  "strict": ${STRICT},
  "timeout_sec": ${TIMEOUT_SEC},
  "target_filter": "${TARGET_FILTER}"
}
SNAPSHOT

# -----------------------------------------------------------------------
# Trace digest: SHA-256 of all output files for determinism validation
# -----------------------------------------------------------------------
digest_file="$ARTIFACT_DIR/${RUN_ID}-trace-digest.txt"
: >"$digest_file"
if has_cmd sha256sum; then
    find "$ARTIFACT_DIR" -name "${RUN_ID}-*.out" -type f 2>/dev/null | sort | while read -r f; do
        sha256sum "$f" >>"$digest_file"
    done
elif has_cmd shasum; then
    find "$ARTIFACT_DIR" -name "${RUN_ID}-*.out" -type f 2>/dev/null | sort | while read -r f; do
        shasum -a 256 "$f" >>"$digest_file"
    done
fi

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------
echo ""
echo "================================================================"
echo "QEMU smoke summary: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
echo "Log file:  $LOG_FILE"
echo "Config:    $config_snapshot"
echo "Digest:    $digest_file"
echo "Artifacts: $ARTIFACT_DIR/"
echo "================================================================"

if [ "$FAIL_COUNT" -gt 0 ]; then
    echo "QEMU SMOKE RESULT: FAIL"
    exit 1
else
    echo "QEMU SMOKE RESULT: PASS"
    exit 0
fi
