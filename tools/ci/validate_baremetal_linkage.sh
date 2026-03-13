#!/usr/bin/env bash
# validate_baremetal_linkage.sh — Verify bare-metal archives have no unexpected
# unresolved symbols (bd-aw69.5)
#
# For each of the 8 bare-metal .a files (4 targets × 2 profiles), this script:
#   1. Extracts truly unresolved symbols (undefined - defined within archive)
#   2. Categorizes each: compiler_intrinsic / libc_minimal / libc_heap / posix / unknown
#   3. Reports per-target results with JSONL logging
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
CROSS_DIR="${BUILD_DIR}/cross"
LOG_DIR="${BUILD_DIR}/linkage-report"

mkdir -p "$LOG_DIR"

JSONL_LOG="$LOG_DIR/linkage.jsonl"
: > "$JSONL_LOG"

PASS=0
WARN=0
FAIL=0

# Timestamp helper
ts() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }

# JSONL emitter
emit() {
    echo "$1" >> "$JSONL_LOG"
    echo "$1"
}

# Categorize a symbol name
categorize_symbol() {
    local sym="$1"
    case "$sym" in
        # ARM compiler intrinsics
        __aeabi_*|__gnu_*|__udiv*|__div*|__mod*|__mul*|__clz*|__ctz*|__bswap*)
            echo "compiler_intrinsic" ;;
        # RISC-V / generic GCC intrinsics
        __divdi3|__udivdi3|__moddi3|__umoddi3|__divsi3|__udivsi3|__modsi3|__umodsi3)
            echo "compiler_intrinsic" ;;
        __muldi3|__multi3|__lshrdi3|__ashldi3|__ashrdi3)
            echo "compiler_intrinsic" ;;
        # Minimal libc (provided by newlib/picolibc, compiler-generated)
        memcpy|memset|memcmp|memmove|strlen|strcmp|strncmp|strncpy|strcpy|strstr|strnlen)
            echo "libc_minimal" ;;
        snprintf|vsnprintf)
            echo "libc_minimal" ;;
        # Heap functions (concerning for FREESTANDING)
        malloc|free|realloc|calloc)
            echo "libc_heap" ;;
        # POSIX functions (should never appear)
        pthread_*|fork|exec*|open|close|read|write|ioctl|mmap|munmap|signal|sigaction)
            echo "posix" ;;
        clock_gettime|gettimeofday|time|nanosleep|usleep)
            echo "posix" ;;
        # Anything else
        *)
            echo "unknown" ;;
    esac
}

# Validate one archive
validate_archive() {
    local target="$1"
    local suffix="$2"
    local profile="$3"
    local archive="$CROSS_DIR/${target}-${suffix}/lib/libasx.a"

    if [ ! -f "$archive" ]; then
        emit "{\"ts\":\"$(ts)\",\"target\":\"$target\",\"profile\":\"$profile\",\"step\":\"check\",\"status\":\"skip\",\"reason\":\"archive not found\"}"
        return
    fi

    # Get all defined symbols in the archive
    local defined_file undefined_file
    defined_file=$(mktemp)
    undefined_file=$(mktemp)

    nm --defined-only "$archive" 2>/dev/null | awk '{print $NF}' | LC_ALL=C sort -u > "$defined_file"
    nm --undefined-only "$archive" 2>/dev/null | awk '{print $NF}' | LC_ALL=C sort -u > "$undefined_file"

    # Truly unresolved = undefined - defined
    local external_unresolved
    external_unresolved=$(LC_ALL=C comm -23 "$undefined_file" "$defined_file" \
        | grep -v '^asx_' | grep -v '^g_' | grep -v '^$' || true)

    rm -f "$defined_file" "$undefined_file"

    # Categorize
    local compiler_intrinsic=0
    local libc_minimal=0
    local libc_heap=0
    local posix=0
    local unknown_count=0
    local heap_symbols=""
    local posix_symbols=""
    local unknown_symbols=""

    while IFS= read -r sym; do
        [ -z "$sym" ] && continue
        local cat
        cat=$(categorize_symbol "$sym")
        case "$cat" in
            compiler_intrinsic) compiler_intrinsic=$((compiler_intrinsic + 1)) ;;
            libc_minimal)       libc_minimal=$((libc_minimal + 1)) ;;
            libc_heap)
                libc_heap=$((libc_heap + 1))
                heap_symbols="${heap_symbols:+$heap_symbols, }\"$sym\""
                ;;
            posix)
                posix=$((posix + 1))
                posix_symbols="${posix_symbols:+$posix_symbols, }\"$sym\""
                ;;
            unknown)
                unknown_count=$((unknown_count + 1))
                unknown_symbols="${unknown_symbols:+$unknown_symbols, }\"$sym\""
                ;;
        esac
    done <<< "$external_unresolved"

    local symbol_list
    symbol_list=$(echo "$external_unresolved" | grep -v '^$' | sed 's/^/"/;s/$/"/' | paste -sd',' - || echo "")

    emit "{\"ts\":\"$(ts)\",\"target\":\"$target\",\"profile\":\"$profile\",\"step\":\"extract_undefined\",\"symbols\":[$symbol_list],\"count\":$((compiler_intrinsic + libc_minimal + libc_heap + posix + unknown_count))}"

    emit "{\"ts\":\"$(ts)\",\"target\":\"$target\",\"profile\":\"$profile\",\"step\":\"categorize\",\"compiler_intrinsic\":$compiler_intrinsic,\"libc_minimal\":$libc_minimal,\"libc_heap\":$libc_heap,\"posix\":$posix,\"unknown\":$unknown_count}"

    # Determine status
    local status="pass"
    if [ "$posix" -gt 0 ]; then
        status="fail"
        FAIL=$((FAIL + 1))
        echo "  FAIL: POSIX symbols found: [$posix_symbols]"
    elif [ "$libc_heap" -gt 0 ]; then
        status="warn"
        WARN=$((WARN + 1))
        echo "  WARN: heap symbols found: [$heap_symbols] (need user-provided allocator or newlib stub)"
    elif [ "$unknown_count" -gt 0 ]; then
        status="warn"
        WARN=$((WARN + 1))
        echo "  WARN: unknown symbols: [$unknown_symbols]"
    else
        PASS=$((PASS + 1))
    fi

    local archive_size
    archive_size=$(wc -c < "$archive")
    emit "{\"ts\":\"$(ts)\",\"target\":\"$target\",\"profile\":\"$profile\",\"step\":\"link_test\",\"status\":\"$status\",\"archive_size\":$archive_size}"
}

# Profile diff for a given target
profile_diff() {
    local target="$1"
    local free_archive="$CROSS_DIR/${target}-free/lib/libasx.a"
    local router_archive="$CROSS_DIR/${target}-router/lib/libasx.a"

    if [ ! -f "$free_archive" ] || [ ! -f "$router_archive" ]; then
        return
    fi

    local free_tmp router_tmp
    free_tmp=$(mktemp)
    router_tmp=$(mktemp)

    nm --defined-only "$free_archive" 2>/dev/null | awk '{print $NF}' | LC_ALL=C sort -u > "$free_tmp"
    nm --defined-only "$router_archive" 2>/dev/null | awk '{print $NF}' | LC_ALL=C sort -u > "$router_tmp"

    local free_only router_only
    free_only=$(LC_ALL=C comm -23 "$free_tmp" "$router_tmp" | grep -v '^$' || true)
    router_only=$(LC_ALL=C comm -13 "$free_tmp" "$router_tmp" | grep -v '^$' || true)

    local free_list router_list
    free_list=$(echo "$free_only" | grep -v '^$' | sed 's/^/"/;s/$/"/' | paste -sd',' - 2>/dev/null || echo "")
    router_list=$(echo "$router_only" | grep -v '^$' | sed 's/^/"/;s/$/"/' | paste -sd',' - 2>/dev/null || echo "")
    local count_diff
    count_diff=$(( $(wc -l < "$router_tmp") - $(wc -l < "$free_tmp") ))

    rm -f "$free_tmp" "$router_tmp"

    emit "{\"ts\":\"$(ts)\",\"target\":\"$target\",\"step\":\"profile_diff\",\"freestanding_only\":[$free_list],\"router_only\":[$router_list],\"count_diff\":$count_diff}"
}

echo "=========================================="
echo "Bare-Metal Linkage Validation (bd-aw69.5)"
echo "=========================================="
echo ""

TARGETS="arm-m4 arm-m0 riscv32 riscv64"
# suffix used in build directory name → profile define passed to compiler
SUFFIXES="free router"

for target in $TARGETS; do
    for suffix in $SUFFIXES; do
        if [ "$suffix" = "free" ]; then
            profile="FREESTANDING"
        else
            profile="EMBEDDED_ROUTER"
        fi
        echo "--- $target / $profile ---"
        validate_archive "$target" "$suffix" "$profile"
        echo ""
    done
    profile_diff "$target"
done

echo "=========================================="
echo "Summary: PASS=$PASS  WARN=$WARN  FAIL=$FAIL"
echo "JSONL log: $JSONL_LOG"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo "RESULT: FAIL"
    exit 1
elif [ "$WARN" -gt 0 ]; then
    echo "RESULT: WARN (heap/unknown symbols present — acceptable with newlib/picolibc)"
    exit 0
else
    echo "RESULT: PASS"
    exit 0
fi
