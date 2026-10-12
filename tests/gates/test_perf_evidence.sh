#!/usr/bin/env bash
# bd-9kll.14.2: actual dashboard controls. Generated JSON/library bytes are
# explicitly SYNTHETIC GATE INPUTS, never benchmark or runtime performance proof.
# Optional --failed-archive replays unchanged JSON from an actual hosted artifact.
# All inputs, outputs and diagnostics are retained; no files are deleted.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FAILED_ARCHIVE=""
if [ "$#" -gt 0 ]; then
    [ "$#" -eq 2 ] && [ "$1" = "--failed-archive" ] || exit 2
    FAILED_ARCHIVE="$2"
    [ -f "$FAILED_ARCHIVE" ] || { echo 'failed artifact not found' >&2; exit 2; }
fi
for tool in python3 jq git mktemp; do command -v "$tool"; done
WORK="$(mktemp -d "${TMPDIR:-/tmp}/asx-perf-evidence-XXXXXXXX")"
SHA="$(git -C "$ROOT" rev-parse HEAD)"
printf '[perf-evidence] SYNTHETIC gate controls; no live performance claim; artifacts=%s\n' "$WORK"

for mode in valid core_fail hft_fail size_fail control_fail checkout_fail dependencies_fail missing skipped wrong_sha wrong_run stale wrong_hash wrong_core_library malformed zero_run; do
    CASE="$WORK/$mode"
    python3 -I - "$CASE" "$mode" "$SHA" <<'PY'
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
case, mode, sha = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
case.mkdir()
core, hft = case / 'core-build', case / 'hft-build'
for build in [core, hft]:
    (build / 'lib').mkdir(parents=True)
    (build / 'lib/libasx.a').write_text('SYNTHETIC gate input bytes; not a compiled library\n')
run = mode
now = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
paths = {'CORE': case / 'core.json', 'HFT': case / 'hft.json', 'size': case / 'size.json'}
def slo(profile):
    return {'synthetic_gate_input': True, 'schema': 'asx.slo_gate_report.v1',
            'profile': profile, 'run_id': run + ('-hft' if profile == 'HFT' else ''),
            'generated_at': now, 'status': 'pass',
            'summary': {'total': 2, 'passed': 2, 'failed': 0, 'skipped': 0,
                        'slo_total': 1, 'slo_failed': 0, 'validation_total': 1, 'validation_failed': 0},
            'baseline_validation': {'status': 'pass', 'checks': [
                {'check': 'baseline_not_stale', 'passed': True, 'captured_at': now, 'max_age_days': 120}]},
            'passes': [{'benchmark': 'synthetic-input', 'metric': 'p99_ns', 'actual': 1, 'budget': 2}],
            'violations': [], 'validation_failures': []}
reports = {'CORE': slo('CORE'), 'HFT': slo('HFT'),
           'size': {'synthetic_gate_input': True, 'schema': 'asx.size_gate_report.v1',
                    'profile': 'CORE', 'run_id': run, 'generated_at': now, 'status': 'pass',
                    'summary': {'total': 1, 'passed': 1, 'failed': 0, 'skipped': 0},
                    'measured': {'cold_start': {'samples': 1}},
                    'sources': {'lib': str(core / 'lib/libasx.a')},
                    'passes': [{'category': 'synthetic-input', 'metric': 'archive_bytes'}], 'violations': []}}
steps = ['checkout', 'dependencies', 'perf-evidence-controls',
         'release-build', 'bench', 'bench-json', 'validate-bench-artifacts', 'slo-gate-core',
         'hft-build', 'hft-bench-json', 'slo-gate-hft', 'size-gate']
outcomes = {name: 'success' for name in steps}
if mode in ['core_fail', 'hft_fail', 'size_fail']:
    label = {'core_fail': 'CORE', 'hft_fail': 'HFT', 'size_fail': 'size'}[mode]
    report = reports[label]
    report['status'] = 'fail'
    report['summary']['failed'] = 1
    report['summary']['passed'] -= 1
    report['violations'] = report['passes']
    report['passes'] = []
    if label != 'size':
        report['summary']['slo_failed'] = 1
    outcomes[{'CORE': 'slo-gate-core', 'HFT': 'slo-gate-hft', 'size': 'size-gate'}[label]] = 'failure'
if mode in ['control_fail', 'checkout_fail', 'dependencies_fail']:
    outcomes[{'control_fail': 'perf-evidence-controls', 'checkout_fail': 'checkout',
              'dependencies_fail': 'dependencies'}[mode]] = 'failure'
if mode == 'skipped':
    outcomes['slo-gate-hft'] = 'skipped'
if mode == 'stale':
    reports['CORE']['baseline_validation']['checks'][0]['captured_at'] = '2026-03-01T05:10:00Z'
if mode == 'zero_run':
    reports['CORE']['summary'].update(total=1, passed=1, slo_total=0)
    reports['CORE']['passes'] = []
if mode == 'wrong_core_library':
    reports['size']['sources']['lib'] = str(hft / 'lib/libasx.a')
for label, file in paths.items():
    if mode == 'missing' and label == 'CORE':
        continue
    file.write_text('{malformed' if mode == 'malformed' and label == 'CORE' else json.dumps(reports[label]))
def binding(file):
    return {'path': str(file), 'sha256': hashlib.sha256(file.read_bytes()).hexdigest() if file.exists() else ''}
manifest = {'synthetic_gate_input': True, 'sha': sha, 'run_tag': run,
            'status': 'fail' if 'failure' in outcomes.values() else 'incomplete' if 'skipped' in outcomes.values() else 'pass',
            'steps': [{'id': name, 'outcome': outcomes[name]} for name in steps],
            'build_dirs': {'CORE': str(core), 'HFT': str(hft)},
            'core_library': binding(core / 'lib/libasx.a'),
            'reports': {label: binding(file) for label, file in paths.items()}}
if mode == 'wrong_sha': manifest['sha'] = '0' * 40
if mode == 'wrong_run': manifest['run_tag'] = 'other-run'
if mode == 'wrong_hash': manifest['reports']['CORE']['sha256'] = '0' * 64
# A claimed passing manifest cannot hide an actual required-step failure.
if mode in ['control_fail', 'checkout_fail', 'dependencies_fail']: manifest['status'] = 'pass'
(case / 'manifest.json').write_text(json.dumps(manifest))
(case / 'bench.json').write_text(json.dumps({'synthetic_gate_input': True, 'profile': 'CORE',
                                           'benchmarks': {'scheduler_multi_round': {'count': 1, 'p99_ns': 1}}}))
PY
    actual=0
    bash "$ROOT/tools/ci/run_evidence_dashboard.sh" --run-id "$mode" --expected-sha "$SHA" \
        --core-slo-report "$CASE/core.json" --hft-slo-report "$CASE/hft.json" --size-report "$CASE/size.json" \
        --perf-manifest "$CASE/manifest.json" --bench-json "$CASE/bench.json" \
        --conformance-dir "$CASE/optional-conformance" --fuzz-dir "$CASE/optional-fuzz" \
        --adaptive-ledger-json "$CASE/optional-adaptive.json" \
        --history-file "$CASE/history.jsonl" --dashboard-out "$CASE/dashboard.json" \
        --ledger-out "$CASE/ledger.json" --trend-out "$CASE/trend.json" --strict \
        > "$CASE/diagnostics.log" 2>&1 || actual=$?
    cat "$CASE/diagnostics.log"
    python3 -I - "$CASE/dashboard.json" "$mode" "$actual" <<'PY'
import json
from pathlib import Path
import sys
report = json.loads(Path(sys.argv[1]).read_text())
mode, code = sys.argv[2], int(sys.argv[3])
expected = 'pass' if mode == 'valid' else 'incomplete' if mode in ['missing', 'skipped'] else 'fail'
assert report['perf_readiness'] == expected, (mode, report['perf_evidence'])
assert code == (0 if mode == 'valid' else 1), (mode, code)
assert report['release_readiness'] == ('fail' if expected == 'fail' else 'incomplete')
assert report['release_scope']['full_release_proof'] is False
if mode == 'core_fail':
    assert report['perf_evidence']['reports']['HFT']['status'] == 'pass'
    assert report['perf_evidence']['reports']['size']['status'] == 'pass'
if mode in ['control_fail', 'checkout_fail', 'dependencies_fail']:
    required = {'control_fail': 'perf-evidence-controls', 'checkout_fail': 'checkout',
                'dependencies_fail': 'dependencies'}[mode]
    assert any(issue['check'] == required and issue['status'] == 'fail'
               for issue in report['perf_evidence']['issues'])
print(f'[perf-evidence] PASS synthetic {mode}: perf={expected} exit={code}')
PY
done

# These are workflow wiring checks, not a simulated GitHub runner or live benchmark.
python3 -I - "$ROOT/.github/workflows/perf.yml" <<'PY'
from pathlib import Path
import re
import sys
text = Path(sys.argv[1]).read_text()
blocks = text.split('\n      - name: ')
by_id = {}
for block in blocks:
    match = re.search(r'^        id: (\S+)$', block, re.M)
    if match: by_id[match.group(1)] = block
for name in ['test_perf_evidence', 'build_release', 'run_bench_json', 'slo_gate', 'build_hft', 'run_bench_hft', 'slo_gate_hft', 'size_gate', 'evidence_dashboard']:
    assert 'if: always()' in by_id[name], f'{name} hides evidence after a previous failure'
assert 'slo_gate.outcome' not in by_id['build_hft'] + by_id['size_gate']
assert 'BUILD_DIR="${CORE_BUILD_DIR}"' in by_id['build_release']
assert 'BUILD_DIR="${HFT_BUILD_DIR}"' in by_id['build_hft']
assert '--lib "${CORE_BUILD_DIR}/lib/libasx.a"' in by_id['size_gate']
assert 'continue-on-error:' not in text
assert text.index('- name: Emit perf lane manifest') < text.index('- name: Run evidence dashboard')
assert 'else "incomplete" end)' in text
for name in ['checkout', 'dependencies', 'test_perf_evidence']:
    assert '${{ steps.' + name + '.outcome }}' in text
for name in ['checkout', 'dependencies', 'perf-evidence-controls']:
    assert '{id: "' + name + '", outcome:' in text
print('[perf-evidence] PASS workflow failure propagation and separate profile wiring')
PY

if [ -n "$FAILED_ARCHIVE" ]; then
    CASE="$WORK/hosted-failure"
    python3 -I - "$FAILED_ARCHIVE" "$CASE" <<'PY'
from pathlib import Path
from zipfile import ZipFile
import sys
case = Path(sys.argv[2])
case.mkdir()
with ZipFile(sys.argv[1]) as archive:
    for source, target in [('bench-results.json', 'bench.json'),
                           ('build/perf/slo_gate_report.json', 'core.json'),
                           ('build/ci-manifests/perf-tail-deadline.manifest.json', 'manifest.json')]:
        (case / target).write_bytes(archive.read(source))
PY
    HOSTED_RUN="$(jq -r '.run_tag' "$CASE/manifest.json")"
    HOSTED_SHA="$(jq -r '.sha' "$CASE/manifest.json")"
    actual=0
    bash "$ROOT/tools/ci/run_evidence_dashboard.sh" --run-id "$HOSTED_RUN" --expected-sha "$HOSTED_SHA" \
        --core-slo-report "$CASE/core.json" --hft-slo-report "$CASE/hft.json" --size-report "$CASE/size.json" \
        --perf-manifest "$CASE/manifest.json" --bench-json "$CASE/bench.json" \
        --conformance-dir "$CASE/optional-conformance" --fuzz-dir "$CASE/optional-fuzz" \
        --adaptive-ledger-json "$CASE/optional-adaptive.json" \
        --history-file "$CASE/history.jsonl" --dashboard-out "$CASE/dashboard.json" \
        --ledger-out "$CASE/ledger.json" --trend-out "$CASE/trend.json" --strict \
        > "$CASE/diagnostics.log" 2>&1 || actual=$?
    cat "$CASE/diagnostics.log"
    python3 -I - "$CASE/dashboard.json" "$actual" <<'PY'
import json
from pathlib import Path
import sys
report = json.loads(Path(sys.argv[1]).read_text())
assert int(sys.argv[2]) == 1 and report['perf_readiness'] == report['release_readiness'] == 'fail'
issues = report['perf_evidence']['issues']
assert any(issue['check'] == 'CORE_result' for issue in issues)
assert any(issue['check'] == 'slo-gate-hft' for issue in issues)
assert any(issue['check'] == 'size-gate' for issue in issues)
assert report['perf_evidence']['reports']['CORE']['status'] == 'fail'
print('[perf-evidence] PASS actual hosted failure replay; downloaded binary never executed')
PY
fi
printf '[perf-evidence] all controls passed; retained=%s\n' "$WORK"
