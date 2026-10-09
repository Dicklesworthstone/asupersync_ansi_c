#!/usr/bin/env bash
# promote_fixtures.sh — promote a staged capture into the committed corpus
# (bead W1.1, bd-9kll.2.1). Invoked by `make fixtures-promote RUN_ID=<id>`.
#
# Rules (single baseline per milestone):
#   - every staged fixture must name the same rust_baseline_commit;
#   - staged baseline == the inventory's active baseline: a refresh, allowed;
#   - staged baseline == the pending rebase baseline: a rebase, allowed only
#     when the rebase record has owner sign-off ("- approved_by: <name>");
#     the inventory then moves the active entry to history and makes the
#     pending one active;
#   - any other baseline: refused.
# Files are copied over; nothing is deleted. Committed fixtures that the
# staging lacks are reported as STALE for the owner to handle.
#
# Usage: tools/ci/promote_fixtures.sh --run-id <id>
# Environment overrides (used by tests/gates/test_fixture_promotion.sh):
#   FIXTURE_DIR, STAGING_ROOT, INVENTORY
#
# Exit 0 = promoted, 1 = refused, 2 = usage error.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FIXTURE_DIR="${FIXTURE_DIR:-$PROJECT_ROOT/fixtures/rust_reference}"
STAGING_ROOT="${STAGING_ROOT:-$PROJECT_ROOT/build/fixture_staging}"
INVENTORY="${INVENTORY:-$PROJECT_ROOT/docs/rust_baseline_inventory.json}"
RUN_ID=""

while [ $# -gt 0 ]; do
    case "$1" in
        --run-id) RUN_ID="$2"; shift 2 ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$RUN_ID" ] || { echo "usage: $0 --run-id <id>" >&2; exit 2; }

STAGING_DIR="$STAGING_ROOT/$RUN_ID"
refuse() {
    echo "[asx] fixtures-promote: REFUSED — $*" >&2
    exit 1
}

[ -d "$STAGING_DIR" ] || refuse "no staging dir $STAGING_DIR (run tools/ci/capture_rust_fixtures.sh first)"
[ -f "$INVENTORY" ] || refuse "missing inventory $INVENTORY"

mapfile -t staged_files < <(find "$STAGING_DIR" -name '*.json' ! -name 'provenance.json' -type f | sort)
[ "${#staged_files[@]}" -gt 0 ] || refuse "staging dir $STAGING_DIR has no fixtures"

mapfile -t baselines < <(jq -r '.provenance.rust_baseline_commit // "missing"' "${staged_files[@]}" | sort -u)
if [ "${#baselines[@]}" -ne 1 ]; then
    refuse "staged fixtures name ${#baselines[@]} baselines (${baselines[*]}); one capture run is one baseline"
fi
staged_baseline="${baselines[0]}"

active_baseline="$(jq -r '.source_repo.commit' "$INVENTORY")"
pending_baseline="$(jq -r '.pending_rebase.source_repo.commit // ""' "$INVENTORY")"
mode=""
if [ "$staged_baseline" = "$active_baseline" ]; then
    mode="refresh"
elif [ -n "$pending_baseline" ] && [ "$staged_baseline" = "$pending_baseline" ]; then
    mode="rebase"
    record_rel="$(jq -r '.pending_rebase.rebase_record // ""' "$INVENTORY")"
    record="$record_rel"
    [ "${record#/}" = "$record" ] && record="$PROJECT_ROOT/$record_rel"
    [ -f "$record" ] || refuse "rebase record not found: $record_rel"
    if ! grep -Eq '^- approved_by:[[:space:]]*[^[:space:]]' "$record"; then
        refuse "rebase to $staged_baseline needs owner sign-off: fill '- approved_by:' in $record_rel"
    fi
else
    refuse "staged fixtures name baseline $staged_baseline, which is neither the active ($active_baseline) nor the pending (${pending_baseline:-none}) baseline"
fi

copied=0
for staged in "${staged_files[@]}"; do
    rel="${staged#"$STAGING_DIR"/}"
    mkdir -p "$(dirname "$FIXTURE_DIR/$rel")"
    cp "$staged" "$FIXTURE_DIR/$rel"
    copied=$((copied + 1))
done
stale=0
while IFS= read -r -d '' committed; do
    rel="${committed#"$FIXTURE_DIR"/}"
    if [ ! -f "$STAGING_DIR/$rel" ]; then
        echo "  STALE    $rel (not in this capture; left in place)"
        stale=$((stale + 1))
    fi
done < <(find "$FIXTURE_DIR" -name '*.json' ! -name 'provenance.json' -type f -print0 | sort -z)

if [ "$mode" = "rebase" ]; then
    tmp="$(mktemp "${INVENTORY}.XXXXXX")"
    jq --arg now "$(date -u +%Y-%m-%dT%H:%M:%SZ)" '
        (.history // []) as $history
        | (del(.history, .pending_rebase, .provenance_policy, .schema_version)) as $old
        | .pending_rebase as $p
        | {
            schema_version: .schema_version,
            milestone_id: $p.milestone_id,
            captured_at_utc: $now,
            source_repo: ($p.source_repo + {working_tree_clean: true}),
            rust_toolchain: $p.rust_toolchain,
            cargo_lock: $p.cargo_lock,
            rebase_record: $p.rebase_record,
            history: ($history + [$old]),
            provenance_policy: .provenance_policy
          }' "$INVENTORY" >"$tmp"
    mv "$tmp" "$INVENTORY"
fi

echo "[asx] fixtures-promote: PROMOTED mode=$mode baseline=$staged_baseline copied=$copied stale=$stale"
