#!/usr/bin/env bash
# bd-ij9w: live Rust/C comparison, runtime-mutant detection and reduction.
# Required executable files: ASX_TWIN_BIN, ASX_C_RUNNER, ASX_MUTANT_C_RUNNER.
# The mutant runner links ASX_MUTANT_BUDGET_PRIORITY_MIN runtime code.
# BUILD_DIR selects retained artifacts. Defaults: seed 9, count 200.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_ROOT="${BUILD_DIR:-$PROJECT_ROOT/build}"
case "$BUILD_ROOT" in /*) ;; *) BUILD_ROOT="$PROJECT_ROOT/$BUILD_ROOT" ;; esac
SEED="${ASX_DIFF_SEED:-9}"
COUNT="${ASX_DIFF_COUNT:-200}"

fail() { printf '[live-diff] FAIL: %s\n' "$*" >&2; exit 1; }
for name in ASX_TWIN_BIN ASX_C_RUNNER ASX_MUTANT_C_RUNNER; do
    value="${!name:-}"
    if [ -z "$value" ] || [ ! -f "$value" ] || [ ! -x "$value" ]; then
        fail "$name must name an executable file"
    fi
    case "$value" in /*) ;; *) printf -v "$name" '%s' "$PWD/$value" ;; esac
done
for tool in python3 tee mktemp; do
    command -v "$tool" || fail "required tool unavailable: $tool"
done
[[ "$SEED" =~ ^(0|[1-9][0-9]*)$ ]] || fail "invalid ASX_DIFF_SEED: $SEED"
[[ "$COUNT" =~ ^[1-9][0-9]*$ ]] || fail "invalid ASX_DIFF_COUNT: $COUNT"
python3 -I - "$SEED" "$COUNT" <<'PY'
import sys
assert 0 <= int(sys.argv[1]) < 2**64, 'seed must fit u64'
assert int(sys.argv[2]) >= 200, 'live control requires at least 200 scenarios'
PY

mkdir -p "$BUILD_ROOT/fuzz-live-control"
RUN_DIR="$(mktemp -d "$BUILD_ROOT/fuzz-live-control/seed-$SEED-XXXXXXXX")"
printf '[live-diff] retained artifacts: %s\n' "$RUN_DIR"
run_logged() {
    local log="$1"
    local -a pipeline_status
    shift
    if "$@" 2>&1 | tee "$log"; then
        pipeline_status=("${PIPESTATUS[@]}")
    else
        pipeline_status=("${PIPESTATUS[@]}")
    fi
    [ "${pipeline_status[1]}" -eq 0 ] || fail "could not retain complete log: $log"
    return "${pipeline_status[0]}"
}

run_logged "$RUN_DIR/generate.log" "$ASX_TWIN_BIN" generate \
    --seed "$SEED" --count "$COUNT" --out "$RUN_DIR/scenarios"
shopt -s nullglob
scenarios=("$RUN_DIR"/scenarios/*.json)
[ "${#scenarios[@]}" -eq "$COUNT" ] || fail "generator did not write $COUNT scenarios"
run_logged "$RUN_DIR/capture.log" "$ASX_TWIN_BIN" capture "${scenarios[@]}" --out "$RUN_DIR/fixtures"
fixtures=("$RUN_DIR"/fixtures/*.json)
[ "${#fixtures[@]}" -eq "$COUNT" ] || fail "Rust did not capture all $COUNT scenarios"

# Every fixture must be this run's pinned Rust capture of its exact input.
python3 -I - "$PROJECT_ROOT" "$RUN_DIR" "$SEED" "$COUNT" <<'PY'
import hashlib
import json
from pathlib import Path
import re
import sys
project, run = map(Path, sys.argv[1:3])
seed, count = map(int, sys.argv[3:5])
lock = (project / 'tools/twin_run/Cargo.lock').read_bytes()
pin = re.search(r'git\+https://github.com/Dicklesworthstone/asupersync[^\n]*#([0-9a-f]{40})', lock.decode()).group(1)
ids = {f'gen-{seed}-{index}' for index in range(count)}
capture_log = (run / 'capture.log').read_text()
passed = re.findall(r'^PASS (\S+) sha256:[0-9a-f]{64}$', capture_log, re.M)
assert len(passed) == count and set(passed) == ids, 'missing or duplicate Rust capture'
assert f'twin_run capture: {count} scenario(s), 0 failed' in capture_log
for scenario_id in ids:
    scenario = json.loads((run / 'scenarios' / f'{scenario_id}.json').read_text())
    fixture = json.loads((run / 'fixtures' / f'{scenario_id}.json').read_text())
    assert scenario['schema'] == 'asx.scenario.v2' and scenario['id'] == scenario_id
    assert fixture['schema'] == 'asx.fixture.v2' and fixture['scenario_id'] == scenario_id
    assert fixture['scenario'] == scenario, 'capture does not embed the generated input'
    provenance = fixture['provenance']
    assert provenance['producer'] == 'tools/twin_run' and provenance['rust_baseline_commit'] == pin
    assert provenance['cargo_lock_sha256'] == hashlib.sha256(lock).hexdigest()
    assert provenance['capture_run_id'] and provenance['rust_toolchain_commit_hash'] and provenance['rust_toolchain_host']
    assert fixture['vocabulary'] == 'asx.vocab.v2'
print(f'[live-diff] authenticated {count} live Rust captures at {pin}')
PY

check_compare() {
    python3 -I - "$1" "$2" "$3" "$4" <<'PY'
from pathlib import Path
import re
import sys
mode, count, status, path = sys.argv[1:]
count, status = int(count), int(status)
text = Path(path).read_text()
summaries = re.findall(r'^asx-conformance: compared=(\d+) pass=(\d+) fail=(\d+) error=(\d+)$', text, re.M)
assert len(summaries) == 1, 'missing or duplicate actual comparison summary'
compared, passed, failed, errors = map(int, summaries[0])
assert compared == count and passed + failed + errors == count and errors == 0
rows = re.findall(r'^(PASS|FAIL|ERROR) (\S+?)(?::| )', text, re.M)
assert len(rows) == count and len({row[1] for row in rows}) == count, 'missing or duplicate comparisons'
assert sum(row[0] == 'PASS' for row in rows) == passed
assert sum(row[0] == 'FAIL' for row in rows) == failed
if mode == 'clean':
    assert status == 0 and passed == count and failed == 0, 'clean runtime diverged'
else:
    assert status == 1 and failed > 0, 'mutant must produce semantic FAIL, not ERROR or zero-run success'
print(f'[live-diff] {mode}: compared={compared} pass={passed} fail={failed} error={errors} exit={status}')
PY
}

clean_status=0
run_logged "$RUN_DIR/clean.compare.log" "$ASX_C_RUNNER" compare "${fixtures[@]}" || clean_status=$?
check_compare clean "$COUNT" "$clean_status" "$RUN_DIR/clean.compare.log"
mutant_status=0
run_logged "$RUN_DIR/mutant.compare.log" "$ASX_MUTANT_C_RUNNER" compare "${fixtures[@]}" || mutant_status=$?
check_compare mutant "$COUNT" "$mutant_status" "$RUN_DIR/mutant.compare.log"

FAILED_ID="$(python3 -I - "$RUN_DIR/mutant.compare.log" <<'PY'
from pathlib import Path
import re
import sys
print(re.search(r'^FAIL (gen-[0-9]+-[0-9]+): ', Path(sys.argv[1]).read_text(), re.M).group(1))
PY
)"
original="$RUN_DIR/scenarios/$FAILED_ID.json"
run_logged "$RUN_DIR/minimize.log" "$ASX_TWIN_BIN" minimize "$original" \
    --runner "$ASX_MUTANT_C_RUNNER" --out "$RUN_DIR/minimized"
reduced="$RUN_DIR/minimized/$FAILED_ID-min.scenario.json"
reduced_fixture="$RUN_DIR/minimized/$FAILED_ID-min.fixture.json"
if [ ! -s "$reduced" ] || [ ! -s "$reduced_fixture" ]; then
    fail "minimizer wrote no retained witness"
fi
python3 -I - "$original" "$reduced" "$reduced_fixture" <<'PY'
import json
from pathlib import Path
import sys
original, reduced, fixture = [json.loads(Path(path).read_text()) for path in sys.argv[1:]]
def operations(value):
    if isinstance(value, dict):
        return int(isinstance(value.get('op'), str)) + sum(operations(v) for v in value.values())
    if isinstance(value, list):
        return sum(map(operations, value))
    return 0
before, after = operations(original), operations(reduced)
assert 0 < after < before, f'counterexample did not shrink: {before} -> {after} operations'
assert fixture['scenario'] == reduced and fixture['scenario_id'] == reduced['id']
print(f'[live-diff] retained smaller live counterexample: {before} -> {after} operations')
PY

# Re-run Rust in a separate process on the retained reduced input.
run_logged "$RUN_DIR/reduced.capture.log" "$ASX_TWIN_BIN" capture "$reduced" --out "$RUN_DIR/reduced-live"
live_fixture="$RUN_DIR/reduced-live/$FAILED_ID-min.json"
[ -s "$live_fixture" ] || fail "reduced input did not produce a fresh live Rust capture"
python3 -I - "$reduced_fixture" "$live_fixture" <<'PY'
import json
from pathlib import Path
import sys
minimal, live = [json.loads(Path(path).read_text()) for path in sys.argv[1:]]
assert live['provenance']['producer'] == 'tools/twin_run'
assert live['provenance']['rust_baseline_commit'] == minimal['provenance']['rust_baseline_commit']
assert live['provenance']['cargo_lock_sha256'] == minimal['provenance']['cargo_lock_sha256']
for field in ['scenario', 'scenario_id', 'trace', 'snapshot', 'observations', 'schedule', 'semantic_digest']:
    assert minimal[field] == live[field], f'fresh reduced Rust capture differs: {field}'
PY
reduced_clean_status=0
run_logged "$RUN_DIR/reduced.clean.compare.log" "$ASX_C_RUNNER" compare "$live_fixture" || reduced_clean_status=$?
check_compare clean 1 "$reduced_clean_status" "$RUN_DIR/reduced.clean.compare.log"
reduced_mutant_status=0
run_logged "$RUN_DIR/reduced.mutant.compare.log" "$ASX_MUTANT_C_RUNNER" compare "$live_fixture" || reduced_mutant_status=$?
check_compare mutant 1 "$reduced_mutant_status" "$RUN_DIR/reduced.mutant.compare.log"

# These explicit test doubles exercise only the minimizer's child-output parser.
# They do not generate fixtures and never count as live Rust/C parity evidence.
mkdir -p "$RUN_DIR/parser-controls"
cat > "$RUN_DIR/parser-controls/success-exit-fail.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
python3 -I - "$2" <<'PY'
import json
from pathlib import Path
import sys
fixture = json.loads(Path(sys.argv[1]).read_text())
print(f"FAIL {fixture['scenario_id']}: trace planted parser-only test double")
print('asx-conformance: compared=1 pass=0 fail=1 error=0')
PY
SH
cat > "$RUN_DIR/parser-controls/empty-output.sh" <<'SH'
#!/usr/bin/env bash
exit 0
SH
cat > "$RUN_DIR/parser-controls/crashing.sh" <<'SH'
#!/usr/bin/env bash
kill -TERM "$$"
SH
for control in success-exit-fail empty-output crashing; do
    double="$RUN_DIR/parser-controls/$control.sh"
    chmod +x "$double"
    parser_status=0
    run_logged "$RUN_DIR/parser-controls/$control.log" "$ASX_TWIN_BIN" minimize "$original" \
        --runner "$double" --out "$RUN_DIR/parser-controls/$control-output" || parser_status=$?
    python3 -I - "$control" "$parser_status" "$RUN_DIR/parser-controls/$control.log" <<'PY'
from pathlib import Path
import re
import sys
control, status, path = sys.argv[1:]
assert int(status) != 0, f'parser-only {control} runner was accepted as live divergence'
assert re.search(r'^twin_run minimize: .+', Path(path).read_text(), re.M), 'minimizer error diagnostic missing'
print(f'[live-diff] parser-only {control}: rejected exit={status}; diagnostic retained')
PY
done
printf '[live-diff] PASS: %s clean live comparisons; runtime mutant detected and smaller witness rechecked; artifacts=%s\n' "$COUNT" "$RUN_DIR"
