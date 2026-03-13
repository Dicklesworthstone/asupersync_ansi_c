#!/usr/bin/env bash
# measure_baremetal_footprint.sh — Measure bare-metal static memory footprint
#
# Extracts .text/.rodata/.data/.bss section sizes from bare-metal archives
# and compares against R1 budget thresholds.
#
# Usage:
#   ./tools/ci/measure_baremetal_footprint.sh [OPTIONS]
#
# Options:
#   --target TARGET    Specific target (arm-m4, arm-m0, riscv32, riscv64)
#   --profile PROFILE  FREESTANDING or EMBEDDED_ROUTER
#   --run-id ID        Run identifier for tracking
#   --all              Measure all 8 targets (default)
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
CROSS_DIR="${BUILD_DIR}/cross"
LOG_DIR="${BUILD_DIR}/footprint-report"

mkdir -p "$LOG_DIR"

JSONL_LOG="$LOG_DIR/footprint.jsonl"
: > "$JSONL_LOG"

RUN_ID="${RUN_ID:-$(date +%s)}"
TARGET_FILTER=""
PROFILE_FILTER=""
RUN_ALL=1

while [ $# -gt 0 ]; do
    case "$1" in
        --target)   TARGET_FILTER="$2"; RUN_ALL=0; shift 2 ;;
        --profile)  PROFILE_FILTER="$2"; RUN_ALL=0; shift 2 ;;
        --run-id)   RUN_ID="$2"; shift 2 ;;
        --all)      RUN_ALL=1; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

PASS=0
FAIL=0
SKIP=0

ts() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }

emit() {
    echo "$1" >> "$JSONL_LOG"
    echo "$1"
}

# R1 Budget thresholds (bytes)
# These are based on measured sizes with headroom for growth.
# The library includes large static arrays (trace ring, event log, region/task slots)
# that dominate BSS. ROM (text+rodata) is the primary optimization target.
#
# Targets:
#   ROM (text + rodata): primary constraint on flash-limited MCUs
#   RAM (data + bss): dominated by configurable static arrays
budget_rom() {
    local target="$1" profile="$2"
    case "$profile" in
        FREESTANDING)     echo 131072 ;;  # 128KB — text ~65KB + rodata headroom
        EMBEDDED_ROUTER)  echo 131072 ;;  # 128KB — same source surface currently
    esac
}

budget_ram() {
    local target="$1" profile="$2"
    # BSS dominated by static arrays (g_trace_ring 24KB, g_event_log 16KB, etc.)
    # These are configurable; budget reflects default configuration.
    case "$profile" in
        FREESTANDING)     echo 524288 ;;  # 512KB — BSS ~308KB with default configs
        EMBEDDED_ROUTER)  echo 524288 ;;  # 512KB
    esac
}

# Select the right size tool for the target
size_tool() {
    local target="$1"
    case "$target" in
        arm-*)    echo "arm-none-eabi-size" ;;
        riscv*)   echo "riscv64-unknown-elf-size" ;;
    esac
}

# Measure one archive
measure_archive() {
    local target="$1"
    local suffix="$2"
    local profile="$3"
    local archive="$CROSS_DIR/${target}-${suffix}/lib/libasx.a"

    if [ ! -f "$archive" ]; then
        emit "{\"ts\":\"$(ts)\",\"run_id\":\"$RUN_ID\",\"target\":\"$target\",\"profile\":\"$profile\",\"status\":\"skip\",\"reason\":\"archive not found\"}"
        SKIP=$((SKIP + 1))
        return
    fi

    local st
    st=$(size_tool "$target")
    if ! command -v "$st" >/dev/null 2>&1; then
        emit "{\"ts\":\"$(ts)\",\"run_id\":\"$RUN_ID\",\"target\":\"$target\",\"profile\":\"$profile\",\"status\":\"skip\",\"reason\":\"size tool not found\"}"
        SKIP=$((SKIP + 1))
        return
    fi

    # Extract per-section totals using size -A -t
    # Sum .text*, .rodata* for ROM; .data*, .bss* for RAM
    local size_output
    size_output=$($st -A "$archive" 2>/dev/null)

    local text_total rodata_total data_total bss_total
    text_total=$(echo "$size_output" | /usr/bin/grep -E '^\.text' | awk '{sum+=$2} END {print sum+0}')
    rodata_total=$(echo "$size_output" | /usr/bin/grep -E '^\.rodata' | awk '{sum+=$2} END {print sum+0}')
    data_total=$(echo "$size_output" | /usr/bin/grep -E '^\.data' | awk '{sum+=$2} END {print sum+0}')
    bss_total=$(echo "$size_output" | /usr/bin/grep -E '^\.bss' | awk '{sum+=$2} END {print sum+0}')

    local rom_total=$((text_total + rodata_total))
    local ram_total=$((data_total + bss_total))

    local rom_budget ram_budget
    rom_budget=$(budget_rom "$target" "$profile")
    ram_budget=$(budget_ram "$target" "$profile")

    # Per-section reports
    local section_status
    for section in text rodata data bss; do
        local size_var="${section}_total"
        local size_val=${!size_var}
        emit "{\"ts\":\"$(ts)\",\"run_id\":\"$RUN_ID\",\"target\":\"$target\",\"profile\":\"$profile\",\"section\":\".${section}\",\"size_bytes\":$size_val}"
    done

    # Summary with budget check
    local rom_status="pass"
    local ram_status="pass"
    [ "$rom_total" -gt "$rom_budget" ] && rom_status="fail"
    [ "$ram_total" -gt "$ram_budget" ] && ram_status="fail"

    local overall="pass"
    if [ "$rom_status" = "fail" ] || [ "$ram_status" = "fail" ]; then
        overall="fail"
        FAIL=$((FAIL + 1))
    else
        PASS=$((PASS + 1))
    fi

    emit "{\"ts\":\"$(ts)\",\"run_id\":\"$RUN_ID\",\"target\":\"$target\",\"profile\":\"$profile\",\"rom_total\":$rom_total,\"rom_budget\":$rom_budget,\"ram_total\":$ram_total,\"ram_budget\":$ram_budget,\"status\":\"$overall\"}"

    # Human-readable summary
    local rom_pct=$((rom_total * 100 / rom_budget))
    local ram_pct=$((ram_total * 100 / ram_budget))
    echo "  ROM: ${rom_total}B / ${rom_budget}B (${rom_pct}%) [${rom_status}]"
    echo "  RAM: ${ram_total}B / ${ram_budget}B (${ram_pct}%) [${ram_status}]"
    echo "  .text=${text_total}  .rodata=${rodata_total}  .data=${data_total}  .bss=${bss_total}"
}

echo "=========================================="
echo "Bare-Metal Memory Footprint (bd-aw69.6)"
echo "Run ID: $RUN_ID"
echo "=========================================="
echo ""

TARGETS="arm-m4 arm-m0 riscv32 riscv64"
SUFFIXES="free router"

for target in $TARGETS; do
    if [ -n "$TARGET_FILTER" ] && [ "$TARGET_FILTER" != "$target" ]; then
        continue
    fi
    for suffix in $SUFFIXES; do
        if [ "$suffix" = "free" ]; then
            profile="FREESTANDING"
        else
            profile="EMBEDDED_ROUTER"
        fi
        if [ -n "$PROFILE_FILTER" ] && [ "$PROFILE_FILTER" != "$profile" ]; then
            continue
        fi
        echo "--- $target / $profile ---"
        measure_archive "$target" "$suffix" "$profile"
        echo ""
    done
done

echo "=========================================="
echo "Summary: PASS=$PASS  FAIL=$FAIL  SKIP=$SKIP"
echo "JSONL log: $JSONL_LOG"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo "RESULT: FAIL (budget exceeded)"
    exit 1
else
    echo "RESULT: PASS"
    exit 0
fi
