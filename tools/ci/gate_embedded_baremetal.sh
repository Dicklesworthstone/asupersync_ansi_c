#!/usr/bin/env bash
# gate_embedded_baremetal.sh — Bare-metal embedded CI gate (GATE-EMBEDDED-BAREMETAL)
#
# Runs the complete bare-metal validation pipeline:
#   1. Cross-compile libasx.a for all 8 bare-metal targets
#   2. Validate linkage (no unresolved POSIX/libc symbols)
#   3. Measure RAM/ROM footprint against R1 budgets
#   4. Run QEMU e2e tests on ARM and RISC-V targets
#
# Gracefully SKIPs if cross-compilation toolchains are not installed.
# JSONL output to stdout for CI consumption.
#
# Usage:
#   tools/ci/gate_embedded_baremetal.sh [OPTIONS]
#
# Options:
#   --strict    Fail on any SKIP (require all toolchains)
#   --dry-run   Show steps without executing
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

STRICT=0
DRY_RUN=0
GATE_PASS=1
GATE_SKIP=0

while [ $# -gt 0 ]; do
    case "$1" in
        --strict)  STRICT=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

ts() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
now_ns() { date +%s%N; }

emit() { echo "$1"; }

emit_step() {
    local step="$1" status="$2" detail="$3" start_ns="$4"
    local end_ns duration_ms
    end_ns=$(now_ns)
    duration_ms=$(( (end_ns - start_ns) / 1000000 ))
    emit "{\"ts\":\"$(ts)\",\"gate\":\"embedded-baremetal\",\"step\":\"$step\",\"status\":\"$status\",\"detail\":\"$detail\",\"duration_ms\":$duration_ms}"
}

# Check if any bare-metal toolchain is available
has_arm_toolchain() { command -v arm-none-eabi-gcc >/dev/null 2>&1; }
has_riscv_toolchain() { command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; }

GATE_START_NS=$(now_ns)

# ── Step 1: Toolchain check ────────────────────────────────────────────

step_start=$(now_ns)
HAS_ARM=0
HAS_RISCV=0

if has_arm_toolchain; then HAS_ARM=1; fi
if has_riscv_toolchain; then HAS_RISCV=1; fi

if [ "$HAS_ARM" -eq 0 ] && [ "$HAS_RISCV" -eq 0 ]; then
    if [ "$STRICT" -eq 1 ]; then
        emit_step "toolchain_check" "fail" "no bare-metal toolchains found (strict mode)" "$step_start"
        GATE_PASS=0
    else
        emit_step "toolchain_check" "skip" "no bare-metal toolchains found" "$step_start"
        GATE_SKIP=1
    fi
else
    tc_detail=""
    [ "$HAS_ARM" -eq 1 ] && tc_detail="arm"
    [ "$HAS_RISCV" -eq 1 ] && tc_detail="${tc_detail:+$tc_detail+}riscv"
    emit_step "toolchain_check" "pass" "toolchains: $tc_detail" "$step_start"
fi

if [ "$GATE_SKIP" -eq 1 ]; then
    end_ns=$(now_ns)
    total_ms=$(( (end_ns - GATE_START_NS) / 1000000 ))
    emit "{\"ts\":\"$(ts)\",\"gate\":\"embedded-baremetal\",\"step\":\"gate_result\",\"status\":\"skip\",\"detail\":\"no toolchains available\",\"duration_ms\":$total_ms}"
    exit 0
fi

if [ "$GATE_PASS" -eq 0 ]; then
    end_ns=$(now_ns)
    total_ms=$(( (end_ns - GATE_START_NS) / 1000000 ))
    emit "{\"ts\":\"$(ts)\",\"gate\":\"embedded-baremetal\",\"step\":\"gate_result\",\"status\":\"fail\",\"detail\":\"toolchain check failed\",\"duration_ms\":$total_ms}"
    exit 1
fi

if [ "$DRY_RUN" -eq 1 ]; then
    emit_step "cross_build" "skip" "dry-run" "$GATE_START_NS"
    emit_step "linkage_validation" "skip" "dry-run" "$GATE_START_NS"
    emit_step "footprint_check" "skip" "dry-run" "$GATE_START_NS"
    emit_step "qemu_e2e" "skip" "dry-run" "$GATE_START_NS"
    end_ns=$(now_ns)
    total_ms=$(( (end_ns - GATE_START_NS) / 1000000 ))
    emit "{\"ts\":\"$(ts)\",\"gate\":\"embedded-baremetal\",\"step\":\"gate_result\",\"status\":\"pass\",\"detail\":\"dry-run complete\",\"duration_ms\":$total_ms}"
    exit 0
fi

# ── Step 2: Cross-compile all bare-metal targets ───────────────────────

step_start=$(now_ns)
if make -C "$PROJECT_ROOT" cross-baremetal-all >/dev/null 2>&1; then
    emit_step "cross_build" "pass" "all bare-metal targets built" "$step_start"
else
    emit_step "cross_build" "fail" "cross-baremetal-all failed" "$step_start"
    GATE_PASS=0
fi

# ── Step 3: Linkage validation ─────────────────────────────────────────

if [ "$GATE_PASS" -eq 1 ]; then
    step_start=$(now_ns)
    LINKAGE_SCRIPT="$SCRIPT_DIR/validate_baremetal_linkage.sh"
    if [ -x "$LINKAGE_SCRIPT" ]; then
        if "$LINKAGE_SCRIPT" >/dev/null 2>&1; then
            emit_step "linkage_validation" "pass" "no unresolved POSIX symbols" "$step_start"
        else
            emit_step "linkage_validation" "fail" "linkage validation failed" "$step_start"
            GATE_PASS=0
        fi
    else
        emit_step "linkage_validation" "skip" "validate_baremetal_linkage.sh not found" "$step_start"
    fi
fi

# ── Step 4: Footprint check ───────────────────────────────────────────

if [ "$GATE_PASS" -eq 1 ]; then
    step_start=$(now_ns)
    FOOTPRINT_SCRIPT="$SCRIPT_DIR/measure_baremetal_footprint.sh"
    if [ -x "$FOOTPRINT_SCRIPT" ]; then
        if "$FOOTPRINT_SCRIPT" >/dev/null 2>&1; then
            emit_step "footprint_check" "pass" "within R1 budgets" "$step_start"
        else
            emit_step "footprint_check" "fail" "footprint exceeds R1 budgets" "$step_start"
            GATE_PASS=0
        fi
    else
        emit_step "footprint_check" "skip" "measure_baremetal_footprint.sh not found" "$step_start"
    fi
fi

# ── Step 5: QEMU e2e tests ────────────────────────────────────────────

if [ "$GATE_PASS" -eq 1 ]; then
    step_start=$(now_ns)
    E2E_SCRIPT="$SCRIPT_DIR/run_qemu_e2e.sh"
    if [ -x "$E2E_SCRIPT" ]; then
        if "$E2E_SCRIPT" >/dev/null 2>&1; then
            emit_step "qemu_e2e" "pass" "all QEMU targets pass" "$step_start"
        else
            emit_step "qemu_e2e" "fail" "QEMU e2e tests failed" "$step_start"
            GATE_PASS=0
        fi
    else
        emit_step "qemu_e2e" "skip" "run_qemu_e2e.sh not found" "$step_start"
    fi
fi

# ── Gate result ───────────────────────────────────────────────────────

end_ns=$(now_ns)
total_ms=$(( (end_ns - GATE_START_NS) / 1000000 ))

if [ "$GATE_PASS" -eq 1 ]; then
    emit "{\"ts\":\"$(ts)\",\"gate\":\"embedded-baremetal\",\"step\":\"gate_result\",\"status\":\"pass\",\"detail\":\"all checks passed\",\"duration_ms\":$total_ms}"
    exit 0
else
    emit "{\"ts\":\"$(ts)\",\"gate\":\"embedded-baremetal\",\"step\":\"gate_result\",\"status\":\"fail\",\"detail\":\"one or more checks failed\",\"duration_ms\":$total_ms}"
    exit 1
fi
