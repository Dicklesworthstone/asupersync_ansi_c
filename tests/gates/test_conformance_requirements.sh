#!/usr/bin/env bash
# test_conformance_requirements.sh — negative controls for the runner's
# scenario "requires" skip (DEC-008, bd-9kll.5.7).
#
# A scenario that declares an unbounded mpsc channel lists
# "requires": ["unbounded_queue"]. A profile whose overload catalog forbids
# unbounded queues (FREESTANDING, EMBEDDED_ROUTER, HFT, BROWSER) refuses the
# channel, so the runner skips the scenario there and counts it
# (skipped_forbidden=N). A skip that is too wide would hide a real divergence
# in the profiles that forbid it, so each case here is a way the skip must
# NOT apply, plus the one way it must:
#
#   declared_allowed      "requires" a profile that allows it    -> runs, PASS
#   declared_allowed      under HFT                              -> SKIP, counted
#   declared_unbounded    under HFT (an unbounded channel too)   -> SKIP, counted
#   base fixture          under HFT, no "requires"               -> runs, PASS
#   undeclared_unbounded  under HFT: unbounded, not declared     -> ERROR (fails;
#                                                       a sweep run reports FAIL)
#   unknown_requirement   "requires": ["warp_drive"], any profile-> ERROR (fails)
#
# compare and compare-sweep both skip; a skip is counted in the summary's
# skipped_forbidden=N and is never a PASS.
#
# The variants are one real Rust capture (mpsc-two-phase-send-recv-001) with
# only its embedded scenario edited; a fixture's semantic digest covers the
# trace, snapshot and observations, not the scenario, so each stays
# authentic. Two runners are used: the default build's (ASX_CONFORMANCE_RUNNER,
# or build/bin/asx-conformance), which must be a profile that allows
# unbounded queues, and one built with PROFILE=HFT, which forbids them.
#
# Builds under build/test-gates/requirements-hft/ (the normal build is
# untouched), writes under build/test-gates/<run_id>/ and logs JSONL to
# build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="requirements-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
JOBS="${JOBS:-4}"
BASE="$REPO_ROOT/fixtures/rust_reference_v2/mpsc-two-phase-send-recv-001.json"
ALLOW="${ASX_CONFORMANCE_RUNNER:-$REPO_ROOT/build/bin/asx-conformance}"
HFT_DIR="${ASX_REQUIREMENTS_HFT_DIR:-$REPO_ROOT/build/test-gates/requirements-hft}"
FORBID="$HFT_DIR/bin/asx-conformance"
mkdir -p "$WORK" "$LOG_DIR"

failures=0

log_case() {
    # case expected observed status detail
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"conformance_requirements","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# run_case <name> <runner> <expected exit: 0|nonzero> <regex...> -- <runner args...>
# A regex starting with '!' must NOT match the output.
run_case() {
    local name="$1" runner="$2" expect="$3"
    shift 3
    local patterns=()
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do
        patterns+=("$1")
        shift
    done
    shift
    local out rc observed status="pass" p
    out="$("$runner" "$@" 2>&1)"
    rc=$?
    printf '%s\n' "$out" >"$WORK/$name.log"
    if [ "$rc" -eq 0 ]; then observed="exit0"; else observed="exit$rc"; fi
    if [ "$expect" = "0" ] && [ "$rc" -ne 0 ]; then status="fail"; fi
    if [ "$expect" = "nonzero" ] && [ "$rc" -eq 0 ]; then status="fail"; fi
    for p in "${patterns[@]}"; do
        if [ "${p:0:1}" = "!" ]; then
            if grep -Eq -- "${p:1}" <<<"$out"; then status="fail"; fi
        elif ! grep -Eq -- "$p" <<<"$out"; then
            status="fail"
        fi
    done
    if [ "$status" = "pass" ]; then
        echo "  PASS: $name ($observed)"
    else
        echo "  FAIL: $name ($observed; expected $expect matching ${patterns[*]}; log $WORK/$name.log)"
        failures=$((failures + 1))
    fi
    log_case "$name" "$expect:${patterns[*]}" "$observed" "$status" "$WORK/$name.log"
}

echo "=== test_conformance_requirements ($RUN_ID) ==="

if [ ! -x "$ALLOW" ]; then
    echo "  FAIL: no conformance runner at $ALLOW (make build/bin/asx-conformance first)"
    exit 1
fi
if [ ! -f "$BASE" ]; then
    echo "  FAIL: base fixture $BASE is missing"
    exit 1
fi

# The forbidding runner: the same sources under the HFT profile.
mkdir -p "$HFT_DIR"
if ! make -C "$REPO_ROOT" -j"$JOBS" --no-print-directory PROFILE=HFT DETERMINISTIC=1 \
    BUILD_DIR="$HFT_DIR" "$FORBID" >"$HFT_DIR/build.log" 2>&1; then
    echo "  FAIL: the HFT conformance runner does not build (log $HFT_DIR/build.log)"
    log_case "build_hft_runner" "builds" "build-failed" "fail" "$HFT_DIR/build.log"
    exit 1
fi

if ! python3 -I - "$BASE" "$WORK" <<'PY'
import copy
import json
import sys

base_path, out = sys.argv[1], sys.argv[2]
with open(base_path) as f:
    base = json.load(f)


def unbounded(scenario):
    for channel in scenario["channels"]:
        if channel["type"] == "mpsc":
            del channel["capacity"]
            channel["unbounded"] = True


def write(name, edit):
    fixture = copy.deepcopy(base)
    edit(fixture["scenario"])
    with open("%s/%s.json" % (out, name), "w") as f:
        json.dump(fixture, f, separators=(",", ":"))


def declared_unbounded(scenario):
    unbounded(scenario)
    scenario["requires"] = ["unbounded_queue"]


write("declared_allowed", lambda s: s.update(requires=["unbounded_queue"]))
write("declared_unbounded", declared_unbounded)
write("undeclared_unbounded", unbounded)
write("unknown_requirement", lambda s: s.update(requires=["warp_drive"]))
PY
then
    echo "  FAIL: could not derive the control fixtures from $BASE"
    exit 1
fi

ID='mpsc-two-phase-send-recv-001'

run_case declared_requirement_a_profile_allows_runs "$ALLOW" 0 \
    "^PASS $ID " 'skipped_forbidden=0' '!^SKIP ' -- \
    compare "$WORK/declared_allowed.json"
run_case declared_requirement_a_profile_forbids_is_skipped "$FORBID" 0 \
    "^SKIP $ID: .*unbounded_queue" 'compared=1 pass=0 fail=0 error=0' 'skipped_forbidden=1' \
    '!^PASS ' -- compare "$WORK/declared_allowed.json"
run_case declared_unbounded_channel_is_skipped_where_forbidden "$FORBID" 0 \
    "^SKIP $ID: .*unbounded_queue" 'compared=1 pass=0 fail=0 error=0' 'skipped_forbidden=1' -- \
    compare "$WORK/declared_unbounded.json"
run_case a_forbidding_profile_still_runs_scenarios_with_no_requirement "$FORBID" 0 \
    "^PASS $ID " 'skipped_forbidden=0' '!^SKIP ' -- \
    compare "$BASE"
run_case an_undeclared_refusal_still_fails "$FORBID" nonzero \
    "^ERROR $ID: C run failed: the profile forbids this channel" \
    'compared=1 pass=0 fail=0 error=1' 'skipped_forbidden=0' '!^SKIP ' -- \
    compare "$WORK/undeclared_unbounded.json"
run_case an_unknown_requirement_is_an_error_where_allowed "$ALLOW" nonzero \
    "^ERROR $ID: .*requires" 'error=1' 'skipped_forbidden=0' '!^SKIP ' -- \
    compare "$WORK/unknown_requirement.json"
run_case an_unknown_requirement_is_an_error_where_forbidden "$FORBID" nonzero \
    "^ERROR $ID: .*requires" 'error=1' 'skipped_forbidden=0' '!^SKIP ' -- \
    compare "$WORK/unknown_requirement.json"
run_case the_sweep_skips_a_declared_requirement_where_forbidden "$FORBID" 0 \
    "^SKIP $ID: .*unbounded_queue" 'skipped_forbidden=1' 'pass=0 known=0 fail=0 stale=0 error=0' -- \
    compare-sweep "$WORK/declared_allowed.json"
run_case the_sweep_fails_an_undeclared_refusal "$FORBID" nonzero \
    "^FAIL $ID: sweep run failed: the profile forbids this channel" 'fail=1' \
    'skipped_forbidden=0' '!^SKIP ' -- compare-sweep "$WORK/undeclared_unbounded.json"

if [ "$failures" -eq 0 ]; then
    echo "test_conformance_requirements: all cases passed"
    exit 0
fi
echo "test_conformance_requirements: $failures case(s) failed"
exit 1
