#!/usr/bin/env bash
# =============================================================================
# check_kernel_only.sh — the kernel-only archive must not need what it leaves
#                        out (bd-9kll.13.3, owner decision DEC-011/D6)
#
# Usage: tools/ci/check_kernel_only.sh <full libasx.a> <kernel libasx_kernel.a>
#
# A kernel object that refers to a symbol only a left-out module defines
# (the conformance interpreter, the CLI, a protocol model) is a kernel that
# cannot link without that module: the build would claim a footprint it does
# not have. Both archives are built from the same profile, class and build
# type; the full one is every library object, the kernel one the subset the
# Makefile's KERNEL_SRC names.
#
# Exit 0: every undefined symbol of the kernel archive is defined inside it
#         or by neither archive (the C library, the platform).
# Exit 1: a left-out module defines one; the offenders are listed with the
#         kernel member that refers to them.
# Exit 2: usage or a missing tool or archive.
#
# SPDX-License-Identifier: MIT
# =============================================================================

set -euo pipefail

# sort and comm must agree on one order. Under a UTF-8 locale on uutils
# coreutils (the host's default since Ubuntu 25.10) they do not: comm reports
# its input unsorted and the check failed closed on a kernel that was fine.
export LC_ALL=C

if [ $# -ne 2 ]; then
    echo "usage: $0 <full libasx.a> <kernel libasx_kernel.a>" >&2
    exit 2
fi
FULL="$1"
KERNEL="$2"

command -v nm >/dev/null 2>&1 || { echo "check_kernel_only: nm not found" >&2; exit 2; }
for a in "$FULL" "$KERNEL"; do
    [ -f "$a" ] || { echo "check_kernel_only: no such archive: $a" >&2; exit 2; }
done

# Global symbols an archive defines (text, data, bss, rodata, weak, common).
defined() {
    nm -g --defined-only "$1" 2>/dev/null |
        awk 'NF == 3 && $2 ~ /^[TDBRVWCGSI]$/ { print $3 }' | sort -u
}

# Global symbols the archive's members leave undefined.
undefined() {
    nm -g -u "$1" 2>/dev/null | awk 'NF == 2 && $1 == "U" { print $2 }' | sort -u
}

only_in_full="$(comm -13 <(defined "$KERNEL") <(defined "$FULL"))"
needed="$(comm -12 <(undefined "$KERNEL") <(printf '%s\n' "$only_in_full" | sort -u))"

if [ -z "$needed" ]; then
    echo "[asx] kernel-only-check: PASS ($(defined "$KERNEL" | wc -l | tr -d ' ') symbols defined," \
        "$(undefined "$KERNEL" | wc -l | tr -d ' ') left to the C library and the platform)"
    exit 0
fi

echo "[asx] kernel-only-check: FAIL: kernel objects refer to symbols only a left-out module defines"
nm -A -g -u "$KERNEL" 2>/dev/null | while read -r member _ sym; do
    case "$member" in *:) member="${member%:}" ;; esac
    if printf '%s\n' "$needed" | grep -qx -- "$sym"; then
        echo "  $sym  <-  ${member##*/}"
    fi
done | sort -u
exit 1
