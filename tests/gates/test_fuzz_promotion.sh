#!/usr/bin/env bash
# test_fuzz_promotion.sh — negative controls for promoting a minimized fuzz
# counterexample into the per-push corpus (bd-9kll.2.24).
#
# tools/ci/promote_fuzz_counterexample.sh writes a scenario into
# tests/conformance/scenarios_v2 and its twin_run capture into
# fixtures/rust_reference_v2. These controls run the real script against a
# scratch corpus with stub twin_run, check-jsonschema and C runner binaries,
# and require:
#   - a good promotion installs both files, the scenario renamed and noted,
#     the fixture captured from the renamed scenario              -> PASSES
#   - an existing scenario or fixture is never overwritten          -> FAILS
#   - a missing or non-DSL minimized scenario                       -> FAILS
#   - a name that is not <slug>-NNN                                 -> FAILS
#   - a schema-invalid scenario, a failed capture, a capture whose
#     provenance is not tools/twin_run or whose id is not the name  -> FAILS
#   - a C runtime that FAILs the capture                            -> FAILS,
#     and installs nothing, unless --allow-failing                  -> PASSES
#   - no check-jsonschema configured                                -> FAILS
# A refusal leaves the scratch corpus byte-identical.
#
# Everything lives under build/test-gates/<run_id>/; the real corpus is never
# touched. Logs JSONL to build/test-logs/gates.jsonl. Nothing is deleted.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROMOTE="$REPO_ROOT/tools/ci/promote_fuzz_counterexample.sh"
RUN_ID="fuzz-promotion-$(date -u +%Y%m%dT%H%M%SZ)-$$"
WORK="$REPO_ROOT/build/test-gates/$RUN_ID"
LOG_DIR="$REPO_ROOT/build/test-logs"
LOG="$LOG_DIR/gates.jsonl"
MIN="$WORK/minimized"
SCEN="$WORK/scenarios"
FIX="$WORK/fixtures"
mkdir -p "$MIN" "$SCEN" "$FIX" "$LOG_DIR"

command -v jq >/dev/null 2>&1 || { echo "test_fuzz_promotion: jq not found"; exit 1; }

# One minimized scenario, as twin_run minimize writes it: canonical JSON under
# the id <id>-min, which the schema's -NNN pattern rejects.
printf '%s\n' '{"description":"generated","id":"gen-9-017-min","schema":"asx.scenario.v2","seed":9,"unit":"scheduler"}' \
    >"$MIN/gen-9-017-min.scenario.json"
printf '%s\n' '{"id":"x","schema":"asx.scenario.v1"}' >"$MIN/notdsl-min.scenario.json"

# Stub twin_run: `capture <scenario> --out <dir>`; STUB_MODE picks the fault.
cat >"$WORK/twin_run.sh" <<'EOF'
#!/usr/bin/env bash
[ "$1" = "capture" ] || exit 2
scenario="$2"; out="$4"
id="$(jq -r '.id' "$scenario")"
case "${STUB_MODE:-ok}" in
fail) echo "stub: capture failed" >&2; exit 1 ;;
none) exit 0 ;;
foreign) producer="someone-else" ;;
*) producer="tools/twin_run" ;;
esac
sid="$id"
[ "${STUB_MODE:-ok}" = "wrong_id" ] && sid="not-$id"
jq -c --arg id "$sid" --arg p "$producer" \
    '{schema:"asx.fixture.v2", scenario_id:$id, scenario:., provenance:{producer:$p}}' \
    "$scenario" >"$out/$id.json"
EOF
# Stub check-jsonschema and C runner.
cat >"$WORK/schema.sh" <<'EOF'
#!/usr/bin/env bash
[ "${STUB_SCHEMA:-ok}" = "ok" ]
EOF
cat >"$WORK/runner.sh" <<'EOF'
#!/usr/bin/env bash
[ "$1" = "compare" ] || exit 2
if [ "${STUB_COMPARE:-pass}" = "pass" ]; then echo "PASS stub"; exit 0; fi
echo "FAIL stub: trace"; exit 1
EOF
chmod +x "$WORK/twin_run.sh" "$WORK/schema.sh" "$WORK/runner.sh"

failures=0
log_case() {
    printf '{"ts":"%s","run_id":"%s","layer":"gates","suite":"fuzz_promotion","test":"%s","expected":"%s","observed":"%s","status":"%s","detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RUN_ID" "$1" "$2" "$3" "$4" "$5" >>"$LOG"
}

# corpus_state: a stable listing of the scratch corpus (names and sizes).
corpus_state() { (cd "$WORK" && find scenarios fixtures -type f -exec cksum {} + | sort); }

# promote <case> <expected: 0|nonzero> <regex> <env assignments...> -- <args...>
# Runs the script with the stubs; checks exit class, output pattern, and that
# a refusal changed nothing in the scratch corpus.
promote() {
    local name="$1" expect="$2" pattern="$3"
    shift 3
    local envs=()
    while [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    local before after out rc status="pass" observed
    before="$(corpus_state)"
    out="$(env TWIN_RUN="$WORK/twin_run.sh" CHECK_JSONSCHEMA="$WORK/schema.sh" "${envs[@]}" \
        bash "$PROMOTE" --minimized "$MIN" --runner "$WORK/runner.sh" --scenarios-dir "$SCEN" \
        --fixtures-dir "$FIX" --work-dir "$WORK/stage" "$@" 2>&1)"
    rc=$?
    after="$(corpus_state)"
    printf '%s\n' "$out" >"$WORK/$name.log"
    observed="exit$rc"
    if [ "$expect" = "0" ] && [ "$rc" -ne 0 ]; then status="fail"; fi
    if [ "$expect" = "nonzero" ]; then
        # 2 is a usage error: the control itself is mis-specified.
        if [ "$rc" -eq 0 ] || [ "$rc" -eq 2 ]; then status="fail"; fi
        if [ "$before" != "$after" ]; then status="fail"; observed="$observed corpus-changed"; fi
    fi
    if ! grep -Eq -- "$pattern" <<<"$out"; then status="fail"; fi
    if [ "$status" = "pass" ]; then
        echo "  PASS: $name ($observed)"
    else
        echo "  FAIL: $name ($observed; expected $expect matching /$pattern/; log $WORK/$name.log)"
        failures=$((failures + 1))
    fi
    log_case "$name" "$expect:$pattern" "$observed" "$status" "$WORK/$name.log"
}

echo "=== test_fuzz_promotion ($RUN_ID) ==="

# A good promotion.
promote promote_good 0 'promote: PASS: gen-9-017-min-001' -- --id gen-9-017 \
    --note "Minimized from fuzz seed 9."
good_ok=0
[ -f "$SCEN/gen-9-017-min-001.json" ] && [ -f "$FIX/gen-9-017-min-001.json" ] || good_ok=1
if [ "$good_ok" -eq 0 ]; then
    # The scenario is renamed and noted; the fixture was captured from it.
    jq -e '.id == "gen-9-017-min-001" and (.description | endswith("Minimized from fuzz seed 9."))' \
        "$SCEN/gen-9-017-min-001.json" >/dev/null || good_ok=1
    jq -e '.scenario_id == "gen-9-017-min-001" and .scenario.id == "gen-9-017-min-001"' \
        "$FIX/gen-9-017-min-001.json" >/dev/null || good_ok=1
fi
if [ "$good_ok" -eq 0 ]; then
    echo "  PASS: promote_good_files (scenario renamed and noted, fixture captured from it)"
    log_case "promote_good_files" "files" "ok" "pass" "$WORK"
else
    echo "  FAIL: promote_good_files (the installed scenario or fixture is not what a promotion makes)"
    failures=$((failures + 1))
    log_case "promote_good_files" "files" "wrong" "fail" "$WORK"
fi

# Never overwrite: the same promotion again, and an explicit existing name.
promote refuse_existing nonzero 'REFUSED: .* already exists' -- --id gen-9-017
promote refuse_missing_input nonzero 'REFUSED: .* does not exist' -- --id gen-0-000 --name other-001
promote refuse_not_dsl nonzero 'REFUSED: .* is not an asx.scenario.v2' -- --id notdsl
promote refuse_bad_name nonzero 'REFUSED: name .* does not match' -- --id gen-9-017 --name Bad_Name

# Each later step refuses before anything is installed.
promote refuse_schema_invalid nonzero 'REFUSED: the renamed scenario is not valid' \
    STUB_SCHEMA=bad -- --id gen-9-017 --name schema-case-001
promote refuse_capture_failed nonzero 'REFUSED: twin_run could not capture' \
    STUB_MODE=fail -- --id gen-9-017 --name capture-fail-001
promote refuse_capture_missing nonzero 'REFUSED: twin_run wrote no' \
    STUB_MODE=none -- --id gen-9-017 --name capture-none-001
promote refuse_foreign_provenance nonzero 'REFUSED: .* produced by tools/twin_run' \
    STUB_MODE=foreign -- --id gen-9-017 --name foreign-001
promote refuse_wrong_scenario_id nonzero 'REFUSED: .* produced by tools/twin_run' \
    STUB_MODE=wrong_id -- --id gen-9-017 --name wrong-id-001
promote refuse_c_fails nonzero 'REFUSED: the C runtime does not pass' \
    STUB_COMPARE=fail -- --id gen-9-017 --name c-fails-001

# An open divergence may be landed red on purpose.
promote allow_failing 0 'WARNING: the C runtime FAILs' STUB_COMPARE=fail -- \
    --id gen-9-017 --name c-fails-001 --allow-failing

# check-jsonschema is required, not optional.
out="$(env -u CHECK_JSONSCHEMA TWIN_RUN="$WORK/twin_run.sh" bash "$PROMOTE" --minimized "$MIN" \
    --id gen-9-017 --name no-schema-001 --runner "$WORK/runner.sh" --scenarios-dir "$SCEN" \
    --fixtures-dir "$FIX" --work-dir "$WORK/stage" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] && grep -q 'CHECK_JSONSCHEMA is not set' <<<"$out" && [ ! -e "$SCEN/no-schema-001.json" ]; then
    echo "  PASS: refuse_no_schema_validator (exit$rc)"
    log_case "refuse_no_schema_validator" "nonzero" "exit$rc" "pass" "$WORK"
else
    echo "  FAIL: refuse_no_schema_validator (exit$rc)"
    failures=$((failures + 1))
    log_case "refuse_no_schema_validator" "nonzero" "exit$rc" "fail" "$WORK"
fi

if [ "$failures" -eq 0 ]; then
    echo "test_fuzz_promotion: every control behaved"
    exit 0
fi
echo "test_fuzz_promotion: $failures control(s) failed"
exit 1
