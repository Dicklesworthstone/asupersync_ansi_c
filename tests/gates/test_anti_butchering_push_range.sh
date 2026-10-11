#!/usr/bin/env bash
# test_anti_butchering_push_range.sh — negative controls for the
# semantic-sensitive proof-block gate on CI push events
# (tools/ci/check_anti_butchering.sh; bridge bead W0.2, bd-9kll.1.3).
#
# On push events the gate used to read only .pull_request SHAs, find none,
# and judge an empty changed-file list: every push to main was a "skip",
# whatever it changed. These controls replay push payloads over commits that
# are permanently in main's history:
#   - d722cfe: touches src/runtime and include/asx, no proof block  -> FAILS
#   - 729c102: touches include/asx, carries a proof block           -> PASSES
#   - 18c643f..729c102: a range in which one commit has the block   -> PASSES
#   - 18c643f: .beads only                                          -> skip, PASSES
#   - branch creation (all-zero before) at 729c102                  -> PASSES
#   - unknown `before` commit                                       -> FAILS
#   - payload without before/after                                  -> FAILS
#
# Needs full history (CI's check job uses fetch-depth: 0). Each run writes
# under build/test-gates/<run_id>/ and logs JSONL to build/test-logs/gates.jsonl.
# Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GATE="$REPO_ROOT/tools/ci/check_anti_butchering.sh"
RUN_ID="anti-butchering-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
mkdir -p "$WORK" "$LOG_DIR"

failures=0

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"anti_butchering_push_range","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

full_sha() {
    git -C "$REPO_ROOT" rev-parse --verify --quiet "$1^{commit}"
}

for c in 18c643f d722cfe 729c102; do
    if ! full_sha "$c" >/dev/null; then
        echo "test_anti_butchering_push_range: FAIL (commit $c not in history; shallow clone?)"
        exit 1
    fi
done
C18="$(full_sha 18c643f)"
C5F="$(full_sha 18c643f~1)"
CD7="$(full_sha d722cfe)"
C72="$(full_sha 729c102)"
ZERO="0000000000000000000000000000000000000000"
UNKNOWN="0123456789abcdef0123456789abcdef01234567"

# run_case <name> <expected exit: 0|nonzero> <expected output regex> <event json>
run_case() {
    local name="$1" expect="$2" pattern="$3" event="$4"
    local event_file="$WORK/$name.event.json"
    printf '%s\n' "$event" >"$event_file"

    local out rc observed status
    out="$(env -u ASX_GUARANTEE_IMPACT_TEXT -u ASX_ANTI_BUTCHER_BASE_REF -u ASX_ANTI_BUTCHER_HEAD_REF \
        GITHUB_EVENT_PATH="$event_file" bash "$GATE" --run-id "$RUN_ID-$name" 2>&1)"
    rc=$?
    printf '%s\n' "$out" >"$WORK/$name.log"

    if [ "$rc" -eq 0 ]; then observed="exit0"; else observed="exit$rc"; fi
    status="pass"
    if [ "$expect" = "0" ] && [ "$rc" -ne 0 ]; then status="fail"; fi
    if [ "$expect" = "nonzero" ] && [ "$rc" -eq 0 ]; then status="fail"; fi
    if ! grep -Eq -- "$pattern" <<<"$out"; then status="fail"; fi

    if [ "$status" = "pass" ]; then
        echo "  PASS: $name ($observed)"
    else
        echo "  FAIL: $name ($observed; expected $expect matching /$pattern/; log $WORK/$name.log)"
        failures=$((failures + 1))
    fi
    log_case "$name" "$expect:$pattern" "$observed" "$status" "$WORK/$name.log"
}

echo "=== test_anti_butchering_push_range ($RUN_ID) ==="

run_case sensitive_push_without_block_fails nonzero 'missing fields' \
    "{\"before\":\"$C18\",\"after\":\"$CD7\"}"
run_case sensitive_push_with_block_passes 0 'anti-butchering: PASS' \
    "{\"before\":\"$CD7\",\"after\":\"$C72\"}"
run_case multi_commit_push_judged_by_range 0 'anti-butchering: PASS' \
    "{\"before\":\"$C18\",\"after\":\"$C72\"}"
run_case non_sensitive_push_skips 0 'status=skip' \
    "{\"before\":\"$C5F\",\"after\":\"$C18\"}"
run_case branch_creation_judges_tip 0 'anti-butchering: PASS' \
    "{\"before\":\"$ZERO\",\"after\":\"$C72\"}"
run_case unknown_before_fails nonzero 'cannot resolve the diff range' \
    "{\"before\":\"$UNKNOWN\",\"after\":\"$C72\"}"
run_case payload_without_range_fails nonzero 'cannot resolve the diff range' \
    '{"ref":"refs/heads/main"}'

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_anti_butchering_push_range: PASS (7 cases)"
    exit 0
fi
echo "test_anti_butchering_push_range: FAIL ($failures case(s))"
exit 1
