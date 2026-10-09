#!/usr/bin/env bash
# =============================================================================
# check_readme_samples.sh — every C sample in README.md compiles (bd-9kll.1.6)
#
# Each ```c fence is a complete translation unit, compiled against include/
# with -Werror. A fence with a main() is linked against the library and run;
# it must exit 0. Any other fence is compiled to an object. A fence directly
# preceded by a line starting "<!-- readme-c: excerpt" quotes code from a
# header and is skipped.
#
# usage: tools/ci/check_readme_samples.sh <README.md> <libasx.a> <work-dir>
# =============================================================================
set -euo pipefail

readme=${1:?usage: check_readme_samples.sh <README.md> <libasx.a> <work-dir>}
lib=${2:?missing libasx.a}
work=${3:?missing work dir}
cc=${CC:-cc}
flags=(-std=c99 -Wall -Wextra -Wpedantic -Werror -Iinclude)

mkdir -p "$work"
# One file per fence: sample-NN.c (or .excerpt) plus "NN <first README line>"
# on stdout.
index=$(awk -v out="$work" '
    /^<!-- readme-c: excerpt/ { excerpt = 1; next }
    /^```c$/ {
        n++
        ext = excerpt ? "excerpt" : "c"
        file = sprintf("%s/sample-%02d.%s", out, n, ext)
        printf "" > file
        printf "%02d %d %s\n", n, NR + 1, ext
        inblock = 1
        excerpt = 0
        next
    }
    /^```/ { if (inblock) { inblock = 0; close(file) } next }
    inblock { print > file; next }
    NF { excerpt = 0 }
' "$readme")

if [ -z "$index" ]; then
    echo "[asx] readme-samples: FAIL (no C fences found in $readme)" >&2
    exit 1
fi

compiled=0
skipped=0
failed=0
while read -r n line ext; do
    src="$work/sample-$n.c"
    if [ "$ext" = excerpt ]; then
        skipped=$((skipped + 1))
        continue
    fi
    if grep -q 'int main(' "$src"; then
        if "$cc" "${flags[@]}" "$src" "$lib" -o "$work/sample-$n" && "$work/sample-$n" > /dev/null; then
            compiled=$((compiled + 1))
        else
            echo "[asx] readme-samples: FAIL README.md:$line (sample $n does not build or run)" >&2
            failed=$((failed + 1))
        fi
    elif "$cc" "${flags[@]}" -c "$src" -o "$work/sample-$n.o"; then
        compiled=$((compiled + 1))
    else
        echo "[asx] readme-samples: FAIL README.md:$line (sample $n does not compile)" >&2
        failed=$((failed + 1))
    fi
done <<< "$index"

echo "[asx] readme-samples: $compiled compiled, $skipped excerpt(s) skipped, $failed failed"
[ "$failed" -eq 0 ]
