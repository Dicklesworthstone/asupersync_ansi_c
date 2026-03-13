#!/usr/bin/env bash
# run_qemu_e2e.sh — Cross-compile and run asupersync smoke tests under QEMU
#
# Builds embedded_smoke.c linked against libasx.a for each bare-metal target,
# then runs under QEMU semihosting to verify runtime correctness.
#
# Usage:
#   ./tools/ci/run_qemu_e2e.sh [OPTIONS]
#
# Options:
#   --dry-run        Show commands without executing
#   --strict         Fail on any SKIP
#   --timeout SEC    Per-binary timeout (default: 30)
#   --target TARGET  Run only one target (arm, riscv32, riscv64)
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
CROSS_DIR="${BUILD_DIR}/cross"
E2E_DIR="${BUILD_DIR}/qemu-e2e"
EMBEDDED_DIR="${PROJECT_ROOT}/tests/embedded"
LINKER_DIR="${PROJECT_ROOT}/tools/embedded"

mkdir -p "$E2E_DIR"

JSONL_LOG="$E2E_DIR/results.jsonl"
: > "$JSONL_LOG"

DRY_RUN=0
STRICT=0
TIMEOUT=30
TARGET_FILTER=""

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)    DRY_RUN=1; shift ;;
        --strict)     STRICT=1; shift ;;
        --timeout)    TIMEOUT="$2"; shift 2 ;;
        --target)     TARGET_FILTER="$2"; shift 2 ;;
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

COMMON_WARN="-Wno-type-limits -Wno-unused-value -Wno-format"
COMMON_INC="-I${PROJECT_ROOT}/include -I${PROJECT_ROOT}/src"

# Build and run one test for one target
run_e2e() {
    local name="$1"       # e.g. "arm"
    local test_src="$2"   # e.g. "embedded_smoke"
    local cc="$3"
    local arch_flags="$4"
    local specs="$5"
    local linker="$6"
    local lib_suffix="$7" # e.g. "arm-m4-free"
    local qemu_cmd="$8"
    local qemu_args="$9"
    local profile="${10}"

    local test_name="${test_src}"
    local elf="$E2E_DIR/${name}-${test_src}.elf"

    echo "  [$name] $test_src..."

    # Check toolchain
    if ! command -v "$cc" >/dev/null 2>&1; then
        local msg="toolchain $cc not found"
        if [ "$STRICT" -eq 1 ]; then
            echo "    FAIL: $msg (strict)"
            emit "{\"ts\":\"$(ts)\",\"test\":\"$test_src\",\"target\":\"$name\",\"status\":\"fail\",\"reason\":\"$msg\"}"
            FAIL=$((FAIL + 1))
        else
            echo "    SKIP: $msg"
            emit "{\"ts\":\"$(ts)\",\"test\":\"$test_src\",\"target\":\"$name\",\"status\":\"skip\",\"reason\":\"$msg\"}"
            SKIP=$((SKIP + 1))
        fi
        return
    fi

    # Check QEMU
    if ! command -v "$qemu_cmd" >/dev/null 2>&1; then
        echo "    SKIP: $qemu_cmd not found"
        emit "{\"ts\":\"$(ts)\",\"test\":\"$test_src\",\"target\":\"$name\",\"status\":\"skip\",\"reason\":\"QEMU not found\"}"
        SKIP=$((SKIP + 1))
        return
    fi

    # Check library
    local lib_archive="$CROSS_DIR/$lib_suffix/lib/libasx.a"
    if [ ! -f "$lib_archive" ]; then
        echo "    SKIP: $lib_archive not found (run make cross-baremetal-all first)"
        emit "{\"ts\":\"$(ts)\",\"test\":\"$test_src\",\"target\":\"$name\",\"status\":\"skip\",\"reason\":\"library not built\"}"
        SKIP=$((SKIP + 1))
        return
    fi

    # Determine startup file
    local startup
    case "$name" in
        arm*)    startup="$EMBEDDED_DIR/startup_arm_semihosting.s" ;;
        riscv*)  startup="$EMBEDDED_DIR/startup_riscv_semihosting.s" ;;
    esac

    if [ "$DRY_RUN" -eq 1 ]; then
        echo "    [dry-run] would build and run"
        return
    fi

    # Build
    if ! $cc $arch_flags $specs -std=c99 -O2 $COMMON_WARN $COMMON_INC \
         -DASX_PROFILE_$profile \
         -T "$linker" \
         -o "$elf" \
         "$startup" \
         "$EMBEDDED_DIR/${test_src}.c" \
         "$EMBEDDED_DIR/stubs_baremetal.c" \
         -L"$(dirname "$lib_archive")" \
         -lasx -nostartfiles -lgcc 2>/dev/null; then
        echo "    FAIL: build failed"
        emit "{\"ts\":\"$(ts)\",\"test\":\"$test_src\",\"target\":\"$name\",\"status\":\"fail\",\"reason\":\"build failed\"}"
        FAIL=$((FAIL + 1))
        return
    fi

    # Run under QEMU
    local start_ns
    start_ns=$(date +%s%N)

    local exit_code=0
    timeout "$TIMEOUT" $qemu_cmd $qemu_args -kernel "$elf" 2>/dev/null || exit_code=$?

    local end_ns
    end_ns=$(date +%s%N)
    local duration_ms=$(( (end_ns - start_ns) / 1000000 ))

    if [ "$exit_code" -eq 0 ]; then
        echo "    PASS (${duration_ms}ms)"
        emit "{\"ts\":\"$(ts)\",\"test\":\"$test_src\",\"target\":\"$name\",\"status\":\"pass\",\"exit_code\":0,\"duration_ms\":$duration_ms}"
        PASS=$((PASS + 1))
    elif [ "$exit_code" -eq 124 ]; then
        echo "    FAIL: timeout after ${TIMEOUT}s"
        emit "{\"ts\":\"$(ts)\",\"test\":\"$test_src\",\"target\":\"$name\",\"status\":\"fail\",\"reason\":\"timeout\",\"duration_ms\":$duration_ms}"
        FAIL=$((FAIL + 1))
    else
        echo "    FAIL: exit code $exit_code"
        emit "{\"ts\":\"$(ts)\",\"test\":\"$test_src\",\"target\":\"$name\",\"status\":\"fail\",\"exit_code\":$exit_code,\"duration_ms\":$duration_ms}"
        FAIL=$((FAIL + 1))
    fi
}

echo "=========================================="
echo "QEMU Bare-Metal E2E Tests (bd-aw69.8)"
echo "=========================================="
echo ""

# ARM targets (using mps2-an385 with enough SRAM for BSS)
if [ -z "$TARGET_FILTER" ] || [ "$TARGET_FILTER" = "arm" ]; then
    echo "--- ARM Cortex-M3 (mps2-an385) ---"
    run_e2e "arm" "embedded_smoke" \
        "arm-none-eabi-gcc" \
        "-mcpu=cortex-m3 -mthumb" \
        "--specs=nosys.specs" \
        "$LINKER_DIR/qemu-arm.ld" \
        "arm-m4-free" \
        "qemu-system-arm" \
        "-machine mps2-an385 -cpu cortex-m3 -nographic -semihosting-config enable=on,target=native" \
        "FREESTANDING"
    echo ""
fi

# RISC-V 32-bit
if [ -z "$TARGET_FILTER" ] || [ "$TARGET_FILTER" = "riscv32" ]; then
    echo "--- RISC-V 32-bit (virt) ---"
    run_e2e "riscv32" "embedded_smoke" \
        "riscv64-unknown-elf-gcc" \
        "-march=rv32imac -mabi=ilp32" \
        "--specs=picolibc.specs" \
        "$LINKER_DIR/riscv32.ld" \
        "riscv32-free" \
        "qemu-system-riscv32" \
        "-machine virt -nographic -bios none" \
        "FREESTANDING"
    echo ""
fi

# RISC-V 64-bit
if [ -z "$TARGET_FILTER" ] || [ "$TARGET_FILTER" = "riscv64" ]; then
    echo "--- RISC-V 64-bit (virt) ---"
    run_e2e "riscv64" "embedded_smoke" \
        "riscv64-unknown-elf-gcc" \
        "-march=rv64imac -mabi=lp64 -mcmodel=medany" \
        "--specs=picolibc.specs" \
        "$LINKER_DIR/riscv64.ld" \
        "riscv64-free" \
        "qemu-system-riscv64" \
        "-machine virt -nographic -bios none" \
        "FREESTANDING"
    echo ""
fi

echo "=========================================="
echo "Summary: PASS=$PASS  FAIL=$FAIL  SKIP=$SKIP"
echo "JSONL log: $JSONL_LOG"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo "RESULT: FAIL"
    exit 1
else
    echo "RESULT: PASS"
    exit 0
fi
