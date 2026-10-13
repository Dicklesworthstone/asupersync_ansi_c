#!/usr/bin/env bash
# capacity_table.sh — the README's capacity-macro table, generated from the
# headers (bd-9kll.10.4).
#
# Usage: tools/ci/capacity_table.sh [--check FILE] --out-dir DIR -- CC CFLAGS...
#
# The macro list is the one test-capacity-x4 raises 4x plus the few kept at
# their maximum (make print-capacity-macros). The script fails if a guarded
# size limit in include/ or src/ (#ifndef ASX_MAX_*, ASX_*_CAPACITY or any
# ASX_ name with a MAX, CAPACITY, LIMIT or CAP word, bd-t8if) is in neither
# that list nor tools/ci/capacity_fixed.txt (the limits that are not
# capacities, each with its reason), is in both, or the lists name a macro with
# no #ifndef guard. Every capacity needs a lower-bound #error.
#
# Defaults are what the compiler sees with the build's CFLAGS: macros from
# headers are evaluated by a probe that includes them; the few defined in a
# src/*.c file are read from their guarded #define.
#
# Without --check, prints the capacity table and then the fixed-limits table.
# With --check FILE, fails unless the block between the capacity-table markers
# in FILE is exactly the capacity table and the block between the
# fixed-limits markers is exactly the fixed-limits table.
#
# SPDX-License-Identifier: MIT

set -euo pipefail
export LC_ALL=C

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

check_file=""
out_dir=""
while [ $# -gt 0 ]; do
    case "$1" in
    --check)
        check_file="$2"
        shift 2
        ;;
    --out-dir)
        out_dir="$2"
        shift 2
        ;;
    --)
        shift
        break
        ;;
    *)
        echo "capacity_table: unknown argument: $1" >&2
        exit 2
        ;;
    esac
done
if [ -z "$out_dir" ] || [ $# -lt 1 ]; then
    echo "usage: $0 [--check FILE] --out-dir DIR -- CC CFLAGS..." >&2
    exit 2
fi
cc="$1"
shift

listed="$(make --no-print-directory -s print-capacity-macros | tr ' ' '\n' | sed '/^$/d' | sort -u)"

# The size limits that are not capacities: NAME, a space, the reason.
fixed_file="tools/ci/capacity_fixed.txt"
fixed_lines="$(sed -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$/d' "$fixed_file")"
fixed="$(printf '%s\n' "$fixed_lines" | awk '{print $1}' | sort -u)"

# Every guarded size limit, with the file that guards it. A name is a size
# limit when ASX_ is followed by words one of which is MAX, CAPACITY, LIMIT or
# CAP (so ASX_CAPABILITY_* is not one).
guards="$(grep -rnoE '^#ifndef ASX_([A-Z0-9]+_)*(MAX|CAPACITY|LIMIT|CAP)(_[A-Z0-9]+)*\b' include src |
    sed -E 's/^([^:]+):[0-9]+:#ifndef (.*)$/\2 \1/' | sort -u -k1,1)"
discovered="$(printf '%s\n' "$guards" | awk '{print $1}')"

both="$(comm -12 <(printf '%s\n' "$listed") <(printf '%s\n' "$fixed"))"
if [ -n "$both" ]; then
    echo "capacity_table: FAIL: in both CAPACITY_X4_CFLAGS and $fixed_file:" >&2
    printf '  %s\n' $both >&2
    exit 1
fi
missing="$(comm -23 <(printf '%s\n' "$discovered") <(printf '%s\n%s\n' "$listed" "$fixed" | sort -u))"
if [ -n "$missing" ]; then
    echo "capacity_table: FAIL: guarded size limits in neither CAPACITY_X4_CFLAGS nor $fixed_file:" >&2
    printf '  %s\n' $missing >&2
    exit 1
fi

status=0
rows_src=""
probe="$out_dir/capacity_probe.c"
mkdir -p "$out_dir"
{
    echo '#include <stdio.h>'
    for h in $(cd include && find asx -name '*.h' | sort); do echo "#include <$h>"; done
    echo '#include "sync/wait_queue.h"'
    echo '#include "conformance/canon.h"'
    echo '#include "conformance/json.h"'
    echo 'int main(void) {'
} >"$probe.tmp"

for m in $listed; do
    file="$(printf '%s\n' "$guards" | awk -v m="$m" '$1 == m {print $2}')"
    if [ -z "$file" ]; then
        file="$(grep -rlE "^#ifndef $m\\b" include src | head -1 || true)"
    fi
    if [ -z "$file" ]; then
        echo "capacity_table: FAIL: $m is listed but nothing guards it with #ifndef" >&2
        status=1
        continue
    fi
    # An override below the minimum must fail the build, not size an
    # array 0: every capacity has a lower-bound #if/#error check.
    if ! grep -rqE "\\($m\\) *<|#(if|elif) +$m *<" include src; then
        echo "capacity_table: FAIL: $m has no lower-bound #error check" >&2
        status=1
        continue
    fi
    case "$file" in
    *.h)
        # A macro only some profiles define reads "-" in the others.
        {
            echo "#ifdef $m"
            printf '    printf("| `%s` | %%lu | `%s` |\\n", (unsigned long)(%s));\n' \
                "$m" "$file" "$m"
            echo '#else'
            printf '    printf("| `%s` | - | `%s` |\\n");\n' "$m" "$file"
            echo '#endif'
        } >>"$probe.tmp"
        ;;
    *.c)
        value="$(grep -A1 -E "^#ifndef $m\\b" "$file" |
            sed -nE "s/^#define $m ([0-9]+)u?\$/\\1/p" | head -1)"
        if [ -z "$value" ]; then
            echo "capacity_table: FAIL: $m in $file has no literal default" >&2
            status=1
            continue
        fi
        rows_src="${rows_src}| \`$m\` | $value | \`$file\` |"$'\n'
        ;;
    esac
done
for m in $fixed; do
    file="$(printf '%s\n' "$guards" | awk -v m="$m" '$1 == m {print $2}')"
    if [ -z "$file" ]; then
        file="$(grep -rlE "^#ifndef $m\\b" include src | head -1 || true)"
    fi
    if [ -z "$file" ]; then
        echo "capacity_table: FAIL: $m is in $fixed_file but nothing guards it with #ifndef" >&2
        status=1
        continue
    fi
    case "$file" in
    *.h)
        {
            echo "#ifdef $m"
            printf '    printf("F %s %%lu\\n", (unsigned long)(%s));\n' "$m" "$m"
            echo '#else'
            printf '    printf("F %s -\\n");\n' "$m"
            echo '#endif'
        } >>"$probe.tmp"
        ;;
    *)
        echo "capacity_table: FAIL: $m in $file: a fixed limit must be defined in a header" >&2
        status=1
        ;;
    esac
done
[ "$status" -eq 0 ] || exit 1
echo '    return 0;' >>"$probe.tmp"
echo '}' >>"$probe.tmp"
mv "$probe.tmp" "$probe"

"$cc" "$@" -Iinclude -Isrc -o "$out_dir/capacity_probe" "$probe"

"$out_dir/capacity_probe" >"$out_dir/capacity_probe.out"

table="$out_dir/capacity_table.md"
{
    echo '| Macro | Default | Defined in |'
    echo '|---|---|---|'
    { grep '^| ' "$out_dir/capacity_probe.out"; printf '%s' "$rows_src"; } | sort
} >"$table"

# The fixed limits: the probe's "F NAME VALUE" lines and the reason of the file.
fixed_table="$out_dir/capacity_fixed_table.md"
{
    echo '| Macro | Default | Why it is not a capacity |'
    echo '|---|---|---|'
    grep '^F ' "$out_dir/capacity_probe.out" | while read -r _ name value; do
        reason="$(printf '%s\n' "$fixed_lines" | awk -v m="$name" '$1 == m {sub(/^[^ ]+ /, ""); print}')"
        printf '| `%s` | %s | %s |\n' "$name" "$value" "$reason"
    done | sort
} >"$fixed_table"

if [ -z "$check_file" ]; then
    cat "$table"
    echo
    cat "$fixed_table"
    exit 0
fi

block="$out_dir/capacity_table.readme.md"
sed -n '/<!-- capacity-table:begin -->/,/<!-- capacity-table:end -->/p' "$check_file" |
    sed '1d;$d' >"$block"
if ! diff -u "$block" "$table" >&2; then
    echo "capacity_table: FAIL: the table in $check_file is not the generated one" >&2
    echo "  regenerate: make capacity-table, and paste it between the markers" >&2
    exit 1
fi

fixed_block="$out_dir/capacity_fixed_table.readme.md"
sed -n '/<!-- fixed-limits:begin -->/,/<!-- fixed-limits:end -->/p' "$check_file" |
    sed '1d;$d' >"$fixed_block"
if ! diff -u "$fixed_block" "$fixed_table" >&2; then
    echo "capacity_table: FAIL: the fixed-limits table in $check_file is not the generated one" >&2
    echo "  regenerate: make capacity-table, and paste its second table between the fixed-limits markers" >&2
    exit 1
fi
echo "capacity_table: $check_file matches ($(($(wc -l <"$table") - 2)) capacities, $(($(wc -l <"$fixed_table") - 2)) fixed limits)"
