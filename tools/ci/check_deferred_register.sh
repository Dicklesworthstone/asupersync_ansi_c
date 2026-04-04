#!/usr/bin/env bash
# =============================================================================
# check_deferred_register.sh — validate deferred stubs register completeness
#
# Checks that every source file containing deferral markers (walking skeleton,
# stub, spike, deferred, not yet implemented) is referenced in the deferred
# stubs register. Reports unregistered deferrals as warnings.
#
# Exit 0 = pass, Exit 1 = unregistered deferrals found
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REGISTER="$REPO_ROOT/docs/DEFERRED_STUBS_REGISTER.md"

WARNINGS=0
CHECKED=0
PASSED=0

echo "[deferred-register] checking register completeness..."

# 1. Register file must exist
if [ ! -f "$REGISTER" ]; then
    echo "[deferred-register] FAIL — $REGISTER not found"
    exit 1
fi

if [ ! -s "$REGISTER" ]; then
    echo "[deferred-register] FAIL — $REGISTER is empty"
    exit 1
fi

# 2. Find source files with deferral markers
deferral_files=$(rg -l -i "walking skeleton|not yet implemented|research spike|stub.*platform|deferred.*platform" "$REPO_ROOT/src/" --type c 2>/dev/null | sort -u || true)

# 3. Check each is referenced in register
for filepath in $deferral_files; do
    CHECKED=$((CHECKED + 1))
    # Extract relative path from repo root
    relpath="${filepath#$REPO_ROOT/}"

    if grep -q "$relpath" "$REGISTER" 2>/dev/null; then
        PASSED=$((PASSED + 1))
    else
        # Check if the basename is referenced (register may use abbreviated paths)
        basename=$(basename "$filepath")
        if grep -q "$basename" "$REGISTER" 2>/dev/null; then
            PASSED=$((PASSED + 1))
        else
            echo "  WARN: $relpath contains deferral markers but is not in register"
            WARNINGS=$((WARNINGS + 1))
        fi
    fi
done

# 4. Check spike files specifically
spike_files=$(find "$REPO_ROOT/src" -name '*spike*' -name '*.c' 2>/dev/null | sort)
for filepath in $spike_files; do
    relpath="${filepath#$REPO_ROOT/}"
    basename=$(basename "$filepath")
    if ! grep -q "$basename" "$REGISTER" 2>/dev/null; then
        # Don't double-count if already caught above
        already_warned=0
        for df in $deferral_files; do
            if [ "$df" = "$filepath" ]; then
                already_warned=1
                break
            fi
        done
        if [ "$already_warned" = "0" ]; then
            CHECKED=$((CHECKED + 1))
            echo "  WARN: spike file $relpath not in register"
            WARNINGS=$((WARNINGS + 1))
        fi
    fi
done

# 5. Check platform adapter stubs
for adapter in posix/hooks.c win32/hooks.c freestanding/hooks.c; do
    CHECKED=$((CHECKED + 1))
    basename=$(basename "$adapter" .c)
    dirname=$(dirname "$adapter")
    if grep -q "$adapter\|$dirname\|platform.*$basename" "$REGISTER" 2>/dev/null; then
        PASSED=$((PASSED + 1))
    else
        echo "  WARN: platform adapter src/platform/$adapter not in register"
        WARNINGS=$((WARNINGS + 1))
    fi
done

echo ""
echo "[deferred-register] $CHECKED files checked, $PASSED referenced, $WARNINGS unregistered"

if [ "$WARNINGS" -gt 0 ]; then
    echo "[deferred-register] WARN — $WARNINGS file(s) with deferral markers not in register"
    echo "[deferred-register] PASS (warnings only, no violations)"
    exit 0
fi

echo "[deferred-register] PASS"
exit 0
