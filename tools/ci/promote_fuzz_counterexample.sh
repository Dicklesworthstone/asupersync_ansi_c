#!/usr/bin/env bash
# =============================================================================
# promote_fuzz_counterexample.sh — a minimized fuzz counterexample becomes a
# per-push fixture (bd-9kll.2.24)
#
# `make fuzz-minimize` leaves, for every divergence of a differential fuzz
# run, <id>-min.scenario.json (the 1-minimal scenario) under
# build/fuzz_v2/seed-<n>/minimized/. Nothing used to carry it from there into
# the corpus `make conformance` runs on every push, so a regression found at
# night stayed a CI artifact until someone copied files by hand. This does it:
#
#   tests/conformance/scenarios_v2/<name>.json   the scenario, renamed <name>
#   fixtures/rust_reference_v2/<name>.json       its genuine Rust capture
#
# The fixture is captured here, now, by `twin_run capture` on the renamed
# scenario (the capture minimize wrote embeds the scenario under its old id).
# It is never built by hand or edited: the C runner rejects a fixture whose
# provenance is not tools/twin_run or whose digest does not recompute.
#
# Steps, each fail-closed; nothing is written to the corpus until all pass:
#   1. the minimized scenario exists, is asx.scenario.v2, and <name> matches
#      the schema's id pattern (^[a-z0-9][a-z0-9-]*-[0-9]{3}$; default
#      <id>-min-001);
#   2. neither target exists (an existing scenario or fixture is never
#      overwritten);
#   3. the renamed scenario validates against schemas/scenario_dsl_v2.json;
#   4. twin_run captures it and the capture is an asx.fixture.v2 of that id
#      produced by tools/twin_run;
#   5. the C runner compares the capture. PASS is required, since the fixture
#      joins the blocking corpus; --allow-failing installs one the C runtime
#      still FAILs (a divergence not fixed yet), and make conformance is red
#      until it is.
# Then both files are installed. Staging stays under the work dir.
#
# Environment:
#   TWIN_RUN          command that runs twin_run (default: cargo run the
#                     pinned tools/twin_run, as the Makefile does)
#   CHECK_JSONSCHEMA  command that runs check-jsonschema (required)
#
# Usage:
#   promote_fuzz_counterexample.sh --minimized <dir> --id <scenario id>
#       --runner <asx-conformance> [--name <promoted id>] [--note <text>]
#       [--allow-failing] [--scenarios-dir <dir>] [--fixtures-dir <dir>]
#       [--work-dir <dir>]
#
# Exit codes: 0 promoted; 1 refused (a step above failed); 2 usage.
#
# SPDX-License-Identifier: MIT
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

MINIMIZED=""
ID=""
NAME=""
NOTE=""
RUNNER=""
ALLOW_FAILING=0
SCENARIOS_DIR="$REPO_ROOT/tests/conformance/scenarios_v2"
FIXTURES_DIR="$REPO_ROOT/fixtures/rust_reference_v2"
WORK_DIR="$REPO_ROOT/build/fuzz-promote"
SCHEMA="$REPO_ROOT/schemas/scenario_dsl_v2.json"
TWIN_RUN="${TWIN_RUN:-cargo run --release --locked --quiet --manifest-path $REPO_ROOT/tools/twin_run/Cargo.toml --}"

usage() {
    sed -n '/^# Usage:/,/^# Exit codes/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --minimized) [[ $# -ge 2 ]] || usage; MINIMIZED="$2"; shift 2 ;;
        --id) [[ $# -ge 2 ]] || usage; ID="$2"; shift 2 ;;
        --name) [[ $# -ge 2 ]] || usage; NAME="$2"; shift 2 ;;
        --note) [[ $# -ge 2 ]] || usage; NOTE="$2"; shift 2 ;;
        --runner) [[ $# -ge 2 ]] || usage; RUNNER="$2"; shift 2 ;;
        --allow-failing) ALLOW_FAILING=1; shift ;;
        --scenarios-dir) [[ $# -ge 2 ]] || usage; SCENARIOS_DIR="$2"; shift 2 ;;
        --fixtures-dir) [[ $# -ge 2 ]] || usage; FIXTURES_DIR="$2"; shift 2 ;;
        --work-dir) [[ $# -ge 2 ]] || usage; WORK_DIR="$2"; shift 2 ;;
        *) usage ;;
    esac
done
[[ -n "$MINIMIZED" && -n "$ID" && -n "$RUNNER" ]] || usage
[[ -n "${CHECK_JSONSCHEMA:-}" ]] || {
    echo "promote: FAIL (CHECK_JSONSCHEMA is not set: check-jsonschema 0.38.2 or uvx is required)" >&2
    exit 1
}
[[ -x "$RUNNER" ]] || { echo "promote: FAIL (runner $RUNNER is not executable; make conformance-runner)" >&2; exit 1; }
command -v jq >/dev/null 2>&1 || { echo "promote: FAIL (jq not found)" >&2; exit 1; }

NAME="${NAME:-${ID}-min-001}"
SRC="$MINIMIZED/${ID}-min.scenario.json"
SCENARIO_OUT="$SCENARIOS_DIR/$NAME.json"
FIXTURE_OUT="$FIXTURES_DIR/$NAME.json"
STAGE="$WORK_DIR/$NAME"

refuse() {
    echo "promote: REFUSED: $*" >&2
    exit 1
}

# 1. the input and the new id
[[ -f "$SRC" ]] || refuse "$SRC does not exist (run make fuzz-minimize for this seed first)"
jq -e '.schema == "asx.scenario.v2"' "$SRC" >/dev/null 2>&1 ||
    refuse "$SRC is not an asx.scenario.v2 scenario"
[[ "$NAME" =~ ^[a-z0-9][a-z0-9-]*-[0-9]{3}$ ]] ||
    refuse "name '$NAME' does not match ^[a-z0-9][a-z0-9-]*-[0-9]{3}\$ (the scenario schema's id pattern)"

# 2. never overwrite
[[ ! -e "$SCENARIO_OUT" ]] || refuse "$SCENARIO_OUT already exists"
[[ ! -e "$FIXTURE_OUT" ]] || refuse "$FIXTURE_OUT already exists"
# Staging is scratch this script owns: a retry after a refusal rewrites it.
mkdir -p "$STAGE/capture" || refuse "cannot create $STAGE"

# 3. the renamed scenario, schema-valid
PROMOTED="$STAGE/$NAME.json"
jq -c --arg id "$NAME" --arg note "$NOTE" \
    '.id = $id | if $note != "" then .description = ((.description // "") + " " + $note) else . end' \
    "$SRC" >"$PROMOTED" || refuse "cannot rename the scenario"
# shellcheck disable=SC2086 # CHECK_JSONSCHEMA is a command line
$CHECK_JSONSCHEMA --schemafile "$SCHEMA" "$PROMOTED" >"$STAGE/schema.log" 2>&1 ||
    refuse "the renamed scenario is not valid DSL v2 (see $STAGE/schema.log)"

# 4. the genuine Rust capture of the renamed scenario
# shellcheck disable=SC2086 # TWIN_RUN is a command line
$TWIN_RUN capture "$PROMOTED" --out "$STAGE/capture" >"$STAGE/capture.log" 2>&1 ||
    refuse "twin_run could not capture '$NAME' (see $STAGE/capture.log)"
CAPTURED="$STAGE/capture/$NAME.json"
[[ -f "$CAPTURED" ]] || refuse "twin_run wrote no $CAPTURED (see $STAGE/capture.log)"
jq -e --arg id "$NAME" \
    '.schema == "asx.fixture.v2" and .scenario_id == $id and .provenance.producer == "tools/twin_run"' \
    "$CAPTURED" >/dev/null 2>&1 ||
    refuse "$CAPTURED is not an asx.fixture.v2 capture of '$NAME' produced by tools/twin_run"

# 5. the C runtime against it
"$RUNNER" compare "$CAPTURED" >"$STAGE/compare.log" 2>&1
compare_rc=$?
if [[ $compare_rc -ne 0 ]]; then
    if [[ $ALLOW_FAILING -ne 1 ]]; then
        sed -n '1,20p' "$STAGE/compare.log" >&2
        refuse "the C runtime does not pass '$NAME' (exit $compare_rc; log $STAGE/compare.log). Fix the C side, or pass --allow-failing to land it red"
    fi
    echo "promote: WARNING: the C runtime FAILs '$NAME'; make conformance is red until it is fixed" >&2
fi

# Install. Both targets were absent at the start of this run.
mkdir -p "$SCENARIOS_DIR" "$FIXTURES_DIR" || refuse "cannot create the corpus directories"
cp -n "$PROMOTED" "$SCENARIO_OUT" || refuse "cannot write $SCENARIO_OUT"
cp -n "$CAPTURED" "$FIXTURE_OUT" || refuse "cannot write $FIXTURE_OUT"

echo "promote: PASS: $NAME"
echo "  scenario  $SCENARIO_OUT"
echo "  fixture   $FIXTURE_OUT"
echo "  C runtime $([[ $compare_rc -eq 0 ]] && echo PASS || echo "FAIL (allowed)")"
echo "next: the README rust_fixtures fact counts the corpus (sh tools/count_inventory.sh --facts);"
echo "      commit both files with their Guarantee Impact block and the divergence's bead."
exit 0
