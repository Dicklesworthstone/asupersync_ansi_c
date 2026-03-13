#!/usr/bin/env bash
# run_qemu_baremetal.sh — Run bare-metal binaries under QEMU semihosting
#
# Extends the QEMU smoke test pattern for bare-metal ARM and RISC-V targets.
# Uses semihosting (ARM) and test finisher (RISC-V) for exit signaling.
#
# Usage:
#   ./tools/ci/run_qemu_baremetal.sh [OPTIONS]
#
# Options:
#   --build-first    Build smoke test binaries before running
#   --dry-run        Show commands without executing
#   --strict         Fail on any SKIP (require all toolchains)
#   --timeout SEC    Per-binary timeout (default: 10)
#   --target TARGET  Run only the specified target (arm-m4, arm-m0, riscv32, riscv64)
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
EMBEDDED_DIR="${PROJECT_ROOT}/tests/embedded"
LINKER_DIR="${PROJECT_ROOT}/tools/embedded"
LOG_DIR="${BUILD_DIR}/qemu-baremetal"

mkdir -p "$LOG_DIR"

JSONL_LOG="$LOG_DIR/results.jsonl"
: > "$JSONL_LOG"

# Defaults
BUILD_FIRST=0
DRY_RUN=0
STRICT=0
TIMEOUT=10
TARGET_FILTER=""

while [ $# -gt 0 ]; do
    case "$1" in
        --build-first)  BUILD_FIRST=1; shift ;;
        --dry-run)      DRY_RUN=1; shift ;;
        --strict)       STRICT=1; shift ;;
        --timeout)      TIMEOUT="$2"; shift 2 ;;
        --target)       TARGET_FILTER="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

PASS=0
FAIL=0
SKIP=0

ts() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }

emit() {
    echo "$1" >> "$JSONL_LOG"
}

# Build and run one target
run_target() {
    local name="$1"
    local cc="$2"
    local arch_flags="$3"
    local specs="$4"
    local linker="$5"
    local qemu_cmd="$6"
    local qemu_args="$7"

    echo "--- $name ---"

    # Check toolchain
    local cc_bin
    cc_bin=$(echo "$cc" | awk '{print $1}')
    if ! command -v "$cc_bin" >/dev/null 2>&1; then
        if [ "$STRICT" -eq 1 ]; then
            echo "  FAIL: $cc_bin not found (strict mode)"
            emit "{\"ts\":\"$(ts)\",\"target\":\"$name\",\"status\":\"fail\",\"reason\":\"toolchain not found\"}"
            FAIL=$((FAIL + 1))
        else
            echo "  SKIP: $cc_bin not found"
            emit "{\"ts\":\"$(ts)\",\"target\":\"$name\",\"status\":\"skip\",\"reason\":\"toolchain not found\"}"
            SKIP=$((SKIP + 1))
        fi
        return
    fi

    # Check QEMU
    local qemu_bin
    qemu_bin=$(echo "$qemu_cmd" | awk '{print $1}')
    if ! command -v "$qemu_bin" >/dev/null 2>&1; then
        if [ "$STRICT" -eq 1 ]; then
            echo "  FAIL: $qemu_bin not found (strict mode)"
            emit "{\"ts\":\"$(ts)\",\"target\":\"$name\",\"status\":\"fail\",\"reason\":\"QEMU not found\"}"
            FAIL=$((FAIL + 1))
        else
            echo "  SKIP: $qemu_bin not found"
            emit "{\"ts\":\"$(ts)\",\"target\":\"$name\",\"status\":\"skip\",\"reason\":\"QEMU not found\"}"
            SKIP=$((SKIP + 1))
        fi
        return
    fi

    local elf="$BUILD_DIR/qemu-baremetal/${name}.elf"
    mkdir -p "$(dirname "$elf")"

    # Build
    if [ "$BUILD_FIRST" -eq 1 ] || [ ! -f "$elf" ]; then
        local startup="$EMBEDDED_DIR/startup_${name%%_*}_semihosting.s"
        # Determine startup file based on target
        case "$name" in
            arm-*)   startup="$EMBEDDED_DIR/startup_arm_semihosting.s" ;;
            riscv*)  startup="$EMBEDDED_DIR/startup_riscv_semihosting.s" ;;
        esac

        local build_cmd="$cc $arch_flags $specs -std=c99 -O2 -T $linker -o $elf $startup $EMBEDDED_DIR/smoke_baremetal.c -nostartfiles"

        if [ "$DRY_RUN" -eq 1 ]; then
            echo "  [dry-run] $build_cmd"
            emit "{\"ts\":\"$(ts)\",\"target\":\"$name\",\"status\":\"dry-run\",\"build_cmd\":\"$build_cmd\"}"
            return
        fi

        echo "  Building: $cc_bin → $elf"
        if ! eval "$build_cmd" 2>&1; then
            echo "  FAIL: build failed"
            emit "{\"ts\":\"$(ts)\",\"target\":\"$name\",\"status\":\"fail\",\"reason\":\"build failed\"}"
            FAIL=$((FAIL + 1))
            return
        fi
    fi

    # Run under QEMU
    local run_cmd="timeout $TIMEOUT $qemu_cmd $qemu_args -kernel $elf"

    if [ "$DRY_RUN" -eq 1 ]; then
        echo "  [dry-run] $run_cmd"
        emit "{\"ts\":\"$(ts)\",\"target\":\"$name\",\"status\":\"dry-run\",\"run_cmd\":\"$run_cmd\"}"
        return
    fi

    echo "  Running: $qemu_bin (timeout=${TIMEOUT}s)"
    local start_time
    start_time=$(date +%s%N)

    local exit_code=0
    eval "$run_cmd" 2>/dev/null || exit_code=$?

    local end_time
    end_time=$(date +%s%N)
    local duration_ms=$(( (end_time - start_time) / 1000000 ))

    if [ "$exit_code" -eq 0 ]; then
        echo "  PASS (${duration_ms}ms)"
        emit "{\"ts\":\"$(ts)\",\"target\":\"$name\",\"status\":\"pass\",\"duration_ms\":$duration_ms,\"exit_code\":0}"
        PASS=$((PASS + 1))
    elif [ "$exit_code" -eq 124 ]; then
        echo "  FAIL: timeout after ${TIMEOUT}s"
        emit "{\"ts\":\"$(ts)\",\"target\":\"$name\",\"status\":\"fail\",\"reason\":\"timeout\",\"duration_ms\":$duration_ms}"
        FAIL=$((FAIL + 1))
    else
        echo "  FAIL: exit code $exit_code"
        emit "{\"ts\":\"$(ts)\",\"target\":\"$name\",\"status\":\"fail\",\"reason\":\"exit_code\",\"exit_code\":$exit_code,\"duration_ms\":$duration_ms}"
        FAIL=$((FAIL + 1))
    fi
}

echo "=========================================="
echo "QEMU Bare-Metal Smoke Tests (bd-aw69.3)"
echo "=========================================="
echo ""

should_run() {
    [ -z "$TARGET_FILTER" ] || [ "$TARGET_FILTER" = "$1" ]
}

# ARM Cortex-M4
if should_run "arm-m4"; then
    run_target "arm-m4" \
        "arm-none-eabi-gcc" \
        "-mcpu=cortex-m4 -mthumb" \
        "--specs=nosys.specs" \
        "$LINKER_DIR/qemu-arm.ld" \
        "qemu-system-arm" \
        "-machine lm3s6965evb -cpu cortex-m4 -nographic -semihosting-config enable=on,target=native"
fi

# ARM Cortex-M0 (uses cortex-m3 QEMU CPU — M0 is a subset)
if should_run "arm-m0"; then
    run_target "arm-m0" \
        "arm-none-eabi-gcc" \
        "-mcpu=cortex-m0 -mthumb" \
        "--specs=nosys.specs" \
        "$LINKER_DIR/qemu-arm.ld" \
        "qemu-system-arm" \
        "-machine lm3s6965evb -cpu cortex-m3 -nographic -semihosting-config enable=on,target=native"
fi

# RISC-V 32-bit
if should_run "riscv32"; then
    run_target "riscv32" \
        "riscv64-unknown-elf-gcc" \
        "-march=rv32imac -mabi=ilp32" \
        "--specs=picolibc.specs" \
        "$LINKER_DIR/riscv32.ld" \
        "qemu-system-riscv32" \
        "-machine virt -nographic -bios none"
fi

# RISC-V 64-bit
if should_run "riscv64"; then
    run_target "riscv64" \
        "riscv64-unknown-elf-gcc" \
        "-march=rv64imac -mabi=lp64 -mcmodel=medany" \
        "--specs=picolibc.specs" \
        "$LINKER_DIR/riscv64.ld" \
        "qemu-system-riscv64" \
        "-machine virt -nographic -bios none"
fi

echo ""
echo "=========================================="
echo "Summary: PASS=$PASS  FAIL=$FAIL  SKIP=$SKIP"
echo "JSONL log: $JSONL_LOG"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo "RESULT: FAIL"
    exit 1
elif [ "$SKIP" -gt 0 ] && [ "$STRICT" -eq 1 ]; then
    echo "RESULT: FAIL (strict mode, some targets skipped)"
    exit 1
else
    echo "RESULT: PASS"
    exit 0
fi
