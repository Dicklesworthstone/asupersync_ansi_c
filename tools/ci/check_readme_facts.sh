#!/usr/bin/env bash
# check_readme_facts.sh — the README's inventory numbers match the tree
# (bd-9kll.1.5).
#
# Usage: tools/ci/check_readme_facts.sh README.md
#
# Every <!-- fact:KEY -->N<!-- /fact --> in the file must equal KEY's value
# from tools/count_inventory.sh --facts (thousands separators allowed), and
# the API and test badges must carry the same numbers. An unknown KEY fails.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readme="${1:?usage: $0 README.md}"
facts="$(sh "$ROOT_DIR/tools/count_inventory.sh" --facts)"
status=0
checked=0

fact() { printf '%s\n' "$facts" | sed -n "s/^$1=//p"; }

while IFS= read -r marker; do
    key="$(printf '%s' "$marker" | sed -E 's/^<!-- fact:([a-z0-9_]+) -->.*/\1/')"
    shown="$(printf '%s' "$marker" | sed -E 's/^.*-->([0-9,]+)<!--.*/\1/' | tr -d ,)"
    want="$(fact "$key")"
    checked=$((checked + 1))
    if [ -z "$want" ]; then
        echo "check_readme_facts: FAIL: unknown fact '$key' in $readme" >&2
        status=1
    elif [ "$shown" != "$want" ]; then
        echo "check_readme_facts: FAIL: $key is $want, $readme says $shown" >&2
        status=1
    fi
done < <(grep -oE '<!-- fact:[a-z0-9_]+ -->[0-9,]+<!-- /fact -->' "$readme")

# Badge URLs cannot hold markers: check their numbers directly.
check_badge() {
    local prefix="$1" suffix="$2" key="$3" shown
    shown="$(grep -oE "badge/${prefix}[0-9%C]+${suffix}" "$readme" | head -1 |
        sed -E "s/^badge\\/${prefix}//; s/${suffix}\$//; s/%2C//g")"
    checked=$((checked + 1))
    if [ "$shown" != "$(fact "$key")" ]; then
        echo "check_readme_facts: FAIL: badge ${key} says '${shown}', is $(fact "$key")" >&2
        status=1
    fi
}
check_badge 'public%20API-' '%20declarations' api_declarations
check_badge 'tests-' '%20programs' test_programs

if [ "$status" -ne 0 ]; then
    echo "  the counts: sh tools/count_inventory.sh --facts" >&2
    exit 1
fi
echo "check_readme_facts: $readme matches ($checked numbers)"
