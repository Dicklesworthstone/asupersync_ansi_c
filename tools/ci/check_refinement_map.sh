#!/usr/bin/env bash
# check_refinement_map.sh — docs/C_REFINEMENT_MAP.md maps every rule of the
# asupersync v4 Canonical Rule Index, with evidence (bd-9kll.2.12).
#
# Usage: tools/ci/check_refinement_map.sh [MAP.md] [INDEX.json]
#   defaults: docs/C_REFINEMENT_MAP.md schemas/v4_rule_index.json
#
# Reads the rows between <!-- refinement-map:rows:begin --> and
# <!-- refinement-map:rows:end -->. Columns: rule | Rust anchor | C
# implementation | C ghost check | C tests | fixtures | status | notes.
# Fails unless:
#   - the index is a well-formed asx.v4_rule_index.v1 (numbers 1..N, unique
#     names) and every rule in it has exactly one row, and every row names
#     an index rule;
#   - every status is Implemented, Variant, Partial or Missing;
#   - every Implemented row cites at least one existing Rust-captured fixture
#     or C test, and every other row has notes;
#   - every cited fixture exists in fixtures/rust_reference_v2, every cited
#     test `tests/...c::name` is a TEST(name) (or function) in that file, and
#     every C `path:line` (.c/.h, outside the Rust column) is within its file;
#   - the <!-- status-counts: ... --> comment matches the rows;
#   - every rules[] tag in tests/conformance/scenarios_v2 is an index rule,
#     and each rule's scenario_tag is its name iff some scenario uses it.
# With an asupersync checkout holding the index's source revision
# (ASUPERSYNC_REPO, default /dp/asupersync), the index must also equal the
# spec's Canonical Rule Index; without one that comparison is skipped.
#
# SPDX-License-Identifier: MIT

# The patterns below match literal Markdown backticks, not expansions.
# shellcheck disable=SC2016

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
map="${1:-$ROOT_DIR/docs/C_REFINEMENT_MAP.md}"
index="${2:-$ROOT_DIR/schemas/v4_rule_index.json}"
fixtures_dir="$ROOT_DIR/fixtures/rust_reference_v2"
scenarios_dir="$ROOT_DIR/tests/conformance/scenarios_v2"
status=0

fail() {
    echo "check_refinement_map: FAIL: $*" >&2
    status=1
}

trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

command -v jq >/dev/null 2>&1 || {
    echo "check_refinement_map: FAIL: jq is required" >&2
    exit 1
}
[ -f "$map" ] || {
    echo "check_refinement_map: FAIL: no map at $map" >&2
    exit 1
}
[ -f "$index" ] || {
    echo "check_refinement_map: FAIL: no rule index at $index" >&2
    exit 1
}

# --- The index -------------------------------------------------------------
if ! jq -e '.schema == "asx.v4_rule_index.v1" and (.rules | type == "array")
            and (.rules | length > 0)' "$index" >/dev/null 2>&1; then
    echo "check_refinement_map: FAIL: $index is not an asx.v4_rule_index.v1 document" >&2
    exit 1
fi
jq -e '[.rules[].number] == [range(1; (.rules | length) + 1)]' "$index" >/dev/null ||
    fail "$index: rule numbers are not 1..N in order"
jq -e '([.rules[].name] | unique | length) == (.rules | length)' "$index" >/dev/null ||
    fail "$index: duplicate rule names"

declare -A in_index=()
declare -A tag_of=()
mapfile -t names < <(jq -r '.rules[].name' "$index")
while IFS=$'\t' read -r name tag; do
    in_index["$name"]=1
    tag_of["$name"]="$tag"
done < <(jq -r '.rules[] | [.name, (.scenario_tag // "null")] | @tsv' "$index")

# --- The rows ----------------------------------------------------------------
declare -A seen=()
declare -A count=([Implemented]=0 [Variant]=0 [Partial]=0 [Missing]=0)
re_rule='^`([a-z_.]+)`'
re_test='^(tests/[A-Za-z0-9_./-]+\.c)::([A-Za-z0-9_]+)$'
re_anchor='^((src|include|tools|tests)/[A-Za-z0-9_./-]+\.[ch]):([0-9]+)$'
nrows=0
nfix=0
ntest=0
nanchor=0

rows="$(awk '/<!-- refinement-map:rows:begin -->/ { on = 1; next }
             /<!-- refinement-map:rows:end -->/ { on = 0 }
             on && /^\| `/' "$map")"
[ -n "$rows" ] || fail "$map: no rows between the refinement-map:rows markers"

while IFS= read -r row; do
    [ -n "$row" ] || continue
    nrows=$((nrows + 1))
    ncols="$(printf '%s' "$row" | awk -F'|' '{ print NF }')"
    if [ "$ncols" -ne 10 ]; then
        fail "row $nrows has $((ncols - 2)) columns, not 8: ${row:0:80}"
        continue
    fi
    IFS='|' read -r _ c_rule _c_rust c_impl c_ghost c_tests c_fix c_status c_notes _ <<<"$row"
    c_rule="$(trim "$c_rule")"
    c_status="$(trim "$c_status")"
    c_notes="$(trim "$c_notes")"
    if ! [[ $c_rule =~ $re_rule ]]; then
        fail "row $nrows: no rule name in '${c_rule:0:60}'"
        continue
    fi
    rule="${BASH_REMATCH[1]}"
    seen["$rule"]=$((${seen["$rule"]:-0} + 1))
    [ -n "${in_index[$rule]:-}" ] || fail "$rule: row for a rule that is not in $index"

    case "$c_status" in
    Implemented | Variant | Partial | Missing) count["$c_status"]=$((count["$c_status"] + 1)) ;;
    *) fail "$rule: status '$c_status' is not Implemented, Variant, Partial or Missing" ;;
    esac

    evidence=0
    while IFS= read -r fx; do
        [ -n "$fx" ] || continue
        nfix=$((nfix + 1))
        if [ -f "$fixtures_dir/$fx.json" ]; then
            evidence=$((evidence + 1))
        else
            fail "$rule: fixture '$fx' is not in fixtures/rust_reference_v2"
        fi
    done < <(printf '%s' "$c_fix" | grep -oE '`[a-z0-9][a-z0-9-]*`' | tr -d '`' || true)

    while IFS= read -r ref; do
        [ -n "$ref" ] || continue
        ntest=$((ntest + 1))
        if ! [[ $ref =~ $re_test ]]; then
            fail "$rule: malformed test reference '$ref'"
            continue
        fi
        tfile="${BASH_REMATCH[1]}"
        tname="${BASH_REMATCH[2]}"
        if [ ! -f "$ROOT_DIR/$tfile" ]; then
            fail "$rule: test file $tfile does not exist"
        elif grep -qE "^(TEST\\($tname\\)|(static )?void $tname\\()" "$ROOT_DIR/$tfile"; then
            evidence=$((evidence + 1))
        else
            fail "$rule: no test '$tname' in $tfile"
        fi
    done < <(printf '%s' "$c_tests" | grep -oE '`tests/[^`]+`' | tr -d '`' || true)

    while IFS= read -r ref; do
        [ -n "$ref" ] || continue
        nanchor=$((nanchor + 1))
        [[ $ref =~ $re_anchor ]] || continue
        afile="${BASH_REMATCH[1]}"
        aline="${BASH_REMATCH[3]}"
        if [ ! -f "$ROOT_DIR/$afile" ]; then
            fail "$rule: anchor $ref names a missing file"
        elif [ "$(wc -l <"$ROOT_DIR/$afile")" -lt "$aline" ]; then
            fail "$rule: anchor $ref is past the end of $afile"
        fi
    done < <(printf '%s\n%s\n%s' "$c_impl" "$c_ghost" "$c_notes" |
        grep -oE '`(src|include|tools|tests)/[A-Za-z0-9_./-]+\.[ch]:[0-9]+`' | tr -d '`' || true)

    if [ "$c_status" = "Implemented" ] && [ "$evidence" -eq 0 ]; then
        fail "$rule: Implemented without an existing fixture or test"
    fi
    if [ "$c_status" != "Implemented" ] && [ -z "$c_notes" ]; then
        fail "$rule: $c_status without notes saying why"
    fi
done <<<"$rows"

for name in "${names[@]}"; do
    case "${seen[$name]:-0}" in
    1) ;;
    0) fail "$name: no row in $map" ;;
    *) fail "$name: ${seen[$name]} rows in $map" ;;
    esac
done

# --- The summary comment -------------------------------------------------------
summary="$(grep -oE '<!-- status-counts: [^>]*-->' "$map" | head -1 || true)"
if [ -z "$summary" ]; then
    fail "$map: no <!-- status-counts: ... --> comment"
else
    for s in Implemented Variant Partial Missing; do
        shown="$(printf '%s' "$summary" | grep -oE "$s=[0-9]+" | cut -d= -f2 || true)"
        [ "$shown" = "${count[$s]}" ] ||
            fail "status-counts says $s=${shown:-?}, the rows have ${count[$s]}"
    done
fi

# --- Scenario tags -------------------------------------------------------------
declare -A tagged=()
shopt -s nullglob
scenarios=("$scenarios_dir"/*.json)
shopt -u nullglob
if [ "${#scenarios[@]}" -gt 0 ]; then
    while IFS= read -r tag; do
        [ -n "$tag" ] || continue
        tagged["$tag"]=1
        [ -n "${in_index[$tag]:-}" ] || fail "scenario tag '$tag' is not a rule in $index"
    done < <(jq -r '.rules[]?' "${scenarios[@]}" | sort -u)
fi
for name in "${names[@]}"; do
    if [ -n "${tagged[$name]:-}" ]; then
        [ "${tag_of[$name]}" = "$name" ] ||
            fail "$name: scenarios tag it, but its scenario_tag is ${tag_of[$name]}"
    elif [ "${tag_of[$name]}" != "null" ]; then
        fail "$name: scenario_tag is ${tag_of[$name]}, but no scenario tags it"
    fi
done

# --- The spec (when an asupersync checkout is present) ------------------------
repo="${ASUPERSYNC_REPO:-/dp/asupersync}"
source_ref="$(jq -r '.source' "$index")"
spec_path="${source_ref%@*}"
spec_rev="${source_ref##*@}"
if git -C "$repo" cat-file -e "$spec_rev:$spec_path" 2>/dev/null; then
    want="$(git -C "$repo" show "$spec_rev:$spec_path" |
        sed -n '/^### Canonical Rule Index/,/^---/p' |
        sed -nE 's/^- `([a-z_.]+)` \(#([0-9]+)\)$/\1 \2/p')"
    have="$(jq -r '.rules[] | "\(.name) \(.number)"' "$index")"
    if [ "$want" != "$have" ]; then
        fail "$index differs from the Canonical Rule Index of $spec_path@$spec_rev:"
        diff <(printf '%s\n' "$want") <(printf '%s\n' "$have") >&2 || true
    fi
    spec_note="index equals $spec_path@$spec_rev"
else
    spec_note="spec comparison skipped (no $spec_rev in $repo)"
fi

if [ "$status" -ne 0 ]; then
    echo "check_refinement_map: $map is out of date with $index (see above)" >&2
    exit 1
fi
echo "check_refinement_map: $map maps all ${#names[@]} rules (Implemented=${count[Implemented]}" \
    "Variant=${count[Variant]} Partial=${count[Partial]} Missing=${count[Missing]});" \
    "$nfix fixture, $ntest test and $nanchor code references resolve; $spec_note"
