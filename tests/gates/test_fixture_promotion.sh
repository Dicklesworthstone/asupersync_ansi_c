#!/usr/bin/env bash
# test_fixture_promotion.sh — negative controls for non-destructive capture and
# baseline-aware promotion (bead W1.1, bd-9kll.2.1).
#
# capture_rust_fixtures.sh used to rewrite fixtures/rust_reference in place,
# which would have destroyed the corpus on a rebase. These controls run the
# real capture and promote scripts against a scratch corpus, inventory and
# rebase record, with a stub capture binary that stamps a chosen baseline:
#   - capture writes staging only (committed corpus byte-identical)  -> PASSES
#   - capture refuses to reuse a staging dir                          -> FAILS
#   - promote a refresh (staged == active baseline)                   -> PASSES
#   - promote a rebase without owner sign-off                         -> FAILS
#   - promote a rebase with sign-off; inventory rotates               -> PASSES
#   - promote a baseline that is neither active nor pending           -> FAILS
#   - promote a staging dir that mixes two baselines                  -> FAILS
#   - fixture-integrity on a pending-baseline fixture never promoted  -> FAILS
#
# Everything lives under build/test-gates/<run_id>/; the real corpus,
# inventory and rebase record are never touched. Logs JSONL to
# build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CAPTURE="$REPO_ROOT/tools/ci/capture_rust_fixtures.sh"
PROMOTE="$REPO_ROOT/tools/ci/promote_fixtures.sh"
RUN_ID="fixture-promotion-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
mkdir -p "$WORK/reference/core" "$WORK/staging" "$LOG_DIR"

ACTIVE="$(jq -r '.source_repo.commit' "$REPO_ROOT/docs/rust_baseline_inventory.json")"
PENDING="5e60b1c4c53d62aaddae68de3ee7de4732f1755b"
OTHER="0123456789abcdef0123456789abcdef01234567"

# Scratch corpus (one fixture on the active baseline), inventory, and record.
cp "$REPO_ROOT/fixtures/negative_controls/valid_control.json" "$WORK/reference/core/a.json"
cp "$REPO_ROOT/docs/rebase_records/2026-10-a9e737d8-to-5e60b1c4c.md" "$WORK/rebase.md"
jq --arg rec "$WORK/rebase.md" '.pending_rebase.rebase_record = $rec' \
    "$REPO_ROOT/docs/rust_baseline_inventory.json" >"$WORK/inventory.json"

# Stub capture binary: rewrites each staged fixture's baseline to $STUB_BASELINE.
cat >"$WORK/stub_capture.sh" <<'EOF'
#!/usr/bin/env bash
dir="$2"
find "$dir" -name '*.json' -type f | while read -r f; do
    jq --arg b "$STUB_BASELINE" '.provenance.rust_baseline_commit = $b' "$f" >"$f.tmp" && mv "$f.tmp" "$f"
done
EOF
chmod +x "$WORK/stub_capture.sh"

failures=0
log_case() {
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"fixture_promotion","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# check <name> <expected exit: 0|nonzero> <regex> <rc> <output> [extra condition result 0/1]
check() {
    local name="$1" expect="$2" pattern="$3" rc="$4" out="$5" extra="${6:-0}"
    local observed status="pass"
    printf '%s\n' "$out" >"$WORK/$name.log"
    if [ "$rc" -eq 0 ]; then observed="exit0"; else observed="exit$rc"; fi
    if [ "$expect" = "0" ] && [ "$rc" -ne 0 ]; then status="fail"; fi
    if [ "$expect" = "nonzero" ] && [ "$rc" -eq 0 ]; then status="fail"; fi
    if ! printf '%s\n' "$out" | grep -Eq "$pattern"; then status="fail"; fi
    if [ "$extra" -ne 0 ]; then status="fail"; fi
    if [ "$status" = "pass" ]; then
        echo "  PASS: $name ($observed)"
    else
        echo "  FAIL: $name ($observed; expected $expect matching /$pattern/; log $WORK/$name.log)"
        failures=$((failures + 1))
    fi
    log_case "$name" "$expect:$pattern" "$observed" "$status" "$WORK/$name.log"
}

capture() { # capture <run-id> <baseline>
    STUB_BASELINE="$2" FIXTURE_DIR="$WORK/reference" STAGING_ROOT="$WORK/staging" \
        CAPTURE_BIN="$WORK/stub_capture.sh" bash "$CAPTURE" --run-id "$1" 2>&1
}
promote() { # promote <run-id>
    FIXTURE_DIR="$WORK/reference" STAGING_ROOT="$WORK/staging" INVENTORY="$WORK/inventory.json" \
        bash "$PROMOTE" --run-id "$1" 2>&1
}
tree_digest() { find "$WORK/reference" -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum; }

echo "=== test_fixture_promotion ($RUN_ID) ==="

before="$(tree_digest)"
out="$(capture refresh "$ACTIVE")"; rc=$?
after="$(tree_digest)"
[ "$before" = "$after" ] && [ -f "$WORK/staging/refresh/core/a.json" ]; staged_only=$?
check capture_writes_staging_only 0 'PASS: staged 1 fixtures' "$rc" "$out" "$staged_only"

out="$(capture refresh "$ACTIVE")"; rc=$?
check capture_refuses_existing_staging nonzero 'staging dir already exists' "$rc" "$out"

out="$(promote refresh)"; rc=$?
check promote_refresh_same_baseline 0 'PROMOTED mode=refresh' "$rc" "$out"

out="$(capture rebase "$PENDING")"; rc=$?
out="$(promote rebase)"; rc=$?
check promote_rebase_without_signoff_refused nonzero 'needs owner sign-off' "$rc" "$out"

sed -i 's/^- approved_by:.*/- approved_by: gate-test owner/' "$WORK/rebase.md"
out="$(promote rebase)"; rc=$?
rotated=1
if [ "$(jq -r '.source_repo.commit' "$WORK/inventory.json")" = "$PENDING" ] &&
    [ "$(jq -r '.history | length' "$WORK/inventory.json")" = "1" ] &&
    [ "$(jq -r '.history[0].source_repo.commit' "$WORK/inventory.json")" = "$ACTIVE" ] &&
    [ "$(jq -r 'has("pending_rebase")' "$WORK/inventory.json")" = "false" ]; then
    rotated=0
fi
check promote_rebase_with_signoff_rotates_inventory 0 'PROMOTED mode=rebase' "$rc" "$out" "$rotated"

out="$(capture other "$OTHER")"; rc=$?
out="$(promote other)"; rc=$?
check promote_unknown_baseline_refused nonzero 'neither the active' "$rc" "$out"

out="$(capture mixed "$PENDING")"; rc=$?
cp "$WORK/staging/other/core/a.json" "$WORK/staging/mixed/core/b.json"
out="$(promote mixed)"; rc=$?
check promote_mixed_baselines_refused nonzero 'name 2 baselines' "$rc" "$out"

# A fixture captured on the pending baseline but committed without promotion
# is a generation mix and fails fixture-integrity (real inventory, scratch root).
mkdir -p "$WORK/premature/core" "$WORK/premature-reports"
jq --arg b "$PENDING" '.provenance.rust_baseline_commit = $b' \
    "$REPO_ROOT/fixtures/negative_controls/valid_control.json" >"$WORK/premature/core/a.json"
out="$(REPORT_DIR="$WORK/premature-reports" bash "$REPO_ROOT/tools/ci/run_conformance.sh" \
    --mode fixture-integrity --fixtures-root "$WORK/premature" --run-id "$RUN_ID-premature" 2>&1)"
rc=$?
check pending_baseline_fixture_before_promotion_fails nonzero 'mixing generations' "$rc" "$out"

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_fixture_promotion: PASS (8 cases)"
    exit 0
fi
echo "test_fixture_promotion: FAIL ($failures case(s))"
exit 1
